@echo off
rem MinGW-w64 (MSYS2) build of the dinput8.dll proxy -> copy next to TekkenGame-Win64-Shipping.exe
rem   build.bat           release (quiet log, overlay, seamless rematch)
rem   build.bat research  same + full game trace / match-loop / UE probes in rematch_mod.log
where g++ >nul 2>nul || set "PATH=C:\msys64\mingw64\bin;%PATH%"
set DEFS=
if /i "%1"=="research" set DEFS=-DRESEARCH=1
g++ -shared -O2 -static -s -Wall %DEFS% -I. -Iimgui -DIMGUI_USER_CONFIG=\"imgui_user_config.h\" dinput8.cpp imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp imgui/imgui_impl_dx11.cpp imgui/imgui_impl_win32.cpp -o dinput8.dll -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -limm32 -lgdi32 || exit /b 1
echo built dinput8.dll %1
