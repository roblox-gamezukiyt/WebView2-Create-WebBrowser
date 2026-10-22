# WebView2-Create-WebBrowser
WebView2のブラウザを作成するビルドツールです。
Microsoft Visual StudioのBuildToolとnugetが必要です
nuget.exeをbuild.batとががあるフォルダに置き
nuget install Microsoft.Web.WebView2 -OutputDirectory packages
nuget install Microsoft.Windows.ImplementationLibrary -OutputDirectory packages
を実行
""C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" を実行しcd build.batがあるフォルダのパス を実行しbuild.batを実行
作成者 by Gemini AI Claude AI