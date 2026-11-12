/*
 * Copyright (c) 1998-2003 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 *
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * Trackpad – Treiber für das ADB-Trackpad des iBook G4 (PowerBook6,5) mit Zwei-Finger-Scrollen.
 *
 * Geänderte Fassung von Apples AppleADBMouse-212 (Klasse AppleADBMouseType4, Version 2.1.0f3),
 * Änderungen vom 10.11.2026:
 *  - nur das W-Enhanced-Trackpad (Signatur 'tpad', 4 Tasten gemeldet), alles in einer Klasse.
 *  - Läuft NEBEN Apples Treiber (eigene IOMatchCategory). Apples Treiber bleibt geladen; dieser Treiber
 *    übernimmt nur den Paketempfang (owner/handler im ADBDeviceControl des ADB-Busses) und gibt ihn beim
 *    Entladen an Apples Treiber zurück. Ein Wächter prüft alle 2 s, ob der Empfang noch hier ist
 *    (nach dem Aufwachen starten beide Treiber neu, in beliebiger Reihenfolge).
 *  - Zwei Finger: das Trackpad meldet sie selbst (Byte 3, Bit 7 = 0; Bit 3 = 1 heißt alle Finger weg),
 *    wie in Apples packetW(). Statt die Bewegung zu verwerfen, entstehen daraus Scrollereignisse, wenn in
 *    den Trackpad-Einstellungen Scrollen erlaubt ist (Parameter TrackpadScroll / TrackpadHorizScroll).
 *    Beschleunigung und pixelgenaues Scrollen übernimmt IOHIPointing (HIDTrackpadScrollAcceleration,
 *    Werte wie Apples USB-Trackpads, siehe Info.plist).
 *  - packet() ist wie bei Apple die erste neue virtuelle Methode nach IOHIPointing (gleiche Stelle in der
 *    Methodentabelle), damit ein Paket, das beim Umschalten an den jeweils anderen Treiber geht, trotzdem
 *    richtig verarbeitet wird.
 *  - Rechtsklick: zwei Finger + Klick (Schalter "Sekundärklick" der Systemeinstellung, TrackpadRightClick).
 *  - Nachlaufen: nach schnellem Wischen scrollt es abklingend weiter (Ganzzahl-Rechnung, Takt 16 ms).
 *  - sysctl debug.trackpad: natuerlich (1 = Inhalt folgt den Fingern), schwung (Nachlaufen), beruehrt (Finger auf),
 *    protokoll (n Pakete loggen), zustand.
 *  - Positionsmodus (nur auf Wunsch eines Programms, z. B. Theremin): Register 1 Byte 6 = 0x00 statt 0x0D, das
 *    Trackpad meldet dann die Fingerposition X/Y und den Druck Z (Paketaufbau wie im Linux-Patch adbsyn 0.3).
 *    Das Programm schreibt alle 0,25 s debug.trackpad.position_an=1; bleibt das 1 s aus (Ende, Absturz,
 *    Programmwechsel) oder kommt 0, schaltet der Wächter zurück auf 0x0D. Solange der Modus an ist, bewegt das
 *    Trackpad den Zeiger nicht; Werte in debug.trackpad.position (x, y, z, Zähler, Modus).
 */

#include <IOKit/adb/IOADBDevice.h>
#include <IOKit/adb/IOADBBus.h>
#include <IOKit/hidsystem/IOHIPointing.h>
#include <IOKit/hidsystem/IOHIDTypes.h>
#include <IOKit/hidsystem/IOHIDParameter.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/OSAtomic.h>
extern "C" {
#include <sys/sysctl.h>
#include <sys/errno.h>
#include <string.h>
}

#define WAECHTER_MS 250                 /* Empfang und Positionsmodus prüfen */
#define MODUS_NORMAL 0x0D               /* Register 1 Byte 6: W Enhanced (wie Apple) */
#define MODUS_POSITION 0x00             /* Register 1 Byte 6: Fingerposition (adbsyn) */

static inline int my_abs(int x) { return x < 0 ? -x : x; }
static inline void sperre(void) { __asm__ volatile("sync" ::: "memory"); }

enum {
    kIgnoreTrackpadState_USBMouse           = 0x1,
    kIgnoreTrackpadState_MouseKeys          = 0x2,
    kIgnoreTrackpadState_MouseKeys_State_On = 0x4
};

class Trackpad : public IOHIPointing
{
    OSDeclareDefaultStructors(Trackpad);

public:
    /* erste neue virtuelle Methode – gleiche Stelle wie AppleADBMouse::packet */
    virtual void packet(UInt8 adbCommand, IOByteCount length, UInt8 *data);

    virtual IOService *probe(IOService *provider, SInt32 *score);
    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);
    virtual void free(void);
    virtual UInt32 interfaceID(void);
    virtual UInt32 deviceType(void);
    virtual IOFixed resolution(void);
    virtual IOItemCount buttonCount(void);
    virtual OSData *copyAccelerationTable(void);
    virtual IOReturn setParamProperties(OSDictionary *dict);
    void _check_usb_mouse(IOService *service, bool added);
    void uebernehmen(void);
    void zurueckgeben(void);
    void waechter(void);
    void schwungTakt(void);
    bool modusSetzen(UInt8 modus);
    void positionPruefen(void);

private:
    IOADBDevice *adbDevice;
    ADBDeviceControl *_ctrl;
    IOService *_vorher;                 /* Apples Treiber, solange dieser hier die Pakete bekommt */
    IOWorkLoop *_wl;
    IOTimerEventSource *_uhr;
    IOFixed _resolution;

    /* Rechtsklick: zwei Finger + Klick (Schalter TrackpadRightClick der Systemeinstellung) */
    bool _rechtsklickAn, _rechts;
    /* Nachlaufen: Geschwindigkeit je 16 ms in 1/256 Zählern, gleitend gemittelt */
    IOTimerEventSource *_schwungUhr;
    volatile int _schwungZustand;
    SInt32 _vV, _vH, _restV, _restH, _schwungTakte;
    uint64_t _letzterScroll;

    bool Clicking, Dragging, DragLock;
    bool _jitterclick, _jittermove;
    bool _palmnomove, _palmnoaction, _2fingernoaction, _sticky2finger, _zonenomove, _zonenoaction,
         _zonepecknomove, _ignorezone;
    bool _scrollAn, _waagerechtAn;
    UInt8 _WThreshold;
    UInt16 _jitterdelta;
    UInt32 _ignoreTrackpadState, _oldButtonState;
    UInt64 _jitterclicktime64, _timeoutSticky64;
    uint64_t _jitterclicktimeAB, _zonepeckingtimeAB, _timeoutStickyAB, _sticky2fingerTimeAB,   /* Mach-Zeit */
             _fake5minAB, _keyboardTimeAB;
    IOService *_pADBKeyboard;
    IONotifier *_notifierA, *_notifierT;
    const OSSymbol *_gettime;
    IOLock *_mouseLock;
    OSSet *_externalMice;

    UInt32 _herzschlagAlt;              /* Positionsmodus: letzter gesehener Herzschlag, Takte ohne */
    int _ohneHerzschlag, _modusFehler;
    int _normalOffen;                   /* Rückkehr in den Normalmodus noch nicht bestätigt: so viele Versuche übrig */

    UInt32 deviceSignature;
    UInt16 deviceResolution;
    UInt8 deviceClass, deviceNumButtons;

    void erweitertEinschalten(void);
    void eigenschaftenLesen(void);
    void scrollen(int dx, int dy, uint64_t now);
};

