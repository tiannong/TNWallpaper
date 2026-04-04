// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 动态壁纸 - 多屏幕动态壁纸工具
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 功能特性：
//   ✓ 支持多显示器，按位置稳定排序
//   ✓ 视频壁纸（通过 ffplay 播放，崩溃自动重启）
//   ✓ 嵌入任意窗口到桌面
//   ✓ 系统托盘图标（右键菜单：显示/停止/退出）
//   ✓ 最小化到托盘而非任务栏
//   ✓ 统一的 WorkerW 查找逻辑
//   ✓ 正确清理钩子
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

#include <Windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <shellapi.h>
#include <string>
#include <algorithm>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")

// ── 资源标识 ────────────────────────────────────────────────────────
#define IDI_APP_ICON          101
#define IDC_BTN_SELECT_VIDEO  1001
#define IDC_BTN_SELECT_WINDOW 1002
#define IDC_BTN_STOP          1003
#define IDC_BTN_EXIT          1004
#define IDC_STATUS            1005
#define IDC_MONITOR_COMBO     1006
#define IDC_TITLE             1007
#define WM_TRAYICON           (WM_USER + 1)
#define IDM_TRAY_SHOW         2001
#define IDM_TRAY_HIDE         2002
#define IDM_TRAY_EXIT         2003

#define MONITOR_ALL  -1
#define MAX_PLAYERS  8

// ── 全局变量 ────────────────────────────────────────────────────────
static HWND       g_hMainWnd = NULL;
static HWND       g_hStatus = NULL;
static HWND       g_hMonitorCombo = NULL;
static HWND       g_hTitle = NULL;
static HWND       g_embeddedWindow = NULL;
static bool       g_playing = false;
static bool       g_embedding = false;
static bool       g_isVideoMode = true;
static std::wstring g_videoPath;
static DWORD      g_originalStyle = 0;
static LONG_PTR   g_originalExStyle = 0;
static HWND       g_originalParent = NULL;
static RECT       g_originalRect = { 0 };
static int        g_selectedMonitor = 0;

// ── 界面资源 ────────────────────────────────────────────────────────
static HFONT      g_hFontTitle = NULL;
static HFONT      g_hFontNormal = NULL;
static HFONT      g_hFontStatus = NULL;
static HBRUSH     g_hBrushBg = NULL;
static HBRUSH     g_hBrushBtnPrimary = NULL;
static HBRUSH     g_hBrushBtnSecondary = NULL;
static HBRUSH     g_hBrushBtnHover = NULL;
static bool       g_darkMode = false;
static HWND       g_hHoverBtn = NULL;

// ── 播放器进程 ──────────────────────────────────────────────────────
static PROCESS_INFORMATION g_playerProcs[MAX_PLAYERS] = { 0 };
static HWND     g_playerWindows[MAX_PLAYERS] = { 0 };
static RECT     g_playerTargetRects[MAX_PLAYERS] = { 0 };
static int      g_playerCount = 0;
static UINT_PTR g_resizeTimer = 0;
static UINT_PTR g_watchdogTimer = 0;
static UINT_PTR g_deferredRefreshTimer = 0;

// ── 窗口选择状态 ────────────────────────────────────────────────────
static HWND  g_hHoveredWindow = NULL;
static HWND  g_selectedWindow = NULL;
static bool  g_selectingWindow = false;
static HHOOK g_mouseHook = NULL;
static HHOOK g_keyHook = NULL;
static RECT  g_lastHighlightRect = { 0 };
static HBRUSH g_hNullBrush = NULL;

// ── 系统托盘 ────────────────────────────────────────────────────────
static NOTIFYICONDATAW g_nid = { 0 };
static HMENU g_hTrayMenu = NULL;

// ── 函数声明 ────────────────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK ButtonProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
bool PlayVideo(const std::wstring& videoPath);
void StopVideo();
bool SelectVideoFile(std::wstring& outPath);
void EmbedWindowToDesktop(HWND hwnd, const RECT* targetRect);
std::wstring GetExeDir();
void RestoreEmbeddedWindow();
void StartWindowSelection();
void UnhookWindowSelection();
void UpdateStatus(const wchar_t* text);
void RefreshDesktop();
bool IsSystemDarkMode();
void InitFonts();
void InitBrushes();
void InitTrayIcon();
void RemoveTrayIcon();
RECT GetMonitorRect(int monitorIndex);
std::vector<RECT> EnumerateMonitors();
HWND FindDesktopWorkerW();
void RestartDeadPlayers();

// ── 显示器枚举 ──────────────────────────────────────────────────────

std::vector<RECT> EnumerateMonitors()
{
    std::vector<RECT> monitors;
    EnumDisplayMonitors(NULL, NULL, [](HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam) -> BOOL {
        std::vector<RECT>* pMons = (std::vector<RECT>*)lParam;
        MONITORINFOEX mi;
        mi.cbSize = sizeof(mi);
        GetMonitorInfo(hMonitor, &mi);
        pMons->push_back(mi.rcMonitor);
        return TRUE;
        }, (LPARAM)&monitors);

    // 排序：主显示器优先（0,0），然后按左坐标排列
    std::sort(monitors.begin(), monitors.end(), [](const RECT& a, const RECT& b) {
        bool aPrimary = (a.left == 0 && a.top == 0);
        bool bPrimary = (b.left == 0 && b.top == 0);
        if (aPrimary && !bPrimary) return true;
        if (!aPrimary && bPrimary) return false;
        if (a.left != b.left) return a.left < b.left;
        return a.top < b.top;
        });

    return monitors;
}

