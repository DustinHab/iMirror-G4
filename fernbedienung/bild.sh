#!/bin/sh
# Bildschirmfoto der Sitzung (mit Zeiger) nach /tmp/bild.png
P=$(ps -axo pid,comm | awk '/loginwindow/{print $1; exit}')
sudo launchctl bsexec $P screencapture -x -C -t png /tmp/bild.png
