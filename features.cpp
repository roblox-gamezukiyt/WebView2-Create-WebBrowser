// Features.exe : 機能の追加・変更 / 拡張機能のインストール ツール
// アプリのフォルダ (name.txt と <name>.exe がある場所) に置いて使う。
// 変更内容は features.txt に保存され、アプリの次回起動時に反映される。

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <urlmon.h>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

// ------------------------------------------------------------
// ID
// ------------------------------------------------------------
enum {
    ID_FEAT_NAV = 401, ID_FEAT_TABS, ID_FEAT_EXT, ID_FEAT_BOOKMARKS, ID_FEAT_SETTINGS, ID_FEAT_DEVTOOLS,
    ID_COMBO_THEME = 410, ID_COMBO_SEARCH,
    ID_EXT_INSTALL_BASE = 420,   // 420 + i : おすすめ拡張機能のインストール
    ID_EXT_STATUS_BASE = 440,
    ID_BTN_OPENFOLDER = 450,
    ID_EXT_URL_EDIT = 470,
    ID_BTN_ADD_URL,
    ID_LIST_EXT,
    ID_BTN_REMOVE_SEL,
    ID_BTN_SAVE = 460,
    ID_BTN_CLOSE = 461
};
#define WM_APP_INSTALL_DONE (WM_APP + 1)
#define WM_APP_AUTO_ADD (WM_APP + 2)
// 拡張機能グループの高さ（96dpi換算）
#define EXT_GROUP_H (28 + EXT_COUNT * 34 + 34 + 22 + 20 + 98 + 36 + 10)

struct FeatureDef { int id; const wchar_t* key; const wchar_t* label; };
static const FeatureDef FEATURES[] = {
    { ID_FEAT_NAV,       L"nav",       L"アドレスバー・戻る・進む・更新ボタン" },
    { ID_FEAT_TABS,      L"tabs",      L"タブ機能" },
    { ID_FEAT_EXT,       L"ext",       L"拡張機能サポート（拡張機能の読み込み・管理ボタン）" },
    { ID_FEAT_BOOKMARKS, L"bookmarks", L"ブックマーク" },
    { ID_FEAT_SETTINGS,  L"settings",  L"設定ボタン（歯車）" },
    { ID_FEAT_DEVTOOLS,  L"devtools",  L"デベロッパーツール（F10）" },
};

struct ExtDef {
    const wchar_t* folder;   // extensions\ 内のフォルダ名
    const wchar_t* storeId;  // Chrome ウェブストアの拡張機能ID
    const wchar_t* label;
};
static const ExtDef EXTS[] = {
    { L"ublock_origin_lite",        L"ddkjiahejlhfcafbddmgiahcphecmpfh", L"uBlock Origin Lite（広告ブロック）" },
    { L"malwarebytes_browser_guard", L"ihcjicgdanjaechkgeegckofjjedodee", L"Malwarebytes Browser Guard（悪質サイト・詐欺対策）" },
};
static const int EXT_COUNT = sizeof(EXTS) / sizeof(EXTS[0]);

static std::wstring g_appName, g_exeDir, g_dataDir;
static HFONT g_font = NULL;
static int g_dpi = 96;
static HWND g_hWnd = NULL;
static std::wstring g_initialUrl;
static bool g_busy = false;
static int S(int v) { return MulDiv(v, g_dpi, 96); }

// ------------------------------------------------------------
// ユーティリティ
// ------------------------------------------------------------
static std::wstring DirOf(const std::wstring& p) {
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"" : p.substr(0, pos);
}

static std::string ReadBytes(const std::wstring& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f.is_open()) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool WriteBytes(const std::wstring& path, const char* data, size_t len) {
    std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f.write(data, (std::streamsize)len);
    return f.good();
}

static std::wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::wstring ReadTextFile(const std::wstring& path) {
    std::wstring w = Utf8ToW(ReadBytes(path));
    size_t first = w.find_first_not_of(L"\r\n\t \xFEFF");
    if (first == std::wstring::npos) return L"";
    size_t last = w.find_last_not_of(L"\r\n\t \xFEFF");
    return w.substr(first, last - first + 1);
}