RECT GetMonitorRect(int monitorIndex)
{
    auto monitors = EnumerateMonitors();
    if (monitorIndex >= 0 && monitorIndex < (int)monitors.size())
        return monitors[monitorIndex];
    return { 0, 0, 1920, 1080 };
}

// ── 查找桌面 WorkerW ───────────────────────────────────────────────

// 查找桌面窗口（精简版系统兼容版）
// 精简版系统兼容的 FindDesktopWorkerW
HWND FindDesktopWorkerW() {
    // 先尝试标准方法
    HWND hProgman = FindWindowW(L"Progman", NULL);
    if (hProgman) {
        SendMessageW(hProgman, 0x052C, 0, 0);
    }

    HWND hWorkerW = NULL;
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        HWND hDefView = FindWindowExW(hwnd, NULL, L"SHELLDLL_DefView", NULL);
        if (hDefView) {
            HWND hBg = FindWindowExW(NULL, hwnd, L"WorkerW", NULL);
            if (hBg) {
                *(HWND*)lParam = hBg;
                return FALSE;
            }
        }
        return TRUE;
        }, (LPARAM)&hWorkerW);

    if (hWorkerW) return hWorkerW;

    // 精简版系统回退
    if (hProgman) {
        SendMessageW(hProgman, 0x052C, 0xD, 0);
        SendMessageW(hProgman, 0x052C, 0xD, 1);
        Sleep(100);

        hWorkerW = FindWindowW(L"WorkerW", NULL);
        if (hWorkerW) return hWorkerW;

        hWorkerW = FindWindowExW(hProgman, NULL, L"WorkerW", NULL);
        if (hWorkerW) return hWorkerW;

        return hProgman; // 终极回退
    }

    return GetDesktopWindow();
}



// ── 工具函数 ────────────────────────────────────────────────────────

bool IsSystemDarkMode()
{
    DWORD value = 1;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD size = sizeof(DWORD);
        RegQueryValueExW(hKey, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&value, &size);
        RegCloseKey(hKey);
    }
    return value == 0;
}

void UpdateStatus(const wchar_t* text)
{
    if (g_hStatus) SetWindowTextW(g_hStatus, text);
}

std::wstring GetExeDir()
{
    WCHAR path[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, path, MAX_PATH);
    for (int i = lstrlenW(path); i >= 0; --i) {
        if (path[i] == L'\\') { path[i + 1] = L'\0'; break; }
    }
    return path;
}

bool SelectVideoFile(std::wstring& outPath)
{
    OPENFILENAMEW ofn = { 0 };
    WCHAR szFile[MAX_PATH] = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"视频文件\0*.mp4;*.avi;*.mkv;*.mov;*.wmv;*.webm;*.flv\0所有文件\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    ofn.lpstrTitle = L"选择视频文件";
    if (GetOpenFileNameW(&ofn)) { outPath = szFile; return true; }
    return false;
}

// ── 窗口嵌入/恢复 ────────────────────────────────────────────────────

