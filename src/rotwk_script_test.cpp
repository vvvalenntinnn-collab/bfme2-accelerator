// Differential tests against the user's private mapped executable. This
// fixture does not start the game or distribute any game bytes.
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

struct Name {
    std::vector<BYTE> storage;
    DWORD value;
    Name(const std::vector<BYTE>& bytes,bool nullEmpty=false):storage(bytes.size()+9),value(0) {
        assert(bytes.size()<=65535);
        *(DWORD*)storage.data()=17; *(WORD*)(storage.data()+4)=(WORD)bytes.size();
        if(!bytes.empty()) memcpy(storage.data()+8,bytes.data(),bytes.size());
        if(!bytes.empty() || !nullEmpty) value=(DWORD)storage.data();
    }
    Name(const Name&)=delete; Name& operator=(const Name&)=delete;
};
static int nameCompare(DWORD a,DWORD b) {
    unsigned al=a?*(WORD*)(a+4):0,bl=b?*(WORD*)(b+4):0,n=al<bl?al:bl;
    int cmp=n?memcmp((void*)(a+8),(void*)(b+8),n):0;
    return cmp?cmp:(int)al-(int)bl;
}
struct Record { DWORD prev,next,name; BYTE released,pad; WORD refs; DWORD nodes; };
static_assert(sizeof(Record)==20,"retail record stride");
struct Table {
    BYTE list[0x4C]; std::vector<Record> records; std::vector<DWORD> order;
    Table() { memset(list,0,sizeof(list)); }
    void sync() {
        std::stable_sort(order.begin(),order.end(),[&](DWORD a,DWORD b){return nameCompare(records[a].name,records[b].name)<0;});
        DWORD a=(DWORD)order.data(),b=(DWORD)records.data();
        DWORD fields[]={a,a+(DWORD)order.size()*4,a+(DWORD)order.capacity()*4,
            b,b+(DWORD)records.size()*20,b+(DWORD)records.capacity()*20,0xFFFFFFFF,0xFFFFFFFF};
        memcpy(list+0x2C,fields,sizeof(fields));
    }
    void add(DWORD name,DWORD nodes) {
        // Leave a free slot between every two live records; sorted IDs are
        // deliberately unrelated to name order and record-array positions.
        Record free={0xFFFFFFFF,0xFFFFFFFF,0,1,0,0,0}; records.push_back(free);
        Record r={0xFFFFFFFF,0xFFFFFFFF,name,0,0,19,nodes};
        order.push_back((DWORD)records.size()); records.push_back(r);
    }
};
static DWORD rng=0x713569A9;
static DWORD randomWord() { rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }
static unsigned comparisons=0;
static void verify(Table& t,const DWORD* query,RwScriptFn native) {
    BYTE list[sizeof(t.list)]; memcpy(list,t.list,sizeof(list));
    std::vector<Record> records=t.records; std::vector<DWORD> order=t.order;
    void* candidate=(void*)0xBAD; assert(rwScriptCandidate(t.list,query,&candidate));
    void* stock=native(t.list,NULL,query); assert(stock==candidate);
    assert(!memcmp(list,t.list,sizeof(list)) && records.size()==t.records.size());
    if(!records.empty()) assert(!memcmp(records.data(),t.records.data(),records.size()*sizeof(Record)));
    assert(order==t.order); ++comparisons;
}
static void layoutTests(RwScriptFn native) {
    const std::vector<BYTE> keys[]={ {}, {'a'}, {'a',0}, {'a',0,'b'}, {'a',0,'c'},
        {'a','a'}, {'a','b'}, {0x7F}, {0x80}, {0xFF}, std::vector<BYTE>(65535,'z') };
    std::vector<Name*> names;
    for(const auto& key:keys) names.push_back(new Name(key));
    names.push_back(new Name({},true));
    for(unsigned n:{0u,1u,2u,3u,31u,32u,256u,2048u}) {
        Table t;
        for(unsigned i=0;i<n;++i) t.add(names[randomWord()%names.size()]->value,0x20000000+i*16);
        t.sync();
        for(unsigned i=0;i<128;++i) verify(t,&names[randomWord()%names.size()]->value,native);
        if(n) {
            DWORD id=t.order[n/2]; t.records[id].nodes=0;verify(t,&t.records[id].name,native);
            t.records[id].nodes=0xFFFFFFFC;verify(t,&t.records[id].name,native);
        }
    }
    Table edge;edge.add(names[1]->value,0);edge.sync();
    verify(edge,&names[1]->value,native);
    assert(native(edge.list,NULL,&names[1]->value)==(void*)4);
    edge.records[1].nodes=0xFFFFFFFC;verify(edge,&names[1]->value,native);
    assert(native(edge.list,NULL,&names[1]->value)==NULL);
    Table t; for(unsigned i=0;i<32;++i) t.add(names[i%names.size()]->value,0x21000000+i*16); t.sync();
    for(unsigned i=0;i<2000;++i) {
        unsigned id=1+2*(randomWord()%32); t.records[id].name=names[randomWord()%names.size()]->value;
        t.records[id].nodes=randomWord();
        if(i%3==0) { auto it=std::find(t.order.begin(),t.order.end(),id); if(it!=t.order.end()) t.order.erase(it); }
        else if(std::find(t.order.begin(),t.order.end(),id)==t.order.end()) t.order.push_back(id);
        std::reverse(t.order.begin(),t.order.end());t.sync();
        verify(t,&names[randomWord()%names.size()]->value,native);
    }
    // Owned string bytes/refcounts are unchanged by either implementation.
    for(unsigned i=0;i<_countof(keys);++i) {
        assert(*(DWORD*)names[i]->storage.data()==17);
        if(!keys[i].empty()) assert(!memcmp(names[i]->storage.data()+8,keys[i].data(),keys[i].size()));
    }
    // A length-limited comparison must not read through an inaccessible page.
    BYTE* pages=(BYTE*)VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(pages);
    DWORD old;assert(VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&old));
    DWORD pageName=(DWORD)(pages+4096-8-3);memset((void*)pageName,0,8);
    *(WORD*)(pageName+4)=3;memcpy((void*)(pageName+8),"abc",3);
    Table boundary;boundary.add(pageName,0x22000000);boundary.sync();
    Name same({'a','b','c'}),longer({'a','b','c','d'});
    verify(boundary,&same.value,native);verify(boundary,&longer.value,native);
    VirtualFree(pages,0,MEM_RELEASE);
    // Candidate declines uncertain layouts. Native ignores record-end, so
    // this particular metadata mismatch is safe to exercise through fallback.
    DWORD saved=*(DWORD*)(t.list+0x3C);*(DWORD*)(t.list+0x3C)=saved+1;
    void* result; assert(!rwScriptCandidate(t.list,&same.value,&result));
    *(DWORD*)(t.list+0x3C)=saved;
    assert(!rwScriptCandidate(NULL,&same.value,&result));
    assert(!rwScriptCandidate(t.list,NULL,&result));
    // Neither path changes rounding/FTZ/DAZ or the x87 control word.
    for(unsigned i=0;i<16;++i) {
        DWORD csr=0x1F80|((i&3)<<13)|((i&4)?0x8000:0)|((i&8)?0x40:0);
        _mm_setcsr(csr); unsigned short cwBefore,cwAfter;
        __asm fnstcw cwBefore
        verify(t,&same.value,native);
        __asm fnstcw cwAfter
        assert(cwBefore==cwAfter && _mm_getcsr()==csr);
    }
    _mm_setcsr(0x1F80);
    for(Name* name:names) delete name;
    for(unsigned n:{32u,256u,2048u}) {
        Table unique;std::vector<Name*> names,misses;
        for(unsigned i=0;i<n;++i) {
            char suffix[32];sprintf(suffix,"_%08u",(i*57)&(n-1));
            std::vector<BYTE> bytes(128,'x');bytes.insert(bytes.end(),suffix,suffix+strlen(suffix));
            names.push_back(new Name(bytes));unique.add(names.back()->value,0x22000000+i*16);
            bytes.push_back('#');misses.push_back(new Name(bytes));
        }
        unique.sync();
        for(unsigned i=0;i<n;++i) {verify(unique,&names[i]->value,native);verify(unique,&misses[i]->value,native);}
        for(Name* name:names) delete name;for(Name* name:misses) delete name;
    }
    printf("PASS: %u native/candidate comparisons; sparse IDs, duplicates, live mutations, empty/embedded-NUL/high-byte/max-length names and bounds\n",comparisons);
}

