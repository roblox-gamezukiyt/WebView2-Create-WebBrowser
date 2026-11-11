#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
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
#pragma comment(lib, "dwmapi.lib")

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
#define IDM_EXT_ADD                231
#define IDM_EXT_OPEN_POPUP_BASE    6000 // 6000-6199
#define IDM_EXT_OPEN_OPTIONS_BASE  6200 // 6200-6399
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

// 機能フラグ（実行時に features.txt から読み込む。無ければビルド時の既定値）
struct Features {
    bool nav = false, tabs = false, ext = false, bookmarks = false, settings = false, devtools = false;
    int theme = DEFAULT_THEME;            // 0=デバイス設定 / 1=ホワイト / 2=ダーク
    int search = DEFAULT_SEARCH_ENGINE;   // 0=Google / 1=Bing
};
Features g_feat;
bool g_extensionsInstalled = false;
std::map<std::wstring, std::wstring> g_extDirById;   // 拡張機能ID -> extensions 内のフォルダ
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

// 機能フラグ読み込み（<exe>\\features.txt）
void LoadFeatures() {
#ifdef ENABLE_NAV_BAR
    g_feat.nav = true;
#endif
#ifdef ENABLE_TABS
    g_feat.tabs = true;
#endif
#ifdef ENABLE_EXTENSIONS
    g_feat.ext = true;
#endif
#ifdef ENABLE_BOOKMARKS
    g_feat.bookmarks = true;
#endif
#ifdef ENABLE_SETTINGS_UI
    g_feat.settings = true;
#endif
#ifdef ENABLE_DEVTOOLS
    g_feat.devtools = true;
#endif
    std::wstring content = ReadFileRaw(GetExePath() + L"\\features.txt");
    for (auto& line : SplitLines(content)) {
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = line.substr(0, eq);
        std::wstring val = line.substr(eq + 1);
        bool on = (val == L"1");
        if (key == L"nav") g_feat.nav = on;
        else if (key == L"tabs") g_feat.tabs = on;
        else if (key == L"ext") g_feat.ext = on;
        else if (key == L"bookmarks") g_feat.bookmarks = on;
        else if (key == L"settings") g_feat.settings = on;
        else if (key == L"devtools") g_feat.devtools = on;
        else if (key == L"theme") g_feat.theme = (val == L"1") ? 1 : (val == L"2") ? 2 : 0;
        else if (key == L"search") g_feat.search = (val == L"1") ? 1 : 0;
    }
}

// 設定読み書き
void LoadSettings() {
    g_darkMode = (g_feat.theme == 2);
    if (g_feat.theme == 0) {
        HKEY hKey;
        DWORD val = 1, sz = sizeof(val);
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            RegQueryValueExW(hKey, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&val, &sz);
            RegCloseKey(hKey);
        }
        g_darkMode = (val == 0);
    }
    g_searchEngine = g_feat.search;

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

// <exe>\extensions\<名前>\manifest.json がある展開済み拡張機能を探す
std::wstring ManifestStamp(const std::wstring& dir);
std::wstring ManifestStampForward(const std::wstring& dir) { return ManifestStamp(dir); }

std::vector<std::wstring> FindExtensionDirs() {
    std::vector<std::wstring> dirs;
    std::wstring base = GetExePath() + L"\\extensions";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return dirs;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.' || fd.cFileName[0] == L'_') continue;
        std::wstring dir = base + L"\\" + fd.cFileName;
        if (GetFileAttributesW((dir + L"\\manifest.json").c_str()) != INVALID_FILE_ATTRIBUTES) dirs.push_back(dir);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return dirs;
}

// 最低限の JSON 文字列抽出（manifest.json の popup / options ページ取得用）
std::wstring JsonString(const std::wstring& t, const std::wstring& key, size_t from = 0) {
    std::wstring pat = L"\"" + key + L"\"";
    size_t p = t.find(pat, from);
    while (p != std::wstring::npos) {
        size_t c = t.find_first_not_of(L" \t\r\n", p + pat.size());
        if (c != std::wstring::npos && t[c] == L':') {
            size_t q = t.find_first_not_of(L" \t\r\n", c + 1);
            if (q != std::wstring::npos && t[q] == L'"') {
                std::wstring out;
                for (size_t i = q + 1; i < t.size(); ++i) {
                    wchar_t ch = t[i];
                    if (ch == L'"') return out;
                    if (ch == L'\\' && i + 1 < t.size()) {
                        wchar_t n = t[++i];
                        if (n == L'u' && i + 4 < t.size()) {
                            out.push_back((wchar_t)wcstoul(t.substr(i + 1, 4).c_str(), nullptr, 16));
                            i += 4;
                        } else out.push_back(n);
                    } else out.push_back(ch);
                }
                return out;
            }
        }
        p = t.find(pat, p + 1);
    }
    return L"";
}

