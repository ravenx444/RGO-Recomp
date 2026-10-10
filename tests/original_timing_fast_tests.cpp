#include "jfg/boot/original_timing_fast.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>
namespace ordinary {
#include "jfg/boot/original_timing_cache.h"
}
extern "C" { JfgOriginalTimingFast* jfg_original_timing_state = nullptr; }
static unsigned fallbacks=0;
extern "C" void jfg_phase9_execution_probe_predecoded(unsigned,unsigned,unsigned,void*) { ++fallbacks; }
static void check(bool okay,const char* reason) { if(!okay){std::cerr<<reason<<'\n';std::exit(1);} }
int main() {
 std::vector<uint8_t> ram(4U*1024U*1024U);
 uint32_t table=0x80010000;memcpy(ram.data()+0xfeaa0,&table,4);
 for(unsigned slot=1;slot<158;++slot){uint32_t base=0x80020000+slot*0x4000;memcpy(ram.data()+0x10000+slot*32,&base,4);}
 std::array<JfgTimingCacheLine,512> ic{},dc{};std::array<uint64_t,12> stats{};
 uint64_t revision=7,guest_count=0;
 JfgOriginalTimingFast fast{};fast.rdram=ram.data();fast.instruction_cache=ic.data();fast.data_cache=dc.data();fast.cache_counters=stats.data();
 fast.io_revision=&revision;fast.io_guest_count=&guest_count;fast.execution_fast_revision=revision;
 fast.execution_fast_deadline=UINT64_MAX;fast.execution_fast_profile_ready=true;
 jfg_original_timing_state=&fast;
 recomp_context cpu{};cpu.status_reg=0x20000000;
 uint64_t expected_count=0,half=0;uint32_t rng=0x73421499,previous_pc=0;bool previous_branch=false;
 for(unsigned i=0;i<250000;++i) {
  rng=rng*1664525U+1013904223U;
  unsigned primary=0,word=0,metadata=0;
  switch(i%6) {
   case 0:word=0x20;break;
   case 1:primary=0x23;metadata=1U<<12;break;
   case 2:primary=0x2b;metadata=2U<<12;break;
   case 3:primary=4;metadata=1U<<10;break;
   case 4:word=24;metadata=4U<<16;break;
   case 5:primary=17;word=(16U<<21)|2U;metadata=(1U<<8)|(4U<<16);break;
  }
  if(primary==0x23 || primary==0x2b) {
   cpu.r1=(i%11==0?0xa0000000U:i%13==0?0x00100000U:0x80000000U)+(rng&0x3ffffcU);
   word=(1U<<21)|(rng&0xffffU);
  }
  word|=primary<<26;
  if(i&1U)metadata|=0x800000U; // Host inlining hint must not change modeled costs.
  unsigned pc=(i%17==0?0x00100000U:0x80000000U)+0x1000+(i%97==0?(rng&0xfffcU):(i%8)*4);
  const auto cost=ordinary::jfg_full_cache_cost(pc,word,&cpu,ram.data())+half;
  expected_count+=cost/2;half=cost%2;
  jfg_original_timing_step(metadata,pc,word,&cpu);
  check(fallbacks==0,"ordinary instructions unexpectedly fell back");
  check(fast.cpu_count==expected_count && guest_count==expected_count && fast.diagnostic_cpu_half_tick==half,"Count or half-cycle differs");
  check(fast.guest_previous_delay==(previous_branch && pc==previous_pc+4) && fast.guest_previous_branch==((metadata&(1U<<10))!=0),"branch bookkeeping differs");
  check(fast.guest_last_pc==pc && fast.guest_last_word==word && fast.observed_guest_instructions==i+1 && fast.execution_fast_hits==i+1,"instruction bookkeeping differs");
  check(memcmp(stats.data(),ordinary::cache_stats,sizeof(stats))==0,"cache statistics differ");
  previous_pc=pc;previous_branch=fast.guest_previous_branch;
 }
 check(memcmp(ic.data(),ordinary::ic,sizeof(ic))==0 && memcmp(dc.data(),ordinary::dc,sizeof(dc))==0,"cache lines differ");
 auto boundary=[&](unsigned metadata) {
  const auto before=fast;const auto counters=stats;const auto old=fallbacks;
  jfg_original_timing_step(metadata,0x80010000,0,&cpu);
  check(fallbacks==old+1 && fast.cpu_count==before.cpu_count && fast.observed_guest_instructions==before.observed_guest_instructions && stats==counters,"event boundary changed state before native handler");
 };
 boundary(3U<<12);boundary(1U<<11);boundary(1U<<9);
 fast.guest_eret_boundary=true;boundary(0);fast.guest_eret_boundary=false;
 fast.execution_fast_profile_ready=false;boundary(0);fast.execution_fast_profile_ready=true;
 ++revision;boundary(0);--revision;
 fast.execution_fast_deadline=fast.cpu_count;boundary(0);fast.execution_fast_deadline=UINT64_MAX;
 cpu.status_reg=0;boundary(1U<<8);
 cpu.status_reg=0x401;fast.execution_fast_pending=0x400;boundary(0);
 cpu.status_reg=0x20000000;fast.execution_fast_pending=0;
 // RGO: an overclocked CPU charges Count cost / (2 << shift) with the same
 // cache model; the remainder still carries between instructions.
 for(unsigned shift=1;shift<=3;++shift) {
  fast.cpu_overclock_shift=shift;
  const uint64_t start=fast.cpu_count;uint64_t remainder=fast.diagnostic_cpu_half_tick,charged=0;
  for(unsigned i=0;i<1000;++i) {
   const unsigned pc=0x80001000U+(i%8)*4U;
   const auto cost=ordinary::jfg_full_cache_cost(pc,0x20,&cpu,ram.data())+remainder;
   charged+=cost>>(shift+1);remainder=cost&((2ULL<<shift)-1U);
   jfg_original_timing_step(0,pc,0x20,&cpu);
  }
  check(fast.cpu_count==start+charged && fast.diagnostic_cpu_half_tick==remainder,"overclocked Count differs");
  check(charged*(1ULL<<shift)<=fast.cache_counters[10] && charged>0,"overclock does not reduce Count");
 }
 fast.cpu_overclock_shift=0;
 std::cout<<"250000 instruction/cache transitions and event boundaries matched\n";
}
