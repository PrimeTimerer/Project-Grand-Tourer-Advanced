@echo off
rem Optimised build (Release) in build-release\.  Usage: build_release.bat [extra cmake --build args]
setlocal
set "ROOT=%~dp0"
set PYTHONUTF8=1
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin;%PATH%"
cd /d "%ROOT%"
rem The Python that runs the ps3recomp generators: python on PATH, or PY set beforehand.
if not defined PY set "PY=python"
set "GAMEDIR="
for /f "usebackq delims=" %%G in ("%ROOT%game.path") do set "GAMEDIR=%ROOT%%%G"
if not exist "%GAMEDIR%\EMAIN.ELF" (
  echo Missing canonical game data. Check game.path.
  exit /b 1
)
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
  "-DPython3_EXECUTABLE=%PY%" ^
  "-DPORT_SPU_INPUTS=%GAMEDIR%\EMAIN.ELF;%GAMEDIR%\firmware\libsre.prx" || exit /b 1
cmake --build build-release -j 16 %* || exit /b 1
