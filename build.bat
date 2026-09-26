@echo off
setlocal EnableExtensions

rem Run every target from the repository root, even when this batch file is
rem called from another working directory. The wrapper restores the caller's
rem directory on every exit path.
if defined _RG_AUDIO_BUILD_IN_ROOT goto build_main
set "_RG_AUDIO_BUILD_IN_ROOT=1"
pushd "%~dp0" >nul 2>nul
if errorlevel 1 (
	echo Could not enter the rg_audio repository directory: %~dp0
	exit /b 1
)
call "%~f0" %*
set "_RG_AUDIO_BUILD_EXIT=%errorlevel%"
popd
exit /b %_RG_AUDIO_BUILD_EXIT%

:build_main
set "RG_AUDIO_ROOT=%~dp0"
set "TARGET=%~1"
if not defined TARGET set "TARGET=test"
if not defined RG_CORE_DIR set "RG_CORE_DIR=%~dp0..\rg_core"
if not defined RG_GUI_DIR set "RG_GUI_DIR=%~dp0..\rg_gui"
if not defined RG_TEXT_DIR set "RG_TEXT_DIR=%~dp0..\rg_text"

if /I "%TARGET%"=="clean" goto clean
if /I "%TARGET%"=="test" goto test
if /I "%TARGET%"=="test_ci" goto test_ci
if /I "%TARGET%"=="test_release" goto test_release
if /I "%TARGET%"=="test_rgs" goto test_rgs
if /I "%TARGET%"=="runtime" goto test_rgs
if /I "%TARGET%"=="test_allocator" goto test_allocator
if /I "%TARGET%"=="allocator" goto test_allocator
if /I "%TARGET%"=="test_cpp" goto test_cpp
if /I "%TARGET%"=="cpp" goto test_cpp
if /I "%TARGET%"=="test_stream" goto test_stream
if /I "%TARGET%"=="stream" goto test_stream
if /I "%TARGET%"=="test_tools" goto test_tools
if /I "%TARGET%"=="test_prepare" goto test_prepare
if /I "%TARGET%"=="test_player_stream" goto test_player_stream
if /I "%TARGET%"=="playback_check" goto playback_check
if /I "%TARGET%"=="example" goto example
if /I "%TARGET%"=="rgs_convert" goto rgs_convert
if /I "%TARGET%"=="rgs_player" goto rgs_player
if /I "%TARGET%"=="player" goto rgs_player
if /I "%TARGET%"=="shaders" goto shaders
if /I "%TARGET%"=="tools" goto tools
if /I "%TARGET%"=="bench" goto bench
if /I "%TARGET%"=="benchmarks" goto bench
if /I "%TARGET%"=="bench_rgs" goto bench_rgs
if /I "%TARGET%"=="bench_stream" goto bench_stream
if /I "%TARGET%"=="bench_compare" goto bench_compare
if /I "%TARGET%"=="bench_workloads" goto bench_workloads
if /I "%TARGET%"=="bench_playback" goto bench_playback
if /I "%TARGET%"=="bench_decode" goto bench_decode

echo Unknown target: %TARGET%
goto help_error

:ensure_compiler
where cl >nul 2>nul
if not errorlevel 1 exit /b 0
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
	echo Could not find cl.exe or vswhere.exe.
	exit /b 1
)
set "VSINSTALL="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
	echo Could not find a Visual Studio C++ installation.
	exit /b 1
)
call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
exit /b 0

:validate_runtime_dependency
if not exist "%RG_CORE_DIR%\src\rg_defs.h" (
	echo rg_core not found at "%RG_CORE_DIR%".
	echo Set RG_CORE_DIR to the rg_core repository root.
	exit /b 1
)
exit /b 0

:validate_player_dependencies
call :validate_runtime_dependency
if errorlevel 1 exit /b 1
if not exist "%RG_GUI_DIR%\src\rg_gui_gpu.h" (
	echo rg_gui not found at "%RG_GUI_DIR%".
	echo Set RG_GUI_DIR to the rg_gui repository root.
	exit /b 1
)
if not exist "%RG_TEXT_DIR%\src\rg_text.h" (
	echo rg_text not found at "%RG_TEXT_DIR%".
	echo Set RG_TEXT_DIR to the rg_text repository root.
	exit /b 1
)
exit /b 0

