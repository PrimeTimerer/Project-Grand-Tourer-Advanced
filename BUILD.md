# Building from source

Players need only the release zip (see [README.md](README.md)). This page is for building the port yourself on a
Windows 10 / 11 x64 PC.

## Tools

- **Visual Studio 2022** (Community) with *Desktop development with C++* and *C++ Clang tools for Windows*; CMake and
  Ninja come with it. `build_release.bat` calls the Community edition's `vcvars64.bat`; change that line for another
  edition.
- **Python 3** with ps3recomp's packages: `pip install -r ps3recomp\requirements.txt`. `build_release.bat` uses
  `python` from PATH, or the interpreter in the `PY` environment variable if it is set.
- **SDL2**: the Visual C++ development package (`SDL2-devel-2.x.x-VC.zip`), unpacked so that
  `third_party\SDL2\include` and `third_party\SDL2\lib\x64\SDL2.lib` exist.
- The launcher is built with the C# compiler of the .NET Framework 4 that comes with Windows.

## 1. Source and engine

```powershell
git clone --recursive https://github.com/PrimeTimerer/Project-Grand-Tourer-Advanced
cd Project-Grand-Tourer-Advanced\ps3recomp
Get-ChildItem ..\patches\*.patch | Sort-Object Name | ForEach-Object { git apply --whitespace=nowarn $_.FullName }
cd ..
```

The submodule stays on the commit this repository pins (ps3recomp v0.18.0). Patches 0001 - 0026 come from the
PS3Recomp starter kit, 0027 - 0057 are this port's. Applied in number order, they give exactly the engine the release
was built with.

## 2. Game files

Run the launcher's setup once with your own disc (`GT6 Launcher.exe` from a release, or the one from step 5). It fills
the `game\` folder beside the launcher. Move that folder into the repository folder as `game\`: `game.path` names it,
and the build reads `game\EMAIN.ELF` and `game\firmware\libsre.prx` from there.

## 3. Lift the game's PPU code

```powershell
python -X utf8 tools\rpcs3_patches.py <RPCS3 patch.yml> --emit
python -X utf8 ps3recomp\tools\ppu_loader.py game\EMAIN.ELF -o out\
python -X utf8 ps3recomp\tools\ppu_lifter.py game\EMAIN.ELF --functions out\EMAIN.functions.json --hle-stubs out\EMAIN.imports.json --insn-patches out\EMAIN.insn_patches.json --output recompiled\ --chunk-lines 100000 --code-end 0x10C2208
python -X utf8 tools\post_lift.py recompiled
python -X utf8 tools\trainer_post_lift.py recompiled --bindings tools\native_bindings.json
python -X utf8 tools\optimize_pdi_leaf.py
python -X utf8 tools\optimize_pdi_leaves.py
```

- The first line turns RPCS3's game patches for BCES01893 1.22 into the port's run-time switches (`GT6_PATCH_*`). It
  needs RPCS3's patch file; without it, leave out `--insn-patches ...` and those switches do nothing.
- The lift takes about a minute. The post-lift tools check what they change against fixed hashes and are safe to run
  twice. `trainer_post_lift.py` binds the in-game menu's game bridge (`gt6_trainer.cpp`) to the sites in
  `tools\native_bindings.json`.

## 3b. Lift the firmware's SPURS library (libsre)

The game's SPURS task sets and job chains run on the firmware's own libsre (FW 4.93, which the setup puts into
`game\firmware\`), lifted like the game. The build takes it from `prx_src\libsre\`: `ppu_recomp.h`,
`ppu_recomp_*.cpp` and `libsre_exports.c`.

```powershell
python -X utf8 ps3recomp\tools\lift_prx.py game\firmware\libsre.prx --base 0x30000000 --output out\libsre\
python -X utf8 ps3recomp\tools\ppu_lifter.py out\libsre\libsre_image.bin --raw --base 0x30000000 --functions out\libsre\libsre_functions.json --symbol-prefix libsre_ --hle-stubs <stubs> --output prx_src\libsre\
python -X utf8 ps3recomp\tools\prx_exports.py game\firmware\libsre.prx --symbol libsre --out prx_src\libsre\libsre_exports.c
```

`<stubs>` lists the functions libsre imports itself (`out\libsre\libsre_imports.json` names them), so that the
lifted library calls the port's own versions of them.

## 4. Lift the SPU jobs (optional)

The build lifts the SPU images of `game\EMAIN.ELF` and `game\firmware\libsre.prx` by itself. Three more sets are
optional; without them those jobs run in the SPU interpreter, which is slower:

- `spu_jobs\` (the job modules): `python -X utf8 ps3recomp\tools\lift_jobmods.py --engine-elf game\EMAIN.ELF
  --engine-zero-base --entry-hints spu_jobs\entry_hints.json --out spu_jobs --symbol-prefix gt6`. The entry hints come
  from an RPCS3 SPU cache of the game (`tools\spu_cache_hints.py`).
- `spu_policy\` (the job manager): `python -X utf8 tools\lift_policy_module.py`
- `spu_audio\` (the audio jobs): `python -X utf8 tools\lift_audio_module.py` (its header says what it reads).

## 5. Build

```powershell
.\build_release.bat           # the game: build-release\GT6Recomp.exe
.\build_launcher.bat          # the launcher: build-launcher\GT6 Launcher.exe
```

## 6. Package

Put `GT6Recomp.exe`, `SDL2.dll`, the Visual C++ runtime DLLs, `GT6 Launcher.exe` and `package\PLAY_GT6.bat` into one
folder, and run
`pwsh -File tools\make_release.ps1 -Package <that folder>` for the release zip.