static bool PathExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static bool DeletePathOnce(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        std::wstring from = p;
        from.push_back(L'\0');
        SHFILEOPSTRUCTW op = {};
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR;
        SHFileOperationW(&op);
    } else {
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(p.c_str());
    }
    return !PathExists(p);
}

static bool DeletePath(const std::wstring& p) {
    for (int i = 0; i < 4; ++i) {
        if (DeletePathOnce(p)) return true;
        Sleep(400);
    }
    return false;
}

static bool IsAppRunning() {
    std::wstring exeName = g_appName + L".exe";
    std::wstring target = g_exeDir + L"\\" + exeName;
    bool running = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName.c_str()) != 0) continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h) continue;
            wchar_t buf[MAX_PATH * 2];
            DWORD len = MAX_PATH * 2;
            if (QueryFullProcessImageNameW(h, 0, buf, &len) && _wcsicmp(buf, target.c_str()) == 0) running = true;
            CloseHandle(h);
        } while (!running && Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return running;
}

static bool RunHidden(const std::wstring& cmd, DWORD timeoutMs, DWORD* exitCode) {
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    if (!CreateProcessW(NULL, buf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return false;
    DWORD w = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1;
    if (w == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exitCode) *exitCode = code;
    return w == WAIT_OBJECT_0;
}

// ------------------------------------------------------------
// features.txt / config.txt
// ------------------------------------------------------------
struct FeatureValues {
    bool on[6] = { false, false, false, false, false, false };
    int theme = 0;
    int search = 0;
};

static FeatureValues LoadFeatureValues() {
    FeatureValues v;
    std::stringstream ss(ReadBytes(g_exeDir + L"\\features.txt"));
    std::string line;
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::wstring key = Utf8ToW(line.substr(0, eq));
        std::wstring val = Utf8ToW(line.substr(eq + 1));
        for (int i = 0; i < 6; ++i) if (key == FEATURES[i].key) v.on[i] = (val == L"1");
        if (key == L"theme") v.theme = (val == L"1") ? 1 : (val == L"2") ? 2 : 0;
        if (key == L"search") v.search = (val == L"1") ? 1 : 0;
    }
    return v;
}

static bool SaveFeatureValues(const FeatureValues& v) {
    std::string out;
    for (int i = 0; i < 6; ++i) {
        out += std::string(FEATURES[i].key, FEATURES[i].key + wcslen(FEATURES[i].key)) + "=" + (v.on[i] ? "1" : "0") + "\r\n";
    }
    out += "theme=" + std::to_string(v.theme) + "\r\n";
    out += "search=" + std::to_string(v.search) + "\r\n";
    return WriteBytes(g_exeDir + L"\\features.txt", out.data(), out.size());
}

// アプリ側が config.txt に保存した値（歯車メニューでの切り替え結果）を消して、features.txt の値を優先させる
static void StripConfigKey(const char* key) {
    std::wstring path = g_exeDir + L"\\config.txt";
    std::string data = ReadBytes(path);
    if (data.empty()) return;
    std::stringstream ss(data);
    std::string line, out;
    std::string prefix = std::string(key) + "=";
    while (std::getline(ss, line)) {
        std::string t = line;
        while (!t.empty() && (t.back() == '\r' || t.back() == '\n')) t.pop_back();
        if (t.rfind(prefix, 0) == 0) continue;
        out += t + "\r\n";
    }
    WriteBytes(path, out.data(), out.size());
}

// ------------------------------------------------------------
// 拡張機能のダウンロード・展開
// ------------------------------------------------------------
static bool CrxToZip(const std::wstring& crx, const std::wstring& zip) {
    std::string d = ReadBytes(crx);
    if (d.size() < 16 || memcmp(d.data(), "Cr24", 4) != 0) return false;
    auto u32 = [&](size_t off) { unsigned int v; memcpy(&v, d.data() + off, 4); return v; };
    unsigned int ver = u32(4);
    size_t off = 0;
    if (ver == 3) off = 12 + (size_t)u32(8);
    else if (ver == 2) off = 16 + (size_t)u32(8) + (size_t)u32(12);
    else return false;
    if (off >= d.size()) return false;
    return WriteBytes(zip, d.data() + off, d.size() - off);
}

static std::wstring ExtDirOf(const std::wstring& folder) { return g_exeDir + L"\\extensions\\" + folder; }
static std::wstring ExtDir(int i) { return ExtDirOf(EXTS[i].folder); }
static bool IsExtInstalled(int i) { return PathExists(ExtDir(i) + L"\\manifest.json"); }

// --- manifest.json から名前を読む（最低限の JSON 文字列抽出） ---
static std::wstring JsonString(const std::wstring& t, const std::wstring& key, size_t from = 0) {
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
                        } else if (n == L'n') out.push_back(L' ');
                        else out.push_back(n);
                    } else out.push_back(ch);
                }
                return out;
            }
        }
        p = t.find(pat, p + 1);
    }
    return L"";
}

