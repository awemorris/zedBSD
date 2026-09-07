/* Production stream prediction: no filesystem or scheduler model. */
#include <kern/readahead.h>
#include <kern/page.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#define PAGE ZEDBSD_PAGE_SIZE
static void check_request(struct readahead_state *state,struct readahead_request *r,uint64_t end,uint64_t eof)
{
 if(!r->length)return;
 assert(readahead_current(state,r));
 assert(r->offset>=end && r->offset<eof);
 assert(r->offset%PAGE==0 && r->length%PAGE==0 && r->length<=65536);
 assert(r->offset+r->length<=((end+PAGE-1)&~(uint64_t)(PAGE-1))+state->window);
 assert(r->offset+r->length<=((eof+PAGE-1)&~(uint64_t)(PAGE-1)));
}
int main(void)
{
 struct readahead_state state={0},other={0};
 struct readahead_request r,old;
 uint64_t next,end,seed=1,total;
 unsigned i;
 assert(readahead_observe(NULL,0,1,1,0,0,&r)==EINVAL);
 assert(readahead_observe(&state,0,65536,1048576,0,0,&r)==0 && r.length==0);
 assert(readahead_observe(&state,65536,65536,1048576,0,0,&r)==0 && r.offset==131072 && r.length==65536);
 old=r;assert(readahead_current(&state,&old) && !readahead_current(&other,&old));
 assert(readahead_observe(&state,131072,65536,1048576,65536,0,&r)==0);
 assert(state.window==131072 && r.length==65536);check_request(&state,&r,196608,1048576);
 assert(readahead_observe(&state,17,1,1048576,0,0,&r)==0 && r.length==0);
 assert(state.window==65536 && !readahead_current(&state,&old));
 assert(readahead_observe(&state,18,1,1048576,0,1,&r)==0 && r.length==0 && !state.valid);
 assert(readahead_observe(&state,19,1,1048576,0,0,&r)==0 && r.length==0);
 assert(readahead_observe(&state,20,1,1048576,0,0,&r)==0 && r.length==65536);
 old=r;readahead_reset(&state);assert(!readahead_current(&state,&old));
 total=0;
 for(i=0;i<32768;i++) {
  assert(readahead_observe(&state,i,1,1048576,0,0,&r)==0);
  check_request(&state,&r,i+1,1048576);total+=r.length;
  assert(total<=i+1+65536+PAGE);
 }
 assert(total<1048576);
 readahead_reset(&state);total=0;
 for(i=0;i<64;i++) {
  assert(readahead_observe(&state,(uint64_t)i*4096,4096,1048576,0,0,&r)==0);
  check_request(&state,&r,(uint64_t)(i+1)*4096,1048576);
  if(r.length){assert(r.length>=32768);total++;}
 }
 assert(total>=2 && total<=9);
 readahead_reset(&state);
 assert(readahead_observe(&state,0,4096,10000,0,0,&r)==0);
 assert(readahead_observe(&state,4096,4096,10000,0,0,&r)==0 && r.offset==8192 && r.length==4096);
 assert(readahead_observe(&state,8192,1808,10000,0,0,&r)==0 && r.length==0);
 assert(readahead_observe(&state,10000,0,10000,0,0,&r)==0 && !state.valid);
 assert(readahead_observe(&state,UINT64_MAX,1,UINT64_MAX,0,0,&r)==EINVAL && !state.valid);
 assert(readahead_observe(&state,INT64_MAX-8192,4096,INT64_MAX,0,0,&r)==0);
 assert(readahead_observe(&state,INT64_MAX-4096,4096,INT64_MAX,0,0,&r)==0 && r.length==0);
 readahead_reset(&state);next=0;
 for(i=0;i<50000;i++) {
  seed=seed*6364136223846793005ULL+1;
  if((seed&7)==0)next=(seed>>16)%524288;
  end=next+1+(seed%8192);
  assert(readahead_observe(&state,next,(size_t)(end-next),1048576,seed&4095,(seed&255)==0,&r)==0);
  check_request(&state,&r,end,1048576);next=end;
  if(next>524288)next=0;
 }
 state.generation=UINT64_MAX;readahead_reset(&state);
 assert(state.exhausted && state.generation==UINT64_MAX);
 for(i=0;i<3;i++)assert(readahead_observe(&state,i,1,1048576,0,0,&r)==0 && !r.length);
 puts("readahead state PASS: sequential/useful growth, random/pressure reset, one-byte horizon, EOF, overflow, generation exhaustion");
 return 0;
}
