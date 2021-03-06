#include <Windows.h>
#include <stdint.h>
#include <tchar.h>
#include <Psapi.h>
#include <sddl.h>

#include "helpers.h"
#include "logging.h"
#include "service.h"
#include "iathook.h"
#include "patternfind.h"
#include "hooks.h"

LOADLIBRARYEXW fpLoadLibraryExW = NULL;
LOADLIBRARYEXA fpLoadLibraryExA = NULL;

DWORD WINAPI NewThreadProc(LPVOID lpParam) {
    SC_HANDLE hSCManager = OpenSCManager(NULL, SERVICES_ACTIVE_DATABASE, SC_MANAGER_CONNECT);

    TCHAR lpBinaryPathName[0x8000];
    get_svcpath(hSCManager, _T("wuauserv"), lpBinaryPathName, _countof(lpBinaryPathName));
    CloseServiceHandle(hSCManager);

    if (_tcsicmp(GetCommandLine(), lpBinaryPathName))
        return 0;

    SECURITY_ATTRIBUTES sa = { 0 };
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    ConvertStringSecurityDescriptorToSecurityDescriptor(_T("D:PAI(A;;FA;;;BA)"), SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL);
    sa.bInheritHandle = FALSE;

    HANDLE hEvent = CreateEvent(&sa, TRUE, FALSE, _T("Global\\wufuc_UnloadEvent"));
    if (!hEvent)
        return 0;

    DWORD dwProcessId = GetCurrentProcessId();
    DWORD dwThreadId = GetCurrentThreadId();
    HANDLE lphThreads[0x1000];
    SIZE_T count;

    suspend_other_threads(dwProcessId, dwThreadId, lphThreads, _countof(lphThreads), &count);

    HMODULE hm = GetModuleHandle(NULL);
    iat_hook(hm, "LoadLibraryExA", (LPVOID)&fpLoadLibraryExA, LoadLibraryExA_hook);
    iat_hook(hm, "LoadLibraryExW", (LPVOID)&fpLoadLibraryExW, LoadLibraryExW_hook);

    HMODULE hwu = GetModuleHandle(get_wuauservdll());
    if (hwu && PatchWUA(hwu))
        trace(L"Successfully patched previously loaded WUA module!");
    
    resume_and_close_threads(lphThreads, count);

    WaitForSingleObject(hEvent, INFINITE);
    trace(L"Unloading...");

    suspend_other_threads(dwProcessId, dwThreadId, lphThreads, _countof(lphThreads), &count);

    iat_hook(hm, "LoadLibraryExA", NULL, fpLoadLibraryExA);
    iat_hook(hm, "LoadLibraryExW", NULL, fpLoadLibraryExW);

    resume_and_close_threads(lphThreads, count);

    trace(L"Bye bye!");
    CloseHandle(hEvent);
    FreeLibraryAndExitThread(HINST_THISCOMPONENT, 0);
}

HMODULE WINAPI LoadLibraryExA_hook(
    _In_       LPCSTR  lpFileName,
    _Reserved_ HANDLE  hFile,
    _In_       DWORD   dwFlags
) {
    HMODULE result = LoadLibraryExA(lpFileName, hFile, dwFlags);
    if (result) {
        trace(L"Loaded library: %S", lpFileName);
        if (!_stricmp(lpFileName, get_wuauservdllA()) && PatchWUA(result))
            trace(L"Successfully patched WUA module!");
    }
    return result;
}

HMODULE WINAPI LoadLibraryExW_hook(
    _In_       LPCWSTR lpFileName,
    _Reserved_ HANDLE  hFile,
    _In_       DWORD   dwFlags
) {
    HMODULE result = LoadLibraryExW(lpFileName, hFile, dwFlags);
    if (result) {
        trace(L"Loaded library: %s", lpFileName);
        if (!_wcsicmp(lpFileName, get_wuauservdllW()) && PatchWUA(result))
            trace(L"Successfully patched WUA module!");
    }
    return result;
};

BOOL PatchWUA(HMODULE hModule) {
    LPSTR pattern;
    SIZE_T offset00, offset01;
#ifdef _AMD64_
    pattern = "FFF3 4883EC?? 33DB 391D???????? 7508 8B05????????";
    offset00 = 10;
    offset01 = 18;
#elif defined(_X86_)
    if (IsWindows7()) {
        pattern = "833D????????00 743E E8???????? A3????????";
        offset00 = 2;
        offset01 = 15;
    } else if (IsWindows8Point1()) {
        pattern = "8BFF 51 833D????????00 7507 A1????????";
        offset00 = 5;
        offset01 = 13;
    }
#endif

    MODULEINFO modinfo;
    GetModuleInformation(GetCurrentProcess(), hModule, &modinfo, sizeof(MODULEINFO));

    LPBYTE ptr = patternfind(modinfo.lpBaseOfDll, modinfo.SizeOfImage, 0, pattern);
    if (!ptr) {
        trace(L"No pattern match!");
        return FALSE;
    }
    trace(L"wuaueng!IsDeviceServiceable VA: %p", ptr);
    BOOL result = FALSE;
    LPBOOL lpbFirstRun, lpbIsCPUSupportedResult;
#ifdef _AMD64_
    lpbFirstRun = (LPBOOL)(ptr + offset00 + sizeof(uint32_t) + *(uint32_t *)(ptr + offset00));
    lpbIsCPUSupportedResult = (LPBOOL)(ptr + offset01 + sizeof(uint32_t) + *(uint32_t *)(ptr + offset01));
#elif defined(_X86_)
    lpbFirstRun = (LPBOOL)(*(uintptr_t *)(ptr + offset00));
    lpbIsCPUSupportedResult = (LPBOOL)(*(uintptr_t *)(ptr + offset01));
#endif

    if (*lpbFirstRun) {
        *lpbFirstRun = FALSE;
        trace(L"Patched boolean value #1: %p = %08x", lpbFirstRun, *lpbFirstRun);
        result = TRUE;
    }
    if (!*lpbIsCPUSupportedResult) {
        *lpbIsCPUSupportedResult = TRUE;
        trace(L"Patched boolean value #2: %p = %08x", lpbIsCPUSupportedResult, *lpbIsCPUSupportedResult);
        result = TRUE;
    }
    return result;
}
