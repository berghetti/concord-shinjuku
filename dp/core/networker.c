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

/*
 * networker.c - networking core functionality
 *
 * A single core is responsible for receiving all network packets in the
 * system and forwading them to the dispatcher.
 */
#include <stdio.h>

#include <ix/vm.h>
#include <ix/cfg.h>
#include <ix/log.h>
#include <ix/mbuf.h>
#include <ix/dispatch.h>
#include <ix/ethqueue.h>
#include <ix/transmit.h>

#include <asm/chksum.h>

#include <net/ip.h>
#include <net/udp.h>
#include <net/ethernet.h>

#include <ix/leveldb.h>
#include <time.h>

#include "helpers.h"
#include "benchmark.h"

extern volatile bool INIT_FINISHED;
volatile bool TEST_STARTED = false;

#ifdef LOAD_LEVEL // Passed during compilation
	double load_level = LOAD_LEVEL/100.0; 
#else
	double load_level = 0;
#endif

struct custom_payload* generate_benchmark_request(struct mbuf* temp, uint64_t t);
struct db_req* generate_db_req(struct request * req);

/**
 * do_networking - implements networking core's functionality
 */
void do_networking(void)
{
	log_info("Do networking started \n");
	int i, j, num_recv;
	rqueue.head = NULL;
	while (1)
	{
		eth_process_poll();
		num_recv = eth_process_recv();
		if (num_recv == 0)
			continue;
		while (networker_pointers.cnt != 0);
		for (i = 0; i < networker_pointers.free_cnt; i++)
		{
			struct request * req = networker_pointers.reqs[i];
			for (j = 0; j < req->pkts_length; j++) {
				mbuf_free(req->mbufs[j]);
			}
			mempool_free(&request_mempool, req);
		}
		networker_pointers.free_cnt = 0;
	j = 0;
        for (i = 0; i < num_recv; i++) {
			struct request * req = rq_update(&rqueue, recv_mbufs[i]);
			if (req) {
				networker_pointers.reqs[j] = req;
				networker_pointers.types[j] = (uint8_t) req->type;
				j++;
			}
        }
        networker_pointers.cnt = num_recv;
	}
}

/**
 * do_fake_networking - implements fake network logic with a given benchmark
 */
void do_fake_networking(int num_cpus)
{
	// Adjust the load level depending on the number of CPUs.
	load_level = load_level * (num_cpus-2);

	srand(time(NULL));
	TEST_STARTED = true;
	log_info("Generating fake work\n");
	assert(load_level && "No load level passed, exiting");
	log_info("Load level:  %f\n", load_level);
	log_info("Test started\n");

	uint64_t i, j, total_packet = 0;
	rqueue.head = NULL;
	while (!INIT_FINISHED);
	
	while (true)
	{
		#if BENCHMARK_CREATE_NO_PACKET != -1
			if (total_packet >= BENCHMARK_CREATE_NO_PACKET)
				break;
			
			total_packet++;
		#endif
		
		while (networker_pointers.cnt != 0);
		for (i = 0; i < networker_pointers.free_cnt; i++)
		{
			struct request * req = networker_pointers.reqs[i];
			for (j = 0; j < req->pkts_length; j++) {
				mbuf_free(req->mbufs[j]);
			}
			mempool_free(&request_mempool, req);
		}
		networker_pointers.free_cnt = 0;

		for (uint64_t t = 0; t < ETH_RX_MAX_BATCH; t++)
		{
			struct mbuf* temp = mbuf_alloc_local();
			uint16_t req_type = 0; // For now, only 1 port/type
			struct request * req = fake_work_rq_update(&rqueue, temp, req_type);
			if(req){
				/* -------- Generate fake req -------- */ 
				generate_db_req(req);
				/* -------- Send -------- */ 
				networker_pointers.reqs[t] = req;
				networker_pointers.types[t] = req_type; 	
			}
		}
		
		networker_pointers.cnt = ETH_RX_MAX_BATCH;
	}
}



struct db_req* generate_db_req(struct request * temp)
{
	struct db_req* req = mbuf_mtod(temp->mbufs[0], struct db_req *);
	#if BENCHMARK_TYPE == 0
	req->type = DB_ITERATOR; 
	#elif BENCHMARK_TYPE == 1
	req->type = (rand() % 2) ? DB_GET : DB_ITERATOR;
	#elif BENCHMARK_TYPE == 2
	req->type = (rand() % 1000) < 995 ? DB_GET : DB_ITERATOR;
	#elif (BENCHMARK_TYPE == 3) || (BENCHMARK_TYPE == 4)
	req->type = DB_GET;
	#elif (BENCHMARK_TYPE == 5)
	uint32_t num = rand() % 100;
	if(num < 44)
		req->type = DB_GET;
	else if(num < 48)
		req->type = DB_ITERATOR;
	else if(num < 92)
		req->type = DB_PUT;
	else if(num < 96)
		req->type = DB_DELETE;
	else	req->type = DB_SEEK;
	#else
  assert(0 && "Unknown benchmark type, quitting");
	#endif

	if (req->type == DB_GET)
	{
		int key_num = rand() % DB_NUM_KEYS;
		snprintf(req->key, KEYSIZE, "key%d", key_num);
		strcpy(req->val, "fakevalue");
		#if BENCHMARK_TYPE == 4
		req->ns = (uint64_t)(get_random_expo(MU) * 1000);
		#else
		req->ns = BENCHMARK_DB_GET_NS;
		#endif 
	} 
	else if(req->type == DB_ITERATOR)
	{
		strcpy(req->key, "");
		strcpy(req->key, "");
		req->ns = BENCHMARK_DB_ITERATOR_NS;
	}
	else if(req->type == DB_DELETE)
	{
		#if BENCHMARK_TYPE == 5
		req->ns = BENCHMARK_DB_DELETE_NS;
		#endif
	}
	else if(req->type == DB_PUT)
	{
		#if BENCHMARK_TYPE == 5
		req->ns = BENCHMARK_DB_PUT_NS;
		#endif	
	}
	else if (req->type == DB_SEEK)
	{
		#if BENCHMARK_TYPE == 5
		req->ns = BENCHMARK_DB_SEEK_NS;
		#endif	
	}

	// Wait for given inter-arrival time
	uint64_t wait_time_cycles = (1000 * CPU_FREQ_GHZ)/(MU*load_level);
	uint64_t start_time = rdtsc();
	while(rdtsc() - start_time < wait_time_cycles);
	req->ts = get_ns();
	return req;
}

struct custom_payload* generate_benchmark_request(struct mbuf* temp, uint64_t t) 
{
	struct custom_payload *
	req = mbuf_mtod(temp, struct custom_payload *);

	req->id = t + 1;

	#if BENCHMARK_TYPE == 1
	req->ns = (rand() % 2) ? 1 * 1000 : 100 * 1000;
	#elif BENCHMARK_TYPE == 2
	req->ns = (rand() % 1000) < 995 ? 0.5 * 1000 : 500 * 1000;
	#else
	// No Benchmark Provided
	#endif

	req->timestamp = get_us();
	return req;
}
