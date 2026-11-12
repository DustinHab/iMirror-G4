# -*- coding: utf-8 -*-
# Erzeugt die Info.plist für Trackpad.kext (Python 2.5 auf dem iBook).
# Trackpad-Werte wie Apples AppleADBMouseType4, Scroll-Werte wie Apples USB-Trackpad (Geyser),
# beides aus den installierten Treibern gelesen.
import plistlib, sys

ADB = '/System/Library/Extensions/AppleADBMouse.kext/Contents/Info.plist'
USB = '/System/Library/Extensions/AppleUSBTopCase.kext/Contents/PlugIns/AppleUSBTrackpad.kext/Contents/Info.plist'

adb = plistlib.readPlist(ADB)
usb = plistlib.readPlist(USB)
p = dict(adb['IOKitPersonalities']['AppleADBMouseType4'])
u = usb['IOKitPersonalities']['Geyser II ANSI']

p['CFBundleIdentifier'] = 'local.trackpad'
p['IOClass'] = 'Trackpad'
p['IOMatchCategory'] = 'Trackpad'      # eigene Kategorie: läuft neben Apples Treiber
p['IOProbeScore'] = 30000
for k in ('HIDScrollAccelerationType', 'HIDScrollResolution', 'HIDScrollAccelerationTable'):
    p[k] = u[k]
p['TrackpadScroll'] = True             # Trackpad kann scrollen (für die Systemeinstellung)
p['TrackpadHorizScroll'] = True
p['TrackpadRightClick'] = True         # zwei Finger + Klick = Rechtsklick (Schalter in der Systemeinstellung)

info = {
    'CFBundleDevelopmentRegion': 'German',
    'CFBundleExecutable': 'Trackpad',
    'CFBundleIdentifier': 'local.trackpad',
    'CFBundleInfoDictionaryVersion': '6.0',
    'CFBundleName': 'Trackpad',
    'CFBundlePackageType': 'KEXT',
    'CFBundleShortVersionString': '1.0.0',
    'CFBundleVersion': '1.0.0',
    'IOKitPersonalities': {'Trackpad': p},
    'OSBundleLibraries': {
        'com.apple.kpi.bsd': '8.0.0',
        'com.apple.kpi.iokit': '8.0.0',
        'com.apple.kpi.libkern': '8.0.0',
        'com.apple.kpi.mach': '8.0.0',
        'com.apple.iokit.IOADBFamily': '1.0.0b1',
        'com.apple.iokit.IOHIDFamily': '1.0.0b1',
    },
}
plistlib.writePlist(info, sys.argv[1])
print 'Info.plist: %d Eigenschaften in der Personality' % len(p)