:find_sdl
if defined SDL3_INCLUDE_DIR if defined SDL3_LIB_DIR goto find_sdl_validate
if defined SDL3_DIR goto find_sdl_from_root
if defined VCPKG_INSTALLED_DIR if exist "%VCPKG_INSTALLED_DIR%\x64-windows\include\SDL3\SDL.h" set "SDL3_DIR=%VCPKG_INSTALLED_DIR%\x64-windows"
if not defined SDL3_DIR if exist "%RG_AUDIO_ROOT%vcpkg_installed\x64-windows\include\SDL3\SDL.h" set "SDL3_DIR=%RG_AUDIO_ROOT%vcpkg_installed\x64-windows"
if not defined SDL3_DIR if defined VCPKG_ROOT if exist "%VCPKG_ROOT%\installed\x64-windows\include\SDL3\SDL.h" set "SDL3_DIR=%VCPKG_ROOT%\installed\x64-windows"

:find_sdl_from_root
if not defined SDL3_INCLUDE_DIR set "SDL3_INCLUDE_DIR=%SDL3_DIR%\include"
if not defined SDL3_LIB_DIR if exist "%SDL3_DIR%\lib\x64\SDL3.lib" set "SDL3_LIB_DIR=%SDL3_DIR%\lib\x64"
if not defined SDL3_LIB_DIR if exist "%SDL3_DIR%\lib\SDL3.lib" set "SDL3_LIB_DIR=%SDL3_DIR%\lib"
if not defined SDL3_BIN_DIR if exist "%SDL3_DIR%\bin" set "SDL3_BIN_DIR=%SDL3_DIR%\bin"
if not defined SDL3_BIN_DIR if exist "%SDL3_DIR%\lib\x64\SDL3.dll" set "SDL3_BIN_DIR=%SDL3_DIR%\lib\x64"

:find_sdl_validate
if not exist "%SDL3_INCLUDE_DIR%\SDL3\SDL.h" exit /b 1
if not exist "%SDL3_LIB_DIR%\SDL3.lib" exit /b 1
exit /b 0

:setup_runtime
call :ensure_compiler
if errorlevel 1 exit /b 1
call :validate_runtime_dependency
if errorlevel 1 exit /b 1
set RUNTIME_FLAGS=/nologo /std:c11 /W4 /WX /O2 /I src /I "%RG_CORE_DIR%\src"
exit /b 0

:setup_sdl_tool
call :setup_runtime
if errorlevel 1 exit /b 1
call :find_sdl
if errorlevel 1 (
	echo SDL3 not found. Set SDL3_DIR or SDL3_INCLUDE_DIR and SDL3_LIB_DIR.
	exit /b 1
)
if defined SDL3_BIN_DIR set "PATH=%SDL3_BIN_DIR%;%PATH%"
call :stage_sdl_runtime "%RG_AUDIO_ROOT%"
if errorlevel 1 exit /b 1
call :find_audio_deps
if errorlevel 1 exit /b 1
set SDL_FLAGS=%RUNTIME_FLAGS% /I "%SDL3_INCLUDE_DIR%" /I "%RG_AUDIO_DEPS_DIR%\include"
exit /b 0

:setup_player
call :ensure_compiler
if errorlevel 1 exit /b 1
call :validate_player_dependencies
if errorlevel 1 exit /b 1
call :find_sdl
if errorlevel 1 (
	echo SDL3 not found. Set SDL3_DIR or SDL3_INCLUDE_DIR and SDL3_LIB_DIR.
	exit /b 1
)
if defined SDL3_BIN_DIR set "PATH=%SDL3_BIN_DIR%;%PATH%"
call :stage_sdl_runtime "%RG_AUDIO_ROOT%"
if errorlevel 1 exit /b 1
call :find_audio_deps
if errorlevel 1 exit /b 1
set PLAYER_FLAGS=/nologo /std:c11 /W4 /WX /O2 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I src /I "%RG_CORE_DIR%\src" /I "%RG_GUI_DIR%\src" /I "%RG_TEXT_DIR%\src" /I "%SDL3_INCLUDE_DIR%" /I "%RG_AUDIO_DEPS_DIR%\include"
exit /b 0