// 拡張機能の popup / 設定ページ（manifest.json から取得）。無ければ空
std::wstring ExtManifestPage(const std::wstring& id, bool popup) {
    auto it = g_extDirById.find(id);
    if (it == g_extDirById.end()) return L"";
    std::wstring m = ReadFileRaw(it->second + L"\\manifest.json");
    std::wstring page;
    if (popup) {
        page = JsonString(m, L"default_popup");
    } else {
        size_t p = m.find(L"\"options_ui\"");
        if (p != std::wstring::npos) page = JsonString(m, L"page", p);
        if (page.empty()) page = JsonString(m, L"options_page");
    }
    while (!page.empty() && (page[0] == L'/' || page[0] == L'\\')) page.erase(0, 1);
    return page;
}

// 状態ファイルの値は「更新印|拡張機能ID」。更新印が一致し、IDが記録されていれば導入済みとみなす
bool StateEntryValid(const std::wstring& value, const std::wstring& dir) {
    std::wstring stamp = ManifestStampForward(dir);
    return !stamp.empty() && value.size() > stamp.size() + 1 && value.compare(0, stamp.size() + 1, stamp + L"|") == 0;
}

std::wstring ManifestStamp(const std::wstring& dir) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW((dir + L"\\manifest.json").c_str(), GetFileExInfoStandard, &d)) return L"";
    ULONGLONG t = ((ULONGLONG)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    return std::to_wstring(t) + L"_" + std::to_wstring(d.nFileSizeLow);
}

// extensions フォルダ内の拡張機能をプロファイルに追加（新規・更新分のみ）。完了後に onDone を呼ぶ
// ※ 状態ファイル extensions.state は「フォルダ|更新印|拡張機能ID」
void InstallLocalExtensions(ICoreWebView2* wv, std::function<void()> onDone) {
    std::vector<std::wstring> dirs = FindExtensionDirs();
    ComPtr<ICoreWebView2_13> w13;
    ComPtr<ICoreWebView2Profile> prof;
    ComPtr<ICoreWebView2Profile7> p7;
    if (dirs.empty() || !wv || FAILED(wv->QueryInterface(IID_PPV_ARGS(&w13))) || !w13 ||
        FAILED(w13->get_Profile(&prof)) || !prof || FAILED(prof.As(&p7)) || !p7) {
        onDone();
        return;
    }

    FetchExtensions([dirs, p7, onDone]() {
        auto state = std::make_shared<std::map<std::wstring, std::wstring>>();
        for (auto& line : SplitLines(ReadFileRaw(GetExePath() + L"\\extensions.state"))) {
            size_t bar = line.find(L'|');
            if (bar != std::wstring::npos) (*state)[line.substr(0, bar)] = line.substr(bar + 1);
        }
        // プロファイルに1つも拡張機能が無い（データ削除後など）場合は全て入れ直す
        bool anyInstalled = !g_extensionIdCache.empty();
        auto todo = std::make_shared<std::vector<std::wstring>>();
        for (auto& d : dirs) {
            auto it = state->find(d);
            if (!anyInstalled || it == state->end() || !StateEntryValid(it->second, d)) todo->push_back(d);
        }

        auto idx = std::make_shared<size_t>(0);
        auto step = std::make_shared<std::function<void()>>();
        auto finish = [dirs, state, onDone]() {
            std::wstring content;
            for (auto& d : dirs) {
                auto it = state->find(d);
                if (it != state->end() && !it->second.empty()) content += d + L"|" + it->second + L"\r\n";
            }
            WriteFileRaw(GetExePath() + L"\\extensions.state", content);
            // 拡張機能ID -> フォルダ の対応（ポップアップ/設定画面を開くのに使う）
            g_extDirById.clear();
            for (auto& kv : *state) {
                size_t bar = kv.second.find(L'|');
                if (bar != std::wstring::npos && bar + 1 < kv.second.size()) g_extDirById[kv.second.substr(bar + 1)] = kv.first;
            }
            onDone();
        };
        *step = [todo, idx, step, p7, state, finish]() {
            if (*idx >= todo->size()) { finish(); return; }
            std::wstring dir = (*todo)[(*idx)++];
            HRESULT hr = p7->AddBrowserExtension(dir.c_str(),
                Callback<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler>(
                    [step, state, dir](HRESULT result, ICoreWebView2BrowserExtension* ext) -> HRESULT {
                        if (SUCCEEDED(result) && ext) {
                            LPWSTR extId = nullptr;
                            ext->get_Id(&extId);
                            if (extId && *extId) (*state)[dir] = ManifestStamp(dir) + L"|" + extId;
                            if (extId) CoTaskMemFree(extId);
                        }
                        (*step)();
                        return S_OK;
                    }).Get());
            if (FAILED(hr)) (*step)();
        };
        (*step)();
    });
}

