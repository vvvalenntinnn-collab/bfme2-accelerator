#pragma once
// Reports potential contiguous triangle-list ranges only. This predicate does
// not establish device-cap, instancing, resource or image equivalence.
static bool accelAdjacentDrawRanges(unsigned op,const DWORD* a,const DWORD* b) {
    if(a[0]!=b[0] || a[1]!=4 || b[1]!=4) return false;
    if(op==81) {
        if(!a[3] || !b[3]) return false;
        ULONGLONG end=(ULONGLONG)a[2]+(ULONGLONG)a[3]*3;
        return end<=0xFFFFFFFFu && end==b[2] && (ULONGLONG)a[3]+b[3]<=0xFFFFFFFFu;
    }
    if(op==82) {
        if(a[2]!=b[2] || !a[4] || !b[4] || !a[6] || !b[6]) return false;
        ULONGLONG end=(ULONGLONG)a[5]+(ULONGLONG)a[6]*3;
        return end<=0xFFFFFFFFu && end==b[5] && (ULONGLONG)a[6]+b[6]<=0xFFFFFFFFu &&
            (ULONGLONG)a[3]+a[4]<=0xFFFFFFFFu && (ULONGLONG)b[3]+b[4]<=0xFFFFFFFFu;
    }
    return false;
}
