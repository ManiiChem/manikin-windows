@echo off
rem poor mans makefile
rem   make            build build\companion.exe
rem   make run        build then launch it
rem   make console    build with a console window (for printf debugging) and run it
rem   make preview    draw his built in moods to build\preview.png
rem   make anim X     draw the scenario for animation module X to build\anim_X.png
rem                   (emotion, gaze, sleep, physics, point, bye, wheel)
rem   make showreel   record the animations at real speed as GIFs in build\showreel\
rem   make sounds     build the soundboard (build\soundboard) and open it
rem   make sounds check|export   check every sounds length and level, or write them to build\sounds
rem   make icon       redraw his app icon from the sprite into assets\manikin.ico
rem                   (plus each size as a PNG in build\icon)
rem   make setup      make brain\.venv and install the brains python packages
rem   make test       run the brains offline tests
rem   make ctest      build and run the C tests (pointing, driving, input)
rem   make clean      delete build output (keeps build\data and build\sandbox)
rem
rem set MANIKIN_BACKEND=echo to run the brain without calling the API
setlocal
cd /d "%~dp0"

set TARGET=%1
if "%TARGET%"=="" set TARGET=build
set SCENARIO=%2

set MASCOT_SRCS=src\mascot.c src\anim_emotion.c src\anim_gaze.c src\anim_sleep.c src\anim_physics.c src\anim_point.c src\anim_bye.c src\anim_wheel.c
set OUT=build
set PREVIEW_PNG=build\preview.png
if /i not "%TARGET%"=="anim" goto scenario_done
if "%SCENARIO%"=="" (
    echo usage: make anim emotion^|gaze^|sleep^|physics^|point^|bye^|wheel
    exit /b 1
)
set OUT=build\anim_%SCENARIO%
set PREVIEW_PNG=build\anim_%SCENARIO%.png
:scenario_done
if /i "%TARGET%"=="showreel" set OUT=build\showreel
if /i "%TARGET%"=="sounds" set OUT=build\soundboard
if /i "%TARGET%"=="icon" set OUT=build\icon
if /i "%TARGET%"=="ctest" set OUT=build\ctest

if /i "%TARGET%"=="build"   goto build
if /i "%TARGET%"=="run"     goto build
if /i "%TARGET%"=="console" goto build
if /i "%TARGET%"=="preview" goto build
if /i "%TARGET%"=="anim"    goto build
if /i "%TARGET%"=="showreel" goto build
if /i "%TARGET%"=="sounds"  goto build
if /i "%TARGET%"=="icon"    goto build
if /i "%TARGET%"=="ctest"   goto build
if /i "%TARGET%"=="setup"   goto setup
if /i "%TARGET%"=="test"    goto test
if /i "%TARGET%"=="clean"   goto clean
echo unknown target "%TARGET%" (try: build, run, console, preview, anim, showreel, sounds, icon, setup, test, ctest, clean)
exit /b 1

:clean
rem his memory and sandbox live in build\data and build\sandbox, never delete those
if not exist build goto cleaned
for /d %%d in (build\*) do if /i not "%%~nxd"=="data" if /i not "%%~nxd"=="sandbox" rmdir /s /q "%%d"
del /q build\*.* 2>nul
:cleaned
echo cleaned (kept build\data and build\sandbox)
exit /b 0

:setup
if exist brain\.venv\Scripts\python.exe goto have_venv
py -3 -m venv brain\.venv 2>nul || python -m venv brain\.venv
if errorlevel 1 exit /b 1
:have_venv
brain\.venv\Scripts\python.exe -m pip install --quiet --upgrade pip
brain\.venv\Scripts\python.exe -m pip install --quiet -r brain\requirements.txt
if errorlevel 1 exit /b 1
echo brain ready
exit /b 0

:test
if not exist brain\.venv\Scripts\python.exe (
    echo run "make setup" first
    exit /b 1
)
pushd brain
.venv\Scripts\python.exe -m unittest discover tests
set RESULT=%errorlevel%
popd
exit /b %RESULT%

:build
rem not in a developer prompt? find visual studio and load its environment
where cl >nul 2>nul
if not errorlevel 1 goto have_cl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo could not find Visual Studio with the C++ tools
    exit /b 1
)
rem vcvars looks for vswhere.exe on PATH too and complains for nothing, so hide it
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
:have_cl

set CFLAGS=/nologo /O2 /W3 /MD /std:c11 /D_CRT_SECURE_NO_WARNINGS /DCJSON_HIDE_SYMBOLS /Ivendor\raylib\include /Ivendor\cjson /Isrc
set LIBS=/LIBPATH:vendor\raylib\lib raylib.lib opengl32.lib gdi32.lib winmm.lib user32.lib shell32.lib advapi32.lib
if not exist %OUT% mkdir %OUT%

