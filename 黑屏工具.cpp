// 黑屏工具 (BlackScreen) —— 一键黑屏，再按恢复，带图形设置界面
//
// 默认快捷键：
//   Alt+X         黑屏（可在 GUI 中更改）
//   Ctrl+O        恢复显示（默认，可在 GUI 中更改；黑屏期间注册为全局热键）
//   Ctrl+Q        退出本工具
//
// 黑屏方式（可在 GUI 中切换）：
//   黑屏          创建铺满所有显示器、置顶的黑色窗口，吞掉鼠标键盘，
//                 显示器保持点亮（有背光微光）。
//   断显示信号    黑色窗口之上再广播 SC_MONITORPOWER 关闭显示器信号，
//                 消除背光微光；黑屏期间每 3 秒补发一次防止误唤醒。
//
// GUI：程序启动后显示设置窗口，可捕获新快捷键、选择黑屏方式并「应用」，
//      配置持久化到 exe 同目录 settings.ini。关闭窗口只是隐藏到托盘，
//      托盘图标双击/右键可再次打开或退出。
//
// 编译（MinGW-w64 g++，C++17）：
//   g++ -O2 -std=c++17 -Wall -Wextra -static -mwindows -luser32 -lshell32 -lgdi32 -o 黑屏工具.exe 黑屏工具.cpp
//
// 自检（不黑屏、不弹窗，验证快捷键与覆盖范围后按退出码返回）：
//   黑屏工具.exe --check

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>
#include <string>
#include <cstdio>

static const int HK_TOGGLE = 0xAC01;               // 黑屏
static const int HK_QUIT   = 0xAC02;               // 退出工具
static const int HK_RESUME = 0xAC03;               // 恢复显示

static const wchar_t *kBlackCls = L"BlackScreenCoverWnd";
static const wchar_t *kMsgCls   = L"BlackScreenMsgWnd";
static const wchar_t *kGuiCls   = L"BlackScreenGuiWnd";

static HWND g_hMsg   = nullptr;                    // 接收热键的 message-only 窗口
static HWND g_hBlack = nullptr;                    // 黑屏窗口（存在即处于黑屏状态）
static HWND g_hGui   = nullptr;                    // 设置 GUI 窗口

// ---- 黑屏方式 ----
enum BlackMode
{
    MODE_BLACK  = 0,                               // 仅黑色窗口盖屏
    MODE_SIGNAL = 1                                // 黑色窗口 + 切断显示信号
};

struct Config
{
    UINT mods  = MOD_ALT;                          // 黑屏快捷键修饰键位
    UINT vk    = 'X';                              // 黑屏快捷键主键
    UINT rmods = MOD_CONTROL;                      // 恢复快捷键修饰键位，默认 Ctrl+O
    UINT rvk   = 'O';                              // 恢复快捷键主键
    int  mode  = MODE_SIGNAL;                      // 默认保持旧版行为
};

static Config g_cfg;                               // 已生效配置
static Config g_pending;                           // GUI 编辑中的待应用配置
static bool   g_resumeKeyFailed = false;           // 恢复键注册失败时，Esc 作为兜底

// ---- 显示电源控制：SC_MONITORPOWER 2=关信号 1=待机 -1=重新点亮 ----
static const UINT_PTR TIMER_REBLANK = 0xAC10;
static const DWORD    REBLANK_MS    = 3000;

static void DisplayPower(int mode)
{
    SendMessageTimeoutW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER,
                        static_cast<LPARAM>(mode), SMTO_ABORTIFHUNG, 1000, nullptr);
}

// 计算能罩住整个虚拟屏幕（所有显示器）的矩形，外扩 8px 避免分数缩放下露缝
static RECT FullCoverRect()
{
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0)
    {
        x = 0; y = 0;
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
    }
    RECT r = { x - 8, y - 8, x + w + 8, y + h + 8 };
    return r;
}

static void Uncover()
{
    if (g_hMsg)
    {
        KillTimer(g_hMsg, TIMER_REBLANK);
        UnregisterHotKey(g_hMsg, HK_RESUME);       // 恢复热键仅黑屏期间注册
    }
    if (g_hBlack)
    {
        DestroyWindow(g_hBlack);                   // WM_DESTROY 里会把 g_hBlack 置空
        g_hBlack = nullptr;
    }
    DisplayPower(-1);                              // 重新点亮显示器
    SetCursor(LoadCursorA(nullptr, IDC_ARROW));    // 强制恢复鼠标指针
}

