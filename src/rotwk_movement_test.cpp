#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include "aotr_accel.cpp"
#include "mapped_retail_test.h"
struct Cell;
struct Info {
    DWORD x,y;Info* parent;DWORD unknown0C;WORD total,cost;
    BYTE opaque14[0x1C];Cell* cell;DWORD next,back;
};
struct Cell {Info* info;DWORD unknown04,unknown08,flags;};
static_assert(sizeof(Info)==0x3C && offsetof(Info,cell)==0x30,"verified RotWK pool shape");
static DWORD rng=0x713569A9;
static DWORD randomWord() {rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static volatile DWORD sink;
template<class Fn> static double timed(unsigned n,Fn fn) {
    LONG64 start=qpcNow();for(unsigned i=0;i<n;++i) sink^=fn(i);
    return (qpcNow()-start)*1e9/g_pqpf.QuadPart/n;
}
typedef DWORD (__fastcall* RwMoveCostFn)(void*,void*,const void*);
static RwMoveCostFn nativeCost=(RwMoveCostFn)0x934602;
static unsigned failAt,writes;
static bool failingWrite(DWORD address,const BYTE* bytes,int size) {
    assert(rwWriteEdit(address,bytes,size));return writes++!=failAt;
}
static BYTE beforeHeap[2][6],afterHeap[2][6];
static BYTE* adjustOracleCode;
static RwMoveHeapFn adjustOracle;
static void selectKernel(bool patched) {
    assert(rwWriteEdit(0x6ECF1D,patched?afterHeap[0]:beforeHeap[0],6));
    assert(rwWriteEdit(0x6ECF65,patched?afterHeap[1]:beforeHeap[1],6));
}
static void __cdecl wrongHeap(DWORD* first,LONG,LONG,DWORD,DWORD) {first[0]=0xBAD;}
static void installChecks() {
    assert(rwRegionsMatch(kRwMoveHeapRegions,_countof(kRwMoveHeapRegions)));
    memcpy(beforeHeap[0],(void*)0x6ECF1D,6);memcpy(beforeHeap[1],(void*)0x6ECF65,6);
    // The original adjust calls the push entry, which will be detoured. Keep
    // a private whole-body oracle and retarget its sole CALL to the untouched
    // push trampoline so differential tests use both fully native operations.
    adjustOracleCode=(BYTE*)VirtualAlloc(NULL,96,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);assert(adjustOracleCode);
    memcpy(adjustOracleCode,(void*)0x6ECF65,96);assert(adjustOracleCode[0x54]==0xE8);
    assert(!rwMoveHeapSelfTest(wrongHeap,wrongHeap));
    RwEdit edits[]={rwBranchEdit(0x6ECF1D,6,0xE9,(void*)hkRwMoveHeapPush),rwBranchEdit(0x6ECF65,6,0xE9,(void*)hkRwMoveHeapAdjust)};
    for(failAt=0;failAt<2;++failAt) {
        writes=0;assert(!rwCommitEdits(edits,2,kRwMoveHeapRegions,_countof(kRwMoveHeapRegions),failingWrite));
        assert(rwRegionsMatch(kRwMoveHeapRegions,_countof(kRwMoveHeapRegions)));
    }
    for(const RwRegion& region:kRwMoveHeapRegions) {
        BYTE* address=(BYTE*)region.address;BYTE saved=*address;*address^=1;
        assert(!rwInstallMoveHeap());*address=saved;
    }
    SetEnvironmentVariableA("AOTR_MOVEHEAP","0");assert(!rwInstallMoveHeap());
    SetEnvironmentVariableA("AOTR_MOVEHEAP",NULL);assert(rwInstallMoveHeap());
    memcpy(afterHeap[0],(void*)0x6ECF1D,6);memcpy(afterHeap[1],(void*)0x6ECF65,6);
    *(LONG*)(adjustOracleCode+0x55)=(LONG)((DWORD)g_rwMoveHeapPushOriginal-(DWORD)(adjustOracleCode+0x59));
    FlushInstructionCache(GetCurrentProcess(),adjustOracleCode,96);adjustOracle=(RwMoveHeapFn)adjustOracleCode;
    printf("PASS: both full-body guards, wrong-result startup rejection, 72 startup workloads, opt-out, six-byte detours and failure-after-write rollback\n");
}
typedef RwMoveHeapFn HeapFn;
static HeapFn nativePush=(HeapFn)0x6ECF1D,nativeAdjust=(HeapFn)0x6ECF65;
static void heapChecks(DWORD seed) {
    const unsigned count=2048;std::vector<Cell> cells(count);std::vector<Info> info(count);
    std::vector<DWORD> a(count+1),b(count+1);
    auto word=[&]() {seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;};
    for(unsigned i=0;i<count;++i) {cells[i].info=&info[i];info[i].cell=&cells[i];}
    for(unsigned trial=0;trial<12000;++trial) {
        unsigned size=1+word()%count,hole=word()%size,top=word()%(hole+1);DWORD value=(DWORD)&cells[word()%count];
        for(unsigned i=0;i<count;++i) {info[i].total=(WORD)(trial%3==0?word()%5:word());a[i]=(DWORD)&cells[word()%count];}
        a[count]=0xDEADBEEF;b=a;
        std::vector<Cell> oldCells=cells;std::vector<Info> oldInfo=info;
        g_rwMoveHeapPushOriginal(a.data(),hole,top,value,0xBAD);nativePush(b.data(),hole,top,value,0xBAD);assert(a==b);
        assert(!memcmp(oldCells.data(),cells.data(),count*sizeof(Cell)) && !memcmp(oldInfo.data(),info.data(),count*sizeof(Info)));
        // Arbitrary/repeated IDs and non-root holes also exercise mutations
        // made between operations; no persistent priority cache is permitted.
        info[word()%count].total=(WORD)word();b=a;oldInfo=info;
        adjustOracle(a.data(),hole,size,value,0xBAD);nativeAdjust(b.data(),hole,size,value,0xBAD);assert(a==b);
        assert(!memcmp(oldCells.data(),cells.data(),count*sizeof(Cell)) && !memcmp(oldInfo.data(),info.data(),count*sizeof(Info)));
        g_rwMoveHeapPushOriginal(a.data(),hole,top,value,0);nativePush(b.data(),hole,top,value,0);assert(a==b);
        assert(!memcmp(oldInfo.data(),info.data(),count*sizeof(Info)));
    }
    DWORD csr=_mm_getcsr();unsigned short before,after;__asm fnstcw before
    DWORD trivialA=0xBAD,trivialB=0xBAD;
    g_rwMoveHeapPushOriginal(&trivialA,0,0,0,0);nativePush(&trivialB,0,0,0,0);assert(trivialA==trivialB);
    adjustOracle(&trivialA,0,1,0,0);nativeAdjust(&trivialB,0,1,0,0);assert(trivialA==trivialB);
    // Observe the native helpers' incidental EAX result through the exact
    // x86 cdecl register contract, including their trivial null-value case.
    typedef DWORD (__cdecl* ReturnFn)(DWORD*,LONG,LONG,DWORD,DWORD);
    assert(((ReturnFn)g_rwMoveHeapPushOriginal)(&trivialA,0,0,0,0)==((ReturnFn)nativePush)(&trivialB,0,0,0,0));
    assert(((ReturnFn)adjustOracle)(&trivialA,0,1,0,0)==((ReturnFn)nativeAdjust)(&trivialB,0,1,0,0));
    __asm fnstcw after
    assert(csr==_mm_getcsr() && before==after);
    printf("PASS: 36000 native/hooked heap operations; 1..2048 entries, duplicates/ties, non-root holes/top, live key mutation, null trivial values, full arrays/cell bytes and control state\n");
}
static DWORD WINAPI foreignHeap(void* context) {heapChecks((DWORD)context);return 0;}
static void threadChecks() {
    g_mkTid=0;HANDLE threads[]={CreateThread(NULL,0,foreignHeap,(void*)0xCAFE,0,NULL),CreateThread(NULL,0,foreignHeap,(void*)0xBABE,0,NULL)};
    assert(threads[0] && threads[1]);assert(WaitForMultipleObjects(2,threads,TRUE,30000)==WAIT_OBJECT_0);
    CloseHandle(threads[0]);CloseHandle(threads[1]);g_mkTid=GetCurrentThreadId();
    printf("PASS: simultaneous independent heaps before render-thread identification; no global scratch/cache or thread gate\n");
}
struct SearchResult {
    std::vector<DWORD> expanded,queriedCosts,route,finalCosts,parents;
    bool found;
    bool operator==(const SearchResult& b) const {
        return found==b.found && expanded==b.expanded && queriedCosts==b.queriedCosts &&
            route==b.route && finalCosts==b.finalCosts && parents==b.parents;
    }
};
static SearchResult search(const std::vector<BYTE>& blocked,unsigned start,unsigned goal,RwMoveCostFn costFunction,HeapFn pushHeap,HeapFn adjustHeap) {
    // Controlled grid driver: actual retail cost + retail heap operations,
    // modeled occupancy/neighbors and integer heuristic. No retail world,
    // formation, goal adjustment, collision, zone refresh or queue is invoked.
    const unsigned width=32,n=width*width;assert(blocked.size()==n);
    std::vector<Cell> cells(n);std::vector<Info> info(n);std::vector<BYTE> closed(n);
    std::vector<Cell*> heap;heap.reserve(n*2);SearchResult result;result.found=false;
    for(unsigned i=0;i<n;++i) {
        cells[i].info=&info[i];cells[i].flags=i%7==0?0x10000:0;
        info[i].x=i%width;info[i].y=i/width;info[i].cost=65535;info[i].cell=&cells[i];
    }
    auto heuristic=[&](unsigned i) {unsigned x=(unsigned)abs((int)(i%width)-(int)(goal%width));
        unsigned y=(unsigned)abs((int)(i/width)-(int)(goal/width));return 10*(x+y)-6*(x<y?x:y);};
    auto push=[&](unsigned i) {heap.push_back(&cells[i]);pushHeap((DWORD*)heap.data(),(LONG)heap.size()-1,0,(DWORD)&cells[i],0);};
    if(!blocked[start]) {info[start].cost=0;info[start].total=(WORD)heuristic(start);push(start);}
    const int directions[8][2]={{1,0},{0,1},{-1,0},{0,-1},{1,1},{-1,1},{-1,-1},{1,-1}};
    while(!heap.empty()) {
        Cell* cell=heap[0];Cell* last=heap.back();heap.pop_back();
        if(!heap.empty()) adjustHeap((DWORD*)heap.data(),0,(LONG)heap.size(),(DWORD)last,0);
        unsigned at=(unsigned)(cell-cells.data());if(closed[at]) continue;
        closed[at]=1;result.expanded.push_back(at);
        if(at==goal) {result.found=true;break;}
        for(const auto& direction:directions) {
            int x=(int)info[at].x+direction[0],y=(int)info[at].y+direction[1];
            if(x<0 || y<0 || x>=(int)width || y>=(int)width) continue;
            unsigned next=y*width+x;if(blocked[next] || closed[next]) continue;
            // Both diagonally adjacent occupancy cells must also be clear.
            if(direction[0] && direction[1] && (blocked[info[at].y*width+x] || blocked[y*width+info[at].x])) continue;
            DWORD cost=costFunction(&cells[next],NULL,cell);result.queriedCosts.push_back(cost);
            if(cost<info[next].cost) {
                info[next].cost=(WORD)cost;info[next].total=(WORD)(cost+heuristic(next));info[next].parent=&info[at];push(next);
            }
        }
    }
    if(result.found) {
        unsigned at=goal;
        for(unsigned length=0;;++length) {
            assert(length<n);result.route.push_back(at);
            if(!info[at].parent) break;at=(unsigned)(info[at].parent-info.data());
        }
    }
    for(const auto& record:info) {
        result.finalCosts.push_back(record.cost);
        result.parents.push_back(record.parent?(DWORD)(record.parent-info.data()):0xFFFFFFFF);
    }
    return result;
}
static void scenarioChecks(bool benchmark) {
    assert(rwBodyHash((BYTE*)0x934602,170)==0x34F5953F && rwBodyHash((BYTE*)0x6E810B,20)==0xF63F9A3B);
    unsigned requests=0,expansions=0,costs=0;
    std::vector<std::vector<BYTE>> maps(5,std::vector<BYTE>(1024));
    for(unsigned y=0;y<32;++y) {maps[1][y*32+16]=y==15?0:1;maps[2][y*32+16]=1;}
    for(unsigned i=0;i<1024;++i) maps[3][i]=randomWord()%5==0;
    maps[4]=maps[2]; // closed gate, then open it during the request sweep
    for(unsigned map=0;map<maps.size();++map) for(unsigned request=0;request<32;++request) {
        if(map==4 && request==16) maps[map][15*32+16]=0;
        unsigned start=(request*17)%512,goal=512+(request*23)%512;
        maps[map][start]=0;maps[map][goal]=0;
        SearchResult stock=search(maps[map],start,goal,nativeCost,g_rwMoveHeapPushOriginal,adjustOracle);
        SearchResult fast=search(maps[map],start,goal,nativeCost,nativePush,nativeAdjust);assert(stock==fast);
        expansions+=(unsigned)stock.expanded.size();costs+=(unsigned)stock.queriedCosts.size();++requests;
    }
    printf("PASS: %u controlled grid requests; %u identical ordered expansions, %u identical queried costs; open field, choke, barrier, clutter and gate changes; routes/parents/final costs match\n",requests,expansions,costs);
    if(benchmark) {
        double a[7],b[7];auto stock=[&](unsigned i){return (DWORD)search(maps[1],(i*17)%512,512+(i*23)%512,nativeCost,nativePush,nativeAdjust).queriedCosts.size();};
        auto fast=[&](unsigned i){return (DWORD)search(maps[1],(i*17)%512,512+(i*23)%512,nativeCost,nativePush,nativeAdjust).queriedCosts.size();};
        for(unsigned sample=0;sample<7;++sample) {
            if(sample&1) {selectKernel(true);b[sample]=timed(128,fast);selectKernel(false);a[sample]=timed(128,stock);}
            else {selectKernel(false);a[sample]=timed(128,stock);selectKernel(true);b[sample]=timed(128,fast);}
        }
        selectKernel(true);
        std::sort(a,a+7);std::sort(b,b+7);
        printf("BENCH controlled choke search: native %.2f us, hooked %.2f us, %.3fx (includes driver allocation/tracing; not retail whole search)\n",a[3]/1000,b[3]/1000,a[3]/b[3]);
    }
}
static void bench() {
    DWORD_PTR oldMask,systemMask;assert(GetProcessAffinityMask(GetCurrentProcess(),&oldMask,&systemMask));
    unsigned cpu=GetCurrentProcessorNumber();assert(SetProcessAffinityMask(GetCurrentProcess(),(DWORD_PTR)1<<cpu));
    printf("BENCH CONFIG: process pinned to logical CPU %u; seven alternating medians; original entry restored outside native timed samples (no trampoline overhead)\n",cpu);
    for(unsigned count:{32u,256u,2048u}) {
        std::vector<Cell> cells(count);std::vector<Info> info(count);std::vector<DWORD> original(count),a(count),b(count);
        for(unsigned i=0;i<count;++i) {cells[i].info=&info[i];info[i].cell=&cells[i];info[i].total=(WORD)randomWord();original[i]=(DWORD)&cells[i];}
        auto run=[&](std::vector<DWORD>& heap,HeapFn pushHeap,HeapFn adjustHeap) {
            heap=original;
            for(unsigned i=0;i<count;++i) pushHeap(heap.data(),i,0,original[i],0);
            for(unsigned size=count-1;size;--size) adjustHeap(heap.data(),0,size,heap[size],0);
            return heap[0];
        };
        auto stock=[&](unsigned){return run(a,nativePush,nativeAdjust);};
        auto fast=[&](unsigned){return run(b,nativePush,nativeAdjust);};
        for(unsigned i=0;i<8;++i) {selectKernel(false);DWORD answer=stock(i);selectKernel(true);assert(answer==fast(i));assert(a==b);}
        double aa[7],bb[7];
        for(unsigned sample=0;sample<7;++sample) {
            if(sample&1) {selectKernel(true);bb[sample]=timed(1000,fast);selectKernel(false);aa[sample]=timed(1000,stock);}
            else {selectKernel(false);aa[sample]=timed(1000,stock);selectKernel(true);bb[sample]=timed(1000,fast);}
        }
        selectKernel(true);
        std::sort(aa,aa+7);std::sort(bb,bb+7);
        printf("BENCH heap %u push+drain: native %.2f us, hooked %.2f us, %.3fx\n",count,aa[3]/1000,bb[3]/1000,aa[3]/bb[3]);
    }
    scenarioChecks(true);assert(SetProcessAffinityMask(GetCurrentProcess(),oldMask));
}
int main(int argc,char** argv) {
    setvbuf(stdout,NULL,_IONBF,0);assert(argc==2 || argc==3);
    if(strcmp(argv[1],"child")) return runMappedRetailChild(argv[1],argc==3);
    InitializeCriticalSection(&g_logCs);QueryPerformanceFrequency(&g_pqpf);g_mkTid=GetCurrentThreadId();
    installChecks();heapChecks(0x713569A9);threadChecks();scenarioChecks(false);
    if(argc==3) bench();
    VirtualFree(adjustOracleCode,0,MEM_RELEASE);DeleteCriticalSection(&g_logCs);printf("ALL MOVEMENT CHECKS PASSED\n");return 0;
}