/* Die KPI von 10.5 rechnet Zeiten als uint64_t, IOHIPointing will AbsoluteTime */
static inline AbsoluteTime alsZeit(uint64_t u)
{
    union { uint64_t u; AbsoluteTime a; } z;
    z.u = u;
    return z.a;
}

/* ---------- gemeinsame Werte (es gibt genau ein Trackpad) ---------- */
static Trackpad *gAktiv = 0;                       /* Instanz, die gerade die Pakete bekommt */
static ADB_callback_func gVorherHandler = 0;       /* Apples Paketfunktion */
static int gNatuerlich = 1;                        /* 1: Inhalt folgt den Fingern, 0: wie ein Mausrad */
static int gProtokoll = 0;                         /* > 0: so viele Pakete ins Systemprotokoll */
static UInt32 gPakete = 0, gScrollPakete = 0, gUebernahmen = 0, gRechtsklicks = 0, gSchwuenge = 0;
static int gScrollAn = -1, gWaagerechtAn = -1, gRechtsklickAn = -1;   /* nur für den Zustand */
static int gSchwung = 1;                           /* 1: nach dem Wischen nachlaufen lassen */
static int gBeruehrt = 0;                          /* 1: Finger liegt auf (bis zum Paket "alle Finger weg") */
/* Positionsmodus */
static volatile int gAbsolut = 0;                  /* 0 normal, 1 Position, 2 schaltet gerade um (Pakete verwerfen) */
static volatile int gWunsch = 0;                   /* Programm will den Positionsmodus */
static volatile UInt32 gHerzschlag = 0;            /* zählt jeden Wunsch des Programms */
static volatile int gPosX = 0, gPosY = 0, gPosZ = 0;
static volatile UInt32 gPosZaehler = 0, gEinschaltungen = 0, gModusNeu = 0, gNachgeholt = 0;
static int gMinX, gMaxX, gMinY, gMaxY, gMaxZ;    /* gesehener Bereich seit dem Einschalten (Finger auf) */

#define SCHWUNG_MS      16                         /* Takt des Nachlaufens */
#define SCHWUNG_START   (3 * 256)                  /* ab 3 Zählern je 16 ms beim Loslassen */
#define SCHWUNG_ENDE    40                         /* unter ~0,16 Zählern je 16 ms aufhören */
#define SCHWUNG_MAX_T   250                        /* höchstens 4 s */

/* Paketfunktion für den ADB-Bus. Ruft packet() virtuell auf: Ist owner beim Umschalten kurz noch
   Apples Treiber, landet das Paket dort (gleiche Stelle in der Methodentabelle). */
static void Paket(IOService *ziel, UInt8 cmd, IOByteCount laenge, UInt8 *daten)
{
    if (ziel != (IOService *)gAktiv && gVorherHandler) { gVorherHandler(ziel, cmd, laenge, daten); return; }
    ((Trackpad *)ziel)->packet(cmd, laenge, daten);
}

/* schluckt Pakete, während das Gerät abgebaut wird (Ruhezustand) */
static void Leer(IOService *, UInt8, IOByteCount, UInt8 *) { }

static bool add_usb_mouse(OSObject *us, void *, IOService *dev)
{
    if (us) ((Trackpad *)us)->_check_usb_mouse(dev, true);
    return true;
}
static bool remove_usb_mouse(OSObject *us, void *, IOService *dev)
{
    if (us) ((Trackpad *)us)->_check_usb_mouse(dev, false);
    return true;
}
static void Uhr(OSObject *o, IOTimerEventSource *)
{
    ((Trackpad *)o)->waechter();
}
static void SchwungUhr(OSObject *o, IOTimerEventSource *)
{
    ((Trackpad *)o)->schwungTakt();
}

#undef super
#define super IOHIPointing
OSDefineMetaClassAndStructors(Trackpad, IOHIPointing);

IOService *Trackpad::probe(IOService *provider, SInt32 *score)
{
    UInt8 data[8];
    IOByteCount length = 8;

    adbDevice = OSDynamicCast(IOADBDevice, provider);
    if (!adbDevice) return 0;
    if (adbDevice->setHandlerID(4) != kIOReturnSuccess) return 0;     /* wie Apple; ist meist schon 4 */
    if (adbDevice->readRegister(1, data, &length) != kIOReturnSuccess || length != 8) return 0;
    deviceSignature  = ((UInt32 *)data)[0];
    deviceResolution = ((UInt16 *)data)[2];
    deviceClass      = data[6];
    deviceNumButtons = data[7];
    /* nur das W-Enhanced-Trackpad: Signatur 'tpad' und 4 gemeldete Tasten (siehe Apples start()) */
    if (deviceSignature != 0x74706164 /* 'tpad' */ || deviceNumButtons != 4) return 0;
    return super::probe(provider, score);
}

