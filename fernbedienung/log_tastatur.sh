#!/bin/sh
# Tastatur-Zeilen aus dem Protokoll; mit "voll" eine Loesch- und eine Einfuegenachricht ungekuerzt
f=~/tools/fernbedienung/protokoll.txt
if [ "$1" = voll ]; then
    grep 'Tastatur:.*deletionCount' $f | tail -1
    echo
    grep 'Tastatur:.*insertionText' $f | tail -1
    exit 0
fi
grep -E 'Textfeld|Tastatur|Fokus|_tiC|_tiStart|_tiStop' $f | tail -${1:-40} | cut -c1-500
