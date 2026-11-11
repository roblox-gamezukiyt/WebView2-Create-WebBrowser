@echo off
setlocal enabledelayedexpansion

echo ===========================================
echo  全アプリ 一括再ビルドスクリプト
echo ===========================================
echo.
echo このフォルダ内の「ビルド済みアプリフォルダ」(WebView2Loader.dll と name.txt があるフォルダ)を
echo フォルダ名に関係なくすべて最新のソースで更新します。
echo   ・アプリ名 / URL / 機能設定 / Cookie などのデータはそのまま引き継ぎます
echo   ・更新するのは exe / WebView2Loader.dll / Uninstall.exe / Features.exe のみです
echo   ・起動中のアプリは上書きできないため、先に終了してください
echo.
pause

cd /d "%~dp0"

:: 0. Visual Studio ビルド環境 (cl.exe) の自動セットアップ
where cl.exe >nul 2>&1
if %ERRORLEVEL% equ 0 goto VC_READY

echo Visual Studio のビルド環境を検索中...
set "VCVARS="
set "VSPATH="
set "VS_DEFAULT=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VS_DEFAULT%" set "VCVARS=%VS_DEFAULT%"
if defined VCVARS goto VC_CALL

:: 見つからなければ vswhere で最新の VS / Build Tools を探す
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto ERR_VC
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto ERR_VC
if exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS goto ERR_VC

:VC_CALL
echo 環境を設定中: %VCVARS%
call "%VCVARS%" >nul
where cl.exe >nul 2>&1
if %ERRORLEVEL% neq 0 goto ERR_VC

:VC_READY

set "PACKAGES_DIR=packages"
set "WEBVIEW2_INC=%PACKAGES_DIR%\Microsoft.Web.WebView2.1.0.4191.47\build\native\include"
set "WIL_INC=%PACKAGES_DIR%\Microsoft.Windows.ImplementationLibrary.1.0.260126.7\include"
set "WEBVIEW2_LIB=%PACKAGES_DIR%\Microsoft.Web.WebView2.1.0.4191.47\build\native\x64"

if not exist "main.cpp" goto ERR_SRC
if not exist "cleaner.cpp" goto ERR_SRC
if not exist "features.cpp" goto ERR_SRC
if not exist "%WEBVIEW2_INC%\WebView2.h" goto ERR_WV
if not exist "%WIL_INC%\wil\com.h" goto ERR_WIL
if not exist "%WEBVIEW2_LIB%\WebView2Loader.dll" goto ERR_LIB

:: 更新対象の確認
set /a TARGETS=0
for /d %%D in (*) do (
    if exist "%%D\name.txt" if exist "%%D\WebView2Loader.dll" set /a TARGETS+=1
)
if %TARGETS% equ 0 goto ERR_NONE
echo 更新対象: %TARGETS% 個のアプリフォルダ（WebView2Loader.dll と name.txt があるフォルダ）
echo.

:: 旧バージョンのフォルダには機能設定 (features.txt) が無く、以前どの機能を有効にしていたか分からないため選択式にする
echo features.txt が無い旧形式のフォルダは、以前の機能設定が分かりません。
echo   0: 全機能を無効で作成（あとで各フォルダの Features.exe で必要な機能だけ有効にする）
echo   1: 全機能を有効で作成（あとで Features.exe で不要な機能を無効にする）
set "LEGACY=0"
set /p LEGACY="旧形式フォルダの機能 (0 / 1) [初期値: 0]: "
if not "!LEGACY!"=="1" set "LEGACY=0"
echo.

:: 1. 共通の実行ファイルを一度だけビルド（アプリ名・URL・機能は実行時に設定ファイルから読むため全アプリで共通）
set "TMP_DIR=_rebuild_tmp"
if exist "%TMP_DIR%" rmdir /s /q "%TMP_DIR%"
mkdir "%TMP_DIR%"

echo [1/3] 本体をコンパイル中...
cl.exe /std:c++17 /EHsc /utf-8 /W3 /O2 /MD /DUNICODE /D_UNICODE /I"%WEBVIEW2_INC%" /I"%WIL_INC%" /Fo%TMP_DIR%\ main.cpp /link /SUBSYSTEM:WINDOWS /MACHINE:X64 /LIBPATH:"%WEBVIEW2_LIB%" WebView2Loader.dll.lib user32.lib shell32.lib comctl32.lib advapi32.lib ole32.lib dwmapi.lib gdi32.lib /OUT:"%TMP_DIR%\App.exe"
if errorlevel 1 goto ERR_BUILD

echo [2/3] データ削除/アンインストールツールをコンパイル中...
cl.exe /std:c++17 /EHsc /utf-8 /W3 /O2 /MD /DUNICODE /D_UNICODE /Fo%TMP_DIR%\ cleaner.cpp /link /SUBSYSTEM:WINDOWS /MACHINE:X64 /MANIFEST:EMBED user32.lib gdi32.lib shell32.lib comctl32.lib advapi32.lib ole32.lib /OUT:"%TMP_DIR%\Uninstall.exe"
if errorlevel 1 goto ERR_BUILD