bool Trackpad::start(IOService *provider)
{
    mach_timespec_t t;
    OSNumber *n;

    _vorher = 0;
    _ctrl = (ADBDeviceControl *)adbDevice->busRef();
    if (!_ctrl) return false;
    _resolution = deviceResolution << 16;
    _notifierA = _notifierT = 0;
    _gettime = OSSymbol::withCString("get_last_keydown");
    _mouseLock = IOLockAlloc();
    if (!_mouseLock) return false;
    _scrollAn = true;                  /* bis IOHIDSystem die Einstellungen schickt */
    _waagerechtAn = true;
    _rechtsklickAn = _rechts = false;
    _schwungUhr = 0;
    _schwungZustand = 0;
    _vV = _vH = _restV = _restH = _schwungTakte = 0;
    _letzterScroll = 0;
    gAbsolut = 0;                      /* frisch gestartetes Trackpad ist im Normalmodus */
    _herzschlagAlt = gHerzschlag;
    _ohneHerzschlag = 100;             /* erst ein neuer Herzschlag schaltet ein */
    _modusFehler = 0;
    _normalOffen = 0;

    t.tv_sec = 1; t.tv_nsec = 0;       /* auf die Tastatur höchstens 1 s warten (wie Apple) */
    _pADBKeyboard = waitForService(serviceMatching("AppleADBKeyboard"), &t);

    n = OSDynamicCast(OSNumber, getProperty("Trackpad Jitter Milliseconds"));
    _jitterclicktime64 = (UInt64)(n ? n->unsigned16BitValue() : 750) * 1000 * 1000;
    nanoseconds_to_absolutetime(_jitterclicktime64, &_jitterclicktimeAB);
    n = OSDynamicCast(OSNumber, getProperty("Trackpad Jitter Max delta"));
    _jitterdelta = n ? n->unsigned16BitValue() : 16;
    nanoseconds_to_absolutetime((UInt64)5 * 60 * 1000 * 1000 * 1000, &_fake5minAB);

    erweitertEinschalten();
    eigenschaftenLesen();
    setProperty(kIOHIDPointerAccelerationTypeKey, kIOHIDTrackpadAccelerationType);
    setProperty("W Enhanced Trackpad", (unsigned long long)true, 8 * sizeof(bool));

    _notifierA = addNotification(gIOFirstMatchNotification, serviceMatching("IOHIPointing"),
                                 (IOServiceNotificationHandler)add_usb_mouse, this, 0);
    _notifierT = addNotification(gIOTerminatedNotification, serviceMatching("IOHIPointing"),
                                 (IOServiceNotificationHandler)remove_usb_mouse, this, 0);

    if (!super::start(provider)) return false;

    uebernehmen();

    _wl = IOWorkLoop::workLoop();
    _uhr = _wl ? IOTimerEventSource::timerEventSource(this, Uhr) : 0;
    if (_uhr && _wl->addEventSource(_uhr) == kIOReturnSuccess) _uhr->setTimeoutMS(WAECHTER_MS);
    _schwungUhr = _wl ? IOTimerEventSource::timerEventSource(this, SchwungUhr) : 0;
    if (_schwungUhr && _wl->addEventSource(_schwungUhr) != kIOReturnSuccess) { _schwungUhr->release(); _schwungUhr = 0; }
    IOLog("Trackpad: bereit, Pakete kommen hierher (zwei Finger = Scrollen)\n");
    return true;
}

void Trackpad::stop(IOService *provider)
{
    _schwungZustand = 0;
    if (_schwungUhr) {
        _schwungUhr->cancelTimeout();
        if (_wl) _wl->removeEventSource(_schwungUhr);
        _schwungUhr->release();
        _schwungUhr = 0;
    }
    if (_uhr) {
        _uhr->cancelTimeout();
        if (_wl) _wl->removeEventSource(_uhr);
        _uhr->release();
        _uhr = 0;
    }
    if (_wl) { _wl->release(); _wl = 0; }
    if (gAbsolut && adbDevice && !adbDevice->isInactive()) { gAbsolut = 2; modusSetzen(MODUS_NORMAL); }
    gAbsolut = 0;
    zurueckgeben();
    if (_notifierA) { _notifierA->remove(); _notifierA = 0; }
    if (_notifierT) { _notifierT->remove(); _notifierT = 0; }
    super::stop(provider);
}

void Trackpad::free(void)
{
    if (_notifierA) { _notifierA->remove(); _notifierA = 0; }
    if (_notifierT) { _notifierT->remove(); _notifierT = 0; }
    if (_gettime) { _gettime->release(); _gettime = 0; }
    if (_mouseLock) { IOLockFree(_mouseLock); _mouseLock = 0; }
    if (_externalMice) { _externalMice->release(); _externalMice = 0; }
    if (_vorher) { _vorher->release(); _vorher = 0; }
    super::free();
}

/* Paketempfang übernehmen: erst handler, dann owner (Zwischenstand handler=Paket, owner=alt
   wird von Paket() an Apples Funktion weitergereicht). */
void Trackpad::uebernehmen(void)
{
    IOService *alt = _ctrl->owner;
    if (alt == this) return;
    if (alt && _ctrl->handler != (ADB_callback_func)Paket) {
        alt->retain();
        if (_vorher) _vorher->release();
        _vorher = alt;
        gVorherHandler = _ctrl->handler;
    }
    gAktiv = this;
    sperre();
    _ctrl->handler = (ADB_callback_func)Paket;
    sperre();
    _ctrl->owner = this;
    sperre();
    gUebernahmen++;
}

/* Paketempfang zurückgeben: erst owner, dann handler (Zwischenstand owner=Apple, handler=Paket
   reicht Paket() an Apples Funktion weiter). */
void Trackpad::zurueckgeben(void)
{
    if (_ctrl && _ctrl->owner == this) {
        if (_vorher && !_vorher->isInactive() && gVorherHandler) {
            _ctrl->owner = _vorher;
            sperre();
            _ctrl->handler = gVorherHandler;
        } else if (adbDevice && adbDevice->isInactive()) {
            _ctrl->handler = (ADB_callback_func)Leer;      /* Gerät wird abgebaut, Struktur gleich weg */
        } else {
            _ctrl->owner = 0;
            sperre();
            _ctrl->handler = 0;
        }
        sperre();
    }
    if (gAktiv == this) gAktiv = 0;
}

void Trackpad::waechter(void)
{
    if (isInactive() || !_uhr) return;
    if (_ctrl->owner != this) uebernehmen();
    positionPruefen();
    _uhr->setTimeoutMS(WAECHTER_MS);
}

/* Register 1 Byte 6 setzen, ADB-Flush (sonst kann das Trackpad hängen, siehe Linux init_trackpad), nachlesen.
   10.11.2026: Bei gleichzeitiger Dauerlast auf der PMU schlug das Nachlesen fehl, es gab keinen Flush mehr und das
   Trackpad antwortete danach gar nicht mehr (erst ein Ruhezustand half). Deshalb jetzt auf jedem Fehlerweg Flush
   und ein zweiter Leseversuch. */
