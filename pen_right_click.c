// pen_right_click.c
// Monitors pen barrel button via Raw Input, writes state to shared memory,
// and injects pen_eraser_hook.dll into Clip Studio Paint so that holding
// the barrel button simulates eraser (tail-switch) mode.
//
// Compile: cl /O2 /W4 pen_right_click.c user32.lib shell32.lib
//          (pen_eraser_hook.dll must sit next to the exe)

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#include <windows.h>
#include <shellapi.h>

#define PEN_USAGE_PAGE    13
#define PEN_USAGE          2
#define PEN_BARREL_BUTTON  8

#include <shlobj.h>

#define WM_TRAYICON       (WM_USER + 1)
#define IDM_EXIT           1001
#define IDM_STARTUP_ON     1002
#define IDM_STARTUP_OFF    1003

#define STARTUP_REG_KEY    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define STARTUP_REG_VALUE  L"PenEraser"

// ---------- shared memory for barrel state ----------
typedef struct {
    volatile LONG barrelPressed;
} SharedState;

static SharedState *g_shared = NULL;
static HANDLE       g_hMap   = NULL;

// ---------- hook into CSP ----------
static HHOOK   g_hook       = NULL;
static HMODULE g_hookDll    = NULL;
static DWORD   g_cspThreadId = 0;

// ---------- other globals ----------
static int lastState = 0;
static NOTIFYICONDATAW nid = {0};

// ---------- shared memory ----------
static BOOL CreateSharedMem(void) {
    g_hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                0, sizeof(SharedState), L"PenEraserSharedMem");
    if (!g_hMap) return FALSE;
    g_shared = (SharedState *)MapViewOfFile(g_hMap, FILE_MAP_WRITE, 0, 0, sizeof(SharedState));
    if (!g_shared) { CloseHandle(g_hMap); g_hMap = NULL; return FALSE; }
    g_shared->barrelPressed = 0;
    return TRUE;
}

static void DestroySharedMem(void) {
    if (g_shared) { UnmapViewOfFile((void *)g_shared); g_shared = NULL; }
    if (g_hMap)   { CloseHandle(g_hMap); g_hMap = NULL; }
}

// ---------- CSP hook injection ----------
static BOOL FindAndHookCSP(void) {
    // Try common CSP window classes/titles
    HWND cspWnd = FindWindowW(NULL, L"CLIP STUDIO PAINT");
    if (!cspWnd) cspWnd = FindWindowW(L"HwndWrapper", NULL);  // WPF-based CSP
    if (!cspWnd) return FALSE;

    g_cspThreadId = GetWindowThreadProcessId(cspWnd, NULL);
    if (!g_cspThreadId) return FALSE;

    // Load our hook DLL
    // Get the directory of our own exe
    WCHAR dllPath[MAX_PATH];
    GetModuleFileNameW(NULL, dllPath, MAX_PATH);
    WCHAR *lastSlash = wcsrchr(dllPath, L'\\');
    if (lastSlash) *(lastSlash + 1) = 0;
    wcscat_s(dllPath, MAX_PATH, L"pen_eraser_hook.dll");

    g_hookDll = LoadLibraryW(dllPath);
    if (!g_hookDll) return FALSE;

    typedef LRESULT (CALLBACK *HOOKPROC_T)(int, WPARAM, LPARAM);
    HOOKPROC_T proc = (HOOKPROC_T)GetProcAddress(g_hookDll, "MsgHookProc");
    if (!proc) { FreeLibrary(g_hookDll); g_hookDll = NULL; return FALSE; }

    g_hook = SetWindowsHookExW(WH_GETMESSAGE, (HOOKPROC)proc, g_hookDll, g_cspThreadId);
    if (!g_hook) { FreeLibrary(g_hookDll); g_hookDll = NULL; return FALSE; }

    // Force the hook DLL to load in CSP by posting a benign message
    PostThreadMessageW(g_cspThreadId, WM_NULL, 0, 0);
    return TRUE;
}

static void UnhookCSP(void) {
    if (g_hook)    { UnhookWindowsHookEx(g_hook); g_hook = NULL; }
    if (g_hookDll) { FreeLibrary(g_hookDll); g_hookDll = NULL; }
    g_cspThreadId = 0;
}