void EmbedWindowToDesktop(HWND hwnd, const RECT* tr) {
    if (!hwnd) return;

    g_originalStyle = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
    g_originalExStyle = (LONG_PTR)GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    g_originalParent = GetParent(hwnd);
    GetWindowRect(hwnd, &g_originalRect);
    g_embeddedWindow = hwnd;

    HWND hDesktop = FindDesktopWorkerW();
    if (!hDesktop) return;

    // 获取目标矩形（相对于屏幕）
    RECT target = *tr;

    // 如果是 Progman 或 DesktopWindow，需要特殊处理
    WCHAR className[256] = { 0 };
    GetClassNameW(hDesktop, className, 256);
    bool isProgman = (wcscmp(className, L"Progman") == 0);

    ShowWindow(hwnd, SW_HIDE);
    SetParent(hwnd, hDesktop);

    if (isProgman) {
        // Progman 作为父窗口时，需要设置 WS_CHILD 并去掉 WS_CAPTION
        SetWindowLongPtr(hwnd, GWL_STYLE, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN);
    }
    else {
        SetWindowLongPtr(hwnd, GWL_STYLE, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
    }

    SetWindowLongPtr(hwnd, GWL_EXSTYLE, 0);
    ShowWindow(hwnd, SW_SHOW);

    // 对于 Progman，直接使用屏幕坐标
    if (isProgman) {
        MoveWindow(hwnd, target.left, target.top,
            target.right - target.left, target.bottom - target.top, TRUE);
    }
    else {
        RECT wr;
        GetWindowRect(hDesktop, &wr);
        MoveWindow(hwnd, target.left - wr.left, target.top - wr.top,
            target.right - target.left, target.bottom - target.top, TRUE);
    }

    UpdateWindow(hwnd);
    g_embedding = true;
}


void RestoreEmbeddedWindow()
{
    if (!g_embedding) return;

    for (int i = 0; i < g_playerCount; i++) {
        if (g_playerWindows[i] && IsWindow(g_playerWindows[i])) {
            SetParent(g_playerWindows[i], NULL);
            SetWindowLongPtr(g_playerWindows[i], GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowLongPtr(g_playerWindows[i], GWL_EXSTYLE, WS_EX_APPWINDOW);
            SetWindowPos(g_playerWindows[i], NULL, 0, 0, 800, 600,
                SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
    }

    if (g_embeddedWindow && IsWindow(g_embeddedWindow) && !g_isVideoMode) {
        SetParent(g_embeddedWindow, g_originalParent);
        SetWindowLongPtr(g_embeddedWindow, GWL_STYLE, g_originalStyle);
        SetWindowLongPtr(g_embeddedWindow, GWL_EXSTYLE, g_originalExStyle);
        SetWindowPos(g_embeddedWindow, NULL,
            g_originalRect.left, g_originalRect.top,
            g_originalRect.right - g_originalRect.left,
            g_originalRect.bottom - g_originalRect.top,
            SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    g_embeddedWindow = NULL;
    g_embedding = false;
    // 恢复嵌入窗口后强制刷新壁纸层，避免短暂黑屏未重绘的问题
    // 立即刷新一次
    RefreshDesktop();
    // 再在短时间后再刷新一次，避免 GPU/合成延迟导致的黑帧
    if (g_deferredRefreshTimer) { KillTimer(g_hMainWnd, g_deferredRefreshTimer); g_deferredRefreshTimer = 0; }
    g_deferredRefreshTimer = SetTimer(g_hMainWnd, 99, 40, NULL);
}

void StopVideo()
{
    if (g_embedding) RestoreEmbeddedWindow();
    UnhookWindowSelection();

    if (g_resizeTimer) {
        KillTimer(g_hMainWnd, g_resizeTimer);
        g_resizeTimer = 0;
    }
    if (g_watchdogTimer) {
        KillTimer(g_hMainWnd, g_watchdogTimer);
        g_watchdogTimer = 0;
    }
    if (g_deferredRefreshTimer) {
        KillTimer(g_hMainWnd, g_deferredRefreshTimer);
        g_deferredRefreshTimer = 0;
    }

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_playerProcs[i].hProcess) {
            TerminateProcess(g_playerProcs[i].hProcess, 0);
            CloseHandle(g_playerProcs[i].hThread);
            CloseHandle(g_playerProcs[i].hProcess);
            g_playerProcs[i] = { 0 };
            g_playerWindows[i] = NULL;
        }
    }
    g_playerCount = 0;
    g_playing = false;
    UpdateStatus(L"已停止播放");
    // 刷新桌面，确保原来的壁纸/图标被重绘，避免出现空白桌面
    RefreshDesktop();
}

// 强制刷新桌面与 WorkerW，避免停止视频后桌面壁纸丢失或未重绘
void RefreshDesktop()
{
    // 仅重新应用当前壁纸以强制刷新壁纸层，避免触发桌面图标/快捷方式的重绘
    WCHAR wallpaper[MAX_PATH] = { 0 };
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wallpaper, 0)) {
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, wallpaper,
            SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    }
}

// 延迟刷新回调（通过定时器触发），避免恢复窗口时短暂黑帧
void CALLBACK DeferredRefreshTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
    if (idEvent == g_deferredRefreshTimer) {
        if (g_deferredRefreshTimer) {
            KillTimer(g_hMainWnd, g_deferredRefreshTimer);
            g_deferredRefreshTimer = 0;
        }
        RefreshDesktop();
    }
}

// ── 进程看门狗 ──────────────────────────────────────────────────────

bool LaunchSinglePlayer(const std::wstring& ffplayPath, const RECT& monRect, int slot)
{
    int w = monRect.right - monRect.left;
    int h = monRect.bottom - monRect.top;

    WCHAR cmdLine[2048] = { 0 };
    wsprintfW(cmdLine, L"\"%s\" -an -loop 0 -noborder -x %d -y %d \"%s\"",
        ffplayPath.c_str(), w, h, g_videoPath.c_str());

    STARTUPINFOW si = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL,
        &si, &g_playerProcs[slot])) {
        return false;
    }

    DWORD pid = GetProcessId(g_playerProcs[slot].hProcess);
    HWND foundHwnd = NULL;

    for (int retry = 0; retry < 15 && !foundHwnd; retry++) {
        Sleep(20);
        struct EnumData { DWORD pid; HWND result; int bestScore; RECT target; };
        EnumData data = { pid, NULL, INT_MAX, monRect };
        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            EnumData* pData = (EnumData*)lParam;
            if (!IsWindowVisible(hwnd)) return TRUE;
            if (GetParent(hwnd) != NULL) return TRUE;
            RECT r;
            GetWindowRect(hwnd, &r);
            if (r.right - r.left < 100 || r.bottom - r.top < 100) return TRUE;
            DWORD winPid = 0;
            GetWindowThreadProcessId(hwnd, &winPid);
            if (winPid == pData->pid) {
                int cx = (r.left + r.right) / 2;
                int cy = (r.top + r.bottom) / 2;
                int tcx = (pData->target.left + pData->target.right) / 2;
                int tcy = (pData->target.top + pData->target.bottom) / 2;
                int dist = abs(cx - tcx) + abs(cy - tcy);
                if (dist < pData->bestScore) {
                    pData->bestScore = dist;
                    pData->result = hwnd;
                }
            }
            return TRUE;
            }, (LPARAM)&data);
        if (data.result) foundHwnd = data.result;
    }

    if (foundHwnd) {
        g_playerWindows[slot] = foundHwnd;
        g_playerTargetRects[slot] = monRect;
        EmbedWindowToDesktop(foundHwnd, &monRect);
    }
    else {
        TerminateProcess(g_playerProcs[slot].hProcess, 0);
        CloseHandle(g_playerProcs[slot].hThread);
        CloseHandle(g_playerProcs[slot].hProcess);
        g_playerProcs[slot] = { 0 };
        return false;
    }

    return true;
}