bool Trackpad::modusSetzen(UInt8 modus)
{
    UInt8 r[8];
    IOByteCount l = 8;
    if (adbDevice->readRegister(1, r, &l) != kIOReturnSuccess || l != 8) {
        adbDevice->flush();
        l = 8;
        if (adbDevice->readRegister(1, r, &l) != kIOReturnSuccess || l != 8) return false;
    }
    if (r[6] == modus) return true;
    r[6] = modus;
    l = 8;
    if (adbDevice->writeRegister(1, r, &l) != kIOReturnSuccess) { adbDevice->flush(); return false; }
    adbDevice->flush();
    l = 8;
    if (adbDevice->readRegister(1, r, &l) != kIOReturnSuccess || l != 8) {
        adbDevice->flush();
        l = 8;
        if (adbDevice->readRegister(1, r, &l) != kIOReturnSuccess || l != 8) return false;
    }
    return r[6] == modus;
}

/* Positionsmodus nur, solange ein Programm ihn will und sich regelmäßig meldet */
void Trackpad::positionPruefen(void)
{
    bool soll;
    UInt32 h = gHerzschlag;
    if (h != _herzschlagAlt) { _herzschlagAlt = h; _ohneHerzschlag = 0; }
    else if (_ohneHerzschlag < 100) _ohneHerzschlag++;
    soll = gWunsch && _ohneHerzschlag < 4;                  /* 4 Takte = 1 s ohne Meldung */
    if (!soll) _modusFehler = 0;
    if (soll && gAbsolut != 1) {
        if (_modusFehler >= 3) return;                      /* klappt nicht: nicht ständig neu versuchen */
        _schwungZustand = 0;
        gAbsolut = 2;
        if (modusSetzen(MODUS_POSITION)) {
            gMinX = gMinY = 99999; gMaxX = gMaxY = gMaxZ = 0;
            gBeruehrt = 0;
            gEinschaltungen++;
            gAbsolut = 1;
        } else {
            _modusFehler++;
            IOLog("Trackpad: Positionsmodus nicht verfügbar\n");
            if (!modusSetzen(MODUS_NORMAL)) _normalOffen = 40;
            gAbsolut = 0;
        }
    } else if (!soll && gAbsolut != 0) {
        gAbsolut = 2;
        if (!modusSetzen(MODUS_NORMAL)) _normalOffen = 40;
        gBeruehrt = 0;
        gAbsolut = 0;
    } else if (!soll && _normalOffen > 0) {                /* Normalmodus nachholen, bis zu 10 s lang */
        if (modusSetzen(MODUS_NORMAL)) {
            _normalOffen = 0;
            gNachgeholt++;
            IOLog("Trackpad: Normalmodus nachgeholt\n");
        } else if (--_normalOffen == 0)
            IOLog("Trackpad: Normalmodus nicht erreichbar, Ruhezustand setzt das Trackpad zurück\n");
    } else if (soll) {                                      /* hat jemand den Modus zurückgestellt? */
        UInt8 r[8];
        IOByteCount l = 8;
        if (adbDevice->readRegister(1, r, &l) == kIOReturnSuccess && l == 8 && r[6] != MODUS_POSITION) {
            gAbsolut = 2;
            if (modusSetzen(MODUS_POSITION)) gModusNeu++;
            gAbsolut = 1;
        }
    }
}

UInt32 Trackpad::interfaceID(void) { return NX_EVS_DEVICE_INTERFACE_ADB; }
UInt32 Trackpad::deviceType(void) { return adbDevice->handlerID(); }
IOFixed Trackpad::resolution(void) { return _resolution; }
IOItemCount Trackpad::buttonCount(void) { return 1; }   /* alle Apple-Trackpads haben 1 Taste */

/* Andere Zeigegeräte merken (für "Trackpad ignorieren, wenn Maus angeschlossen").
   Apples Treiber am selben Gerät zählt nicht als Maus. */
void Trackpad::_check_usb_mouse(IOService *service, bool added)
{
    if (service == this || !OSDynamicCast(IOHIPointing, service) ||
        service->getProperty(kIOHIDVirtualHIDevice) == kOSBooleanTrue ||
        service->getProvider() == adbDevice)
        return;
    if (!_mouseLock) return;
    IOLockLock(_mouseLock);
    if (!_externalMice && !(_externalMice = OSSet::withCapacity(4))) { IOLockUnlock(_mouseLock); return; }
    if (added) _externalMice->setObject(service);
    else _externalMice->removeObject(service);
    IOLockUnlock(_mouseLock);
}

void Trackpad::scrollen(int dx, int dy, uint64_t now)
{
    int v = dy, h = _waagerechtAn ? dx : 0;
    /* Achsensperre: bei klar senkrechter Bewegung kein seitliches Zittern und umgekehrt */
    if (my_abs(v) >= 2 * my_abs(h)) h = 0;
    else if (my_abs(h) >= 2 * my_abs(v)) v = 0;
    /* IOHIPointing: positiv = wie Mausrad von sich weg (nach oben). Natürlich: Finger hoch (dy < 0)
       schiebt den Inhalt hoch, also Richtung Dokumentende = negativ. */
    if (!gNatuerlich) { v = -v; h = -h; }
    if (v > 1000) v = 1000; else if (v < -1000) v = -1000;
    if (h > 1000) h = 1000; else if (h < -1000) h = -1000;
    if (v || h) dispatchScrollWheelEvent((short)v, (short)h, 0, alsZeit(now));
}

/* ---------- Nachlaufen ----------
 * Beim Scrollen wird die Geschwindigkeit je 16 ms (in 1/256 Zählern) gleitend gemittelt. Kommt 60 ms lang kein
 * Scrollpaket mehr (Finger weg) oder meldet das Trackpad "alle Finger weg", läuft das Scrollen mit dieser
 * Geschwindigkeit weiter und klingt je Takt um 1/16 ab. Jede neue Berührung beendet es. */
enum { SCHWUNG_AUS = 0, SCHWUNG_WARTET = 1, SCHWUNG_LAEUFT = 2 };

static SInt32 geschwindigkeit(int d, uint64_t dtNs)
{
    if (dtNs < 4000000ULL) dtNs = 4000000ULL;            /* 4..100 ms zwischen zwei Paketen */
    if (dtNs > 100000000ULL) dtNs = 100000000ULL;
    return (SInt32)(((SInt64)d * 256 * 16000000LL) / (SInt64)dtNs);
}

