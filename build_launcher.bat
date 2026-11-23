@echo off
rem Builds the GT6 PC launcher (launcher\*.cs, Theme.xaml, icon.ico) with the C# compiler of the .NET Framework 4
rem that is part of Windows (C# 5). Output: build-launcher\GT6 Launcher.exe. It belongs in the package folder,
rem beside PLAY_GT6.bat.
setlocal
set "ROOT=%~dp0"
set "SRC=%ROOT%launcher"
set "OUT=%ROOT%build-launcher"
set "FW=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319"
if not exist "%FW%\csc.exe" (
  echo The .NET Framework 4 compiler was not found: %FW%\csc.exe
  exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"
"%FW%\csc.exe" /nologo /target:winexe /optimize+ /codepage:65001 "/out:%OUT%\GT6 Launcher.exe" "/win32icon:%SRC%\icon.ico" ^
  /r:System.Core.dll /r:System.Xaml.dll /r:System.Xml.dll /r:System.Windows.Forms.dll /r:System.Drawing.dll ^
  /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll /r:System.Web.Extensions.dll /r:System.Security.dll ^
  "/r:%FW%\WPF\PresentationCore.dll" "/r:%FW%\WPF\PresentationFramework.dll" "/r:%FW%\WPF\WindowsBase.dll" ^
  "/r:%FW%\WPF\UIAutomationClient.dll" "/r:%FW%\WPF\UIAutomationTypes.dll" ^
  "/resource:%SRC%\Theme.xaml,Theme.xaml" "/resource:%SRC%\icon.ico,icon.ico" "%SRC%\*.cs"
if errorlevel 1 exit /b 1
echo Built: %OUT%\GT6 Launcher.exe