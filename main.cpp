#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <functional>
#include <fstream>
#include <sstream>
#include <wrl.h>
#include <wil/com.h>
#include <WebView2EnvironmentOptions.h>
#include "WebView2.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")

using namespace Microsoft::WRL;

#ifndef DEFAULT_THEME
#define DEFAULT_THEME 0            // 0=デバイス設定 / 1=ホワイト / 2=ダーク
#endif
#ifndef DEFAULT_SEARCH_ENGINE
#define DEFAULT_SEARCH_ENGINE 0    // 0=Google / 1=Bing
#endif

// UIコントロールID
#define ID_BTN_BACK       101
#define ID_BTN_FORWARD    102
#define ID_BTN_RELOAD     103
#define ID_EDIT_URL       104
#define ID_TAB_CTRL       105
#define ID_BTN_NEW_TAB    106
#define ID_BTN_BOOKMARK   107
#define ID_BTN_EXTENSIONS 108
#define ID_BTN_SETTINGS   109

// メニューID
#define IDM_REOPEN_CLOSED_TAB      201
#define IDM_SETTINGS_DARKMODE      210
#define IDM_SETTINGS_SEARCH_GOOGLE 211
#define IDM_SETTINGS_SEARCH_BING   212
#define IDM_SETTINGS_DEVTOOLS      213
#define IDM_BOOKMARK_ADD_REMOVE    220
#define IDM_EXT_DISABLE_ALL_THIS_SITE 230
#define IDM_CLOSE_TAB              240

// 可変メニューID範囲
#define IDM_BOOKMARK_ITEM_BASE     1000  // 1000-1999
#define IDM_EXT_ITEM_THIS_SITE_BASE 3000 // 3000-3199
#define IDM_EXT_ITEM_ALL_SITES_BASE 4000 // 4000-4199

// アクセラレータ用コマンドID
#define ID_ACCEL_FULLSCREEN    5001
#define ID_ACCEL_TOGGLE_CHROME 5002
#define ID_ACCEL_DEVTOOLS      5003

// グローバル構造体・変数
struct TabInfo {
    ICoreWebView2Controller* controller = nullptr;
    ICoreWebView2* webView = nullptr;
    std::wstring url;
    std::wstring title;
};

std::vector<TabInfo> g_tabs;
std::vector<std::wstring> g_closedTabUrls;
int g_activeTabIndex = 0;
HWND g_hWndMain = NULL;
HWND g_hEditUrl = NULL;
HWND g_hTabCtrl = NULL;
HWND g_hBtnNewTab = NULL;
HWND g_hBtnBookmark = NULL;
HWND g_hBtnExtensions = NULL;
HWND g_hBtnSettings = NULL;
WNDPROC g_oldEditProc = NULL;

std::wstring g_targetUrl = L"https://discord.com/app";
std::wstring g_appName = L"Discord";

bool g_darkMode = false;
int  g_searchEngine = 0;

bool g_chromeHidden = false;
bool g_isFullscreen = false;
LONG_PTR g_prevWindowStyle = 0;
WINDOWPLACEMENT g_prevPlacement = { sizeof(WINDOWPLACEMENT) };

// パーミッション（マイク・カメラなど）の設定値
bool g_allowMicrophone = true;
bool g_allowCamera = true;

std::vector<std::wstring> g_bookmarks;
std::vector<std::wstring> g_globalDisabledExt;
std::map<std::wstring, std::vector<std::wstring>> g_hostDisabledExt;
std::vector<std::wstring> g_extensionIdCache;
std::vector<ComPtr<ICoreWebView2BrowserExtension>> g_extensionObjs;

// プロトタイプ宣言
void ResizeUI(HWND hWnd);
void SwitchToTab(int index);
void CreateTab(HWND hWnd, const std::wstring& url);
void CloseTab(int index);
void ApplyExtensionSettingsForHost(const std::wstring& host);
std::wstring GetExePath();

// ============================================================
// ファイルIO / 設定読み書き
// ============================================================

std::wstring GetExePath() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(NULL, buffer, MAX_PATH);
    std::wstring path(buffer);
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"" : path.substr(0, pos);
}

std::wstring ReadFileRaw(const std::wstring& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return L"";
    std::stringstream ss;
    ss << file.rdbuf();
    std::string str = ss.str();
    if (str.empty()) return L"";

    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    if (!wstrTo.empty() && wstrTo[0] == 0xFEFF) wstrTo.erase(0, 1);
    return wstrTo;
}

void WriteFileRaw(const std::wstring& filePath, const std::wstring& content) {
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, content.c_str(), (int)content.size(), NULL, 0, NULL, NULL);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, content.c_str(), (int)content.size(), &str[0], size_needed, NULL, NULL);
    std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
    if (file.is_open()) file.write(str.data(), (std::streamsize)str.size());
}

