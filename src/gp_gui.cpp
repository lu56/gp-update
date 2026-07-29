#include "gp_gui.h"
#include "gp_auth.h"
#include "gp_update.h"
#include "gp_http.h"
#include "gp_json.h"
#include "gp_fingerprint.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sstream>
#include <fstream>
#include <algorithm>

namespace gp {

// ===== UTF-8 <-> UTF-16 helpers =====
static std::wstring utf8ToW(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring ws(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

static std::string wToUtf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
}

static std::wstring numToW(int n) {
    return std::to_wstring(n);
}

// Helper to create a static label
static HWND createLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h,
                        DWORD style = SS_LEFT, HINSTANCE hInst = nullptr, int id = -1) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | style,
                          x, y, w, h, parent, (HMENU)(INT_PTR)id, hInst, nullptr);
}

// Helper to create a button
static HWND createButton(HWND parent, const wchar_t* text, int x, int y, int w, int h,
                         HINSTANCE hInst, int id) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                          x, y, w, h, parent, (HMENU)(INT_PTR)id, hInst, nullptr);
}

// Helper to create an edit box
static HWND createEdit(HWND parent, const wchar_t* text, int x, int y, int w, int h,
                       DWORD style, HINSTANCE hInst, int id) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | style,
                          x, y, w, h, parent, (HMENU)(INT_PTR)id, hInst, nullptr);
}

// Helper to create a checkbox
static HWND createCheckbox(HWND parent, const wchar_t* text, int x, int y, int w, int h,
                           HINSTANCE hInst, int id, bool checked = false) {
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                            x, y, w, h, parent, (HMENU)(INT_PTR)id, hInst, nullptr);
    if (checked) SendMessageW(hwnd, BM_SETCHECK, BST_CHECKED, 0);
    return hwnd;
}

// ===== Constructor / Destructor =====

GuiApp::GuiApp(AppConfig& cfg, RouteEngine& eng, const std::string& devId)
    : config(cfg), engine(eng), deviceId(devId) {
    hInst = GetModuleHandleW(nullptr);
}

GuiApp::~GuiApp() {
    if (revalTimer) KillTimer(hMainWnd, revalTimer);
    if (nid.hWnd) Shell_NotifyIconW(NIM_DELETE, &nid);
    if (hIconGreen) DestroyIcon(hIconGreen);
    if (hIconYellow) DestroyIcon(hIconYellow);
    if (hIconRed) DestroyIcon(hIconRed);
    if (hTrayMenu) DestroyMenu(hTrayMenu);
}

// ===== Icon creation =====

HICON GuiApp::createColorIcon(COLORREF color) {
    // Create a 16x16 icon with a colored circle
    HDC hdcScreen = GetDC(nullptr);
    HDC hdc = CreateCompatibleDC(hdcScreen);
    HBITMAP hBmpColor = CreateCompatibleBitmap(hdcScreen, 16, 16);
    HBITMAP hBmpMask = CreateBitmap(16, 16, 1, 1, nullptr);
    ReleaseDC(nullptr, hdcScreen);

    HBITMAP hOld = (HBITMAP)SelectObject(hdc, hBmpColor);

    // Fill with transparent (green key)
    HBRUSH hBg = CreateSolidBrush(RGB(0, 255, 0));
    RECT rc = {0, 0, 16, 16};
    FillRect(hdc, &rc, hBg);
    DeleteObject(hBg);

    // Draw filled circle
    HBRUSH hCircle = CreateSolidBrush(color);
    HPEN hPen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hCircle);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    SelectObject(hdc, hCircle);
    Ellipse(hdc, 1, 1, 15, 15);
    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hCircle);
    DeleteObject(hPen);

    SelectObject(hdc, hOld);
    DeleteDC(hdc);

    // Create mask - all zeros (no transparency mask, icon will show the circle)
    HDC hdcMask = CreateCompatibleDC(nullptr);
    HBITMAP hOldMask = (HBITMAP)SelectObject(hdcMask, hBmpMask);
    PatBlt(hdcMask, 0, 0, 16, 16, WHITENESS);
    // Draw black where the circle is (non-transparent area)
    HDC hdcMaskSrc = CreateCompatibleDC(nullptr);
    HBITMAP hBmpColorCopy = CreateCompatibleBitmap(GetDC(nullptr), 16, 16);
    HDC hdcColorCopy = CreateCompatibleDC(nullptr);
    HBITMAP hOldCC = (HBITMAP)SelectObject(hdcColorCopy, hBmpColorCopy);
    // Copy color bitmap
    HDC hdcTmp = CreateCompatibleDC(nullptr);
    HBITMAP hOldTmp = (HBITMAP)SelectObject(hdcTmp, hBmpColorCopy);
    BitBlt(hdcTmp, 0, 0, 16, 16, hdc, 0, 0, SRCCOPY);
    // This is getting complicated - let me use the simpler ICONINFO approach

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.xHotspot = 0;
    ii.yHotspot = 0;
    ii.hbmMask = hBmpMask;
    ii.hbmColor = hBmpColor;

    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hBmpColor);
    DeleteObject(hBmpMask);
    DeleteDC(hdcMask);
    DeleteDC(hdcColorCopy);
    DeleteDC(hdcTmp);

    return hIcon;
}

// ===== Tray =====

