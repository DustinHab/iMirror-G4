/* Eingaben ins vordere Programm: Tasten, Text, Systemlautstaerke, Bildschirm aus.
   Teil von fernbedienung (iPhone-Fernbedienung fuer das iBook). */
#include <ApplicationServices/ApplicationServices.h>
#include <CoreAudio/CoreAudio.h>
#include <IOKit/IOKitLib.h>
#include <AudioToolbox/AudioToolbox.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "eingabe.h"

/* virtuelle Tastencodes (US-Belegung, auf der deutschen gleich) */
enum { T_RETURN = 36, T_TAB = 48, T_SPACE = 49, T_ESC = 53, T_CMD = 55,
       T_LINKS = 123, T_RECHTS = 124, T_RUNTER = 125, T_HOCH = 126 };

static void taste_mit(CGKeyCode k, CGEventFlags flags)
{
    CGEventRef d = CGEventCreateKeyboardEvent(NULL, k, true), u = CGEventCreateKeyboardEvent(NULL, k, false);
    if (flags) { CGEventSetFlags(d, flags); CGEventSetFlags(u, flags); }
    CGEventPost(kCGHIDEventTap, d);
    usleep(20000);
    CGEventPost(kCGHIDEventTap, u);
    CFRelease(d); CFRelease(u);
}

void eingabe_taste(int art)
{
    switch (art) {
        case E_HOCH:    taste_mit(T_HOCH, 0); break;
        case E_RUNTER:  taste_mit(T_RUNTER, 0); break;
        case E_LINKS:   taste_mit(T_LINKS, 0); break;
        case E_RECHTS:  taste_mit(T_RECHTS, 0); break;
        case E_WAHL:    taste_mit(T_RETURN, 0); break;
        case E_ZURUECK: taste_mit(T_ESC, 0); break;
        case E_PAUSE:   taste_mit(T_SPACE, 0); break;
        case E_WECHSEL: {
            /* Befehlstaste halten, Tab, loslassen: zum zuletzt benutzten Programm */
            CGEventRef cd = CGEventCreateKeyboardEvent(NULL, T_CMD, true), cu = CGEventCreateKeyboardEvent(NULL, T_CMD, false);
            CGEventSetFlags(cd, kCGEventFlagMaskCommand);
            CGEventPost(kCGHIDEventTap, cd);
            usleep(20000);
            taste_mit(T_TAB, kCGEventFlagMaskCommand);
            usleep(20000);
            CGEventSetFlags(cu, 0);
            CGEventPost(kCGHIDEventTap, cu);
            CFRelease(cd); CFRelease(cu);
            break;
        }
    }
}

/* Text als Unicode-Tastendruecke; "\n" wird zu Return */
void eingabe_text(const UniChar *text, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (text[i] == '\n' || text[i] == '\r') { taste_mit(T_RETURN, 0); continue; }
        CGEventRef d = CGEventCreateKeyboardEvent(NULL, 0, true), u = CGEventCreateKeyboardEvent(NULL, 0, false);
        CGEventKeyboardSetUnicodeString(d, 1, text + i);
        CGEventKeyboardSetUnicodeString(u, 1, text + i);
        CGEventPost(kCGHIDEventTap, d);
        CGEventPost(kCGHIDEventTap, u);
        CFRelease(d); CFRelease(u);
        usleep(8000);
    }
}

/* Loeschen (Rueckschritt) n-mal */
void eingabe_loeschen(int n)
{
    while (n-- > 0) taste_mit(51, 0);
}

/* Text in UTF-8 tippen */
void eingabe_text_utf8(const char *s)
{
    CFStringRef t = CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
    CFIndex n;
    UniChar *b;
    if (!t) return;
    n = CFStringGetLength(t);
    b = malloc(sizeof(UniChar) * (n + 1));
    CFStringGetCharacters(t, CFRangeMake(0, n), b);
    eingabe_text(b, (int)n);
    free(b);
    CFRelease(t);
}