std::vector<std::wstring> SplitLines(const std::wstring& s) {
    std::vector<std::wstring> lines;
    std::wstringstream ss(s);
    std::wstring line;
    while (std::getline(ss, line)) {
        while (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

std::wstring ReadFileToWString(const std::wstring& filePath) {
    std::wstring raw = ReadFileRaw(filePath);
    size_t first = raw.find_first_not_of(L"\r\n\t \xFEFF");
    if (first == std::wstring::npos) return L"";
    size_t last = raw.find_last_not_of(L"\r\n\t \xFEFF");
    return raw.substr(first, (last - first + 1));
}

// パーミッション設定 (permissions.txt) の読み込み・保存
void LoadPermissions() {
    std::wstring path = GetExePath() + L"\\permissions.txt";
    std::wstring content = ReadFileRaw(path);
    if (content.empty()) {
        // ファイルがない場合はデフォルト true で作成
        WriteFileRaw(path, L"mic=true\r\ncamera=true\r\n");
        g_allowMicrophone = true;
        g_allowCamera = true;
        return;
    }
    for (auto& line : SplitLines(content)) {
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = line.substr(0, eq);
        std::wstring val = line.substr(eq + 1);
        std::transform(val.begin(), val.end(), val.begin(), ::tolower);
        bool flag = (val == L"true" || val == L"1");
        if (key == L"mic" || key == L"microphone") g_allowMicrophone = flag;
        else if (key == L"camera") g_allowCamera = flag;
    }
}

// URL / ホストパーサ
std::wstring ParseUrlOrSearch(const std::wstring& input) {
    if (input.empty()) return L"about:blank";
    if (input.find(L"http://") == 0 || input.find(L"https://") == 0) return input;
    if (input.find(L".") != std::wstring::npos && input.find(L" ") == std::wstring::npos) return L"https://" + input;

    std::wstring encodedQuery;
    for (wchar_t c : input) encodedQuery += (c == L' ') ? L'+' : c;
    return (g_searchEngine == 1) ? (L"https://www.bing.com/search?q=" + encodedQuery)
                                 : (L"https://www.google.com/search?q=" + encodedQuery);
}

std::wstring GetHostFromUrl(const std::wstring& url) {
    size_t schemeEnd = url.find(L"://");
    size_t start = (schemeEnd == std::wstring::npos) ? 0 : schemeEnd + 3;
    size_t end = url.find_first_of(L"/?#", start);
    return (end == std::wstring::npos) ? url.substr(start) : url.substr(start, end - start);
}

TabInfo* GetActiveTab() {
    if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size()) return &g_tabs[g_activeTabIndex];
    return nullptr;
}

// 設定読み書き
void LoadSettings() {
    g_darkMode = (DEFAULT_THEME == 2);
    if (DEFAULT_THEME == 0) {
        HKEY hKey;
        DWORD val = 1, sz = sizeof(val);
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            RegQueryValueExW(hKey, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&val, &sz);
            RegCloseKey(hKey);
        }
        g_darkMode = (val == 0);
    }
    g_searchEngine = DEFAULT_SEARCH_ENGINE;

    std::wstring content = ReadFileRaw(GetExePath() + L"\\config.txt");
    for (auto& line : SplitLines(content)) {
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = line.substr(0, eq);
        std::wstring val = line.substr(eq + 1);
        if (key == L"darkmode") g_darkMode = (val == L"1");
        else if (key == L"searchengine") g_searchEngine = (val == L"1") ? 1 : 0;
    }
}

void SaveSettings() {
    std::wstring content = L"darkmode=" + std::wstring(g_darkMode ? L"1" : L"0") + L"\r\n"
        + L"searchengine=" + std::to_wstring(g_searchEngine) + L"\r\n";
    WriteFileRaw(GetExePath() + L"\\config.txt", content);
}

// ブックマーク
void LoadBookmarks() { g_bookmarks = SplitLines(ReadFileRaw(GetExePath() + L"\\bookmarks.txt")); }
void SaveBookmarks() {
    std::wstring content;
    for (auto& b : g_bookmarks) content += b + L"\r\n";
    WriteFileRaw(GetExePath() + L"\\bookmarks.txt", content);
}
bool IsBookmarked(const std::wstring& url) { return std::find(g_bookmarks.begin(), g_bookmarks.end(), url) != g_bookmarks.end(); }
void ToggleBookmark(const std::wstring& url) {
    auto it = std::find(g_bookmarks.begin(), g_bookmarks.end(), url);
    if (it != g_bookmarks.end()) g_bookmarks.erase(it);
    else g_bookmarks.push_back(url);
}

// 拡張機能
void LoadExtSettings() {
    g_globalDisabledExt.clear();
    g_hostDisabledExt.clear();
    std::wstring content = ReadFileRaw(GetExePath() + L"\\extsettings.txt");
    for (auto& line : SplitLines(content)) {
        if (line.rfind(L"GLOBAL=", 0) == 0) {
            std::wstring rest = line.substr(7);
            std::wstringstream ss(rest);
            std::wstring item;
            while (std::getline(ss, item, L',')) if (!item.empty()) g_globalDisabledExt.push_back(item);
        } else if (line.rfind(L"HOST ", 0) == 0) {
            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring host = line.substr(5, eq - 5);
            std::wstring rest = line.substr(eq + 1);
            std::wstringstream ss(rest);
            std::wstring item;
            while (std::getline(ss, item, L',')) if (!item.empty()) g_hostDisabledExt[host].push_back(item);
        }
    }
}

void SaveExtSettings() {
    std::wstring content = L"GLOBAL=";
    for (size_t i = 0; i < g_globalDisabledExt.size(); ++i) {
        if (i) content += L",";
        content += g_globalDisabledExt[i];
    }
    content += L"\r\n";
    for (auto& kv : g_hostDisabledExt) {
        if (kv.second.empty()) continue;
        content += L"HOST " + kv.first + L"=";
        for (size_t i = 0; i < kv.second.size(); ++i) {
            if (i) content += L",";
            content += kv.second[i];
        }
        content += L"\r\n";
    }
    WriteFileRaw(GetExePath() + L"\\extsettings.txt", content);
}

bool IsExtensionDisabledGlobally(const std::wstring& id) { return std::find(g_globalDisabledExt.begin(), g_globalDisabledExt.end(), id) != g_globalDisabledExt.end(); }
bool IsExtensionDisabledForHost(const std::wstring& id, const std::wstring& host) {
    auto it = g_hostDisabledExt.find(host);
    if (it == g_hostDisabledExt.end()) return false;
    return std::find(it->second.begin(), it->second.end(), id) != it->second.end();
}
void ToggleGlobalExtensionDisabled(const std::wstring& id) {
    auto it = std::find(g_globalDisabledExt.begin(), g_globalDisabledExt.end(), id);
    if (it != g_globalDisabledExt.end()) g_globalDisabledExt.erase(it); else g_globalDisabledExt.push_back(id);
}
void ToggleHostExtensionDisabled(const std::wstring& host, const std::wstring& id) {
    auto& vec = g_hostDisabledExt[host];
    auto it = std::find(vec.begin(), vec.end(), id);
    if (it != vec.end()) vec.erase(it); else vec.push_back(id);
}

void FetchExtensions(std::function<void()> onDone) {
    TabInfo* tab = GetActiveTab();
    if (!tab || !tab->webView) { if (onDone) onDone(); return; }
    ComPtr<ICoreWebView2_13> webview13;
    if (FAILED(tab->webView->QueryInterface(IID_PPV_ARGS(&webview13))) || !webview13) { if (onDone) onDone(); return; }
    ComPtr<ICoreWebView2Profile> profile;
    if (FAILED(webview13->get_Profile(&profile)) || !profile) { if (onDone) onDone(); return; }
    ComPtr<ICoreWebView2Profile7> profile7;
    if (FAILED(profile.As(&profile7)) || !profile7) { if (onDone) onDone(); return; }

    profile7->GetBrowserExtensions(
        Callback<ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler>(
            [onDone](HRESULT result, ICoreWebView2BrowserExtensionList* list) -> HRESULT {
                g_extensionIdCache.clear();
                g_extensionObjs.clear();
                if (SUCCEEDED(result) && list) {
                    UINT count = 0;
                    list->get_Count(&count);
                    for (UINT i = 0; i < count && i < 200; ++i) {
                        ComPtr<ICoreWebView2BrowserExtension> ext;
                        if (FAILED(list->GetValueAtIndex(i, &ext)) || !ext) continue;
                        LPWSTR id = nullptr;
                        ext->get_Id(&id);
                        g_extensionIdCache.push_back(id ? id : L"");
                        if (id) CoTaskMemFree(id);
                        g_extensionObjs.push_back(ext);
                    }
                }
                if (onDone) onDone();
                return S_OK;
            }).Get());
}

void ApplyExtensionSettingsForHost(const std::wstring& host) {
    for (size_t i = 0; i < g_extensionObjs.size(); ++i) {
        std::wstring id = g_extensionIdCache[i];
        bool shouldEnable = !IsExtensionDisabledGlobally(id) && !IsExtensionDisabledForHost(id, host);
        g_extensionObjs[i]->Enable(shouldEnable,
            Callback<ICoreWebView2BrowserExtensionEnableCompletedHandler>([](HRESULT) -> HRESULT { return S_OK; }).Get());
    }
}

// ダークモード
std::wstring BuildDarkModeInitScript() {
    std::wstring flag = g_darkMode ? L"true" : L"false";
    return L"(function(){window.__appDarkMode = " + flag + L";"
           L"function __applyDark(){var css = window.__appDarkMode ? 'html{filter:invert(1) hue-rotate(180deg) !important;background:#111 !important;}img,video,picture,canvas,svg,iframe{filter:invert(1) hue-rotate(180deg) !important;}' : '';"
           L"var s = document.getElementById('__app_darkmode_style');if(!s){s=document.createElement('style');s.id='__app_darkmode_style';(document.head||document.documentElement).appendChild(s);}"
           L"s.textContent = css;}if(document.readyState !== 'loading') __applyDark();else document.addEventListener('DOMContentLoaded', __applyDark);})();";
}

void ApplyDarkModeAllTabs() {
    std::wstring toggleScript = L"window.__appDarkMode=" + std::wstring(g_darkMode ? L"true" : L"false") + L";"
                                L"(function(){var css = window.__appDarkMode ? 'html{filter:invert(1) hue-rotate(180deg) !important;background:#111 !important;}img,video,picture,canvas,svg,iframe{filter:invert(1) hue-rotate(180deg) !important;}' : '';"
                                L"var s=document.getElementById('__app_darkmode_style');if(!s){s=document.createElement('style');s.id='__app_darkmode_style';(document.head||document.documentElement).appendChild(s);}"
                                L"s.textContent=css;})();";
    for (auto& tab : g_tabs) {
        if (tab.webView) tab.webView->ExecuteScript(toggleScript.c_str(), nullptr);
    }
}

// 全画面・UI切替
void ToggleFullscreen(HWND hWnd) {
    if (!g_isFullscreen) {
        g_prevWindowStyle = GetWindowLongPtr(hWnd, GWL_STYLE);
        g_prevPlacement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(hWnd, &g_prevPlacement);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            SetWindowLongPtr(hWnd, GWL_STYLE, g_prevWindowStyle & ~(WS_CAPTION | WS_THICKFRAME));
            SetWindowPos(hWnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        g_isFullscreen = true;
    } else {
        SetWindowLongPtr(hWnd, GWL_STYLE, g_prevWindowStyle);
        SetWindowPlacement(hWnd, &g_prevPlacement);
        SetWindowPos(hWnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        g_isFullscreen = false;
    }
    ResizeUI(hWnd);
}

void ToggleChromeHidden(HWND hWnd) {
    g_chromeHidden = !g_chromeHidden;
    ResizeUI(hWnd);
}

// メニュー表示
void ShowSettingsMenu(HWND hWnd) {
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING | (g_darkMode ? MF_CHECKED : MF_UNCHECKED), IDM_SETTINGS_DARKMODE, L"ダークモード");
    HMENU hSearchMenu = CreatePopupMenu();
    AppendMenuW(hSearchMenu, MF_STRING | (g_searchEngine == 0 ? MF_CHECKED : MF_UNCHECKED), IDM_SETTINGS_SEARCH_GOOGLE, L"Google");
    AppendMenuW(hSearchMenu, MF_STRING | (g_searchEngine == 1 ? MF_CHECKED : MF_UNCHECKED), IDM_SETTINGS_SEARCH_BING, L"Bing");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSearchMenu, L"検索エンジン");

#ifdef ENABLE_DEVTOOLS
    AppendMenuW(hMenu, MF_STRING, IDM_SETTINGS_DEVTOOLS, L"デベロッパーツールを開く (F10)");
#endif

    POINT pt; GetCursorPos(&pt);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);
}