static std::wstring ResolveMsg(const std::wstring& dir, const std::wstring& val) {
    if (val.size() < 8 || val.rfind(L"__MSG_", 0) != 0) return val;
    std::wstring key = val.substr(6, val.size() - 8);   // __MSG_xxx__
    const wchar_t* locs[] = { L"ja", L"en", L"en_US", L"en_GB" };
    for (auto loc : locs) {
        std::wstring text = Utf8ToW(ReadBytes(dir + L"\\_locales\\" + loc + L"\\messages.json"));
        if (text.empty()) continue;
        size_t p = text.find(L"\"" + key + L"\"");
        if (p == std::wstring::npos) continue;
        std::wstring m = JsonString(text, L"message", p);
        if (!m.empty()) return m;
    }
    return val;
}

static std::wstring ExtDisplayName(const std::wstring& folder) {
    std::wstring dir = ExtDirOf(folder);
    std::wstring manifest = Utf8ToW(ReadBytes(dir + L"\\manifest.json"));
    std::wstring name = ResolveMsg(dir, JsonString(manifest, L"name"));
    return name.empty() ? folder : name;
}

// --- URL（Chrome ウェブストア / Microsoft Edge アドオン）または拡張機能IDから ID を取り出す ---
// source: 0 = Chrome ウェブストア, 1 = Microsoft Edge アドオン
static bool ParseExtensionId(std::wstring s, std::wstring& id, int& source) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    size_t b = s.find_last_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return false;
    s = s.substr(a, b - a + 1);

    std::wstring lower = s;
    for (auto& c : lower) c = (wchar_t)towlower(c);
    source = (lower.find(L"microsoftedge.microsoft.com") != std::wstring::npos) ? 1 : 0;

    std::wstring found;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(L'/', start);
        if (end == std::wstring::npos) end = s.size();
        std::wstring seg = s.substr(start, end - start);
        size_t cut = seg.find_first_of(L"?#");
        if (cut != std::wstring::npos) seg = seg.substr(0, cut);
        if (seg.size() == 32) {
            bool ok = true;
            for (wchar_t c : seg) { if (c < L'a' || c > L'p') { ok = false; break; } }
            if (ok) found = seg;
        }
        start = end + 1;
    }
    if (found.empty()) return false;
    id = found;
    return true;
}

