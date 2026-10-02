#!/usr/bin/env sh
# Compilar con mingw-w64: Linux/WSL (prefijo x86_64-w64-mingw32-) o MSYS2 en Windows.
# Flags de endurecimiento: ASLR de alta entropía, DEP, stack protector.
set -e
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then P=x86_64-w64-mingw32-; else P=; fi
if command -v ${P}windres >/dev/null 2>&1; then RC=${P}windres; else RC=windres; fi

$RC app.rc -O coff -o app.res
${P}gcc -O2 -s -static -municode -mwindows \
  -Wall -Wextra -Wno-missing-field-initializers \
  -fstack-protector-strong \
  -Wl,--dynamicbase,--nxcompat,--high-entropy-va \
  corner_radius.c gfx.c panel.c notch.c install.c winnotif.c menubar.c dock.c pop.c tray.c app.res -o CornerRadius.exe \
  -lshell32 -ladvapi32 -lgdi32 -luser32 -lole32 -luuid -ldwmapi -lsqlite3 -lmsimg32 -lwinmm -lwlanapi -lversion -loleaut32 -lwbemuuid -lbthprops
echo "OK -> CornerRadius.exe"
