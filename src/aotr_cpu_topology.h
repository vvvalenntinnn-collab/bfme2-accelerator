#pragma once
// One representative per allowed physical core; SMT siblings never create
// extra compute slots. The 32-bit accelerator conservatively supports group 0.
struct AccelCpuPlan { DWORD count, cpu[32]; DWORD_PTR mask[32]; };
static AccelCpuPlan accelCpuPlan(const DWORD_PTR* cores,unsigned count,DWORD_PTR allowed,DWORD current) {
    AccelCpuPlan p={};
    DWORD_PTR currentBit=current<32 ? (DWORD_PTR)1<<current : 0;
    for (unsigned pass=0;pass<2;++pass) for (unsigned i=0;i<count && p.count<32;++i) {
        DWORD_PTR m=cores[i]&allowed;
        if (!m || ((m&currentBit)!=0)!=(pass==0)) continue;
        DWORD cpu=0; while (!((m>>cpu)&1)) ++cpu;
        if (m&currentBit) cpu=current;
        p.cpu[p.count]=cpu; p.mask[p.count++]=m;
    }
    return p;
}
static AccelCpuPlan accelAllowedCpuPlan() {
    AccelCpuPlan empty={}; DWORD_PTR allowed=0,system=0;
    if (!GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system)) return empty;
    DWORD length=0; GetLogicalProcessorInformationEx(RelationProcessorCore,NULL,&length);
    if (!length || length>(1u<<20)) return empty;
    BYTE* data=(BYTE*)VirtualAlloc(NULL,length,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (!data) return empty;
    DWORD_PTR masks[32]={}; unsigned count=0;
    bool ok=GetLogicalProcessorInformationEx(RelationProcessorCore,(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)data,&length)!=FALSE;
    for (DWORD off=0;ok && off<length;) {
        auto e=(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(data+off);
        if (!e->Size || e->Size>length-off) { ok=false; break; }
        if (e->Relationship==RelationProcessorCore && e->Processor.GroupCount==1 && e->Processor.GroupMask[0].Group==0 && count<32)
            masks[count++]=(DWORD_PTR)e->Processor.GroupMask[0].Mask;
        off+=e->Size;
    }
    AccelCpuPlan plan=ok?accelCpuPlan(masks,count,allowed,GetCurrentProcessorNumber()):empty;
    VirtualFree(data,0,MEM_RELEASE); return plan;
}
static AccelCpuPlan accelReserveCpuRoles(AccelCpuPlan p,DWORD mainCpu,DWORD renderCpu) {
    DWORD roles[2]={mainCpu,renderCpu};
    for(unsigned slot=0;slot<2 && slot<p.count;++slot) {
        if(roles[slot]>=32) continue;
        DWORD_PTR bit=(DWORD_PTR)1<<roles[slot];
        for(unsigned i=slot;i<p.count;++i) if(p.mask[i]&bit) {
            if(i!=slot) {
                DWORD cpu=p.cpu[slot];DWORD_PTR mask=p.mask[slot];
                p.cpu[slot]=p.cpu[i];p.mask[slot]=p.mask[i];p.cpu[i]=cpu;p.mask[i]=mask;
            }
            p.cpu[slot]=roles[slot];break;
        }
    }
    return p;
}