static bool DownloadCrx(int source, const std::wstring& id, const std::wstring& crx) {
    std::wstring url;
    if (source == 1) {
        url = L"https://edge.microsoft.com/extensionwebstorebase/v1/crx?response=redirect&prodversion=130.0.0.0"
              L"&acceptformat=crx3&x=id%3D" + id + L"%26installsource%3Dondemand%26uc";
    } else {
        url = L"https://clients2.google.com/service/update2/crx?response=redirect&prodversion=130.0.0.0"
              L"&acceptformat=crx2,crx3&x=id%3D" + id + L"%26installsource%3Dondemand%26uc";
    }
    url += L"&t=" + std::to_wstring(GetTickCount64());
    DeleteFileW(crx.c_str());
    if (FAILED(URLDownloadToFileW(NULL, url.c_str(), crx.c_str(), 0, NULL))) return false;
    std::string head = ReadBytes(crx);
    return head.size() > 16 && memcmp(head.data(), "Cr24", 4) == 0;
}

static bool InstallExtension(const std::wstring& folder, const std::wstring& id, int primary, std::wstring& err) {
    CreateDirectoryW((g_exeDir + L"\\extensions").c_str(), NULL);

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring crx = std::wstring(tmp) + folder + L".crx";
    std::wstring zip = std::wstring(tmp) + folder + L".zip";
    DeleteFileW(zip.c_str());

    // 指定のストアで見つからなければ、もう一方のストアも試す
    bool got = DownloadCrx(primary, id, crx) || DownloadCrx(primary == 0 ? 1 : 0, id, crx);
    if (!got) {
        err = L"ダウンロードに失敗しました。\nネットワーク接続と、URL/IDが正しいか確認してください。";
        DeleteFileW(crx.c_str());
        return false;
    }
    if (!CrxToZip(crx, zip)) {
        err = L"ダウンロードしたファイルの形式を解釈できませんでした。";
        DeleteFileW(crx.c_str());
        return false;
    }

    std::wstring dest = ExtDirOf(folder);
    DeletePath(dest);
    CreateDirectoryW(dest.c_str(), NULL);

    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    DWORD code = 1;
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + dest + L"\"";
    bool ok = RunHidden(cmd, 60000, &code) && code == 0 && PathExists(dest + L"\\manifest.json");
    if (!ok) {
        // tar が使えない場合は PowerShell で展開
        cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" + zip +
              L"' -DestinationPath '" + dest + L"' -Force\"";
        ok = RunHidden(cmd, 120000, &code) && code == 0 && PathExists(dest + L"\\manifest.json");
    }
    DeleteFileW(crx.c_str());
    DeleteFileW(zip.c_str());
    if (!ok) {
        err = L"ファイルの展開に失敗しました。";
        DeletePath(dest);
        return false;
    }
    DeletePath(dest + L"\\_metadata");   // 先頭が _ のフォルダは WebView2 が拒否するため除去
    return true;
}

struct InstallJob {
    std::wstring folder, id;
    int source = 0;
    bool ok = false;
    std::wstring error;
};
static InstallJob g_job;

static DWORD WINAPI InstallThread(LPVOID) {
    g_job.ok = InstallExtension(g_job.folder, g_job.id, g_job.source, g_job.error);
    PostMessageW(g_hWnd, WM_APP_INSTALL_DONE, 0, 0);
    return 0;
}

// ------------------------------------------------------------
// UI
// ------------------------------------------------------------
static bool IsChecked(HWND h, int id) { return SendDlgItemMessageW(h, id, BM_GETCHECK, 0, 0) == BST_CHECKED; }

static std::vector<std::wstring> g_listFolders;

static void RefreshExtList(HWND hWnd) {
    HWND lb = GetDlgItem(hWnd, ID_LIST_EXT);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    g_listFolders.clear();

    std::wstring base = g_exeDir + L"\\extensions";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.' || fd.cFileName[0] == L'_') continue;
        std::wstring folder = fd.cFileName;
        if (!PathExists(ExtDirOf(folder) + L"\\manifest.json")) continue;
        std::wstring text = ExtDisplayName(folder) + L"　[" + folder + L"]";
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)text.c_str());
        g_listFolders.push_back(folder);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void RefreshExtStatus(HWND hWnd) {
    for (int i = 0; i < EXT_COUNT; ++i) {
        bool inst = IsExtInstalled(i);
        std::wstring text = std::wstring(EXTS[i].label) + (inst ? L"　✔ インストール済み" : L"　未インストール");
        SetDlgItemTextW(hWnd, ID_EXT_STATUS_BASE + i, text.c_str());
        SetDlgItemTextW(hWnd, ID_EXT_INSTALL_BASE + i, inst ? L"再インストール" : L"インストール");
        EnableWindow(GetDlgItem(hWnd, ID_EXT_INSTALL_BASE + i), !g_busy);
    }
    EnableWindow(GetDlgItem(hWnd, ID_BTN_ADD_URL), !g_busy);
    EnableWindow(GetDlgItem(hWnd, ID_BTN_REMOVE_SEL), !g_busy);
    RefreshExtList(hWnd);
}

