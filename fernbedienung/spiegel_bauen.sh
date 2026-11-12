#!/bin/bash
# Spiegel.app bauen: Anzeige der Bildschirmsynchronisierung mit libvlc aus PowerVLC
set -e
cd ~/tools/fernbedienung
VLC=/Applications/PowerVLC.app/Contents/MacOS
APP=Spiegel.app
SDK="-isysroot /Developer/SDKs/MacOSX10.5.sdk -mmacosx-version-min=10.5 -arch ppc"
# Header aus PowerVLC; doppelte typedef (C11) fuer gcc 4.2 entfernen
if [ ! -d vlcinclude ]; then
    cp -R "$VLC/include" vlcinclude
    sed -i '' '/^typedef struct libvlc_renderer_item_t libvlc_renderer_item_t;/d' vlcinclude/vlc/libvlc_events.h
fi
mkdir -p $APP/Contents/MacOS
# libvlccore findet sein Verzeichnis ueber ein geladenes Image, dessen Name auf "VLC" endet
if [ ! -f $APP/Contents/MacOS/VLC ]; then
    echo 'int spiegel_vlc_marke;' > /tmp/spiegel_marke.c
    gcc-4.2 -arch ppc -dynamiclib $SDK -install_name @executable_path/VLC -o $APP/Contents/MacOS/VLC /tmp/spiegel_marke.c
    rm -f /tmp/spiegel_marke.c
fi
# UYVY-Umsetzung mit AltiVec
gcc-4.2 $SDK -std=gnu99 -O3 -mcpu=7450 -faltivec -Wall -c -o uyvy.o uyvy.c
# AltiVec-Glaettungsfilter, die der Dekoder gegen FFmpegs C-Fassungen tauscht
gcc-4.2 $SDK -std=gnu99 -O3 -mcpu=7450 -faltivec -Wall -c -o filter.o filter.c
# H.264: FFmpeg aus PowerVLCs avcodec-Modul (gemessen 13-17 % schneller als das eigene FFmpeg 4.4.5 mit gcc 4.2 -O3,
# Bilder bitgleich); "spiegel_bauen.sh eigen" bindet stattdessen das eigene aus ~/tools/ffmpeg-g4 ein
FF=$HOME/tools/ffmpeg-g4/aus
DEK=""
FFL=""
if [ -f $FF/lib/libavcodec.a ] && [ "$1" = eigen ]; then
    DEK="-DEIGENES_FFMPEG -I$FF/include"
    FFL="$FF/lib/libavcodec.a $FF/lib/libavutil.a"
fi
gcc-4.2 $SDK -std=gnu99 -O2 -mcpu=7450 -Wall $DEK -c -o dekoder.o dekoder.c
# erst daneben bauen, dann umbenennen: ein laufendes Fenster behaelt so seine alte Datei
gcc-4.2 $SDK -O2 -mcpu=7450 -Wall -Ivlcinclude -L"$VLC/lib" -lpowervlc.5 -lpowervlccore.9 $APP/Contents/MacOS/VLC \
    -framework Cocoa -framework Carbon -framework OpenGL -o $APP/Contents/MacOS/Spiegel.neu spiegel.m uyvy.o dekoder.o filter.o $FFL
strip $APP/Contents/MacOS/Spiegel.neu
mv -f $APP/Contents/MacOS/Spiegel.neu $APP/Contents/MacOS/Spiegel
for l in lib plugins share; do [ -e $APP/Contents/MacOS/$l ] || ln -s "$VLC/$l" $APP/Contents/MacOS/$l; done
cp Spiegel-Info.plist $APP/Contents/Info.plist
