@echo off
setlocal
cd /d "%~dp0"
if not exist "game\EMAIN.ELF" (
  echo Missing game\EMAIN.ELF. Keep the complete package together.
  pause
  exit /b 1
)
if not exist "GT6Recomp.exe" (
  echo Missing GT6Recomp.exe.
  pause
  exit /b 1
)
rem Use this package's own data and current player save.
set "PS3_VFS_ROOT=%~dp0game\dev_bdvd"
set "PS3_HDD0_ROOT=%~dp0game\dev_hdd0"
set "PS3_HDD1_ROOT=%~dp0game\dev_hdd1"
set "PS3_DEV_FLASH=%~dp0game\dev_flash"
set "PS3_GAMEDATA_ROOT=%~dp0game"
set "RSX_SHADER_CACHE_DIR=%~dp0scratch\rsx_shader_cache_v2"
set "GT6_AA=gpu"
set "LD_GPU_AA=1"
set "GT6_MOVIE_POST=gpu"
set "LD_VTX_SLAB=1"
set "LD_GPU_VERTEX_PULL=1"
set "LD_TYPED_TEXTURES=1"
set "LD_TEXTURE_METADATA=1"
set "LD_FRAMEBUFFER_SRGB=1"
set "LD_SHADOW_COMPARE=1"
set "LD_PSO_PREWARM=1"
set "LD_SHADER_PREPARE=1"
set "RSX_FP_COMPILE_INIT=1"
set "GT6_AUDIO_FIFO=1"
set "GT6_AUDIO_AVX512=1"
set "GT6_MOVIE_CLOCK=device"
set "PS3_AUDIO_BUF_MS=40"
set "GT6_AUDIO_PERSISTENT_WORKER=1"
set "GT6_AUDIO_FIFO_WAIT=1"
set "LD_PIPELINE_LOOKAHEAD=1"
set "GT6_NATIVE_PDI_IRQ=1"
set "GT6_NATIVE_PDI_LEAVES=1"
set "PS3_MUTE=0"
set "PS3_VERBOSE=0"
set "PS3_FSLOG=0"
rem Remove automated-input, capture, profiling and unaccepted timing experiments.
for %%V in (PS3_SAMPLE PS3_SAMPLE_MAIN PS3_SAMPLE_DRAIN PS3_SAMPLE_NAME PS3_SAMPLE_RVAS PS3_SAMPLE_MULTI PS3_SAMPLE_MULTI_OUT PS3_SAMPLE_MULTI_MS PS3_SAMPLE_MULTI_FROM PS3_SAMPLE_MULTI_UNTIL) do set "%%V="
for %%V in (GT6_VBLANK_HZ GT6_MOVIE_CLOCK_TRACE GT6_FRAME_TIMING_TRACE LD_TEX_WRITE_WATCH LD_TEX_WW_ADAPTIVE LD_VTX_CACHE_MIN_REFS LD_SURFACE_KEY_INDEX LD_FRAME_DUMP_GATE) do set "%%V="
for %%V in (PAD_SCRIPT PAD_SCRIPT_HOLD PAD_AUTOPRESS PAD_FILE PAD_NO_KEYBOARD GT6_BOOT_WATCH GT6_PEEK GT6_PEEK_MS GT6_BGM_PEEK GT6_BGM_PEEK_RAW GT6_BGM_TRACE GT6_DIAG_SKIP_DOF GT6_DIAG_NO_MOVIE_STREAMS GT6_MOVIE_CAPTURE GT6_MOVIE_POST_CAPTURE GT6_MOVIE_STATS GT6_AUDIO_CLOCK_DIAG GT6_FRAME_TRACE GT6_RENDER_INTERVAL GT6_PATCH_UNLOCK_FPS GCM_BLOCKED_WRITE_KICK GCM_BLOCKED_WRITE_KICK_LOG LD_PRESENT_MAX_FPS LD_SYNC_INTERVAL LD_PSO_RAW_STATE LD_WW_REHARVEST_BLOCKS RSX_NO_SHADER_DISK_CACHE LD_NO_PSO_LIBRARY AUDIO_RATE AUDIO_PEAK AUDIO_WAV AUDIO_FLOW_TRACE AUDIO_FLOW_TRACE_AFTER_MS AUDIO_FLOW_TRACE_DURATION_MS AUDIO_FLOW_TRACE_PORT LD_FRAME_DUMP LD_FRAME_DUMP_FROM LD_FRAME_DUMP_EVERY SPU_FAULT_DUMP SPU_GET_TRACE SPU_GET_TRACE_UNIQUE YDKJ_SPU_LSDUMP SPU_SAMPLE_TID) do set "%%V="
for %%V in (PS3_USLEEP_TRACE PS3_USLEEP_TRACE_TID PS3_USLEEP_TRACE_AFTER_MS PS3_USLEEP_TRACE_DURATION_MS SPU_INTERP_STATS SPU_INTERP_STATS_TID SPU_INTERP_CAPTURE SPU_INTERP_CAPTURE_TID SPU_INTERP_CAPTURE_MIN_PC SPU_INTERP_CAPTURE_MAX_PC SPU_INTERP_CAPTURE_AFTER_MS) do set "%%V="
if not exist "logs" mkdir "logs"
if exist "logs\play_out.txt" copy /y "logs\play_out.txt" "logs\previous_out.txt" >nul
if exist "logs\play_err.txt" copy /y "logs\play_err.txt" "logs\previous_err.txt" >nul
echo Starting GT6 test build. Logs: logs\play_out.txt and logs\play_err.txt
"%~dp0GT6Recomp.exe" >"logs\play_out.txt" 2>"logs\play_err.txt"
set "GT6_EXIT=%ERRORLEVEL%"
if not "%GT6_EXIT%"=="0" (
  echo Game exited with code %GT6_EXIT%. See logs\play_err.txt.
  pause
)
exit /b %GT6_EXIT%
