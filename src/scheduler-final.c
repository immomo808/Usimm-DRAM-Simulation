#include <stdio.h>
#include <stdlib.h>
#include "utlist.h"
#include "utils.h"
#include "memory_controller.h"
#define MARKINGCAP 100
#define HI_WM (WQ_CAPACITY - NUM_BANKS)
#define LO_WM (HI_WM - 8)
#define MIN_WRITES_ONCE_WRITING_HAS_BEGUN 1
#define MAX_DISTANCE 13
#define M2C_INTERVAL 970
#define C2C_INTERVAL 220
#define TRAFFIC_LIGHT 0
#define MAX_ROWS 32768
typedef struct state {
	int marked;
	int incoming; 
} State; 

extern int WQ_CAPACITY;
extern int NUM_BANKS;
extern long long int CYCLE_VAL;
extern int NUMCORES;
int drain_writes[MAX_NUM_CHANNELS];
int writes_done_this_drain[MAX_NUM_CHANNELS];
int draining_writes_due_to_rq_empty[MAX_NUM_CHANNELS];



int *load_bank, *load_max, *load_all, marked_num;

#if TRAFFIC_LIGHT 
// Traffic light
int *traffic_light;
int *requests_per_channel;
int *requests_per_rank;
#endif

// Thread phase prediction
int *distance;
int *interval;
int *phase;
extern long long int * committed; 

// row buffer locality between thread
int *localityCounter;

// Cleaning the loading
void init_all_banks(){
	int bank_count = NUMCORES * MAX_NUM_CHANNELS * MAX_NUM_RANKS * MAX_NUM_BANKS;
	for(int index = 0; index < bank_count; index++)
		load_bank[index] = 0;
}

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
	int bank_count = NUMCORES * MAX_NUM_CHANNELS * MAX_NUM_RANKS * MAX_NUM_BANKS;
	load_bank = (int*)malloc( bank_count * sizeof(int) ); 
	load_max = (int*)malloc( NUMCORES * sizeof(int));
	load_all = (int*)malloc( NUMCORES * sizeof(int));
	distance = (int*)malloc( NUMCORES * sizeof(int));
	interval = (int*)malloc( NUMCORES * sizeof(int));
	phase = (int*)malloc( NUMCORES * sizeof(int));
#if TRAFFIC_LIGHT 
	traffic_light = (int*)malloc( NUMCORES * sizeof(int));
	requests_per_channel = (int*)malloc( NUMCORES * MAX_NUM_CHANNELS * sizeof(int));
	requests_per_rank = (int*)malloc( NUMCORES * MAX_NUM_CHANNELS * MAX_NUM_BANKS * sizeof(int));
#endif
	//	localityCounter = (int*)malloc( MAX_NUM_RANKS * MAX_NUM_BANKS * MAX_ROWS * sizeof(int));
	init_all_banks();
	init_distance_interval();
	marked_num = 0;
	return;
}