// ============================================================
// ダークモード（上部バーのみ。Webページの表示には一切手を加えない）
// ============================================================
struct BarTheme {
    COLORREF bar;         // バー背景
    COLORREF text;        // 文字色
    COLORREF btn;         // ボタン背景
    COLORREF btnPressed;  // ボタン押下時
    COLORREF border;      // 枠線
    COLORREF tabSel;      // 選択中タブ
    COLORREF accent;      // 選択中タブの下線
    COLORREF editBg;      // アドレスバー背景
    COLORREF editText;    // アドレスバー文字
};

static const BarTheme THEME_LIGHT = {
    RGB(240, 240, 240), RGB(32, 32, 32), RGB(228, 228, 228), RGB(203, 203, 203),
    RGB(196, 196, 196), RGB(255, 255, 255), RGB(0, 120, 215), RGB(255, 255, 255), RGB(0, 0, 0)
};
static const BarTheme THEME_DARK = {
    RGB(32, 32, 32), RGB(235, 235, 235), RGB(51, 51, 51), RGB(78, 78, 78),
    RGB(70, 70, 70), RGB(64, 64, 64), RGB(96, 165, 250), RGB(44, 44, 44), RGB(240, 240, 240)
};

HBRUSH g_brBar = NULL;
HBRUSH g_brEdit = NULL;
HFONT  g_uiFont = NULL;

const BarTheme& Theme() { return g_darkMode ? THEME_DARK : THEME_LIGHT; }

// テーマ変更を反映（ブラシ再生成・タイトルバー・再描画）
void ApplyBarTheme(HWND hWnd) {
    const BarTheme& t = Theme();
    if (g_brBar) DeleteObject(g_brBar);
    if (g_brEdit) DeleteObject(g_brEdit);
    g_brBar = CreateSolidBrush(t.bar);
    g_brEdit = CreateSolidBrush(t.editBg);

    // タイトルバー（Windows 10 20H1+ / Windows 11。非対応環境では何も起きない）
    BOOL dark = g_darkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(hWnd, 19, &dark, sizeof(dark));   // 旧 DWMWA_USE_IMMERSIVE_DARK_MODE
    DwmSetWindowAttribute(hWnd, 20, &dark, sizeof(dark));   // DWMWA_USE_IMMERSIVE_DARK_MODE
    COLORREF cap = g_darkMode ? t.bar  : 0xFFFFFFFF;        // 0xFFFFFFFF = DWMWA_COLOR_DEFAULT
    COLORREF txt = g_darkMode ? t.text : 0xFFFFFFFF;
    DwmSetWindowAttribute(hWnd, 35, &cap, sizeof(cap));     // DWMWA_CAPTION_COLOR (Win11)
    DwmSetWindowAttribute(hWnd, 36, &txt, sizeof(txt));     // DWMWA_TEXT_COLOR (Win11)

    RedrawWindow(hWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW);
}

