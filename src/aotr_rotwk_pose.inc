// Per-call quaternion-to-matrix reuse for the verified retail HLod routine.
// All animation updates and virtual child calls remain on the calling thread.
typedef void (__fastcall* RwPoseFn)(void*, void*);
static RwPoseFn g_rwPoseOriginal = NULL;
static RwPoseFn g_rwPoseBase = (RwPoseFn)0x005A5050;
static __forceinline __m128 rwTwiceProduct(__m128 a, __m128 b) {
    return _mm_mul_ss(_mm_mul_ss(a, b), _mm_set_ss(2.0f));
}
static void rwPivotMatrix(const float* q, float* m) {
    __m128 x = _mm_load_ss(q), y = _mm_load_ss(q+1), z = _mm_load_ss(q+2), w = _mm_load_ss(q+3);
    __m128 xx = rwTwiceProduct(x,x), yy = rwTwiceProduct(y,y), zz = rwTwiceProduct(z,z);
    __m128 xy = rwTwiceProduct(y,x), xz = rwTwiceProduct(x,z), wx = rwTwiceProduct(x,w);
    __m128 wy = rwTwiceProduct(y,w), yz = rwTwiceProduct(y,z), wz = rwTwiceProduct(w,z);
    __m128 one = _mm_set_ss(1.0f);
    _mm_store_ss(m,   _mm_sub_ss(_mm_sub_ss(one,yy),zz));
    _mm_store_ss(m+1, _mm_sub_ss(xy,wz));
    _mm_store_ss(m+2, _mm_add_ss(wy,xz));
    _mm_store_ss(m+4, _mm_add_ss(wz,xy));
    _mm_store_ss(m+5, _mm_sub_ss(_mm_sub_ss(one,zz),xx));
    _mm_store_ss(m+6, _mm_sub_ss(yz,wx));
    _mm_store_ss(m+8, _mm_sub_ss(xz,wy));
    _mm_store_ss(m+9, _mm_add_ss(yz,wx));
    _mm_store_ss(m+10,_mm_sub_ss(_mm_sub_ss(one,yy),xx));
    memcpy(m+3,q+4,4); memcpy(m+7,q+5,4); memcpy(m+11,q+6,4);
}
struct RwPoseMatrixEntry { const float* pivot; DWORD control, status; BYTE input[28]; float matrix[12]; };
struct RwPoseMatrixCache { ULONGLONG valid; RwPoseMatrixEntry entries[64]; };
static __forceinline bool rwPoseInputEqual(const BYTE* a,const BYTE* b) {
    __m128i first = _mm_cmpeq_epi32(_mm_loadu_si128((const __m128i*)a),_mm_loadu_si128((const __m128i*)b));
    __m128i last = _mm_cmpeq_epi32(_mm_loadu_si128((const __m128i*)(a+12)),_mm_loadu_si128((const __m128i*)(b+12)));
    return _mm_movemask_epi8(_mm_and_si128(first,last)) == 65535;
}
static __forceinline void rwPoseMatrixCopy(float* dst,const float* src) {
    _mm_storeu_ps(dst,_mm_loadu_ps(src));
    _mm_storeu_ps(dst+4,_mm_loadu_ps(src+4));
    _mm_storeu_ps(dst+8,_mm_loadu_ps(src+8));
}
static __forceinline void rwPoseMatrix(RwPoseMatrixCache& cache, BYTE* pivot, DWORD bone, float* matrix) {
    unsigned slot = (bone * 2654435761u) >> 26;
    ULONGLONG mask = 1ull << slot;
    RwPoseMatrixEntry& entry = cache.entries[slot];
    const float* input = (const float*)(pivot + 0x30);
    DWORD csr = _mm_getcsr();
    if ((csr & 0x1F80) != 0x1F80 || !(cache.valid & mask) || entry.pivot != input || entry.control != (csr & ~63u) ||
        (entry.status & ~csr) || !rwPoseInputEqual(entry.input,(const BYTE*)input)) {
        rwPivotMatrix(input, entry.matrix);
        entry.pivot = input; entry.control = csr & ~63u; entry.status = _mm_getcsr() & 63;
        _mm_storeu_si128((__m128i*)entry.input,_mm_loadu_si128((const __m128i*)input));
        _mm_storeu_si128((__m128i*)(entry.input+12),_mm_loadu_si128((const __m128i*)((const BYTE*)input+12)));
        cache.valid |= mask;
    }
    rwPoseMatrixCopy(matrix,entry.matrix);
}
static bool rwHasOffset(const float* offset) {
    __m128 zero = _mm_setzero_ps();
    return (_mm_movemask_ps(_mm_cmpneq_ss(_mm_load_ss(offset),zero)) & 1) ||
           (_mm_movemask_ps(_mm_cmpneq_ss(_mm_load_ss(offset+1),zero)) & 1) ||
           (_mm_movemask_ps(_mm_cmpneq_ss(_mm_load_ss(offset+2),zero)) & 1);
}
static void rwPoseTranslate(float* matrix, const float* offset) {
    for (int row = 0; row < 3; ++row) {
        float* m = matrix + row * 4;
        __m128 value = _mm_mul_ss(_mm_load_ss(m),_mm_load_ss(offset));
        value = _mm_add_ss(value,_mm_mul_ss(_mm_load_ss(offset+2),_mm_load_ss(m+2)));
        value = _mm_add_ss(value,_mm_mul_ss(_mm_load_ss(offset+1),_mm_load_ss(m+1)));
        _mm_store_ss(m+3,_mm_add_ss(value,_mm_load_ss(m+3)));
    }
}
static __forceinline BYTE* rwPosePivot(BYTE* self, DWORD bone) {
    BYTE* tree = *(BYTE**)(self+0xF8);
    return *(BYTE**)(tree+0x14) + bone * 0x58;
}
static __forceinline void rwPoseApply(BYTE* self, BYTE* item, bool additional, RwPoseMatrixCache& cache) {
    void* child = *(void**)item; DWORD bone = *(DWORD*)(item+4);
    float matrix[12];
    rwPoseMatrix(cache, rwPosePivot(self,bone), bone, matrix);
    if (additional && rwHasOffset((const float*)(item+8))) rwPoseTranslate(matrix,(const float*)(item+8));
    ((void(__fastcall*)(void*,void*,const float*))(*(void***)child)[0x54/4])(child,NULL,matrix);
    // The stock order differs for additional models. Reload pivot data after
    // each callback, since callbacks may replace a tree or mutate its fields.
    if (additional) {
        DWORD factor = *(DWORD*)(rwPosePivot(self,bone)+0x54);
        ((void(__fastcall*)(void*,void*,DWORD,DWORD))(*(void***)child)[0x5C/4])(child,NULL,0,factor);
    }
    DWORD hidden = rwPosePivot(self,bone)[0x50] == 0;
    ((void(__fastcall*)(void*,void*,DWORD))(*(void***)child)[0x19C/4])(child,NULL,hidden);
    if (!additional) {
        DWORD factor = *(DWORD*)(rwPosePivot(self,bone)+0x54);
        ((void(__fastcall*)(void*,void*,DWORD,DWORD))(*(void***)child)[0x5C/4])(child,NULL,0,factor);
    }
    ((RwPoseFn)(*(void***)child)[0xA8/4])(child,NULL);
}
static void __fastcall hkRwPose(void* object, void* edx) {
    if (!g_mkTid || GetCurrentThreadId() != g_mkTid || (_mm_getcsr() & 0x1F80) != 0x1F80) {
        g_rwPoseOriginal(object,edx); return;
    }
    BYTE* self = (BYTE*)object;
    RwPoseMatrixCache cache; cache.valid = 0;
    g_rwPoseBase(object,edx);
    for (LONG lod = 0; lod < *(LONG*)(self+0x120); ++lod) {
        for (LONG model = 0;; ++model) {
            BYTE* group = *(BYTE**)(self+0x128) + lod * 0x28;
            if (model >= *(LONG*)(group+0x10)) break;
            rwPoseApply(self,*(BYTE**)(group+4)+model*0x14,false,cache);
        }
    }
    for (LONG model = 0; model < *(LONG*)(self+0x148); ++model)
        rwPoseApply(self,*(BYTE**)(self+0x13C)+model*0x14,true,cache);
    self[0x12] &= 0xDF;
}
