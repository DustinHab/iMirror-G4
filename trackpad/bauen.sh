#!/bin/bash
# Trackpad.kext bauen und pruefen (laedt NICHT). Braucht Xcode 3.1 (gcc-4.2) und sudo.
set -e
Q=$(cd "$(dirname "$0")" && pwd)
B=$Q/bau
sudo rm -rf "$B"; mkdir -p "$B"
H=/System/Library/Frameworks/Kernel.framework/Headers
CC_KEXT=$(ls /usr/lib/gcc/powerpc-apple-darwin9/4.2.1/libcc_kext.a 2>/dev/null || true)
FL="-arch ppc -nostdinc -fno-builtin -fno-common -msoft-float -O2 -mkernel -finline -fno-keep-inline-functions -force_cpusubtype_ALL -mlong-branch -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -I$H -I$H/bsd -Wall"
g++-4.2 -x c++ $FL -fapple-kext -fno-exceptions -fno-rtti -c "$Q/Trackpad.cpp" -o "$B/Trackpad.o"
gcc-4.2 -x c $FL -c "$Q/info.c" -o "$B/info.o"
g++-4.2 -arch ppc -static -nostdlib -r -o "$B/Trackpad" "$B/Trackpad.o" "$B/info.o" -lkmodc++ -lkmod $CC_KEXT
K=$B/Trackpad.kext
mkdir -p "$K/Contents/MacOS"
python "$Q/info_erzeugen.py" "$K/Contents/Info.plist"
cp "$B/Trackpad" "$K/Contents/MacOS/"
nm -u "$K/Contents/MacOS/Trackpad" | grep -v "^__Z" ; echo "C++-Symbole offen: $(nm -u "$K/Contents/MacOS/Trackpad" | grep -c "^__Z")"
sudo chown -R root:wheel "$K"
sudo chmod -R go-w "$K"
echo "== Pruefung ohne Laden"
sudo kextload -t -n -v "$K" 2>&1 | tail -15