static void Cover()
{
    if (g_hBlack) return;
    RECT r = FullCoverRect();
    g_hBlack = CreateWindowExW(WS_EX_TOPMOST, kBlackCls, L"", WS_POPUP,
                               r.left, r.top, r.right - r.left, r.bottom - r.top,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_hBlack) return;
    ShowWindow(g_hBlack, SW_SHOW);
    // 重新声明置顶，压过任务栏；不使用 SWP_SHOWWINDOW，位置一次到位
    SetWindowPos(g_hBlack, HWND_TOPMOST, r.left, r.top,
                 r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
    SetForegroundWindow(g_hBlack);                 // 尽力拿焦点，失败也不影响热键
    if (g_hMsg)
    {
        // 黑屏期间才占用恢复键；注册失败则退回 Esc 兜底恢复
        g_resumeKeyFailed = RegisterHotKey(g_hMsg, HK_RESUME, g_cfg.rmods, g_cfg.rvk) == FALSE;
    }
    if (g_cfg.mode == MODE_SIGNAL)
    {
        DisplayPower(2);                           // 掐断显示信号，消除背光微光
        if (g_hMsg) SetTimer(g_hMsg, TIMER_REBLANK, REBLANK_MS, nullptr);
    }
}

// ---- 快捷键文字化与解析 ----
static void HotkeyText(UINT mods, UINT vk, wchar_t *buf, size_t cch)
{
    buf[0] = L'\0';
    wchar_t tmp[64] = L"";
    if (mods & MOD_CONTROL) StringCchCatW(tmp, 64, L"Ctrl+");
    if (mods & MOD_ALT)     StringCchCatW(tmp, 64, L"Alt+");
    if (mods & MOD_SHIFT)   StringCchCatW(tmp, 64, L"Shift+");
    if (mods & MOD_WIN)     StringCchCatW(tmp, 64, L"Win+");

    wchar_t key[32] = L"";
    if ((vk >= L'A' && vk <= L'Z') || (vk >= L'0' && vk <= L'9'))
        StringCchPrintfW(key, 32, L"%c", static_cast<wchar_t>(vk));
    else if (vk >= VK_F1 && vk <= VK_F12)
        StringCchPrintfW(key, 32, L"F%u", vk - VK_F1 + 1);
    else switch (vk)
    {
    case VK_SPACE:   StringCchCopyW(key, 32, L"Space"); break;
    case VK_RETURN:  StringCchCopyW(key, 32, L"Enter"); break;
    case VK_TAB:     StringCchCopyW(key, 32, L"Tab");   break;
    case VK_ESCAPE:  StringCchCopyW(key, 32, L"Esc");   break;
    case VK_PRIOR:   StringCchCopyW(key, 32, L"PgUp");  break;
    case VK_NEXT:    StringCchCopyW(key, 32, L"PgDn");  break;
    case VK_HOME:    StringCchCopyW(key, 32, L"Home");  break;
    case VK_END:     StringCchCopyW(key, 32, L"End");   break;
    case VK_INSERT:  StringCchCopyW(key, 32, L"Insert"); break;
    case VK_DELETE:  StringCchCopyW(key, 32, L"Delete"); break;
    case VK_LEFT:    StringCchCopyW(key, 32, L"Left");  break;
    case VK_RIGHT:   StringCchCopyW(key, 32, L"Right"); break;
    case VK_UP:      StringCchCopyW(key, 32, L"Up");    break;
    case VK_DOWN:    StringCchCopyW(key, 32, L"Down");  break;
    case VK_SNAPSHOT:StringCchCopyW(key, 32, L"PrtSc"); break;
    default:         StringCchPrintfW(key, 32, L"VK%02X", vk); break;
    }
    StringCchPrintfW(buf, cch, L"%s%s", tmp, key);
}

// 正在按住的键是否只是修饰键本身
static bool IsModifierVk(UINT vk)
{
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_MENU     || vk == VK_LMENU   || vk == VK_RMENU ||
           vk == VK_SHIFT    || vk == VK_LSHIFT  || vk == VK_RSHIFT ||
           vk == VK_LWIN     || vk == VK_RWIN;
}

// 从物理按键状态提取 RegisterHotKey 修饰键位
static UINT CurrentMods()
{
    UINT m = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) m |= MOD_ALT;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) m |= MOD_SHIFT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) ||
        (GetAsyncKeyState(VK_RWIN) & 0x8000))  m |= MOD_WIN;
    return m;
}