// Compare the Priority in Rule 2
// 1.Marked
// 2.Row hit
// 3.Core Rnking
// 4.FCFS
// and if REQ_B = NULL then return REQ_A
int higher(request_t *req_a, request_t *req_b){
	if( req_b == NULL ) return 1;                                                       // req_b = NULL -> always return req_a
	State * s_a = (State*)(req_a->user_ptr);
	State * s_b = (State*)(req_b->user_ptr);
	if (((s_a->marked)) && !(s_b->marked)) return 1;                              // if req_a marked and req_b didn't then return req_a
	int req_a_hit = req_a->command_issuable && (req_a->next_command == COL_READ_CMD), 
		req_b_hit = req_b->command_issuable && (req_b->next_command == COL_READ_CMD);   // row hit status
	int req_a_core  = req_a->thread_id, 
		req_b_core  = req_b->thread_id;                                                 // core of each req
#if TRAFFIC_LIGHT 
	int light_a  = traffic_light[req_a_core], 
		light_b  = traffic_light[req_b_core];                                                 // light of each req
#endif
	//	dram_address_t * dram_addr = &(req_a->dram_addr);
	//	int locality_a = localityCounter[dram_addr->rank * MAX_NUM_BANKS * MAX_ROWS + dram_addr->bank * MAX_ROWS + dram_addr->row];
	//	dram_addr = &(req_b->dram_addr);
	//	int locality_b = localityCounter[dram_addr->rank * MAX_NUM_BANKS * MAX_ROWS + dram_addr->bank * MAX_ROWS + dram_addr->row];
	if (!(s_a->marked)){
		if (s_b->marked) return 0;                                              
		else 
			if (req_b_hit)      return 0;                                               // hit   
			else if (req_a_hit) return 1;
			else                {
				//				if (locality_a > locality_b) return 1;
				//				if (locality_b > locality_a) return 0;
				if (phase[req_a_core] && !phase[req_b_core]) return 1;
				if (!phase[req_a_core] && phase[req_b_core]) return 0;
#if TRAFFIC_LIGHT 
				if (!light_a && light_b) return 1;
				if (light_a && !light_b) return 0;
#endif
				if (load_max[req_a_core] < load_max[req_b_core]) return 1;                          // all and max load ranking
				else if (load_max[req_a_core] > load_max[req_b_core]) return 0;
				else if (load_all[req_a_core] < load_all[req_b_core]) return 1;
				return 0;
			};
	}
	if (req_a_hit && !req_b_hit) return 1;
	if (!req_a_hit && req_b_hit) return 0;
	//	if (locality_a > locality_b) return 1;
	//	if (locality_b > locality_a) return 0;
	if (phase[req_a_core] && !phase[req_b_core]) return 1;
	if (!phase[req_a_core] && phase[req_b_core]) return 0;
#if TRAFFIC_LIGHT 
	if (!light_a && light_b) return 1;
	if (light_a && !light_b) return 0;
#endif
	if (load_max[req_a_core] < load_max[req_b_core]) return 1;                          // all and max load ranking
	else if (load_max[req_a_core] > load_max[req_b_core]) return 0;
	else if (load_all[req_a_core] < load_all[req_b_core]) return 1;
	return 0;
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

void predictThreadPhase(int channel) {
	request_t * req_ptr = NULL;
	LL_FOREACH(read_queue_head[channel],req_ptr) {
		// commit count is used by the incoming request only.
		State * st = (State*)(req_ptr->user_ptr);
		if(st->incoming) {
			int thread_id = req_ptr->thread_id;

			if(phase[thread_id] == 1) {
				if(committed[thread_id] > interval[thread_id]) {
					distance[thread_id] = 0;
					interval[thread_id] = committed[thread_id] + C2C_INTERVAL;
					phase[thread_id] = 1;
				} else {
					distance[thread_id] += 1;
					if(distance[thread_id] > MAX_DISTANCE){
						phase[thread_id] = 0;
					}
				}
			} else {
				if(committed[thread_id] > interval[thread_id]) {
					interval[thread_id] = 0;
					interval[thread_id] = committed[thread_id] + M2C_INTERVAL;
					phase[thread_id] = 1;
				} else {
					distance[thread_id] += 1;
					if(distance[thread_id] > MAX_DISTANCE){
						phase[thread_id] = 0;
					}
				}
			}
			st->incoming = 0;
		}
	}

}

void schedule(int channel)
{

	stateAssign(channel);
	predictThreadPhase(channel);
	//	updateLocality(channel);
	request_t * rd_ptr = NULL;
	request_t * wr_ptr = NULL;
	request_t * auto_ptr = NULL;
	int read_issued = 0;
	int write_issued = 0;

	// begin write drain if we're above the high water mark
	if((write_queue_length[channel] > HI_WM) && (!drain_writes[channel]))
	{
		drain_writes[channel] = 1;
		writes_done_this_drain[channel] = 0;
	}

	// also begin write drain if read queue is empty
	if((read_queue_length[channel] < 1) && (write_queue_length[channel] > 0) && (!drain_writes[channel]))
	{
		drain_writes[channel] = 1;
		writes_done_this_drain[channel] = 0;
		draining_writes_due_to_rq_empty[channel] = 1;
	}

	// end write drain if we're below the low water mark
	if((drain_writes[channel]) && (write_queue_length[channel] <= LO_WM) && (!draining_writes_due_to_rq_empty[channel]))
	{
		drain_writes[channel] = 0;
	}

	// end write drain that was due to read_queue emptiness only if at least one write has completed
	if((drain_writes[channel]) && (read_queue_length[channel] > 0) && (draining_writes_due_to_rq_empty[channel]) && (writes_done_this_drain[channel] > MIN_WRITES_ONCE_WRITING_HAS_BEGUN))
	{
		drain_writes[channel] = 0;
		draining_writes_due_to_rq_empty[channel] = 0;
	}

	// make sure we don't try to drain writes if there aren't any
	if(write_queue_length[channel] == 0)
	{
		drain_writes[channel] = 0;
	}

	// drain from write queue now
	if(drain_writes[channel])
	{
		// prioritize open row hits
		LL_FOREACH(write_queue_head[channel], wr_ptr)
		{
			// if COL_WRITE_CMD is the next command, then that means the appropriate row must already be open
			if(wr_ptr->command_issuable && (wr_ptr->next_command == COL_WRITE_CMD))
			{
				writes_done_this_drain[channel]++;
				issue_request_command(wr_ptr);
				write_issued = 1;
				break;
			}
		}
		request_t * issue = NULL;
		if(!write_issued){
			// if no write row hit, check read queue
			LL_FOREACH(read_queue_head[channel], rd_ptr)
			{
				// if COL_WRITE_CMD is the next command, then that means the appropriate row must already be open && batch first
				if(rd_ptr->command_issuable && (rd_ptr->next_command == COL_READ_CMD))
				{
					if (higher(rd_ptr, issue)) {
						issue = rd_ptr;
						read_issued = 1;
					}
				}
			}
		}

		if(!write_issued && !read_issued){
			// if no open rows, just issue any other available commands
			LL_FOREACH(write_queue_head[channel], wr_ptr)
			{
				if(wr_ptr->command_issuable)
				{
					issue_request_command(wr_ptr);
					write_issued = 1;
					break;
				}
			}
		}

		if (!write_issued && !read_issued) {
			LL_FOREACH(read_queue_head[channel], rd_ptr)
			{
				// Random one
				if(rd_ptr->command_issuable)
				{
					if (higher(rd_ptr, issue)) {
						issue = rd_ptr;
						read_issued = 1;
					}
				}
			}
		}

		if(issue){                                                          // Start issue
			issue_request_command(issue);
			read_issued = 1;
			State * st = (State*)(issue->user_ptr);
			if ((st->marked) && issue->next_command == COL_READ_CMD)
				marked_num--;
		}
		rd_ptr = issue;
		// try auto-precharge
		if (!write_issued && !read_issued) {
			return; // no request issued, quit
		}

		if (!write_issued && read_issued) {
			wr_ptr = rd_ptr;
		}

		if (is_autoprecharge_allowed(channel, wr_ptr->dram_addr.rank, wr_ptr->dram_addr.bank)) {
			//if (!write_issued && read_issued) {
			//	wr_ptr = rd_ptr;
			//}
			LL_FOREACH(write_queue_head[channel], auto_ptr) {
				if (!auto_ptr->request_served
						&& auto_ptr->dram_addr.rank == wr_ptr->dram_addr.rank
						&& auto_ptr->dram_addr.bank == wr_ptr->dram_addr.bank
						&& auto_ptr->dram_addr.row == wr_ptr->dram_addr.row) {
					return; // has hit, no auto precharge
				}
			}
			LL_FOREACH(read_queue_head[channel], auto_ptr) {
				if (!auto_ptr->request_served
						&& auto_ptr->dram_addr.rank == wr_ptr->dram_addr.rank
						&& auto_ptr->dram_addr.bank == wr_ptr->dram_addr.bank
						&& auto_ptr->dram_addr.row == wr_ptr->dram_addr.row) {
					return; // has hit, no auto precharge
				}
			}
			issue_autoprecharge(channel, wr_ptr->dram_addr.rank, wr_ptr->dram_addr.bank);
			// nothing issuable this cycle
			return;
		}
		return;
	}
	// do a read
	// Start PAR-BS
	if( marked_num == 0 ){          // if last batch finish
		init_all_banks();
		for(int ch = 0; ch < MAX_NUM_CHANNELS; ch++){
			LL_FOREACH(read_queue_head[ch], rd_ptr){
				int index = 
					rd_ptr->thread_id * MAX_NUM_CHANNELS * MAX_NUM_RANKS * MAX_NUM_BANKS + 
					ch * MAX_NUM_RANKS * MAX_NUM_BANKS +
					(rd_ptr->dram_addr).rank * MAX_NUM_BANKS + 
					(rd_ptr->dram_addr).bank; 
				if ( load_bank[index] < MARKINGCAP ){                   // Mark the Req if load <  MARKINGCAP (Rule 1)
					load_bank[index]++;
					marked_num++;
					State * st = (State*)(rd_ptr->user_ptr);
					if (st == NULL) { 
						// Batch is cross channel, but state initial is per-channel
						// st maybe NULL
						st = (State*) malloc (sizeof(State));
						st->marked = 0;
						st->incoming = 1;
						rd_ptr->user_ptr = st;
					}
					st->marked = 1;
				}
			}
		}
		int max_load = 0, all_load = 0;
		for(int coreid = 0; coreid < NUMCORES; coreid++){               // Compute all & max loading in each core
			for(int i = coreid * MAX_NUM_CHANNELS * MAX_NUM_RANKS * MAX_NUM_BANKS; 
					i < (coreid + 1) * MAX_NUM_CHANNELS * MAX_NUM_RANKS * MAX_NUM_BANKS;
					i++ ){
				all_load += load_bank[i];
				if ( max_load < load_bank[i] )
					max_load = load_bank[i];
			}
			load_max[coreid] = max_load;
			load_all[coreid] = all_load;
			max_load = 0;
			all_load = 0;
		}
	}

#if TRAFFIC_LIGHT 
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
#endif


	request_t *issue = NULL;
	LL_FOREACH(read_queue_head[channel], rd_ptr) {                       // Start batch
		if(rd_ptr->command_issuable)
			if(higher(rd_ptr, issue))
				issue = rd_ptr;
	}
	if(issue){                                                          // Start issue
		issue_request_command(issue);
		read_issued = 1;
		State * st = (State*)(issue->user_ptr);
		if ((st->marked) && issue->next_command == COL_READ_CMD)
			marked_num--;
	}
	else {
		// prioritize open row hits
		LL_FOREACH(write_queue_head[channel], wr_ptr)
		{
			// if COL_WRITE_CMD is the next command, then that means the appropriate row must already be open
			if(wr_ptr->command_issuable && (wr_ptr->next_command == COL_WRITE_CMD))
			{
				//writes_done_this_drain[channel]++;
				issue_request_command(wr_ptr);
				write_issued = 1;
				break;
			}
		}

		if (!write_issued) {
			// No read can issue, issue a random write
			LL_FOREACH(write_queue_head[channel], wr_ptr)
			{
				if(wr_ptr->command_issuable)
				{
					issue_request_command(wr_ptr);
					write_issued = 1;
					break;
				}
			}
		}
	}
	rd_ptr = issue;
	// try auto-precharge
	if (!write_issued && !read_issued) {
		return; // no request issued, quit
	}

	if (write_issued && !read_issued) {
		rd_ptr = wr_ptr;
	}


	if (is_autoprecharge_allowed(channel, rd_ptr->dram_addr.rank, rd_ptr->dram_addr.bank)) {
		//if (!read_issued && write_issued) {
		//	rd_ptr = wr_ptr;
		//}
		LL_FOREACH(read_queue_head[channel], auto_ptr) {
			if (!auto_ptr->request_served
					&& auto_ptr->dram_addr.rank == rd_ptr->dram_addr.rank
					&& auto_ptr->dram_addr.bank == rd_ptr->dram_addr.bank
					&& auto_ptr->dram_addr.row == rd_ptr->dram_addr.row) {
				return; // has hit, no auto precharge
			}
		}
		LL_FOREACH(write_queue_head[channel], auto_ptr) {
			if (!auto_ptr->request_served
					&& auto_ptr->dram_addr.rank == rd_ptr->dram_addr.rank
					&& auto_ptr->dram_addr.bank == rd_ptr->dram_addr.bank
					&& auto_ptr->dram_addr.row == rd_ptr->dram_addr.row) {
				return; // has hit, no auto precharge
			}
		}
		// no hit pending, auto precharge
		issue_autoprecharge(channel, rd_ptr->dram_addr.rank, rd_ptr->dram_addr.bank);
		return;

	}
}
void scheduler_stats()
{
	/* Nothing to print for now. */
}