if /i "%TARGET%"=="preview" goto preview
if /i "%TARGET%"=="anim"    goto preview
if /i "%TARGET%"=="showreel" goto showreel
if /i "%TARGET%"=="sounds"  goto sounds
if /i "%TARGET%"=="icon"    goto icon
if /i "%TARGET%"=="ctest"   goto ctest

set SUBSYS=/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup
if /i "%TARGET%"=="console" set SUBSYS=/SUBSYSTEM:CONSOLE

rem his icon for explorer and pinned shortcuts if rc is around, the window and taskbar
rem get it either way since he draws it at startup (src\icon.c)
set RES=
where rc >nul 2>nul
if errorlevel 1 goto no_rc
rc /nologo /i assets /fo %OUT%\manikin.res assets\manikin.rc
if not errorlevel 1 set RES=%OUT%\manikin.res
:no_rc

cl %CFLAGS% src\main.c %MASCOT_SRCS% src\ui.c src\icon.c src\chat.c src\speech.c src\pointer.c src\drive.c src\point_fx.c src\brain.c src\platform_win32.c src\synth.c src\sfx.c src\cues.c vendor\cjson\cJSON.c ^
   /Fo%OUT%\ /Fe%OUT%\companion.exe /link %LIBS% %SUBSYS% %RES%
if errorlevel 1 exit /b 1

if /i "%TARGET%"=="run"     start "" %OUT%\companion.exe
if /i "%TARGET%"=="console" %OUT%\companion.exe
exit /b 0

:preview
cl %CFLAGS% tools\preview.c %MASCOT_SRCS% src\ui.c src\point_fx.c ^
   /Fo%OUT%\ /Fe%OUT%\preview.exe /link %LIBS% /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
if /i "%TARGET%"=="anim" (%OUT%\preview.exe %PREVIEW_PNG% %SCENARIO%) else (%OUT%\preview.exe %PREVIEW_PNG%)
exit /b %errorlevel%

:showreel
cl %CFLAGS% tools\showreel.c %MASCOT_SRCS% src\ui.c src\point_fx.c ^
   /Fo%OUT%\ /Fe%OUT%\showreel.exe /link %LIBS% /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
del /q %OUT%\*.png %OUT%\*.gif 2>nul
%OUT%\showreel.exe %OUT%
if errorlevel 1 exit /b 1
if not exist brain\.venv\Scripts\python.exe (
    echo frames are in %OUT%; run "make setup" to turn them into GIFs
    exit /b 0
)
brain\.venv\Scripts\python.exe tools\make_gifs.py %OUT%
exit /b %errorlevel%

:icon
cl %CFLAGS% tools\make_icon.c src\icon.c %MASCOT_SRCS% src\ui.c src\point_fx.c ^
   /Fo%OUT%\ /Fe%OUT%\make_icon.exe /link %LIBS% /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
%OUT%\make_icon.exe assets\manikin.ico %OUT%
exit /b %errorlevel%

:sounds
if "%SCENARIO%"=="" goto sounds_ok
if /i "%SCENARIO%"=="check" goto sounds_ok
if /i "%SCENARIO%"=="export" goto sounds_ok
echo usage: make sounds [check^|export]
exit /b 1
:sounds_ok
rem the synth is plain C, raylib is only here for the window, the speakers and ExportWave
cl %CFLAGS% tools\soundboard.c src\synth.c ^
   /Fo%OUT%\ /Fe%OUT%\soundboard.exe /link %LIBS% /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
if /i "%SCENARIO%"=="check"  %OUT%\soundboard.exe --check
if /i "%SCENARIO%"=="export" %OUT%\soundboard.exe --export build\sounds
if "%SCENARIO%"==""          %OUT%\soundboard.exe
exit /b %errorlevel%

:ctest
cl %CFLAGS% tools\pointer_test.c /Fo%OUT%\ /Fe%OUT%\pointer_test.exe
if errorlevel 1 exit /b 1
%OUT%\pointer_test.exe
if errorlevel 1 exit /b 1
cl %CFLAGS% tools\drive_test.c src\pointer.c /Fo%OUT%\ /Fe%OUT%\drive_test.exe
if errorlevel 1 exit /b 1
%OUT%\drive_test.exe
if errorlevel 1 exit /b 1
cl %CFLAGS% tools\input_test.c /Fo%OUT%\ /Fe%OUT%\input_test.exe /link user32.lib advapi32.lib
if errorlevel 1 exit /b 1
%OUT%\input_test.exe
exit /b %errorlevel%
