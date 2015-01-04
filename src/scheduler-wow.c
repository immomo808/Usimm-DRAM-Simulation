#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include "utlist.h"
#include "utils.h"

#include "memory_controller.h"

extern long long int CYCLE_VAL;
extern int WQ_CAPACITY;
extern int NUM_BANKS;

// write queue high water mark; begin draining writes if write queue exceeds this value
#define HI_WM (WQ_CAPACITY - NUM_BANKS)
// end write queue drain once write queue has this many writes in it
#define LO_WM (HI_WM - 8)
#define AUTO_PRECHARGE 1
#define THREAD_PHASE 1
#define TRAFFIC_LIGHT 1
#define LOCALITY 1

#define MAX_ROWS 32768
#define MAX_DISTANCE 13
#define M2C_INTERVAL 970
#define C2C_INTERVAL 220

#if THREAD_PHASE
// Thread phase prediction
int *distance;
int *interval;
int *phase;
extern int NUMCORES;
extern long long int * committed;
#endif

#if TRAFFIC_LIGHT 
// Traffic light
int *traffic_light;
int *requests_per_channel;
int *requests_per_rank;
#endif

#if LOCALITY
// row buffer locality between thread
int *localityCounter;
#endif

// 1 means we are in write-drain mode for that channel
int drain_writes[MAX_NUM_CHANNELS];

typedef struct state {
	int marked;
	int incoming;
} State;

void init_distance_interval() {
	for (int i = 0; i < NUMCORES; i++) {
		distance[i] = 0;
		interval[i] = 0;
		phase[i] = 0;
	}
}

void init_scheduler_vars()
{
    // initialize all scheduler variables here
#if THREAD_PHASE
	distance = (int*)malloc( NUMCORES * sizeof(int));
	interval = (int*)malloc( NUMCORES * sizeof(int));
	phase = (int*)malloc( NUMCORES * sizeof(int));
	init_distance_interval();
#endif
#if TRAFFIC_LIGHT 
	traffic_light = (int*)malloc( NUMCORES * sizeof(int));
	requests_per_channel = (int*)malloc( NUMCORES * MAX_NUM_CHANNELS * sizeof(int));
	requests_per_rank = (int*)malloc( NUMCORES * MAX_NUM_CHANNELS * MAX_NUM_BANKS * sizeof(int));
#endif
#if LOCALITY
	localityCounter = (int*)malloc( MAX_NUM_RANKS * MAX_NUM_BANKS * MAX_ROWS * sizeof(int));
#endif
    return;
}

void stateAssign(int channel) {
	request_t * rd_ptr = NULL;
	request_t * wr_ptr = NULL;
	LL_FOREACH(write_queue_head[channel], wr_ptr)
	{
		if (wr_ptr->user_ptr == NULL) {
			State * st = (State*) malloc (sizeof(State));
			st->marked = 0;
			st->incoming = 1;
			wr_ptr->user_ptr = st;
		}
	}
	LL_FOREACH(read_queue_head[channel], rd_ptr)
	{
		if (rd_ptr->user_ptr == NULL) {
			State * st = (State*) malloc (sizeof(State));
			st->marked = 0;
			st->incoming = 1;
			rd_ptr->user_ptr = st;
		}
	}
}

#if THREAD_PHASE
void predictThreadPhase(int channel)
{
	request_t * req_ptr = NULL;
	LL_FOREACH(read_queue_head[channel],req_ptr)
	{
		// commit count is used by the incoming request only.
		State * st = (State*)(req_ptr->user_ptr);
		if (st->incoming)
		{
			int thread_id = req_ptr->thread_id;

			if (phase[thread_id] == 1)
			{
				if (committed[thread_id] > interval[thread_id])
				{
					distance[thread_id] = 0;
					interval[thread_id] = committed[thread_id] + C2C_INTERVAL;
					phase[thread_id] = 1;
				}
				else
				{
					distance[thread_id] += 1;
					if (distance[thread_id] > MAX_DISTANCE) phase[thread_id] = 0;
				}
			}
			else
			{
				if (committed[thread_id] > interval[thread_id])
				{
					interval[thread_id] = 0;
					interval[thread_id] = committed[thread_id] + M2C_INTERVAL;
					phase[thread_id] = 1;
				}
				else
				{
					distance[thread_id] += 1;
					if(distance[thread_id] > MAX_DISTANCE) phase[thread_id] = 0;
				}
			}
			st->incoming = 0;
		}
	}
}
#endif