// ボタン（オーナードロー）
void DrawBarButton(const DRAWITEMSTRUCT* dis) {
    const BarTheme& t = Theme();
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;

    HBRUSH bg = CreateSolidBrush(pressed ? t.btnPressed : t.btn);
    FillRect(dis->hDC, &dis->rcItem, bg);
    DeleteObject(bg);
    HBRUSH bd = CreateSolidBrush(t.border);
    FrameRect(dis->hDC, &dis->rcItem, bd);
    DeleteObject(bd);

    wchar_t text[64] = L"";
    GetWindowTextW(dis->hwndItem, text, 64);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, t.text);
    HFONT oldFont = (HFONT)SelectObject(dis->hDC, g_uiFont ? g_uiFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
    RECT rc = dis->rcItem;
    DrawTextW(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dis->hDC, oldFont);
}

// ============================================================
// 自前のタブバー（幅はタイトルの長さに応じて可変 / クリック・中クリック・右クリック・ドラッグ移動）
// ============================================================
static const int TAB_PAD = 14;     // 文字の左右余白
static const int TAB_MIN = 72;     // 最小幅
static const int TAB_MAX = 240;    // 最大幅
static const int TAB_FLOOR = 36;   // 詰め込み時の下限
#define WM_APP_CLOSETAB (WM_APP + 1)
static const wchar_t* TABBAR_CLASS = L"WV2TabBar";

int  g_tabDragIndex = -1;     // マウスを押しているタブ
bool g_tabDragging = false;   // ドラッグ移動中か
int  g_tabGrabX = 0;          // タブ内でつかんだ横位置
POINT g_tabDownPt = { 0, 0 };
int  g_tabHover = -1;
bool g_tabTracking = false;

std::wstring TabLabel(int i) {
    const TabInfo& t = g_tabs[i];
    std::wstring s = t.title;
    if (s.empty()) s = GetHostFromUrl(t.url);
    if (s.empty()) s = L"新しいタブ";
    if (s.size() > 100) s = s.substr(0, 100);
    return s;
}