void RestartDeadPlayers()
{
    if (!g_playing || !g_isVideoMode) return;

    std::wstring exeDir = GetExeDir();
    WCHAR ffplayPath[MAX_PATH] = { 0 };
    wsprintfW(ffplayPath, L"%sffplay.exe", exeDir.c_str());
    if (!PathFileExistsW(ffplayPath)) {
        wcscpy_s(ffplayPath, L"ffplay.exe");
        if (!PathFileExistsW(ffplayPath)) return;
    }

    for (int i = 0; i < g_playerCount; i++) {
        if (!g_playerProcs[i].hProcess) continue;

        DWORD exitCode = 0;
        if (GetExitCodeProcess(g_playerProcs[i].hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
            CloseHandle(g_playerProcs[i].hThread);
            CloseHandle(g_playerProcs[i].hProcess);
            g_playerProcs[i] = { 0 };
            g_playerWindows[i] = NULL;

            if (g_embedding) RestoreEmbeddedWindow();
            LaunchSinglePlayer(ffplayPath, g_playerTargetRects[i], i);
            break;
        }
    }
}

// ── 播放视频 ────────────────────────────────────────────────────────

bool PlayVideo(const std::wstring& videoPath)
{
    if (g_embedding) { RestoreEmbeddedWindow(); Sleep(80); }
    if (g_playing) { StopVideo(); Sleep(150); }

    std::wstring exeDir = GetExeDir();
    WCHAR ffplayPath[MAX_PATH] = { 0 };
    wsprintfW(ffplayPath, L"%sffplay.exe", exeDir.c_str());
    if (!PathFileExistsW(ffplayPath)) {
        wcscpy_s(ffplayPath, L"ffplay.exe");
        if (!PathFileExistsW(ffplayPath)) {
            MessageBoxW(g_hMainWnd,
                L"未找到 ffplay.exe！\n\n请将 ffplay.exe 放置在本程序所在目录下。",
                L"错误", MB_ICONERROR);
            return false;
        }
    }

    auto monitors = EnumerateMonitors();
    int comboIndex = (int)SendMessageW(g_hMonitorCombo, CB_GETCURSEL, 0, 0);

    std::vector<RECT> targetMons;
    if (comboIndex == 0) {
        targetMons = monitors;
        g_selectedMonitor = MONITOR_ALL;
    }
    else {
        int idx = comboIndex - 1;
        if (idx >= 0 && idx < (int)monitors.size())
            targetMons.push_back(monitors[idx]);
        g_selectedMonitor = idx;
    }

    if (targetMons.empty()) {
        MessageBoxW(g_hMainWnd, L"没有可用的显示器！", L"错误", MB_ICONERROR);
        return false;
    }

    g_videoPath = videoPath;
    g_playerCount = 0;
    g_isVideoMode = true;

    for (size_t i = 0; i < targetMons.size() && g_playerCount < MAX_PLAYERS; i++) {
        if (LaunchSinglePlayer(ffplayPath, targetMons[i], g_playerCount)) {
            g_playerCount++;
        }
    }

    if (g_playerCount == 0) {
        MessageBoxW(g_hMainWnd, L"启动 ffplay 失败！", L"错误", MB_ICONERROR);
        return false;
    }

    g_resizeTimer = SetTimer(g_hMainWnd, 1, 200, NULL);
    g_watchdogTimer = SetTimer(g_hMainWnd, 2, 1000, NULL);
    g_playing = true;
    g_embedding = true;

    if (g_playerCount > 1) {
        UpdateStatus(L"✓ 正在所有显示器上播放");
    }
    else {
        UpdateStatus(L"✓ 正在播放动态壁纸");
    }
    return true;
}

// ── 窗口选择 ────────────────────────────────────────────────────────

void ClearLastHighlight()
{
    if (g_lastHighlightRect.right <= g_lastHighlightRect.left) return;
    HDC hdc = GetDC(NULL);
    if (hdc) {
        HPEN hPen = CreatePen(PS_SOLID, 4, RGB(0, 200, 255));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, g_hNullBrush);
        SetROP2(hdc, R2_XORPEN);
        Rectangle(hdc, g_lastHighlightRect.left, g_lastHighlightRect.top,
            g_lastHighlightRect.right, g_lastHighlightRect.bottom);
        SelectObject(hdc, hOldPen);
        SelectObject(hdc, hOldBrush);
        DeleteObject(hPen);
        ReleaseDC(NULL, hdc);
    }
    g_lastHighlightRect = { 0 };
}

