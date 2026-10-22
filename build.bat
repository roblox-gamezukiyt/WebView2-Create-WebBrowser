@echo off
setlocal enabledelayedexpansion

echo ===========================================
echo  軽量ウェブブラウザ 自動ビルドスクリプト
echo ===========================================
echo.

set "PACKAGES_DIR=packages"

:: 1. アプリ情報の入力
set /p APP_NAME="[アプリ名] ビルドする実行ファイル/フォルダの名前を入力してください [初期値: Discord]: "
if "%APP_NAME%"=="" set "APP_NAME=Discord"

set /p TARGET_URL="[対象URL] 開きたいサイトのURLを入力してください [初期値: https://discord.com/app]: "
if "%TARGET_URL%"=="" set "TARGET_URL=https://discord.com/app"

echo.
echo --- 機能の有効化 / 無効化 設定 ---
set /p ENABLE_NAV="[1] アドレスバー・戻る・再読み込みボタンを有効にしますか？ (1: 有効 / 0: 無効) [初期値: 0]: "
if "%ENABLE_NAV%"=="" set "ENABLE_NAV=0"

set /p ENABLE_TABS="[2] タブ機能を有効にしますか？ (1: 有効 / 0: 無効) [初期値: 0]: "
if "%ENABLE_TABS%"=="" set "ENABLE_TABS=0"

set /p ENABLE_EXT="[3] 拡張機能 (Extensions) サポートを有効にしますか？ (1: 有効 / 0: 無効) [初期値: 0]: "
if "%ENABLE_EXT%"=="" set "ENABLE_EXT=0"

set /p ENABLE_BOOKMARKS="[4] ブックマーク機能を有効にしますか？ (1: 有効 / 0: 無効) [初期値: 0]: "
if "%ENABLE_BOOKMARKS%"=="" set "ENABLE_BOOKMARKS=0"

set /p ENABLE_SETTINGS="[5] ブラウザ内設定 (歯車ボタン) を有効にしますか？ (1: 有効 / 0: 無効) [初期値: 0]: "
if "%ENABLE_SETTINGS%"=="" set "ENABLE_SETTINGS=0"

set /p DEFAULT_THEME="[6] 初期テーマ (0: デバイス設定 / 1: ホワイト / 2: ダーク) [初期値: 0]: "
if "%DEFAULT_THEME%"=="" set "DEFAULT_THEME=0"

set /p DEFAULT_SEARCH="[7] デフォルトの検索エンジン (0: Google / 1: Bing) [初期値: 0]: "
if "%DEFAULT_SEARCH%"=="" set "DEFAULT_SEARCH=0"

set /p ENABLE_DEVTOOLS="[8] デベロッパーツール (F10) を有効にしますか？ (1: 有効 / 0: 無効) [初期値: 0]: "
if "%ENABLE_DEVTOOLS%"=="" set "ENABLE_DEVTOOLS=0"

echo.
:: C++コンパイルマクロフラグの組み立て
set "MACROS=/DUNICODE /D_UNICODE /DDEFAULT_THEME=%DEFAULT_THEME% /DDEFAULT_SEARCH_ENGINE=%DEFAULT_SEARCH%"
if "%ENABLE_NAV%"=="1" set "MACROS=!MACROS! /DENABLE_NAV_BAR"
if "%ENABLE_TABS%"=="1" set "MACROS=!MACROS! /DENABLE_TABS"
if "%ENABLE_EXT%"=="1" set "MACROS=!MACROS! /DENABLE_EXTENSIONS"
if "%ENABLE_BOOKMARKS%"=="1" set "MACROS=!MACROS! /DENABLE_BOOKMARKS"
if "%ENABLE_SETTINGS%"=="1" set "MACROS=!MACROS! /DENABLE_SETTINGS_UI"
if "%ENABLE_DEVTOOLS%"=="1" set "MACROS=!MACROS! /DENABLE_DEVTOOLS"

set "WEBVIEW2_INC=%PACKAGES_DIR%\Microsoft.Web.WebView2.1.0.4191.47\build\native\include"
set "WIL_INC=%PACKAGES_DIR%\Microsoft.Windows.ImplementationLibrary.1.0.260126.7\include"
set "WEBVIEW2_LIB=%PACKAGES_DIR%\Microsoft.Web.WebView2.1.0.4191.47\build\native\x64"

if not exist "%WEBVIEW2_INC%\WebView2.h" goto ERR_WV
if not exist "%WIL_INC%\wil\com.h" goto ERR_WIL
if not exist "%WEBVIEW2_LIB%\WebView2Loader.dll" goto ERR_LIB

echo コンパイル中...
cl.exe /std:c++17 /EHsc /utf-8 /W3 /O2 /MD %MACROS% /I"%WEBVIEW2_INC%" /I"%WIL_INC%" main.cpp /link /SUBSYSTEM:WINDOWS /MACHINE:X64 /LIBPATH:"%WEBVIEW2_LIB%" WebView2Loader.dll.lib user32.lib shell32.lib comctl32.lib advapi32.lib ole32.lib /OUT:"%APP_NAME%.exe"

if %ERRORLEVEL% neq 0 (
    echo [エラー] ビルドに失敗しました。
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo 成果物を [%APP_NAME%] フォルダに整理中...
if not exist "%APP_NAME%" mkdir "%APP_NAME%"

move /y "%APP_NAME%.exe" "%APP_NAME%\" > nul
copy /y "%WEBVIEW2_LIB%\WebView2Loader.dll" "%APP_NAME%\" > nul

:: 設定ファイル類（name.txt, url.txt, permissions.txt）を出力
powershell -Command "[System.IO.File]::WriteAllText('%APP_NAME%\name.txt', '%APP_NAME%', [System.Text.Encoding]::UTF8)"
powershell -Command "[System.IO.File]::WriteAllText('%APP_NAME%\url.txt', '%TARGET_URL%', [System.Text.Encoding]::UTF8)"

:: 既に permissions.txt が存在しない場合のみ初期生成
if not exist "%APP_NAME%\permissions.txt" (
    powershell -Command "[System.IO.File]::WriteAllText('%APP_NAME%\permissions.txt', 'mic=true'`r`n'camera=true'`r`n', [System.Text.Encoding]::UTF8)"
)

if exist "main.obj" del main.obj

echo.
echo すべての処理が完了しました。
pause
exit /b 0

:ERR_WV
echo [エラー] WebView2 インクルードが見つかりません。
pause
exit /b 1

:ERR_WIL
echo [エラー] WIL インクルードが見つかりません。
pause
exit /b 1

:ERR_LIB
echo [エラー] WebView2 ライブラリが見つかりません。
pause
exit /b 1