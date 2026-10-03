// Exercises shipping waits/scopes and a real queue worker in both build modes.
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>

static DWORD fixtureMainTid;
static volatile LONG fixtureMainClocks, fixtureOtherClocks;
static bool fixtureFastTime;
static LONG64 fixtureClockJump, fixtureFrequency;
static BOOL WINAPI fixtureClock(LARGE_INTEGER* value) {
    BOOL result = QueryPerformanceCounter(value);
    if (GetCurrentThreadId() == fixtureMainTid) {
        InterlockedIncrement(&fixtureMainClocks);
        if (fixtureFastTime) {
            value->QuadPart += fixtureClockJump;
            fixtureClockJump += fixtureFrequency * 6;
        }
    } else InterlockedIncrement(&fixtureOtherClocks);
    return result;
}
#define QueryPerformanceCounter fixtureClock
#include "aotr_accel.cpp"
#undef QueryPerformanceCounter

static HANDLE fixtureMessageEvent;
static volatile LONG fixtureMessages;
static LRESULT CALLBACK fixtureWindow(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_APP + 42) {
        InterlockedIncrement(&fixtureMessages);
        SetEvent(fixtureMessageEvent);
        return 42;
    }
    return DefWindowProcA(window, message, w, l);
}
static DWORD WINAPI fixtureSend(void* window) {
    return (DWORD)SendMessageA((HWND)window, WM_APP + 42, 0, 0);
}

