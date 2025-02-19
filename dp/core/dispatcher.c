/*
 * Copyright 2018-19 Board of Trustees of Stanford University
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/*
 * dispatcher.c - dispatcher core functionality
 *
 * A single core is responsible for receiving network packets from the network
 * core and dispatching these packets or contexts to the worker cores.
 */

#include <ix/cfg.h>
#include <ix/context.h>
#include <ix/dispatch.h>

#ifdef USE_CI

static int *workers_concord_flag[MAX_WORKERS];

void
register_worker (uint8_t w, int *flag_pointer)
{
  workers_concord_flag[w] = flag_pointer;
}

#endif

uint16_t num_workers = 0;

extern void dune_apic_send_posted_ipi (uint8_t vector, uint32_t dest_core);

#define PREEMPT_VECTOR 0xf2

static void
timestamp_init (int num_workers)
{
  int i;
  for (i = 0; i < num_workers; i++)
    timestamps[i] = MAX_UINT64;
}

static void
preempt_check_init (int num_workers)
{
  int i;
  for (i = 0; i < num_workers; i++)
    preempt_check[i] = false;
}

static void
dispatch_states_init ()
{
  int i;
  for (i = 0; i < num_workers; i++)
    {
      dispatch_states[i].next_push = 0;
      dispatch_states[i].next_pop = 0;
      dispatch_states[i].occupancy = 0;
      idle_list[i] = i;
    }
  idle_list_head = 0;
}

static void
requests_init ()
{
  int i;
  for (i = 0; i < num_workers; i++)
    {
      for (uint8_t j = 0; j < JBSQ_LEN; j++)
        dispatcher_requests[i].requests[j].flag = INACTIVE;
    }
}

static inline void
handle_finished (int i, uint8_t active_req)
{
  if (worker_responses[i].responses[active_req].req == NULL)
    log_warn ("No mbuf was returned from worker\n");

  context_free (worker_responses[i].responses[active_req].rnbl);
  request_enqueue (
      &frqueue,
      (struct request *)worker_responses[i].responses[active_req].req);
  worker_responses[i].responses[active_req].flag = PROCESSED;
}

static inline void
handle_preempted (int i, uint8_t active_req)
{
  void *rnbl;
  struct request *req;
  uint8_t type, category;
  uint64_t timestamp;

  struct worker_response *wr = &worker_responses[i].responses[active_req];
  rnbl = wr->rnbl;
  req = wr->req;
  category = wr->category;
  type = wr->type;
  timestamp = wr->timestamp;
  wr->flag = PROCESSED;

  // rnbl = worker_responses[i].responses[active_req].rnbl;
  // req = worker_responses[i].responses[active_req].req;
  // category = worker_responses[i].responses[active_req].category;
  // type = worker_responses[i].responses[active_req].type;
  // timestamp = worker_responses[i].responses[active_req].timestamp;
  // worker_responses[i].responses[active_req].flag = PROCESSED;
  tskq_enqueue_tail (&tskq, rnbl, req, type, category, timestamp);
}

static inline void
dispatch_requests (uint64_t cur_time)
{
  while (1)
    {
      void *rnbl;
      struct request *req;
      uint8_t type, category;
      uint64_t timestamp;

      int idle;

      if (likely (idle_list_head < num_workers))
        {
          idle = idle_list[idle_list_head];
          idle_list_head++;
          if (tskq_dequeue (&tskq, &rnbl, &req, &type, &category, &timestamp))
            {
              idle_list_head--;
              return;
            }
        }
      else
        {
          for (idle = 0; idle < num_workers; idle++)
            {
              if (dispatch_states[idle].occupancy == 1)
                break;
            }
          if (idle == num_workers)
            return;
          if (tskq_dequeue (&tskq, &rnbl, &req, &type, &category, &timestamp))
            return;
        }
      uint8_t active_req = dispatch_states[idle].next_push;
      dispatcher_requests[idle].requests[active_req].rnbl = rnbl;
      dispatcher_requests[idle].requests[active_req].req = req;
      dispatcher_requests[idle].requests[active_req].type = type;
      dispatcher_requests[idle].requests[active_req].category = category;
      dispatcher_requests[idle].requests[active_req].timestamp = timestamp;
      dispatcher_requests[idle].requests[active_req].flag = READY;
      jbsq_get_next (&(dispatch_states[idle].next_push));
      dispatch_states[idle].occupancy++;
    }
}