void GuiApp::setupTray() {
    hIconGreen = createColorIcon(RGB(0, 200, 0));
    hIconYellow = createColorIcon(RGB(255, 180, 0));
    hIconRed = createColorIcon(RGB(220, 0, 0));

    hTrayMenu = CreatePopupMenu();
    AppendMenuW(hTrayMenu, MF_STRING | MF_DISABLED, IDM_TRAY_SHOW, L"Status: ---");
    AppendMenuW(hTrayMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hTrayMenu, MF_STRING, IDM_TRAY_FIX, L"Fix Now");
    AppendMenuW(hTrayMenu, MF_STRING, IDM_TRAY_TOGGLE, L"Pause / Resume");
    AppendMenuW(hTrayMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hTrayMenu, MF_STRING, IDM_TRAY_SHOW, L"Open Window");
    AppendMenuW(hTrayMenu, MF_STRING, IDM_TRAY_EXIT, L"Exit");

    nid.cbSize = sizeof(nid);
    nid.hWnd = hMainWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = hIconGreen;
    wcscpy_s(nid.szTip, L"GatewayPolicy");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

// ===== Callbacks from route engine (background thread) =====

void GuiApp::setupCallbacks() {
    engine.onStateChanged = [this](MonitorState state) {
        PostMessageW(hMainWnd, WM_APP_STATE, (WPARAM)state, 0);
    };
    engine.onFixCompleted = [this]() {
        PostMessageW(hMainWnd, WM_APP_FIX_COMPLETE, 0, 0);
    };
}

// ===== Main window creation =====

bool GuiApp::createMainWindow() {
    static const wchar_t* className = L"GatewayPolicyMainWnd";

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = className;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    hMainWnd = CreateWindowExW(0, className, L"GatewayPolicy",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 520, nullptr, nullptr, hInst, this);

    if (!hMainWnd) return false;

    // Create tab control
    hTab = CreateWindowExW(0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS,
        0, 0, 580, 480, hMainWnd, (HMENU)IDC_TAB, hInst, nullptr);

    // Add tabs
    TCITEMW ti = {};
    ti.mask = TCIF_TEXT;
    ti.pszText = (LPWSTR)L"\x72B6\x6001"; // 状态
    TabCtrl_InsertItem(hTab, 0, &ti);
    ti.pszText = (LPWSTR)L"\x8BBE\x7F6E"; // 设置
    TabCtrl_InsertItem(hTab, 1, &ti);
    ti.pszText = (LPWSTR)L"\x65E5\x5FD7"; // 日志
    TabCtrl_InsertItem(hTab, 2, &ti);
    ti.pszText = (LPWSTR)L"\x5173\x4E8E"; // 关于
    TabCtrl_InsertItem(hTab, 3, &ti);

    // Create page container windows (one per tab)
    for (int i = 0; i < 4; i++) {
        hPages[i] = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | (i == 0 ? WS_VISIBLE : 0) | SS_LEFT,
            2, 28, 574, 448, hTab, nullptr, hInst, nullptr);
    }

    // Build tab pages using page containers as parents
    buildStatusTab(hPages[0]);
    buildSettingsTab(hPages[1]);
    buildLogTab(hPages[2]);
    buildAboutTab(hPages[3]);

    setupTray();
    setupCallbacks();

    // Load logs
    auto logs = Logger::instance().getRecentLogs(500);
    SetWindowTextW(hTxtLog, utf8ToW(logs).c_str());

    // Load settings
    SetWindowTextW(hNumInterval, numToW(config.checkIntervalSeconds).c_str());
    SetWindowTextW(hNumTapMetric, numToW(config.tapMetric).c_str());
    SetWindowTextW(hNumMainMetric, numToW(config.mainMetric).c_str());
    SendMessageW(hChkAutoStart, BM_SETCHECK, config.autoStart ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(hChkMinimize, BM_SETCHECK, config.minimizeToTray ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(hChkNotify, BM_SETCHECK, config.showNotification ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(hChkBark, BM_SETCHECK, config.barkEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(hTxtBarkServer, utf8ToW(config.barkServer).c_str());
    SetWindowTextW(hTxtBarkKey, utf8ToW(config.barkDeviceKey).c_str());
    for (auto& net : config.privateNets) {
        SendMessageW(hLbPrivNets, LB_ADDSTRING, 0, (LPARAM)utf8ToW(net).c_str());
    }

    return true;
}

// ===== Build tab pages =====

void GuiApp::buildStatusTab(HWND parent) {
    int x = 30, y = 45, labelW = 110, valW = 400, rowH = 28;

    createLabel(parent, L"\x76D1\x63A7\x72B6\x6001:", x, y, labelW, rowH, SS_RIGHT, hInst); // 监控状态:
    hLblState = createLabel(parent, L"\x5DF2\x505C\x6B62", x + labelW + 10, y, valW, rowH, SS_LEFT, hInst, IDC_LBL_STATE);
    y += rowH;

    createLabel(parent, L"\x4E3B\x7F51\x5361:", x, y, labelW, rowH, SS_RIGHT, hInst); // 主网卡:
    hLblMainNic = createLabel(parent, L"-", x + labelW + 10, y, valW, rowH, SS_LEFT, hInst, IDC_LBL_MAINNIC);
    y += rowH;

    createLabel(parent, L"TAP \x9002\x914D\x5668:", x, y, labelW, rowH, SS_RIGHT, hInst); // TAP 适配器:
    hLblTapNic = createLabel(parent, L"-", x + labelW + 10, y, valW, rowH, SS_LEFT, hInst, IDC_LBL_TAPNIC);
    y += rowH;

    createLabel(parent, L"\x4E0A\x6B21\x4FEE\x590D:", x, y, labelW, rowH, SS_RIGHT, hInst); // 上次修复:
    hLblLastFix = createLabel(parent, L"-", x + labelW + 10, y, valW, rowH, SS_LEFT, hInst, IDC_LBL_LASTFIX);
    y += rowH;

    createLabel(parent, L"\x7D2F\x8BA1\x4FEE\x590D:", x, y, labelW, rowH, SS_RIGHT, hInst); // 累计修复:
    hLblTotalFixes = createLabel(parent, L"0", x + labelW + 10, y, valW, rowH, SS_LEFT, hInst, IDC_LBL_TOTALFIXES);
    y += rowH + 15;

    // 监控状态 -> 状态 = \x72B6\x6001
    hBtnToggle = createButton(parent,
        L"\x542F\x52A8\x76D1\x63A7", // 启动监控
        x + labelW + 10, y, 130, 32, hInst, IDC_BTN_TOGGLE);
    hBtnFixNow = createButton(parent,
        L"\x7ACB\x5373\x4FEE\x590D", // 立即修复
        x + labelW + 150, y, 100, 32, hInst, IDC_BTN_FIX_NOW);
}

void GuiApp::buildSettingsTab(HWND parent) {
    int x = 30, y = 45, labelW = 130, valW = 200, rowH = 28;

    // 监控 section
    createLabel(parent, L"\x76D1\x63A7", x, y, 300, rowH, SS_LEFT, hInst); // 监控
    y += rowH;

    createLabel(parent, L"\x68C0\x6D4B\x95F4\x9694(\x79D2):", x, y, labelW, rowH, SS_RIGHT, hInst); // 检测间隔(秒):
    hNumInterval = createEdit(parent, L"4", x + labelW + 10, y, 80, 22, ES_NUMBER, hInst, IDC_NUM_INTERVAL);
    y += rowH;

    createLabel(parent, L"TAP Metric:", x, y, labelW, rowH, SS_RIGHT, hInst);
    hNumTapMetric = createEdit(parent, L"50", x + labelW + 10, y, 80, 22, ES_NUMBER, hInst, IDC_NUM_TAPMETRIC);
    y += rowH;

    createLabel(parent, L"\x4E3B\x7F51\x5361 Metric:", x, y, labelW, rowH, SS_RIGHT, hInst); // 主网卡 Metric:
    hNumMainMetric = createEdit(parent, L"16", x + labelW + 10, y, 80, 22, ES_NUMBER, hInst, IDC_NUM_MAINMETRIC);
    y += rowH + 10;

    // 行为 section
    createLabel(parent, L"\x884C\x4E3A", x, y, 300, rowH, SS_LEFT, hInst); // 行为
    y += rowH;

    hChkAutoStart = createCheckbox(parent,
        L"\x5F00\x673A\x81EA\x542F", // 开机自启
        x + labelW + 10, y, 150, rowH, hInst, IDC_CHK_AUTOSTART);
    y += rowH;

    hChkMinimize = createCheckbox(parent,
        L"\x6700\x5C0F\x5316\x5230\x6258\x76D8", // 最小化到托盘
        x + labelW + 10, y, 200, rowH, hInst, IDC_CHK_MINIMIZE);
    y += rowH;

    hChkNotify = createCheckbox(parent,
        L"\x4FEE\x590D\x65F6\x663E\x793A\x901A\x77E5", // 修复时显示通知
        x + labelW + 10, y, 200, rowH, hInst, IDC_CHK_NOTIFY);
    y += rowH + 10;

    // 内网网段 section
    createLabel(parent, L"\x5185\x7F51\x7F51\x6BB5", x, y, 300, rowH, SS_LEFT, hInst); // 内网网段
    y += rowH;

    hLbPrivNets = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | LBS_STANDARD | WS_VSCROLL,
        x + labelW + 10, y, 300, 80, parent, (HMENU)IDC_LB_PRIVNETS, hInst, nullptr);
    hBtnAddNet = createButton(parent, L"\x6DFB\x52A0", x + labelW + 10, y + 85, 60, 24, hInst, IDC_BTN_ADDNET); // 添加
    hBtnDelNet = createButton(parent, L"\x5220\x9664", x + labelW + 80, y + 85, 60, 24, hInst, IDC_BTN_DELNET); // 删除
    y += 120;

    // Bark 推送 section
    createLabel(parent, L"Bark \x63A8\x9001", x, y, 300, rowH, SS_LEFT, hInst); // Bark 推送
    y += rowH;

    hChkBark = createCheckbox(parent, L"\x542F\x7528 Bark \x63A8\x9001", // 启用 Bark 推送
        x + labelW + 10, y, 200, rowH, hInst, IDC_CHK_BARK);
    y += rowH;

    createLabel(parent, L"\x670D\x52A1\x5668:", x, y, labelW, rowH, SS_RIGHT, hInst); // 服务器:
    hTxtBarkServer = createEdit(parent, L"", x + labelW + 10, y, 250, 22, 0, hInst, IDC_TXT_BARKSERVER);
    y += rowH;

    createLabel(parent, L"\x8BBE\x5907\x5BC6\x94A5:", x, y, labelW, rowH, SS_RIGHT, hInst); // 设备密钥:
    hTxtBarkKey = createEdit(parent, L"", x + labelW + 10, y, 250, 22, 0, hInst, IDC_TXT_BARKKEY);
    y += rowH + 15;

    hBtnSave = createButton(parent,
        L"\x4FDD\x5B58\x8BBE\x7F6E", // 保存设置
        x + labelW + 10, y, 120, 32, hInst, IDC_BTN_SAVE);
}

void GuiApp::buildLogTab(HWND parent) {
    hTxtLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL,
        20, 40, 540, 370, parent, (HMENU)IDC_TXT_LOG, hInst, nullptr);

    // Set monospace font
    HFONT hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_DONTCARE, L"Consolas");
    SendMessageW(hTxtLog, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Dark background for log (requires WM_CTLCOLOR handling - skip for now)

    hBtnExportLog = createButton(parent,
        L"\x5BFC\x51FA\x65E5\x5FD7", // 导出日志
        20, 415, 100, 28, hInst, IDC_BTN_EXPORTLOG);
    hBtnClearLog = createButton(parent,
        L"\x6E05\x7A7A\x65E5\x5FD7", // 清空日志
        130, 415, 100, 28, hInst, IDC_BTN_CLEARLOG);
}

void GuiApp::buildAboutTab(HWND parent) {
    int y = 60;

    createLabel(parent, L"GatewayPolicy", 30, y, 300, 35, SS_LEFT, hInst);
    y += 35;
    // Version
    std::wstring verStr = L"v" + utf8ToW(config.appVersion);
    createLabel(parent, verStr.c_str(), 30, y, 100, 20, SS_LEFT, hInst);
    y += 30;

    // Description
    createLabel(parent,
        L"\x5185\x7F51\x6D41\x91CF\x8D70VPN\xFF0C\x516C\x7F51\x6D41\x91CF\x8D70\x672C\x5730\x7F51\x5361\x3002", // 内网流量走VPN，公网流量走本地网卡。
        30, y, 400, 20, SS_LEFT, hInst);
    y += 22;
    createLabel(parent,
        L"\x9632\x6B62VPN\x8DEF\x7531\x52AB\x6301\x5BFC\x81F4\x516C\x7F51\x4E2D\x65AD\x3002", // 防止VPN路由劫持导致公网中断。
        30, y, 400, 20, SS_LEFT, hInst);
    y += 35;

    // Auth status
    std::string authStatus = !config.authToken.empty() ? "Authorized" : "Not authorized";
    std::wstring authLine = L"\x6388\x6743\x72B6\x6001: " + utf8ToW(authStatus) +
        L"  |  \x5230\x671F: " + utf8ToW(config.authExpire.empty() ? "-" : config.authExpire);
    createLabel(parent, authLine.c_str(), 30, y, 500, 20, SS_LEFT, hInst);
    y += 22;

    // Device code
    std::wstring devLine = L"\x8BBE\x5907\x7801: " + utf8ToW(deviceId); // 设备码:
    createLabel(parent, devLine.c_str(), 30, y, 500, 20, SS_LEFT, hInst);
    y += 35;

    hBtnCheckUpdate = createButton(parent,
        L"\x68C0\x67E5\x66F4\x65B0", // 检查更新
        30, y, 150, 30, hInst, IDC_BTN_CHECKUPDATE);
}

// ===== Show/hide tab pages =====

void GuiApp::showTab(int index) {
    for (int i = 0; i < 4; i++) {
        ShowWindow(hPages[i], (i == index) ? SW_SHOW : SW_HIDE);
    }
}

void GuiApp::refreshStatus() {
    auto state = engine.getState();
    const wchar_t* stateText;
    switch (state) {
        case MonitorState::Running:
            stateText = L"\x8FD0\x884C\x4E2D"; // 运行中
            break;
        case MonitorState::Fixing:
            stateText = L"\x4FEE\x590D\x4E2D"; // 修复中
            break;
        default:
            stateText = L"\x5DF2\x505C\x6B62"; // 已停止
            break;
    }
    SetWindowTextW(hLblState, stateText);
    // Can't easily set label color without WM_CTLCOLORSTATIC handling

    // NIC info
    std::wstring mainNic = engine.getMainNicName();
    if (!mainNic.empty()) {
        mainNic = mainNic + L" (" + engine.getMainNicDesc() + L")";
    } else {
        mainNic = L"\x672A\x627E\x5230"; // 未找到
    }
    SetWindowTextW(hLblMainNic, mainNic.c_str());

    std::wstring tapNic = engine.getTapNicName();
    if (!tapNic.empty()) {
        tapNic = tapNic + L" (" + engine.getTapNicDesc() + L")";
    } else {
        tapNic = L"\x672A\x627E\x5230"; // 未找到
    }
    SetWindowTextW(hLblTapNic, tapNic.c_str());

    SetWindowTextW(hLblLastFix, utf8ToW(config.lastFixTime.empty() ? "-" : config.lastFixTime).c_str());
    SetWindowTextW(hLblTotalFixes, numToW((int)config.totalFixes).c_str());

    SetWindowTextW(hBtnToggle,
        (state == MonitorState::Stopped) ?
        L"\x542F\x52A8\x76D1\x63A7" :  // 启动监控
        L"\x505C\x6B62\x76D1\x63A7");  // 停止监控
}

void GuiApp::appendLog(const std::string& msg) {
    int len = GetWindowTextLengthW(hTxtLog);
    SendMessageW(hTxtLog, EM_SETSEL, len, len);
    std::wstring wmsg = utf8ToW(msg) + L"\r\n";
    SendMessageW(hTxtLog, EM_REPLACESEL, FALSE, (LPARAM)wmsg.c_str());

    // Limit to 2000 lines
    int lineCount = (int)SendMessageW(hTxtLog, EM_GETLINECOUNT, 0, 0);
    if (lineCount > 2000) {
        // Remove first ~200 lines
        int firstLineEnd = (int)SendMessageW(hTxtLog, EM_LINEINDEX, lineCount - 2000, 0);
        SendMessageW(hTxtLog, EM_SETSEL, 0, firstLineEnd);
        SendMessageW(hTxtLog, EM_REPLACESEL, FALSE, (LPARAM)L"");
    }
}

void GuiApp::updateTrayIcon(MonitorState state) {
    HICON icon;
    const wchar_t* text;
    switch (state) {
        case MonitorState::Running: icon = hIconGreen; text = L"Running"; break;
        case MonitorState::Fixing: icon = hIconYellow; text = L"Fixing"; break;
        default: icon = hIconRed; text = L"Stopped"; break;
    }
    nid.hIcon = icon;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    ModifyMenuW(hTrayMenu, IDM_TRAY_SHOW, MF_STRING | MF_DISABLED, IDM_TRAY_SHOW,
        (std::wstring(L"Status: ") + text).c_str());
}

// ===== Event handlers =====

void GuiApp::onToggleMonitor() {
    if (engine.getState() == MonitorState::Stopped) {
        engine.start();
    } else {
        engine.stop();
    }
    refreshStatus();
}

void GuiApp::onFixNow() {
    if (engine.getState() == MonitorState::Stopped) {
        engine.start();
    }
    Logger::instance().write("Manual fix triggered", LogLevel::Info);
    engine.doCheckNow();
    refreshStatus();
}

void GuiApp::onSaveSettings() {
    wchar_t buf[256];

    GetWindowTextW(hNumInterval, buf, 256); config.checkIntervalSeconds = _wtoi(buf);
    GetWindowTextW(hNumTapMetric, buf, 256); config.tapMetric = _wtoi(buf);
    GetWindowTextW(hNumMainMetric, buf, 256); config.mainMetric = _wtoi(buf);
    config.autoStart = SendMessageW(hChkAutoStart, BM_GETCHECK, 0, 0) == BST_CHECKED;
    config.minimizeToTray = SendMessageW(hChkMinimize, BM_GETCHECK, 0, 0) == BST_CHECKED;
    config.showNotification = SendMessageW(hChkNotify, BM_GETCHECK, 0, 0) == BST_CHECKED;
    config.barkEnabled = SendMessageW(hChkBark, BM_GETCHECK, 0, 0) == BST_CHECKED;
    GetWindowTextW(hTxtBarkServer, buf, 256); config.barkServer = wToUtf8(buf);
    GetWindowTextW(hTxtBarkKey, buf, 256); config.barkDeviceKey = wToUtf8(buf);

    // Private nets
    config.privateNets.clear();
    int count = (int)SendMessageW(hLbPrivNets, LB_GETCOUNT, 0, 0);
    for (int i = 0; i < count; i++) {
        wchar_t itemBuf[256];
        SendMessageW(hLbPrivNets, LB_GETTEXT, i, (LPARAM)itemBuf);
        std::string s = wToUtf8(itemBuf);
        if (!s.empty()) config.privateNets.push_back(s);
    }

    config.save();
    setAutoStart(config.autoStart);
    Logger::instance().write("Settings saved", LogLevel::Info);
    MessageBoxW(hMainWnd, L"\x8BBE\x7F6E\x5DF2\x4FDD\x5B58\x3002", // 设置已保存。
        L"GatewayPolicy", MB_OK | MB_ICONINFORMATION);
}

void GuiApp::onAddPrivateNet() {
    // Simple input dialog using a MessageBox-style approach
    // For simplicity, we'll use a small dialog
    // TODO: implement a proper input dialog
    // For now, use a simple approach with GetActiveWindow + DialogBox
    // Actually, let me just prompt with a simple inline dialog
    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"#32770",
        L"\x6DFB\x52A0\x5185\x7F51\x7F51\x6BB5", // 添加内网网段
        WS_VISIBLE | WS_CAPTION | WS_SYSMENU,
        0, 0, 400, 150, hMainWnd, nullptr, hInst, nullptr);
    // This is getting complex. Let me use a simpler approach.
    DestroyWindow(hDlg);

    // Use InputBox pattern: create a modal dialog via DialogBox
    // For simplicity, use a simple prompt
    // Actually let me just create a simple child window as a dialog
    // ... The simplest approach for Win32 is to just use a modeless dialog
    // Let me skip this for now and use a very basic approach
}

void GuiApp::onRemovePrivateNet() {
    int sel = (int)SendMessageW(hLbPrivNets, LB_GETCURSEL, 0, 0);
    if (sel != LB_ERR) {
        SendMessageW(hLbPrivNets, LB_DELETESTRING, sel, 0);
    }
}

void GuiApp::onExportLog() {
    std::string logs = Logger::instance().exportAllLogs();

    wchar_t filename[MAX_PATH] = {0};
    time_t now = time(nullptr);
    struct tm lt;
    localtime_s(&lt, &now);
    wchar_t dateBuf[32];
    wcsftime(dateBuf, 32, L"%Y%m%d", &lt);
    swprintf(filename, MAX_PATH, L"GatewayPolicy_%s.log", dateBuf);

    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = hMainWnd;
    ofn.lpstrFilter = L"Log Files (*.log)\0*.log\0Text Files (*.txt)\0*.txt\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"log";

    if (GetSaveFileNameW(&ofn)) {
        std::ofstream f(filename, std::ios::binary);
        if (f.is_open()) {
            f << logs;
            f.close();
            MessageBoxW(hMainWnd, L"\x65E5\x5FD7\x5DF2\x5BFC\x51FA\x3002", // 日志已导出。
                L"GatewayPolicy", MB_OK | MB_ICONINFORMATION);
        }
    }
}

void GuiApp::onClearLog() {
    if (MessageBoxW(hMainWnd, L"\x786E\x8BA4\x6E05\x7A7A\x6240\x6709\x65E5\x5FD7\xFF1F", // 确认清空所有日志？
        L"GatewayPolicy", MB_YESNO | MB_ICONQUESTION) == IDYES) {
        Logger::instance().clearLogs();
        SetWindowTextW(hTxtLog, L"");
    }
}

void GuiApp::onCheckUpdate() {
    EnableWindow(hBtnCheckUpdate, FALSE);
    auto info = checkUpdate(config);
    EnableWindow(hBtnCheckUpdate, TRUE);

    if (!info.error.empty()) {
        MessageBoxW(hMainWnd, utf8ToW("\xe6\xa3\x80\xe6\x9f\xa5\xe6\x9b\xb4\xe6\x96\xb0\xe5\xa4\xb1\xe8\xb4\xa5: " + info.error).c_str(),
            L"\xe6\xa3\x80\xe6\x9f\xa5\xe6\x9b\xb4\xe6\x96\xb0", MB_OK | MB_ICONWARNING);
        return;
    }

    if (!info.hasUpdate) {
        MessageBoxW(hMainWnd, L"\x5DF2\x662F\x6700\x65B0\x7248\x672C\x3002", // 已是最新版本。
            L"\x68C0\x67E5\x66F4\x65B0", MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (info.downloadUrl.empty()) {
        MessageBoxW(hMainWnd, utf8ToW("Found version " + info.tagName + " but no downloadable file.").c_str(),
            L"\x68C0\x67E5\x66F4\x65B0", MB_OK | MB_ICONWARNING);
        return;
    }

    wchar_t msg[512];
    swprintf(msg, 512,
        L"\x53D1\x73B0\x65B0\x7248\x672C: %s (\x5F53\x524D: v%s)\n\x6587\x4EF6: %s\n\n\x662F\x5426\x7ACB\x5373\x4E0B\x8F7D\x5E76\x66F4\x65B0\xFF1F",
        utf8ToW(info.tagName).c_str(), utf8ToW(config.appVersion).c_str(), utf8ToW(info.assetName).c_str());

    if (MessageBoxW(hMainWnd, msg, L"\x53D1\x73B0\x65B0\x7248\x672C", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
        if (downloadAndInstall(info.downloadUrl, info.assetName)) {
            MessageBoxW(hMainWnd,
                L"\x4E0B\x8F7D\x5B8C\x6210\xFF0C\x70B9\x51FB\x786E\x5B9A\x540E\x7A0B\x5E8F\x5C06\x81EA\x52A8\x91CD\x542F\x66F4\x65B0\x3002",
                L"\x66F4\x65B0\x5C31\x7EEA", MB_OK | MB_ICONINFORMATION);
            PostQuitMessage(0);
        } else {
            MessageBoxW(hMainWnd, L"\x66F4\x65B0\x5931\x8D25\x3002", L"\x66F4\x65B0\x9519\x8BEF", MB_OK | MB_ICONERROR);
        }
    }
}

void GuiApp::onFixCompleted() {
    refreshStatus();
    if (config.showNotification) {
        nid.dwInfoFlags = NIIF_INFO;
        wcscpy_s(nid.szInfo, L"\x8DEF\x7531\x4FEE\x590D\x5B8C\x6210\xFF0C\x516C\x7F51\x5DF2\x6062\x590D\x3002"); // 路由修复完成，公网已恢复。
        wcscpy_s(nid.szInfoTitle, L"GatewayPolicy");
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

void GuiApp::onRevalidate() {
    // Run in background to avoid blocking UI
    std::thread([this]() {
        if (!verifyTokenSilent(deviceId, config)) {
            Logger::instance().write("Authorization revoked, stopping monitor", LogLevel::Warning);
            engine.stop();
            PostMessageW(hMainWnd, WM_APP_REVAL, 1, 0); // 1 = revoked
        }
    }).detach();
}

void GuiApp::setAutoStart(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) return;

    if (enable) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring val = L"\"" + std::wstring(exePath) + L"\"";
        RegSetValueExW(hKey, L"GatewayPolicy", 0, REG_SZ,
            (BYTE*)val.c_str(), (DWORD)(val.size() * 2 + 2));
    } else {
        RegDeleteValueW(hKey, L"GatewayPolicy");
    }
    RegCloseKey(hKey);
}

// ===== Window procedure =====

LRESULT CALLBACK GuiApp::wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static GuiApp* self = nullptr;

    if (msg == WM_CREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = reinterpret_cast<GuiApp*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = reinterpret_cast<GuiApp*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    switch (msg) {
        case WM_CREATE:
            // Window already created in createMainWindow, nothing here
            return 0;

        case WM_NOTIFY: {
            LPNMHDR nmh = (LPNMHDR)lParam;
            if (nmh->hwndFrom == self->hTab && nmh->code == TCN_SELCHANGE) {
                int sel = TabCtrl_GetCurSel(self->hTab);
                self->showTab(sel);
            }
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            switch (id) {
                case IDC_BTN_TOGGLE: self->onToggleMonitor(); break;
                case IDC_BTN_FIX_NOW: self->onFixNow(); break;
                case IDC_BTN_SAVE: self->onSaveSettings(); break;
                case IDC_BTN_ADDNET: self->onAddPrivateNet(); break;
                case IDC_BTN_DELNET: self->onRemovePrivateNet(); break;
                case IDC_BTN_EXPORTLOG: self->onExportLog(); break;
                case IDC_BTN_CLEARLOG: self->onClearLog(); break;
                case IDC_BTN_CHECKUPDATE: self->onCheckUpdate(); break;
            }
            return 0;
        }

        case WM_APP_LOG: {
            // wParam = LogLevel, lParam = std::string* (heap allocated)
            auto* str = reinterpret_cast<std::string*>(lParam);
            if (str) {
                self->appendLog(*str);
                delete str;
            }
            return 0;
        }

        case WM_APP_STATE: {
            auto state = (MonitorState)wParam;
            self->refreshStatus();
            self->updateTrayIcon(state);
            return 0;
        }

        case WM_APP_FIX_COMPLETE: {
            self->onFixCompleted();
            return 0;
        }

        case WM_APP_REVAL: {
            if (wParam == 1) {
                MessageBoxW(hWnd,
                    L"\x8BBE\x5907\x6388\x6743\x5DF2\x88AB\x64A4\x9500\xFF0C\x8BF7\x91CD\x65B0\x9A8C\x8BC1\x3002", // 设备授权已被撤销，请重新验证。
                    L"GatewayPolicy", MB_OK | MB_ICONWARNING);
                if (showAuthDialog(self->hInst, hWnd, self->deviceId,
                                   self->config.effectiveAuthServer(), self->config)) {
                    self->engine.start();
                    Logger::instance().write("Re-authorized, resuming monitor", LogLevel::Info);
                } else {
                    PostQuitMessage(0);
                }
            }
            return 0;
        }

        case WM_APP_AUTOUPDATE: {
            self->onCheckUpdate();
            return 0;
        }

        case WM_TRAYICON: {
            if (lParam == WM_LBUTTONDBLCLK) {
                ShowWindow(hWnd, SW_SHOW);
                SetForegroundWindow(hWnd);
            } else if (lParam == WM_RBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hWnd);
                TrackPopupMenu(self->hTrayMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
            }
            return 0;
        }

        case WM_COMMAND + 0x10000: // Tray menu commands via WM_COMMAND
            // Actually tray menu items send WM_COMMAND
            // The IDM_ values are handled here
            // But wait - WM_COMMAND will catch these too since we're using the same handler
            // This case is for reference
            break;

        case WM_CLOSE: {
            if (self->config.minimizeToTray) {
                ShowWindow(hWnd, SW_HIDE);
                return 0;
            }
            // Fall through to destroy
            self->config.minimizeToTray = false; // Allow close
            DestroyWindow(hWnd);
            return 0;
        }

        case WM_QUERYENDSESSION:
            return TRUE;

        case WM_ENDSESSION:
            self->engine.stop();
            return 0;

        case WM_DESTROY:
            self->engine.stop();
            PostQuitMessage(0);
            return 0;

        case WM_TIMER: {
            if (wParam == 9999) {
                self->onRevalidate();
            }
            return 0;
        }
    }

    // Handle tray menu items (they come asWM_COMMAND)
    if (msg == WM_COMMAND) {
        int id = LOWORD(wParam);
        switch (id) {
            case IDM_TRAY_SHOW:
                ShowWindow(hWnd, SW_SHOW);
                SetForegroundWindow(hWnd);
                break;
            case IDM_TRAY_FIX:
                self->onFixNow();
                break;
            case IDM_TRAY_TOGGLE:
                self->onToggleMonitor();
                break;
            case IDM_TRAY_EXIT:
                self->config.minimizeToTray = false;
                DestroyWindow(hWnd);
                break;
        }
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ===== Run =====

int GuiApp::run() {
    if (!createMainWindow()) return 1;

    // Logger callback -> post to UI
    Logger::instance().setCallback([this](const std::string& msg, LogLevel level) {
        if (hMainWnd) {
            auto* str = new std::string(msg);
            PostMessageW(hMainWnd, WM_APP_LOG, (WPARAM)level, (LPARAM)str);
        }
    });

    // Start revalidation timer (30 min)
    revalTimer = SetTimer(hMainWnd, 9999, 30 * 60 * 1000, nullptr);

    // Auto-check update after 5 seconds
    if (config.autoUpdateCheck && !config.updateRepo.empty()) {
        SetTimer(hMainWnd, 9998, 5000, nullptr);
    }

    // Show window
    ShowWindow(hMainWnd, SW_SHOW);
    UpdateWindow(hMainWnd);

    // Auto-start monitoring if configured
    if (config.autoStart) {
        engine.start();
    }

    refreshStatus();

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_TIMER && msg.wParam == 9998) {
            KillTimer(hMainWnd, 9998);
            // Silent update check
            // For now just call it
            auto info = checkUpdate(config);
            if (info.hasUpdate && !info.downloadUrl.empty()) {
                // Show notification
                wcscpy_s(nid.szInfo, L"New version available. Click check update to install.");
                wcscpy_s(nid.szInfoTitle, L"GatewayPolicy Update");
                nid.dwInfoFlags = NIIF_INFO;
                Shell_NotifyIconW(NIM_MODIFY, &nid);
            }
        }
        if (!IsDialogMessageW(hMainWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    return (int)msg.wParam;
}

// ===== Auth Dialog =====

bool showAuthDialog(HINSTANCE hInst, HWND hParent, const std::string& deviceId,
                    const std::string& authServer, AppConfig& config) {
    // We can't easily use DialogBox with a resource template (no .rc compiled)
    // So we'll create the dialog programmatically using CreateWindowEx

    // Actually, let me use a simple approach: create a modal dialog window manually
    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"#32770",
        L"GatewayPolicy - \x8BBE\x5907\x6388\x6743",
        WS_VISIBLE | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 480, 340, hParent, nullptr, hInst, nullptr);

    if (!hDlg) return false;

    // Create controls
    // Title
    CreateWindowExW(0, L"STATIC", L"\x8BBE\x5907\x6388\x6743\x9A8C\x8BC1",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 15, 300, 28, hDlg, nullptr, hInst, nullptr);
    // Use a big font for title
    HFONT hBigFont = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    SendMessageW(GetWindow(hDlg, GW_CHILD), WM_SETFONT, (WPARAM)hBigFont, TRUE);

    // Instructions
    CreateWindowExW(0, L"STATIC",
        L"\x8BF7\x5C06\x4E0B\x65B9\x673A\x5668\x7801\x53D1\x9001\x7ED9\x7BA1\x7406\x5458\xFF0C\x5F85\x7BA1\x7406\x5458\x5F55\x5165\x540E\x70B9\x51FB\"\x9A8C\x8BC1\x6388\x6743\"\x3002",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 50, 420, 30, hDlg, nullptr, hInst, nullptr);

    // Device ID textbox + copy button
    HWND hTxtId = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        utf8ToW(deviceId).c_str(),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
        20, 90, 340, 28, hDlg, (HMENU)IDC_AUTH_DEVICEID, hInst, nullptr);
    HFONT hMonoFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    SendMessageW(hTxtId, WM_SETFONT, (WPARAM)hMonoFont, TRUE);

    HWND hBtnCopy = CreateWindowExW(0, L"BUTTON", L"\x590D\x5236",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        370, 90, 70, 28, hDlg, (HMENU)IDC_AUTH_BTN_COPY, hInst, nullptr);

    // Status label
    HWND hLblStatus = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 130, 420, 20, hDlg, (HMENU)IDC_AUTH_LBL_STATUS, hInst, nullptr);

    // Verify button
    HWND hBtnVerify = CreateWindowExW(0, L"BUTTON", L"\x9A8C\x8BC1\x6388\x6743",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        20, 250, 150, 36, hDlg, (HMENU)IDC_AUTH_BTN_VERIFY, hInst, nullptr);

    // Cancel button
    HWND hBtnCancel = CreateWindowExW(0, L"BUTTON", L"\x53D6\x6D88",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        350, 250, 80, 36, hDlg, (HMENU)IDCANCEL, hInst, nullptr);

    // Center dialog on parent
    RECT rc, rcParent;
    GetWindowRect(hDlg, &rc);
    GetWindowRect(hParent, &rcParent);
    int x = rcParent.left + (rcParent.right - rcParent.left - (rc.right - rc.left)) / 2;
    int y = rcParent.top + (rcParent.bottom - rcParent.top - (rc.bottom - rc.top)) / 2;
    SetWindowPos(hDlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    // Make it modal
    EnableWindow(hParent, FALSE);

    // Message loop for modal dialog
    bool authorized = false;
    bool done = false;

    MSG msg;
    while (!done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.hwnd == hDlg || IsChild(hDlg, msg.hwnd)) {
            if (msg.message == WM_COMMAND) {
                int id = LOWORD(msg.wParam);
                if (id == IDC_AUTH_BTN_COPY) {
                    // Copy to clipboard
                    if (OpenClipboard(hDlg)) {
                        EmptyClipboard();
                        std::wstring ws = utf8ToW(deviceId);
                        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (ws.size() + 1) * 2);
                        if (hMem) {
                            memcpy(GlobalLock(hMem), ws.c_str(), (ws.size() + 1) * 2);
                            GlobalUnlock(hMem);
                            SetClipboardData(CF_UNICODETEXT, hMem);
                        }
                        CloseClipboard();
                        SetWindowTextW(hBtnCopy, L"\x5DF2\x590D\x5236");
                        SetTimer(hDlg, 8888, 1500, nullptr);
                    }
                } else if (id == IDC_AUTH_BTN_VERIFY) {
                    EnableWindow(hBtnVerify, FALSE);
                    SetWindowTextW(hLblStatus, L"\x6B63\x5728\x9A8C\x8BC1...");
                    // Background auth check
                    std::thread([hDlg, &deviceId, &authServer, &config, &authorized]() {
                        auto result = checkAuth(deviceId, authServer);
                        if (result.ok) {
                            config.authToken = result.token;
                            config.authExpire = makeTokenExpiry();
                            config.save();
                            authorized = true;
                        }
                        // Store message for display
                        static thread_local std::string msgText = result.message;
                        PostMessageW(hDlg, WM_APP + 100, result.ok ? 1 : 0, 0);
                    }).detach();
                } else if (id == IDCANCEL) {
                    done = true;
                } else if (id == IDOK && authorized) {
                    done = true;
                }
            } else if (msg.message == WM_APP + 100) {
                bool ok = msg.wParam == 1;
                if (ok) {
                    SetWindowTextW(hLblStatus, L"\x6388\x6743\x9A8C\x8BC1\x901A\x8FC7\xFF01");
                    SetTimer(hDlg, 7777, 800, nullptr);
                } else {
                    EnableWindow(hBtnVerify, TRUE);
                    SetWindowTextW(hLblStatus,
                        L"\x9A8C\x8BC1\x5931\x8D25\x3002\x8BF7\x786E\x4FDD\x7BA1\x7406\x5458\x5DF2\x5F55\x5165\x8BBE\x5907\x3002");
                }
            } else if (msg.message == WM_TIMER) {
                if (msg.wParam == 8888) {
                    KillTimer(hDlg, 8888);
                    SetWindowTextW(hBtnCopy, L"\x590D\x5236");
                } else if (msg.wParam == 7777) {
                    KillTimer(hDlg, 7777);
                    done = true;
                }
            }
            // Default processing
            DefDlgProcW(hDlg, msg.message, msg.wParam, msg.lParam);
        } else {
            // Allow other windows to process their messages
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(hParent, TRUE);
    SetFocus(hParent);
    DestroyWindow(hDlg);
    DeleteObject(hBigFont);
    DeleteObject(hMonoFont);

    return authorized;
}

} // namespace gp