std::vector<int> ComputeTabWidths(HWND hWnd) {
    int count = (int)g_tabs.size();
    std::vector<int> w(count > 0 ? count : 0);
    if (count <= 0) return w;

    RECT rc;
    GetClientRect(hWnd, &rc);
    HDC hdc = GetDC(hWnd);
    HFONT oldFont = (HFONT)SelectObject(hdc, g_uiFont ? g_uiFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
    int total = 0;
    for (int i = 0; i < count; ++i) {
        std::wstring label = TabLabel(i);
        SIZE sz = { 0, 0 };
        GetTextExtentPoint32W(hdc, label.c_str(), (int)label.size(), &sz);
        int width = sz.cx + TAB_PAD * 2;
        if (width < TAB_MIN) width = TAB_MIN;
        if (width > TAB_MAX) width = TAB_MAX;
        w[i] = width;
        total += width;
    }
    SelectObject(hdc, oldFont);
    ReleaseDC(hWnd, hdc);

    // 入りきらない場合は比率を保ったまま縮める
    if (total > rc.right && rc.right > 0) {
        for (int i = 0; i < count; ++i) {
            w[i] = w[i] * rc.right / total;
            if (w[i] < TAB_FLOOR) w[i] = TAB_FLOOR;
        }
    }
    return w;
}

int TabHitTest(HWND hWnd, int px, int* outLeft = nullptr) {
    std::vector<int> w = ComputeTabWidths(hWnd);
    int x = 0;
    for (int i = 0; i < (int)w.size(); ++i) {
        if (px >= x && px < x + w[i]) {
            if (outLeft) *outLeft = x;
            return i;
        }
        x += w[i];
    }
    return -1;
}

// タブの並べ替え（g_tabs 内の位置を入れ替える。WebView は pointer で管理しているので影響なし）
void MoveTab(int from, int to) {
    int n = (int)g_tabs.size();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    TabInfo t = g_tabs[from];
    g_tabs.erase(g_tabs.begin() + from);
    g_tabs.insert(g_tabs.begin() + to, t);

    if (g_activeTabIndex == from) g_activeTabIndex = to;
    else if (from < g_activeTabIndex && g_activeTabIndex <= to) g_activeTabIndex--;
    else if (to <= g_activeTabIndex && g_activeTabIndex < from) g_activeTabIndex++;

    if (g_hTabCtrl) InvalidateRect(g_hTabCtrl, NULL, FALSE);
}

void ShowTabContextMenu(HWND hWnd, int idx) {
    enum { CMD_CLOSE = 1, CMD_LEFT, CMD_RIGHT, CMD_REOPEN };
    HMENU hMenu = CreatePopupMenu();
    int n = (int)g_tabs.size();
    if (idx >= 0) {
        AppendMenuW(hMenu, MF_STRING, CMD_CLOSE, L"このタブを閉じる");
        AppendMenuW(hMenu, MF_STRING | (idx > 0 ? MF_ENABLED : MF_GRAYED), CMD_LEFT, L"左へ移動");
        AppendMenuW(hMenu, MF_STRING | (idx < n - 1 ? MF_ENABLED : MF_GRAYED), CMD_RIGHT, L"右へ移動");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(hMenu, MF_STRING | (g_closedTabUrls.empty() ? MF_GRAYED : MF_ENABLED), CMD_REOPEN, L"閉じたタブを再度開く");

    POINT pt;
    GetCursorPos(&pt);
    int cmd = (int)TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);

    HWND parent = GetParent(hWnd);
    switch (cmd) {
    case CMD_CLOSE:  PostMessage(parent, WM_APP_CLOSETAB, (WPARAM)idx, 0); break;   // 右クリックしたタブを閉じる
    case CMD_LEFT:   MoveTab(idx, idx - 1); break;
    case CMD_RIGHT:  MoveTab(idx, idx + 1); break;
    case CMD_REOPEN: PostMessage(parent, WM_COMMAND, IDM_REOPEN_CLOSED_TAB, 0); break;
    }
}

LRESULT CALLBACK TabBarProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        int px = (short)LOWORD(lParam);
        int left = 0;
        int idx = TabHitTest(hWnd, px, &left);
        if (idx >= 0) {
            g_tabDragIndex = idx;
            g_tabDragging = false;
            g_tabGrabX = px - left;
            g_tabDownPt.x = px;
            g_tabDownPt.y = (short)HIWORD(lParam);
            SetCapture(hWnd);
            if (idx != g_activeTabIndex) SwitchToTab(idx);
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int px = (short)LOWORD(lParam);
        if (!g_tabTracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            g_tabTracking = true;
        }
        if (g_tabDragIndex >= 0 && (wParam & MK_LBUTTON)) {
            if (!g_tabDragging && abs(px - g_tabDownPt.x) >= 5) g_tabDragging = true;
            if (g_tabDragging) {
                std::vector<int> w = ComputeTabWidths(hWnd);
                int n = (int)w.size();
                int di = g_tabDragIndex;
                if (di >= 0 && di < n) {
                    // つかんだタブの中心が、他のタブの中心を越えた数 = 移動先の位置
                    int dragCenter = (px - g_tabGrabX) + w[di] / 2;
                    int x = 0, target = 0;
                    for (int j = 0; j < n; ++j) {
                        if (j == di) continue;
                        if (x + w[j] / 2 < dragCenter) target++;
                        x += w[j];
                    }
                    if (target != di) {
                        MoveTab(di, target);
                        g_tabDragIndex = target;
                    }
                }
            }
        }
        int hover = TabHitTest(hWnd, px);
        if (hover != g_tabHover) {
            g_tabHover = hover;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tabTracking = false;
        if (g_tabHover != -1) {
            g_tabHover = -1;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hWnd) ReleaseCapture();
        g_tabDragIndex = -1;
        g_tabDragging = false;
        return 0;
    case WM_CAPTURECHANGED:
        g_tabDragIndex = -1;
        g_tabDragging = false;
        return 0;

    case WM_MBUTTONUP: {   // 中クリックで、カーソル下のタブを閉じる
        int idx = TabHitTest(hWnd, (short)LOWORD(lParam));
        if (idx >= 0) PostMessage(GetParent(hWnd), WM_APP_CLOSETAB, (WPARAM)idx, 0);
        return 0;
    }
    case WM_RBUTTONUP: {   // 右クリックメニュー（カーソル下のタブが対象）
        int idx = TabHitTest(hWnd, (short)LOWORD(lParam));
        ShowTabContextMenu(hWnd, idx);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        // ダブルバッファ
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);

        const BarTheme& t = Theme();
        HBRUSH bg = CreateSolidBrush(t.bar);
        FillRect(mem, &rc, bg);
        DeleteObject(bg);

        SetBkMode(mem, TRANSPARENT);
        HFONT oldFont = (HFONT)SelectObject(mem, g_uiFont ? g_uiFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT));

        std::vector<int> widths = ComputeTabWidths(hWnd);
        int x = 0;
        for (int i = 0; i < (int)widths.size(); ++i) {
            RECT r = { x, 0, x + widths[i], rc.bottom };
            x += widths[i];

            if (i == g_activeTabIndex) {
                HBRUSH selBr = CreateSolidBrush(t.tabSel);
                FillRect(mem, &r, selBr);
                DeleteObject(selBr);
                RECT line = { r.left, r.bottom - 2, r.right, r.bottom };
                HBRUSH acc = CreateSolidBrush(t.accent);
                FillRect(mem, &line, acc);
                DeleteObject(acc);
            } else if (i == g_tabHover) {
                HBRUSH hov = CreateSolidBrush(t.btn);
                FillRect(mem, &r, hov);
                DeleteObject(hov);
            }
            // 区切り線
            RECT sep = { r.right - 1, r.top + 6, r.right, r.bottom - 6 };
            HBRUSH sepBr = CreateSolidBrush(t.border);
            FillRect(mem, &sep, sepBr);
            DeleteObject(sepBr);

            std::wstring label = TabLabel(i);
            SetTextColor(mem, t.text);
            RECT tr = r;
            tr.left += TAB_PAD; tr.right -= TAB_PAD;
            DrawTextW(mem, label.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldFont);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hWnd, &ps);
        return 0;
    }
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
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

if (g_feat.devtools) {
    AppendMenuW(hMenu, MF_STRING, IDM_SETTINGS_DEVTOOLS, L"デベロッパーツールを開く (F10)");
}

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

// Chrome ウェブストア / Microsoft Edge アドオン の拡張機能ページか
bool IsExtensionStoreUrl(const std::wstring& url) {
    std::wstring u = url;
    for (auto& c : u) c = (wchar_t)towlower(c);
    return u.find(L"chromewebstore.google.com/detail") != std::wstring::npos ||
           u.find(L"chrome.google.com/webstore/detail") != std::wstring::npos ||
           u.find(L"microsoftedge.microsoft.com/addons/detail") != std::wstring::npos;
}

// Features.exe を起動（ストアのURLを渡すと、入力済みで確認画面まで進む）
void LaunchFeaturesExe(HWND hWnd, const std::wstring& url) {
    std::wstring exe = GetExePath() + L"\\Features.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(hWnd, L"Features.exe が見つかりません。\n（rebuild_all.bat または build.bat で作り直してください）", L"拡張機能を追加", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring params = url.empty() ? L"" : (L"\"" + url + L"\"");
    ShellExecuteW(hWnd, L"open", exe.c_str(), params.empty() ? NULL : params.c_str(), GetExePath().c_str(), SW_SHOWNORMAL);
}