void ShowBookmarksMenu(HWND hWnd) {
    TabInfo* tab = GetActiveTab();
    HMENU hMenu = CreatePopupMenu();
    bool bookmarked = tab && IsBookmarked(tab->url);
    AppendMenuW(hMenu, MF_STRING, IDM_BOOKMARK_ADD_REMOVE, bookmarked ? L"★ このページをブックマークから削除" : L"☆ このページをブックマークに追加");

    if (!g_bookmarks.empty()) {
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        for (size_t i = 0; i < g_bookmarks.size() && i < 999; ++i) {
            std::wstring label = g_bookmarks[i];
            if (label.size() > 60) label = label.substr(0, 60) + L"...";
            AppendMenuW(hMenu, MF_STRING, (UINT)(IDM_BOOKMARK_ITEM_BASE + i), label.c_str());
        }
    }
    POINT pt; GetCursorPos(&pt);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);
}

void ShowExtensionsMenu(HWND hWnd) {
    TabInfo* tab = GetActiveTab();
    std::wstring host = tab ? GetHostFromUrl(tab->url) : L"";

    FetchExtensions([hWnd, host]() {
        HMENU hMenu = CreatePopupMenu();
        AppendMenuW(hMenu, MF_STRING, IDM_EXT_DISABLE_ALL_THIS_SITE, L"すべての拡張機能をこのサイトで無効にする");
        if (!g_extensionObjs.empty()) AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

        for (size_t i = 0; i < g_extensionObjs.size(); ++i) {
            LPWSTR name = nullptr;
            g_extensionObjs[i]->get_Name(&name);
            std::wstring wname = name ? name : L"(不明な拡張機能)";
            if (name) CoTaskMemFree(name);

            std::wstring id = g_extensionIdCache[i];
            bool disabledHere = IsExtensionDisabledForHost(id, host);
            bool disabledGlobally = IsExtensionDisabledGlobally(id);

            HMENU hSub = CreatePopupMenu();
            AppendMenuW(hSub, MF_STRING | (disabledHere ? MF_CHECKED : MF_UNCHECKED), (UINT)(IDM_EXT_ITEM_THIS_SITE_BASE + i), L"このサイトで無効にする");
            AppendMenuW(hSub, MF_STRING | (disabledGlobally ? MF_CHECKED : MF_UNCHECKED), (UINT)(IDM_EXT_ITEM_ALL_SITES_BASE + i), L"すべてのサイトで無効にする");
            AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSub, wname.c_str());
        }
        if (g_extensionObjs.empty()) {
            AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, L"インストール済みの拡張機能はありません");
        }
        POINT pt; GetCursorPos(&pt);
        TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
        DestroyMenu(hMenu);
    });
}

LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        wchar_t buf[2048];
        GetWindowTextW(hWnd, buf, 2048);
        std::wstring navUrl = ParseUrlOrSearch(buf);
        TabInfo* tab = GetActiveTab();
        if (tab && tab->webView) tab->webView->Navigate(navUrl.c_str());
        return 0;
    }
    return CallWindowProc(g_oldEditProc, hWnd, msg, wParam, lParam);
}

// ============================================================
// レイアウト配置
// ============================================================
void ResizeUI(HWND hWnd) {
    RECT bounds;
    GetClientRect(hWnd, &bounds);
    int topOffset = 0;

    HWND hBack = GetDlgItem(hWnd, ID_BTN_BACK);
    HWND hFwd = GetDlgItem(hWnd, ID_BTN_FORWARD);
    HWND hReload = GetDlgItem(hWnd, ID_BTN_RELOAD);

    if (g_chromeHidden) {
        if (g_hTabCtrl) ShowWindow(g_hTabCtrl, SW_HIDE);
        if (g_hBtnNewTab) ShowWindow(g_hBtnNewTab, SW_HIDE);
        if (hBack) ShowWindow(hBack, SW_HIDE);
        if (hFwd) ShowWindow(hFwd, SW_HIDE);
        if (hReload) ShowWindow(hReload, SW_HIDE);
        if (g_hEditUrl) ShowWindow(g_hEditUrl, SW_HIDE);
        if (g_hBtnBookmark) ShowWindow(g_hBtnBookmark, SW_HIDE);
        if (g_hBtnExtensions) ShowWindow(g_hBtnExtensions, SW_HIDE);
        if (g_hBtnSettings) ShowWindow(g_hBtnSettings, SW_HIDE);
    } else {
        const int utilW = 32;

        // タブ機能が有効の場合のみ描画（非有効時は完全に削除・隠す）
        if (g_hTabCtrl) {
            ShowWindow(g_hTabCtrl, SW_SHOW);
            if (g_hBtnNewTab) ShowWindow(g_hBtnNewTab, SW_SHOW);

            int newTabW = 30;
            bool utilOnTabRow = (g_hEditUrl == NULL);
            int utilTotal = 0;
            if (utilOnTabRow) {
                if (g_hBtnSettings) utilTotal += utilW;
                if (g_hBtnExtensions) utilTotal += utilW;
                if (g_hBtnBookmark) utilTotal += utilW;
            }

            int tabCtrlWidth = bounds.right - newTabW - utilTotal;
            if (tabCtrlWidth < 0) tabCtrlWidth = 0;
            SetWindowPos(g_hTabCtrl, NULL, 0, 0, tabCtrlWidth, 30, SWP_NOZORDER);
            if (g_hBtnNewTab) SetWindowPos(g_hBtnNewTab, NULL, tabCtrlWidth, 0, newTabW, 30, SWP_NOZORDER);

            if (utilOnTabRow) {
                int x = tabCtrlWidth + newTabW;
                if (g_hBtnBookmark) { ShowWindow(g_hBtnBookmark, SW_SHOW); SetWindowPos(g_hBtnBookmark, NULL, x, 0, utilW, 30, SWP_NOZORDER); x += utilW; }
                if (g_hBtnExtensions) { ShowWindow(g_hBtnExtensions, SW_SHOW); SetWindowPos(g_hBtnExtensions, NULL, x, 0, utilW, 30, SWP_NOZORDER); x += utilW; }
                if (g_hBtnSettings) { ShowWindow(g_hBtnSettings, SW_SHOW); SetWindowPos(g_hBtnSettings, NULL, x, 0, utilW, 30, SWP_NOZORDER); x += utilW; }
            }
            topOffset += 30;
        }

        if (g_hEditUrl) {
            int navY = topOffset;
            int btnWidth = 40, reloadWidth = 60, padding = 5;

            if (hBack) { ShowWindow(hBack, SW_SHOW); SetWindowPos(hBack, NULL, padding, navY + 3, btnWidth, 25, SWP_NOZORDER); }
            if (hFwd) { ShowWindow(hFwd, SW_SHOW); SetWindowPos(hFwd, NULL, padding + btnWidth + 2, navY + 3, btnWidth, 25, SWP_NOZORDER); }
            if (hReload) { ShowWindow(hReload, SW_SHOW); SetWindowPos(hReload, NULL, padding + (btnWidth * 2) + 4, navY + 3, reloadWidth, 25, SWP_NOZORDER); }

            int rightX = bounds.right - padding;
            if (g_hBtnSettings) { rightX -= utilW; ShowWindow(g_hBtnSettings, SW_SHOW); SetWindowPos(g_hBtnSettings, NULL, rightX, navY + 3, utilW - 2, 25, SWP_NOZORDER); }
            if (g_hBtnExtensions) { rightX -= utilW; ShowWindow(g_hBtnExtensions, SW_SHOW); SetWindowPos(g_hBtnExtensions, NULL, rightX, navY + 3, utilW - 2, 25, SWP_NOZORDER); }
            if (g_hBtnBookmark) { rightX -= utilW; ShowWindow(g_hBtnBookmark, SW_SHOW); SetWindowPos(g_hBtnBookmark, NULL, rightX, navY + 3, utilW - 2, 25, SWP_NOZORDER); }

            int editX = padding + (btnWidth * 2) + reloadWidth + 10;
            int editWidth = rightX - editX - padding;
            if (editWidth < 50) editWidth = 50;
            ShowWindow(g_hEditUrl, SW_SHOW);
            SetWindowPos(g_hEditUrl, NULL, editX, navY + 4, editWidth, 23, SWP_NOZORDER);

            topOffset += 35;
        } else if (!g_hTabCtrl) {
            bool anyUtil = (g_hBtnSettings || g_hBtnExtensions || g_hBtnBookmark);
            if (anyUtil) {
                int rightX = bounds.right;
                if (g_hBtnSettings) { rightX -= utilW; ShowWindow(g_hBtnSettings, SW_SHOW); SetWindowPos(g_hBtnSettings, NULL, rightX, 0, utilW, 30, SWP_NOZORDER); }
                if (g_hBtnExtensions) { rightX -= utilW; ShowWindow(g_hBtnExtensions, SW_SHOW); SetWindowPos(g_hBtnExtensions, NULL, rightX, 0, utilW, 30, SWP_NOZORDER); }
                if (g_hBtnBookmark) { rightX -= utilW; ShowWindow(g_hBtnBookmark, SW_SHOW); SetWindowPos(g_hBtnBookmark, NULL, rightX, 0, utilW, 30, SWP_NOZORDER); }
                topOffset += 30;
            }
        }
    }

    for (int i = 0; i < (int)g_tabs.size(); ++i) {
        if (g_tabs[i].controller) {
            if (i == g_activeTabIndex) {
                RECT webBounds = bounds;
                webBounds.top = topOffset;
                g_tabs[i].controller->put_Bounds(webBounds);
                g_tabs[i].controller->put_IsVisible(TRUE);
            } else {
                g_tabs[i].controller->put_IsVisible(FALSE);
            }
        }
    }
}

