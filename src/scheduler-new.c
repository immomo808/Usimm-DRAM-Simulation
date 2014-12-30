#include <stdio.h>
#include "utlist.h"
#include "utils.h"
#include <stdlib.h>

#include "memory_controller.h"
#include "params.h"

extern long long int CYCLE_VAL;
extern int NUMCORES;

typedef unsigned int uint;
#define MAX_NUMCORES (16)
#define GHTLength (512)
#define LLRLength (1)
int maxRefCount = 127;
int TH_GAIN;
long long int latestReadTime [MAX_NUMCORES];
int gainfulPCbased [MAX_NUMCORES];
int gainfulPCbased_ptr[MAX_NUM_CHANNELS];
///
int interval=0;
int threadreadcnt[16];
int threadreadcnt2[16];
int priorityarray[16];
int threadtypeflag;


typedef struct gainHistoryTable{
    int valid;
    int tag;
    int refCount;
} GHT_struct;

typedef struct lastReadRequest{
    uint pc;
    int gainCount;
    int valid;
} LLR_struct;

GHT_struct GHT[MAX_NUMCORES][GHTLength];
LLR_struct LLR [MAX_NUMCORES][LLRLength];

void initialGHT() {
    for(int core=0; core<NUMCORES; core++){
	for(int length=0; length<GHTLength; length++){
	    GHT[core][length].valid = 0;
	    GHT[core][length].tag = 0;
	    GHT[core][length].refCount = 0;
	}
    }
}

void initialLLR(){
    for(int core=0; core<NUMCORES; core++){
	for(int l =0; l<LLRLength; l++){
	    LLR[core][l].valid = 0;
	    LLR[core][l].pc = 0;
	    LLR[core][l].gainCount = 0;
	}
	latestReadTime[core] = 0;
    }
}

GHT_struct* searchGHT (int core, uint pc){
    GHT_struct* target = NULL;
    for(int l=0; l<GHTLength; l++){
	GHT_struct* compare = &GHT[core][l];
	if(compare->valid != 0 && compare->tag == pc){
	    target = compare;
	    break;
	}
    }
    return target;
}

GHT_struct* evictGHT(int core){
    int minRefCount = maxRefCount +1;
    GHT_struct* target = NULL;
    for(int l=0; l<GHTLength; l++){
	GHT_struct* compare = &GHT[core][l];
	if(compare->valid == 0){
	    target = compare;
	    break;
	}
	else if(compare->valid !=0 && compare->refCount < minRefCount){
	    minRefCount = compare->refCount;
	    target = compare;
	}
    }
    return target;
}

void commitGHT(int core, uint pc){
    GHT_struct* compare = searchGHT(core, pc);
    if(compare){
	compare->valid = 1;
	compare->refCount += 1;
	if(compare->refCount >= maxRefCount)
	    compare->refCount = maxRefCount;
    }
    else{
	compare = evictGHT(core);
	compare->valid = 1;
	compare->tag = pc;
	compare->refCount = 1;
    }
}

void decrementGHT(int core, uint pc){
    GHT_struct* compare = searchGHT(core, pc);
    if(compare && compare->refCount > 0){
	if(compare->refCount == 1){
	    compare->valid = 0;
	}
	if(compare->refCount >1){
	    compare->refCount -= 1;
	}
    }
}

void commitLLR(int core, uint pc){
    int targetValid = LLR[core][0].valid;
    int targetGain = LLR[core][0].gainCount;
    uint targetPC = LLR[core][0].pc;
    if(targetValid && targetGain >= TH_GAIN){
	commitGHT(core, targetPC);
    }
    else if(targetValid && targetGain < TH_GAIN){
	//decrementGHT(core, targetPC);
    }

    /// restart LLR
    LLR[core][0].valid = 1;
    LLR[core][0].pc = pc;
    LLR[core][0].gainCount = 0;
}

void updateLLR(){
    request_t * rd_ptr = NULL;

    for(int core=0; core<NUMCORES; core++){
	for(int l=0; l<LLRLength; l++){
	    if(LLR[core][l].valid){
		LLR[core][l].gainCount++;
	    }
	}
    }

    for(int cnl=0; cnl<NUM_CHANNELS; cnl++){
	LL_FOREACH(read_queue_head[cnl], rd_ptr){
	    for(int core=0; core<NUMCORES; core++){
		if(rd_ptr->thread_id == core &&
			rd_ptr->arrival_time > latestReadTime[core]){
		    commitLLR(core, rd_ptr->instruction_pc);
		}
	    }
	}
    }

    //Update Last Read Time
    for(int cnl=0; cnl<NUM_CHANNELS; cnl++){
	LL_FOREACH(read_queue_head[cnl], rd_ptr){
	    for(int core=0; core<NUMCORES; core++){
		if(rd_ptr->thread_id == core &&
			rd_ptr->arrival_time > latestReadTime[core]){
		    latestReadTime[core] = rd_ptr->arrival_time;
		}
	    }
	}
    }
    //
}

