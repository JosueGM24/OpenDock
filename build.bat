@echo off
rem Compilar en Windows con MSVC (Developer Command Prompt / vcvars64.bat).
rem SQLite: sqlite3.c + sqlite3.h (amalgamacion de sqlite.org) junto al codigo, o bien
rem SQLITE_DIR apuntando a una instalacion estatica de vcpkg (include\ y lib\sqlite3.lib).
rem /MT CRT estatica (sin VCRUNTIME140.dll) - /guard:cf Control Flow Guard - /CETCOMPAT shadow stack
rem /sdl /GS comprobaciones extra - /DEPENDENTLOADFLAG:0x800 DLL que no son KnownDLLs, solo de System32
set SQL_SRC=sqlite3.c
set SQL_INC=
set SQL_LIB=
if not exist sqlite3.c (
  if "%SQLITE_DIR%"=="" (
    echo Falta sqlite3.c ^(amalgamacion^) o SQLITE_DIR ^(vcpkg, x64-windows-static^)
    exit /b 1
  )
  set SQL_SRC=
  set SQL_INC=/I"%SQLITE_DIR%\include"
  set SQL_LIB=/LIBPATH:"%SQLITE_DIR%\lib" sqlite3.lib
)
rc /nologo /fo app.res app.rc || exit /b 1
cl /nologo /O2 /MT /W4 /sdl /GS /guard:cf /DUNICODE /D_UNICODE %SQL_INC% ^
   corner_radius.c gfx.c panel.c notch.c install.c winnotif.c menubar.c dock.c pop.c tray.c %SQL_SRC% app.res ^
   /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup /MANIFEST:NO ^
   /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /CETCOMPAT /guard:cf /DEPENDENTLOADFLAG:0x800 ^
   shell32.lib advapi32.lib gdi32.lib user32.lib ole32.lib uuid.lib dwmapi.lib msimg32.lib winmm.lib wlanapi.lib iphlpapi.lib ^
   version.lib oleaut32.lib wbemuuid.lib bthprops.lib %SQL_LIB% /OUT:OpenDock.exe || exit /b 1
echo OK -^> OpenDock.exe
