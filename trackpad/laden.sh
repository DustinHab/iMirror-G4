#!/bin/bash
# Trackpad.kext nach /Library/Application Support/iMirror-G4 kopieren und laden (Apples Treiber bleibt geladen).
# Zurueck zu Apple: sudo kextunload -b local.trackpad
# Optional: laden.sh N schreibt die naechsten N Pakete ins Systemprotokoll.
Q=$(cd "$(dirname "$0")" && pwd)
Z="/Library/Application Support/iMirror-G4"
kextstat | grep -q local.trackpad && { sudo kextunload -b local.trackpad || exit 1; }
sudo mkdir -p "$Z"
sudo rm -rf "$Z/Trackpad.kext"
sudo cp -R "$Q/bau/Trackpad.kext" "$Z/"
sudo chown -R root:wheel "$Z/Trackpad.kext"; sudo chmod -R go-w "$Z/Trackpad.kext"
sudo kextload "$Z/Trackpad.kext" || exit 1
sleep 3
sudo sysctl -w debug.trackpad.protokoll=${1:-0} >/dev/null
echo "== Zustand"; sysctl -n debug.trackpad.zustand
echo "== Treiber am Trackpad"
ioreg -w0 -r -c IOADBDevice -d 2 | grep -E '<class (IOADBDevice|AppleADBMouseType4|Trackpad)' | sed 's/, id.*//'
grep -E 'Trackpad:' /var/log/system.log | tail -3