// 拡張機能のページ（ポップアップ/設定画面）を開く
void OpenExtensionPage(HWND hWnd, const std::wstring& id, bool popup) {
    std::wstring page = ExtManifestPage(id, popup);
    if (page.empty()) return;
    std::wstring url = L"chrome-extension://" + id + L"/" + page;
    if (g_feat.tabs) {
        CreateTab(hWnd, url);
    } else {
        TabInfo* t = GetActiveTab();
        if (t && t->webView) t->webView->Navigate(url.c_str());
    }
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
            bool hasPopup = !ExtManifestPage(id, true).empty();
            bool hasOptions = !ExtManifestPage(id, false).empty();
            if (hasPopup) AppendMenuW(hSub, MF_STRING, (UINT)(IDM_EXT_OPEN_POPUP_BASE + i), L"ポップアップを開く");
            if (hasOptions) AppendMenuW(hSub, MF_STRING, (UINT)(IDM_EXT_OPEN_OPTIONS_BASE + i), L"設定画面を開く");
            if (hasPopup || hasOptions) AppendMenuW(hSub, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hSub, MF_STRING | (disabledHere ? MF_CHECKED : MF_UNCHECKED), (UINT)(IDM_EXT_ITEM_THIS_SITE_BASE + i), L"このサイトで無効にする");
            AppendMenuW(hSub, MF_STRING | (disabledGlobally ? MF_CHECKED : MF_UNCHECKED), (UINT)(IDM_EXT_ITEM_ALL_SITES_BASE + i), L"すべてのサイトで無効にする");
            AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSub, wname.c_str());
        }
        if (g_extensionObjs.empty()) {
            AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, L"インストール済みの拡張機能はありません");
        }
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        TabInfo* curTab = GetActiveTab();
        AppendMenuW(hMenu, MF_STRING, IDM_EXT_ADD,
                    (curTab && IsExtensionStoreUrl(curTab->url)) ? L"このページの拡張機能を追加…" : L"拡張機能を追加（ストアのURLから）…");
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

