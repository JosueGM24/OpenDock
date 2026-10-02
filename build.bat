@echo off
rem Compilar en Windows con MSVC (Developer Command Prompt)
rem Requiere sqlite3.c/sqlite3.h (amalgamación de sqlite.org) junto al código.
rem /guard:cf Control Flow Guard · /CETCOMPAT shadow stack · /sdl /GS comprobaciones extra
rc /nologo /fo app.res app.rc || exit /b 1
cl /nologo /O2 /W4 /sdl /GS /guard:cf /DUNICODE /D_UNICODE ^
   corner_radius.c gfx.c panel.c notch.c install.c winnotif.c menubar.c dock.c pop.c tray.c sqlite3.c app.res ^
   /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup /MANIFEST:NO ^
   /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /CETCOMPAT /guard:cf ^
   shell32.lib advapi32.lib gdi32.lib user32.lib ole32.lib uuid.lib dwmapi.lib msimg32.lib winmm.lib wlanapi.lib version.lib oleaut32.lib wbemuuid.lib bthprops.lib /OUT:CornerRadius.exe
