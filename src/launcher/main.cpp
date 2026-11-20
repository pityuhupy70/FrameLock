// FrameLock launcher: pick a game, inject the hook DLL, set FPS/output, Record/Stop.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <commdlg.h>
#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <cmath>
#include <cwchar>
#include "../shared.h"

#ifdef _WIN64
#define DLL_NAME L"FrameLockHook64.dll"
#define APP_TITLE L"FrameLock Recorder (64-bit games)"
#else
#define DLL_NAME L"FrameLockHook32.dll"
#define APP_TITLE L"FrameLock Recorder (32-bit games)"
#endif

enum { IDC_PROC = 101, IDC_REFRESH, IDC_LAUNCH, IDC_FPS, IDC_OUT, IDC_BROWSE, IDC_ATTACH, IDC_RECORD, IDC_STATUS, IDC_REALTIME, IDC_TEST };
static HWND g_wnd, hProc, hFps, hOut, hRecord, hStatus, hRealtime;
static std::vector<DWORD> g_pids;
static DWORD g_pid = 0; static HANDLE g_hProcess = nullptr;
static HANDLE g_map = nullptr; static FLShared* g_sh = nullptr;
static DWORD g_launchPid = 0; static ULONGLONG g_launchTime = 0;

static void Msg(const wchar_t* t) { MessageBoxW(g_wnd, t, L"FrameLock", MB_OK | MB_ICONINFORMATION); }