/* Befehl-A: alles im Feld markieren */
void eingabe_alles_markieren(void)
{
    taste_mit(0, kCGEventFlagMaskCommand);
}

/* --- aktives Textfeld ueber die Bedienungshilfen ----------------------- */
extern signed char NSApplicationLoad(void);   /* AppKit */
int eingabe_ax_fehler;          /* letzter Fehler der Bedienungshilfen, fuers Protokoll */
char eingabe_ax_rolle[64];      /* Rolle des fokussierten Elements */
static void teil(CFStringRef s, CFIndex von, CFIndex bis, char *o, int max)
{
    CFStringRef t;
    o[0] = 0;
    if (!s || bis <= von) return;
    t = CFStringCreateWithSubstring(NULL, s, CFRangeMake(von, bis - von));
    if (t) { CFStringGetCString(t, o, max, kCFStringEncodingUTF8); CFRelease(t); }
}

static void attr_text(AXUIElementRef el, CFStringRef a, char *o, int max)
{
    CFTypeRef w = NULL;
    if (!o[0] && AXUIElementCopyAttributeValue(el, a, &w) == kAXErrorSuccess && w) {
        if (CFGetTypeID(w) == CFStringGetTypeID()) CFStringGetCString(w, o, max, kCFStringEncodingUTF8);
        CFRelease(w);
    }
}

/* 1: Textfeld hat den Fokus. *el bekommt das Element (vom Aufrufer freizugeben),
   dazu Titel und Text vor, in und nach der Auswahl. */
int eingabe_textfeld(AXUIElementRef *el_aus, char *titel, int tmax, char *vor, int vmax, char *ausw, int amax, char *nach, int nmax)
{
    static AXUIElementRef sys;
    AXUIElementRef el = NULL, app = NULL;
    CFTypeRef rolle = NULL, wert = NULL, ber = NULL;
    int ja = 0;
    titel[0] = vor[0] = ausw[0] = nach[0] = 0;
    *el_aus = NULL;
    eingabe_ax_rolle[0] = 0;
    if (!AXAPIEnabled()) { eingabe_ax_fehler = 1; return 0; }
    /* Ohne AppKit-Anbindung antworten unter 10.5 keine Programme (Fehler -25204) */
    if (!sys) { NSApplicationLoad(); sys = AXUIElementCreateSystemWide(); AXUIElementSetMessagingTimeout(sys, 0.4f); }
    eingabe_ax_fehler = AXUIElementCopyAttributeValue(sys, kAXFocusedUIElementAttribute, (CFTypeRef *)&el);
    if (eingabe_ax_fehler != kAXErrorSuccess || !el) return 0;
    AXUIElementSetMessagingTimeout(el, 0.4f);
    if (AXUIElementCopyAttributeValue(el, kAXRoleAttribute, &rolle) == kAXErrorSuccess && rolle) {
        CFStringGetCString(rolle, eingabe_ax_rolle, sizeof eingabe_ax_rolle, kCFStringEncodingUTF8);
        if (CFEqual(rolle, kAXTextFieldRole) || CFEqual(rolle, kAXTextAreaRole) || CFEqual(rolle, kAXComboBoxRole)) ja = 1;
        CFRelease(rolle);
    }
    if (!ja) { CFRelease(el); return 0; }
    if (AXUIElementCopyAttributeValue(el, kAXValueAttribute, &wert) == kAXErrorSuccess && wert && CFGetTypeID(wert) == CFStringGetTypeID()) {
        CFIndex len = CFStringGetLength(wert);
        CFRange r = CFRangeMake(len, 0);
        if (AXUIElementCopyAttributeValue(el, kAXSelectedTextRangeAttribute, &ber) == kAXErrorSuccess && ber) {
            AXValueGetValue(ber, kAXValueCFRangeType, &r);
            CFRelease(ber);
        }
        if (r.location < 0 || r.location > len) r.location = len;
        if (r.length < 0 || r.location + r.length > len) r.length = len - r.location;
        teil(wert, 0, r.location, vor, vmax);
        teil(wert, r.location, r.location + r.length, ausw, amax);
        teil(wert, r.location + r.length, len, nach, nmax);
    }
    if (wert) CFRelease(wert);
    attr_text(el, kAXTitleAttribute, titel, tmax);
    attr_text(el, kAXDescriptionAttribute, titel, tmax);
    if (!titel[0] && AXUIElementCopyAttributeValue(sys, kAXFocusedApplicationAttribute, (CFTypeRef *)&app) == kAXErrorSuccess && app) {
        attr_text(app, kAXTitleAttribute, titel, tmax);
        CFRelease(app);
    }
    *el_aus = el;
    return 1;
}