// ---- 配置持久化（exe 同目录 settings.ini） ----
static void ConfigPath(wchar_t *buf, size_t cch)
{
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(cch));
    wchar_t *dot = wcsrchr(buf, L'.');
    wchar_t *sep = wcsrchr(buf, L'\\');
    if (dot && sep && dot > sep) *dot = L'\0';
    StringCchCatW(buf, cch, L"\\settings.ini");
}

static void LoadConfig()
{
    wchar_t path[MAX_PATH];
    ConfigPath(path, MAX_PATH);
    UINT mods  = GetPrivateProfileIntW(L"Hotkey", L"Mods", -1, path);
    UINT vk    = GetPrivateProfileIntW(L"Hotkey", L"Vk", -1, path);
    UINT rmods = GetPrivateProfileIntW(L"Resume", L"Mods", -1, path);
    UINT rvk   = GetPrivateProfileIntW(L"Resume", L"Vk", -1, path);
    int  mode  = GetPrivateProfileIntW(L"Black",  L"Mode", MODE_SIGNAL, path);
    if (mods != static_cast<UINT>(-1) && vk != static_cast<UINT>(-1) && vk != 0)
    {
        g_cfg.mods = mods;
        g_cfg.vk   = vk;
    }
    if (rmods != static_cast<UINT>(-1) && rvk != static_cast<UINT>(-1) && rvk != 0)
    {
        g_cfg.rmods = rmods;
        g_cfg.rvk   = rvk;
    }
    g_cfg.mode = (mode == MODE_BLACK) ? MODE_BLACK : MODE_SIGNAL;
    g_pending  = g_cfg;
}

static void SaveConfig()
{
    wchar_t path[MAX_PATH];
    ConfigPath(path, MAX_PATH);
    wchar_t num[16];
    StringCchPrintfW(num, 16, L"%u", g_cfg.mods);
    WritePrivateProfileStringW(L"Hotkey", L"Mods", num, path);
    StringCchPrintfW(num, 16, L"%u", g_cfg.vk);
    WritePrivateProfileStringW(L"Hotkey", L"Vk", num, path);
    StringCchPrintfW(num, 16, L"%u", g_cfg.rmods);
    WritePrivateProfileStringW(L"Resume", L"Mods", num, path);
    StringCchPrintfW(num, 16, L"%u", g_cfg.rvk);
    WritePrivateProfileStringW(L"Resume", L"Vk", num, path);
    StringCchPrintfW(num, 16, L"%d", g_cfg.mode);
    WritePrivateProfileStringW(L"Black", L"Mode", num, path);
}

// ---- 托盘图标 ----
static const UINT  TRAY_ID   = 1;
static const UINT  WM_TRAY   = WM_APP + 1;
static NOTIFYICONDATAW g_nid = {};

