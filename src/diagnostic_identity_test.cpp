// Exercise the shipping logger/symbol formatting and unsupported-map exit.
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include "aotr_accel.cpp"

int main() {
    InitializeCriticalSection(&g_logCs);
    char temp[MAX_PATH];
    assert(GetTempPathA(sizeof(temp), temp));
    assert(GetTempFileNameA(temp, "adi", 0, kLogPath));

    aotrSetGameIdentity(GetModuleHandleA(NULL));
    assert(g_gameModulePath[0] && strstr(g_gameModuleName, ".exe"));
    assert(!strchr(g_gameModuleName, '\\'));
    g_gameBase = 0x400000; g_gameEnd = 0xED4000;
    char symbol[160];
    lstrcpyA(g_gameModuleName, "lotrbfme2ep1.exe");
    symName(0x5A7DF0, symbol);
    assert(!strcmp(symbol, "lotrbfme2ep1.exe+1A7DF0"));
    lstrcpyA(g_gameModuleName, "game.dat");
    symName(0x5A7DF0, symbol);
    assert(!strcmp(symbol, "game.dat+1A7DF0"));

    g_profileMapVerified = false;
    assert(gsFunc(0x5F5123) == 0);
    startGameSampler();
    assert(!g_gsBound && !g_gsStart);
    g_profileMapVerified = true;
    assert(gsFunc(kGameFuncs[100]) == kGameFuncs[100]);
#ifdef AOTR_PROD
    startGameSampler();
    assert(!g_gsBound && !g_gsStart);
#endif

    char longStack[4096]; memset(longStack, 'x', sizeof(longStack)-1); longStack[sizeof(longStack)-1] = 0;
    logf("crash: %s", longStack);
    logf("following record %d", 42);
    HANDLE file = CreateFileA(kLogPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    char records[8192] = {}; DWORD bytes = 0;
    assert(ReadFile(file, records, sizeof(records)-1, &bytes, NULL)); CloseHandle(file);
    const char* crash = strstr(records, "crash: "); assert(crash);
    const char* end = strstr(crash, "\r\n"); assert(end);
    assert(end - crash <= 1021 && end - crash > 900);
    assert(strstr(end + 2, "following record 42\r\n"));
    g_engineHooks = 0; g_mkFrames = 0; g_mkPeriod = 0; g_rtLastPortablePresent = 0;
    rtNotePortableFrame(); rtNotePortableFrame();
    assert(g_mkFrames == 2);
#ifdef AOTR_PROD
    assert(!g_rtLastPortablePresent && !g_mkPeriod);
#else
    assert(g_rtLastPortablePresent && g_mkPeriod >= 0);
#endif
    g_engineHooks = 1;
    rtNotePortableFrame(); assert(g_mkFrames == 2);
    assert(DeleteFileA(kLogPath));
    DeleteCriticalSection(&g_logCs);
    puts("PASS: module identity, EXE/DAT symbols, unsupported-map fallback, bounded logs, portable frame accounting and production clock exclusion");
    return 0;
}