/* --- Systemlautstaerke ueber CoreAudio (Hauptkanal, sonst Kanal 1 und 2) --- */
static AudioDeviceID ausgabe(void)
{
    AudioDeviceID d = 0;
    UInt32 n = sizeof(d);
    AudioHardwareGetProperty(kAudioHardwarePropertyDefaultOutputDevice, &n, &d);
    return d;
}

float eingabe_lautstaerke(void)
{
    AudioDeviceID d = ausgabe();
    Float32 v = 0;
    UInt32 n = sizeof(v);
    if (AudioDeviceGetProperty(d, 0, false, kAudioDevicePropertyVolumeScalar, &n, &v) == noErr) return v;
    n = sizeof(v);
    if (AudioDeviceGetProperty(d, 1, false, kAudioDevicePropertyVolumeScalar, &n, &v) == noErr) return v;
    return 0.5f;
}

void eingabe_setze_lautstaerke(float v)
{
    AudioDeviceID d = ausgabe();
    Float32 w = v < 0 ? 0 : v > 1 ? 1 : v;
    UInt32 aus = 0;
    if (AudioDeviceSetProperty(d, NULL, 0, false, kAudioDevicePropertyVolumeScalar, sizeof(w), &w) != noErr) {
        AudioDeviceSetProperty(d, NULL, 1, false, kAudioDevicePropertyVolumeScalar, sizeof(w), &w);
        AudioDeviceSetProperty(d, NULL, 2, false, kAudioDevicePropertyVolumeScalar, sizeof(w), &w);
    }
    if (w > 0) AudioDeviceSetProperty(d, NULL, 0, false, kAudioDevicePropertyMute, sizeof(aus), &aus);
}

void eingabe_lauter(int richtung)
{
    /* in Sechzehnteln wie die Lautstaerketasten */
    float v = eingabe_lautstaerke() + richtung / 16.0f;
    eingabe_setze_lautstaerke(v);
}

void eingabe_stumm(int an)
{
    AudioDeviceID d = ausgabe();
    UInt32 m = an ? 1 : 0;
    if (AudioDeviceSetProperty(d, NULL, 0, false, kAudioDevicePropertyMute, sizeof(m), &m) != noErr) {
        AudioDeviceSetProperty(d, NULL, 1, false, kAudioDevicePropertyMute, sizeof(m), &m);
        AudioDeviceSetProperty(d, NULL, 2, false, kAudioDevicePropertyMute, sizeof(m), &m);
    }
}

int eingabe_ist_stumm(void)
{
    AudioDeviceID d = ausgabe();
    UInt32 m = 0, n = sizeof(m);
    if (AudioDeviceGetProperty(d, 0, false, kAudioDevicePropertyMute, &n, &m) != noErr) {
        n = sizeof(m);
        AudioDeviceGetProperty(d, 1, false, kAudioDevicePropertyMute, &n, &m);
    }
    return m != 0;
}

/* --- Mauszeiger ---------------------------------------------------- */
static CGRect bildflaeche(void)
{
    CGDirectDisplayID d[8];
    CGDisplayCount n = 0, i;
    CGRect r = CGRectNull;
    if (CGGetActiveDisplayList(8, d, &n) != kCGErrorSuccess || n == 0) return CGDisplayBounds(CGMainDisplayID());
    for (i = 0; i < n; i++) r = CGRectUnion(r, CGDisplayBounds(d[i]));
    return r;
}