:stage_sdl_runtime
rem Windows searches the executable directory before system DLLs and PATH.
if not defined SDL3_BIN_DIR (
	echo SDL3 runtime missing. Set SDL3_BIN_DIR to the directory containing SDL3.dll.
	exit /b 1
)
if not exist "%SDL3_BIN_DIR%\SDL3.dll" exit /b 1
for %%I in ("%SDL3_BIN_DIR%\SDL3.dll") do set "RGS_SDL_SOURCE=%%~fI"
for %%I in ("%~1\SDL3.dll") do set "RGS_SDL_DESTINATION=%%~fI"
if /I "%RGS_SDL_SOURCE%"=="%RGS_SDL_DESTINATION%" exit /b 0
copy /Y "%RGS_SDL_SOURCE%" "%RGS_SDL_DESTINATION%" >nul
exit /b %errorlevel%

:find_audio_deps
if not defined RG_AUDIO_DEPS_DIR set "RG_AUDIO_DEPS_DIR=%RG_AUDIO_ROOT%build\deps\install"
if not exist "%RG_AUDIO_DEPS_DIR%\include\soxr.h" (
	echo Audio tool dependencies missing. Run python tools\build_audio_deps.py or set RG_AUDIO_DEPS_DIR.
	exit /b 1
)
if not exist "%RG_AUDIO_DEPS_DIR%\lib\soxr.lib" exit /b 1
set "PATH=%RG_AUDIO_DEPS_DIR%\bin;%PATH%"
exit /b 0

:test
call "%~f0" test_rgs
if errorlevel 1 exit /b 1
call "%~f0" test_allocator
if errorlevel 1 exit /b 1
call "%~f0" test_cpp
if errorlevel 1 exit /b 1
call "%~f0" test_stream
if errorlevel 1 exit /b 1
call "%~f0" test_player_stream
if errorlevel 1 exit /b 1
call "%~f0" example
if errorlevel 1 exit /b 1
echo All SDL-free rg_audio runtime tests passed.
exit /b 0

:test_ci
call "%~f0" test
if errorlevel 1 exit /b 1
call "%~f0" test_prepare
if errorlevel 1 exit /b 1
call "%~f0" test_tools
if errorlevel 1 exit /b 1
call "%~f0" rgs_player
if errorlevel 1 exit /b 1
echo rg_audio hosted CI suite passed. GPU and audio-device execution were not run; use test_release locally.
exit /b 0

:test_release
call "%~f0" test_ci
if errorlevel 1 exit /b 1
call :find_audio_deps
if errorlevel 1 exit /b 1
call :find_sdl
if errorlevel 1 (
	echo SDL3 not found for the local player smoke test.
	exit /b 1
)
if defined SDL3_BIN_DIR set "PATH=%SDL3_BIN_DIR%;%PATH%"
echo Running local player GPU/audio smoke test...
rgs_player.exe --smoke-test
if errorlevel 1 exit /b 1
echo All rg_audio release tests passed.
exit /b 0

:test_rgs
call :setup_runtime
if errorlevel 1 exit /b 1
cl %RUNTIME_FLAGS% tests\test_rgs.c /Fe:test_rgs.exe
if errorlevel 1 exit /b 1
test_rgs.exe
exit /b %errorlevel%

:test_allocator
call :setup_runtime
if errorlevel 1 exit /b 1
cl %RUNTIME_FLAGS% tests\test_rgs_allocator.c /Fe:test_rgs_allocator.exe
if errorlevel 1 exit /b 1
test_rgs_allocator.exe
exit /b %errorlevel%

:test_cpp
call :ensure_compiler
if errorlevel 1 exit /b 1
call :validate_runtime_dependency
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /I src /I "%RG_CORE_DIR%\src" tests\test_rgs_cpp.cpp /Fe:test_rgs_cpp.exe
if errorlevel 1 exit /b 1
test_rgs_cpp.exe
exit /b %errorlevel%