struct WinInfo { std::map<DWORD, std::wstring>* m; };
static BOOL CALLBACK EnumWinProc(HWND w, LPARAM lp) {
    auto m = (std::map<DWORD, std::wstring>*)lp;
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    wchar_t t[256]; if (!GetWindowTextW(w, t, 256) || !t[0]) return TRUE;
    DWORD pid; GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() && !m->count(pid)) (*m)[pid] = t;
    return TRUE;
}
static void RefreshProcs() {
    std::map<DWORD, std::wstring> wins; EnumWindows(EnumWinProc, (LPARAM)&wins);
    SendMessageW(hProc, CB_RESETCONTENT, 0, 0); g_pids.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe)) do {
        auto it = wins.find(pe.th32ProcessID); if (it == wins.end()) continue;
        wchar_t line[512]; swprintf_s(line, L"%s  (PID %lu)  -  %s", pe.szExeFile, pe.th32ProcessID, it->second.c_str());
        SendMessageW(hProc, CB_ADDSTRING, 0, (LPARAM)line); g_pids.push_back(pe.th32ProcessID);
    } while (Process32NextW(snap, &pe));
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
}
static bool ParseFps(const wchar_t* s, LONG& num, LONG& den) {
    if (const wchar_t* sl = wcschr(s, L'/')) { num = _wtol(s); den = _wtol(sl + 1); return num > 0 && den > 0; }
    double v = _wtof(s); if (v <= 0 || v > 1000) return false;
    struct { double v; LONG n, d; } sp[] = { {23.976,24000,1001},{29.97,30000,1001},{47.952,48000,1001},{59.94,60000,1001},{119.88,120000,1001} };
    for (auto& x : sp) if (fabs(v - x.v) < 0.002) { num = x.n; den = x.d; return true; }
    num = (LONG)llround(v * 1000); den = 1000;
    LONG a = num, b = den; while (b) { LONG t = a % b; a = b; b = t; }
    num /= a; den /= a; return true;
}
static bool OpenShared(DWORD pid) {
    if (g_sh) { UnmapViewOfFile(g_sh); g_sh = nullptr; } if (g_map) { CloseHandle(g_map); g_map = nullptr; }
    wchar_t name[64]; swprintf_s(name, FL_SHARED_FMT, pid);
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(FLShared), name);
    bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    if (!g_map) return false;
    g_sh = (FLShared*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FLShared));
    if (!g_sh) return false;
    if (!existed || g_sh->magic != FL_MAGIC) { ZeroMemory(g_sh, sizeof(FLShared)); g_sh->magic = FL_MAGIC; g_sh->fpsNum = 24; g_sh->fpsDen = 1; }
    return true;
}
static bool Inject(DWORD pid, std::wstring& err) {
    wchar_t dll[MAX_PATH]; GetModuleFileNameW(nullptr, dll, MAX_PATH);
    wchar_t* slash = wcsrchr(dll, L'\\'); if (slash) *(slash + 1) = 0; wcscat_s(dll, DLL_NAME);
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) { err = L"Hook DLL not found next to the launcher: " DLL_NAME; return false; }
    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
    if (!p) { err = L"Cannot open the game process. Run FrameLock as Administrator."; return false; }
    BOOL tw = FALSE, sw = FALSE; IsWow64Process(p, &tw); IsWow64Process(GetCurrentProcess(), &sw);
    if (tw != sw) { CloseHandle(p); err = sw ? L"This game is 64-bit: use FrameLock64.exe." : L"This game is 32-bit: use FrameLock32.exe."; return false; }
    size_t bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(p, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem || !WriteProcessMemory(p, mem, dll, bytes, nullptr)) { CloseHandle(p); err = L"Writing to the game process failed."; return false; }
    auto ll = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE t = CreateRemoteThread(p, nullptr, 0, ll, mem, 0, nullptr);
    if (!t) { VirtualFreeEx(p, mem, 0, MEM_RELEASE); CloseHandle(p); err = L"CreateRemoteThread failed (anti-cheat or protected process?)."; return false; }
    WaitForSingleObject(t, 15000); DWORD code = 0; GetExitCodeThread(t, &code); CloseHandle(t);
    VirtualFreeEx(p, mem, 0, MEM_RELEASE);
    if (!code) { CloseHandle(p); err = L"LoadLibrary failed inside the game (blocked by anti-cheat?)."; return false; }
    if (g_hProcess) CloseHandle(g_hProcess); g_hProcess = p;
    return true;
}
static void Attach(DWORD pid) {
    if (!OpenShared(pid)) { Msg(L"Shared memory creation failed."); return; }
    std::wstring err;
    if (!Inject(pid, err)) { Msg(err.c_str()); return; }
    g_pid = pid;
}
static bool ApplySettings() {
    wchar_t fps[64]; GetWindowTextW(hFps, fps, 64);
    LONG n, d; if (!ParseFps(fps, n, d)) { Msg(L"Invalid FPS. Examples: 24, 30, 60, 23.976, 60000/1001"); return false; }
    wchar_t out[MAX_PATH]; GetWindowTextW(hOut, out, MAX_PATH);
    if (!out[0]) { Msg(L"Choose an output folder."); return false; }
    int r = SHCreateDirectoryExW(nullptr, out, nullptr);
    if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS) { Msg(L"Cannot create the output folder."); return false; }
    g_sh->fpsNum = n; g_sh->fpsDen = d; wcsncpy_s(g_sh->outDir, out, _TRUNCATE);
    InterlockedExchange(&g_sh->mode, SendMessageW(hRealtime, BM_GETCHECK, 0, 0) == BST_CHECKED ? FL_MODE_REALTIME : FL_MODE_LOCKED);
    return true;
}
static void ToggleRecord() {
    if (!g_sh || !g_pid) { Msg(L"Attach to a game first."); return; }
    if (!g_sh->hooksReady) { Msg(L"Hooks are not ready yet. Wait a second and try again."); return; }
    if (!g_sh->wantRecord) { if (ApplySettings()) InterlockedExchange(&g_sh->wantRecord, 1); }
    else InterlockedExchange(&g_sh->wantRecord, 0);
}
static void StartTest() {
    if (!g_sh || !g_pid) { Msg(L"Attach to a game first."); return; }
    if (!g_sh->hooksReady) { Msg(L"Hooks are not ready yet. Wait a second and try again."); return; }
    if (g_sh->wantRecord || g_sh->isRecording) { Msg(L"Stop the current recording first."); return; }
    if (ApplySettings()) { InterlockedExchange(&g_sh->testFrames, 120); InterlockedExchange(&g_sh->wantRecord, 1); }
}
static BOOL CALLBACK FindPidWin(HWND w, LPARAM lp) {
    DWORD pid; GetWindowThreadProcessId(w, &pid);
    if (pid == g_launchPid && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) { *(bool*)lp = true; return FALSE; }
    return TRUE;
}
static void LaunchGame() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW of = { sizeof(of) }; of.hwndOwner = g_wnd; of.lpstrFilter = L"Game executable\0*.exe\0"; of.lpstrFile = file; of.nMaxFile = MAX_PATH; of.Flags = OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&of)) return;
    wchar_t dir[MAX_PATH]; wcscpy_s(dir, file); if (wchar_t* s = wcsrchr(dir, L'\\')) *s = 0;
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    std::wstring cmd = L"\"" + std::wstring(file) + L"\"";
    if (!CreateProcessW(file, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, dir, &si, &pi)) { Msg(L"Could not start the game."); return; }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    g_launchPid = pi.dwProcessId; g_launchTime = GetTickCount64();
}
static void UpdateStatus() {
    if (g_launchPid) {
        bool found = false; EnumWindows(FindPidWin, (LPARAM)&found);
        if (found && GetTickCount64() - g_launchTime > 3000) { DWORD p = g_launchPid; g_launchPid = 0; Attach(p); }
        else if (GetTickCount64() - g_launchTime > 120000) { g_launchPid = 0; Msg(L"Game window not found. If it uses a launcher, start it normally and use Attach."); }
    }
    wchar_t txt[2048];
    if (g_hProcess && WaitForSingleObject(g_hProcess, 0) == WAIT_OBJECT_0) { CloseHandle(g_hProcess); g_hProcess = nullptr; g_pid = 0; }
    if (!g_sh || !g_pid) { swprintf_s(txt, L"Not attached. Start your game, select it above and press Attach.%s", g_launchPid ? L"\r\nWaiting for launched game window..." : L""); }
    else {
        swprintf_s(txt, L"Attached to PID %lu   |   Hooks: %s   |   API: %s   |   Resolution: %ld x %ld\r\n"
            L"State: %s (%s)   |   Frames queued: %lld   saved: %lld   |   Clock slips: %lld   |   Last HRESULT: 0x%08X\r\nTake folder: %s\r\n%s",
            g_pid, g_sh->hooksReady ? L"active" : L"loading", g_sh->api[0] ? g_sh->api : L"(waiting for first frame)", g_sh->width, g_sh->height,
            g_sh->isRecording ? L"RECORDING" : L"idle", g_sh->mode == FL_MODE_REALTIME ? L"real-time" : L"locked clock", (long long)g_sh->framesQueued, (long long)g_sh->framesSaved, (long long)g_sh->slipEvents, (unsigned)g_sh->lastHr, g_sh->takeDir, g_sh->status);
    }
    wchar_t old[2048]; GetWindowTextW(hStatus, old, 2048); if (wcscmp(old, txt)) SetWindowTextW(hStatus, txt);
    const wchar_t* lbl = (g_sh && (g_sh->wantRecord || g_sh->isRecording)) ? L"\u25A0  STOP  (F9)" : L"\u25CF  RECORD  (F9)";
    wchar_t cur[64]; GetWindowTextW(hRecord, cur, 64); if (wcscmp(cur, lbl)) SetWindowTextW(hRecord, lbl);
}
static HWND Ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(cls == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}
static LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        g_wnd = w;
        Ctl(L"STATIC", L"Game:", 0, 12, 16, 90, 20, 0);
        hProc = Ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 105, 12, 400, 300, IDC_PROC);
        Ctl(L"BUTTON", L"Refresh", WS_TABSTOP, 512, 11, 80, 24, IDC_REFRESH);
        Ctl(L"BUTTON", L"Attach", WS_TABSTOP, 598, 11, 80, 24, IDC_ATTACH);
        Ctl(L"BUTTON", L"Launch game .exe and attach...", WS_TABSTOP, 105, 42, 250, 24, IDC_LAUNCH);
        Ctl(L"STATIC", L"FPS:", 0, 12, 80, 90, 20, 0);
        hFps = Ctl(L"EDIT", L"24", ES_AUTOHSCROLL | WS_TABSTOP, 105, 77, 120, 24, IDC_FPS);
        Ctl(L"STATIC", L"(24, 30, 60, 23.976, 60000/1001 ...)   JPEG quality: 100% (4:4:4), native resolution", 0, 235, 80, 450, 20, 0);
        Ctl(L"STATIC", L"Output folder:", 0, 12, 114, 90, 20, 0);
        wchar_t def[MAX_PATH]; GetEnvironmentVariableW(L"USERPROFILE", def, MAX_PATH); wcscat_s(def, L"\\Videos\\FrameLock");
        hOut = Ctl(L"EDIT", def, ES_AUTOHSCROLL | WS_TABSTOP, 105, 111, 487, 24, IDC_OUT);
        Ctl(L"BUTTON", L"Browse...", WS_TABSTOP, 598, 111, 80, 24, IDC_BROWSE);
        hRecord = Ctl(L"BUTTON", L"\u25CF  RECORD  (F9)", WS_TABSTOP, 105, 148, 380, 44, IDC_RECORD);
        Ctl(L"BUTTON", L"Self-test: 120 frames (F10)", WS_TABSTOP, 492, 148, 186, 44, IDC_TEST);
        hRealtime = Ctl(L"BUTTON", L"Real-time capture (do not lock the game clock; saves only frames the game actually renders)", BS_AUTOCHECKBOX | WS_TABSTOP, 105, 198, 573, 20, IDC_REALTIME);
        hStatus = Ctl(L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL, 12, 226, 666, 110, IDC_STATUS);
        RefreshProcs(); SetTimer(w, 1, 250, nullptr);
        return 0; }
    case WM_TIMER: UpdateStatus(); return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_REFRESH: RefreshProcs(); break;
        case IDC_ATTACH: { int i = (int)SendMessageW(hProc, CB_GETCURSEL, 0, 0); if (i < 0 || i >= (int)g_pids.size()) Msg(L"Select the game process first."); else Attach(g_pids[i]); break; }
        case IDC_LAUNCH: LaunchGame(); break;
        case IDC_RECORD: ToggleRecord(); break;
        case IDC_TEST: StartTest(); break;
        case IDC_BROWSE: {
            BROWSEINFOW bi = {}; bi.hwndOwner = w; bi.lpszTitle = L"Output folder for frames"; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) { wchar_t p[MAX_PATH]; if (SHGetPathFromIDListW(pidl, p)) SetWindowTextW(hOut, p); CoTaskMemFree(pidl); }
            break; }
        }
        return 0;
    case WM_DESTROY:
        if (g_sh && g_sh->wantRecord) InterlockedExchange(&g_sh->wantRecord, 0);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc = { sizeof(wc) }; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"FrameLockMain";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, APP_TITLE, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 706, 390, nullptr, nullptr, hi, nullptr);
    ShowWindow(w, show);
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0)) { if (!IsDialogMessageW(w, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    return 0;
}
