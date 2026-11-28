@echo off
rem Builds tools\tests\test_native_a8dec1.c against <dir>\native_a8dec1_case.inc with the flags of the lifted SPU sources.
rem usage: build_native_a8dec1.cmd <dir>     (first: python tools\tests\prepare_native_a8dec1_fixture.py <dir>)
if "%~1"=="" exit /b 2
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "NC_ROOT=%~dp0..\.."
set "NC_OUT=%~1"
set "NC_CC=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-cl.exe"
set NC_COMMON=/nologo /TC /std:c17 /O2 /Ob2 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DSPU_CALL_FAST_PATH -w -fno-strict-aliasing -fwrapv -msse4.1 -mavx2 -mfma /clang:-ffp-contract=off /I "%NC_ROOT%\ps3recomp\runtime\spu" /I "%NC_ROOT%\ps3recomp\include" /I "%NC_ROOT%\ps3recomp\runtime\platform" /I "%NC_ROOT%\ps3recomp\runtime" /I "%NC_OUT%"
"%NC_CC%" %NC_COMMON% /c "%NC_ROOT%\tools\tests\pdi_leaf_callbacks.c" /Fo"%NC_OUT%\callbacks.obj" || exit /b 1
"%NC_CC%" %NC_COMMON% /c "%NC_ROOT%\gt6_native_a8dec1.c" /Fo"%NC_OUT%\native_a8dec1.obj" || exit /b 1
"%NC_CC%" %NC_COMMON% "%NC_ROOT%\tools\tests\test_native_a8dec1.c" /Fe"%NC_OUT%\test_native_a8dec1.exe" /Fo"%NC_OUT%\test_native_a8dec1.obj" /link /OPT:REF "%NC_OUT%\callbacks.obj" "%NC_OUT%\native_a8dec1.obj" || exit /b 1
