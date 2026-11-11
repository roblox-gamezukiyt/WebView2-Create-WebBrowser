// Uninstall.exe : データ削除 / アンインストール ツール
// アプリのフォルダ (name.txt と <name>.exe がある場所) に置いて使う。

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

// ------------------------------------------------------------
// ID
// ------------------------------------------------------------
enum {
    ID_CHK_COOKIE = 301,
    ID_CHK_CACHE,
    ID_CHK_SITEDATA,
    ID_CHK_HISTORY,
    ID_CHK_PASSWORD,
    ID_CHK_EXTENSION,
    ID_CHK_APPSETTINGS,
    ID_CHK_ALL,
    ID_BTN_DELETE = 320,
    ID_BTN_UNINSTALL,
    ID_BTN_CLOSE
};

static std::wstring g_appName;
static std::wstring g_exeDir;
static std::wstring g_selfPath;
static std::wstring g_dataDir;   // %LOCALAPPDATA%\<name>_Data
static HFONT g_font = NULL;
static int g_dpi = 96;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

// ------------------------------------------------------------
// ユーティリティ
// ------------------------------------------------------------
static std::wstring GetSelfPath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH * 2);
    return std::wstring(buf, n);
}

static std::wstring DirOf(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"" : path.substr(0, pos);
}

static std::wstring ReadTextFile(const std::wstring& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f.is_open()) return L"";
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    size_t first = w.find_first_not_of(L"\r\n\t \xFEFF");
    if (first == std::wstring::npos) return L"";
    size_t last = w.find_last_not_of(L"\r\n\t \xFEFF");
    return w.substr(first, last - first + 1);
}

static bool PathExists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool IsValidAppName(const std::wstring& name) {
    if (name.empty()) return false;
    return name.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos;
}

// 1回だけ削除を試みる。削除済み（または元から無い）なら true
static bool DeletePathOnce(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        std::wstring from = p;
        from.push_back(L'\0');  // 二重ヌル終端
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

// WebView2 の子プロセスがファイルを掴んでいる場合があるので、少し待ちながらリトライ
static bool DeletePath(const std::wstring& p) {
    for (int i = 0; i < 6; ++i) {
        if (DeletePathOnce(p)) return true;
        Sleep(500);
    }
    return false;
}

// ------------------------------------------------------------
// 実行中アプリの検出・終了
// ------------------------------------------------------------
static std::vector<DWORD> FindAppProcesses() {
    std::vector<DWORD> pids;
    std::wstring exeName = g_appName + L".exe";
    std::wstring target = g_exeDir + L"\\" + exeName;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName.c_str()) != 0) continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h) continue;
            wchar_t buf[MAX_PATH * 2];
            DWORD len = MAX_PATH * 2;
            if (QueryFullProcessImageNameW(h, 0, buf, &len) && _wcsicmp(buf, target.c_str()) == 0) {
                pids.push_back(pe.th32ProcessID);
            }
            CloseHandle(h);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

static BOOL CALLBACK CloseWindowsOfPids(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    auto* pids = (std::vector<DWORD>*)lp;
    if (std::find(pids->begin(), pids->end(), pid) != pids->end()) PostMessageW(h, WM_CLOSE, 0, 0);
    return TRUE;
}