void Trackpad::schwungTakt(void)
{
    uint64_t now;
    int dV, dH;
    if (_schwungZustand == SCHWUNG_WARTET) {             /* Ruhe nach dem Scrollen: Finger sind weg */
        if (!gSchwung || (my_abs(_vV) < SCHWUNG_START && my_abs(_vH) < SCHWUNG_START)) { _schwungZustand = SCHWUNG_AUS; return; }
        _schwungZustand = SCHWUNG_LAEUFT;
        _restV = _restH = 0;
        _schwungTakte = 0;
        gSchwuenge++;
    }
    if (_schwungZustand != SCHWUNG_LAEUFT) return;
    _restV += _vV;
    _restH += _vH;
    dV = _restV / 256; _restV -= dV * 256;
    dH = _restH / 256; _restH -= dH * 256;
    clock_get_uptime(&now);
    if (dV || dH) scrollen(dH, dV, now);
    _vV = _vV * 15 / 16;
    _vH = _vH * 15 / 16;
    if (++_schwungTakte > SCHWUNG_MAX_T || (my_abs(_vV) < SCHWUNG_ENDE && my_abs(_vH) < SCHWUNG_ENDE)) {
        _schwungZustand = SCHWUNG_AUS;
        return;
    }
    if (_schwungUhr) _schwungUhr->setTimeoutMS(SCHWUNG_MS);
}

void Trackpad::packet(UInt8, IOByteCount length, UInt8 *data)
{
    int dx, dy, cnt, numExtraBytes;
    bool palm = false, outzone = false, zwei = false, has2fingers = false, losgelassen = false;
    UInt32 buttonState = 0;
    uint64_t now;

    if (length < 2) return;
    gPakete++;
    if (gProtokoll > 0) {
        gProtokoll--;
        IOLog("Trackpad: %u Bytes %02x %02x %02x %02x %02x (Modus %d)\n", (unsigned)length, data[0], data[1],
              length > 2 ? data[2] : 0, length > 3 ? data[3] : 0, length > 4 ? data[4] : 0, gAbsolut);
    }
    /* NEU: Positionsmodus. Byte 0 Taste + Y 0-6, Byte 1 X 0-6, Byte 2/3 je Y (Bits 4-6) und X (Bits 0-2) höher,
       Byte 4 Druck Z (Bits 0-2 und 4-6). Der Zeiger bleibt stehen. */
    if (gAbsolut) {
        if (gAbsolut == 1 && length >= 5) {
            int x = (data[1] & 0x7f) | ((data[2] & 0x07) << 7) | ((data[3] & 0x07) << 10);
            int y = (data[0] & 0x7f) | ((data[2] & 0x70) << 3) | ((data[3] & 0x70) << 6);
            int z = (data[4] & 0x07) | ((data[4] & 0x70) >> 1);
            /* Abheben = Paket 80 80 88 88 00 (Position 0/0). Der Druck taugt beim iBook-G4-Trackpad nicht als
               Schwelle: leichtes Auflegen 1-7, Ziehen um 16-22, erst festes Drücken über 30 (adbsyn-Schwelle). */
            bool auf = x != 0 || y != 0;
            gPosX = x; gPosY = y; gPosZ = z;
            sperre();
            gPosZaehler++;
            gBeruehrt = auf;
            if (auf) {
                if (x < gMinX) gMinX = x;
                if (x > gMaxX) gMaxX = x;
                if (y < gMinY) gMinY = y;
                if (y > gMaxY) gMaxY = y;
            }
            if (z > gMaxZ) gMaxZ = z;
        }
        return;
    }
    losgelassen = length >= 4 && (data[3] & 0x08);           /* Trackpad: alle Finger weg */
    gBeruehrt = !losgelassen;

    IOLockLock(_mouseLock);
    bool shouldIgnore = (((_ignoreTrackpadState & kIgnoreTrackpadState_USBMouse) && _externalMice && _externalMice->getCount() > 0)
                         || ((_ignoreTrackpadState & kIgnoreTrackpadState_MouseKeys) && (_ignoreTrackpadState & kIgnoreTrackpadState_MouseKeys_State_On)));
    IOLockUnlock(_mouseLock);
    if (shouldIgnore) return;

    numExtraBytes = length - 2;
    dy = data[0] & 0x7f;
    dx = data[1] & 0x7f;
    if ((data[0] & 0x80) == 0) buttonState |= 1;

    if (length >= 4) {
        if ((data[2] & 0x80) == 0 && _zonenomove) outzone = true;
        if ((data[2] & 0x08) == 0 && _palmnomove) palm = true;
        if ((data[3] & 0x80) == 0) {              /* zwei Finger laut Trackpad */
            zwei = true;
            _sticky2finger = true;
            clock_get_uptime(&_sticky2fingerTimeAB);
        }
        if ((data[3] & 0x08) != 0) _sticky2finger = false;   /* alle Finger weg */
        if (_sticky2finger && !zwei) {            /* kurz nachhalten, damit kein Sprung entsteht */
            uint64_t d;
            clock_get_uptime(&d);
            if (d - _sticky2fingerTimeAB < _timeoutStickyAB) zwei = true;
            else _sticky2finger = false;
        }
        has2fingers = zwei && _2fingernoaction;
        data[2] |= 0x88;                          /* Tastenbits (negative Logik) löschen, wie Apple */
        data[3] |= 0x88;
    }

    if ((deviceNumButtons > 1) && ((data[1] & 0x80) == 0)) {
        if (_jitterclick && _pADBKeyboard) {
            _pADBKeyboard->callPlatformFunction(_gettime, false, (void *)&_keyboardTimeAB, 0, 0, 0);
            clock_get_uptime(&now);
            if ((now - _keyboardTimeAB > _jitterclicktimeAB) || (_oldButtonState & 0x1)) buttonState |= 1;
            if (!outzone) buttonState |= 1;
            if (_zonepeckingtimeAB != _keyboardTimeAB) buttonState = 0;
        } else
            buttonState |= 1;
    }

    for (cnt = 0; cnt < numExtraBytes; cnt++) {
        dy |= ((data[2 + cnt] >> 4) & 7) << (7 + (cnt * 3));
        dx |= ((data[2 + cnt])      & 7) << (7 + (cnt * 3));
        if ((deviceNumButtons > (cnt + 2)) && ((data[2 + cnt] & 0x80) == 0)) buttonState |= 4 << (cnt * 2);
        if ((deviceNumButtons > (cnt + 2 + 1)) && ((data[2 + cnt] & 0x08) == 0)) buttonState |= 4 << (cnt * 2 + 1);
    }
    if (dy & (0x40 << (numExtraBytes * 3))) dy |= (0xffffffc0 << (numExtraBytes * 3));
    if (dx & (0x40 << (numExtraBytes * 3))) dx |= (0xffffffc0 << (numExtraBytes * 3));

    clock_get_uptime(&now);

    /* NEU: Nachlaufen. "Alle Finger weg" startet es sofort, jede andere Berührung beendet es. */
    if (_schwungZustand != SCHWUNG_AUS) {
        if (losgelassen) {
            if (_schwungZustand == SCHWUNG_WARTET && _schwungUhr) _schwungUhr->setTimeoutMS(1);
            return;
        }
        _schwungZustand = SCHWUNG_AUS;
        if (_schwungUhr) _schwungUhr->cancelTimeout();
    }

    /* NEU: Rechtsklick – beim Drücken entscheiden (zwei Finger + Schalter an), bis zum Loslassen halten */
    if (buttonState & 1) {
        if (!(_oldButtonState & 3)) {
            _rechts = zwei && _rechtsklickAn;
            if (_rechts) gRechtsklicks++;
        }
        if (_rechts) { buttonState = (buttonState & ~1u) | 2; dx = 0; dy = 0; }
    } else
        _rechts = false;

    /* NEU: zwei Finger und keine Taste -> scrollen statt Zeiger bewegen */
    if (zwei && _scrollAn && !buttonState && !(_oldButtonState & 3)) {
        uint64_t dt = now - _letzterScroll;
        gScrollPakete++;
        absolutetime_to_nanoseconds(dt, &dt);
        if (dt > 100000000ULL) { _vV = geschwindigkeit(dy, dt); _vH = geschwindigkeit(dx, dt); }  /* neuer Wisch */
        else { _vV = (_vV + geschwindigkeit(dy, dt)) / 2; _vH = (_vH + geschwindigkeit(dx, dt)) / 2; }
        _letzterScroll = now;
        scrollen(dx, dy, now);
        if (gSchwung && _schwungUhr) {                   /* kommt 60 ms nichts mehr, sind die Finger weg */
            _schwungZustand = SCHWUNG_WARTET;
            _schwungUhr->setTimeoutMS(60);
        }
        return;
    }

    /* ab hier wie Apples packetW() */
    if (_jittermove) {
        if ((has2fingers && !_rechts) || (outzone && palm)) {
            if (!buttonState && !_oldButtonState) return;
            else if ((data[1] & 0x80) == 0) return;
            else { dx = 0; dy = 0; }
        }
        if (!outzone && _pADBKeyboard) {
            _pADBKeyboard->callPlatformFunction(_gettime, false, (void *)&_zonepeckingtimeAB, 0, 0, 0);
        } else if (_pADBKeyboard) {
            _pADBKeyboard->callPlatformFunction(_gettime, false, (void *)&_keyboardTimeAB, 0, 0, 0);
            if (now - _keyboardTimeAB < _fake5minAB) {
                if (_zonepeckingtimeAB != _keyboardTimeAB) {
                    if (!buttonState && !_oldButtonState) return;
                    else if ((data[1] & 0x80) == 0) return;
                    else { dx = 0; dy = 0; }
                }
            }
        }
    }

    _oldButtonState = buttonState;
    dispatchRelativePointerEvent(dx, dy, buttonState, alsZeit(now));
}