void sortThread(){
    request_t * rd_ptr = NULL;
    for(int i=0; i<NUMCORES; i++){
	gainfulPCbased [i] = -1; //initial
    }

    int PCbasedScore [MAX_NUMCORES];
    int PCbasedCount [MAX_NUMCORES];
    for(int i=0; i<NUMCORES; i++){
	PCbasedScore[i]=0;
	PCbasedCount[i]=0;
    }
    for(int cnl=0; cnl<NUM_CHANNELS; cnl++){
	LL_FOREACH(read_queue_head[cnl], rd_ptr){
	    uint thread = rd_ptr->thread_id;
	    uint pc = rd_ptr->instruction_pc;
	    GHT_struct* target = searchGHT(thread,pc);
	    if(target){
		if(PCbasedCount[thread] == 0){
		    PCbasedScore[thread] += target->refCount;
		}
	    }
	    PCbasedCount[thread]++;
	}
    }
    for(int i=0; i<NUMCORES; i++){
	gainfulPCbased[i] = i;
	if(PCbasedScore[i] ==0){
	    gainfulPCbased[i] = -1;
	}
    }

    ///sort PCbased
    for(int i=0; i<NUMCORES; i++){
	for(int j=i+1; j<NUMCORES; j++){
	    if(PCbasedScore[j] > PCbasedScore[i]){
		int temp = PCbasedScore[i];
		PCbasedScore[i] = PCbasedScore[j];
		PCbasedScore[j] = temp;
		temp = gainfulPCbased[i];
		gainfulPCbased[i] = gainfulPCbased[j];
		gainfulPCbased[j] = temp;
	    }
	}
    }
    ///
    
    for(int i=0; i<NUMCORES; i++){
	if(i>1)
	    gainfulPCbased[i] = -1;
    }
 }

int getGainfulPCbased(int channel){
    if(gainfulPCbased_ptr[channel] < NUMCORES){
	int gain = gainfulPCbased[gainfulPCbased_ptr[channel]];
	gainfulPCbased_ptr[channel] ++ ;
	return gain;///return highest priority thread number
    }
    return -1;
}

int readRowHitPC(int channel, int thread_id){
    request_t * rd_ptr = NULL;
    int rank, bank, row;
    LL_FOREACH(read_queue_head[channel], rd_ptr) {
	rank = rd_ptr->dram_addr.rank;
	bank = rd_ptr->dram_addr.bank;
	row = rd_ptr->dram_addr.row;

	if(rd_ptr->command_issuable && rd_ptr->thread_id == thread_id &&
		dram_state[channel][rank][bank].active_row == row){
	    if(rd_ptr->next_command == COL_READ_CMD){
	    }
	    issue_request_command(rd_ptr);
	    return 1;
	}
    }
    return 0;
}

int readPC(int channel, int thread_id){
    request_t * rd_ptr = NULL;
    int rank, bank;
    LL_FOREACH(read_queue_head[channel], rd_ptr) {
	rank = rd_ptr->dram_addr.rank;
	bank = rd_ptr->dram_addr.bank;

	if(rd_ptr->command_issuable && rd_ptr->thread_id == thread_id ){
	    if(rd_ptr->next_command == COL_READ_CMD){
	    }
	    issue_request_command(rd_ptr);
	    return 1;

	}
    }
    return 0;
}

int readRowHit(int channel){
    request_t * rd_ptr = NULL;
    int rank, bank, row;
    LL_FOREACH(read_queue_head[channel], rd_ptr){
	rank = rd_ptr->dram_addr.rank;
	bank = rd_ptr->dram_addr.bank;
	row = rd_ptr->dram_addr.row;

	if(rd_ptr->command_issuable &&
          dram_state[channel][rank][bank].active_row == row){
	    issue_request_command(rd_ptr);
	    return 1;
	}
    }
    return 0;
}

int read(int channel){
    request_t * rd_ptr = NULL;
    int rank, bank;
    LL_FOREACH(read_queue_head[channel],rd_ptr) {
	rank = rd_ptr->dram_addr.rank;
	bank = rd_ptr->dram_addr.bank;

	if (rd_ptr->command_issuable){
	    issue_request_command(rd_ptr);
	    return 1;
	}
    }
    return 0;
}

