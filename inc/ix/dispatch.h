/*
 * Copyright 2018-19 Board of Trustees of Stanford University
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <limits.h>
#include <stdint.h>
#include <ucontext.h>
#include <stdio.h>

#include <ix/cfg.h>
#include <ix/mempool.h>
#include <ix/ethqueue.h>

#include <net/ip.h>
#include <net/udp.h>

#define MAX_WORKERS   18

#define INACTIVE    0x00
#define READY       0x01
#define DONE        0x02 

#define RUNNING     0x00
#define FINISHED    0x01
#define PREEMPTED   0x02
#define PROCESSED   0x03

// Dispatcher job states
#define IDLE 0
#define ONGOING 1
#define COMPLETED 2

#define NOCONTENT   0x00
#define PACKET      0x01
#define CONTEXT     0x02

#define TYPE_REQ 1
#define TYPE_RES 0

#define MAX_UINT64  0xFFFFFFFFFFFFFFFF

extern int req_offset;

struct mempool_datastore task_datastore;
struct mempool task_mempool __attribute((aligned(64)));
struct mempool_datastore fini_request_cell_datastore;
struct mempool fini_request_cell_mempool __attribute((aligned(64)));
struct mempool_datastore request_datastore;
struct mempool request_mempool __attribute((aligned(64)));
struct mempool_datastore rq_datastore;
struct mempool rq_mempool __attribute((aligned(64)));

#define JBSQ_LEN    0x02

#if JBSQ_LEN == 0x02
static inline void jbsq_get_next(uint8_t* iter){
        *iter =  *iter^1; // This is for JBSQ_LEN = 2
}
#elif JBSQ_LEN == 0x01
static inline void jbsq_get_next(uint8_t* iter){}
#endif

struct message {
        uint16_t type;
        uint16_t seq_num;
	uint32_t queue_length[3];
        uint16_t client_id;
        uint32_t req_id;
        uint32_t pkts_length;
        uint64_t runNs;
        uint64_t genNs;
} __attribute__((__packed__));

struct request
{
	uint32_t pkts_length;
	uint16_t type;
	void * mbufs[8];
} __attribute__((packed, aligned(64)));

struct request_cell
{
	uint8_t pkts_remaining;
	uint16_t client_id;
	uint32_t req_id;
	struct request * req;
	struct request_cell * next;
	struct request_cell * prev;
} __attribute__((packed, aligned(64)));

struct request_queue {
        struct request_cell * head;
};

struct request_queue rqueue;

struct worker_response
{
        uint64_t flag;
        void * rnbl;
        struct request * req;
        uint64_t timestamp;
        uint8_t type;
        uint8_t category;
        char make_it_64_bytes[30];
} __attribute__((packed, aligned(64)));

struct jbsq_worker_response {
        struct worker_response responses[JBSQ_LEN];
}__attribute__((packed, aligned(64)));

struct dispatcher_request
{
        uint64_t flag;
        void * rnbl;
        struct request * req;
        uint8_t type;
        uint8_t category;
        uint64_t timestamp;
        char make_it_64_bytes[30];
} __attribute__((packed, aligned(64)));

struct jbsq_dispatcher_request {
        struct dispatcher_request requests[JBSQ_LEN];
}__attribute__((packed, aligned(64)));

struct jbsq_preemption {
        uint64_t timestamp;
        uint8_t check;
        char make_it_64_bytes[55]; 
};__attribute__((packed, aligned(64)));

struct worker_state {
        uint8_t next_push;
        uint8_t next_pop;
        uint8_t occupancy;
} __attribute__((packed));

struct networker_pointers_t
{
        uint8_t cnt;
        uint8_t free_cnt;
        uint8_t types[ETH_RX_MAX_BATCH];
        struct request * reqs[ETH_RX_MAX_BATCH];
        char make_it_64_bytes[64 - ETH_RX_MAX_BATCH*9 - 2];
} __attribute__((packed, aligned(64)));

struct fini_request_cell {
        struct request * req;
        struct fini_request_cell * next;
};

struct fini_request_queue {
        struct fini_request_cell * head;
};

struct fini_request_queue frqueue;

static inline struct request * request_dequeue(struct fini_request_queue * frq)
{
        struct fini_request_cell * tmp;
        struct request * req;

        if (!frq->head)
                return NULL;

        req = frq->head->req;
        tmp = frq->head;
        mempool_free(&fini_request_cell_mempool, tmp);
        frq->head = frq->head->next;

        return req;
}

static inline void request_enqueue(struct fini_request_queue * frq, struct request * req)
{
        if (unlikely(!req))
                return;
        struct fini_request_cell * frcell = mempool_alloc(&fini_request_cell_mempool);
        frcell->req = req;
        frcell->next = frq->head;
        frq->head = frcell;
}

struct task {
        void * runnable;
        struct request * req;
        uint8_t type;
        uint8_t category;
        uint64_t timestamp;
        struct task * next;
};

struct task_queue
{
        struct task * head;
        struct task * tail;
};
        
struct task_queue tskq;

static inline void tskq_enqueue_head(struct task_queue * tq, void * rnbl,
                                     struct request * req, uint8_t type,
                                     uint8_t category, uint64_t timestamp)
{
        struct task * tsk = mempool_alloc(&task_mempool);
        tsk->runnable = rnbl;
        tsk->req = req;
        tsk->type = type;
        tsk->category = category;
        tsk->timestamp = timestamp;
        if (tq->head != NULL) {
            struct task * tmp = tq->head;
            tq->head = tsk;
            tsk->next = tmp;
        } else {
            tq->head = tsk;
            tq->tail = tsk;
            tsk->next = NULL;
        }
}

static inline void tskq_enqueue_tail(struct task_queue * tq, void * rnbl,
                                     struct request * req, uint8_t type,
                                     uint8_t category, uint64_t timestamp)
{
        struct task * tsk = mempool_alloc(&task_mempool);
        if (!tsk)
                return;
        tsk->runnable = rnbl;
        tsk->req = req;
        tsk->type = type;
        tsk->category = category;
        tsk->timestamp = timestamp;
        if (tq->head != NULL) {
            tq->tail->next = tsk;
            tq->tail = tsk;
            tsk->next = NULL;
        } else {
            tq->head = tsk;
            tq->tail = tsk;
            tsk->next = NULL;
        }
}

static inline int tskq_dequeue(struct task_queue * tq, void ** rnbl_ptr,
                                struct request ** req, uint8_t *type, uint8_t *category,
                                uint64_t *timestamp)
{
        if (tq->head == NULL)
            return -1;
        (*rnbl_ptr) = tq->head->runnable;
        (*req) = tq->head->req;
        (*type) = tq->head->type;
        (*category) = tq->head->category;
        (*timestamp) = tq->head->timestamp;
        struct task * tsk = tq->head;
        tq->head = tq->head->next;
        mempool_free(&task_mempool, tsk);
        if (tq->head == NULL)
                tq->tail = NULL;
        return 0;
}

static inline int tskq_dequeue_category(struct task_queue * tq, void ** rnbl_ptr,
                                struct request ** req, uint8_t *type, uint8_t *category,
                                uint64_t *timestamp, uint8_t required_category){
        struct task* curr = tq->head;
        struct task* prev = NULL;
        int found = 0;
        while(curr != NULL){
                if(curr->category == required_category){
                        found = 1;
                        break;
                }
                prev = curr;
                curr = curr->next;
        }
        if(found){
                (*rnbl_ptr) = curr->runnable;
                (*req) = curr->req;
                (*type) = curr->type;
                (*category) = curr->category;
                (*timestamp) = curr->timestamp;
                struct task * tsk = prev;
                if(curr == tq->head)
                        tq->head = tq->head->next;
                else
                        prev->next = curr->next;

                mempool_free(&task_mempool, curr);
                if (tq->head == NULL)
                        tq->tail = NULL;
                return 0;
        }     
        return -1;


}
static inline uint64_t get_queue_timestamp(struct task_queue * tq, uint64_t * timestamp)
{
        if (tq->head == NULL)
            return -1;
        (*timestamp) = tq->head->timestamp;
        return 0;
}

static inline int naive_tskq_dequeue(struct task_queue * tq, void ** rnbl_ptr,
                                     struct request ** req, uint8_t *type,
                                     uint8_t *category, uint64_t *timestamp)
{
        int i;
        for (i = 0; i < CFG.num_ports; i++) {
                if(tskq_dequeue(&tq[i], rnbl_ptr, req, type, category,
                                timestamp) == 0)
                        return 0;
        }
        return -1;
}

static inline int smart_tskq_dequeue(struct task_queue * tq, void ** rnbl_ptr,
                                     struct request ** req, uint8_t *type,
                                     uint8_t *category, uint64_t *timestamp,
                                     uint64_t cur_time)
{
        int i, ret;
        uint64_t queue_stamp;
        int index = -1;
        double max = 0;

        for (i = 0; i < CFG.num_ports; i++) {
                ret = get_queue_timestamp(&tq[i], &queue_stamp);
                if (ret)
                        continue;

                int64_t diff = cur_time - queue_stamp;
                double current = diff / CFG.slos[i];
                if (current > max) {
                        max = current;
                        index = i;
                }
        }

        if (index != -1) {
                return tskq_dequeue(&tq[index], rnbl_ptr, req, type, category,
                                    timestamp);
        }
        return -1;
}

static inline struct request * rq_update(struct request_queue * rq, struct mbuf * pkt)
{
  // simplify to AFP test
	struct request * req = mempool_alloc(&request_mempool);
        if (unlikely(!req)) {
        	mbuf_free(pkt);
        	return NULL;
        }
	req->type = 0;
	req->pkts_length = 1;
	req->mbufs[0] = pkt;
	return req;
	// Quickly parse packet without doing checks
  //      struct eth_hdr * ethhdr = mbuf_mtod(pkt, struct eth_hdr *);
  //      struct ip_hdr *  iphdr = mbuf_nextd(ethhdr, struct ip_hdr *);
  //      int hdrlen = iphdr->header_len * sizeof(uint32_t);
	//struct udp_hdr * udphdr = mbuf_nextd_off(iphdr, struct udp_hdr *,
  //                                               hdrlen);
	//// Get data and udp header
	//void * data = mbuf_nextd(udphdr, void *);
	//struct message * msg = (struct message *) data;

	//uint16_t type = msg->type - req_offset;
  //      log_debug("RQ received packet type %d\n", type);
  //      uint16_t seq_num = msg->seq_num;
  //      uint16_t client_id = msg->client_id;
  //      uint32_t req_id = msg->req_id;
  //      uint32_t pkts_length = msg->pkts_length / sizeof(struct message);
	//if (pkts_length == 1) {
	//	struct request * req = mempool_alloc(&request_mempool);
  //      if (unlikely(!req)) {
  //          mbuf_free(pkt);
  //          return NULL;
  //      }
	//	req->type = type;
	//	req->pkts_length = 1;
	//	req->mbufs[0] = pkt;
	//	return req;
	//}

  //      if (!rq->head) {
  //              struct request_cell * rc = mempool_alloc(&rq_mempool);
  //              rc->pkts_remaining = pkts_length - 1;
  //              rc->client_id = client_id;
  //              rc->req_id = req_id;
  //              rc->req = mempool_alloc(&request_mempool);
  //              rc->req->mbufs[seq_num] = pkt;
  //              rc->req->pkts_length = pkts_length;
  //              rc->req->type = type;
  //              rc->next = NULL;
  //              rc->prev = NULL;
  //              rq->head = rc;
  //              return NULL;
  //      }
	//struct request_cell * cur = rq->head;
  //      while (cur != NULL) {
  //              if (cur->client_id == client_id && cur->req_id == req_id) {
  //                      cur->req->mbufs[seq_num] = pkt;
  //                      cur->pkts_remaining--;
	//		if (cur->pkts_remaining == 0) {
	//			struct request * req = cur->req;
	//			if (cur->prev == NULL) {
	//				rq->head = cur->next;
	//				if (rq->head != NULL)
	//					rq->head->prev = NULL;
	//			} else {
	//				cur->prev->next = cur->next;
	//				if (cur->next != NULL)
	//					cur->next->prev = cur->prev;
	//			}
	//			mempool_free(&rq_mempool, cur);
	//			return req;
	//		}
	//		return NULL;
	//	}
	//	cur = cur->next;
  //      }

  //      if (cur == NULL) {
  //              struct request_cell * rc = mempool_alloc(&rq_mempool);
  //              rc->pkts_remaining = pkts_length - 1;
  //              rc->client_id = client_id;
  //              rc->req_id = req_id;
  //              rc->req = mempool_alloc(&request_mempool);
  //              rc->req->mbufs[seq_num] = pkt;
  //              rc->req->pkts_length = pkts_length;
  //              rc->req->type = type;
  //              rc->next = rq->head;
	//	rc->next->prev = rc;
	//	rc->prev = NULL;
  //              rq->head = rc;
  //              return NULL;
  //      }
  //      return NULL;
}

static inline struct request * fake_work_rq_update(struct request_queue * rq, 
                                                struct mbuf * pkt, uint16_t req_type)
{
        struct request * req = mempool_alloc(&request_mempool);
        if (unlikely(!req)) {
            mbuf_free(pkt);
            return NULL;
        }
        req->type = req_type;
        req->pkts_length = 1;
        req->mbufs[0] = pkt;
        return req;
}

volatile struct jbsq_preemption preempt_check[MAX_WORKERS];
volatile struct networker_pointers_t networker_pointers;
volatile struct jbsq_worker_response worker_responses[MAX_WORKERS];
volatile struct jbsq_dispatcher_request dispatcher_requests[MAX_WORKERS];
struct worker_state dispatch_states[MAX_WORKERS];
uint8_t idle_list[MAX_WORKERS];
uint8_t idle_list_head;