OSData *Trackpad::copyAccelerationTable(void)
{
    char keyName[10];
    OSData *data;

    strlcpy(keyName, "accl", sizeof(keyName));
    keyName[4] = (deviceSignature >> 24);
    keyName[5] = (deviceSignature >> 16);
    keyName[6] = (deviceSignature >> 8);
    keyName[7] = (deviceSignature >> 0);
    keyName[8] = 0;
    IOLockLock(_mouseLock);
    data = OSDynamicCast(OSData, getProperty(keyName));
    IOLockUnlock(_mouseLock);
    if (data) data->retain();
    else data = super::copyAccelerationTable();
    return data;
}

/* Erweiterten Modus einschalten, falls Apples Treiber das noch nicht getan hat (nach dem Aufwachen
   kann dieser Treiber zuerst starten). Register wie Apples enableEnhancedMode(). */
void Trackpad::erweitertEinschalten(void)
{
    UInt8 adbdata[8];
    IOByteCount adblength = 8;

    if (adbDevice->readRegister(1, adbdata, &adblength) != kIOReturnSuccess) return;
    if (adbdata[6] == 0x0D) {          /* schon an: Einstellungen aus Register 2 übernehmen */
        adblength = 8;
        if (adbDevice->readRegister(2, adbdata, &adblength) == kIOReturnSuccess) {
            Clicking = (adbdata[0] & 0x80) != 0;
            Dragging = (adbdata[1] & 0x80) != 0;
            DragLock = adbdata[3] == 0xFF;
        }
    } else {
        adbdata[6] = 0x0D;
        if (adbDevice->writeRegister(1, adbdata, &adblength) != kIOReturnSuccess) return;
        adblength = 8;
        if (adbDevice->readRegister(1, adbdata, &adblength) != kIOReturnSuccess || adbdata[6] != 0x0D) {
            IOLog("Trackpad: erweiterter Modus nicht verfügbar\n");
            return;
        }
        adbdata[0] = 0x19; adbdata[1] = 0x14; adbdata[2] = 0x19; adbdata[3] = 0xB2;
        adbdata[4] = 0xB2; adbdata[5] = 0x8A; adbdata[6] = 0x1B; adbdata[7] = 0x57;
        adblength = 8;
        adbDevice->writeRegister(2, adbdata, &adblength);
        Clicking = Dragging = DragLock = false;
    }
    setProperty("Clicking", (unsigned long long)Clicking, 8 * sizeof(bool));
    setProperty("Dragging", (unsigned long long)Dragging, 8 * sizeof(bool));
    setProperty("DragLock", (unsigned long long)DragLock, 8 * sizeof(bool));
}

static bool zahl(IOService *s, const char *name, bool vorgabe)
{
    OSNumber *n = OSDynamicCast(OSNumber, s->getProperty(name));
    return n ? (n->unsigned16BitValue() != 0) : vorgabe;
}

void Trackpad::eigenschaftenLesen(void)
{
    OSNumber *n;
    /* W-Enhanced-Trackpads: Filter standardmäßig an (wie Apple) */
    setProperty("JitterNoClick", true, sizeof(UInt32));
    setProperty("JitterNoMove", true, sizeof(UInt32));
    _jitterclick = _jittermove = true;
    n = OSDynamicCast(OSNumber, getProperty("W sticky input timeout"));
    _timeoutSticky64 = (UInt64)(n ? n->unsigned32BitValue() : 500) * 1000 * 1000;
    nanoseconds_to_absolutetime(_timeoutSticky64, &_timeoutStickyAB);
    n = OSDynamicCast(OSNumber, getProperty("W threshold"));
    _WThreshold = n ? (UInt8)n->unsigned16BitValue() : 7;
    _2fingernoaction = zahl(this, "TwofingerNoAction", false);
    _palmnoaction = zahl(this, "PalmNoAction Permanent", false);
    _palmnomove = zahl(this, "PalmNoAction When Typing", false);
    _zonenoaction = zahl(this, "OutsidezoneNoAction Permanent", false);
    _zonenomove = zahl(this, "OutsidezoneNoAction When Typing", false);
    _zonepecknomove = zahl(this, "OutsidezoneNoAction When Pecking", false);
    _ignorezone = false;
    nanoseconds_to_absolutetime(0, &_zonepeckingtimeAB);
}