#if TRAFFIC_LIGHT 
void updateTrafficLight() {
	// TODO: Traffic light
	// Iterate through all the ranks and channels to know the request number of each thread toward ranks and channels
	// Reset first
	for (int ch = 0; ch < NUMCORES * MAX_NUM_CHANNELS; ch++) {
		requests_per_channel[ch] = 0;
	}
	for (int rk = 0; rk < NUMCORES * MAX_NUM_CHANNELS * MAX_NUM_RANKS; rk++) {
		requests_per_rank[rk] = 0;
	}
	for (int coreid = 0; coreid < NUMCORES; coreid++) {
		traffic_light[coreid] = 0;
	}
	// Compute
	request_t * rd_ptr = NULL;
	for(int ch = 0; ch < MAX_NUM_CHANNELS; ch++){
		LL_FOREACH(read_queue_head[ch], rd_ptr){
			int channel_index =
				rd_ptr->thread_id * MAX_NUM_CHANNELS + ch;
			int rank_index = 
				rd_ptr->thread_id * MAX_NUM_CHANNELS * MAX_NUM_RANKS + 
				ch * MAX_NUM_RANKS + (rd_ptr->dram_addr).rank; 
			requests_per_rank[rank_index]++;
			requests_per_channel[channel_index]++;
		}
	}
	// Turn on the light
	for(int ch = 0; ch < MAX_NUM_CHANNELS; ch++){
		if (drain_writes[ch]) {
			for (int coreid = 0; coreid < NUMCORES; coreid++) {
				if (requests_per_channel[coreid * MAX_NUM_CHANNELS + ch] > 0) {
					traffic_light[coreid] = 1;
				}
			}	
		}
		for (int rk = 0; rk < MAX_NUM_RANKS; rk++) {
			if (forced_refresh_mode_on[ch][rk]) {
				for (int coreid = 0; coreid < NUMCORES; coreid++) {
					if (requests_per_rank[coreid * MAX_NUM_CHANNELS * MAX_NUM_RANKS + ch * MAX_NUM_RANKS + rk] > 0) {
						traffic_light[coreid] = 1;
					}
				}	
			}
		}
	}
}
#endif

#if LOCALITY
void updateLocality(int channel) {
	// reset localityCounter
	/*
	   for (int i = 0; i < MAX_NUM_RANKS * MAX_NUM_BANKS * MAX_ROWS; i++) {
	   localityCounter[i] = 0;
	   }*/
	// use reverse reset
	request_t * rd_ptr = NULL;
	request_t * wr_ptr = NULL;
	LL_FOREACH(write_queue_head[channel], wr_ptr)
	{
		dram_address_t * dram_addr = &(wr_ptr->dram_addr);
		localityCounter[dram_addr->rank * MAX_NUM_BANKS * MAX_ROWS + dram_addr->bank * MAX_ROWS + dram_addr->row] = 0;
	}
	LL_FOREACH(read_queue_head[channel], rd_ptr)
	{
		dram_address_t * dram_addr = &(rd_ptr->dram_addr);
		localityCounter[dram_addr->rank * MAX_NUM_BANKS * MAX_ROWS + dram_addr->bank * MAX_ROWS + dram_addr->row] = 0;
	}
	LL_FOREACH(write_queue_head[channel], wr_ptr)
	{
		dram_address_t * dram_addr = &(wr_ptr->dram_addr);
		localityCounter[dram_addr->rank * MAX_NUM_BANKS * MAX_ROWS + dram_addr->bank * MAX_ROWS + dram_addr->row]++;
	}
	LL_FOREACH(read_queue_head[channel], rd_ptr)
	{
		dram_address_t * dram_addr = &(rd_ptr->dram_addr);
		localityCounter[dram_addr->rank * MAX_NUM_BANKS * MAX_ROWS + dram_addr->bank * MAX_ROWS + dram_addr->row]++;
	}
}
#endif

int hit(request_t * req)
{
    if (req == NULL)
    {
        return 0;
    }
    else
    {
        if (req->operation_type == READ)
        {
            return req->command_issuable && req->next_command == COL_READ_CMD;
        }
        else
        {
            return req->command_issuable && req->next_command == COL_WRITE_CMD;
        }
    }
}

#if THREAD_PHASE
int compare_phase(request_t * req_a, request_t * req_b)
{
	int req_a_core = req_a->thread_id,
		req_b_core = req_b->thread_id;
	if (phase[req_a_core] && !phase[req_b_core]) return 1;
	if (!phase[req_a_core] && phase[req_b_core]) return 0;
	return -1;
}
#endif

#if TRAFFIC_LIGHT
int compare_traffic_light(request_t * req_a, request_t * req_b)
{
	int req_a_core = req_a->thread_id,
		req_b_core = req_b->thread_id;

	if (!traffic_light[req_a_core] && traffic_light[req_b_core]) return 1;
	if (traffic_light[req_a_core] && !traffic_light[req_b_core]) return 0;
	return -1;
}
#endif

