/*
ares

Copyright (c) 2004-2021 ares team, Near et al

Permission to use, copy, modify, and/or distribute this software for any

purpose with or without fee is hereby granted, provided that the above

copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES

WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF

MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR

ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES

WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN

ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF

OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

Instruction/cache timing model based on ares VR4300 costs, as vendored by
BizHawk bdddf4a58aa1a022afb11dc73294a81a5aa7bbd5.
*/

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include "recomp.h"
#if defined(_MSC_VER)
#define JFG_CLOCK_INLINE __forceinline
#else
#define JFG_CLOCK_INLINE inline __attribute__((always_inline))
#endif
typedef struct { unsigned tag,valid,dirty; } JfgTimingCacheLine;
// Shared host state only. The native event handler and generated instruction
// fast path access the same storage; no guest clocks or observations are cached.
typedef struct JfgOriginalTimingFast {
 uint8_t* rdram;
 uint64_t cpu_count,observed_guest_instructions;
 uint64_t execution_fast_revision,execution_fast_deadline,execution_fast_hits;
 uint64_t diagnostic_cpu_half_tick;
 uint32_t execution_fast_pending,guest_last_pc,guest_last_word;
 bool execution_fast_profile_ready,guest_previous_branch,guest_previous_delay;
 bool guest_eret_boundary,play_mode;
 const volatile uint64_t* io_revision;
 volatile uint64_t* io_guest_count;
 JfgTimingCacheLine *instruction_cache,*data_cache;
 uint64_t* cache_counters;
 // RGO fast clock (docs/rgo/framerate.md). 0: the VR4300 cost and cache
 // model above (Original N64 slowdown). 1: every instruction costs a flat
 // 1/16 of a Count tick and the cache model is skipped, so a game frame's
 // CPU work takes a small fraction of a VI; idle and polling loops are
 // skipped to the next device event by the native runtime.
 uint32_t fast_clock;
 // RGO fast clock skip run: instructions granted to jfg_fast_budget that
 // have not been accounted to Count yet (0: no run).
 uint32_t fast_run_length;
} JfgOriginalTimingFast;
#ifdef __cplusplus
extern "C" {
#endif
extern JfgOriginalTimingFast* jfg_original_timing_state;
// RGO fast clock: ordinary instructions the generated hooks may still skip
// before the next native check (see jfg_fast_run_sync). Always 0 under the
// original cost model, so its hooks run exactly as before.
extern uint32_t jfg_fast_budget;
// Nonzero when the run's thread had the FPU usable (Status CU1). Otherwise
// an FPU instruction ends the run, so the native check still raises the
// coprocessor-unusable exception libultra uses to save FPU state lazily.
extern uint32_t jfg_fast_fpu;
void jfg_phase9_execution_probe_predecoded(unsigned,unsigned,unsigned,void*);
#ifdef __cplusplus
}
#endif
static JFG_CLOCK_INLINE unsigned jfg_clock_address(JfgOriginalTimingFast* s,unsigned a) {
 unsigned section,slot,table,base;
 if(a>=0x10000000U || a<0x100000U)return a;
 section=a>>20;slot=(section<158U && section!=21U && section!=56U)?section:0U;
 if(!slot||!s->rdram){++s->cache_counters[11];return a;}
 memcpy(&table,s->rdram+0xfeaa0U,4);
 if(table<0x80000000U||table>=0x80400000U-157U*32U){++s->cache_counters[11];return a;}
 memcpy(&base,s->rdram+(table&0x1fffffffU)+slot*32U,4);
 if(base<0x80000000U||base>=0x80400000U){++s->cache_counters[11];return a;}
 return base+(a&0xfffffU);
}
static JFG_CLOCK_INLINE unsigned jfg_clock_fetch(JfgOriginalTimingFast* s,unsigned pc) {
 unsigned a=jfg_clock_address(s,pc)&0x1fffffffU,tag=a&~0xfffU;
 JfgTimingCacheLine* line=&s->instruction_cache[(a>>5)&511U];
 ++s->cache_counters[0];
 if(line->valid && line->tag==tag){++s->cache_counters[1];return 1;}
 ++s->cache_counters[2];line->valid=1;line->tag=tag;return 48;
}
static JFG_CLOCK_INLINE unsigned jfg_clock_data(JfgOriginalTimingFast* s,unsigned address,unsigned write) {
 unsigned a,tag,cost=1;JfgTimingCacheLine* line;
 address=jfg_clock_address(s,address);a=address&0x1fffffffU;tag=a&~0xfffU;
 ++s->cache_counters[write?4:3];
 if((address&0xe0000000U)!=0x80000000U){++s->cache_counters[8];return 1;}
 line=&s->data_cache[(a>>4)&511U];
 if(line->valid && line->tag==tag)++s->cache_counters[5];
 else {
  ++s->cache_counters[6];cost=40;
  if(line->valid && line->dirty){++s->cache_counters[7];cost+=40;}
  line->valid=1;line->dirty=0;line->tag=tag;
 }
 if(write)line->dirty=1;
 return cost;
}
// Count ticks per instruction under the RGO fast clock, as a shift: 1/16.
#define JFG_FAST_CLOCK_SHIFT 4U
// Charge one instruction to Count: its VR4300 cycles at half the CPU clock,
// or the fast clock's flat fraction. The remainder carries to the next one.
static JFG_CLOCK_INLINE void jfg_clock_charge(JfgOriginalTimingFast* s,uint64_t cycles) {
 if(s->fast_clock){
  const uint64_t ticks=s->diagnostic_cpu_half_tick+1U;
  s->cpu_count+=ticks>>JFG_FAST_CLOCK_SHIFT;
  s->diagnostic_cpu_half_tick=ticks&(uint64_t)((1U<<JFG_FAST_CLOCK_SHIFT)-1U);
  return;
 }
 cycles+=s->diagnostic_cpu_half_tick;
 s->cpu_count+=cycles>>1U;s->diagnostic_cpu_half_tick=cycles&1U;
}
// RGO fast clock skip runs. Under the fast clock an ordinary instruction
// only adds 1/16 of a Count tick, so the native check grants a run of
// instructions that cannot reach the next device deadline and the generated
// hook just counts them down (the macro at the end of this file). A run
// ends at its last instruction, or early when anything the native code does
// may change device state: an MMIO access, a COP0 operation, a skip of Count
// in the dispatcher. Sync accounts the instructions run so far to Count.
static JFG_CLOCK_INLINE void jfg_fast_run_sync(JfgOriginalTimingFast* s) {
 uint32_t consumed;uint64_t ticks;
 if(!s->fast_run_length)return;
 consumed=s->fast_run_length-jfg_fast_budget;
 s->fast_run_length=jfg_fast_budget;
 s->observed_guest_instructions+=consumed;s->execution_fast_hits+=consumed;
 ticks=s->diagnostic_cpu_half_tick+consumed;
 s->cpu_count+=ticks>>JFG_FAST_CLOCK_SHIFT;
 s->diagnostic_cpu_half_tick=ticks&(uint64_t)((1U<<JFG_FAST_CLOCK_SHIFT)-1U);
 *s->io_guest_count=s->cpu_count;
}
// Ends a run. Skipped instructions left no branch bookkeeping, so after any
// the next instruction is never taken as a device-event boundary. A run
// that skipped nothing keeps it: the idle-loop skip needs its delay slot.
static JFG_CLOCK_INLINE void jfg_fast_run_end(JfgOriginalTimingFast* s) {
 bool skipped;
 if(!s->fast_run_length)return;
 skipped=s->fast_run_length!=jfg_fast_budget;
 jfg_fast_run_sync(s);
 jfg_fast_budget=0U;s->fast_run_length=0U;
 if(skipped){s->guest_previous_branch=false;s->guest_previous_delay=false;}
}
static JFG_CLOCK_INLINE void jfg_original_timing_step(unsigned metadata,unsigned pc,unsigned word,void* context) {
 JfgOriginalTimingFast* s=jfg_original_timing_state;
 unsigned kind=(metadata>>12U)&3U;
 // The fast clock's check, run accounting and grants live in the native
 // handler; it is reached once per skip run, not per instruction.
 if(s && s->fast_clock){jfg_phase9_execution_probe_predecoded(metadata,pc,word,context);return;}
 // CACHE instructions and every event/observer boundary retain the fully
// checked native handler. Constant instruction operands fold at generation sites.
 if(s && context && kind!=3U && s->execution_fast_profile_ready &&
    !s->guest_eret_boundary && (metadata&0x800U)==0U &&
    (s->play_mode || s->observed_guest_instructions<24000000000ULL) &&
    s->execution_fast_deadline>s->cpu_count && s->execution_fast_revision==*s->io_revision) {
  recomp_context* cpu=(recomp_context*)context;
  bool fpu=(metadata&0x100U)!=0U;
  bool eligible=(cpu->status_reg&7U)==1U && (s->execution_fast_pending&cpu->status_reg&0xff00U)!=0U;
  if(!eligible && !(fpu && (cpu->status_reg&0x20000000U)==0U) && (metadata&0x200U)==0U) {
   bool delay=s->guest_previous_branch && pc==s->guest_last_pc+4U;
   uint64_t cost=jfg_clock_fetch(s,pc)+((metadata>>16U)&127U);
   if(kind==1U || kind==2U) {
    unsigned reg=(word>>21U)&31U;uint64_t value=0;
    if(reg)memcpy(&value,(const unsigned char*)cpu+offsetof(recomp_context,r1)+(reg-1)*sizeof(uint64_t),sizeof(value));
    cost+=jfg_clock_data(s,(uint32_t)value+(uint32_t)(int32_t)(int16_t)word,kind==2U);
   }
   s->cache_counters[10]+=cost;
   ++s->observed_guest_instructions;++s->execution_fast_hits;
   jfg_clock_charge(s,cost);
   *s->io_guest_count=s->cpu_count;
   s->guest_last_pc=pc;s->guest_last_word=word;
   s->guest_previous_delay=delay;s->guest_previous_branch=(metadata&0x400U)!=0U;
   return;
  }
 }
 jfg_phase9_execution_probe_predecoded(metadata,pc,word,context);
}
#ifndef __cplusplus
// Generated C retains its existing hook ABI and source/object inventory.
// RGO: an ordinary instruction inside a fast clock skip run only counts the
// run down. Idle-loop and observer boundaries (0x800), thread parks (0x200)
// and CACHE (kind 3) always reach the native handler, and FPU instructions
// (0x100) only skip with the FPU usable; the metadata is a constant at every
// site, so these tests fold away.
#define jfg_phase9_execution_probe_predecoded(m,p,w,c) do { \
  if (((m)&0xA00U)==0U && ((m)&0x3000U)!=0x3000U && jfg_fast_budget!=0U && \
      (((m)&0x100U)==0U || jfg_fast_fpu!=0U)) --jfg_fast_budget; \
  else if ((m)&0x800000U) jfg_original_timing_step(m,p,w,c); \
  else (jfg_phase9_execution_probe_predecoded)(m,p,w,c); } while (0)
#endif
#undef JFG_CLOCK_INLINE