echo [3/3] 機能設定ツールをコンパイル中...
cl.exe /std:c++17 /EHsc /utf-8 /W3 /O2 /MD /DUNICODE /D_UNICODE /Fo%TMP_DIR%\ features.cpp /link /SUBSYSTEM:WINDOWS /MACHINE:X64 /MANIFEST:EMBED user32.lib gdi32.lib shell32.lib comctl32.lib advapi32.lib ole32.lib urlmon.lib /OUT:"%TMP_DIR%\Features.exe"
if errorlevel 1 goto ERR_BUILD

:: 2. 各アプリフォルダへ配置
echo.
echo --- 各アプリを更新中 ---
set /a OK=0
set /a NG=0
set /a SKIP=0
for /d %%D in (*) do (
    if exist "%%D\WebView2Loader.dll" (
        if exist "%%D\name.txt" (
            call :UPDATE_ONE "%%D"
        ) else (
            echo [%%D] name.txt が無い旧形式のため、スキップしました
            set /a SKIP+=1
        )
    )
)

rmdir /s /q "%TMP_DIR%" > nul 2>&1
if exist "main.obj" del main.obj
if exist "cleaner.obj" del cleaner.obj
if exist "features.obj" del features.obj

echo.
echo ===========================================
echo  完了: 成功 !OK! 件 / 失敗 !NG! 件 / スキップ !SKIP! 件
echo ===========================================
if !NG! gtr 0 echo 失敗したアプリは、起動中でないか確認してもう一度実行してください。
pause
exit /b 0

:UPDATE_ONE
set "APP=%~1"
echo [!APP!]

:: アプリ名は name.txt から取得（フォルダ名と違っていてもよい。BOM 付きでも可）
set "NAME="
for /f "usebackq delims=" %%N in (`powershell -NoProfile -Command "(Get-Content -Raw -Encoding UTF8 -LiteralPath '!APP!\name.txt').Trim()"`) do set "NAME=%%N"
if not defined NAME set "NAME=!APP!"

:: 上書きする本体 exe を決める（<name>.exe が無ければ、フォルダ内の既存の本体 exe を使う）
set "TARGET_EXE=!NAME!.exe"
if not exist "!APP!\!TARGET_EXE!" (
    set "FOUND="
    for %%F in ("!APP!\*.exe") do (
        if /i not "%%~nxF"=="Uninstall.exe" if /i not "%%~nxF"=="Features.exe" if not defined FOUND set "FOUND=%%~nxF"
    )
    if defined FOUND set "TARGET_EXE=!FOUND!"
)
echo   アプリ名: !NAME! / 本体: !TARGET_EXE!

:: features.txt が無い旧形式フォルダは、最初に選んだ値で作成（あとで Features.exe で変更可能）
if not exist "!APP!\features.txt" (
    (echo nav=!LEGACY!& echo tabs=!LEGACY!& echo ext=!LEGACY!& echo bookmarks=!LEGACY!& echo settings=!LEGACY!& echo devtools=!LEGACY!& echo theme=0& echo search=0) > "!APP!\features.txt"
    echo   features.txt を新規作成しました（全機能 !LEGACY!。Features.exe で変更できます）
)

copy /y "%TMP_DIR%\App.exe" "!APP!\!TARGET_EXE!" > nul
if errorlevel 1 goto UPD_FAIL
copy /y "%WEBVIEW2_LIB%\WebView2Loader.dll" "!APP!\" > nul
if errorlevel 1 goto UPD_FAIL
copy /y "%TMP_DIR%\Uninstall.exe" "!APP!\" > nul
if errorlevel 1 goto UPD_FAIL
copy /y "%TMP_DIR%\Features.exe" "!APP!\" > nul
if errorlevel 1 goto UPD_FAIL

echo   更新しました
set /a OK+=1
exit /b 0

:UPD_FAIL
echo   [失敗] 上書きできませんでした。アプリが起動中の可能性があります。
set /a NG+=1
exit /b 0

:ERR_VC
echo [エラー] Visual Studio (C++ ビルドツール) が見つかりません。
echo   "Visual Studio Build Tools" の「C++ によるデスクトップ開発」をインストールするか、
echo   このファイル先頭付近の VS_DEFAULT のパスを環境に合わせて修正してください。
pause
exit /b 1

:ERR_SRC
echo [エラー] main.cpp / cleaner.cpp / features.cpp がこのフォルダに必要です。
pause
exit /b 1

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

:ERR_NONE
echo [エラー] 更新対象のアプリフォルダ（WebView2Loader.dll と name.txt があるフォルダ）が見つかりません。
echo   このファイルを、ビルド済みフォルダが並んでいる場所に置いてください。
pause
exit /b 1

:ERR_BUILD
echo [エラー] ビルドに失敗しました。
if exist "%TMP_DIR%" rmdir /s /q "%TMP_DIR%" > nul 2>&1
pause
exit /b 1