#if LOCALITY
int compare_locality(request_t * req_a, request_t * req_b)
{
	dram_address_t * dram_addr = &(req_a->dram_addr);
	int locality_a = localityCounter[dram_addr->rank * MAX_NUM_BANKS * MAX_ROWS + dram_addr->bank * MAX_ROWS + dram_addr->row];
	dram_addr = &(req_b->dram_addr);
	int locality_b = localityCounter[dram_addr->rank * MAX_NUM_BANKS * MAX_ROWS + dram_addr->bank * MAX_ROWS + dram_addr->row];
	if (locality_a > locality_b) return 1;
	if (locality_a < locality_b) return 0;
	return -1;
}
#endif

int compare(request_t * req_a, request_t * req_b)
{
	if (req_b == NULL) return 1;

    if (req_a == NULL)
    {
        int hit_b = hit(req_b);
        if (hit_b) return 0;
        return 1;
    }
    else
    {
        // Request type should be the same
        assert(req_a->operation_type == req_b->operation_type);

        int hit_a = hit(req_a),
            hit_b = hit(req_b);
        if (hit_a && !hit_b) return 1;
        if (!hit_a && hit_b) return 0;
		if (hit_a && hit_b)
		{
#if TRAFFIC_LIGHT
			if (compare_traffic_light(req_a, req_b) > 0) return 1;
			if (!compare_traffic_light(req_a, req_b)) return 0;
#endif
#if THREAD_PHASE
			return compare_phase(req_a, req_b);
#endif
		}
        return 1;
    }
}