if (g_feat.tabs) {
    if (g_hTabCtrl) InvalidateRect(g_hTabCtrl, NULL, FALSE);
}

    TabInfo* tab = GetActiveTab();
    if (tab && g_hEditUrl) SetWindowTextW(g_hEditUrl, tab->url.c_str());
    if (tab) {
        SetWindowTextW(g_hWndMain, tab->title.empty() ? g_appName.c_str() : tab->title.c_str());
if (g_feat.ext) {
        ApplyExtensionSettingsForHost(GetHostFromUrl(tab->url));
}
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

if (g_feat.tabs) {
    if (g_hTabCtrl) InvalidateRect(g_hTabCtrl, NULL, FALSE);
}

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
if (g_feat.ext) {
    if (options) options->put_AreBrowserExtensionsEnabled(TRUE);
}

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
                                            else if (key == VK_F10 && g_feat.devtools) {
                                                args->put_Handled(TRUE);
                                                TabInfo* t = GetActiveTab();
                                                if (t && t->webView) t->webView->OpenDevToolsWindow();
                                            }
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
if (g_feat.ext) {
                                                            ApplyExtensionSettingsForHost(GetHostFromUrl(uri));
}
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
if (g_feat.tabs) {
                                                        if (g_hTabCtrl) InvalidateRect(g_hTabCtrl, NULL, FALSE);
}
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

if (g_feat.tabs) {
                            if (g_hTabCtrl) InvalidateRect(g_hTabCtrl, NULL, FALSE);
}

                            SwitchToTab(createdIndex);

                            // 拡張機能は最初のタブで導入し、導入後にページを開く（広告ブロック等を最初から効かせる）
                            ICoreWebView2* wvForNav = newTab.webView;
                            std::wstring navUrl = url;
                            auto navigate = [wvForNav, navUrl]() { wvForNav->Navigate(navUrl.c_str()); };
                            if (g_feat.ext && !g_extensionsInstalled) {
                                g_extensionsInstalled = true;
                                InstallLocalExtensions(wvForNav, [navigate]() {
                                    FetchExtensions([]() {
                                        TabInfo* t = GetActiveTab();
                                        ApplyExtensionSettingsForHost(t ? GetHostFromUrl(t->url) : L"");
                                    });
                                    navigate();
                                });
                            } else {
                                navigate();
                            }

                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

// ウィンドウプロシージャ
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        {
            NONCLIENTMETRICSW ncm = { sizeof(ncm) };
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
            g_uiFont = CreateFontIndirectW(&ncm.lfMessageFont);
        }

        INITCOMMONCONTROLSEX icex;
        icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
        icex.dwICC = ICC_TAB_CLASSES;
        InitCommonControlsEx(&icex);

if (g_feat.tabs) {
        {
            WNDCLASSW tbc = {};
            tbc.lpfnWndProc = TabBarProc;
            tbc.hInstance = GetModuleHandle(NULL);
            tbc.lpszClassName = TABBAR_CLASS;
            tbc.hCursor = LoadCursor(NULL, IDC_ARROW);
            RegisterClassW(&tbc);
        }
        g_hTabCtrl = CreateWindowExW(0, TABBAR_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, 0, 0, 0, hWnd, (HMENU)ID_TAB_CTRL, GetModuleHandle(NULL), NULL);

        g_hBtnNewTab = CreateWindowEx(0, L"BUTTON", L"＋", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hWnd, (HMENU)ID_BTN_NEW_TAB, GetModuleHandle(NULL), NULL);
}

if (g_feat.nav) {
        CreateWindowEx(0, L"BUTTON", L"←", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hWnd, (HMENU)ID_BTN_BACK, GetModuleHandle(NULL), NULL);
        CreateWindowEx(0, L"BUTTON", L"→", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hWnd, (HMENU)ID_BTN_FORWARD, GetModuleHandle(NULL), NULL);
        CreateWindowEx(0, L"BUTTON", L"更新", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hWnd, (HMENU)ID_BTN_RELOAD, GetModuleHandle(NULL), NULL);

        g_hEditUrl = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 0, 0, hWnd, (HMENU)ID_EDIT_URL, GetModuleHandle(NULL), NULL);

        g_oldEditProc = (WNDPROC)SetWindowLongPtr(g_hEditUrl, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);
}

