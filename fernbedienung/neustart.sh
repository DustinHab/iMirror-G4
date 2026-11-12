#!/bin/sh
# Fernbedienungsdienst in der Aqua-Sitzung neu starten (KeepAlive startet ihn wieder)
export LAUNCHD_SOCKET=$(ls -d /tmp/launchd-*)/sock
launchctl stop local.fernbedienung
sleep 3
tail -3 ~/tools/fernbedienung/protokoll.txt