@echo off
rem Builds and runs tools\tests\test_fifo_burst.c (rsx_dispatch_fifo_burst [GT6 0055] against the header-by-header way).
rem usage: build_fifo_burst.cmd <dir for the exe> [streams]
if "%~1"=="" exit /b 2
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
set "FB_ROOT=%~dp0..\.."
set "FB_CC=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-cl.exe"
"%FB_CC%" /nologo /TC /std:c17 /O2 /Ob2 /D_CRT_SECURE_NO_WARNINGS -w -fno-strict-aliasing -fwrapv /I "%FB_ROOT%\ps3recomp\include" /I "%FB_ROOT%\ps3recomp\libs\video" /I "%FB_ROOT%\ps3recomp\runtime" "%FB_ROOT%\tools\tests\test_fifo_burst.c" /Fe"%~1\test_fifo_burst.exe" /Fo"%~1\test_fifo_burst.obj" /link /STACK:8388608 || exit /b 1
"%~1\test_fifo_burst.exe" %2