static void SetBusy(HWND hWnd, bool busy) {
    g_busy = busy;
    EnableWindow(GetDlgItem(hWnd, ID_BTN_SAVE), !busy);
    RefreshExtStatus(hWnd);
}

static FeatureValues ReadUi(HWND hWnd) {
    FeatureValues v;
    for (int i = 0; i < 6; ++i) v.on[i] = IsChecked(hWnd, FEATURES[i].id);
    int t = (int)SendDlgItemMessageW(hWnd, ID_COMBO_THEME, CB_GETCURSEL, 0, 0);
    int s = (int)SendDlgItemMessageW(hWnd, ID_COMBO_SEARCH, CB_GETCURSEL, 0, 0);
    v.theme = t < 0 ? 0 : t;
    v.search = s < 0 ? 0 : s;
    return v;
}

static FeatureValues g_loaded;

static bool DoSave(HWND hWnd, bool quiet) {
    FeatureValues v = ReadUi(hWnd);
    if (!SaveFeatureValues(v)) {
        MessageBoxW(hWnd, L"features.txt を保存できませんでした。", L"エラー", MB_OK | MB_ICONERROR);
        return false;
    }
    if (v.theme != g_loaded.theme) StripConfigKey("darkmode");
    if (v.search != g_loaded.search) StripConfigKey("searchengine");
    g_loaded = v;
    if (!quiet) {
        std::wstring msg = L"保存しました。";
        msg += IsAppRunning() ? L"\n実行中の「" + g_appName + L"」を再起動すると反映されます。" : L"\n次回起動時に反映されます。";
        MessageBoxW(hWnd, msg.c_str(), L"機能設定", MB_OK | MB_ICONINFORMATION);
    }
    return true;
}