static bool failingWrite(DWORD address,const BYTE* bytes,int size) {
    assert(rwWriteEdit(address,bytes,size)); return false;
}
static void installationTests() {
    assert(rwRegionsMatch(kRwScriptRegions,_countof(kRwScriptRegions)));
    RwEdit edit=rwBranchEdit(0x7B72EC,5,0xE9,(void*)hkRwScriptLookup);
    assert(!rwCommitEdits(&edit,1,kRwScriptRegions,_countof(kRwScriptRegions),failingWrite));
    assert(rwRegionsMatch(kRwScriptRegions,_countof(kRwScriptRegions)));
    for(const RwRegion& region:kRwScriptRegions) {
        BYTE* p=(BYTE*)region.address; BYTE saved=*p;*p^=1;
        assert(!rwInstallScriptLookup());*p=saved;
        assert(rwRegionsMatch(kRwScriptRegions,_countof(kRwScriptRegions)));
    }
    SetEnvironmentVariableA("AOTR_SCRIPTLOOKUP","0");assert(!rwInstallScriptLookup());
    assert(rwRegionsMatch(kRwScriptRegions,_countof(kRwScriptRegions)));
    SetEnvironmentVariableA("AOTR_SCRIPTLOOKUP",NULL);assert(rwInstallScriptLookup());
    printf("PASS: seven dependency guards, failure-after-write rollback, opt-out and five-byte detour installation\n");
}
static unsigned nativeCalls=0;
static RwScriptFn realOriginal;
static void* __fastcall countingOriginal(void* self,void* edx,const DWORD* name) {
    ++nativeCalls;return realOriginal(self,edx,name);
}
static void* __fastcall mismatchingOriginal(void*,void*,const DWORD*) { ++nativeCalls;return (void*)0xBAD; }
static DWORD WINAPI foreignLookup(void* context) {
    Table* t=(Table*)context;DWORD query=t->records[1].name;
    assert(((RwScriptFn)0x7B72EC)(t->list,NULL,&query)==(void*)(t->records[1].nodes+4)); return 0;
}
static void hookTests() {
    Name name({'a'}),missing({'z'});Table t;t.add(name.value,0x20000000);t.sync();
    realOriginal=g_rwScriptOriginal;g_rwScriptOriginal=countingOriginal;
    RwScriptFn hooked=(RwScriptFn)0x7B72EC;g_rwScriptProof=512;g_rwScriptTick=0;
    for(unsigned i=0;i<512+4096;++i) {
        DWORD query=i&1?name.value:missing.value;
        assert(hooked(t.list,NULL,&query)==realOriginal(t.list,NULL,&query));
    }
    assert(!g_rwScriptProof && !g_rwScriptKill && nativeCalls==512+64);
    DWORD tick=g_rwScriptTick;unsigned calls=nativeCalls;
    g_mkTid=0;assert(hooked(t.list,NULL,&name.value)==(void*)0x20000004);g_mkTid=GetCurrentThreadId();
    HANDLE thread=CreateThread(NULL,0,foreignLookup,&t,0,NULL);assert(thread);
    assert(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CloseHandle(thread);
    assert(nativeCalls==calls+2 && g_rwScriptTick==tick);
    DWORD saved=*(DWORD*)(t.list+0x3C);*(DWORD*)(t.list+0x3C)=saved+1;
    assert(hooked(t.list,NULL,&name.value)==(void*)0x20000004);*(DWORD*)(t.list+0x3C)=saved;
    assert(nativeCalls==calls+3);
    g_rwScriptOriginal=mismatchingOriginal;g_rwScriptProof=1;
    assert(hooked(t.list,NULL,&name.value)==(void*)0xBAD && g_rwScriptKill);
    calls=nativeCalls;assert(hooked(t.list,NULL,&name.value)==(void*)0xBAD && nativeCalls==calls+1);
    g_rwScriptOriginal=realOriginal;g_rwScriptProof=0;g_rwScriptKill=false;
    printf("PASS: 512-call proof, 1/64 native sampling, startup/foreign-thread/layout fallback and mismatch circuit breaker\n");
}

// Execute the unchanged native outer wrapper with controlled side selection
// and string-ownership dependencies. This checks its real stack/return ABI,
// output writes and destructor order; it does not benchmark side resolution.
static Table* wrapperTable;
static DWORD prefixName,bareName;
static std::vector<unsigned> events;
static DWORD* __fastcall resolvePrefix(void*,void*,DWORD* out,DWORD* input) {
    events.push_back(1); *out=prefixName; *input=bareName; return out;
}
static void* __fastcall selectSide(void*,void*,const DWORD* prefix) {
    assert(*prefix==prefixName);events.push_back(2);return wrapperTable?wrapperTable->list:NULL;
}
static DWORD* __fastcall assignName(DWORD* self,void*,const DWORD* source) {
    assert(*source==prefixName);events.push_back(3);*self=*source;return self;
}
static void __fastcall destroyName(DWORD* self,void*) {
    events.push_back(*self==prefixName?4:5);*self=0;
}
static void wrapperTests() {
    RwEdit edits[]={ rwBranchEdit(0x604043,5,0xE9,(void*)resolvePrefix),
        rwBranchEdit(0x604986,5,0xE9,(void*)selectSide),
        rwBranchEdit(0x436030,5,0xE9,(void*)assignName),
        rwBranchEdit(0x435D50,5,0xE9,(void*)destroyName) };
    for(auto& edit:edits) { memcpy(edit.before,(void*)edit.address,5); assert(rwWriteEdit(edit.address,edit.after,5)); }
    typedef void* (__fastcall* Wrapper)(void*,void*,DWORD,DWORD*);
    Wrapper wrapper=(Wrapper)0x604A5D;
    Name prefix({'M','o','r','d','o','r'}),bare({'A','t','t','a','c','k'}),qualified({'M','o','r','d','o','r',':',':','A','t','t','a','c','k'}),missing({'N','o'});
    prefixName=prefix.value;Table t;t.add(bare.value,0x23000000);t.sync();BYTE engine[16]={};
    for(unsigned mode=0;mode<8;++mode) {
        wrapperTable=mode&1?NULL:&t;bareName=mode&2?missing.value:bare.value;
        bool write=!(mode&4),hit=!(mode&3);
        const std::vector<unsigned> expected=hit && write?std::vector<unsigned>{1,2,3,4,5}:std::vector<unsigned>{1,2,4,5};
        for(unsigned stock=0;stock<2;++stock) {
            g_rwScriptKill=stock!=0;DWORD output=0xBAD;events.clear();
            void* answer=wrapper(engine,NULL,qualified.value,write?&output:NULL);
            assert(answer==(hit?(void*)0x23000004:NULL));
            assert(output==(hit && write?prefixName:0xBAD) && events==expected);
        }
    }
    g_rwScriptKill=false;
    for(auto& edit:edits) assert(rwWriteEdit(edit.address,edit.before,5));
    assert(rwBodyHash((BYTE*)0x604A5D,128)==0xD338F3DD);
    printf("PASS: unchanged native findScript wrapper ABI, selected/missing side, hit/miss, optional canonical output and destructor order (controlled dependencies)\n");
}

static volatile DWORD sink;
template<class Fn> static double timed(unsigned n,Fn fn) {
    LONG64 start=qpcNow();for(unsigned i=0;i<n;++i) sink^=(DWORD)fn(i);
    return (qpcNow()-start)*1e9/g_pqpf.QuadPart/n;
}
static void benchmarks() {
    // Compare with the pre-existing accelerator, whose imported memcmp is
    // already replaced. This isolates the new lookup change's benefit.
    *(void**)0xBD0698=(void*)fastMemcmp;
    HANDLE current=GetCurrentThread();DWORD_PTR processMask,systemMask;
    assert(GetProcessAffinityMask(GetCurrentProcess(),&processMask,&systemMask));
    unsigned cpu=GetCurrentProcessorNumber();DWORD_PTR mask=(DWORD_PTR)1<<cpu;
    assert(SetProcessAffinityMask(GetCurrentProcess(),mask));
    DWORD_PTR oldAffinity=SetThreadAffinityMask(current,mask);assert(oldAffinity);
    printf("BENCH CONFIG: entire fixture process pinned to logical CPU %u; 200000 calls/sample; 1/64 native self-check enabled\n",cpu);
    for(unsigned count:{1u,2u,8u,32u,256u,2048u}) for(unsigned prefix:{0u,128u}) {
        std::vector<Name*> names,queries;Table t;
        for(unsigned i=0;i<count;++i) {
            char suffix[32];sprintf(suffix,"Script_%08u",i);
            std::vector<BYTE> bytes(prefix,'x');bytes.insert(bytes.end(),suffix,suffix+strlen(suffix));
            names.push_back(new Name(bytes));t.add(names.back()->value,0x24000000+i*16);
            queries.push_back(new Name(bytes)); // independent equal buffers, no pointer shortcut
        }
        for(const char* suffix:{"!missing","Script_99999999"}) {
            std::vector<BYTE> bytes(prefix,'x');bytes.insert(bytes.end(),suffix,suffix+strlen(suffix));
            queries.push_back(new Name(bytes));
        }
        t.sync();
        const unsigned n=200000;std::vector<DWORD> schedule(1024);
        for(unsigned i=0;i<schedule.size();++i)
            schedule[i]=queries[i%4<2?count+i%4:randomWord()%count]->value;
        double stock[7],fast[7];RwScriptFn hooked=(RwScriptFn)0x7B72EC;
        auto native=[&](unsigned i){return realOriginal(t.list,NULL,&schedule[i&1023]);};
        auto candidate=[&](unsigned i){return hooked(t.list,NULL,&schedule[i&1023]);};
        for(unsigned i=0;i<4096;++i) assert(native(i)==candidate(i));
        for(unsigned sample=0;sample<7;++sample) {
            if(sample&1) {fast[sample]=timed(n,candidate);stock[sample]=timed(n,native);}
            else {stock[sample]=timed(n,native);fast[sample]=timed(n,candidate);}
        }
        std::sort(stock,stock+7);std::sort(fast,fast+7);
        printf("BENCH scripts=%u prefix=%u: previous-accelerator %.2f ns, hooked %.2f ns, %.2fx; seven alternating samples, 50%% hits/50%% low+high misses, one logical CPU\n",count,prefix,stock[3],fast[3],stock[3]/fast[3]);
        assert(!g_rwScriptKill);
        for(Name* name:names) delete name;for(Name* name:queries) delete name;
    }
    assert(SetProcessAffinityMask(GetCurrentProcess(),processMask));
    assert(SetThreadAffinityMask(current,oldAffinity));
}
int main(int argc,char** argv) {
    setvbuf(stdout,NULL,_IONBF,0);assert(argc==2 || argc==3);
    if(strcmp(argv[1],"child")) return runMappedRetailChild(argv[1],argc==3);
    InitializeCriticalSection(&g_logCs);QueryPerformanceFrequency(&g_pqpf);g_mkTid=GetCurrentThreadId();
    // Repair only the imported memcmp slot in this private fixture. All
    // guarded search/comparison bodies execute their original native bytes.
    *(void**)0xBD0698=(void*)memcmp;
    printf("REFERENCE: host CRT memcmp\n");layoutTests((RwScriptFn)0x7B72EC);
    char crtPath[MAX_PATH];DWORD length=GetEnvironmentVariableA("AOTR_TEST_MSVCR71",crtPath,sizeof(crtPath));
    HMODULE crt=NULL;
    if(length) {
        assert(length<sizeof(crtPath));crt=LoadLibraryExA(crtPath,NULL,LOAD_WITH_ALTERED_SEARCH_PATH);assert(crt);
        void* comparator=(void*)GetProcAddress(crt,"memcmp");assert(comparator);*(void**)0xBD0698=comparator;
        printf("REFERENCE: supplied msvcr71.dll memcmp\n");layoutTests((RwScriptFn)0x7B72EC);
    }
    installationTests();hookTests();wrapperTests();
    if(argc==3) benchmarks();
    if(crt) FreeLibrary(crt);
    DeleteCriticalSection(&g_logCs);printf("ALL SCRIPT CHECKS PASSED\n");return 0;
}