void SwitchToTab(int index) {
    if (index < 0 || index >= (int)g_tabs.size()) return;
    g_activeTabIndex = index;

#ifdef ENABLE_TABS
    if (g_hTabCtrl) TabCtrl_SetCurSel(g_hTabCtrl, g_activeTabIndex);
#endif

    TabInfo* tab = GetActiveTab();
    if (tab && g_hEditUrl) SetWindowTextW(g_hEditUrl, tab->url.c_str());
    if (tab) {
        SetWindowTextW(g_hWndMain, tab->title.empty() ? g_appName.c_str() : tab->title.c_str());
#ifdef ENABLE_EXTENSIONS
        ApplyExtensionSettingsForHost(GetHostFromUrl(tab->url));
#endif
    }
    ResizeUI(g_hWndMain);
}

void CloseTab(int index) {
    if (index < 0 || index >= (int)g_tabs.size()) return;
    if (!g_tabs[index].url.empty()) {
        g_closedTabUrls.push_back(g_tabs[index].url);
    }

    if (g_tabs[index].webView) g_tabs[index].webView->Release();
    if (g_tabs[index].controller) g_tabs[index].controller->Release();
    g_tabs.erase(g_tabs.begin() + index);

#ifdef ENABLE_TABS
    if (g_hTabCtrl) TabCtrl_DeleteItem(g_hTabCtrl, index);
#endif

    if (g_tabs.empty()) {
        PostQuitMessage(0);
        return;
    }

    if (g_activeTabIndex >= (int)g_tabs.size()) {
        g_activeTabIndex = (int)g_tabs.size() - 1;
    }
    SwitchToTab(g_activeTabIndex);
}