// ---------- tray icon ----------
static void AddTrayIcon(HWND hwnd, HINSTANCE hInstance) {
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIconW(hInstance, L"APPICON");
    if (!nid.hIcon)
        nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    lstrcpyW(nid.szTip, L"Pen Eraser");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void RemoveTrayIcon(void) {
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// ---------- startup registry ----------
static BOOL IsStartupEnabled(void) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return FALSE;
    DWORD type, size = 0;
    BOOL exists = (RegQueryValueExW(hKey, STARTUP_REG_VALUE, NULL, &type, NULL, &size) == ERROR_SUCCESS);
    RegCloseKey(hKey);
    return exists;
}

static void SetStartupEnabled(BOOL enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_REG_KEY, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS)
        return;
    if (enable) {
        WCHAR exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        RegSetValueExW(hKey, STARTUP_REG_VALUE, 0, REG_SZ,
                       (BYTE *)exePath, (DWORD)((wcslen(exePath) + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(hKey, STARTUP_REG_VALUE);
    }
    RegCloseKey(hKey);
}

static void ShowContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();

    // CSP connection status (disabled label)
    if (g_hook)
        AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_CHECKED, 0, L"CSP: Connected");
    else
        AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, L"CSP: Not connected");

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    // Startup toggle
    BOOL startup = IsStartupEnabled();
    if (startup)
        AppendMenuW(menu, MF_STRING, IDM_STARTUP_OFF, L"Disable auto-start");
    else
        AppendMenuW(menu, MF_STRING, IDM_STARTUP_ON, L"Enable auto-start");

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// ---------- timer: retry CSP hook if not yet connected ----------
#define IDT_RETRY_HOOK  2001

// ---------- window proc ----------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_INPUT) {
        UINT size = 0;
        GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER));

        if (size > 0) {
            BYTE *buf = (BYTE *)_alloca(size);
            if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == size) {
                RAWINPUT *raw = (RAWINPUT *)buf;

                if (raw->header.dwType == RIM_TYPEHID && raw->data.hid.dwCount > 0 && raw->data.hid.dwSizeHid > 0) {
                    DWORD val = 0;
                    for (DWORD i = 0; i < raw->data.hid.dwSizeHid && i < 4; i++) {
                        val |= ((DWORD)raw->data.hid.bRawData[i]) << (i * 8);
                    }

                    int proc = (val >> 8) & 0x1F;
                    if (proc != lastState) {
                        // Check barrel bit, not exact match — other bits
                        // (tip, invert, etc.) can be set simultaneously
                        BOOL barrelNow  = (proc & PEN_BARREL_BUTTON) != 0;
                        BOOL barrelPrev = (lastState & PEN_BARREL_BUTTON) != 0;
                        if (barrelNow != barrelPrev && g_shared)
                            InterlockedExchange(&g_shared->barrelPressed, barrelNow ? 1 : 0);
                        lastState = proc;
                    }
                }
            }
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    if (msg == WM_TIMER && wParam == IDT_RETRY_HOOK) {
        if (g_hook) {
            // Check if CSP is still running
            HWND cspWnd = FindWindowW(NULL, L"CLIP STUDIO PAINT");
            if (!cspWnd)
                UnhookCSP();
        } else {
            FindAndHookCSP();
        }
        return 0;
    }

    if (msg == WM_TRAYICON) {
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)
            ShowContextMenu(hwnd);
        return 0;
    }

    if (msg == WM_COMMAND) {
        switch (LOWORD(wParam)) {
        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;
        case IDM_STARTUP_ON:
            SetStartupEnabled(TRUE);
            break;
        case IDM_STARTUP_OFF:
            SetStartupEnabled(FALSE);
            break;
        }
        return 0;
    }

    if (msg == WM_DESTROY) {
        KillTimer(hwnd, IDT_RETRY_HOOK);
        UnhookCSP();
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR cmdLine, int cmdShow) {
    (void)hPrev; (void)cmdLine; (void)cmdShow;

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"PenEraserMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 1;

    if (!CreateSharedMem()) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"PenEraser";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, L"PenEraser", L"Pen Eraser", 0,
                                 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
    if (!hwnd) {
        DestroySharedMem();
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }

    // Register for pen digitizer raw input
    RAWINPUTDEVICE rid = {0};
    rid.usUsagePage = PEN_USAGE_PAGE;
    rid.usUsage     = PEN_USAGE;
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        DestroyWindow(hwnd);
        DestroySharedMem();
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }

    AddTrayIcon(hwnd, hInstance);

    // Try hooking CSP now; poll every 3s for connect/disconnect
    FindAndHookCSP();
    SetTimer(hwnd, IDT_RETRY_HOOK, 3000, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroySharedMem();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
