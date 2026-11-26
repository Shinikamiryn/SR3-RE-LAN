#!/bin/sh
# Build the LAN Mod: the mod loader (dinput8.dll), the installer, and the release folder/zip.
#
# Needs a 64-bit Windows C compiler (MinGW-w64 or LLVM-MinGW). Set CC to override, e.g.
#   CC=x86_64-w64-mingw32-gcc sh build.sh
# Output: dist/LAN_Mod/ and dist/LAN_Mod.zip (if 'zip' or PowerShell is available)
set -e
cd "$(dirname "$0")"
if [ -z "$CC" ]; then
    CC=$(ls -d "$LOCALAPPDATA"/Microsoft/WinGet/Packages/MartinStorsjo.LLVM-MinGW.UCRT*/llvm-mingw*/bin 2>/dev/null | head -1)/x86_64-w64-mingw32-clang
    [ -x "$CC" ] || [ -x "$CC.exe" ] || CC=x86_64-w64-mingw32-gcc
fi
OUT=dist/LAN_Mod
rm -rf dist && mkdir -p $OUT/files

"$CC" -O2 -Wall -Wextra -Wno-unused-parameter -shared -static -s \
    -o $OUT/files/dinput8.dll loader/src/dinput8_proxy.c loader/src/dinput8.def \
    -Wl,--enable-stdcall-fixup -lkernel32
"$CC" -O2 -Wall -Wextra -static -s -o $OUT/LANMod_Installer.exe installer/lanmod_installer.c -ladvapi32

cp patches/lan_join.patch $OUT/files/
cp release/README.txt $OUT/README.txt
cp LICENSE $OUT/LICENSE.txt
printf '@echo off\r\n"%%~dp0LANMod_Installer.exe" uninstall\r\n' > "$OUT/Uninstall LAN Mod.bat"

# The files go at the top level of the zip (no LAN_Mod folder inside it): Windows "Extract All"
# already creates a LAN_Mod folder named after the zip.
if command -v zip >/dev/null 2>&1; then
    (cd $OUT && zip -qr ../LAN_Mod.zip .)
elif command -v powershell >/dev/null 2>&1; then
    powershell -NoProfile -Command "Compress-Archive -Path dist/LAN_Mod/* -DestinationPath dist/LAN_Mod.zip -Force"
fi
echo "built $OUT"
ls -la $OUT $OUT/files