// 新規タブ・WebViewの追加処理
void CreateTab(HWND hWnd, const std::wstring& url) {
    wchar_t localAppData[MAX_PATH];
    std::wstring userDataFolder = L"";
    if (SUCCEEDED(GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH))) {
        userDataFolder = std::wstring(localAppData) + L"\\" + g_appName + L"_Data";
    }

    auto options = Make<CoreWebView2EnvironmentOptions>();
#ifdef ENABLE_EXTENSIONS
    if (options) options->put_AreBrowserExtensionsEnabled(TRUE);
#endif

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataFolder.empty() ? nullptr : userDataFolder.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hWnd, url](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) return result;

                env->CreateCoreWebView2Controller(
                    hWnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hWnd, url](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) return result;

                            TabInfo newTab;
                            newTab.controller = controller;
                            newTab.controller->AddRef();
                            newTab.controller->get_CoreWebView2(&newTab.webView);

                            // キーショートカット処理
                            EventRegistrationToken accelToken;
                            controller->add_AcceleratorKeyPressed(
                                Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
                                    [hWnd](ICoreWebView2Controller* sender, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                                        COREWEBVIEW2_KEY_EVENT_KIND kind;
                                        args->get_KeyEventKind(&kind);
                                        if (kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
                                            kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) {
                                            UINT key = 0;
                                            args->get_VirtualKey(&key);
                                            bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                                            if (key == VK_F11) {
                                                args->put_Handled(TRUE);
                                                if (ctrlDown) ToggleChromeHidden(hWnd);
                                                else ToggleFullscreen(hWnd);
                                            }
#ifdef ENABLE_DEVTOOLS
                                            else if (key == VK_F10) {
                                                args->put_Handled(TRUE);
                                                TabInfo* t = GetActiveTab();
                                                if (t && t->webView) t->webView->OpenDevToolsWindow();
                                            }
#endif
                                        }
                                        return S_OK;
                                    }).Get(), &accelToken);

                            if (newTab.webView) {
                                newTab.webView->AddRef();

                                // ----------------------------------------------------
                                // ★ マイク・カメラなどのパーミッション制御処理 ★
                                // ----------------------------------------------------
                                EventRegistrationToken permToken;
                                newTab.webView->add_PermissionRequested(
                                    Callback<ICoreWebView2PermissionRequestedEventHandler>(
                                        [](ICoreWebView2* sender, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                                            COREWEBVIEW2_PERMISSION_KIND kind;
                                            args->get_PermissionKind(&kind);

                                            // 要求が発生するたびに最新の permissions.txt を読み直し
                                            LoadPermissions();

                                            if (kind == COREWEBVIEW2_PERMISSION_KIND_MICROPHONE) {
                                                args->put_State(g_allowMicrophone ? COREWEBVIEW2_PERMISSION_STATE_ALLOW : COREWEBVIEW2_PERMISSION_STATE_DENY);
                                            } else if (kind == COREWEBVIEW2_PERMISSION_KIND_CAMERA) {
                                                args->put_State(g_allowCamera ? COREWEBVIEW2_PERMISSION_STATE_ALLOW : COREWEBVIEW2_PERMISSION_STATE_DENY);
                                            }
                                            return S_OK;
                                        }).Get(), &permToken);

                                // User-Agent 設定
                                ComPtr<ICoreWebView2Settings> settings;
                                if (SUCCEEDED(newTab.webView->get_Settings(&settings))) {
                                    ComPtr<ICoreWebView2Settings2> settings2;
                                    if (SUCCEEDED(settings.Get()->QueryInterface(IID_PPV_ARGS(&settings2)))) {
                                        settings2->put_UserAgent(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36 Edg/128.0.0.0");
                                    }
                                }

                                // ダークモードCSSの自動注入
                                newTab.webView->AddScriptToExecuteOnDocumentCreated(BuildDarkModeInitScript().c_str(), nullptr);

                                // URL変更検出
                                EventRegistrationToken srcToken;
                                newTab.webView->add_SourceChanged(
                                    Callback<ICoreWebView2SourceChangedEventHandler>(
                                        [](ICoreWebView2* sender, ICoreWebView2SourceChangedEventArgs* args) -> HRESULT {
                                            LPWSTR uri;
                                            if (SUCCEEDED(sender->get_Source(&uri))) {
                                                for (size_t i = 0; i < g_tabs.size(); ++i) {
                                                    if (g_tabs[i].webView == sender) {
                                                        g_tabs[i].url = uri;
                                                        if ((int)i == g_activeTabIndex) {
                                                            if (g_hEditUrl) SetWindowTextW(g_hEditUrl, uri);
#ifdef ENABLE_EXTENSIONS
                                                            ApplyExtensionSettingsForHost(GetHostFromUrl(uri));
#endif
                                                        }
                                                        break;
                                                    }
                                                }
                                                CoTaskMemFree(uri);
                                            }
                                            return S_OK;
                                        }).Get(), &srcToken);

                                // タイトル変更検出
                                EventRegistrationToken titleToken;
                                newTab.webView->add_DocumentTitleChanged(
                                    Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                                        [](ICoreWebView2* sender, IUnknown* args) -> HRESULT {
                                            LPWSTR title;
                                            if (SUCCEEDED(sender->get_DocumentTitle(&title))) {
                                                std::wstring wtitle = title ? title : L"";
                                                for (size_t i = 0; i < g_tabs.size(); ++i) {
                                                    if (g_tabs[i].webView == sender) {
                                                        g_tabs[i].title = wtitle;
#ifdef ENABLE_TABS
                                                        if (g_hTabCtrl) {
                                                            std::wstring shortTitle = wtitle;
                                                            if (shortTitle.size() > 20) shortTitle = shortTitle.substr(0, 20) + L"...";
                                                            TCITEM tie;
                                                            tie.mask = TCIF_TEXT;
                                                            tie.pszText = (LPWSTR)shortTitle.c_str();
                                                            TabCtrl_SetItem(g_hTabCtrl, (int)i, &tie);
                                                        }
#endif
                                                        if ((int)i == g_activeTabIndex && !wtitle.empty()) {
                                                            SetWindowTextW(g_hWndMain, wtitle.c_str());
                                                        }
                                                        break;
                                                    }
                                                }
                                                if (title) CoTaskMemFree(title);
                                            }
                                            return S_OK;
                                        }).Get(), &titleToken);
                            }

                            newTab.url = url;
                            newTab.title = GetHostFromUrl(url);
                            g_tabs.push_back(newTab);

                            int createdIndex = (int)g_tabs.size() - 1;