int activatePC(int channel, int thread_id)
{
  request_t * rd_ptr = NULL;
  int rank, bank, row;
  LL_FOREACH(read_queue_head[channel],rd_ptr){
    rank = rd_ptr->dram_addr.rank;
    bank = rd_ptr->dram_addr.bank;
    row = rd_ptr->dram_addr.row;
    if ( rd_ptr->thread_id == thread_id &&
        is_activate_allowed(channel,rank,bank)) {
      issue_activate_command(channel,rank,bank,row);
      return 1;
    }
  }
  return 0;
}

int activate(int channel)
{
  request_t * rd_ptr = NULL;
  int rank, bank, row;
  LL_FOREACH(read_queue_head[channel],rd_ptr){
    rank = rd_ptr->dram_addr.rank;
    bank = rd_ptr->dram_addr.bank;
    row = rd_ptr->dram_addr.row;
    if ( is_activate_allowed(channel,rank,bank)) {
      issue_activate_command(channel,rank,bank,row);
      return 1;
    }
  }
  return 0;
}

////thread
void thread_priority()
{
	request_t * rd_ptr = NULL;
	int i=0,j=0;
	int max;
		if(interval == 200)
		{
			for(i=0;i<NUMCORES;i++)
			{
			//	printf("threadreadcnt[%d] is '%d'\n",i,threadreadcnt[i]);
				
				threadreadcnt[i]=0;
			}
		/*	for(i=0;i<NUMCORES;i++)
			{
				printf("priorityarray[%d] is '%d'\n",i,priorityarray[i]);
			}
			printf("threadtypeflag is '%d'\n",threadtypeflag);
			printf("\n");*/
			interval=0;
		}

		
		
			for(i=0;i<NUM_CHANNELS;i++)
			{
				LL_FOREACH(read_queue_head[i], rd_ptr)
				{
					if(rd_ptr->user_ptr ==NULL)
					{
						threadreadcnt[rd_ptr->thread_id]++;
						rd_ptr->user_ptr = malloc(sizeof(int));
						*((int *)rd_ptr->user_ptr) = 1;
					}
				}
			}
	
			for(i=0;i<NUMCORES;i++)
			{
				threadreadcnt2[i]=threadreadcnt[i];
			}

			
			for(i=0;i<NUMCORES;i++)
			{
				max=0;
				for(j=0;j<NUMCORES;j++)
				{
					if(threadreadcnt2[j]>threadreadcnt2[max])
						max = j;
				}
				threadreadcnt2[max] = -1;
				priorityarray[NUMCORES-1-i] = max;
			}
		
	
		threadtypeflag=0;
		max=0;
		for(i=1;i<NUMCORES;i++)
		{
			if((threadreadcnt[priorityarray[i]]-threadreadcnt[priorityarray[i-1]])>max)
			{
				max = threadreadcnt[priorityarray[i]]-threadreadcnt[priorityarray[i-1]];
				threadtypeflag=i-1;
			}
		}
}
////

///// thread issue
int issuebyreadcnt(int channel)
{
	request_t * rd_ptr = NULL;
	int i=0;

	 if(!drain_writes[channel])
    {
			for(i=0;i<=threadtypeflag;i++)
			{
				LL_FOREACH(read_queue_head[channel],rd_ptr)
				{
					if((rd_ptr->command_issuable) && (rd_ptr->thread_id==priorityarray[i]) && (rd_ptr->next_command == COL_READ_CMD) )
					{
						issue_request_command(rd_ptr);
						return 1;
					}
				}
			}
			for(i=0;i<=threadtypeflag;i++)
			{
				LL_FOREACH(read_queue_head[channel],rd_ptr)
				{
					if(rd_ptr->command_issuable && rd_ptr->thread_id==priorityarray[i])
					{
						issue_request_command(rd_ptr);
						return 1;
					}
				}
			}
			for(i=threadtypeflag+1;i<NUMCORES;i++)
			{
				LL_FOREACH(read_queue_head[channel],rd_ptr)
				{
					if((rd_ptr->command_issuable) && (rd_ptr->thread_id==priorityarray[i]) && (rd_ptr->next_command == COL_READ_CMD) )
					{
						
						issue_request_command(rd_ptr);
						return 1;
					}
				}
			}
			for(i=threadtypeflag+1;i<NUMCORES;i++)
			{
				LL_FOREACH(read_queue_head[channel],rd_ptr)
				{
					if(rd_ptr->command_issuable && rd_ptr->thread_id==priorityarray[i])
					{
						issue_request_command(rd_ptr);
						return 1;
					}
				}
			}
    }

	 return 0;
}
/////