static void StartInstall(HWND hWnd, const std::wstring& folder, const std::wstring& id, int source, const std::wstring& label) {
    if (g_busy) return;
    std::wstring msg = label + L"\nをダウンロードしてインストールします。\nよろしいですか？";
    if (MessageBoxW(hWnd, msg.c_str(), L"拡張機能のインストール", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    g_job = InstallJob();
    g_job.folder = folder;
    g_job.id = id;
    g_job.source = source;
    SetBusy(hWnd, true);
    HANDLE th = CreateThread(NULL, 0, InstallThread, NULL, 0, NULL);
    if (th) CloseHandle(th);
    else SetBusy(hWnd, false);
}

static void OnInstallRecommended(HWND hWnd, int i) {
    StartInstall(hWnd, EXTS[i].folder, EXTS[i].storeId, 0, EXTS[i].label);
}

static void OnAddUrl(HWND hWnd) {
    if (g_busy) return;
    wchar_t buf[2048] = L"";
    GetDlgItemTextW(hWnd, ID_EXT_URL_EDIT, buf, 2048);
    std::wstring id;
    int source = 0;
    if (!ParseExtensionId(buf, id, source)) {
        MessageBoxW(hWnd, L"URL または拡張機能ID を読み取れませんでした。\n\n例:\nhttps://chromewebstore.google.com/detail/…/(32文字のID)\nhttps://microsoftedge.microsoft.com/addons/detail/…/(32文字のID)",
                    L"拡張機能を追加", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring label = L"拡張機能 ID: " + id + (source == 1 ? L"（Microsoft Edge アドオン）" : L"（Chrome ウェブストア）");
    StartInstall(hWnd, id, id, source, label);
}

static void OnRemoveSelected(HWND hWnd) {
    if (g_busy) return;
    int sel = (int)SendDlgItemMessageW(hWnd, ID_LIST_EXT, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g_listFolders.size()) {
        MessageBoxW(hWnd, L"削除する拡張機能を一覧から選択してください。", L"拡張機能の削除", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (IsAppRunning()) {
        MessageBoxW(hWnd, L"アプリが実行中です。終了してから削除してください。", L"拡張機能の削除", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring folder = g_listFolders[sel];
    std::wstring msg = ExtDisplayName(folder) + L"\nを削除します。よろしいですか？";
    if (MessageBoxW(hWnd, msg.c_str(), L"拡張機能の削除", MB_YESNO | MB_ICONWARNING) != IDYES) return;

    bool ok = DeletePath(ExtDirOf(folder));
    // WebView2 側に登録済みの拡張機能情報も消す（残りの拡張機能は次回起動時に自動で再登録される）
    if (!g_dataDir.empty()) {
        std::wstring d = g_dataDir + L"\\EBWebView\\Default\\";
        const wchar_t* names[] = { L"Extensions", L"Extension State", L"Extension Rules", L"Extension Scripts", L"Local Extension Settings" };
        for (auto n : names) DeletePath(d + n);
    }
    DeletePath(g_exeDir + L"\\extensions.state");
    RefreshExtStatus(hWnd);
    MessageBoxW(hWnd, ok ? L"削除しました。" : L"一部を削除できませんでした。", L"拡張機能の削除", MB_OK | MB_ICONINFORMATION);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = GetModuleHandle(NULL);
        const int margin = S(16);
        const int width = S(560) - margin * 2;
        int y = S(14);
        auto label = [&](const wchar_t* text, int x, int yy, int w, int h) {
            return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, yy, w, h, hWnd, NULL, hInst, NULL);
        };

        std::wstring head = L"対象アプリ： " + g_appName;
        label(head.c_str(), margin, y, width, S(20));
        y += S(30);

        // --- 機能 ---
        int gh = S(34 + 6 * 26);
        CreateWindowExW(0, L"BUTTON", L"機能（次回起動時に反映）", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, margin, y, width, gh, hWnd, NULL, hInst, NULL);
        int cy = y + S(24);
        for (const auto& f : FEATURES) {
            CreateWindowExW(0, L"BUTTON", f.label, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                            margin + S(14), cy, width - S(28), S(22), hWnd, (HMENU)(INT_PTR)f.id, hInst, NULL);
            cy += S(26);
        }
        y += gh + S(10);

        // --- 表示・動作 ---
        gh = S(76);
        CreateWindowExW(0, L"BUTTON", L"表示・動作", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, margin, y, width, gh, hWnd, NULL, hInst, NULL);
        label(L"初期テーマ", margin + S(14), y + S(30), S(110), S(20));
        HWND cbTheme = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
                                       margin + S(124), y + S(26), S(130), S(120), hWnd, (HMENU)(INT_PTR)ID_COMBO_THEME, hInst, NULL);
        SendMessageW(cbTheme, CB_ADDSTRING, 0, (LPARAM)L"デバイス設定");
        SendMessageW(cbTheme, CB_ADDSTRING, 0, (LPARAM)L"ホワイト");
        SendMessageW(cbTheme, CB_ADDSTRING, 0, (LPARAM)L"ダーク");
        label(L"検索エンジン", margin + S(280), y + S(30), S(100), S(20));
        HWND cbSearch = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
                                        margin + S(384), y + S(26), S(130), S(120), hWnd, (HMENU)(INT_PTR)ID_COMBO_SEARCH, hInst, NULL);
        SendMessageW(cbSearch, CB_ADDSTRING, 0, (LPARAM)L"Google");
        SendMessageW(cbSearch, CB_ADDSTRING, 0, (LPARAM)L"Bing");
        y += gh + S(10);

        // --- 拡張機能 ---
        gh = S(EXT_GROUP_H);
        CreateWindowExW(0, L"BUTTON", L"拡張機能（インストールすると拡張機能サポートが自動で有効になります）", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                        margin, y, width, gh, hWnd, NULL, hInst, NULL);
        int ey = y + S(28);
        for (int i = 0; i < EXT_COUNT; ++i) {
            HWND st = label(L"", margin + S(14), ey + S(5), width - S(190), S(22));
            SetWindowLongPtrW(st, GWLP_ID, ID_EXT_STATUS_BASE + i);
            CreateWindowExW(0, L"BUTTON", L"インストール", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                            margin + width - S(164), ey, S(150), S(28), hWnd, (HMENU)(INT_PTR)(ID_EXT_INSTALL_BASE + i), hInst, NULL);
            ey += S(34);
        }
        // URL / ID から追加
        label(L"URL / ID", margin + S(14), ey + S(5), S(64), S(20));
        CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                        margin + S(80), ey + S(2), width - S(80) - S(104) - S(14), S(24), hWnd, (HMENU)(INT_PTR)ID_EXT_URL_EDIT, hInst, NULL);
        CreateWindowExW(0, L"BUTTON", L"追加", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        margin + width - S(96) - S(14), ey, S(96), S(28), hWnd, (HMENU)(INT_PTR)ID_BTN_ADD_URL, hInst, NULL);
        ey += S(34);
        label(L"Chrome ウェブストア / Microsoft Edge アドオン の拡張機能ページのURL（または拡張機能ID）を貼り付けて「追加」", margin + S(14), ey, width - S(28), S(18));
        ey += S(22);
        label(L"インストール済み：", margin + S(14), ey, width - S(28), S(18));
        ey += S(20);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
                        margin + S(14), ey, width - S(28), S(92), hWnd, (HMENU)(INT_PTR)ID_LIST_EXT, hInst, NULL);
        ey += S(98);
        int bw = (width - S(28) - S(10)) / 2;
        CreateWindowExW(0, L"BUTTON", L"選択した拡張機能を削除", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        margin + S(14), ey, bw, S(28), hWnd, (HMENU)(INT_PTR)ID_BTN_REMOVE_SEL, hInst, NULL);
        CreateWindowExW(0, L"BUTTON", L"拡張機能フォルダを開く", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        margin + S(14) + bw + S(10), ey, bw, S(28), hWnd, (HMENU)(INT_PTR)ID_BTN_OPENFOLDER, hInst, NULL);
        y += gh + S(14);

        int half = (width - S(10)) / 2;
        CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                        margin, y, half, S(34), hWnd, (HMENU)(INT_PTR)ID_BTN_SAVE, hInst, NULL);
        CreateWindowExW(0, L"BUTTON", L"閉じる", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        margin + half + S(10), y, half, S(34), hWnd, (HMENU)(INT_PTR)ID_BTN_CLOSE, hInst, NULL);

        EnumChildWindows(hWnd, [](HWND h, LPARAM) -> BOOL {
            SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
            return TRUE;
        }, 0);

        g_loaded = LoadFeatureValues();
        for (int i = 0; i < 6; ++i) SendDlgItemMessageW(hWnd, FEATURES[i].id, BM_SETCHECK, g_loaded.on[i] ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(cbTheme, CB_SETCURSEL, g_loaded.theme, 0);
        SendMessageW(cbSearch, CB_SETCURSEL, g_loaded.search, 0);
        RefreshExtStatus(hWnd);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id >= ID_EXT_INSTALL_BASE && id < ID_EXT_INSTALL_BASE + EXT_COUNT) {
            OnInstallRecommended(hWnd, id - ID_EXT_INSTALL_BASE);
            return 0;
        }
        switch (id) {
        case ID_BTN_OPENFOLDER: {
            std::wstring dir = g_exeDir + L"\\extensions";
            CreateDirectoryW(dir.c_str(), NULL);
            ShellExecuteW(hWnd, L"open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL);
            break;
        }
        case ID_BTN_ADD_URL:
            OnAddUrl(hWnd);
            break;
        case ID_BTN_REMOVE_SEL:
            OnRemoveSelected(hWnd);
            break;
        case ID_BTN_SAVE:
            DoSave(hWnd, false);
            break;
        case ID_BTN_CLOSE:
        case IDCANCEL:
            DestroyWindow(hWnd);
            break;
        }
        return 0;
    }
    case WM_APP_INSTALL_DONE: {
        SetBusy(hWnd, false);
        if (g_job.ok) {
            // 拡張機能サポートを自動で有効化して保存
            SendDlgItemMessageW(hWnd, ID_FEAT_EXT, BM_SETCHECK, BST_CHECKED, 0);
            DoSave(hWnd, true);
            SetDlgItemTextW(hWnd, ID_EXT_URL_EDIT, L"");
            std::wstring m = ExtDisplayName(g_job.folder) + L"\nをインストールしました。\nアプリを（再）起動すると有効になります。";
            MessageBoxW(hWnd, m.c_str(), L"拡張機能のインストール", MB_OK | MB_ICONINFORMATION);
        } else {
            std::wstring m = L"インストールに失敗しました。\n" + g_job.error;
            MessageBoxW(hWnd, m.c_str(), L"拡張機能のインストール", MB_OK | MB_ICONERROR);
        }
        return 0;
    }
    case WM_APP_AUTO_ADD:
        OnAddUrl(hWnd);
        return 0;
    case WM_CLOSE:
        if (g_busy) return 0;   // 処理中は閉じさせない
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    if (lpCmdLine && *lpCmdLine) {
        g_initialUrl = lpCmdLine;
        if (g_initialUrl.size() >= 2 && g_initialUrl.front() == L'"' && g_initialUrl.back() == L'"') g_initialUrl = g_initialUrl.substr(1, g_initialUrl.size() - 2);
    }
    SetProcessDPIAware();
    HDC hdc = GetDC(NULL);
    g_dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(NULL, hdc);

    wchar_t self[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH * 2);
    g_exeDir = DirOf(std::wstring(self, n));
    g_appName = ReadTextFile(g_exeDir + L"\\name.txt");
    if (g_appName.empty() || g_appName.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        MessageBoxW(NULL, L"name.txt が見つからないか、内容が不正です。\nこのツールはアプリのフォルダ内に置いて実行してください。",
                    L"エラー", MB_OK | MB_ICONERROR);
        return 1;
    }
    wchar_t lad[MAX_PATH];
    DWORD ln = GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
    if (ln > 0 && ln < MAX_PATH) g_dataDir = std::wstring(lad) + L"\\" + g_appName + L"_Data";

    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);

    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"WebAppFeaturesWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    int clientW = S(560);
    int clientH = S(14 + 30 + (34 + 6 * 26) + 10 + 76 + 10 + EXT_GROUP_H + 14 + 34 + 16);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, clientW, clientH };
    AdjustWindowRect(&rc, style, FALSE);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    if (y < 0) y = 0;

    g_hWnd = CreateWindowExW(0, wc.lpszClassName, (g_appName + L" - 機能の追加・拡張機能").c_str(),
                             style, x, y, w, h, NULL, NULL, hInstance, NULL);
    if (!g_hWnd) return 1;
    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    // コマンドラインでURLを渡された場合（アプリの「拡張機能を追加」から起動）は、入力して確認画面まで進める
    if (!g_initialUrl.empty()) {
        SetDlgItemTextW(g_hWnd, ID_EXT_URL_EDIT, g_initialUrl.c_str());
        PostMessageW(g_hWnd, WM_APP_AUTO_ADD, 0, 0);
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(g_hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DeleteObject(g_font);
    return 0;
}