#ifdef ENABLE_TABS
                            if (g_hTabCtrl) {
                                TCITEM tie;
                                tie.mask = TCIF_TEXT;
                                std::wstring tabName = newTab.title.empty() ? (L"Tab " + std::to_wstring(g_tabs.size())) : newTab.title;
                                tie.pszText = (LPWSTR)tabName.c_str();
                                TabCtrl_InsertItem(g_hTabCtrl, createdIndex, &tie);
                            }
#endif

                            SwitchToTab(createdIndex);
                            newTab.webView->Navigate(url.c_str());

                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

// ウィンドウプロシージャ
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icex;
        icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
        icex.dwICC = ICC_TAB_CLASSES;
        InitCommonControlsEx(&icex);

#ifdef ENABLE_TABS
        g_hTabCtrl = CreateWindowEx(0, WC_TABCONTROL, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, 0, 0, 0, hWnd, (HMENU)ID_TAB_CTRL, GetModuleHandle(NULL), NULL);

        g_hBtnNewTab = CreateWindowEx(0, L"BUTTON", L"＋", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, (HMENU)ID_BTN_NEW_TAB, GetModuleHandle(NULL), NULL);
#endif

#ifdef ENABLE_NAV_BAR
        CreateWindowEx(0, L"BUTTON", L"←", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, (HMENU)ID_BTN_BACK, GetModuleHandle(NULL), NULL);
        CreateWindowEx(0, L"BUTTON", L"→", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, (HMENU)ID_BTN_FORWARD, GetModuleHandle(NULL), NULL);
        CreateWindowEx(0, L"BUTTON", L"更新", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, (HMENU)ID_BTN_RELOAD, GetModuleHandle(NULL), NULL);

        g_hEditUrl = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 0, 0, hWnd, (HMENU)ID_EDIT_URL, GetModuleHandle(NULL), NULL);

        g_oldEditProc = (WNDPROC)SetWindowLongPtr(g_hEditUrl, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);
#endif

#ifdef ENABLE_BOOKMARKS
        g_hBtnBookmark = CreateWindowEx(0, L"BUTTON", L"★", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, (HMENU)ID_BTN_BOOKMARK, GetModuleHandle(NULL), NULL);
#endif
#ifdef ENABLE_EXTENSIONS
        g_hBtnExtensions = CreateWindowEx(0, L"BUTTON", L"⊞", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, (HMENU)ID_BTN_EXTENSIONS, GetModuleHandle(NULL), NULL);
#endif
#ifdef ENABLE_SETTINGS_UI
        g_hBtnSettings = CreateWindowEx(0, L"BUTTON", L"⚙", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, (HMENU)ID_BTN_SETTINGS, GetModuleHandle(NULL), NULL);