IOReturn Trackpad::setParamProperties(OSDictionary *dict)
{
    OSNumber *datan;
    UInt8 adbdata[8];
    IOByteCount adblength;
    UInt16 settrue;

    IOLockLock(_mouseLock);

    /* NEU: Schalter aus den Trackpad-Einstellungen */
    if ((datan = OSDynamicCast(OSNumber, dict->getObject("TrackpadScroll")))) {
        _scrollAn = datan->unsigned32BitValue() != 0;
        gScrollAn = _scrollAn;
    }
    if ((datan = OSDynamicCast(OSNumber, dict->getObject("TrackpadHorizScroll")))) {
        _waagerechtAn = datan->unsigned32BitValue() != 0;
        gWaagerechtAn = _waagerechtAn;
    }
    if ((datan = OSDynamicCast(OSNumber, dict->getObject("TrackpadRightClick")))) {
        _rechtsklickAn = datan->unsigned32BitValue() != 0;
        gRechtsklickAn = _rechtsklickAn;
    }

    if ((datan = OSDynamicCast(OSNumber, dict->getObject("Clicking")))) {
        settrue = datan->unsigned32BitValue() ? 1 : 0;
        adblength = sizeof(adbdata);
        adbDevice->readRegister(2, adbdata, &adblength);
        adbdata[0] = (adbdata[0] & 0x7F) | (settrue << 7);
        setProperty("Clicking", (unsigned long long)((adbdata[0] & 0x80) >> 7), 8);
        adbDevice->writeRegister(2, adbdata, &adblength);
    }
    if ((datan = OSDynamicCast(OSNumber, dict->getObject("Dragging")))) {
        settrue = datan->unsigned32BitValue() ? 1 : 0;
        adblength = sizeof(adbdata);
        adbDevice->readRegister(2, adbdata, &adblength);
        adbdata[1] = (adbdata[1] & 0x7F) | (settrue << 7);
        setProperty("Dragging", (unsigned long long)((adbdata[1] & 0x80) >> 7), 8);
        adbDevice->writeRegister(2, adbdata, &adblength);
    }
    if ((datan = OSDynamicCast(OSNumber, dict->getObject("DragLock")))) {
        adblength = sizeof(adbdata);
        adbDevice->readRegister(2, adbdata, &adblength);
        adbdata[3] = datan->unsigned32BitValue();
        setProperty("DragLock", (unsigned long long)adbdata[3], 8);
        adbdata[3] = adbdata[3] ? 0xFF : 0xB2;
        adblength = sizeof(adbdata);
        adbDevice->writeRegister(2, adbdata, &adblength);
    }
    if ((datan = OSDynamicCast(OSNumber, dict->getObject("JitterNoClick")))) {
        _jitterclick = datan->unsigned32BitValue() != 0;
        setProperty("JitterNoClick", _jitterclick, sizeof(UInt32));
    }
    if ((datan = OSDynamicCast(OSNumber, dict->getObject("JitterNoMove")))) {
        _jittermove = datan->unsigned32BitValue() != 0;
        setProperty("JitterNoMove", _jittermove, sizeof(UInt32));
        _jitterclick = _jittermove;
    }
    if ((datan = OSDynamicCast(OSNumber, dict->getObject("Trackpad Jitter Milliseconds")))) {
        _jitterclicktime64 = (UInt64)datan->unsigned32BitValue() * 1000 * 1000;
        setProperty("Trackpad Jitter Milliseconds", datan->unsigned32BitValue(), 8);
        nanoseconds_to_absolutetime(_jitterclicktime64, &_jitterclicktimeAB);
    }
    if ((datan = OSDynamicCast(OSNumber, dict->getObject("W sticky input timeout")))) {
        _timeoutSticky64 = (UInt64)datan->unsigned32BitValue() * 1000 * 1000;
        setProperty("W sticky input timeout", datan->unsigned32BitValue(), 8);
        nanoseconds_to_absolutetime(_timeoutSticky64, &_timeoutStickyAB);
    }
    if ((datan = OSDynamicCast(OSNumber, dict->getObject("W threshold")))) {
        _WThreshold = (UInt8)datan->unsigned32BitValue() & 0xf;
        setProperty("W threshold", _WThreshold, 8);
        adblength = sizeof(adbdata);
        adbDevice->readRegister(2, adbdata, &adblength);
        adbdata[7] = (adbdata[7] & 0xf0) | _WThreshold;
        adblength = sizeof(adbdata);
        adbDevice->writeRegister(2, adbdata, &adblength);
    }
    if ((datan = OSDynamicCast(OSNumber, dict->getObject("PalmNoAction Permanent")))) {
        _palmnoaction = datan->unsigned32BitValue() != 0;
        setProperty("PalmNoAction Permanent", _palmnoaction, sizeof(UInt32));
    }
    if ((datan = OSDynamicCast(OSNumber, dict->getObject("PalmNoAction When Typing")))) {
        _palmnomove = datan->unsigned32BitValue() != 0;
        setProperty("PalmNoAction When Typing", _palmnomove, sizeof(UInt32));
    }
    if ((datan = OSDynamicCast(OSNumber, dict->getObject("TwofingerNoAction")))) {
        _2fingernoaction = datan->unsigned32BitValue() != 0;
        setProperty("TwofingerNoAction", _2fingernoaction, sizeof(UInt32));
    }
    if ((datan = OSDynamicCast(OSNumber, dict->getObject("OutsidezoneNoAction Permanent")))) {
        _zonenoaction = datan->unsigned32BitValue() != 0;
        setProperty("OutsidezoneNoAction Permanent", _zonenoaction, sizeof(UInt32));
    }
    if ((datan = OSDynamicCast(OSNumber, dict->getObject("OutsidezoneNoAction When Typing")))) {
        _zonenomove = datan->unsigned32BitValue() != 0;
        setProperty("OutsidezoneNoAction When Typing", _zonenomove, sizeof(UInt32));
    }
    if ((datan = OSDynamicCast(OSNumber, dict->getObject("OutsidezoneNoAction When Pecking")))) {
        _zonepecknomove = datan->unsigned32BitValue() != 0;
        setProperty("OutsidezoneNoAction When Pecking", _zonepecknomove, sizeof(UInt32));
    }
    if ((datan = OSDynamicCast(OSNumber, dict->getObject("USBMouseStopsTrackpad")))) {
        UInt8 mode = datan->unsigned32BitValue();
        setProperty("USBMouseStopsTrackpad", (unsigned long long)mode, 8);
        if (mode) _ignoreTrackpadState |= kIgnoreTrackpadState_USBMouse;
        else _ignoreTrackpadState &= ~kIgnoreTrackpadState_USBMouse;
    }
    if ((datan = OSDynamicCast(OSNumber, dict->getObject("MouseKeysStopsTrackpad")))) {
        UInt8 mode = datan->unsigned32BitValue();
        setProperty("MouseKeysStopsTrackpad", (unsigned long long)mode, 8);
        if (mode) _ignoreTrackpadState |= kIgnoreTrackpadState_MouseKeys;
        else _ignoreTrackpadState &= ~kIgnoreTrackpadState_MouseKeys;
    }
    if ((datan = OSDynamicCast(OSNumber, dict->getObject(kIOHIDMouseKeysOnKey)))) {
        if (datan->unsigned32BitValue()) _ignoreTrackpadState |= kIgnoreTrackpadState_MouseKeys_State_On;
        else _ignoreTrackpadState &= ~kIgnoreTrackpadState_MouseKeys_State_On;
    }

    IOLockUnlock(_mouseLock);
    return super::setParamProperties(dict);
}

