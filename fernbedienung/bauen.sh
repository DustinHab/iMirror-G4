#!/bin/bash
# Fernbedienung bauen: iPhone-Fernbedienung (Kontrollzentrum) fuer das iBook, laeuft als LaunchAgent.
# Quellen in ~/tools/fernbedienung; Programm bleibt dort, Startdatei ~/Library/LaunchAgents/local.fernbedienung.plist
set -e
cd ~/tools/fernbedienung
SDK="-isysroot /Developer/SDKs/MacOSX10.5.sdk -mmacosx-version-min=10.5 -arch ppc"
KRYPTO="kopplung.c opack.c monocypher.c monocypher-ed25519.c"
if [ "$1" = pruefen ]; then
    gcc-4.2 $SDK -std=c99 -O2 -mcpu=7450 -Wall -o pruefen pruefen.c $KRYPTO -lcrypto
    ./pruefen
    exit $?
fi
gcc-4.2 $SDK -std=c99 -Os -mcpu=7450 -Wall -fobjc-exceptions -c -o rti.o rti.m
if [ "$1" = rti ]; then
    gcc-4.2 $SDK -std=c99 -O2 -Wall -o rti_pruefen rti_pruefen.c rti.o -framework Foundation
    ./rti_pruefen
    exit $?
fi
# FairPlay (playfair aus UxPlay, GPL-3, Big-Endian-fest gemacht): fremder Code, ohne Warnungen
PF=""
for f in playfair hand_garble modified_md5 omg_hax sap_hash; do
    [ playfair/$f.o -nt playfair/$f.c ] || gcc-4.2 $SDK -std=c99 -O2 -mcpu=7450 -w -c -o playfair/$f.o playfair/$f.c
    PF="$PF playfair/$f.o"
done
gcc-4.2 $SDK -std=c99 -Os -mcpu=7450 -Wall -o fernbedienung.neu \
    fernbedienung.c eingabe.c airplay.c rti.o $KRYPTO $PF \
    -framework ApplicationServices -framework CoreAudio -framework IOKit -framework CoreFoundation \
    -framework SystemConfiguration -framework AudioToolbox -framework Foundation -framework AppKit -lcrypto
strip fernbedienung.neu
mv fernbedienung.neu fernbedienung
# Spiegel.app: zeigt die Bildschirmsynchronisierung (libvlc aus PowerVLC)
sh spiegel_bauen.sh
# Zeiger.app: runder Zeiger im Mausmodus, ohne Dock-Symbol
mkdir -p Zeiger.app/Contents/MacOS
cp Zeiger-Info.plist Zeiger.app/Contents/Info.plist
gcc-4.2 $SDK -Os -mcpu=7450 -Wall -o Zeiger.app/Contents/MacOS/Zeiger zeiger.m -framework Cocoa
strip Zeiger.app/Contents/MacOS/Zeiger
ls -l fernbedienung Zeiger.app/Contents/MacOS/Zeiger Spiegel.app/Contents/MacOS/Spiegel