void DrawWindowHighlight(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd)) return;
    ClearLastHighlight();
    RECT rect;
    GetWindowRect(hwnd, &rect);
    InflateRect(&rect, 2, 2);
    HDC hdc = GetDC(NULL);
    if (hdc) {
        HPEN hPen = CreatePen(PS_SOLID, 4, RGB(0, 200, 255));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, g_hNullBrush);
        SetROP2(hdc, R2_XORPEN);
        Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
        SelectObject(hdc, hOldPen);
        SelectObject(hdc, hOldBrush);
        DeleteObject(hPen);
        ReleaseDC(NULL, hdc);
    }
    g_lastHighlightRect = rect;
}

LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && g_selectingWindow) {
        MSLLHOOKSTRUCT* pMouse = (MSLLHOOKSTRUCT*)lParam;
        POINT pt = pMouse->pt;
        if (wParam == WM_MOUSEMOVE) {
            HWND hwnd = WindowFromPoint(pt);
            while (hwnd && GetParent(hwnd)) hwnd = GetParent(hwnd);
            if (hwnd && hwnd != g_hMainWnd && hwnd != g_hHoveredWindow) {
                g_hHoveredWindow = hwnd;
                DrawWindowHighlight(hwnd);
            }
        }
        else if (wParam == WM_LBUTTONDOWN) {
            HWND hwnd = WindowFromPoint(pt);
            while (hwnd && GetParent(hwnd)) hwnd = GetParent(hwnd);
            if (hwnd && hwnd != g_hMainWnd) {
                g_selectedWindow = hwnd;
                ClearLastHighlight();
                g_selectingWindow = false;
                UnhookWindowSelection();

                int comboIndex = (int)SendMessageW(g_hMonitorCombo, CB_GETCURSEL, 0, 0);
                if (comboIndex == 0) {
                    MessageBoxW(g_hMainWnd,
                        L"嵌入窗口不支持「所有显示器」选项。\n\n请选择一个具体的显示器。",
                        L"提示", MB_ICONINFORMATION);
                    return 1;
                }
                g_selectedMonitor = comboIndex - 1;
                g_isVideoMode = false;
                RECT monRect = GetMonitorRect(g_selectedMonitor);
                EmbedWindowToDesktop(hwnd, &monRect);
                g_playing = true;
                UpdateStatus(L"✓ 已嵌入窗口到桌面");
            }
            return 1;
        }
        else if (wParam == WM_RBUTTONDOWN) {
            ClearLastHighlight();
            g_selectingWindow = false;
            UnhookWindowSelection();
            UpdateStatus(L"已取消选择");
            return 1;
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

LRESULT CALLBACK KeyHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && g_selectingWindow) {
        KBDLLHOOKSTRUCT* pKey = (KBDLLHOOKSTRUCT*)lParam;
        if (wParam == WM_KEYDOWN && pKey->vkCode == VK_ESCAPE) {
            ClearLastHighlight();
            g_selectingWindow = false;
            UnhookWindowSelection();
            UpdateStatus(L"已取消选择");
        }
    }
    return CallNextHookEx(g_keyHook, nCode, wParam, lParam);
}

void StartWindowSelection()
{
    if (g_selectingWindow) return;
    g_selectingWindow = true;
    g_hHoveredWindow = NULL;
    g_selectedWindow = NULL;
    g_lastHighlightRect = { 0 };
    if (!g_hNullBrush) g_hNullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, NULL, 0);
    g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHookProc, NULL, 0);
    UpdateStatus(L"◉ 请点击要嵌入的窗口（右键或 ESC 取消）");
}

void UnhookWindowSelection()
{
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = NULL; }
    if (g_keyHook) { UnhookWindowsHookEx(g_keyHook);   g_keyHook = NULL; }
    g_selectingWindow = false;
    ClearLastHighlight();
}

// ── 系统托盘 ────────────────────────────────────────────────────────

void InitTrayIcon()
{
    g_hTrayMenu = CreatePopupMenu();
    AppendMenuW(g_hTrayMenu, MF_STRING, IDM_TRAY_SHOW, L"显示主窗口");
    AppendMenuW(g_hTrayMenu, MF_STRING, IDM_TRAY_HIDE, L"隐藏主窗口");
    AppendMenuW(g_hTrayMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(g_hTrayMenu, MF_STRING, IDM_TRAY_EXIT, L"退出程序");

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_hMainWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAYICON;
    // 使用不同的系统图标作为托盘图标（避免某些系统主题下默认图标不可见）
    g_nid.hIcon = LoadIcon(NULL, IDI_WINLOGO);
    wcscpy_s(g_nid.szTip, L"动态壁纸");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RemoveTrayIcon()
{
    if (g_nid.hWnd) Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_hTrayMenu) { DestroyMenu(g_hTrayMenu); g_hTrayMenu = NULL; }
}

void ShowTrayContextMenu()
{
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hMainWnd);
    TrackPopupMenu(g_hTrayMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hMainWnd, NULL);
    PostMessageW(g_hMainWnd, WM_NULL, 0, 0);
}

