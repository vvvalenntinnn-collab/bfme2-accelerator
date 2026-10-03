// Large staged uploads keep their storage until the worker's last access.
// Only the producer allocates/reclaims heap blocks. The worker publishes done
// after Lock/copy/Unlock/Release and never accesses that block again.
#define RT_UPLOAD_MIN (16u << 10)
#define RT_UPLOAD_MAX (4u << 20)
#define RT_UPLOAD_LIMIT (16u << 20)
#define RT_UPLOAD_CLASSES 9
struct RtUploadBlock {
    volatile LONG done;
    DWORD cap;
    RtUploadBlock* next;
};
static HANDLE g_rtUploadHeap = NULL;
static RtUploadBlock* g_rtUploadFree[RT_UPLOAD_CLASSES];
static RtUploadBlock* g_rtUploadFirst = NULL;
static RtUploadBlock* g_rtUploadLast = NULL;
static DWORD g_rtUploadBytes = 0;
static bool g_rtUploadOn = true;
static __forceinline BYTE* rtUploadData(RtUploadBlock* block) {
    return (BYTE*)(((ULONG_PTR)(block + 1) + 15) & ~(ULONG_PTR)15);
}
static int rtUploadClass(DWORD size) {
    int index = 0;
    for (DWORD cap = RT_UPLOAD_MIN; cap < size; cap <<= 1) ++index;
    return index;
}
static void rtUploadReap() {
    while (g_rtUploadFirst && InterlockedCompareExchange(&g_rtUploadFirst->done, 0, 0)) {
        RtUploadBlock* block = g_rtUploadFirst;
        g_rtUploadFirst = block->next;
        if (!g_rtUploadFirst) g_rtUploadLast = NULL;
        int index = rtUploadClass(block->cap);
        block->next = g_rtUploadFree[index];
        g_rtUploadFree[index] = block;
    }
}
static RtUploadBlock* rtUploadAcquire(DWORD size) {
    if (!g_rtUploadOn || size < RT_UPLOAD_MIN || size > RT_UPLOAD_MAX) return NULL;
    rtUploadReap();
    int index = rtUploadClass(size);
    RtUploadBlock* block = g_rtUploadFree[index];
    if (block) {
        g_rtUploadFree[index] = block->next;
    } else {
        DWORD cap = RT_UPLOAD_MIN << index;
        // Trim completed cached storage only. Locked/in-flight storage is never
        // freed to make space; budget pressure falls back to the copied path.
        for (int i = 0; g_rtUploadBytes > RT_UPLOAD_LIMIT - cap && i < RT_UPLOAD_CLASSES; ++i) {
            while (g_rtUploadFree[i] && g_rtUploadBytes > RT_UPLOAD_LIMIT - cap) {
                RtUploadBlock* old = g_rtUploadFree[i];
                g_rtUploadFree[i] = old->next;
                g_rtUploadBytes -= old->cap;
                HeapFree(g_rtUploadHeap, HEAP_NO_SERIALIZE, old);
            }
        }
        if (g_rtUploadBytes > RT_UPLOAD_LIMIT - cap) return NULL;
        if (!g_rtUploadHeap) g_rtUploadHeap = HeapCreate(HEAP_NO_SERIALIZE, 0, 0);
        if (!g_rtUploadHeap) return NULL;
        block = (RtUploadBlock*)HeapAlloc(g_rtUploadHeap, HEAP_NO_SERIALIZE, sizeof(RtUploadBlock) + 15 + cap);
        if (!block) return NULL;
        block->cap = cap;
        g_rtUploadBytes += cap;
    }
    block->done = 0; block->next = NULL;
    return block;
}
static void rtUploadQueue(RtUploadBlock* block) {
    // Unlock order can differ from Lock order. Link in execution order so one
    // completion check can retire the oldest upload without scanning all blocks.
    if (g_rtUploadLast) g_rtUploadLast->next = block;
    else g_rtUploadFirst = block;
    g_rtUploadLast = block;
}
static __forceinline void rtUploadDone(RtUploadBlock* block) {
    if (block) InterlockedExchange(&block->done, 1);
}