void eingabe_zeiger_lage(double *x, double *y)
{
    CGEventRef e = CGEventCreate(NULL);
    CGPoint p = { 0, 0 };
    if (e) { p = CGEventGetLocation(e); CFRelease(e); }
    *x = p.x;
    *y = p.y;
}

/* Zeiger setzen, auf den Bildschirm begrenzt; die begrenzte Lage kommt zurueck */
void eingabe_zeiger(double *x, double *y)
{
    CGRect r = bildflaeche();
    CGPoint p;
    CGEventRef e;
    if (*x < r.origin.x) *x = r.origin.x;
    if (*y < r.origin.y) *y = r.origin.y;
    if (*x > r.origin.x + r.size.width - 1) *x = r.origin.x + r.size.width - 1;
    if (*y > r.origin.y + r.size.height - 1) *y = r.origin.y + r.size.height - 1;
    p.x = *x;
    p.y = *y;
    e = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, p, kCGMouseButtonLeft);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
}

/* Klick an der Zeigerstelle; schnell hintereinander an gleicher Stelle zaehlt als Doppelklick */
void eingabe_klick(int rechts)
{
    static CFAbsoluteTime letzt;
    static CGPoint lp;
    static int zahl;
    CFAbsoluteTime t = CFAbsoluteTimeGetCurrent();
    CGPoint p;
    CGEventRef d, u;
    double x, y;
    eingabe_zeiger_lage(&x, &y);
    p.x = x;
    p.y = y;
    if (!rechts && t - letzt < 0.5 && fabs(p.x - lp.x) < 5 && fabs(p.y - lp.y) < 5) zahl++;
    else zahl = 1;
    letzt = t;
    lp = p;
    d = CGEventCreateMouseEvent(NULL, rechts ? kCGEventRightMouseDown : kCGEventLeftMouseDown, p, rechts ? kCGMouseButtonRight : kCGMouseButtonLeft);
    u = CGEventCreateMouseEvent(NULL, rechts ? kCGEventRightMouseUp : kCGEventLeftMouseUp, p, rechts ? kCGMouseButtonRight : kCGMouseButtonLeft);
    CGEventSetIntegerValueField(d, kCGMouseEventClickState, zahl);
    CGEventSetIntegerValueField(u, kCGMouseEventClickState, zahl);
    CGEventPost(kCGHIDEventTap, d);
    usleep(15000);
    CGEventPost(kCGHIDEventTap, u);
    CFRelease(d);
    CFRelease(u);
}

/* Rollen in Pixeln, positiv nach oben; gleitend wie auf einem Trackpad */
void eingabe_rollen(int pixel)
{
    CGEventRef e = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitPixel, 1, pixel);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
}

/* kurzer Systemton: 1 Tink, 0 Pop */
void eingabe_ton(int welcher)
{
    static SystemSoundID s[2];
    static int geladen[2];
    static const char *pfad[2] = { "/System/Library/Sounds/Pop.aiff", "/System/Library/Sounds/Tink.aiff" };
    welcher = welcher ? 1 : 0;
    if (!geladen[welcher]) {
        CFURLRef u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)pfad[welcher], strlen(pfad[welcher]), false);
        if (u && AudioServicesCreateSystemSoundID(u, &s[welcher]) == noErr) geladen[welcher] = 1;
        if (u) CFRelease(u);
    }
    if (geladen[welcher]) AudioServicesPlaySystemSound(s[welcher]);
}

/* --- Bildschirm aus und an (wie die Ruhe des Bildschirms nach Zeit) --- */
void eingabe_bildschirm(int an)
{
    io_registry_entry_t r = IORegistryEntryFromPath(kIOMasterPortDefault, "IOService:/IOResources/IODisplayWrangler");
    if (r) {
        IORegistryEntrySetCFProperty(r, CFSTR("IORequestIdle"), an ? kCFBooleanFalse : kCFBooleanTrue);
        IOObjectRelease(r);
    }
}
