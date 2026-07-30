#pragma once
#include <string>
#include <windows.h>
#include <commctrl.h>
#include "gp_config.h"
#include "gp_route.h"

namespace gp {

// Custom window messages
#define WM_APP_LOG          (WM_APP + 10)
#define WM_APP_STATE        (WM_APP + 11)
#define WM_APP_FIX_COMPLETE (WM_APP + 12)
#define WM_APP_REVAL        (WM_APP + 13)
#define WM_APP_AUTOUPDATE   (WM_APP + 14)
#define WM_TRAYICON         (WM_APP + 1)

// App icon resource ID
#define IDI_APPICON 100

// Control IDs
enum ControlIds {
    IDC_TAB = 1000,
    IDC_BTN_TOGGLE, IDC_BTN_FIX_NOW, IDC_BTN_RESTORE,
    IDC_LBL_STATE, IDC_LBL_MAINNIC, IDC_LBL_TAPNIC, IDC_LBL_LASTFIX, IDC_LBL_TOTALFIXES,
    IDC_LBL_HIJACK,
    IDC_NUM_INTERVAL, IDC_NUM_TAPMETRIC, IDC_NUM_MAINMETRIC,
    IDC_CHK_AUTOSTART, IDC_CHK_MINIMIZE, IDC_CHK_NOTIFY,
    IDC_CHK_BARK, IDC_TXT_BARKSERVER, IDC_TXT_BARKKEY,
    IDC_LB_PRIVNETS, IDC_BTN_ADDNET, IDC_BTN_DELNET,
    IDC_BTN_SAVE,
    IDC_TXT_LOG, IDC_BTN_EXPORTLOG, IDC_BTN_CLEARLOG, IDC_BTN_COPYLOG,
    IDC_BTN_CHECKUPDATE,
};

// Tray menu IDs
enum TrayIds {
    IDM_TRAY_SHOW = 2000, IDM_TRAY_FIX, IDM_TRAY_TOGGLE, IDM_TRAY_EXIT
};

// Auth dialog
enum AuthIds {
    IDC_AUTH_DEVICEID = 1050, IDC_AUTH_BTN_COPY, IDC_AUTH_BTN_VERIFY, IDC_AUTH_LBL_STATUS
};

class GuiApp {
public:
    GuiApp(AppConfig& cfg, RouteEngine& eng, const std::string& deviceId);
    ~GuiApp();

    int run(); // Message loop

    // Accessed by page subclass procedure for status color
    MonitorState currentVisualState = MonitorState::Stopped;

private:
    AppConfig& config;
    RouteEngine& engine;
    std::string deviceId;

    HWND hMainWnd = nullptr;
    HWND hTab = nullptr;
    HINSTANCE hInst = nullptr;

    // Fonts
    HFONT hGlobalFont = nullptr;  // Microsoft YaHei UI for all controls
    HFONT hLogFont = nullptr;     // Consolas for log textbox

    // Status tab controls
    HWND hLblState, hLblMainNic, hLblTapNic, hLblLastFix, hLblTotalFixes;
    HWND hLblHijack;
    HWND hBtnToggle, hBtnFixNow, hBtnRestore;

    // Settings tab controls
    HWND hNumInterval, hNumTapMetric, hNumMainMetric;
    HWND hChkAutoStart, hChkMinimize, hChkNotify;
    HWND hChkBark, hTxtBarkServer, hTxtBarkKey;
    HWND hLbPrivNets, hBtnAddNet, hBtnDelNet, hBtnSave;

    // Log tab controls
    HWND hTxtLog, hBtnExportLog, hBtnClearLog, hBtnCopyLog;

    // About tab controls
    HWND hBtnCheckUpdate;

    // Tab pages (container windows)
    HWND hPages[4] = {};

    // Tray
    NOTIFYICONDATAW nid = {};
    HICON hIconGreen = nullptr, hIconYellow = nullptr, hIconRed = nullptr;
    HMENU hTrayMenu = nullptr;

    // Revalidation timer
    UINT_PTR revalTimer = 0;

    // Window creation
    bool createMainWindow();
    void buildStatusTab(HWND parent);
    void buildSettingsTab(HWND parent);
    void buildLogTab(HWND parent);
    void buildAboutTab(HWND parent);
    void setupTray();
    void setupCallbacks();
    void applyFontsRecursive(HWND parent);

    // Helpers
    void showTab(int index);
    void refreshStatus();
    void appendLog(const std::string& msg);
    void updateTrayIcon(MonitorState state);
    void setAutoStart(bool enable);

    // Event handlers
    void onToggleMonitor();
    void onFixNow();
    void onRestoreNetwork();
    void onSaveSettings();
    void onAddPrivateNet();
    void onRemovePrivateNet();
    void onExportLog();
    void onClearLog();
    void onCopyLog();
    void onCheckUpdate();
    void onFixCompleted();
    void onRevalidate();

    // Static window procedure
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK authDlgProc(HWND, UINT, WPARAM, LPARAM);

    HICON createColorIcon(COLORREF color);
};

// Show auth dialog and return true if authorized
bool showAuthDialog(HINSTANCE hInst, HWND hParent, const std::string& deviceId,
                    const std::string& authServer, AppConfig& config);

} // namespace gp