static void TrayAdd(HWND owner)
{
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = owner;
    g_nid.uID              = TRAY_ID;
    g_nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon            = LoadIcon(nullptr, IDI_APPLICATION);
    StringCchCopyW(g_nid.szTip, 128, L"黑屏工具（双击打开设置）");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void TrayRemove()
{
    if (g_nid.hWnd) Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void QuitAll()
{
    Uncover();
    TrayRemove();
    if (g_hMsg) { UnregisterHotKey(g_hMsg, HK_TOGGLE); UnregisterHotKey(g_hMsg, HK_RESUME); UnregisterHotKey(g_hMsg, HK_QUIT); }
    PostQuitMessage(0);
}

// ---- 设置 GUI ----
enum GuiCtl
{
    IDC_EDIT_HOTKEY = 101,                         // 黑屏快捷键显示框（只读）
    IDC_BTN_CHANGE  = 102,                         // 开始捕获黑屏快捷键
    IDC_RADIO_BLACK = 103,
    IDC_RADIO_SIG   = 104,
    IDC_BTN_APPLY   = 105,
    IDC_BTN_QUIT    = 106,
    IDC_STATIC_HINT = 107,
    IDC_EDIT_RESUME = 108,                         // 恢复快捷键显示框（只读）
    IDC_BTN_CHG_RES = 109,                         // 开始捕获恢复快捷键
};

enum CaptureSlot { SLOT_NONE = 0, SLOT_TOGGLE = 1, SLOT_RESUME = 2 };

static int g_captureSlot = SLOT_NONE;              // 正在等待用户按新快捷键的槽位

static void GuiRefreshHotkeyEdit()
{
    if (!g_hGui) return;
    wchar_t txt[64];
    HotkeyText(g_pending.mods, g_pending.vk, txt, 64);
    SetWindowTextW(GetDlgItem(g_hGui, IDC_EDIT_HOTKEY),
                   g_captureSlot == SLOT_TOGGLE ? L"请按下新快捷键..." : txt);
    HotkeyText(g_pending.rmods, g_pending.rvk, txt, 64);
    SetWindowTextW(GetDlgItem(g_hGui, IDC_EDIT_RESUME),
                   g_captureSlot == SLOT_RESUME ? L"请按下新快捷键..." : txt);
}

static void GuiApplyVisual()
{
    SendMessageW(GetDlgItem(g_hGui, IDC_RADIO_BLACK), BM_SETCHECK,
                 g_pending.mode == MODE_BLACK   ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(g_hGui, IDC_RADIO_SIG), BM_SETCHECK,
                 g_pending.mode == MODE_SIGNAL  ? BST_CHECKED : BST_UNCHECKED, 0);
    GuiRefreshHotkeyEdit();
}

static void SetHint(const wchar_t *text)
{
    if (g_hGui) SetWindowTextW(GetDlgItem(g_hGui, IDC_STATIC_HINT), text);
}

static bool ApplyHotkeys()
{
    // 黑屏键与恢复键不能相同，否则两键无法区分
    if (g_pending.mods == g_pending.rmods && g_pending.vk == g_pending.rvk)
        return false;

    const bool wasBlacked = g_hBlack != nullptr;
    if (wasBlacked) UnregisterHotKey(g_hMsg, HK_RESUME);
    UnregisterHotKey(g_hMsg, HK_TOGGLE);

    BOOL tOk = RegisterHotKey(g_hMsg, HK_TOGGLE, g_pending.mods, g_pending.vk);
    BOOL rOk = RegisterHotKey(g_hMsg, HK_RESUME, g_pending.rmods, g_pending.rvk);
    if (!tOk || !rOk)
    {
        // 注册失败：回退旧配置，恢复旧热键
        if (tOk) UnregisterHotKey(g_hMsg, HK_TOGGLE);
        if (rOk) UnregisterHotKey(g_hMsg, HK_RESUME);
        RegisterHotKey(g_hMsg, HK_TOGGLE, g_cfg.mods, g_cfg.vk);
        if (wasBlacked)
            RegisterHotKey(g_hMsg, HK_RESUME, g_cfg.rmods, g_cfg.rvk);  // 按旧键重新武装
        g_pending = g_cfg;
        return false;
    }
    g_cfg = g_pending;
    SaveConfig();
    return true;
}

static LRESULT CALLBACK GuiProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        auto Mk = [&](LPCWSTR cls, LPCWSTR text, DWORD style, int x, int y,
                      int w, int h, int id) -> HWND
        {
            HWND c = CreateWindowExW(0, cls, text,
                                     WS_CHILD | WS_VISIBLE | style,
                                     x, y, w, h, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                     GetModuleHandleW(nullptr), nullptr);
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            return c;
        };
        Mk(L"STATIC", L"黑屏快捷键：", 0, 14, 18, 90, 18, 0);
        Mk(L"EDIT", L"", ES_READONLY | ES_CENTER | WS_BORDER,
           108, 14, 130, 24, IDC_EDIT_HOTKEY);
        Mk(L"BUTTON", L"更改快捷键", BS_PUSHBUTTON, 248, 13, 92, 26, IDC_BTN_CHANGE);
        Mk(L"STATIC", L"恢复快捷键：", 0, 14, 50, 90, 18, 0);
        Mk(L"EDIT", L"", ES_READONLY | ES_CENTER | WS_BORDER,
           108, 46, 130, 24, IDC_EDIT_RESUME);
        Mk(L"BUTTON", L"更改快捷键", BS_PUSHBUTTON, 248, 45, 92, 26, IDC_BTN_CHG_RES);
        Mk(L"STATIC", L"黑屏方式：", 0, 14, 88, 90, 18, 0);
        Mk(L"BUTTON", L"黑屏（仅盖黑色窗口，显示器不关）", BS_AUTORADIOBUTTON,
           108, 84, 240, 20, IDC_RADIO_BLACK);
        Mk(L"BUTTON", L"断显示信号（黑色窗口并关闭显示器）", BS_AUTORADIOBUTTON,
           108, 108, 240, 20, IDC_RADIO_SIG);
        Mk(L"BUTTON", L"应用", BS_DEFPUSHBUTTON, 108, 140, 70, 28, IDC_BTN_APPLY);
        Mk(L"BUTTON", L"退出", BS_PUSHBUTTON, 186, 140, 70, 28, IDC_BTN_QUIT);
        Mk(L"STATIC", L"", SS_LEFT, 14, 180, 326, 36, IDC_STATIC_HINT);
        g_hGui = hwnd;
        GuiApplyVisual();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_BTN_CHANGE:
            g_captureSlot = (g_captureSlot == SLOT_TOGGLE) ? SLOT_NONE : SLOT_TOGGLE;
            GuiRefreshHotkeyEdit();
            SetHint(g_captureSlot == SLOT_TOGGLE
                        ? L"现在按下黑屏组合键（须含 Ctrl/Alt/Shift/Win 之一，F1-F12 可单按）；Esc 取消。"
                        : L"");
            if (g_captureSlot == SLOT_TOGGLE) SetFocus(hwnd);
            return 0;
        case IDC_BTN_CHG_RES:
            g_captureSlot = (g_captureSlot == SLOT_RESUME) ? SLOT_NONE : SLOT_RESUME;
            GuiRefreshHotkeyEdit();
            SetHint(g_captureSlot == SLOT_RESUME
                        ? L"现在按下恢复组合键（须含 Ctrl/Alt/Shift/Win 之一，F1-F12 可单按）；Esc 取消。"
                        : L"");
            if (g_captureSlot == SLOT_RESUME) SetFocus(hwnd);
            return 0;
        case IDC_RADIO_BLACK:
            g_pending.mode = MODE_BLACK;
            return 0;
        case IDC_RADIO_SIG:
            g_pending.mode = MODE_SIGNAL;
            return 0;
        case IDC_BTN_APPLY:
        {
            g_captureSlot = SLOT_NONE;
            GuiRefreshHotkeyEdit();
            int oldMode = g_cfg.mode;
            if (!ApplyHotkeys())
            {
                wchar_t txt[64];
                HotkeyText(g_pending.mods, g_pending.vk, txt, 64);
                wchar_t txt2[64];
                HotkeyText(g_pending.rmods, g_pending.rvk, txt2, 64);
                std::wstring why = (std::wstring(L"黑屏键 ") + txt + L" 与恢复键 " + txt2);
                if (g_pending.mods == g_pending.rmods && g_pending.vk == g_pending.rvk)
                    why += L" 相同";
                else
                    why += L" 注册失败（可能被其它程序占用）";
                why += L"，已恢复为当前配置。";
                SetHint(why.c_str());
                GuiApplyVisual();
                return 0;
            }
            bool modeChanged = (oldMode != g_cfg.mode);
            if (modeChanged && g_hBlack)
            {
                // 黑屏过程中改方式：立即按新配置调整信号状态
                if (g_cfg.mode == MODE_SIGNAL)
                {
                    DisplayPower(2);
                    SetTimer(g_hMsg, TIMER_REBLANK, REBLANK_MS, nullptr);
                }
                else
                {
                    KillTimer(g_hMsg, TIMER_REBLANK);
                    DisplayPower(-1);
                }
            }
            wchar_t txt[64], txt2[64];
            HotkeyText(g_cfg.mods, g_cfg.vk, txt, 64);
            HotkeyText(g_cfg.rmods, g_cfg.rvk, txt2, 64);
            SetHint((std::wstring(L"已应用：黑屏 ") + txt +
                     L" · 恢复 " + txt2 +
                     (g_cfg.mode == MODE_SIGNAL ? L" · 断显示信号" : L" · 仅黑屏") +
                     L"。关闭窗口不会退出，程序在托盘中运行。").c_str());
            return 0;
        }
        case IDC_BTN_QUIT:
            QuitAll();
            return 0;
        }
        return 0;
    case WM_KEYDOWN:
        if (g_captureSlot == SLOT_NONE) break;
        if (wp == VK_ESCAPE)
        {
            g_captureSlot = SLOT_NONE;
            GuiRefreshHotkeyEdit();
            SetHint(L"已取消捕获。");
            return 0;
        }
        if (IsModifierVk(static_cast<UINT>(wp))) return 0;   // 只按修饰键不结束捕获
        {
            UINT mods = CurrentMods();
            UINT vk   = static_cast<UINT>(wp);
            bool fkey = (vk >= VK_F1 && vk <= VK_F12);
            if (mods == 0 && !fkey)
            {
                SetHint(L"至少需要一个修饰键（Ctrl/Alt/Shift/Win），或单按 F1-F12。");
                return 0;
            }
            if (g_captureSlot == SLOT_TOGGLE) { g_pending.mods = mods; g_pending.vk = vk; }
            else                              { g_pending.rmods = mods; g_pending.rvk = vk; }
            g_captureSlot = SLOT_NONE;
            GuiRefreshHotkeyEdit();
            wchar_t txt[64];
            HotkeyText(mods, vk, txt, 64);
            SetHint((std::wstring(L"新快捷键 ") + txt + L"，点击「应用」生效。").c_str());
            return 0;
        }
    case WM_CLOSE:                                        // 关窗 = 隐藏回托盘
        ShowWindow(hwnd, SW_HIDE);
        SetHint(L"程序已最小化到托盘，双击托盘图标可再次打开。");
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK)
        {
            ShowWindow(hwnd, IsWindowVisible(hwnd) ? SW_HIDE : SW_SHOW);
            if (IsWindowVisible(hwnd)) SetForegroundWindow(hwnd);
            g_captureSlot = SLOT_NONE;
            GuiRefreshHotkeyEdit();
        }
        else if (LOWORD(lp) == WM_RBUTTONDOWN)
        {
            POINT pt; GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"打开设置");
            AppendMenuW(menu, MF_STRING, 2, L"黑屏 / 恢复");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 3, L"退出");
            SetForegroundWindow(hwnd);                    // 保证菜单点击消息送达
            UINT cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                      pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd == 1)
            {
                ShowWindow(hwnd, SW_SHOW);
                GuiApplyVisual();
            }
            else if (cmd == 2)
            {
                if (g_hBlack) Uncover(); else Cover();
            }
            else if (cmd == 3)
            {
                QuitAll();
            }
            PostMessageW(hwnd, WM_NULL, 0, 0);            // 修复菜单不消失的小毛病
        }
        return 0;
    case WM_DESTROY:
        if (g_hGui == hwnd) g_hGui = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- 黑屏窗口过程 ----
