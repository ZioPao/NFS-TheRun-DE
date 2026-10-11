@echo off
echo Building NFS The Run Definitive Edition (.asi) with MinGW...

rem GXX/PATH come from the environment when set (CI points them at its own
rem MSYS2); otherwise use the local i686 toolchain.
if not defined GXX (
    set "GXX=C:\msys64\mingw32\bin\g++.exe"
    set "PATH=C:\msys64\mingw32\bin;%PATH%"
)
set OUT=NFSTR_DefinitiveEdition.asi

%GXX% -shared -O2 -std=c++17 -m32 -Wall -Wextra ^
    src/main.cpp ^
    src/logger.cpp ^
    src/config.cpp ^
    src/memory.cpp ^
    src/features/patch_util.cpp ^
    src/features/garage_render.cpp ^
    src/features/extra_ui.cpp ^
    src/features/photo_mode.cpp ^
    src/features/fps_unlocker.cpp ^
    src/features/loading_vsync.cpp ^
    src/features/track_rules.cpp ^
    src/features/vehicle_swap.cpp ^
    src/features/traffic.cpp ^
    src/features/player_vehicle.cpp ^
    src/features/difficulty.cpp ^
    src/features/ai_difficulty.cpp ^
    src/features/nos_tuning.cpp ^
    src/features/tod_randomizer.cpp ^
    src/features/input_state.cpp ^
    src/features/force_feedback.cpp ^
    src/features/difficulty_text.cpp ^
    src/features/engine_audio.cpp ^
    src/features/particle_fix.cpp ^
    src/features/render_settings.cpp ^
    src/features/settings_probe.cpp ^
    -o %OUT% ^
    -lpsapi -lkernel32 -luser32 -ldxguid -luuid ^
    -static-libgcc -static-libstdc++

if %ERRORLEVEL% EQU 0 (
    echo.
    echo BUILD SUCCESSFUL: %OUT% created!
    exit /b 0
) else (
    echo.
    echo BUILD FAILED with error level %ERRORLEVEL%
    exit /b 1
)