:test_stream
call :setup_runtime
if errorlevel 1 exit /b 1
cl %RUNTIME_FLAGS% tests\test_rgs_stream.c /Fe:test_rgs_stream.exe
if errorlevel 1 exit /b 1
test_rgs_stream.exe
exit /b %errorlevel%

:example
call :setup_runtime
if errorlevel 1 exit /b 1
cl %RUNTIME_FLAGS% examples\example_rgs.c /Fe:example_rgs.exe
if errorlevel 1 exit /b 1
example_rgs.exe
exit /b %errorlevel%

:rgs_convert
call :setup_sdl_tool
if errorlevel 1 exit /b 1
cl %SDL_FLAGS% tools\rgs_convert.c /Fe:rgs_convert.exe /link /SUBSYSTEM:CONSOLE /LIBPATH:"%SDL3_LIB_DIR%" /LIBPATH:"%RG_AUDIO_DEPS_DIR%\lib" SDL3.lib soxr.lib
if errorlevel 1 exit /b 1
echo Built rgs_convert.exe.
exit /b 0

:test_tools
call :setup_sdl_tool
if errorlevel 1 exit /b 1
cl %SDL_FLAGS% tools\rgs_convert.c /Fe:rgs_convert.exe /link /LIBPATH:"%SDL3_LIB_DIR%" /LIBPATH:"%RG_AUDIO_DEPS_DIR%\lib" SDL3.lib soxr.lib
if errorlevel 1 exit /b 1
cl %SDL_FLAGS% tests\test_rgs_tools.c /Fe:test_rgs_tools.exe /link /LIBPATH:"%SDL3_LIB_DIR%" /LIBPATH:"%RG_AUDIO_DEPS_DIR%\lib" SDL3.lib soxr.lib
if errorlevel 1 exit /b 1
test_rgs_tools.exe rgs_convert.exe
exit /b %errorlevel%

:test_prepare
call :setup_runtime
if errorlevel 1 exit /b 1
call :find_audio_deps
if errorlevel 1 exit /b 1
cl %RUNTIME_FLAGS% /I "%RG_AUDIO_DEPS_DIR%\include" tests\test_rgs_audio_prepare.c /Fe:test_rgs_audio_prepare.exe /link /LIBPATH:"%RG_AUDIO_DEPS_DIR%\lib" soxr.lib
if errorlevel 1 exit /b 1
test_rgs_audio_prepare.exe
exit /b %errorlevel%

:test_player_stream
call :setup_runtime
if errorlevel 1 exit /b 1
cl %RUNTIME_FLAGS% tests\test_rgs_player_stream.c /Fe:test_rgs_player_stream.exe
if errorlevel 1 exit /b 1
test_rgs_player_stream.exe
exit /b %errorlevel%

:tools
call "%~f0" rgs_convert
if errorlevel 1 exit /b 1
call "%~f0" rgs_player
exit /b %errorlevel%

:rgs_player
call :setup_player
if errorlevel 1 exit /b 1
call "%~f0" shaders
if errorlevel 1 exit /b 1
cl %PLAYER_FLAGS% tools\rgs_player.c /Fe:rgs_player.exe /link /LIBPATH:"%SDL3_LIB_DIR%" /LIBPATH:"%RG_AUDIO_DEPS_DIR%\lib" SDL3.lib soxr.lib
if errorlevel 1 exit /b 1
echo Built rgs_player.exe. Hosted builds do not execute its GPU or audio path.
exit /b 0

:playback_check
call :setup_player
if errorlevel 1 exit /b 1
cl %PLAYER_FLAGS% tests\test_rgs_playback.c /Fe:test_rgs_playback.exe /link /LIBPATH:"%SDL3_LIB_DIR%" /LIBPATH:"%RG_AUDIO_DEPS_DIR%\lib" SDL3.lib soxr.lib
if errorlevel 1 exit /b 1
echo Built test_rgs_playback.exe. Run it explicitly with --input to test a real audio device.
exit /b 0

