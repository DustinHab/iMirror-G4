#!/bin/sh
# geraet bauen: Kopplung und Dienste des iPhones (USB ueber usbmuxd oder WLAN)
set -e
cd ~/tools/fernbedienung/iphone
SDK="-isysroot /Developer/SDKs/MacOSX10.5.sdk -mmacosx-version-min=10.5 -arch ppc"
# als geraet.neu bauen und umbenennen: eine laufende Steuerung wird so nicht ueberschrieben
gcc-4.2 $SDK -std=gnu99 -O2 -mcpu=7450 -Wall -Wno-deprecated-declarations -o geraet.neu geraet.c tunnel.c xpc.c \
    -framework CoreFoundation -lssl -lcrypto -lz
strip geraet.neu
mv geraet.neu geraet
ls -l geraet
