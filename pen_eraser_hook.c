// pen_eraser_hook.c
// DLL injected into CSP via SetWindowsHookEx.
// Hooks GetPointerPenInfo to replace barrel-button with eraser/invert flags.
//
// Compile: cl /O2 /LD pen_eraser_hook.c user32.lib /Fe:pen_eraser_hook.dll

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0602
#define WINVER       0x0602
#include <windows.h>
#include <tlhelp32.h>

// ---------- shared memory layout (read-only from DLL side) ----------
typedef struct {
    volatile LONG barrelPressed;
} SharedState;

static SharedState *g_shared  = NULL;
static HANDLE       g_hMap    = NULL;
static HMODULE      g_hSelf   = NULL;

// ---------- pen flag constants ----------
#define MY_PEN_FLAG_BARREL   0x0001
#define MY_PEN_FLAG_INVERTED 0x0002
#define MY_PEN_FLAG_ERASER   0x0020

#ifndef POINTER_FLAG_INCONTACT
#define POINTER_FLAG_INCONTACT      0x00000004
#endif
#ifndef POINTER_FLAG_SECONDBUTTON
#define POINTER_FLAG_SECONDBUTTON   0x00000020
#endif

// WM_POINTER message range
#define MY_WM_POINTERUPDATE  0x0245
#define MY_WM_POINTERDOWN    0x0246
#define MY_WM_POINTERUP      0x0247
#define MY_WM_POINTERENTER   0x0249
#define MY_WM_POINTERLEAVE   0x024A

// wParam high-word flag for secondary button
#define MY_POINTER_MESSAGE_FLAG_SECONDBUTTON 0x0020

// ---------- original function pointers ----------
typedef BOOL (WINAPI *Fn_GetPointerPenInfo)(UINT32, POINTER_PEN_INFO *);
typedef BOOL (WINAPI *Fn_GetPointerPenInfoHistory)(UINT32, UINT32 *, POINTER_PEN_INFO *);
typedef BOOL (WINAPI *Fn_GetPointerInfo)(UINT32, POINTER_INFO *);
typedef BOOL (WINAPI *Fn_GetPointerInfoHistory)(UINT32, UINT32 *, POINTER_INFO *);

static Fn_GetPointerPenInfo        g_origPenInfo     = NULL;
static Fn_GetPointerPenInfoHistory g_origPenInfoHist = NULL;
static Fn_GetPointerInfo           g_origPtrInfo     = NULL;
static Fn_GetPointerInfoHistory    g_origPtrInfoHist = NULL;

// ---------- modify a single POINTER_PEN_INFO ----------
static void PatchPenInfo(POINTER_PEN_INFO *p) {
    if (!g_shared || !g_shared->barrelPressed)
        return;

    // Pen flags: barrel → inverted/eraser
    p->penFlags &= ~MY_PEN_FLAG_BARREL;
    p->penFlags |= MY_PEN_FLAG_INVERTED;
    if (p->pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT)
        p->penFlags |= MY_PEN_FLAG_ERASER;

    // Pointer flags: remove secondary button (barrel) so CSP doesn't see right-click
    p->pointerInfo.pointerFlags &= ~POINTER_FLAG_SECONDBUTTON;

    // Clear barrel-related button change types (SECONDBUTTON_DOWN=9, UP=10)
    if (p->pointerInfo.ButtonChangeType == 9 || p->pointerInfo.ButtonChangeType == 10)
        p->pointerInfo.ButtonChangeType = 0;
}

// ---------- hooked functions ----------
static BOOL WINAPI Hook_GetPointerPenInfo(UINT32 id, POINTER_PEN_INFO *info) {
    BOOL ok = g_origPenInfo(id, info);
    if (ok && info) PatchPenInfo(info);
    return ok;
}

static BOOL WINAPI Hook_GetPointerPenInfoHistory(UINT32 id, UINT32 *cnt, POINTER_PEN_INFO *info) {
    BOOL ok = g_origPenInfoHist(id, cnt, info);
    if (ok && info && cnt) {
        for (UINT32 i = 0; i < *cnt; i++)
            PatchPenInfo(&info[i]);
    }
    return ok;
}

// ---------- patch POINTER_INFO (for GetPointerInfo calls) ----------
static void PatchPointerInfo(POINTER_INFO *p) {
    if (!g_shared || !g_shared->barrelPressed)
        return;
    p->pointerFlags &= ~POINTER_FLAG_SECONDBUTTON;
    if (p->ButtonChangeType == 9 || p->ButtonChangeType == 10)
        p->ButtonChangeType = 0;
}

static BOOL WINAPI Hook_GetPointerInfo(UINT32 id, POINTER_INFO *info) {
    BOOL ok = g_origPtrInfo(id, info);
    if (ok && info) PatchPointerInfo(info);
    return ok;
}