void init_scheduler_vars()
{
    // initialize all scheduler variables here

    TH_GAIN = 300 * NUMCORES;
    initialGHT();
    initialLLR();

    ////
    int i=0;
    // initialize all scheduler variables here
    for(i=0;i<NUMCORES;i++)
    {
	threadreadcnt[i]=0;
	threadreadcnt2[i]=0;
	priorityarray[i]=0;
    }

    ////

    return;
}

// write queue high water mark; begin draining writes if write queue exceeds this value
#define HI_WM 40

// end write queue drain once write queue has this many writes in it
#define LO_WM 20

// 1 means we are in write-drain mode for that channel
int drain_writes[MAX_NUM_CHANNELS];

int cycle= 0;
void schedule(int channel)
{

    cycle++;

    request_t * rd_ptr = NULL;
    request_t * wr_ptr = NULL;

    if(channel == 0){
	updateLLR();
	for(int i=0; i<MAX_NUM_CHANNELS; i++){
	    gainfulPCbased_ptr[i] = 0; 
	}
	sortThread();


	/*for(int i=0; i<GHTLength; i++){
	  if(GHT[0][i].valid ==1 )
	//	printf("tag:%d ref:%d\n",GHT[0][i].tag,GHT[0][i].refCount);
	}*/

	///thread
	thread_priority();
	interval++;
    }

    /////*****
  //     interval++;	
    ////******



    // if in write drain mode, keep draining writes until the
    // write queue occupancy drops to LO_WM
    if (drain_writes[channel] && (write_queue_length[channel] > LO_WM)) {
	drain_writes[channel] = 1; // Keep draining.
    }
    else {
	drain_writes[channel] = 0; // No need to drain.
    }

    // initiate write drain if either the write queue occupancy
    // has reached the HI_WM , OR, if there are no pending read
    // requests
    if(write_queue_length[channel] > HI_WM)
    {
	drain_writes[channel] = 1;
    }
    else {
	if (!read_queue_length[channel])
	    drain_writes[channel] = 1;
    }


    // If in write drain mode, look through all the write queue
    // elements (already arranged in the order of arrival), and
    // issue the command for the first request that is ready
    if(drain_writes[channel])
    {

	LL_FOREACH(write_queue_head[channel], wr_ptr)
	{
	    if(wr_ptr->command_issuable)
	    {
		issue_request_command(wr_ptr);
		break;
	    }
	}
	return;
    }

    // Draining Reads
    // look through the queue and find the first request whose
    // command can be issued in this cycle and issue it 
    // Simple FCFS 
    if(!drain_writes[channel])
    {
	gainfulPCbased_ptr[channel] = 0;
	for(int getPC=getGainfulPCbased(channel); getPC > -1; getPC=getGainfulPCbased(channel)){
	    if(readRowHitPC(channel, getPC)){ 
		printf("PC Gain1\n");
		return;
	    }
	    if(readPC(channel,getPC)){
		printf("PC Gain2\n");
		return;
	    }

	}

	////thread
	if(issuebyreadcnt(channel) ){
	    printf("TH Gain\n");
	    return;
	}
	////


	//if(readRowHit(channel)) return;
	//if(read(channel)) return;

	gainfulPCbased_ptr[channel] = 0;
	for(int getPC=getGainfulPCbased(channel); getPC > -1; getPC=getGainfulPCbased(channel))	{
	    if(activatePC(channel, getPC)){
		printf("active 1\n");
		return;
	    }
	}
	if(activate(channel)){
	    printf("active 2\n");
	    return;
	}
	/*
	   LL_FOREACH(read_queue_head[channel],rd_ptr)
	   {:
	   if(rd_ptr->command_issuable)
	   {
	   issue_request_command(rd_ptr);
	//	printf("arr: %lld core: %d pc: %lld\n",rd_ptr->arrival_time,rd_ptr->thread_id,rd_ptr->instruction_pc);
	break;
	}
	}*/
	int not = 1;
	LL_FOREACH(read_queue_head[channel], rd_ptr){
	    if(rd_ptr->command_issuable && read_queue_length !=0){
		not =0;
	    }
	}
	if(not == 1){

//	    printf("wast: %d\n",cycle);
	}
	return;
    }
}

void scheduler_stats()
{
    /* Nothing to print for now. */
}