static void testWaits() {
    HANDLE event = CreateEventA(NULL, TRUE, FALSE, NULL);
    assert(event);
    rtWaitH(event, 0); // timeout
    SetEvent(event);
    rtWaitH(event, 0); // signaled
#ifdef AOTR_PROD
    assert(!fixtureMainClocks && !g_rtWaitCalls && !g_rtWaitTimeouts && !g_rtWaitTicks);
#else
    assert(fixtureMainClocks == 4 && g_rtWaitCalls == 2 && g_rtWaitTimeouts == 1);
#endif
    CloseHandle(event);

    // Driver-style cross-thread SendMessage must still be dispatched while waiting.
    WNDCLASSA wc = {};
    wc.lpfnWndProc = fixtureWindow;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "AotrQueueWaitFixture";
    assert(RegisterClassA(&wc));
    HWND window = CreateWindowExA(0, wc.lpszClassName, "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    assert(window);
    fixtureMessageEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE sender = CreateThread(NULL, 0, fixtureSend, window, 0, NULL);
    assert(fixtureMessageEvent && sender);
    ULONGLONG deadline = GetTickCount64() + 5000;
    while (!fixtureMessages && GetTickCount64() < deadline) rtWaitH(fixtureMessageEvent, 10);
    assert(fixtureMessages == 1 && WaitForSingleObject(sender, 5000) == WAIT_OBJECT_0);
    DWORD result = 0;
    assert(GetExitCodeThread(sender, &result) && result == 42);
    CloseHandle(sender); CloseHandle(fixtureMessageEvent);
    DestroyWindow(window);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
#ifdef AOTR_PROD
    assert(!fixtureMainClocks && !g_rtWaitCalls && !g_rtWaitTicks);
#else
    assert(fixtureMainClocks == g_rtWaitCalls * 2);
#endif
}

static DWORD WINAPI fixturePresent(void*) {
    Sleep(10);
    InterlockedDecrement(&g_rtPendingPresents);
    SetEvent(g_rtPresented);
    return 0;
}
static void testBackpressure() {
    fixtureMainClocks = 0;
    g_rtWaitCalls = g_rtWaitTimeouts = 0;
    g_rtWaitTicks = 0;
    g_rtPendingPresents = 1;
    rtPresentBackpressure();
    assert(!fixtureMainClocks);
    g_rtPresented = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_rtPendingPresents = 2;
    HANDLE presenter = CreateThread(NULL, 0, fixturePresent, NULL, 0, NULL);
    assert(g_rtPresented && presenter);
    rtPresentBackpressure();
    assert(g_rtPendingPresents == 1 && WaitForSingleObject(presenter, 5000) == WAIT_OBJECT_0);
    CloseHandle(presenter);
#ifdef AOTR_PROD
    assert(!fixtureMainClocks && !g_rtWaitCalls && !g_rtPresentWaitTicks);
#else
    assert(fixtureMainClocks == 2 + 2 * g_rtWaitCalls);
#endif
}

static unsigned fixtureScopeCalls;
static DWORD __stdcall fixtureD3dx(DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f, DWORD g, DWORD h, DWORD i) {
    assert(a+b+c+d+e+f+g+h+i == 45);
    assert(g_rtDirectScope == 1 && g_rtXDepth == 1);
    ++fixtureScopeCalls;
    return 0x88760868; // preserve a failure HRESULT as well as success
}
static void testScope() {
    fixtureMainClocks = 0;
    g_rtMainTid = g_mkTid = fixtureMainTid;
    g_rtActive = 1;
    g_rtXO[0] = (void*)fixtureD3dx;
    assert(rtx0(1,2,3,4,5,6,7,8,9) == 0x88760868 && fixtureScopeCalls == 1);
    assert(!g_rtDirectScope && !g_rtXDepth);
#ifdef AOTR_PROD
    assert(!fixtureMainClocks && !g_rtXCalls[0] && !g_rtXTicks[0] && !g_rtNScope);
#else
    assert(fixtureMainClocks == 2 && g_rtXCalls[0] == 1 && g_rtNScope == 1);
#endif
}

static HANDLE fixtureGate;
static volatile LONG fixtureExecuted;
static DWORD __stdcall fixtureRecord(DWORD value) {
    if (!value) {
        assert(WaitForSingleObject(fixtureGate, 5000) == WAIT_OBJECT_0);
    } else {
        assert(value == (DWORD)fixtureExecuted + 1);
        fixtureExecuted = (LONG)value;
    }
    return value;
}
static DWORD WINAPI fixtureOpenGate(void*) {
    Sleep(50);
    SetEvent(fixtureGate);
    return 0;
}
static void enqueue(DWORD value) {
    RtRec* record = rtAlloc(1, 0);
    record->fn = (void*)fixtureRecord;
    record->op = 9;
    rtArgs(record)[0] = value;
    rtCommit(record, 0);
}

static void testWorker() {
    g_rtBuf = (BYTE*)VirtualAlloc(NULL, RT_BUF_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_rtWake = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_rtDrained = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_rtReady = CreateEventA(NULL, TRUE, FALSE, NULL);
    fixtureGate = CreateEventA(NULL, TRUE, FALSE, NULL);
    assert(g_rtBuf && g_rtWake && g_rtDrained && g_rtReady && fixtureGate);
    g_rtObjRefs = false;
    // The first enqueue must emit a ring-wrap marker; sequence values also wrap.
    g_rtLocalHead = g_rtHead = g_rtTail = RT_BUF_SIZE - 128;
    g_rtPH.cachedTail = g_rtTail;
    g_rtSeq = g_rtExecSeq = 0xfffffff0u;
    g_rtX87 = _control87(0, 0);
    g_rtMxcsr = _mm_getcsr();
    fixtureMainClocks = fixtureOtherClocks = 0;
    HANDLE worker = CreateThread(NULL, 0, rtWorker, NULL, 0, NULL);
    assert(worker && WaitForSingleObject(g_rtReady, 5000) == WAIT_OBJECT_0);

    char tempPath[MAX_PATH];
    assert(GetTempPathA(MAX_PATH, tempPath));
    assert(GetTempFileNameA(tempPath, "arq", 0, kLogPath));
    enqueue(0);
    fixtureFastTime = true; // advance watchdog time without delaying a test by five seconds
    HANDLE opener = CreateThread(NULL, 0, fixtureOpenGate, NULL, 0, NULL);
    assert(opener);
    rtDrain();
    fixtureFastTime = false;
    assert(WaitForSingleObject(opener, 5000) == WAIT_OBJECT_0);
    CloseHandle(opener);
    FILE* log = fopen(kLogPath, "rb");
    assert(log);
    char line[1024] = {};
    assert(fgets(line, sizeof(line), log) && strstr(line, "waited > 5 s for the worker"));
    fclose(log);
    DeleteFileA(kLogPath);
    kLogPath[0] = 0;

    for (DWORD n = 1; n <= 10000; ++n) enqueue(n);
    g_rtHaveResourceRelease = true;
    rtDrain();
    // Holding the execution lock ensures the worker has finished recording this batch's stats.
    EnterCriticalSection(&g_rtExecCs);
    assert(fixtureExecuted == 10000 && g_rtSeq == g_rtExecSeq);
    assert(g_rtSeq == 0xfffffff0u + 10001u);
    assert(!g_rtDrainWaiting && !g_rtMainSleeping && !g_rtHaveResourceRelease && g_rtCurOp == 511);
#ifdef AOTR_PROD
    assert(!fixtureOtherClocks && !g_rtBusyTicks && !g_rtDrainTicks && !g_rtDrains);
    assert(!g_rtWorkerSleeps && !g_rtWorkerTimeouts && !g_rtWH.cpu);
    assert(fixtureMainClocks > 0); // watchdog clocks remain functional
#else
    assert(fixtureOtherClocks >= 2 && g_rtDrains >= 2);
#endif
    LeaveCriticalSection(&g_rtExecCs);
    // This worker intentionally lives until process exit, like the shipping worker.
    // Leave its queue/events/lock alive; do not terminate a thread inside an API call.
    CloseHandle(worker);
}

int main() {
    static_assert(sizeof(void*) == 4, "Use the x86 MSVC toolset");
    fixtureMainTid = GetCurrentThreadId();
    InitializeCriticalSection(&g_rtExecCs);
    InitializeCriticalSection(&g_logCs);
    QueryPerformanceFrequency(&g_pqpf);
    fixtureFrequency = g_pqpf.QuadPart;
    testWaits();
    testBackpressure();
    testScope();
    testWorker();
#ifdef AOTR_PROD
    puts("PASS production queue: report clocks/counters absent; waits, message pumping, scopes, order, wraps and watchdog preserved");
#else
    puts("PASS reporting queue: report clocks active; waits, message pumping, scopes, order, wraps and watchdog preserved");
#endif
}