#endif
        break;
    }
    case WM_COMMAND: {
        TabInfo* tab = GetActiveTab();
        WORD cmdId = LOWORD(wParam);

        if (cmdId >= IDM_BOOKMARK_ITEM_BASE && cmdId < IDM_BOOKMARK_ITEM_BASE + 999) {
            size_t idx = cmdId - IDM_BOOKMARK_ITEM_BASE;
            if (idx < g_bookmarks.size() && tab && tab->webView) {
                tab->webView->Navigate(g_bookmarks[idx].c_str());
            }
            break;
        }
        if (cmdId >= IDM_EXT_ITEM_THIS_SITE_BASE && cmdId < IDM_EXT_ITEM_THIS_SITE_BASE + 200) {
            size_t idx = cmdId - IDM_EXT_ITEM_THIS_SITE_BASE;
            if (idx < g_extensionIdCache.size()) {
                std::wstring host = tab ? GetHostFromUrl(tab->url) : L"";
                ToggleHostExtensionDisabled(host, g_extensionIdCache[idx]);
                SaveExtSettings();
                ApplyExtensionSettingsForHost(host);
            }
            break;
        }
        if (cmdId >= IDM_EXT_ITEM_ALL_SITES_BASE && cmdId < IDM_EXT_ITEM_ALL_SITES_BASE + 200) {
            size_t idx = cmdId - IDM_EXT_ITEM_ALL_SITES_BASE;
            if (idx < g_extensionIdCache.size()) {
                ToggleGlobalExtensionDisabled(g_extensionIdCache[idx]);
                SaveExtSettings();
                std::wstring host = tab ? GetHostFromUrl(tab->url) : L"";
                ApplyExtensionSettingsForHost(host);
            }
            break;
        }

        switch (cmdId) {
        case ID_BTN_BACK:
            if (tab && tab->webView) tab->webView->GoBack();
            break;
        case ID_BTN_FORWARD:
            if (tab && tab->webView) tab->webView->GoForward();
            break;
        case ID_BTN_RELOAD:
            if (tab && tab->webView) tab->webView->Reload();
            break;
        case ID_BTN_NEW_TAB:
            CreateTab(hWnd, L"https://www.google.com");
            break;
        case ID_BTN_BOOKMARK:
            ShowBookmarksMenu(hWnd);
            break;
        case ID_BTN_EXTENSIONS:
            ShowExtensionsMenu(hWnd);
            break;
        case ID_BTN_SETTINGS:
            ShowSettingsMenu(hWnd);
            break;
        case IDM_CLOSE_TAB:
            CloseTab(g_activeTabIndex);
            break;
        case IDM_REOPEN_CLOSED_TAB:
            if (!g_closedTabUrls.empty()) {
                std::wstring lastUrl = g_closedTabUrls.back();
                g_closedTabUrls.pop_back();
                CreateTab(hWnd, lastUrl);
            }
            break;
        case IDM_SETTINGS_DARKMODE:
            g_darkMode = !g_darkMode;
            ApplyDarkModeAllTabs();
            SaveSettings();
            break;
        case IDM_SETTINGS_SEARCH_GOOGLE:
            g_searchEngine = 0;
            SaveSettings();
            break;
        case IDM_SETTINGS_SEARCH_BING:
            g_searchEngine = 1;
            SaveSettings();
            break;
#ifdef ENABLE_DEVTOOLS
        case IDM_SETTINGS_DEVTOOLS:
            if (tab && tab->webView) tab->webView->OpenDevToolsWindow();
            break;
#endif
        case IDM_BOOKMARK_ADD_REMOVE:
            if (tab) { ToggleBookmark(tab->url); SaveBookmarks(); }
            break;
        case IDM_EXT_DISABLE_ALL_THIS_SITE:
            if (tab) {
                std::wstring host = GetHostFromUrl(tab->url);
                for (auto& eid : g_extensionIdCache) {
                    if (!IsExtensionDisabledForHost(eid, host)) ToggleHostExtensionDisabled(host, eid);
                }
                SaveExtSettings();
                ApplyExtensionSettingsForHost(host);
            }
            break;
        case ID_ACCEL_FULLSCREEN:
            ToggleFullscreen(hWnd);
            break;
        case ID_ACCEL_TOGGLE_CHROME:
            ToggleChromeHidden(hWnd);
            break;
#ifdef ENABLE_DEVTOOLS
        case ID_ACCEL_DEVTOOLS:
            if (tab && tab->webView) tab->webView->OpenDevToolsWindow();
            break;
#endif
        }
        break;
    }
    case WM_NOTIFY: {
        LPNMHDR pnmh = (LPNMHDR)lParam;
        if (pnmh && pnmh->idFrom == ID_TAB_CTRL && pnmh->code == TCN_SELCHANGE) {
            int sel = TabCtrl_GetCurSel(g_hTabCtrl);
            SwitchToTab(sel);
        }
        break;
    }
    case WM_MBUTTONDOWN: { // マウスの中クリックでタブを閉じる
        if (g_hTabCtrl) {
            POINT pt = { LOWORD(lParam), HIWORD(lParam) };
            TCHITTESTINFO hti = { pt, 0 };
            int hit = TabCtrl_HitTest(g_hTabCtrl, &hti);
            if (hit != -1) {
                CloseTab(hit);
            }
        }
        break;
    }
    case WM_CONTEXTMENU: {
        HWND hTarget = (HWND)wParam;
        if (g_hTabCtrl && (hTarget == g_hTabCtrl || hTarget == hWnd)) {
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, IDM_CLOSE_TAB, L"このタブを閉じる");
            UINT flags = g_closedTabUrls.empty() ? MF_GRAYED : MF_ENABLED;
            AppendMenuW(hMenu, MF_STRING | flags, IDM_REOPEN_CLOSED_TAB, L"閉じたタブを再度開く");

            POINT pt; pt.x = LOWORD(lParam); pt.y = HIWORD(lParam);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
            DestroyMenu(hMenu);
        }
        break;
    }
    case WM_SIZE:
        ResizeUI(hWnd);
        break;

    case WM_DESTROY:
        for (auto& tab : g_tabs) {
            if (tab.webView) tab.webView->Release();
            if (tab.controller) tab.controller->Release();
        }
        g_tabs.clear();
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// WinMain
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    std::wstring exeDir = GetExePath();
    std::wstring loadedUrl = ReadFileToWString(exeDir + L"\\url.txt");
    std::wstring loadedName = ReadFileToWString(exeDir + L"\\name.txt");

    if (!loadedUrl.empty()) {
        if (loadedUrl.find(L"http://") == 0 || loadedUrl.find(L"https://") == 0) g_targetUrl = loadedUrl;
        else g_targetUrl = L"https://" + loadedUrl;
    }
    if (!loadedName.empty()) g_appName = loadedName;

    LoadPermissions();
    LoadSettings();
    LoadBookmarks();
    LoadExtSettings();

    const wchar_t CLASS_NAME[] = L"LightweightWebView2WindowClass";
    WNDCLASS wc = { };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClass(&wc);

    g_hWndMain = CreateWindowEx(
        0, CLASS_NAME, g_appName.c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
        NULL, NULL, hInstance, NULL
    );

    if (g_hWndMain == NULL) {
        CoUninitialize();
        return 0;
    }

    ShowWindow(g_hWndMain, nCmdShow);
    UpdateWindow(g_hWndMain);

    CreateTab(g_hWndMain, g_targetUrl);

#ifdef ENABLE_EXTENSIONS
    FetchExtensions([]() {
        TabInfo* t = GetActiveTab();
        ApplyExtensionSettingsForHost(t ? GetHostFromUrl(t->url) : L"");
    });
#endif

    ACCEL accels[] = {
        { FVIRTKEY, VK_F11, ID_ACCEL_FULLSCREEN },
        { FVIRTKEY | FCONTROL, VK_F11, ID_ACCEL_TOGGLE_CHROME },
        { FVIRTKEY, VK_F10, ID_ACCEL_DEVTOOLS },
    };
    HACCEL hAccel = CreateAcceleratorTable(accels, 3);

    MSG msg = { };
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (!TranslateAccelerator(g_hWndMain, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    DestroyAcceleratorTable(hAccel);
    CoUninitialize();
    return 0;
}