// ── 界面初始化 ──────────────────────────────────────────────────────

void InitFonts()
{
    g_hFontTitle = CreateFontW(28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_hFontNormal = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_hFontStatus = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void InitBrushes()
{
    if (g_darkMode) {
        g_hBrushBg = CreateSolidBrush(RGB(25, 25, 35));
        g_hBrushBtnPrimary = CreateSolidBrush(RGB(0, 120, 215));
        g_hBrushBtnSecondary = CreateSolidBrush(RGB(55, 55, 65));
        g_hBrushBtnHover = CreateSolidBrush(RGB(70, 70, 85));
    }
    else {
        g_hBrushBg = CreateSolidBrush(RGB(250, 250, 252));
        g_hBrushBtnPrimary = CreateSolidBrush(RGB(0, 120, 215));
        g_hBrushBtnSecondary = CreateSolidBrush(RGB(240, 240, 245));
        g_hBrushBtnHover = CreateSolidBrush(RGB(230, 230, 240));
    }
}

void DrawGradientRect(HDC hdc, const RECT* rc, COLORREF color1, COLORREF color2, bool vertical)
{
    // 简单渐变效果
    int r1 = GetRValue(color1), g1 = GetGValue(color1), b1 = GetBValue(color1);
    int r2 = GetRValue(color2), g2 = GetGValue(color2), b2 = GetBValue(color2);
    int steps = vertical ? (rc->bottom - rc->top) : (rc->right - rc->left);

    for (int i = 0; i < steps; i++) {
        int r = r1 + (r2 - r1) * i / steps;
        int g = g1 + (g2 - g1) * i / steps;
        int b = b1 + (b2 - b1) * i / steps;
        HBRUSH hBrush = CreateSolidBrush(RGB(r, g, b));
        RECT fillRect = *rc;
        if (vertical) {
            fillRect.top = rc->top + i;
            fillRect.bottom = rc->top + i + 1;
        }
        else {
            fillRect.left = rc->left + i;
            fillRect.right = rc->left + i + 1;
        }
        FillRect(hdc, &fillRect, hBrush);
        DeleteObject(hBrush);
    }
}

void DrawModernButton(HWND hWnd, HDC hdc, const wchar_t* text,
    bool isHover, bool isPressed, bool isPrimary)
{
    RECT rc;
    GetClientRect(hWnd, &rc);

    COLORREF bgColor, textColor, borderColor;

    if (isPrimary) {
        if (isPressed) {
            bgColor = RGB(0, 90, 170);
        }
        else if (isHover) {
            bgColor = RGB(0, 140, 230);
        }
        else {
            bgColor = RGB(0, 120, 215);
        }
        textColor = RGB(255, 255, 255);
        borderColor = RGB(0, 90, 170);
    }
    else {
        if (g_darkMode) {
            if (isPressed) {
                bgColor = RGB(40, 40, 50);
            }
            else if (isHover) {
                bgColor = RGB(60, 60, 75);
            }
            else {
                bgColor = RGB(50, 50, 60);
            }
            textColor = RGB(230, 230, 235);
            borderColor = RGB(80, 80, 95);
        }
        else {
            if (isPressed) {
                bgColor = RGB(200, 200, 210);
            }
            else if (isHover) {
                bgColor = RGB(235, 235, 245);
            }
            else {
                bgColor = RGB(245, 245, 250);
            }
            textColor = RGB(30, 30, 35);
            borderColor = RGB(200, 200, 210);
        }
    }

    // 绘制背景
    HBRUSH hBrush = CreateSolidBrush(bgColor);
    FillRect(hdc, &rc, hBrush);
    DeleteObject(hBrush);

    // 绘制圆角边框（简化版）
    HPEN hPen = CreatePen(PS_SOLID, 1, borderColor);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, 6, 6);
    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hOldBrush);
    DeleteObject(hPen);

    // 绘制文字
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, textColor);
    HFONT hOldFont = (HFONT)SelectObject(hdc, g_hFontNormal);

    // 文字阴影效果
    if (!isPrimary && !g_darkMode) {
        SetTextColor(hdc, RGB(255, 255, 255));
        OffsetRect(&rc, 1, 1);
        DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        OffsetRect(&rc, -1, -1);
        SetTextColor(hdc, textColor);
    }

    DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, hOldFont);
}