if (g_feat.bookmarks) {
        g_hBtnBookmark = CreateWindowEx(0, L"BUTTON", L"★", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hWnd, (HMENU)ID_BTN_BOOKMARK, GetModuleHandle(NULL), NULL);
}
if (g_feat.ext) {
        g_hBtnExtensions = CreateWindowEx(0, L"BUTTON", L"⊞", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hWnd, (HMENU)ID_BTN_EXTENSIONS, GetModuleHandle(NULL), NULL);
}
if (g_feat.settings) {
        g_hBtnSettings = CreateWindowEx(0, L"BUTTON", L"⚙", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hWnd, (HMENU)ID_BTN_SETTINGS, GetModuleHandle(NULL), NULL);
}

        // 全コントロールに共通フォントを適用し、テーマを反映
        EnumChildWindows(hWnd, [](HWND h, LPARAM) -> BOOL {
            SendMessage(h, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
            return TRUE;
        }, 0);
        ApplyBarTheme(hWnd);
        break;
    }
    case WM_ERASEBKGND: {
        if (!g_brBar) return DefWindowProc(hWnd, message, wParam, lParam);
        RECT rc;
        GetClientRect(hWnd, &rc);
        FillRect((HDC)wParam, &rc, g_brBar);
        return 1;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, Theme().editText);
        SetBkColor(hdc, Theme().editBg);
        return (LRESULT)g_brEdit;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
        if (dis && dis->CtlType == ODT_BUTTON) {
            DrawBarButton(dis);
            return TRUE;
        }
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
        if (cmdId >= IDM_EXT_OPEN_POPUP_BASE && cmdId < IDM_EXT_OPEN_POPUP_BASE + 200) {
            size_t idx = cmdId - IDM_EXT_OPEN_POPUP_BASE;
            if (idx < g_extensionIdCache.size()) OpenExtensionPage(hWnd, g_extensionIdCache[idx], true);
            break;
        }
        if (cmdId >= IDM_EXT_OPEN_OPTIONS_BASE && cmdId < IDM_EXT_OPEN_OPTIONS_BASE + 200) {
            size_t idx = cmdId - IDM_EXT_OPEN_OPTIONS_BASE;
            if (idx < g_extensionIdCache.size()) OpenExtensionPage(hWnd, g_extensionIdCache[idx], false);
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
            ApplyBarTheme(hWnd);
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
        case IDM_SETTINGS_DEVTOOLS:
            if (g_feat.devtools && tab && tab->webView) tab->webView->OpenDevToolsWindow();
            break;
        case IDM_BOOKMARK_ADD_REMOVE:
            if (tab) { ToggleBookmark(tab->url); SaveBookmarks(); }
            break;
        case IDM_EXT_ADD:
            LaunchFeaturesExe(hWnd, (tab && IsExtensionStoreUrl(tab->url)) ? tab->url : L"");
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
        case ID_ACCEL_DEVTOOLS:
            if (g_feat.devtools && tab && tab->webView) tab->webView->OpenDevToolsWindow();
            break;
        }
        break;
    }
    case WM_APP_CLOSETAB: {   // タブバーからの「閉じる」要求（中クリック・右クリックメニュー）
        CloseTab((int)wParam);
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
        if (g_brBar) DeleteObject(g_brBar);
        if (g_brEdit) DeleteObject(g_brEdit);
        if (g_uiFont) DeleteObject(g_uiFont);
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
    LoadFeatures();
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

if (g_feat.ext) {
    FetchExtensions([]() {
        TabInfo* t = GetActiveTab();
        ApplyExtensionSettingsForHost(t ? GetHostFromUrl(t->url) : L"");
    });
}

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
