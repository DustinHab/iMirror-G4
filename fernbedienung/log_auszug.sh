#!/bin/sh
# Auszug aus dem Protokoll: Modus, Tasten, Klicks, Fehler (ohne die vielen Wischzeilen)
f=~/tools/fernbedienung/protokoll.txt
grep -v '_hidT' $f | tail -${1:-30} | cut -c1-160
echo "--- Zeiger laeuft: $(ps -axo comm | grep -c 'MacOS/Zeiger')"