// アプリが動いていれば確認のうえ終了させる。続行できるなら true
static bool EnsureAppClosed(HWND owner) {
    std::vector<DWORD> pids = FindAppProcesses();
    if (pids.empty()) return true;

    std::wstring msg = L"「" + g_appName + L"」が実行中です。\n終了してから続行しますか？";
    if (MessageBoxW(owner, msg.c_str(), L"確認", MB_YESNO | MB_ICONQUESTION) != IDYES) return false;

    EnumWindows(CloseWindowsOfPids, (LPARAM)&pids);
    for (DWORD pid : pids) {
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
        if (!h) continue;
        if (WaitForSingleObject(h, 5000) == WAIT_TIMEOUT) TerminateProcess(h, 0);
        CloseHandle(h);
    }
    Sleep(500);
    if (!FindAppProcesses().empty()) {
        MessageBoxW(owner, L"アプリを終了できませんでした。手動で終了してからやり直してください。", L"エラー", MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

// ------------------------------------------------------------
// 削除対象
// ------------------------------------------------------------
struct Category {
    int id;
    const wchar_t* label;
};

static const Category CATEGORIES[] = {
    { ID_CHK_COOKIE,      L"Cookie" },
    { ID_CHK_CACHE,       L"キャッシュ" },
    { ID_CHK_SITEDATA,    L"サイトデータ（ログイン状態・ローカルストレージなど）" },
    { ID_CHK_HISTORY,     L"閲覧履歴" },
    { ID_CHK_PASSWORD,    L"保存されたパスワード・自動入力" },
    { ID_CHK_EXTENSION,   L"拡張機能の登録情報（extensions フォルダ内のものは起動時に再登録）" },
    { ID_CHK_APPSETTINGS, L"アプリ設定（テーマ・検索エンジン・ブックマーク・拡張機能設定）" },
};

static std::vector<std::wstring> TargetsFor(int id) {
    const std::wstring E = g_dataDir + L"\\EBWebView\\";
    const std::wstring D = E + L"Default\\";
    std::vector<std::wstring> v;
    auto add = [&](const std::wstring& base, std::initializer_list<const wchar_t*> names) {
        for (auto n : names) v.push_back(base + n);
    };

    switch (id) {
    case ID_CHK_COOKIE:
        add(D, { L"Network\\Cookies", L"Network\\Cookies-journal", L"Cookies", L"Cookies-journal" });
        break;
    case ID_CHK_CACHE:
        add(D, { L"Cache", L"Code Cache", L"GPUCache", L"DawnGraphiteCache", L"DawnWebGPUCache" });
        add(E, { L"ShaderCache", L"GrShaderCache", L"GraphiteDawnCache" });
        break;
    case ID_CHK_SITEDATA:
        add(D, { L"Local Storage", L"Session Storage", L"IndexedDB", L"Service Worker", L"File System",
                 L"Storage", L"blob_storage", L"WebStorage", L"databases", L"Sessions" });
        break;
    case ID_CHK_HISTORY:
        add(D, { L"History", L"History-journal", L"Visited Links", L"Top Sites", L"Top Sites-journal",
                 L"Favicons", L"Favicons-journal", L"Shortcuts", L"Shortcuts-journal" });
        break;
    case ID_CHK_PASSWORD:
        add(D, { L"Login Data", L"Login Data-journal", L"Login Data For Account", L"Login Data For Account-journal",
                 L"Web Data", L"Web Data-journal" });
        break;
    case ID_CHK_EXTENSION:
        add(D, { L"Extensions", L"Extension State", L"Extension Rules", L"Extension Scripts", L"Local Extension Settings" });
        break;
    case ID_CHK_APPSETTINGS:
        add(g_exeDir + L"\\", { L"config.txt", L"bookmarks.txt", L"extsettings.txt" });
        break;
    case ID_CHK_ALL:
        v.push_back(g_dataDir);
        break;
    }
    return v;
}

static bool IsChecked(HWND hWnd, int id) {
    return SendDlgItemMessageW(hWnd, id, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

// ------------------------------------------------------------
// 削除処理
// ------------------------------------------------------------
static void DoDelete(HWND hWnd) {
    bool all = IsChecked(hWnd, ID_CHK_ALL);

    std::vector<int> ids;
    if (all) ids.push_back(ID_CHK_ALL);
    for (const auto& c : CATEGORIES) {
        if (all && c.id != ID_CHK_APPSETTINGS) continue;   // 「すべて」選択時は個別項目を含む
        if (IsChecked(hWnd, c.id)) ids.push_back(c.id);
    }
    if (ids.empty()) {
        MessageBoxW(hWnd, L"削除する項目を選択してください。", L"データ削除", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (g_dataDir.empty() && std::find(ids.begin(), ids.end(), ID_CHK_APPSETTINGS) == ids.end()) {
        MessageBoxW(hWnd, L"データの保存場所を特定できませんでした。", L"エラー", MB_OK | MB_ICONERROR);
        return;
    }

    std::wstring list;
    for (int id : ids) {
        if (id == ID_CHK_ALL) { list += L"・すべてのブラウザデータ（Cookie・キャッシュ・ログイン情報などすべて）\n"; continue; }
        for (const auto& c : CATEGORIES) if (c.id == id) list += std::wstring(L"・") + c.label + L"\n";
    }
    std::wstring confirm = L"次のデータを削除します。元に戻せません。\n\n" + list + L"\nよろしいですか？";
    if (MessageBoxW(hWnd, confirm.c_str(), L"データ削除", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;

    if (!EnsureAppClosed(hWnd)) return;

    HCURSOR oldCur = SetCursor(LoadCursor(NULL, IDC_WAIT));
    int failed = 0;
    for (int id : ids) {
        for (const auto& p : TargetsFor(id)) {
            if (!DeletePath(p)) ++failed;
        }
    }
    SetCursor(oldCur);

    if (failed == 0) {
        MessageBoxW(hWnd, L"削除が完了しました。", L"データ削除", MB_OK | MB_ICONINFORMATION);
    } else {
        std::wstring m = std::to_wstring(failed) + L" 件を削除できませんでした。\n"
                         L"他のプログラムが使用中の可能性があります。しばらくしてからもう一度お試しください。";
        MessageBoxW(hWnd, m.c_str(), L"データ削除", MB_OK | MB_ICONWARNING);
    }
}

// ------------------------------------------------------------
// アンインストール
// ------------------------------------------------------------
static void DoUninstall(HWND hWnd) {
    // アプリフォルダとして正しい場所か確認（別の場所のファイルを消さないため）
    if (!PathExists(g_exeDir + L"\\" + g_appName + L".exe") || !PathExists(g_exeDir + L"\\name.txt")) {
        MessageBoxW(hWnd, L"アプリ本体が見つからないため、アンインストールできません。", L"エラー", MB_OK | MB_ICONERROR);
        return;
    }

    std::wstring msg =
        L"「" + g_appName + L"」をアンインストールします。\n\n"
        L"［はい］　アプリ本体とすべてのデータ（ログイン情報含む）を削除\n"
        L"［いいえ］アプリ本体のみ削除（データは残す）\n"
        L"［キャンセル］中止";
    int r = MessageBoxW(hWnd, msg.c_str(), L"アンインストール", MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON3);
    if (r == IDCANCEL) return;
    bool removeData = (r == IDYES);

    if (!EnsureAppClosed(hWnd)) return;

    HCURSOR oldCur = SetCursor(LoadCursor(NULL, IDC_WAIT));
    bool dataFailed = false;
    if (removeData && !g_dataDir.empty()) dataFailed = !DeletePath(g_dataDir);
    DeletePath(g_exeDir + L"\\extensions");   // インストールした拡張機能
    SetCursor(oldCur);

    // 自分自身(実行中)とアプリ本体は、終了後に cmd で削除する
    const wchar_t* files[] = {
        nullptr,  // <name>.exe
        L"WebView2Loader.dll", L"name.txt", L"url.txt", L"permissions.txt",
        L"config.txt", L"bookmarks.txt", L"extsettings.txt",
        L"features.txt", L"extensions.state", L"Features.exe"
    };
    std::wstring delList = L"\"" + g_exeDir + L"\\" + g_appName + L".exe\"";
    for (size_t i = 1; i < sizeof(files) / sizeof(files[0]); ++i) {
        delList += L" \"" + g_exeDir + L"\\" + files[i] + L"\"";
    }
    delList += L" \"" + g_selfPath + L"\"";

    std::wstring step = L"del /f /q " + delList + L" & rmdir \"" + g_exeDir + L"\"";
    std::wstring cmd = L"cmd.exe /s /c \"ping 127.0.0.1 -n 3 >nul & " + step +
                       L" & ping 127.0.0.1 -n 4 >nul & " + step + L"\"";

    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);   // 削除対象フォルダ外を作業ディレクトリにする

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    if (CreateProcessW(NULL, cmdBuf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, tempDir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        MessageBoxW(hWnd, L"削除処理を開始できませんでした。フォルダを手動で削除してください。", L"エラー", MB_OK | MB_ICONERROR);
        return;
    }

    std::wstring done = L"アンインストールを実行しました。\nこのウィンドウを閉じるとフォルダが削除されます。";
    if (dataFailed) done += L"\n\n※ 一部のデータを削除できませんでした。\n　" + g_dataDir + L"\n　を手動で削除してください。";
    MessageBoxW(hWnd, done.c_str(), L"アンインストール", MB_OK | MB_ICONINFORMATION);
    DestroyWindow(hWnd);
}

// ------------------------------------------------------------
// ウィンドウ
// ------------------------------------------------------------
static void UpdateAllCheckState(HWND hWnd) {
    bool all = IsChecked(hWnd, ID_CHK_ALL);
    for (const auto& c : CATEGORIES) {
        if (c.id == ID_CHK_APPSETTINGS) continue;
        EnableWindow(GetDlgItem(hWnd, c.id), !all);
    }
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = GetModuleHandle(NULL);
        const int margin = S(16);
        const int width = S(500) - margin * 2;
        int y = S(14);

        std::wstring head = L"対象アプリ： " + g_appName;
        CreateWindowExW(0, L"STATIC", head.c_str(), WS_CHILD | WS_VISIBLE, margin, y, width, S(20), hWnd, NULL, hInst, NULL);
        y += S(22);
        std::wstring loc = L"データの場所： " + g_dataDir;
        CreateWindowExW(0, L"STATIC", loc.c_str(), WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS, margin, y, width, S(20), hWnd, NULL, hInst, NULL);
        y += S(34);

        CreateWindowExW(0, L"BUTTON", L"削除するデータ", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                        margin, y, width, S(8 * 28 + 34), hWnd, NULL, hInst, NULL);
        int cy = y + S(24);
        for (const auto& c : CATEGORIES) {
            CreateWindowExW(0, L"BUTTON", c.label, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                            margin + S(14), cy, width - S(28), S(24), hWnd, (HMENU)(INT_PTR)c.id, hInst, NULL);
            cy += S(28);
        }
        CreateWindowExW(0, L"BUTTON", L"すべて（ブラウザデータ全体を削除）", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                        margin + S(14), cy, width - S(28), S(24), hWnd, (HMENU)(INT_PTR)ID_CHK_ALL, hInst, NULL);
        y += S(8 * 28 + 34) + S(16);

        CreateWindowExW(0, L"BUTTON", L"選択したデータを削除", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                        margin, y, width, S(34), hWnd, (HMENU)(INT_PTR)ID_BTN_DELETE, hInst, NULL);
        y += S(34) + S(10);
        int half = (width - S(10)) / 2;
        CreateWindowExW(0, L"BUTTON", L"アンインストール", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                        margin, y, half, S(34), hWnd, (HMENU)(INT_PTR)ID_BTN_UNINSTALL, hInst, NULL);
        CreateWindowExW(0, L"BUTTON", L"閉じる", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                        margin + half + S(10), y, half, S(34), hWnd, (HMENU)(INT_PTR)ID_BTN_CLOSE, hInst, NULL);

        EnumChildWindows(hWnd, [](HWND h, LPARAM) -> BOOL {
            SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
            return TRUE;
        }, 0);
        return 0;
    }
    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case ID_CHK_ALL:
            UpdateAllCheckState(hWnd);
            break;
        case ID_BTN_DELETE:
            DoDelete(hWnd);
            break;
        case ID_BTN_UNINSTALL:
            DoUninstall(hWnd);
            break;
        case ID_BTN_CLOSE:
        case IDCANCEL:
            DestroyWindow(hWnd);
            break;
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    SetProcessDPIAware();
    HDC hdc = GetDC(NULL);
    g_dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(NULL, hdc);

    g_selfPath = GetSelfPath();
    g_exeDir = DirOf(g_selfPath);
    g_appName = ReadTextFile(g_exeDir + L"\\name.txt");

    if (!IsValidAppName(g_appName)) {
        MessageBoxW(NULL, L"name.txt が見つからないか、内容が不正です。\nこのツールはアプリのフォルダ内に置いて実行してください。",
                    L"エラー", MB_OK | MB_ICONERROR);
        return 1;
    }

    wchar_t localAppData[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (n > 0 && n < MAX_PATH) g_dataDir = std::wstring(localAppData) + L"\\" + g_appName + L"_Data";

    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);

    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"WebAppCleanerWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    int clientW = S(500);
    int clientH = S(14 + 22 + 34 + 8 * 28 + 34 + 16 + 34 + 10 + 34 + 16);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, clientW, clientH };
    AdjustWindowRect(&rc, style, FALSE);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HWND hWnd = CreateWindowExW(0, wc.lpszClassName, (g_appName + L" - データ削除 / アンインストール").c_str(),
                                style, x, y, w, h, NULL, NULL, hInstance, NULL);
    if (!hWnd) return 1;
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DeleteObject(g_font);
    return 0;
}
