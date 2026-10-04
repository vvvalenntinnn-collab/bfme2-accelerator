#pragma once
#include "aotr_cpu_topology.h"
#include <xmmintrin.h>
// Joined, single-producer preparation pool. Jobs may touch only borrowed
// immutable inputs and disjoint output ranges. SSE jobs only: callbacks must
// not use x87, engine callbacks or D3D. MXCSR status is joined with the caller.
typedef void (*AccelPrepareFn)(void*,unsigned,unsigned);
struct AccelPreparePool {
    enum { MAX_WORKERS=2 };
    HANDLE threads[MAX_WORKERS],wake[MAX_WORKERS],done[MAX_WORKERS];
    struct Slot { AccelPreparePool* pool; unsigned index; } slots[MAX_WORKERS];
    unsigned workers,count,grain,csr;
    volatile LONG next,stop;
    volatile LONG busy;
    DWORD status[MAX_WORKERS];
    AccelPrepareFn function; void* context;
    void consume() {
        for (;;) {
            unsigned begin=(unsigned)InterlockedExchangeAdd(&next,(LONG)grain);
            if (begin>=count) return;
            unsigned end=begin+grain; if(end>count) end=count;
            function(context,begin,end);
        }
    }
    static DWORD WINAPI entry(void* ptr) {
        Slot& slot=*(Slot*)ptr; AccelPreparePool& p=*slot.pool;
        for (;;) {
            WaitForSingleObject(p.wake[slot.index],INFINITE);
            if (p.stop) return 0;
            DWORD saved=_mm_getcsr(); _mm_setcsr(p.csr);
            p.consume(); p.status[slot.index]=_mm_getcsr()&63;
            _mm_setcsr(saved); SetEvent(p.done[slot.index]);
        }
    }
    // Call only at a quiescent boundary, outside DllMain/loader lock.
    void shutdown() {
        InterlockedExchange(&stop,1);
        for(unsigned i=0;i<workers;++i) SetEvent(wake[i]);
        for(unsigned i=0;i<workers;++i) {
            WaitForSingleObject(threads[i],INFINITE);
            CloseHandle(threads[i]); CloseHandle(wake[i]); CloseHandle(done[i]);
        }
        workers=0; stop=0;
    }
    bool start(const AccelCpuPlan& plan) {
        if(workers) return true;
        unsigned desired=plan.count>2?plan.count-2:0;
        if(desired>MAX_WORKERS) desired=MAX_WORKERS;
        for(unsigned i=0;i<desired;++i) {
            wake[i]=CreateEventA(NULL,FALSE,FALSE,NULL); done[i]=CreateEventA(NULL,FALSE,FALSE,NULL);
            slots[i].pool=this; slots[i].index=i;
            threads[i]=wake[i] && done[i]?CreateThread(NULL,0,entry,&slots[i],0,NULL):NULL;
            if(!threads[i]) {
                if(wake[i]) CloseHandle(wake[i]); if(done[i]) CloseHandle(done[i]);
                shutdown(); return false;
            }
            ++workers; SetThreadIdealProcessor(threads[i],plan.cpu[i+2]);
            // New compute threads stay on their spare physical core. An ideal
            // processor alone permits migration onto the main/render cores.
            if(!SetThreadAffinityMask(threads[i],plan.mask[i+2])) {shutdown();return false;}
        }
        return workers!=0;
    }
    bool run(AccelPrepareFn fn,void* ctx,unsigned n,unsigned chunk) {
        if(!workers || !chunk || n<chunk*2 || InterlockedCompareExchange(&busy,1,0)) return false;
        function=fn; context=ctx; count=n; grain=chunk; next=0; csr=_mm_getcsr();
        // Unmasked FP exceptions must execute on the original thread.
        if((csr&0x1F80)!=0x1F80) { busy=0; return false; }
        for(unsigned i=0;i<workers;++i) { status[i]=0; SetEvent(wake[i]); }
        consume();
        WaitForMultipleObjects(workers,done,TRUE,INFINITE);
        DWORD finalCsr=_mm_getcsr();
        for(unsigned i=0;i<workers;++i) finalCsr|=status[i];
        _mm_setcsr(finalCsr); function=NULL; context=NULL; InterlockedExchange(&busy,0);
        return true;
    }
};
