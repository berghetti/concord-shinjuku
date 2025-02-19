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
#include <ix/transmit.h>
#include <stdio.h>

#include <ix/networker.h>
#include <net/ip.h>
#include <net/udp.h>

#include "benchmark.h"
#include "helpers.h"
// Added for leveldb
//#include "dl-helpers.h"
#include <dlfcn.h>
#include <ix/hijack.h>
#include <ix/leveldb.h>
#include <leveldb/c.h>

// ---- Added for tests ----
extern volatile bool TEST_STARTED;
extern volatile bool IS_FIRST_PACKET;
extern volatile bool INIT_FINISHED;
void print_stats (void);

volatile uint64_t TEST_START_TIME;
volatile uint64_t TEST_END_TIME;
volatile uint64_t TEST_RCVD_DB_GET;
volatile uint64_t TEST_RCVD_DB_ITERATOR;
volatile uint64_t TEST_RCVD_DB_PUT;
volatile uint64_t TEST_RCVD_DB_DELETE;
volatile uint64_t TEST_RCVD_DB_SEEK;
volatile uint64_t TEST_TOTAL_PACKETS_COUNTER = 0;
volatile bool TEST_FINISHED = false;
uint64_t dispatched_pkts = 0;

extern void dune_apic_send_posted_ipi (uint8_t vector, uint32_t dest_core);

#define PREEMPT_VECTOR 0xf2
#define PREEMPTION_DELAY 5000

uint16_t num_workers = 0;
volatile int *cpu_preempt_points[MAX_WORKERS] = { NULL };
uint64_t epoch_slack;
uint64_t time_slice = PREEMPTION_DELAY * CPU_FREQ_GHZ;
uint64_t dispatcher_work_thresh;

#define DISPATCHER_STATS_ITERATOR_LIMIT 1
struct dispatcher_timestamping
{
  uint64_t start;
  uint64_t end;
};
struct dispatcher_timestamping
    dispatcher_timestamps[DISPATCHER_STATS_ITERATOR_LIMIT]
    = { 0 };
uint64_t dispatcher_timestamp_iterator = 0;

extern __thread int concord_lock_counter;

__thread ucontext_t dispatcher_uctx_main;
__thread ucontext_t *dispatcher_cont;

extern int getcontext_fast (ucontext_t *ucp);
extern int swapcontext_fast (ucontext_t *ouctx, ucontext_t *uctx);
extern int swapcontext_fast_to_control (ucontext_t *ouctx, ucontext_t *uctx);
extern int swapcontext_very_fast (ucontext_t *ouctx, ucontext_t *uctx);

extern void concord_enable ();
extern void concord_disable ();

__thread uint64_t concord_preempt_after_cycle;
__thread uint64_t concord_start_time;
char *plugin_file = "../benchmarks/leveldb/lib/concord_apileveldb_rdtsc.so";

void
concord_rdtsc_func ()
{
  if (concord_lock_counter != 0 || unlikely (!INIT_FINISHED))
    return;

  swapcontext_fast_to_control (dispatcher_cont, &dispatcher_uctx_main);
}

struct dispatcher_request dispatcher_job;
__thread uint8_t dispatcher_job_status = IDLE;

// Added for leveldb support
extern leveldb_t *db;
// extern leveldb_iterator_t *iter;
extern leveldb_options_t *options;
extern leveldb_readoptions_t *roptions;
extern leveldb_writeoptions_t *woptions;