static BOOL WINAPI Hook_GetPointerInfoHistory(UINT32 id, UINT32 *cnt, POINTER_INFO *info) {
    BOOL ok = g_origPtrInfoHist(id, cnt, info);
    if (ok && info && cnt) {
        for (UINT32 i = 0; i < *cnt; i++)
            PatchPointerInfo(&info[i]);
    }
    return ok;
}

// ---------- IAT patching ----------
// Replaces an IAT entry whose current value == oldFunc with newFunc.
// Works across all loaded modules so we catch every call site.
static void PatchIATInModule(HMODULE hMod, void *oldFunc, void *newFunc) {
    if (!hMod) return;

    BYTE *base = (BYTE *)hMod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;

    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    DWORD impRVA  = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    DWORD impSize = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
    if (!impRVA || !impSize) return;

    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + impRVA);
    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA *thunk = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; thunk->u1.Function; thunk++) {
            if ((void *)(uintptr_t)thunk->u1.Function == oldFunc) {
                DWORD old;
                if (VirtualProtect(&thunk->u1.Function, sizeof(uintptr_t), PAGE_READWRITE, &old)) {
                    thunk->u1.Function = (uintptr_t)newFunc;
                    VirtualProtect(&thunk->u1.Function, sizeof(uintptr_t), old, &old);
                }
            }
        }
    }
}

static void PatchAllModules(void *oldFunc, void *newFunc) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;

    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            // Don't patch ourselves
            if (me.hModule == g_hSelf) continue;
            PatchIATInModule(me.hModule, oldFunc, newFunc);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
}

static void InstallHooks(void) {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (!hUser32) return;

    g_origPenInfo     = (Fn_GetPointerPenInfo)GetProcAddress(hUser32, "GetPointerPenInfo");
    g_origPenInfoHist = (Fn_GetPointerPenInfoHistory)GetProcAddress(hUser32, "GetPointerPenInfoHistory");
    g_origPtrInfo     = (Fn_GetPointerInfo)GetProcAddress(hUser32, "GetPointerInfo");
    g_origPtrInfoHist = (Fn_GetPointerInfoHistory)GetProcAddress(hUser32, "GetPointerInfoHistory");

    if (g_origPenInfo)
        PatchAllModules((void *)g_origPenInfo, (void *)Hook_GetPointerPenInfo);
    if (g_origPenInfoHist)
        PatchAllModules((void *)g_origPenInfoHist, (void *)Hook_GetPointerPenInfoHistory);
    if (g_origPtrInfo)
        PatchAllModules((void *)g_origPtrInfo, (void *)Hook_GetPointerInfo);
    if (g_origPtrInfoHist)
        PatchAllModules((void *)g_origPtrInfoHist, (void *)Hook_GetPointerInfoHistory);
}

static void RemoveHooks(void) {
    if (g_origPenInfo)
        PatchAllModules((void *)Hook_GetPointerPenInfo, (void *)g_origPenInfo);
    if (g_origPenInfoHist)
        PatchAllModules((void *)Hook_GetPointerPenInfoHistory, (void *)g_origPenInfoHist);
    if (g_origPtrInfo)
        PatchAllModules((void *)Hook_GetPointerInfo, (void *)g_origPtrInfo);
    if (g_origPtrInfoHist)
        PatchAllModules((void *)Hook_GetPointerInfoHistory, (void *)g_origPtrInfoHist);
}

// ---------- SetWindowsHookEx callback ----------
// Patches WM_POINTER message wParam to clear SECONDBUTTON flag
// so CSP doesn't interpret barrel button from the message itself.
__declspec(dllexport) LRESULT CALLBACK MsgHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && g_shared && g_shared->barrelPressed) {
        MSG *pMsg = (MSG *)lParam;
        UINT m = pMsg->message;
        if (m >= MY_WM_POINTERUPDATE && m <= MY_WM_POINTERLEAVE) {
            // Clear SECONDBUTTON from the high word of wParam
            WORD flags = HIWORD(pMsg->wParam);
            if (flags & MY_POINTER_MESSAGE_FLAG_SECONDBUTTON) {
                flags &= ~MY_POINTER_MESSAGE_FLAG_SECONDBUTTON;
                pMsg->wParam = MAKEWPARAM(LOWORD(pMsg->wParam), flags);
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// ---------- DllMain ----------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hSelf = hModule;
        DisableThreadLibraryCalls(hModule);

        g_hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, L"PenEraserSharedMem");
        if (!g_hMap) return TRUE;   // shared mem not available = do nothing

        g_shared = (SharedState *)MapViewOfFile(g_hMap, FILE_MAP_READ, 0, 0, sizeof(SharedState));
        if (!g_shared) {
            CloseHandle(g_hMap);
            g_hMap = NULL;
            return TRUE;
        }

        InstallHooks();
    }
    else if (reason == DLL_PROCESS_DETACH) {
        RemoveHooks();
        if (g_shared) { UnmapViewOfFile((void *)g_shared); g_shared = NULL; }
        if (g_hMap)   { CloseHandle(g_hMap); g_hMap = NULL; }
    }
    return TRUE;
}