:find_shadercross
if defined SHADERCROSS_EXE goto find_shadercross_validate
if defined VCPKG_INSTALLED_DIR call :find_shadercross_in_prefix "%VCPKG_INSTALLED_DIR%\x64-windows"
if not defined SHADERCROSS_EXE call :find_shadercross_in_prefix "%RG_AUDIO_ROOT%vcpkg_installed\x64-windows"
if not defined SHADERCROSS_EXE if defined VCPKG_ROOT call :find_shadercross_in_prefix "%VCPKG_ROOT%\installed\x64-windows"
if not defined SHADERCROSS_EXE for %%i in (shadercross.exe) do set "SHADERCROSS_EXE=%%~$PATH:i"

:find_shadercross_validate
if not exist "%SHADERCROSS_EXE%" exit /b 1
exit /b 0

:find_shadercross_in_prefix
if exist "%~1\tools\sdl3-shadercross\shadercross.exe" set "SHADERCROSS_EXE=%~1\tools\sdl3-shadercross\shadercross.exe"
if not defined SHADERCROSS_EXE if exist "%~1\tools\sdl3_shadercross\shadercross.exe" set "SHADERCROSS_EXE=%~1\tools\sdl3_shadercross\shadercross.exe"
exit /b 0

:shaders
if not exist "%RG_GUI_DIR%\shaders\rg_gui_text.vert.hlsl" (
	echo rg_gui shader sources not found at "%RG_GUI_DIR%\shaders".
	echo Set RG_GUI_DIR to the rg_gui repository root.
	exit /b 1
)
call :find_shadercross
if errorlevel 1 (
	echo SDL_shadercross not found. Set SHADERCROSS_EXE or add shadercross.exe to PATH.
	exit /b 1
)
if not exist "shaders\Compiled\DXIL" mkdir "shaders\Compiled\DXIL"
if not exist "shaders\Compiled\SPIRV" mkdir "shaders\Compiled\SPIRV"
if not exist "shaders\Compiled\MSL" mkdir "shaders\Compiled\MSL"
call :compile_shader rg_gui_text.vert vertex
if errorlevel 1 exit /b 1
call :compile_shader rg_gui_text.frag fragment
if errorlevel 1 exit /b 1
call :compile_shader rg_gui_expand.comp compute
if errorlevel 1 exit /b 1
call :compile_shader rg_gui_geometry.vert vertex
if errorlevel 1 exit /b 1
call :compile_shader rg_gui_solid.frag fragment
if errorlevel 1 exit /b 1
call :compile_shader rg_gui_image.frag fragment
if errorlevel 1 exit /b 1
call :compile_shader rg_gui_indexed.vert vertex
if errorlevel 1 exit /b 1
call :compile_shader rg_gui_indexed.frag fragment
if errorlevel 1 exit /b 1
echo Compiled all rg_gui player shader formats.
exit /b 0

:compile_shader
"%SHADERCROSS_EXE%" "%RG_GUI_DIR%\shaders\%1.hlsl" -s HLSL -d DXIL -t %2 -e main -o "shaders\Compiled\DXIL\%1.dxil"
if errorlevel 1 exit /b 1
"%SHADERCROSS_EXE%" "%RG_GUI_DIR%\shaders\%1.hlsl" -s HLSL -d SPIRV -t %2 -e main -o "shaders\Compiled\SPIRV\%1.spv"
if errorlevel 1 exit /b 1
"%SHADERCROSS_EXE%" "%RG_GUI_DIR%\shaders\%1.hlsl" -s HLSL -d MSL -t %2 -e main -o "shaders\Compiled\MSL\%1.msl"
if errorlevel 1 exit /b 1
exit /b 0

:bench
call "%~f0" bench_rgs
if errorlevel 1 exit /b 1
call "%~f0" bench_stream
exit /b %errorlevel%

:bench_rgs
call :setup_runtime
if errorlevel 1 exit /b 1
cl %RUNTIME_FLAGS% tests\bench_rgs.c /Fe:bench_rgs.exe
if errorlevel 1 exit /b 1
bench_rgs.exe %RG_RGS_BENCH_ARGS%
exit /b %errorlevel%

:bench_stream
call :setup_runtime
if errorlevel 1 exit /b 1
cl %RUNTIME_FLAGS% tests\bench_rgs_stream.c /Fe:bench_rgs_stream.exe
if errorlevel 1 exit /b 1
bench_rgs_stream.exe %RG_RGS_STREAM_BENCH_ARGS%
exit /b %errorlevel%