void schedule(int channel)
{
    request_t * rd_ptr = NULL;
    request_t * wr_ptr = NULL;
	request_t * issue  = NULL;

	stateAssign(channel);
#if THREAD_PHASE
	predictThreadPhase(channel);
#endif
#if TRAFFIC_LIGHT
	updateTrafficLight();
#endif
#if LOCALITY
	updateLocality(channel);
#endif
    // if in write drain mode, keep draining writes until the
    // write queue occupancy drops to LO_WM
    if (drain_writes[channel] && (write_queue_length[channel] > LO_WM))
	{
        drain_writes[channel] = 1; // Keep draining.
    }
	else
	{
		drain_writes[channel] = 0; // No need to drain.
    }

    // initiate write drain if either the write queue occupancy
    // has reached the HI_WM , OR, if there are no pending read
    // requests
    if (write_queue_length[channel] > HI_WM)
	{
        drain_writes[channel] = 1;
    }
	else
	{
        if (!read_queue_length[channel])
            drain_writes[channel] = 1;
    }

    // If in write drain mode, look through all the write queue
    // elements (already arranged in the order of arrival), and
    // issue the command for the first request that is ready
    if (drain_writes[channel])
    {
        // Row hit write
		wr_ptr = NULL;
        LL_FOREACH(write_queue_head[channel], wr_ptr)
        {
			if (!compare(issue, wr_ptr))
			{
				issue = wr_ptr;
			}
        }
        if (write_queue_length[channel] < HI_WM)
        {
            // Row hit read
            if (!issue)
            {
                rd_ptr = NULL;
                LL_FOREACH(read_queue_head[channel], rd_ptr)
                {
                    if (!compare(issue, rd_ptr))
                    {
                        issue = rd_ptr;
                    }
                }
            }
            // Random write
            if (!issue)
            {
                wr_ptr = NULL;
                LL_FOREACH(write_queue_head[channel], wr_ptr)
                {
                    if (wr_ptr->command_issuable)
                    {
						if (!issue)
                        	issue = wr_ptr;
#if LOCALITY
						else if (!compare_locality(issue, wr_ptr))
							issue = wr_ptr;
#endif
                    }
                }
            }
            // Random read
            if (!issue)
            {
                rd_ptr = NULL;
                LL_FOREACH(read_queue_head[channel], rd_ptr)
                {
                    if (rd_ptr->command_issuable)
                    {
						if (!issue)
                        	issue = rd_ptr;
#if TRAFFIC_LIGHT
						int light = compare_traffic_light(issue, rd_ptr);
						if (!light)
							issue = rd_ptr;
						else if (light < 0)
						{
#endif
#if THREAD_PHASE
							int phase = compare_phase(issue, rd_ptr);
							if (!phase) issue = rd_ptr;
							else if (phase < 0)
							{
#endif
#if LOCALITY
								if (!compare_locality(issue, rd_ptr)) issue = rd_ptr;
#endif
#if THREAD_PHASE
							}
#endif
#if TRAFFIC_LIGHT
						}
#endif
                    }
                }
            }
        }
        else
        {
            // Random write
            if (!issue)
            {
                wr_ptr = NULL;
                LL_FOREACH(write_queue_head[channel], wr_ptr)
                {
                    if (wr_ptr->command_issuable)
                    {
						if (!issue)
                        	issue = wr_ptr;
#if LOCALITY
						else if (!compare_locality(issue, wr_ptr))
							issue = wr_ptr;
#endif
                    }
                }
            }
            // Row hit read
            if (!issue)
            {
                rd_ptr = NULL;
                LL_FOREACH(read_queue_head[channel], rd_ptr)
                {
                    if (!compare(issue, rd_ptr))
                    {
                        issue = rd_ptr;
                    }
                }
            }
            // Random read
            if (!issue)
            {
                rd_ptr = NULL;
                LL_FOREACH(read_queue_head[channel], rd_ptr)
                {
                    if (rd_ptr->command_issuable)
                    {
						if (!issue)
                        	issue = rd_ptr;
#if TRAFFIC_LIGHT
						int light = compare_traffic_light(issue, rd_ptr);
						if (!light)
							issue = rd_ptr;
						else if (light < 0)
						{
#endif
#if THREAD_PHASE
							int phase = compare_phase(issue, rd_ptr);
							if (!phase) issue = rd_ptr;
							else if (phase < 0)
							{
#endif
#if LOCALITY
								if (!compare_locality(issue, rd_ptr)) issue = rd_ptr;
#endif
#if THREAD_PHASE
							}
#endif
#if TRAFFIC_LIGHT
						}
#endif
                    }
                }
            }
        }
    }

    // Draining Reads
    // look through the queue and find the first request whose
    // command can be issued in this cycle and issue it 
    // Simple FCFS 
    if (!drain_writes[channel])
    {
        // Row hit read
		rd_ptr = NULL;
        LL_FOREACH(read_queue_head[channel],rd_ptr)
        {
			if (!compare(issue, rd_ptr))
			{
				issue = rd_ptr;
			}
        }
        // Row hit write
		if (!issue)
		{
            wr_ptr = NULL;
			LL_FOREACH(write_queue_head[channel], wr_ptr)
			{
				if (!compare(issue, wr_ptr))
				{
					issue = wr_ptr;
				}
			}
		}
        // Random read
		if (!issue)
		{
			rd_ptr = NULL;
			LL_FOREACH(read_queue_head[channel], rd_ptr)
			{
				if (rd_ptr->command_issuable)
				{
					if (!issue)
						issue = rd_ptr;
#if TRAFFIC_LIGHT
					int light = compare_traffic_light(issue, rd_ptr);
					if (!light)
						issue = rd_ptr;
					else if (light < 0)
					{
#endif
#if THREAD_PHASE
						int phase = compare_phase(issue, rd_ptr);
						if (!phase) issue = rd_ptr;
						else if (phase < 0)
						{
#endif
#if LOCALITY
							if (!compare_locality(issue, rd_ptr)) issue = rd_ptr;
#endif
#if THREAD_PHASE
						}
#endif
#if TRAFFIC_LIGHT
					}
#endif
				}
			}
		}
        // Random write
		if (!issue)
		{
			wr_ptr = NULL;
			LL_FOREACH(write_queue_head[channel], wr_ptr)
			{
				if (wr_ptr->command_issuable)
				{
					if (!issue)
						issue = wr_ptr;
#if LOCALITY
					else if (!compare_locality(issue, wr_ptr))
						issue = wr_ptr;
#endif
				}
			}
		}
    }

	if (issue != NULL && issue->command_issuable)
	{
		issue_request_command(issue);
#if AUTO_PRECHARGE
		if ((issue->next_command == COL_READ_CMD || issue->next_command == COL_WRITE_CMD) && 
			is_autoprecharge_allowed(channel, issue->dram_addr.rank, issue->dram_addr.bank))
		{
			wr_ptr = NULL;
			LL_FOREACH(write_queue_head[channel], wr_ptr)
			{
				if (!wr_ptr->request_served
						&& issue->dram_addr.rank == wr_ptr->dram_addr.rank
						&& issue->dram_addr.bank == wr_ptr->dram_addr.bank
						&& issue->dram_addr.row  == wr_ptr->dram_addr.row)
				{
					return; // has hit, no auto precharge
				}
			}
			rd_ptr = NULL;
			LL_FOREACH(read_queue_head[channel], rd_ptr)
			{
				if (!rd_ptr->request_served
						&& issue->dram_addr.rank == rd_ptr->dram_addr.rank
						&& issue->dram_addr.bank == rd_ptr->dram_addr.bank
						&& issue->dram_addr.row  == rd_ptr->dram_addr.row)
				{
					return; // has hit, no auto precharge
				}
			}
			issue_autoprecharge(channel, issue->dram_addr.rank, issue->dram_addr.bank);
			// nothing issuable this cycle
			return;
		}
#endif
	}
}

void scheduler_stats()
{
    /* Nothing to print for now. */
}

