// SPDX-License-Identifier: GPL-3.0-or-later
#include "stream_arena_trace.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#ifndef P2_ARENA_TRACE_TEST
#include <rex/cvar.h>
#include <rex/logging.h>
REXCVAR_DEFINE_BOOL(p2_stream_arena_trace, false, "Prototype2",
    "Record bounded dual-stream allocator history in RAM, write only on failure; restart required")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
#endif
extern "C" const char* RexSwitchLogDir();
namespace {
constexpr uint32_t kEvents = 8192;
static_assert(std::atomic<uint64_t>::is_always_lock_free);
struct Slot { std::atomic<uint64_t> word[8]{}; };
// Each scalar carries its event number in the upper32 bits. A concurrent overwrite
// can only cause a skipped event, never a mixed/torn record accepted as valid.
std::array<Slot,kEvents> arena_events;
std::atomic<uint32_t> arena_head{0};
std::atomic<bool> arena_wrapped_counter{false};
std::atomic<bool> arena_dumped{false};
struct ArenaEvent { uint32_t sequence, word[8]; };
struct ArenaHeader {
    uint32_t magic=0x52413250, version=1, header_bytes=80, event_bytes=36;
    uint32_t object=0, kind=0, head=0, matched=0, skipped=0, first=0;
    uint32_t state[10]{};
};
static_assert(sizeof(ArenaEvent)==36 && sizeof(ArenaHeader)==80);
bool ReadArenaEvent(uint32_t sequence, ArenaEvent& out) {
    out.sequence=sequence;
    for(unsigned i=0;i<8;++i){
        const uint64_t v=arena_events[sequence&(kEvents-1)].word[i].load(std::memory_order_acquire);
        if(uint32_t(v>>32)!=sequence)return false;
        out.word[i]=uint32_t(v);
    }
    return true;
}
void SaveArenaFailure(uint32_t object,uint32_t kind,const uint32_t* state) {
    if(!state || arena_wrapped_counter.load(std::memory_order_relaxed) ||
       arena_dumped.exchange(true,std::memory_order_acq_rel))return;
    ArenaHeader h;h.object=object;h.kind=kind;h.head=arena_head.load(std::memory_order_acquire);
    h.first=h.head>=kEvents?h.head-kEvents+1:1;
    for(unsigned i=0;i<10;++i)h.state[i]=state[i];
    char path[1024];std::snprintf(path,sizeof(path),"%sp2_arena_failure.bin",RexSwitchLogDir());
    FILE* file=std::fopen(path,"wb");
    bool ok=file!=nullptr;
    if(file){
        ok=std::fwrite(&h,sizeof(h),1,file)==1;
        for(uint64_t index=h.first;index<=h.head;++index){
            const uint32_t n=uint32_t(index);
            ArenaEvent event{};
            if(!ReadArenaEvent(n,event)){++h.skipped;continue;}
            if(event.word[1]!=object)continue;
            ++h.matched;
            ok=(std::fwrite(&event,sizeof(event),1,file)==1)&&ok;
        }
        ok=(std::fseek(file,0,SEEK_SET)==0)&&ok;
        ok=(std::fwrite(&h,sizeof(h),1,file)==1)&&ok;
        ok=(std::fclose(file)==0)&&ok;
    }
#ifndef P2_ARENA_TRACE_TEST
    REXLOG_ERROR("[p2-arena-trace] allocation failed object={:08X} region={} cursor={}/{} active={} history={} skipped={} saved={} path={}",
        object,kind==3?"GPU":"normal",state[kind==3?6:2],state[kind==3?5:1],
        state[kind==3?7:3],h.matched,h.skipped,ok,path);
#endif
}
}
extern "C" bool P2ArenaTraceEnabled(){
#ifdef P2_ARENA_TRACE_TEST
    return arena_test_enabled;
#else
    return REXCVAR_GET(p2_stream_arena_trace);
#endif
}
extern "C" void P2ArenaTraceRecord(uint32_t kind,uint32_t object,
    uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f,
    const uint32_t* failure_state){
    if(!P2ArenaTraceEnabled() || arena_wrapped_counter.load(std::memory_order_relaxed))return;
    const uint32_t n=arena_head.fetch_add(1,std::memory_order_relaxed)+1;
    if(!n){arena_wrapped_counter.store(true,std::memory_order_relaxed);return;}
    const uint32_t values[8]={kind,object,a,b,c,d,e,f};
    for(unsigned i=0;i<8;++i)
        arena_events[n&(kEvents-1)].word[i].store((uint64_t(n)<<32)|values[i],std::memory_order_release);
    // Only a real failed nonzero allocation triggers SD I/O. No periodic file writes.
    if((kind==2||kind==3)&&!a&&b)SaveArenaFailure(object,kind,failure_state);
}