static LRESULT CALLBACK BlackProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && g_resumeKeyFailed) Uncover();
        return 0;                                  // 吞掉按键，黑屏时不误触
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_SYSKEYDOWN:
        return 0;
    case WM_LBUTTONDOWN:  case WM_RBUTTONDOWN:  case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:    case WM_RBUTTONUP:    case WM_MBUTTONUP:
    case WM_LBUTTONDBLCLK:case WM_RBUTTONDBLCLK:case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL:   case WM_MOUSEHWHEEL:
        return 0;                                  // 吞掉鼠标，不透传到桌面
    case WM_NCHITTEST:
        return HTCLIENT;                           // 整个窗口视为客户区
    case WM_SETCURSOR:
        SetCursor(nullptr);                        // 黑屏期间隐藏指针
        return TRUE;
    case WM_DISPLAYCHANGE:                          // 黑屏期间插拔/改分辨率
    {
        RECT r = FullCoverRect();
        MoveWindow(hwnd, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
        return 0;
    }
    case WM_DESTROY:
        if (g_hBlack == hwnd) g_hBlack = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- 热键窗口过程 ----
static LRESULT CALLBACK MsgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_HOTKEY)
    {
        if (wp == HK_TOGGLE)
        {
            if (g_hBlack) Uncover();
            else          Cover();
        }
        else if (wp == HK_RESUME)
        {
            if (g_hBlack) Uncover();               // 恢复键只用于恢复，不反向触发黑屏
        }
        else if (wp == HK_QUIT)
        {
            QuitAll();
        }
        return 0;
    }
    if (msg == WM_TIMER && wp == TIMER_REBLANK)
    {
        if (g_hBlack && g_cfg.mode == MODE_SIGNAL)
            DisplayPower(2);                        // 黑屏期间持续掐断信号
        else
            KillTimer(hwnd, TIMER_REBLANK);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool RegisterClasses()
{
    WNDCLASSEXW wcBlack = {};
    wcBlack.cbSize        = sizeof(wcBlack);
    wcBlack.lpfnWndProc   = BlackProc;
    wcBlack.hInstance     = GetModuleHandleW(nullptr);
    wcBlack.hCursor       = nullptr;               // 黑屏时指针经过也不显示
    wcBlack.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wcBlack.lpszClassName = kBlackCls;
    if (!RegisterClassExW(&wcBlack) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    WNDCLASSEXW wcMsg = {};
    wcMsg.cbSize        = sizeof(wcMsg);
    wcMsg.lpfnWndProc   = MsgProc;
    wcMsg.hInstance     = GetModuleHandleW(nullptr);
    wcMsg.lpszClassName = kMsgCls;
    if (!RegisterClassExW(&wcMsg) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    WNDCLASSEXW wcGui = {};
    wcGui.cbSize        = sizeof(wcGui);
    wcGui.lpfnWndProc   = GuiProc;
    wcGui.hInstance     = GetModuleHandleW(nullptr);
    wcGui.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcGui.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wcGui.lpszClassName = kGuiCls;
    if (!RegisterClassExW(&wcGui) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    return true;
}

// ---- 自检模式 ----
static bool g_allMonitorsInside = true;

static BOOL CALLBACK MonitorInsideProc(HMONITOR, HDC, LPRECT rc, LPARAM lp)
{
    const RECT *cover = reinterpret_cast<const RECT *>(lp);
    if (rc->left < cover->left || rc->top < cover->top ||
        rc->right > cover->right || rc->bottom > cover->bottom)
    {
        g_allMonitorsInside = false;
        return FALSE;                              // 提前结束枚举
    }
    return TRUE;
}

#define CHECK(cond, name)                          \
    do {                                           \
        if (cond) std::printf("[ OK ] %s\n", name); \
        else { std::printf("[FAIL] %s\n", name); ++failed; } \
    } while (0)

static int RunSelfTest()
{
    int failed = 0;

    CHECK(RegisterClasses(), "register window classes");

    g_hMsg = CreateWindowExW(0, kMsgCls, L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(g_hMsg != nullptr, "create message window");

    BOOL ok1 = RegisterHotKey(g_hMsg, HK_TOGGLE, g_cfg.mods, g_cfg.vk);
    BOOL okR = RegisterHotKey(g_hMsg, HK_RESUME, g_cfg.rmods, g_cfg.rvk);
    BOOL ok2 = RegisterHotKey(g_hMsg, HK_QUIT, MOD_CONTROL, 'Q');
    wchar_t hkTxt[64], rkTxt[64];
    HotkeyText(g_cfg.mods, g_cfg.vk, hkTxt, 64);
    HotkeyText(g_cfg.rmods, g_cfg.rvk, rkTxt, 64);
    char hkA[96] = "", rkA[96] = "";
    WideCharToMultiByte(CP_UTF8, 0, hkTxt, -1, hkA, sizeof(hkA), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, rkTxt, -1, rkA, sizeof(rkA), nullptr, nullptr);
    CHECK(ok1, "register hotkey from config");
    std::printf("       hotkey = %s, mode = %s\n", hkA,
                g_cfg.mode == MODE_SIGNAL ? "signal-off" : "black-only");
    CHECK(okR, "register resume hotkey from config");
    std::printf("       resume hotkey = %s\n", rkA);
    CHECK(ok2, "register hotkey Ctrl+Q");

    RECT r = FullCoverRect();
    CHECK(r.right > r.left && r.bottom > r.top, "query virtual screen size");

    HWND probe = CreateWindowExW(WS_EX_TOPMOST, kBlackCls, L"", WS_POPUP,
                                 r.left, r.top, r.right - r.left, r.bottom - r.top,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(probe != nullptr, "create cover window (hidden)");
    if (probe)
    {
        RECT wr = {};
        GetWindowRect(probe, &wr);
        CHECK(wr.left == r.left && wr.top == r.top &&
              wr.right == r.right && wr.bottom == r.bottom, "cover window geometry");
        CHECK((GetWindowLongW(probe, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0, "cover window topmost");
        g_allMonitorsInside = true;
        EnumDisplayMonitors(nullptr, nullptr, MonitorInsideProc,
                            reinterpret_cast<LPARAM>(&r));
        CHECK(g_allMonitorsInside, "cover all monitors");
        std::printf("       virtual screen = [%d,%d]-[%d,%d]\n",
                    static_cast<int>(r.left), static_cast<int>(r.top),
                    static_cast<int>(r.right), static_cast<int>(r.bottom));
        DestroyWindow(probe);
    }

    if (ok1) UnregisterHotKey(g_hMsg, HK_TOGGLE);
    if (okR) UnregisterHotKey(g_hMsg, HK_RESUME);
    if (ok2) UnregisterHotKey(g_hMsg, HK_QUIT);
    if (g_hMsg) { DestroyWindow(g_hMsg); g_hMsg = nullptr; }

    std::printf(failed ? "SELFTEST FAILED (%d)\n" : "SELFTEST OK\n", failed);
    return failed ? 1 : 0;
}

// ---- 演示模式：真实黑屏约 1.2 秒后自动恢复，端到端验证核心功能 ----
static int RunDemo()
{
    if (!RegisterClasses())
    {
        std::printf("[FAIL] register window classes\n");
        return 1;
    }

    Cover();
    if (!g_hBlack)
    {
        std::printf("[FAIL] cover() created no window\n");
        return 1;
    }

    int failed = 0;
    RECT r = FullCoverRect(), wr = {};
    GetWindowRect(g_hBlack, &wr);
    CHECK(wr.left == r.left && wr.top == r.top &&
          wr.right == r.right && wr.bottom == r.bottom, "cover is fullscreen");
    CHECK((GetWindowLongW(g_hBlack, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0, "cover is topmost");
    CHECK(IsWindowVisible(g_hBlack), "cover is visible");
    POINT center = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    CHECK(WindowFromPoint(center) == g_hBlack, "pointer hits cover (clicks swallowed)");

    ULONGLONG t0 = GetTickCount64();
    MSG m;
    while (GetTickCount64() - t0 < 1200)           // 保持黑屏并泵消息，让窗口真正画出
    {
        if (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        else
        {
            Sleep(10);
        }
    }
    Uncover();
    CHECK(g_hBlack == nullptr, "auto-recover after ~1.2s");

    std::printf(failed ? "DEMO FAILED (%d)\n" : "DEMO OK\n", failed);
    return failed ? 1 : 0;
}

static void ShowGui()
{
    RECT wr = { 0, 0, 360, 228 };
    AdjustWindowRect(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 3;
    g_hGui = CreateWindowExW(0, kGuiCls, L"黑屏工具 - 设置",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             x, y, w, h, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    ShowWindow(g_hGui, SW_SHOW);
    SetForegroundWindow(g_hGui);
}

int APIENTRY WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    SetProcessDPIAware();                          // 度量按物理像素返回，黑屏不留边缝

    LoadConfig();

    if (wcsstr(GetCommandLineW(), L"--check") != nullptr)
    {
        AttachConsole(ATTACH_PARENT_PROCESS);
        return RunSelfTest();
    }

    if (wcsstr(GetCommandLineW(), L"--demo") != nullptr)
    {
        AttachConsole(ATTACH_PARENT_PROCESS);
        return RunDemo();
    }

    HANDLE hMutex = CreateMutexW(nullptr, FALSE, L"Local\\QwenBlackScreenTool");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW(nullptr, L"黑屏工具已在运行中。", L"黑屏工具",
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    if (!RegisterClasses())
    {
        MessageBoxW(nullptr, L"窗口类注册失败，无法启动。", L"黑屏工具",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    g_hMsg = CreateWindowExW(0, kMsgCls, L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_hMsg)
    {
        MessageBoxW(nullptr, L"创建消息窗口失败，无法启动。", L"黑屏工具",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    bool hkOk = RegisterHotKey(g_hMsg, HK_TOGGLE, g_cfg.mods, g_cfg.vk) != FALSE;
    RegisterHotKey(g_hMsg, HK_QUIT, MOD_CONTROL, 'Q');   // 失败不影响主要功能

    ShowGui();
    TrayAdd(g_hGui ? g_hGui : g_hMsg);
    if (!hkOk)
        SetHint(L"启动时快捷键注册失败（可能被占用），请更改快捷键后点击「应用」。");

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    TrayRemove();
    if (hMutex) CloseHandle(hMutex);
    return 0;
}