static void
preempt_check_init ()
{
  int i;
  for (i = 0; i < num_workers; i++)
    {
      preempt_check[i].check = false;
      preempt_check[i].timestamp = MAX_UINT64;
    }
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

static void
dispatcher_dl_init ()
{
  //    printf("Loading plugin: %s\n", plugin_file);
  //    dlerror();
  //    char *err = NULL;
  //    void *plugin = dlopen(plugin_file, RTLD_NOW);
  //    if ((err = dlerror())) {
  //        printf("Error loading plugin: %s\n",err);
  //        exit(-1);
  //    }
  //    assert(plugin);
  //
  //	*(void **) (&dl_simpleloop) = dlsym(plugin, STRINGIFY(simpleloop));
  //    if ((err = dlerror())) {
  //        printf("Error loading cncrd_leveldb_get symbol: %s\n",err);
  //        exit(-1);
  //    }
  //    assert(dl_simpleloop);
  //
  //	*(void **) (&dl_cncrd_leveldb_get) = dlsym(plugin,
  // STRINGIFY(cncrd_leveldb_get));
  //    if ((err = dlerror())) {
  //        printf("Error loading cncrd_leveldb_get symbol: %s\n",err);
  //        exit(-1);
  //    }
  //    assert(dl_cncrd_leveldb_get);
  //
  //    *(void **) (&dl_cncrd_leveldb_scan) = dlsym(plugin,
  //    STRINGIFY(cncrd_leveldb_scan)); if ((err = dlerror())) {
  //        printf("Error loading cncrd_leveldb_get symbol: %s\n",err);
  //        exit(-1);
  //    }
  //    assert(dl_cncrd_leveldb_scan);
}

static inline void
handle_finished (uint8_t i, uint8_t active_req)
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
handle_preempted (uint8_t i, uint8_t active_req)
{
  void *rnbl;
  struct request *req;
  uint8_t type, category;
  uint64_t timestamp;

  rnbl = worker_responses[i].responses[active_req].rnbl;
  req = worker_responses[i].responses[active_req].req;
  category = worker_responses[i].responses[active_req].category;
  type = worker_responses[i].responses[active_req].type;
  timestamp = worker_responses[i].responses[active_req].timestamp;
  tskq_enqueue_tail (&tskq, rnbl, req, type, category, timestamp);
  worker_responses[i].responses[active_req].flag = PROCESSED;
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
preempt_worker (uint8_t i, uint64_t cur_time)
{
  uint64_t time_remaining = cur_time - preempt_check[i].timestamp;
  if (likely (time_remaining < time_slice))
    {
      epoch_slack
          = epoch_slack < time_remaining ? epoch_slack : time_remaining;
    }
  else
    {
      if (preempt_check[i].check)
        {
          // Avoid preempting more times.
          preempt_check[i].check = false;
          dune_apic_send_posted_ipi (PREEMPT_VECTOR, CFG.cpu[i + 2]);
        }
    }
}

static inline void
concord_preempt_worker (uint8_t i, uint64_t cur_time)
{
  // if (cur_time < preempt_check[i].timestamp)
  //  {
  //    puts ("BUGGGG");
  //  }

  uint64_t time_remaining;
  uint64_t worker_ts;

  if (preempt_check[i].check)
    {
      worker_ts = preempt_check[i].timestamp;

      if (cur_time > worker_ts)
        {
          // Avoid preempting more times.
          *(cpu_preempt_points[i]) = 1;
          preempt_check[i].check = false;
        }
      else
        {
          time_remaining = worker_ts - cur_time;
          epoch_slack
              = epoch_slack < time_remaining ? epoch_slack : time_remaining;
        }
    }
}

static inline void
handle_worker (uint8_t i, uint64_t cur_time)
{
  concord_preempt_worker (i, cur_time);

  if (dispatcher_requests[i].requests[dispatch_states[i].next_pop].flag
      != READY)
    {
      if (worker_responses[i].responses[dispatch_states[i].next_pop].flag
          == FINISHED)
        {
          handle_finished (i, dispatch_states[i].next_pop);
          jbsq_get_next (&(dispatch_states[i].next_pop));
          dispatch_states[i].occupancy--;
          if (dispatch_states[i].occupancy == 0)
            {
              idle_list_head--;
              idle_list[idle_list_head] = i;
            }
        }
      else if (worker_responses[i].responses[dispatch_states[i].next_pop].flag
               == PREEMPTED)
        {
          handle_preempted (i, dispatch_states[i].next_pop);
          jbsq_get_next (&(dispatch_states[i].next_pop));
          dispatch_states[i].occupancy--;
          if (dispatch_states[i].occupancy == 0)
            {
              idle_list_head--;
              idle_list[idle_list_head] = i;
            }
        }
    }
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
          if (unlikely (networker_pointers.reqs[i] == NULL))
            {
              continue;
            }
          dispatched_pkts++;
          ret = context_alloc (&cont);
          if (unlikely (ret))
            {
              // log_warn("Cannot allocate context\n");
              request_enqueue (&frqueue, networker_pointers.reqs[i]);
              continue;
            }
          type = networker_pointers.types[i];
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

#define AFP_PAYLOAD_SIZE (sizeof (uint64_t) * 6)

static void
afp_server (void *buff)
{
  uint64_t *data = buff;
  // uint32_t type = data[3];
  uint32_t ns_sleep = data[5];

  fake_work_ns_rdtsc (ns_sleep);
}

static void
do_get (char *key)
{
  size_t len;
  char *value, *err = NULL;

  leveldb_readoptions_t *readoptions = leveldb_readoptions_create ();

  value
      = dl_cncrd_leveldb_get (db, readoptions, key, strlen (key), &len, &err);
  free (value);
  free (err);

  leveldb_readoptions_destroy (readoptions);
}

static void
do_scan (void)
{
  const char *retr_key;
  size_t len;

  leveldb_readoptions_t *readoptions = leveldb_readoptions_create ();

  dl_cncrd_leveldb_scan (db, readoptions, 'musa');

  leveldb_readoptions_destroy (readoptions);
}

static void
leveldb_server (void *buff)
{
#define GET 1
#define SCAN 2
  uint64_t *data = buff;
  uint32_t type = data[3];
  uint64_t key = data[4];

  switch (type)
    {
    case GET:
      do_get ((char *)&key);
      break;
    case SCAN:
      do_scan ();
      break;
    default:
      assert (0 && "Invalid request type");
    }
}

/**
 * generic_work - generic function acting as placeholder for application-level
 *                work
 * @msw: the top 32-bits of the pointer containing the data
 * @lsw: the bottom 32 bits of the pointer containing the data
 */
static void
dispatcher_generic_work (uint32_t msw, uint32_t lsw, uint32_t msw_id,
                         uint32_t lsw_id)
{
  asm volatile("sti" :::);

  struct ip_tuple *id = (struct ip_tuple *)((uint64_t)msw_id << 32 | lsw_id);
  void *data = (void *)((uint64_t)msw << 32 | lsw);
  int ret;

  afp_server (data);
  // leveldb_server(data);

  asm volatile("cli" :::);

  struct ip_tuple new_id = { .src_ip = id->dst_ip,
                             .dst_ip = id->src_ip,
                             .src_port = id->dst_port,
                             .dst_port = id->src_port };

  // ret = udp_send_one((void *)&resp, sizeof(struct message), &new_id);
  ret = udp_send_one ((void *)data, AFP_PAYLOAD_SIZE, &new_id);

  //     if (ret)
  //         log_warn("udp_send failed with error %d\n", ret);

  dispatcher_job_status = COMPLETED;
  swapcontext_very_fast (dispatcher_cont, &dispatcher_uctx_main);
}

static inline void
dispatcher_parse_packet (struct mbuf *pkt, void **data_ptr,
                         struct ip_tuple **id_ptr)
{
  // Quickly parse packet without doing checks
  struct eth_hdr *ethhdr = mbuf_mtod (pkt, struct eth_hdr *);
  struct ip_hdr *iphdr = mbuf_nextd (ethhdr, struct ip_hdr *);
  int hdrlen = iphdr->header_len * sizeof (uint32_t);
  struct udp_hdr *udphdr = mbuf_nextd_off (iphdr, struct udp_hdr *, hdrlen);
  // Get data and udp header
  (*data_ptr) = mbuf_nextd (udphdr, void *);
  uint16_t len = ntoh16 (udphdr->len);

  if (unlikely (!mbuf_enough_space (pkt, udphdr, len)))
    {
      // log_warn("worker: not enough space in mbuf\n");
      (*data_ptr) = NULL;
      return;
    }

  (*id_ptr) = mbuf_mtod (pkt, struct ip_tuple *);
  (*id_ptr)->src_ip = ntoh32 (iphdr->src_addr.addr);
  (*id_ptr)->dst_ip = ntoh32 (iphdr->dst_addr.addr);
  (*id_ptr)->src_port = ntoh16 (udphdr->src_port);
  (*id_ptr)->dst_port = ntoh16 (udphdr->dst_port);
  pkt->done = (void *)0xDEADBEEF;
}

static void
dispatcher_do_db_generic_work (struct db_req *db_pkg, uint64_t start_time)
{
  DB_REQ_TYPE type = db_pkg->type;
  uint64_t iter_cnt = 0;

  switch (db_pkg->type)
    {
    case (DB_PUT):
      {
        char *db_err = NULL;
        leveldb_put (db, woptions, db_pkg->key, KEYSIZE, db_pkg->val, VALSIZE,
                     &db_err);
        break;
      }

    case (DB_GET):
      {
#if RUN_UBENCH == 1
        dl_simpleloop (BENCHMARK_DB_GET_SPIN);
#else
        int read_len = VALSIZE;
        char *err;
        char *returned_value = dl_cncrd_leveldb_get (db, roptions, db_pkg->key,
                                                     KEYSIZE, &read_len, &err);
        if (err != NULL)
          {
            fprintf (stderr, "get fail. %s\n", db_pkg->key);
          }
#endif
        break;
      }
    case (DB_DELETE):
      {
        int k = 0;

        while (k < 50000)
          {
            asm volatile("nop");
            asm volatile("nop");
            asm volatile("nop");
            asm volatile("nop");
            k++;
          }

        break;
      }
    case (DB_ITERATOR):
      {
#if RUN_UBENCH == 1
        dl_simpleloop (BENCHMARK_DB_ITERATOR_SPIN);
#else
        dl_cncrd_leveldb_scan (db, roptions, 'musa');
#endif
        break;
      }

    case (DB_SEEK):
      {
        leveldb_iterator_t *iter = leveldb_create_iterator (db, roptions);
        leveldb_iter_seek (iter, "mykey", 5);

        break;
      }
    default:
      break;
    }

  TEST_TOTAL_PACKETS_COUNTER += 1;

  if (TEST_TOTAL_PACKETS_COUNTER == BENCHMARK_STOP_AT_PACKET)
    {
      TEST_END_TIME = get_us ();
      TEST_FINISHED = true;
    }

  switch (type)
    {
    case DB_GET:
      TEST_RCVD_DB_GET++;
      break;
    case DB_ITERATOR:
      TEST_RCVD_DB_ITERATOR++;
      break;
    case DB_PUT:
      TEST_RCVD_DB_PUT++;
      break;
    case DB_DELETE:
      TEST_RCVD_DB_DELETE++;
      break;
    case DB_SEEK:
      TEST_RCVD_DB_SEEK++;
      break;
    default:
      break;
    }

  dispatcher_job_status = COMPLETED;
  swapcontext_very_fast (dispatcher_cont, &dispatcher_uctx_main);
}

static inline void
dispatcher_handle_new_packet (void)
{
  int ret;
  void *data;
  struct ip_tuple *id;
  struct mbuf *pkt = (struct mbuf *)dispatcher_job.req;
  dispatcher_parse_packet (pkt, &data, &id);

  if (data)
    {
      uint32_t msw = ((uint64_t)data & 0xFFFFFFFF00000000) >> 32;
      uint32_t lsw = (uint64_t)data & 0x00000000FFFFFFFF;
      uint32_t msw_id = ((uint64_t)id & 0xFFFFFFFF00000000) >> 32;
      uint32_t lsw_id = (uint64_t)id & 0x00000000FFFFFFFF;

      dispatcher_cont = (struct mbuf *)dispatcher_job.rnbl;
      getcontext_fast (dispatcher_cont);
      set_context_link (dispatcher_cont, &dispatcher_uctx_main);
      makecontext (dispatcher_cont, (void (*) (void))dispatcher_generic_work,
                   4, msw, lsw, msw_id, lsw_id);
      ret = swapcontext_very_fast (&dispatcher_uctx_main, dispatcher_cont);
      if (ret)
        {
          log_err ("Failed to do swap into new context\n");
          exit (-1);
        }
    }
  else
    {
      log_debug ("OOPS No Data\n");
      dispatcher_job_status = COMPLETED;
    }
}

static inline void
dispatcher_handle_fake_new_packet (void)
{
  int ret;
  struct mbuf *pkt;
  struct db_req *req;

  pkt = (struct mbuf *)dispatcher_job.req;

  // req = mbuf_mtod(pkt, struct custom_payload *);
  req = mbuf_mtod (pkt, struct db_req *);

  if (req == NULL)
    {
      log_debug ("OOPS No Data\n");
      dispatcher_job_status = COMPLETED;
      return;
    }

  dispatcher_cont = (struct mbuf *)dispatcher_job.rnbl;
  getcontext_fast (dispatcher_cont);
  set_context_link (dispatcher_cont, &dispatcher_uctx_main);

  makecontext (dispatcher_cont, (void (*) (void))dispatcher_do_db_generic_work,
               2, req, dispatcher_job.timestamp);
  ret = swapcontext_very_fast (&dispatcher_uctx_main, dispatcher_cont);
  if (ret)
    {
      log_err ("Failed to do swap into new context\n");
      exit (-1);
    }
}

static inline void
dispatcher_handle_context (void)
{
  int ret;
  dispatcher_cont = dispatcher_job.rnbl;
  set_context_link (dispatcher_cont, &dispatcher_uctx_main);
  ret = swapcontext_very_fast (&dispatcher_uctx_main, dispatcher_cont);
  if (ret)
    {
      log_err ("Failed to swap to existing context\n");
      exit (-1);
    }
}

static inline void
dispatcher_finish_request (void)
{
  dispatcher_job.rnbl = dispatcher_cont;
  if (dispatcher_job_status == COMPLETED)
    {
      if (dispatcher_job.req == NULL)
        log_warn ("No mbuf was returned from worker\n");
      context_free (dispatcher_job.rnbl);
      request_enqueue (&frqueue, (struct request *)dispatcher_job.req);
    }
  else
    {
      dispatcher_job.category = CONTEXT;
    }
}

static inline void
dispatcher_handle_fake_request (uint64_t cur_time)
{
  if (dispatcher_job_status != ONGOING)
    {
      void *rnbl;
      struct request *req;
      uint8_t type, category;
      uint64_t timestamp;
      if (tskq_dequeue_category (&tskq, &rnbl, &req, &type, &category,
                                 &timestamp, PACKET))
        return;
      dispatcher_job.rnbl = rnbl;
      dispatcher_job.req = req;
      dispatcher_job.type = type;
      dispatcher_job.category = category;
      dispatcher_job.timestamp = timestamp;
      dispatcher_job_status = ONGOING;
    }

  concord_start_time = rdtsc ();
  concord_preempt_after_cycle = epoch_slack;

  if (dispatcher_job.category == PACKET)
    {
      dispatcher_handle_fake_new_packet ();
    }
  else
    {
      dispatcher_handle_context ();
    }
  fake_eth_process_send ();
  dispatcher_finish_request ();
}

static inline void
dispatcher_handle_request (void)
{

  if (dispatcher_job_status != ONGOING)
    {
      void *rnbl;
      struct request *req;
      uint8_t type, category;
      uint64_t timestamp;
      if (tskq_dequeue_category (&tskq, &rnbl, &req, &type, &category,
                                 &timestamp, PACKET))
        return;
      dispatcher_job.rnbl = rnbl;
      dispatcher_job.req = req;
      dispatcher_job.type = type;
      dispatcher_job.category = category;
      dispatcher_job.timestamp = timestamp;
      dispatcher_job_status = ONGOING;
    }

  concord_start_time = rdtsc ();
  concord_preempt_after_cycle = epoch_slack;

  if (dispatcher_job.category == PACKET)
    {
      dispatcher_handle_new_packet ();
    }
  else
    {
      dispatcher_handle_context ();
    }
}

void
dispatcher_do_work (uint64_t cur_time)
{

#ifdef FAKE_WORK
  dispatcher_handle_fake_request (cur_time);
#else

  eth_process_reclaim ();
  eth_process_send ();
  dispatcher_handle_request ();
#endif
}

/**
 * do_dispatching - implements dispatcher core's main loop
 */
void
do_dispatching (int num_cpus)
{
  uint8_t i;
  uint64_t cur_time;
  dispatcher_work_thresh = time_slice / 10;

  while (!INIT_FINISHED)
    ;

  num_workers = num_cpus - 2;
  preempt_check_init ();
  dispatch_states_init ();
  requests_init ();
  // dispatcher_dl_init ();
  bool flag = true;
  while (1)
    {
      // if (flag && TEST_STARTED && IS_FIRST_PACKET && (TEST_FINISHED ||
      // ((get_us() - TEST_START_TIME) > BENCHMARK_DURATION_US )))
      //{
      //	TEST_END_TIME = get_us();
      //	log_info("\n\n ----------- Benchmark FINISHED ----------- \n");
      //	log_info("Benchmark - Total number of packets %d \n",
      // TEST_TOTAL_PACKETS_COUNTER); 	log_info("Benchmark - %d DB_GET, %d
      // DB_ITERATOR, %d DB_PUT, %d DB_DELETE, %d DB_SEEK\n", TEST_RCVD_DB_GET,
      // TEST_RCVD_DB_ITERATOR, TEST_RCVD_DB_PUT, TEST_RCVD_DB_DELETE,
      // TEST_RCVD_DB_SEEK); 	log_info("Benchmark - Time elapsed (us):
      // %llu\n", TEST_END_TIME- TEST_START_TIME); 	uint64_t rate =
      // dispatched_pkts*1000/(TEST_END_TIME- TEST_START_TIME);
      //	log_info("Dispatched pkts, rate: %llu : %llu KRps\n",
      // dispatched_pkts,rate); 	print_stats(); 	for(int i = 0; i
      //<DISPATCHER_STATS_ITERATOR_LIMIT; i++){
      //		if(dispatcher_timestamps[i].start)
      //			log_info("Dispatching latency: %llu\n",
      // dispatcher_timestamps[i].end-dispatcher_timestamps[i].start);
      //	}
      //	log_info("Dispatcher exiting\n");
      //	flag = false;
      //	break;
      //}

      cur_time = rdtsc ();

      // Turn on to measure dispatching latencies
      // dispatcher_timestamps[dispatcher_timestamp_iterator].start = cur_time;
      epoch_slack = PREEMPTION_DELAY;
      for (i = 0; i < num_cpus - 2; i++)
        {
          handle_worker (i, cur_time);
        }
      handle_networker (cur_time);
      dispatch_requests (cur_time);
#if DISPATCHER_DO_WORK == 1
      if (epoch_slack > dispatcher_work_thresh)
        {
          epoch_slack -= dispatcher_work_thresh;
          dispatcher_do_work (cur_time);
        }
#endif

      // Turn on to measure dispatching latencies
      // dispatcher_timestamps[dispatcher_timestamp_iterator].end = rdtsc();
      // dispatcher_timestamp_iterator = (dispatcher_timestamp_iterator+1) &
      // (DISPATCHER_STATS_ITERATOR_LIMIT-1);
    }
}
