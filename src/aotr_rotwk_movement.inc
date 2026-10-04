// Exact retail pathfinding min-heap operations. Preserve Floyd's descent,
// right-child-on-equality choice and the native push-back-up phase. Cell/info
// keys stay immutable during each call; no persistent priority/route cache.
typedef void (__cdecl* RwMoveHeapFn)(DWORD*,LONG,LONG,DWORD,DWORD);
static RwMoveHeapFn g_rwMoveHeapPushOriginal=NULL,g_rwMoveHeapAdjustOriginal=NULL;
static const RwRegion kRwMoveHeapRegions[]={
    {0x006ECF1D,72,0x71BF798C},
    {0x006ECF65,96,0xBA8CDDB2}
};
// Explicit x86 register allocation avoids compiler spills/CMOV dependence
// that regressed small, predictable heaps. Valid heap indices are >= 0.
static void __declspec(naked) __cdecl hkRwMoveHeapPush(DWORD*,LONG,LONG,DWORD,DWORD) {
    __asm {
        push ebx
        push esi
        push edi
        mov esi,[esp+16]
        mov ecx,[esp+20]
        cmp ecx,[esp+24]
        jle push_store
        mov edi,[esp+28]
        mov edi,[edi]
        movzx edi,word ptr [edi+10h]
    push_loop:
        lea eax,[ecx-1]
        shr eax,1
        mov edx,[esi+eax*4]
        mov ebx,[edx]
        cmp word ptr [ebx+10h],di
        jbe push_store
        mov [esi+ecx*4],edx
        mov ecx,eax
        cmp ecx,[esp+24]
        jg push_loop
    push_store:
        // Native also leaves the inserted value in EAX, even though these
        // template helpers are void. Preserve that incidental return state.
        mov eax,[esp+28]
        mov [esi+ecx*4],eax
        pop edi
        pop esi
        pop ebx
        ret
    }
}
static void __declspec(naked) __cdecl hkRwMoveHeapAdjust(DWORD*,LONG,LONG,DWORD,DWORD) {
    __asm {
        push ebx
        push ebp
        push esi
        push edi
        mov esi,[esp+20]
        mov ecx,[esp+24]
        mov ebp,ecx
        lea eax,[ecx*2+2]
        cmp eax,[esp+28]
        jge adjust_tail
    adjust_down:
        mov edx,[esi+eax*4]
        mov ebx,[edx]
        mov edi,[esi+eax*4-4]
        mov edi,[edi]
        mov bx,[ebx+10h]
        cmp bx,[edi+10h]
        jbe adjust_chosen
        dec eax
    adjust_chosen:
        mov edx,[esi+eax*4]
        mov [esi+ecx*4],edx
        mov ecx,eax
        lea eax,[eax*2+2]
        cmp eax,[esp+28]
        jl adjust_down
    adjust_tail:
        jne adjust_up_test
        mov edx,[esi+eax*4-4]
        mov [esi+ecx*4],edx
        lea ecx,[eax-1]
    adjust_up_test:
        cmp ecx,ebp
        jle adjust_store
        mov edi,[esp+32]
        mov edi,[edi]
        movzx ebx,word ptr [edi+10h]
    adjust_up:
        lea eax,[ecx-1]
        shr eax,1
        mov edx,[esi+eax*4]
        mov edi,[edx]
        cmp word ptr [edi+10h],bx
        jbe adjust_store
        mov [esi+ecx*4],edx
        mov ecx,eax
        cmp ecx,ebp
        jg adjust_up
    adjust_store:
        mov eax,[esp+32]
        mov [esi+ecx*4],eax
        pop edi
        pop esi
        pop ebp
        pop ebx
        ret
    }
}
static bool rwMoveHeapSelfTest(RwMoveHeapFn push,RwMoveHeapFn adjust) {
    BYTE cells[256][16]={},info[256][0x3C]={};DWORD a[257],b[257],state=0x713569A9;
    for(unsigned i=0;i<256;++i) *(BYTE**)cells[i]=info[i];
    // A trivial hole requires no key dereference, even for a null value.
    a[0]=b[0]=0xBAD;push(a,0,0,0,0);hkRwMoveHeapPush(b,0,0,0,0);
    if(a[0]!=b[0]) return false;
    adjust(a,0,1,0,0);hkRwMoveHeapAdjust(b,0,1,0,0);
    if(a[0]!=b[0]) return false;
    const unsigned sizes[]={1,2,3,7,8,15,31,32,127,128,255,256};
    for(unsigned mode=0;mode<6;++mode) for(unsigned ni=0;ni<_countof(sizes);++ni) {
        unsigned size=sizes[ni];memset(a,0xBA,sizeof(a));memcpy(b,a,sizeof(a));
        for(unsigned i=0;i<size;++i) {
            state^=state<<13;state^=state>>17;state^=state<<5;
            WORD cost=mode==0?0:mode==1?65535:mode==2?(WORD)i:mode==3?(WORD)(size-i):mode==4?(WORD)(i%5):(WORD)state;
            *(WORD*)(info[i]+0x10)=cost;
            push(a,i,0,(DWORD)cells[i],0);hkRwMoveHeapPush(b,i,0,(DWORD)cells[i],0);
            if(memcmp(a,b,sizeof(a))) return false;
        }
        while(size>1) {
            --size;DWORD value=a[size];adjust(a,0,size,value,0);hkRwMoveHeapAdjust(b,0,size,value,0);
            if(memcmp(a,b,sizeof(a))) return false;
        }
    }
    return true;
}
static bool rwInstallMoveHeap() {
    if(!rwEnabled("AOTR_MOVEHEAP") || !rwRegionsMatch(kRwMoveHeapRegions,_countof(kRwMoveHeapRegions))) return false;
    RwHook hooks[]={
        {0x006ECF1D,72,0x71BF798C,6,(void*)hkRwMoveHeapPush},
        {0x006ECF65,96,0xBA8CDDB2,6,(void*)hkRwMoveHeapAdjust}
    };
    if(!rwPrepareHooks(hooks,_countof(hooks))) return false;
    g_rwMoveHeapPushOriginal=(RwMoveHeapFn)hooks[0].trampoline;
    g_rwMoveHeapAdjustOriginal=(RwMoveHeapFn)hooks[1].trampoline;
    if(!rwMoveHeapSelfTest(g_rwMoveHeapPushOriginal,g_rwMoveHeapAdjustOriginal)) {
        logf("rotwk: path heap startup differs - native operations kept.");return false;
    }
    RwEdit edits[]={rwBranchEdit(hooks[0].address,6,0xE9,hooks[0].replacement),
        rwBranchEdit(hooks[1].address,6,0xE9,hooks[1].replacement)};
    if(!rwCommitEdits(edits,_countof(edits),kRwMoveHeapRegions,_countof(kRwMoveHeapRegions))) return false;
    logf("rotwk: pathfinding heap installed after 72 startup workload matches; exact tie/descent order, no cost/route/budget changes.");
    return true;
}