:bench_decode
call :setup_runtime
if errorlevel 1 exit /b 1
if not defined RG_AUDIO_DECODE_BASELINE set "RG_AUDIO_DECODE_BASELINE=%RG_AUDIO_ROOT%benchmarks\baselines\rg_rgs_2026_09_26.h"
if not defined RG_AUDIO_DECODE_BUILD_DIR set "RG_AUDIO_DECODE_BUILD_DIR=%RG_AUDIO_ROOT%build\decode-profile"
for %%i in ("%RG_AUDIO_DECODE_BASELINE%") do set "RG_AUDIO_DECODE_BASELINE=%%~fi"
for %%i in ("%RG_AUDIO_DECODE_BUILD_DIR%") do set "RG_AUDIO_DECODE_BUILD_DIR=%%~fi"
if not exist "%RG_AUDIO_DECODE_BASELINE%" (
	echo The selected decoder baseline is missing: %RG_AUDIO_DECODE_BASELINE%
	exit /b 1
)
python tools\decode_build_metadata.py --check-baseline
if errorlevel 1 exit /b 1
if not exist "%RG_AUDIO_DECODE_BUILD_DIR%" mkdir "%RG_AUDIO_DECODE_BUILD_DIR%"
set "RG_AUDIO_DECODE_BASELINE_INCLUDE=%RG_AUDIO_DECODE_BASELINE:\=/%"
cl %RUNTIME_FLAGS% "/DRG_RGS_HEADER=\"%RG_AUDIO_DECODE_BASELINE_INCLUDE%\"" benchmarks\decode_profile.c /Fo:"%RG_AUDIO_DECODE_BUILD_DIR%\baseline.obj" /Fe:"%RG_AUDIO_DECODE_BUILD_DIR%\baseline.exe"
if errorlevel 1 exit /b 1
cl %RUNTIME_FLAGS% benchmarks\decode_profile.c /Fo:"%RG_AUDIO_DECODE_BUILD_DIR%\candidate.obj" /Fe:"%RG_AUDIO_DECODE_BUILD_DIR%\candidate.exe"
if errorlevel 1 exit /b 1
python tools\decode_build_metadata.py
if errorlevel 1 exit /b 1
echo Built baseline and candidate decode profilers with build metadata under %RG_AUDIO_DECODE_BUILD_DIR%.
exit /b 0

:bench_compare
call :setup_runtime
if errorlevel 1 exit /b 1
call :find_audio_deps
if errorlevel 1 exit /b 1
if not exist "%RG_AUDIO_DEPS_DIR%\include\sndfile.h" exit /b 1
if not exist "build\bench" mkdir "build\bench"
if not exist "build\baseline\src\rg_rgs.h" (
	if not exist "build\baseline" mkdir "build\baseline"
	git archive af980e8 -o build\baseline.tar
	if errorlevel 1 exit /b 1
	tar -xf build\baseline.tar -C build\baseline
	if errorlevel 1 exit /b 1
)
cl %RUNTIME_FLAGS% /I "%RG_AUDIO_DEPS_DIR%\include" benchmarks\codec_adapter.c /Fo:build\bench\candidate.obj /Fe:build\bench\codec_candidate.exe /link /LIBPATH:"%RG_AUDIO_DEPS_DIR%\lib" soxr.lib sndfile.lib
if errorlevel 1 exit /b 1
cl %RUNTIME_FLAGS% /I "%RG_AUDIO_DEPS_DIR%\include" /DRG_RGS_HEADER=\"../build/baseline/src/rg_rgs.h\" benchmarks\codec_adapter.c /Fo:build\bench\baseline.obj /Fe:build\bench\codec_baseline.exe /link /LIBPATH:"%RG_AUDIO_DEPS_DIR%\lib" soxr.lib sndfile.lib
if errorlevel 1 exit /b 1
python tools\bench_build_metadata.py "%RG_AUDIO_DEPS_DIR%"
if errorlevel 1 exit /b 1
echo Built baseline and candidate adapters. Run python benchmarks\run_benchmarks.py to measure the corpus.
exit /b 0