static inline void
dispatch_one_requests (uint8_t w, uint64_t cur_time)
{
  void *rnbl;
  struct request *req;
  uint8_t type, category;
  uint64_t timestamp;
  int idle = w;

  if (tskq_dequeue (&tskq, &rnbl, &req, &type, &category, &timestamp))
    return;

  uint8_t active_req = dispatch_states[idle].next_push;
  worker_responses[idle].responses[active_req].flag = RUNNING;
  dispatcher_requests[idle].requests[active_req].rnbl = rnbl;
  dispatcher_requests[idle].requests[active_req].req = req;
  dispatcher_requests[idle].requests[active_req].type = type;
  dispatcher_requests[idle].requests[active_req].category = category;
  dispatcher_requests[idle].requests[active_req].timestamp = timestamp;
  dispatcher_requests[idle].requests[active_req].flag = READY;
  // jbsq_get_next (&(dispatch_states[idle].next_push));
  // dispatch_states[idle].occupancy++;
  // timestamps[idle] = cur_time;
  // preempt_check[idle] = true;
}

static inline void
preempt_worker (int i, uint64_t cur_time)
{
  // if (preempt_check[i]
  //    && (((cur_time - timestamps[i]) / 2.5) > CFG.preemption_delay))
  if (preempt_check[i] && timestamps[i] < cur_time)
    {
      // Avoid preempting more times.
      preempt_check[i] = false;
#ifdef USE_CI
      *workers_concord_flag[i] = 1;
#else
      dune_apic_send_posted_ipi (PREEMPT_VECTOR, CFG.cpu[i + 2]);
#endif
    }
}

static inline void
handle_worker (uint8_t i, uint64_t cur_time)
{
  int np = dispatch_states[i].next_pop;
  int worker_flag = worker_responses[i].responses[np].flag;
  // if (dispatcher_requests[i].requests[dispatch_states[i].next_pop].flag
  //   != READY)
  if (worker_flag != RUNNING)
    {
      if (worker_flag == FINISHED)
        {
          handle_finished (i, dispatch_states[i].next_pop);
          // jbsq_get_next (&(dispatch_states[i].next_pop));
          // dispatch_states[i].occupancy--;
          // if (dispatch_states[i].occupancy == 0)
          //  {
          //    idle_list_head--;
          //    idle_list[idle_list_head] = i;
          //  }
        }
      else if (worker_flag == PREEMPTED)
        {
          handle_preempted (i, dispatch_states[i].next_pop);
          // jbsq_get_next (&(dispatch_states[i].next_pop));
          // dispatch_states[i].occupancy--;
          // if (dispatch_states[i].occupancy == 0)
          //  {
          //    idle_list_head--;
          //    idle_list[idle_list_head] = i;
          //  }
        }
      dispatch_one_requests (i, cur_time);
    }
  else
    preempt_worker (i, cur_time);
}

static inline void
handle_networker (uint64_t cur_time)
{
  int i, ret;
  uint8_t type;
  ucontext_t *cont;

  if (networker_pointers.cnt != 0)
    {
      for (i = 0; i < networker_pointers.cnt; i++)
        {
          ret = context_alloc (&cont);
          if (unlikely (ret))
            {
              // log_warn("Cannot allocate context\n");
              request_enqueue (&frqueue, networker_pointers.reqs[i]);
              continue;
            }
          type = networker_pointers.types[i];
          ++queue_length[type];
          tskq_enqueue_tail (&tskq, cont, networker_pointers.reqs[i], type,
                             PACKET, cur_time);
        }

      for (i = 0; i < ETH_RX_MAX_BATCH; i++)
        {
          struct request *req = request_dequeue (&frqueue);
          if (!req)
            break;
          networker_pointers.reqs[i] = req;
          networker_pointers.free_cnt++;
        }
      networker_pointers.cnt = 0;
    }
}

/**
 * do_dispatching - implements dispatcher core's main loop
 */
void
do_dispatching (int num_cpus)
{
  int i;
  uint64_t cur_time;

  num_workers = num_cpus - 2;
  preempt_check_init (num_cpus - 2);
  timestamp_init (num_cpus - 2);
  dispatch_states_init ();
  requests_init ();

  while (1)
    {
      cur_time = rdtsc ();
      for (i = 0; i < num_cpus - 2; i++)
        handle_worker (i, cur_time);
      handle_networker (cur_time);
      // dispatch_requests (cur_time);
    }
}