LRESULT CALLBACK ButtonProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    static bool isPressed = false;
    switch (message) {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        bool isHover = (hWnd == g_hHoverBtn);
        bool isPrimary = (GetDlgCtrlID(hWnd) == IDC_BTN_SELECT_VIDEO);
        WCHAR text[256] = { 0 };
        GetWindowTextW(hWnd, text, 256);
        DrawModernButton(hWnd, hdc, text, isHover, isPressed, isPrimary);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (hWnd != g_hHoverBtn) {
            g_hHoverBtn = hWnd;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hWnd, NULL, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        g_hHoverBtn = NULL;
        InvalidateRect(hWnd, NULL, FALSE);
        break;
    case WM_LBUTTONDOWN:
        isPressed = true;
        InvalidateRect(hWnd, NULL, FALSE);
        break;
    case WM_LBUTTONUP:
        isPressed = false;
        InvalidateRect(hWnd, NULL, FALSE);
        break;
    }
    return DefSubclassProc(hWnd, message, wParam, lParam);
}

// ── 窗口类/主窗口/控件 ──────────────────────────────────────────────

bool RegisterWindowClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex = { 0 };
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = NULL;
    wcex.lpszClassName = L"DynamicWallpaper";
    wcex.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcex.hIconSm = LoadIcon(NULL, IDI_APPLICATION);
    if (!RegisterClassExW(&wcex)) {
        MessageBoxW(NULL, L"窗口类注册失败！", L"错误", MB_ICONERROR);
        return false;
    }
    return true;
}

bool CreateMainWindow(HINSTANCE hInstance, int nCmdShow)
{
    int winW = 420, winH = 400;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    g_hMainWnd = CreateWindowExW(WS_EX_APPWINDOW, L"DynamicWallpaper",
        L"动态壁纸",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (screenW - winW) / 2, (screenH - winH) / 2, winW, winH,
        NULL, NULL, hInstance, NULL);
    if (!g_hMainWnd) {
        MessageBoxW(NULL, L"创建窗口失败！", L"错误", MB_ICONERROR);
        return false;
    }
    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);
    return true;
}

void CreateControls(HWND hWnd, HINSTANCE hInstance)
{
    int margin = 30;
    int y = 25;

    // 标题
    g_hTitle = CreateWindowExW(0, L"STATIC", L"动态壁纸",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        margin, y, 360, 40, hWnd, (HMENU)IDC_TITLE, hInstance, NULL);
    SendMessageW(g_hTitle, WM_SETFONT, (WPARAM)g_hFontTitle, TRUE);
    y += 55;

    // 显示器选择
    CreateWindowExW(0, L"STATIC", L"目标显示器：",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        margin, y, 360, 20, hWnd, NULL, hInstance, NULL);
    y += 25;

    g_hMonitorCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        margin, y, 360, 200, hWnd, (HMENU)IDC_MONITOR_COMBO, hInstance, NULL);
    SendMessageW(g_hMonitorCombo, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    SendMessageW(g_hMonitorCombo, CB_ADDSTRING, 0, (LPARAM)L"🖥 所有显示器");

    auto monitors = EnumerateMonitors();
    for (int i = 0; i < (int)monitors.size(); i++) {
        RECT& m = monitors[i];
        bool isPrimary = (m.left == 0 && m.top == 0);
        WCHAR name[128];
        if (isPrimary)
            wsprintfW(name, L"🖥 主显示器 (%dx%d)", m.right - m.left, m.bottom - m.top);
        else
            wsprintfW(name, L"🖥 显示器 %d (%dx%d)", i + 1, m.right - m.left, m.bottom - m.top);
        SendMessageW(g_hMonitorCombo, CB_ADDSTRING, 0, (LPARAM)name);
    }

    SendMessageW(g_hMonitorCombo, CB_SETCURSEL, 1, 0);
    y += 50;

    // 按钮区域
    int btnW = 170, btnH = 42;
    int btnGap = 20;

    HWND hBtn1 = CreateWindowExW(0, L"BUTTON", L"📹 选择视频文件",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        margin, y, btnW, btnH, hWnd, (HMENU)IDC_BTN_SELECT_VIDEO, hInstance, NULL);
    SetWindowSubclass(hBtn1, ButtonProc, 0, 0);

    HWND hBtn2 = CreateWindowExW(0, L"BUTTON", L"🪟 嵌入窗口",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        margin + btnW + btnGap, y, btnW, btnH, hWnd, (HMENU)IDC_BTN_SELECT_WINDOW, hInstance, NULL);
    SetWindowSubclass(hBtn2, ButtonProc, 0, 0);

    y += btnH + 18;

    HWND hBtn3 = CreateWindowExW(0, L"BUTTON", L"⏹ 停止播放",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        margin, y, btnW, btnH, hWnd, (HMENU)IDC_BTN_STOP, hInstance, NULL);
    SetWindowSubclass(hBtn3, ButtonProc, 0, 0);

    HWND hBtn4 = CreateWindowExW(0, L"BUTTON", L"✖ 退出程序",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        margin + btnW + btnGap, y, btnW, btnH, hWnd, (HMENU)IDC_BTN_EXIT, hInstance, NULL);
    SetWindowSubclass(hBtn4, ButtonProc, 0, 0);

    y += btnH + 25;

    // 状态区域
    CreateWindowExW(0, L"STATIC", L"状态：",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        margin, y, 360, 20, hWnd, NULL, hInstance, NULL);
    y += 22;

    g_hStatus = CreateWindowExW(0, L"STATIC", L"就绪 - 请选择视频文件或嵌入窗口",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        margin, y, 360, 80, hWnd, (HMENU)IDC_STATUS, hInstance, NULL);
    SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)g_hFontStatus, TRUE);
}