/* ---------- sysctl debug.trackpad ---------- */
static int zustandHandler(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    char t[480];
    snprintf(t, sizeof(t), "Pakete %u, davon Scrollen %u, Scrollen erlaubt %d, waagerecht %d, natuerlich %d, "
             "Rechtsklick erlaubt %d (%u), Nachlaufen %d (%u), Empfang %s, Uebernahmen %u, "
             "Positionsmodus %d (Wunsch %d, eingeschaltet %u, erneuert %u, nachgeholt %u, Pakete %u, x %d..%d, y %d..%d, z bis %d)",
             (unsigned)gPakete, (unsigned)gScrollPakete, gScrollAn, gWaagerechtAn, gNatuerlich, gRechtsklickAn,
             (unsigned)gRechtsklicks, gSchwung, (unsigned)gSchwuenge, gAktiv ? "hier" : "bei Apple", (unsigned)gUebernahmen,
             gAbsolut, gWunsch, (unsigned)gEinschaltungen, (unsigned)gModusNeu, (unsigned)gNachgeholt, (unsigned)gPosZaehler,
             gMinX, gMaxX, gMinY, gMaxY, gMaxZ);
    return SYSCTL_OUT(req, t, strlen(t) + 1);
}

/* Wunsch nach dem Positionsmodus: 1 = an (zugleich Herzschlag, alle 0,25 s schreiben), 0 = aus */
static int positionAnHandler(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req)
{
    int v = gAbsolut == 1, fehler = sysctl_handle_int(oidp, &v, 0, req);
    if (fehler || !req->newptr) return fehler;
    gWunsch = v != 0;
    if (v) gHerzschlag++;
    return 0;
}

static int positionHandler(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    int w[5];
    w[0] = gPosX; w[1] = gPosY; w[2] = gPosZ; w[3] = (int)gPosZaehler; w[4] = gAbsolut == 1;
    return SYSCTL_OUT(req, w, sizeof(w));
}

SYSCTL_NODE(_debug, OID_AUTO, trackpad, CTLFLAG_RW, 0, "Trackpad iBook G4");
SYSCTL_INT(_debug_trackpad, OID_AUTO, natuerlich, CTLFLAG_RW | CTLFLAG_ANYBODY, &gNatuerlich, 0,
           "1: Inhalt folgt den Fingern, 0: wie ein Mausrad");
SYSCTL_INT(_debug_trackpad, OID_AUTO, protokoll, CTLFLAG_RW, &gProtokoll, 0, "so viele Pakete ins Systemprotokoll");
SYSCTL_INT(_debug_trackpad, OID_AUTO, schwung, CTLFLAG_RW | CTLFLAG_ANYBODY, &gSchwung, 0, "1: nach dem Wischen nachlaufen lassen");
SYSCTL_INT(_debug_trackpad, OID_AUTO, beruehrt, CTLFLAG_RD | CTLFLAG_ANYBODY, &gBeruehrt, 0, "1: Finger liegt auf dem Trackpad");
SYSCTL_PROC(_debug_trackpad, OID_AUTO, zustand, CTLTYPE_STRING | CTLFLAG_RD, 0, 0, zustandHandler, "A", "Zustand");
SYSCTL_PROC(_debug_trackpad, OID_AUTO, position_an, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY, 0, 0, positionAnHandler, "I",
            "Positionsmodus wuenschen: 1 an (alle 0,25 s wiederholen), 0 aus");
SYSCTL_PROC(_debug_trackpad, OID_AUTO, position, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_ANYBODY, 0, 0, positionHandler, "I",
            "x y z Zaehler Modus");

extern "C" kern_return_t trackpadModulStart(kmod_info_t *, void *)
{
    sysctl_register_oid(&sysctl__debug_trackpad);
    sysctl_register_oid(&sysctl__debug_trackpad_natuerlich);
    sysctl_register_oid(&sysctl__debug_trackpad_protokoll);
    sysctl_register_oid(&sysctl__debug_trackpad_schwung);
    sysctl_register_oid(&sysctl__debug_trackpad_beruehrt);
    sysctl_register_oid(&sysctl__debug_trackpad_zustand);
    sysctl_register_oid(&sysctl__debug_trackpad_position_an);
    sysctl_register_oid(&sysctl__debug_trackpad_position);
    return KERN_SUCCESS;
}

extern "C" kern_return_t trackpadModulStop(kmod_info_t *, void *)
{
    gWunsch = 0;
    sysctl_unregister_oid(&sysctl__debug_trackpad_position);
    sysctl_unregister_oid(&sysctl__debug_trackpad_position_an);
    sysctl_unregister_oid(&sysctl__debug_trackpad_zustand);
    sysctl_unregister_oid(&sysctl__debug_trackpad_beruehrt);
    sysctl_unregister_oid(&sysctl__debug_trackpad_schwung);
    sysctl_unregister_oid(&sysctl__debug_trackpad_protokoll);
    sysctl_unregister_oid(&sysctl__debug_trackpad_natuerlich);
    sysctl_unregister_oid(&sysctl__debug_trackpad);
    return KERN_SUCCESS;
}
