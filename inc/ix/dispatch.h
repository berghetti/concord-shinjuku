#include <stdint.h>
#include <ucontext.h>

#include <ix/mempool.h>
#include <ix/ethqueue.h>

#define MAX_WORKERS   18

#define WAITING     0x00
#define ACTIVE      0x01

#define RUNNING     0x00
#define FINISHED    0x01
#define PREEMPTED   0x02

#define NOCONTENT   0x00
#define PACKET      0x01
#define CONTEXt     0x02

struct mempool_datastore task_datastore;
struct mempool task_mempool __attribute((aligned(64)));

struct worker_response
{
        uint64_t flag;
        void * rnbl;
        uint64_t timestamp;
        uint8_t type;
        char make_it_64_bytes[39];
} __attribute__((packed, aligned(64)));

struct dispatcher_request
{
        uint64_t flag;
        void * rnbl;
        uint8_t type;
        uint64_t timestamp;
        char make_it_64_bytes[39];
} __attribute__((packed, aligned(64)));

struct networker_pointers_t
{
        uint64_t cnt;
        struct mbuf * pkts[ETH_RX_MAX_BATCH];
        char make_it_64_bytes[64 - (ETH_RX_MAX_BATCH + 1) * 8];
} __attribute__((packed, aligned(64)));

struct task {
        void * runnable;
        uint8_t type;
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
                                     uint8_t type, uint64_t timestamp)
{
        struct task * tsk = mempool_alloc(&task_mempool);
        tsk->runnable = rnbl;
        tsk->type = type;
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
                                     uint8_t type, uint64_t timestamp)
{
        struct task * tsk = mempool_alloc(&task_mempool);
        tsk->runnable = rnbl;
        tsk->type = type;
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

static inline void tskq_dequeue(struct task_queue * tq, void ** rnbl_ptr,
                                uint8_t *type, uint64_t *timestamp)
{
        if (tq->head == NULL) {
                (*rnbl_ptr) = NULL;
                return;
        }
        (*rnbl_ptr) = tq->head;
        (*type) = tq->head->type;
        (*timestamp) = tq->head->timestamp;
        struct task * tsk = tq->head;
        tq->head = tq->head->next;
        mempool_free(&task_mempool, tsk);
        if (tq->head == NULL)
                tq->tail = NULL;
}

volatile struct networker_pointers_t networker_pointers;
volatile struct worker_response worker_responses[MAX_WORKERS];
volatile struct dispatcher_request dispatcher_requests[MAX_WORKERS];