// ── 窗口过程 ────────────────────────────────────────────────────────

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {

    case WM_TIMER:
    {
        if (wParam == 1 && g_playing && g_isVideoMode && g_playerCount > 0) {
            HWND hWorkerW = FindDesktopWorkerW();
            if (hWorkerW) {
                RECT workerRect;
                GetWindowRect(hWorkerW, &workerRect);
                for (int i = 0; i < g_playerCount; i++) {
                    if (g_playerWindows[i] && IsWindow(g_playerWindows[i])) {
                        RECT& t = g_playerTargetRects[i];
                        MoveWindow(g_playerWindows[i],
                            t.left - workerRect.left,
                            t.top - workerRect.top,
                            t.right - t.left,
                            t.bottom - t.top, FALSE);
                    }
                }
            }
        }
        else if (wParam == 2 && g_playing && g_isVideoMode) {
            RestartDeadPlayers();
        }
        else if (wParam == 99) {
            // deferred wallpaper refresh timer
            if (g_deferredRefreshTimer) { KillTimer(g_hMainWnd, g_deferredRefreshTimer); g_deferredRefreshTimer = 0; }
            RefreshDesktop();
        }
        break;
    }

    case WM_CREATE:
    {
        HINSTANCE hInstance = ((LPCREATESTRUCT)lParam)->hInstance;
        g_darkMode = IsSystemDarkMode();
        InitFonts();
        InitBrushes();
        CreateControls(hWnd, hInstance);
        InitTrayIcon();
        break;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);

        // 标题使用主色
        HWND hCtrl = (HWND)lParam;
        if (hCtrl == g_hTitle) {
            SetTextColor(hdc, RGB(0, 120, 215));
        }
        else if (hCtrl == g_hStatus) {
            SetTextColor(hdc, g_darkMode ? RGB(150, 200, 150) : RGB(0, 120, 80));
        }
        else {
            SetTextColor(hdc, g_darkMode ? RGB(220, 220, 225) : RGB(30, 30, 35));
        }
        return (LRESULT)g_hBrushBg;
    }

    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hWnd, &rc);

        // 绘制渐变背景
        if (g_darkMode) {
            DrawGradientRect(hdc, &rc, RGB(20, 20, 30), RGB(35, 35, 50), true);
        }
        else {
            DrawGradientRect(hdc, &rc, RGB(245, 248, 255), RGB(235, 240, 250), true);
        }
        return 1;
    }

    case WM_SIZE:
    {
        if (wParam == SIZE_MINIMIZED) {
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        break;
    }

    case WM_TRAYICON:
    {
        switch (LOWORD(lParam)) {
        case WM_RBUTTONUP:
            ShowTrayContextMenu();
            break;
        case WM_LBUTTONDBLCLK:
            ShowWindow(hWnd, SW_RESTORE);
            SetForegroundWindow(hWnd);
            break;
        }
        break;
    }

    case WM_COMMAND:
    {
        int wmId = LOWORD(wParam);
        switch (wmId) {
        case IDC_BTN_SELECT_VIDEO:
        {
            std::wstring path;
            if (SelectVideoFile(path)) {
                PlayVideo(path);
            }
            break;
        }
        case IDC_BTN_SELECT_WINDOW:
        {
            int comboIndex = (int)SendMessageW(g_hMonitorCombo, CB_GETCURSEL, 0, 0);
            if (comboIndex == 0) {
                MessageBoxW(g_hMainWnd,
                    L"嵌入窗口不支持「所有显示器」选项。\n\n请在下拉菜单中选择一个具体的显示器。",
                    L"提示", MB_ICONINFORMATION);
                break;
            }
            StartWindowSelection();
            break;
        }
        case IDC_BTN_STOP:
            StopVideo();
            break;
        case IDC_BTN_EXIT:
            StopVideo();
            RemoveTrayIcon();
            DestroyWindow(hWnd);
            break;

        case IDM_TRAY_SHOW:
            ShowWindow(hWnd, SW_RESTORE);
            SetForegroundWindow(hWnd);
            break;
        case IDM_TRAY_HIDE:
            ShowWindow(hWnd, SW_HIDE);
            break;
        case IDM_TRAY_EXIT:
            StopVideo();
            RemoveTrayIcon();
            DestroyWindow(hWnd);
            break;
        }
        break;
    }

    case WM_CLOSE:
    {
        // 点击关闭按钮时最小化到托盘
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    }

    case WM_DESTROY:
        StopVideo();
        RemoveTrayIcon();
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    return 0;
}

// ── 程序入口 ────────────────────────────────────────────────────────

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
    LPWSTR lpCmdLine, int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // 初始化公共控件
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    if (!RegisterWindowClass(hInstance)) return 1;
    if (!CreateMainWindow(hInstance, nCmdShow)) return 1;

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
