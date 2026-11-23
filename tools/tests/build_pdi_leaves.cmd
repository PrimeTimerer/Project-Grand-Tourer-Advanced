@echo off
rem Builds tools\tests\test_pdi_leaves.c against <dir>\pdi_leaves_cases.inc with the flags of the lifted SPU sources.
rem usage: build_pdi_leaves.cmd <dir>     (first: python tools\optimize_pdi_leaves.py --fixture <dir>)
if "%~1"=="" exit /b 2
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PDI_ROOT=%~dp0..\.."
set "PDI_OUT=%~1"
set "PDI_CC=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-cl.exe"
set PDI_COMMON=/nologo /TC /std:c17 /O2 /Ob2 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS -w -fno-strict-aliasing -fwrapv -msse4.1 -mavx2 -mfma /I "%PDI_ROOT%\ps3recomp\runtime\spu" /I "%PDI_ROOT%\ps3recomp\include" /I "%PDI_ROOT%\ps3recomp\runtime\platform" /I "%PDI_ROOT%\ps3recomp\runtime" /I "%PDI_OUT%"
"%PDI_CC%" %PDI_COMMON% /c "%PDI_ROOT%\tools\tests\pdi_leaf_callbacks.c" /Fo"%PDI_OUT%\callbacks.obj" || exit /b 1
"%PDI_CC%" %PDI_COMMON% "%PDI_ROOT%\tools\tests\test_pdi_leaves.c" /Fe"%PDI_OUT%\test_pdi_leaves.exe" /Fo"%PDI_OUT%\test_pdi_leaves.obj" /link /OPT:REF "%PDI_OUT%\callbacks.obj" || exit /b 1