:bench_workloads
call :setup_runtime
if errorlevel 1 exit /b 1
if not exist "build\bench" mkdir "build\bench"
if not exist "build\baseline\src\rg_rgs.h" (
	echo Build the frozen baseline first with build.bat bench_compare.
	exit /b 1
)
cl %RUNTIME_FLAGS% benchmarks\stream_workload.c /Fo:build\bench\stream_candidate.obj /Fe:build\bench\stream_candidate.exe
if errorlevel 1 exit /b 1
cl %RUNTIME_FLAGS% /DRG_RGS_HEADER=\"../build/baseline/src/rg_rgs.h\" benchmarks\stream_workload.c /Fo:build\bench\stream_baseline.obj /Fe:build\bench\stream_baseline.exe
if errorlevel 1 exit /b 1
echo Built baseline and candidate stream workload executables.
exit /b 0

:bench_playback
call :setup_sdl_tool
if errorlevel 1 exit /b 1
if not exist "build\bench" mkdir "build\bench"
call :stage_sdl_runtime "build\bench"
if errorlevel 1 exit /b 1
cl %SDL_FLAGS% benchmarks\playback_resample.c /Fo:build\bench\playback_resample.obj /Fe:build\bench\playback_resample.exe /link /LIBPATH:"%SDL3_LIB_DIR%" /LIBPATH:"%RG_AUDIO_DEPS_DIR%\lib" SDL3.lib sndfile.lib
if errorlevel 1 exit /b 1
echo Built build\bench\playback_resample.exe. Use a prepared PCM WAV, block size, and trial count.
exit /b 0

:clean
if not exist "%RG_AUDIO_ROOT%src\rg_rgs.h" (
	echo Refusing to clean: repository marker is missing under "%RG_AUDIO_ROOT%".
	exit /b 1
)
for %%f in (test_rgs_audio_prepare.exe test_rgs.exe test_rgs_allocator.exe test_rgs_cpp.exe test_rgs_stream.exe test_rgs_tools.exe test_rgs_player_stream.exe test_rgs_playback.exe example_rgs.exe rgs_convert.exe rgs_player.exe bench_rgs.exe bench_rgs_stream.exe) do del /q "%RG_AUDIO_ROOT%%%f" 2>nul
for %%f in (test_rgs_audio_prepare.obj test_rgs.obj test_rgs_allocator.obj test_rgs_cpp.obj test_rgs_stream.obj test_rgs_tools.obj test_rgs_player_stream.obj test_rgs_playback.obj example_rgs.obj rgs_convert.obj rgs_player.obj bench_rgs.obj bench_rgs_stream.obj) do del /q "%RG_AUDIO_ROOT%%%f" 2>nul
if exist "%RG_AUDIO_ROOT%shaders\Compiled" rmdir /s /q "%RG_AUDIO_ROOT%shaders\Compiled"
for %%f in (test_rgs_audio_prepare.exe test_rgs.exe test_rgs_allocator.exe test_rgs_cpp.exe test_rgs_stream.exe test_rgs_tools.exe test_rgs_player_stream.exe test_rgs_playback.exe example_rgs.exe rgs_convert.exe rgs_player.exe bench_rgs.exe bench_rgs_stream.exe) do if exist "%RG_AUDIO_ROOT%%%f" (
	echo Failed to remove build artifact: %%f
	exit /b 1
)
if exist "%RG_AUDIO_ROOT%shaders\Compiled" (
	echo Failed to remove generated shader directory.
	exit /b 1
)
exit /b 0

:help_error
echo.
echo Usage: build.bat ^<target^>
echo.
echo Runtime: test, test_rgs, test_allocator, test_cpp, test_stream, example
echo Hosted: test_ci, test_tools, test_player_stream
echo Asset preprocessing: test_prepare
echo Tools: rgs_convert, rgs_player, shaders, tools
echo Optional audio-device test build: playback_check
echo Benchmarks: bench, bench_rgs, bench_stream, bench_compare, bench_workloads, bench_playback, bench_decode
echo bench_decode overrides: RG_AUDIO_DECODE_BASELINE, RG_AUDIO_DECODE_BASELINE_SHA256, RG_AUDIO_DECODE_BUILD_DIR
echo Release: test_release, clean
exit /b 1
