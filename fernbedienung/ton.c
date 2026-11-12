/* Ton der Bildschirmsynchronisierung: der AirPlay-Dienst reicht die RTP-Pakete verschluesselt ueber fd 3 weiter
   (Saetze siehe airplay.c, ton_schicken). Hier: AES-128-CBC entschluesseln (nur ganze 16-Byte-Bloecke, IV je
   Paket neu), AAC-ELD dekodieren (FFmpeg aus PowerVLC), in einen Ring schreiben; die Ausgabe (CoreAudio,
   DefaultOutput) spielt jedes Stueck zu seinem Zeitpunkt.

   Zeitpunkt: Das iPhone stempelt Ton (RTP-Zeit, ueber die Synchronpakete auf NTP abgebildet) und Bild (NTP-Zeit
   im Rahmenkopf) auf einen gemeinsamen Wiedergabetermin. Das Fenster zeigt Bilder nicht zum Termin, sondern so
   frueh wie moeglich, im Mittel D Sekunden vorher (laufend gemessen in ton_bild). Damit Ton und Bild
   zusammenpassen, kommt der Ton ebenfalls D vor seinem Termin aus dem Lautsprecher, plus die Zeit, bis ein
   gezeichnetes Bild sichtbar ist. Kommt der Ton dafuer zu knapp an, spielt er so frueh, wie es eben geht. */
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreAudio/HostTime.h>
#include <CoreServices/CoreServices.h>
#include <openssl/aes.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <mach/mach_time.h>
#include "dekoder.h"
#include "ton.h"

#define RATE 44100
#define RING (1 << 16)          /* Rahmen, ~1,5 s */
#define BILDSCHIRM 0.015        /* uebergebenes Bild bis sichtbar: Zeichenfaden, vsync, Bildaufbau */
#define NACHLAUF 0.040          /* Ton bewusst etwas nach dem Bild: unter der Wahrnehmungsschwelle (Ton spaeter als
                                   Bild faellt erst ab ~100 ms auf), gibt Reserve gegen Verzoegerungen im WLAN */
#define SPRUNG 0.030            /* liegt der Ton so weit zu spaet, springt er einmal an die richtige Stelle */
#define RESERVE 0.008           /* Schwankung beim Dekodieren, zusaetzlich zu Puffer und Latenz der Ausgabe */
#define FEIN 0.002              /* je angefangene 2 ms Abweichung ein Rahmen pro Ausgabepuffer auslassen oder doppeln */
#define FEIN_MAX 6              /* hoechstens 6 je Puffer (512 Rahmen), gut 1 %: nur beim Aufholen */
#define HART 0.030              /* ab hier springen */
#define NACHFUEHREN 0.0001      /* Ton folgt dem Bild hoechstens 0,1 ms je Paket (9 ms/s) ... */
#define TOLERANZ 0.008          /* ... und erst ab 8 ms Unterschied */
#define FENSTER 160             /* Pakete (~1,7 s) fuer den kleinsten Vorlauf */

static float Ring[RING * 2];
static volatile unsigned long Schreib, Lese;      /* laufende Rahmennummern */

/* Fahrplan: Rahmen i soll zur Zeit t0 + (i - i0) / RATE hoerbar sein; Sequenzzaehler gegen halbes Lesen */
static volatile unsigned Plan_seq;
static volatile double Plan_t0;
static volatile unsigned long Plan_i0;

static volatile float Verstaerkung = 1.0f;
static double Geraet_latenz, Puffer_s = 512.0 / RATE;
static int Laeuft;

/* Zaehler fuer das Protokoll */
static volatile unsigned long St_pakete, St_rahmen, St_fehler, St_luecken, St_spaet, St_voll, St_unterlauf, St_fein, St_hart;
static volatile double St_vorlauf_min = 1e9, St_fehler_summe;
static volatile unsigned long St_fehler_n;

/* Zustand des Tonfadens */
static AES_KEY Schluessel;      /* FairPlay-Schluessel, mit dem ECDH-Geheimnis gehasht (wie beim Bild) */
static unsigned char Iv[16];
static int Schluessel_da;
static int Ct, Spf, Sr, Dek_da;
static double Uhr_ab, Ts0;
static int Uhr_da, Sync_da;
static unsigned Sync_rtp;
static double Sync_ntp;
static int Start;
static unsigned Naechst_rtp;
static double Ohne_sync_t0;
static unsigned Ohne_sync_rtp;

/* Bild: wie weit vor seinem Termin das Fenster ein Bild zeigt (gleitender Mittelwert) */
static volatile double D_bild = 0.070;
static volatile int D_da;
static volatile double D_ton;               /* zuletzt verwendeter Vorlauf des Tons */

static double jetzt(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

/* NTP-Sekunden (ab 1900) minus eigene Monotonuhr */
static double wand_mono(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + 2208988800.0 + tv.tv_usec / 1e6 - jetzt();
}

/* Termin (Uhr des iPhones) auf die eigene Monotonuhr; Uhr des iPhones zaehlt mal mit, mal ohne 1900-Versatz */
static double lokal(double termin, double nahe)
{
    double t = termin - Uhr_ab - wand_mono();
    if (fabs(t - nahe) > 1e6) {
        if (fabs(t + 2208988800.0 - nahe) < 1e6) t += 2208988800.0;
        else if (fabs(t - 2208988800.0 - nahe) < 1e6) t -= 2208988800.0;
    }
    return t;
}

static unsigned be32(const unsigned char *p) { return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; }

static double ntp_sek(const unsigned char *p)
{
    return (double)be32(p) + (double)be32(p + 4) / 4294967296.0;
}

/* ---------- Ausgabe ---------- */

static void plan_setzen(double t0, unsigned long i0)
{
    Plan_seq++;
    __sync_synchronize();
    Plan_t0 = t0;
    Plan_i0 = i0;
    __sync_synchronize();
    Plan_seq++;
}

static void plan_lesen(double *t0, unsigned long *i0)
{
    unsigned s;
    do {
        s = Plan_seq;
        __sync_synchronize();
        *t0 = Plan_t0;
        *i0 = Plan_i0;
        __sync_synchronize();
    } while ((s & 1) || s != Plan_seq);
}

static OSStatus wiedergabe(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n,
                           AudioBufferList *bl)
{
    float *o = bl->mBuffers[0].mData, g = Verstaerkung;
    unsigned long verfuegbar = Schreib - Lese, i0;
    double t_aus, t0, e;
    UInt32 k = 0, abstand = 0, naechste = 0;
    int korr = 0, richtung = 0;
    (void)ref; (void)fl; (void)bus;
    t_aus = ((ts->mFlags & kAudioTimeStampHostTimeValid) ? AudioConvertHostTimeToNanos(ts->mHostTime) / 1e9 : jetzt())
            + Geraet_latenz;
    plan_lesen(&t0, &i0);
    e = t_aus - (t0 + (double)(long)(Lese - i0) / RATE);     /* > 0: zu spaet */
    if (!verfuegbar) {
        memset(o, 0, n * 8);
        if (Laeuft) St_unterlauf++;
        Laeuft = 0;
        return noErr;
    }
    if (e > HART || (!Laeuft && e > FEIN)) {
        unsigned long weg = (unsigned long)(e * RATE + 0.5);
        if (weg > verfuegbar) weg = verfuegbar;
        Lese += weg;
        verfuegbar -= weg;
        if (Laeuft) St_hart++;
        if (!verfuegbar) { memset(o, 0, n * 8); return noErr; }
    } else if (e < -HART || (!Laeuft && e < -FEIN)) {
        UInt32 stille = (UInt32)(-e * RATE + 0.5);
        if (stille >= n) { memset(o, 0, n * 8); return noErr; }
        memset(o, 0, stille * 8);
        k = stille;
        if (Laeuft) St_hart++;
    } else if (Laeuft) {
        /* kleine Abweichung: einzelne Rahmen auslassen (zu spaet) oder doppeln (zu frueh), gleichmaessig verteilt */
        St_fehler_summe += e;
        St_fehler_n++;
        korr = (int)(fabs(e) / FEIN);
        if (korr > FEIN_MAX) korr = FEIN_MAX;
        if (korr) {
            richtung = e > 0 ? 1 : -1;
            abstand = n / (UInt32)(korr + 1);
            naechste = abstand;
            St_fein += (unsigned long)korr;
        }
    }
    Laeuft = 1;
    while (k < n && verfuegbar) {
        unsigned long j = Lese % RING;
        if (korr && k == naechste) {
            korr--;
            naechste += abstand;
            if (richtung > 0) { Lese++; verfuegbar--; continue; }
            o[2 * k] = Ring[2 * j] * g;          /* doppeln: ausgeben, ohne weiterzulesen */
            o[2 * k + 1] = Ring[2 * j + 1] * g;
            k++;
            continue;
        }
        o[2 * k] = Ring[2 * j] * g;
        o[2 * k + 1] = Ring[2 * j + 1] * g;
        Lese++;
        verfuegbar--;
        k++;
    }
    if (k < n) {
        memset(o + 2 * k, 0, (n - k) * 8);
        St_unterlauf++;
        Laeuft = 0;
    }
    return noErr;
}

static int ausgabe_an(char *fehler, int nf)
{
    ComponentDescription cd = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    Component c = FindNextComponent(NULL, &cd);
    AudioUnit au;
    AudioStreamBasicDescription f;
    AURenderCallbackStruct cb;
    AudioDeviceID geraet = 0;
    UInt32 sz = sizeof geraet, lat = 0, sicher = 0, puffer = 0;
    if (!c || OpenAComponent(c, &au)) { snprintf(fehler, nf, "keine Tonausgabe"); return -1; }
    memset(&f, 0, sizeof f);
    f.mSampleRate = RATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    f.mBytesPerPacket = 8;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 8;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 32;
    if (AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f)) {
        snprintf(fehler, nf, "Tonformat abgelehnt");
        return -1;
    }
    cb.inputProc = wiedergabe;
    cb.inputProcRefCon = NULL;
    AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (AudioUnitInitialize(au) || AudioOutputUnitStart(au)) { snprintf(fehler, nf, "Tonausgabe startet nicht"); return -1; }
    /* Verzoegerung im Geraet: Latenz + Sicherheitsabstand (in Rahmen) */
    if (!AudioHardwareGetProperty(kAudioHardwarePropertyDefaultOutputDevice, &sz, &geraet) && geraet) {
        sz = sizeof lat;
        AudioDeviceGetProperty(geraet, 0, false, kAudioDevicePropertyLatency, &sz, &lat);
        sz = sizeof sicher;
        AudioDeviceGetProperty(geraet, 0, false, kAudioDevicePropertySafetyOffset, &sz, &sicher);
        sz = sizeof puffer;
        AudioDeviceGetProperty(geraet, 0, false, kAudioDevicePropertyBufferFrameSize, &sz, &puffer);
    }
    Geraet_latenz = (double)(lat + sicher) / RATE;
    if (puffer) Puffer_s = (double)puffer / RATE;
    fprintf(stderr, "Spiegel: Ton an, Geraet %u + %u Rahmen Latenz, Puffer %u Rahmen\n", (unsigned)lat, (unsigned)sicher,
            (unsigned)puffer);
    return 0;
}

/* ---------- Eingang ---------- */

static int lies(int fd, void *b, size_t n)
{
    unsigned char *p = b;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* AudioSpecificConfig: ct 8 = AAC-ELD (Spiegeln, 480 Rahmen), ct 4 = AAC-LC */
static void dekoder_an(void)
{
    static const unsigned char eld[4] = { 0xf8, 0xe8, 0x50, 0x00 };   /* ELD, 44,1 kHz, Stereo, 480 */
    static const unsigned char lc[2] = { 0x12, 0x10 };                 /* LC, 44,1 kHz, Stereo */
    char fehler[120] = "";
    if (Dek_da) return;
    if (Sr && Sr != RATE) { fprintf(stderr, "Spiegel: Ton mit %d Hz nicht vorgesehen\n", Sr); Dek_da = -1; return; }
    if (Ct == 8) Dek_da = dek_ton_an(eld, 4, RATE, 2, fehler, sizeof fehler) ? -1 : 1;
    else if (Ct == 4) Dek_da = dek_ton_an(lc, 2, RATE, 2, fehler, sizeof fehler) ? -1 : 1;
    else { snprintf(fehler, sizeof fehler, "Format ct=%d", Ct); Dek_da = -1; }
    if (Dek_da == 1 && ausgabe_an(fehler, sizeof fehler)) Dek_da = -1;
    if (Dek_da < 0) fprintf(stderr, "Spiegel: kein Ton (%s)\n", fehler);
}

/* hoerbar ab wann? (Monotonuhr) */
static double ziel(unsigned rtp, double an)
{
    if (Sync_da && Uhr_da) {
        static double fenster[FENSTER], folge, letzter_sprung;
        static int fn, fp, folge_da;
        double termin = Sync_ntp + (double)(int)(rtp - Sync_rtp) / RATE;
        double t = lokal(termin, an), vor = t - an, vor_min = vor, soll = D_bild - BILDSCHIRM - NACHLAUF, hoechstens;
        int i;
        if (vor < St_vorlauf_min) St_vorlauf_min = vor;
        fenster[fp] = vor;
        fp = (fp + 1) % FENSTER;
        if (fn < FENSTER) fn++;
        for (i = 0; i < fn; i++) if (fenster[i] < vor_min) vor_min = fenster[i];
        /* nicht frueher, als der Ton sicher da ist: auch das spaeteste Paket der letzten ~2 s muss vor dem
           Ausgabepuffer ankommen, der ihn abspielt (Puffer + Latenz der Soundkarte + Reserve) */
        hoechstens = vor_min - Puffer_s - Geraet_latenz - RESERVE;
        if (soll > hoechstens) soll = hoechstens;
        /* dem Bild nur langsam und mit Toleranz folgen: weniger Korrekturen, Ton bleibt ruhig. Liegt der Ton weit
           zurueck (spaete Pakete gleich nach dem Verbinden, Aussetzer im WLAN), springt er einmal statt langsam
           aufzuholen; zwischen zwei Spruengen liegt mindestens ein Fenster (~1,7 s). */
        if (!folge_da) { folge = soll; folge_da = 1; }
        else if (soll - folge > SPRUNG && an - letzter_sprung > 1.7) { folge = soll; letzter_sprung = an; }
        else if (soll - folge > TOLERANZ) folge += NACHFUEHREN;
        else if (folge - soll > TOLERANZ) folge -= NACHFUEHREN;
        if (folge > hoechstens) folge = hoechstens;
        D_ton = folge;
        return t - folge;
    }
    /* ohne Synchronpakete (die erste Sekunde nach dem Verbinden): fester Puffer nach dem ersten Paket, danach nach
       RTP-Zeit; 40 ms gaben am Anfang noch Unterlaeufe, die ersten Pakete kommen stossweise */
    if (!Ohne_sync_t0) { Ohne_sync_t0 = an + 0.120; Ohne_sync_rtp = rtp; }
    return Ohne_sync_t0 + (double)(int)(rtp - Ohne_sync_rtp) / RATE;
}

static void ring_schreiben(unsigned rtp, const Dek_ton *t, double an)
{
    int luecke, i;
    unsigned long frei;
    if (!Start || (int)(rtp - Naechst_rtp) > RATE / 2 || (int)(rtp - Naechst_rtp) < -RATE / 2) {
        Start = 1;
        Naechst_rtp = rtp;
        Ohne_sync_t0 = 0;
    }
    luecke = (int)(rtp - Naechst_rtp);
    if (luecke < 0) { St_spaet++; return; }
    frei = RING - (Schreib - Lese);
    if ((unsigned long)luecke + (unsigned long)t->anzahl > frei) { St_voll++; Naechst_rtp = rtp + t->anzahl; return; }
    if (luecke > 0) {
        for (i = 0; i < luecke; i++) {
            unsigned long j = (Schreib + i) % RING;
            Ring[2 * j] = Ring[2 * j + 1] = 0;
        }
        __sync_synchronize();
        Schreib += luecke;
        St_luecken++;
    }
    plan_setzen(ziel(rtp, an), Schreib);
    for (i = 0; i < t->anzahl; i++) {
        unsigned long j = (Schreib + i) % RING;
        Ring[2 * j] = t->links[i];
        Ring[2 * j + 1] = t->rechts[i];
    }
    __sync_synchronize();
    Schreib += t->anzahl;
    Naechst_rtp = rtp + t->anzahl;
    St_rahmen += t->anzahl;
}

/* Rohdaten fuer Untersuchungen (tontest.c): /tmp/ton_proben vor dem Verbinden anlegen */
static FILE *Proben;
static int Proben_n;

static void probe(int art, const unsigned char *d, int n)
{
    unsigned char k[3];
    if (!Proben || Proben_n >= 600) return;
    k[0] = (unsigned char)(n >> 8);
    k[1] = (unsigned char)n;
    k[2] = (unsigned char)art;
    fwrite(k, 1, 3, Proben);
    fwrite(d, 1, (size_t)n, Proben);
    if (++Proben_n == 600) { fclose(Proben); Proben = NULL; fprintf(stderr, "Spiegel: Tonproben in /tmp/ton_proben.bin\n"); }
}

/* ein Paket entschluesseln, dekodieren, in den Ring */
static void dekodieren(const unsigned char *p, int n, double an)
{
    static int codes[3], ncodes;
    int nutz = n - 12, r;
    unsigned rtp = be32(p + 4);
    unsigned char *b, iv[16];
    Dek_ton t;
    St_pakete++;
    probe('a', p, n);
    b = dek_puffer(nutz);
    if (!b) return;
    memcpy(b, p + 12, (size_t)nutz);
    /* AES-128-CBC, IV je Paket neu, nur ganze Bloecke; kurze Pakete (Stille, 4 Byte) bleiben offen */
    memcpy(iv, Iv, 16);
    if (nutz >= 16) {
        unsigned char *k = dek_puffer(nutz);
        if (!k) { dek_puffer_weg(b); return; }
        AES_cbc_encrypt(b, k, (unsigned long)(nutz / 16 * 16), &Schluessel, iv, AES_DECRYPT);
        memcpy(b, k, (size_t)(nutz / 16 * 16));
        dek_puffer_weg(k);
    }
    if ((r = dek_ton_schicken(b, nutz)) >= 0) {
        while ((r = dek_ton_holen(&t)) == 1) {
            ring_schreiben(rtp, &t, an);
            dek_ton_fertig();
            rtp += (unsigned)t.anzahl;
        }
    }
    if (r < 0) {
        int i;
        St_fehler++;
        for (i = 0; i < ncodes && codes[i] != r; i++) ;
        if (i == ncodes && ncodes < 3) {
            codes[ncodes++] = r;
            fprintf(stderr, "Spiegel: Ton: FFmpeg meldet %d\n", r);
        }
    }
}

/* Reihenfolge: Das iPhone schickt jedes Paket dreimal, versetzt (n-1, n, n+1 ...). Im WLAN kann ein Paket seinen
   Vorgaenger ueberholen. Dekodiert wird streng nach RTP-Zeit (der AAC-ELD-Decoder ueberlappt Rahmen); ein
   vorausgeeiltes Paket wartet, bis die Luecke gefuellt ist, hoechstens WARTEZEIT oder bis WARTE_VOLL Pakete warten. */
#define WARTE 12
#define WARTE_VOLL 6
#define WARTEZEIT 0.040
static struct { unsigned rtp; int n; double an; unsigned char d[512]; } Warte[WARTE];
static int Warte_belegt[WARTE];
/* aufgegebene Pakete: kommt eins davon spaeter doch, war die Wartezeit zu kurz */
static unsigned Aufgegeben[16];
static int Aufgegeben_pos;
static volatile unsigned long St_aufgegeben, St_nachzuegler;

static void aufgeben(unsigned von, unsigned bis, unsigned schritt)
{
    for (; (int)(bis - von) > 0; von += schritt) {
        Aufgegeben[Aufgegeben_pos] = von;
        Aufgegeben_pos = (Aufgegeben_pos + 1) % 16;
        St_aufgegeben++;
    }
}

static void paket(const unsigned char *p, int n, double an)
{
    static unsigned erwartet;
    static int gesehen;
    unsigned rtp, schritt = (unsigned)(Spf > 0 ? Spf : 480);
    int diff, i, belegt = 0, aelteste = -1;
    if (Dek_da != 1 || !Schluessel_da || n <= 12 || n > 512) return;
    rtp = be32(p + 4);
    diff = (int)(rtp - erwartet);
    if (!gesehen || diff > (int)schritt * 64 || diff < -RATE) {
        /* Anfang oder Sprung (Pause, Neustart): von hier an */
        gesehen = 1;
        erwartet = rtp;
        memset(Warte_belegt, 0, sizeof Warte_belegt);
        diff = 0;
    }
    if (diff < 0) {                                         /* schon dekodiert oder aufgegeben */
        for (i = 0; i < 16; i++) if (Aufgegeben[i] == rtp) { St_nachzuegler++; Aufgegeben[i] = rtp + 1; break; }
        return;
    }
    if (diff > 0) {
        for (i = 0; i < WARTE; i++) if (Warte_belegt[i] && Warte[i].rtp == rtp) return;   /* Kopie */
        for (i = 0; i < WARTE && Warte_belegt[i]; i++) ;
        if (i < WARTE) {
            Warte[i].rtp = rtp;
            Warte[i].n = n;
            Warte[i].an = an;
            memcpy(Warte[i].d, p, (size_t)n);
            Warte_belegt[i] = 1;
        }
    } else {
        dekodieren(p, n, an);
        erwartet += schritt;
    }
    for (;;) {
        /* Wartende in Reihenfolge abarbeiten */
        int weiter = 0;
        for (i = 0; i < WARTE; i++)
            if (Warte_belegt[i] && (int)(Warte[i].rtp - erwartet) <= 0) {
                if (Warte[i].rtp == erwartet) { dekodieren(Warte[i].d, Warte[i].n, Warte[i].an); erwartet += schritt; }
                Warte_belegt[i] = 0;
                weiter = 1;
            }
        if (weiter) continue;
        /* Luecke aufgeben, wenn zu viele oder zu lange warten: beim aeltesten Wartenden weitermachen */
        belegt = 0;
        aelteste = -1;
        for (i = 0; i < WARTE; i++)
            if (Warte_belegt[i]) {
                belegt++;
                if (aelteste < 0 || (int)(Warte[i].rtp - Warte[aelteste].rtp) < 0) aelteste = i;
            }
        if (aelteste >= 0 && (belegt >= WARTE_VOLL || an - Warte[aelteste].an > WARTEZEIT)) {
            aufgeben(erwartet, Warte[aelteste].rtp, schritt);
            erwartet = Warte[aelteste].rtp;
            continue;
        }
        break;
    }
}

static void *tonfaden(void *x)
{
    unsigned char kopf[3], d[520];
    (void)x;
    for (;;) {
        int n;
        double an;
        if (lies(3, kopf, 3)) break;
        n = kopf[0] << 8 | kopf[1];
        if (n > (int)sizeof d - 1 || lies(3, d, (size_t)n)) break;
        an = jetzt();
        d[n] = 0;
        switch (kopf[2]) {
        case 'k':
            if (n == 32) {
                AES_set_decrypt_key(d, 128, &Schluessel);
                memcpy(Iv, d + 16, 16);
                Schluessel_da = 1;
                if (!Proben && !Proben_n && access("/tmp/ton_proben", F_OK) == 0) Proben = fopen("/tmp/ton_proben.bin", "wb");
                probe('k', d, n);
            }
            break;
        case 'f':
            probe('f', d, n);
            sscanf((char *)d, "ct=%d spf=%d sr=%d", &Ct, &Spf, &Sr);
            fprintf(stderr, "Spiegel: Ton %s\n", (char *)d);
            dekoder_an();
            break;
        case 'u': {
            unsigned long long t0 = 0;
            if (sscanf((char *)d, "%lf %llu", &Uhr_ab, &t0) >= 1) Uhr_da = 1;
            if (t0) Ts0 = t0 / 1e6;
            break;
        }
        case 'v': {
            double db = atof((char *)d);
            Verstaerkung = db <= -100 ? 0.0f : (float)pow(10.0, (db < -30 ? -30 : db > 0 ? 0 : db) / 20.0);
            break;
        }
        case 's':
            probe('s', d, n);
            if (n >= 20) {
                Sync_rtp = be32(d + 4);
                Sync_ntp = ntp_sek(d + 8);
                Sync_da = 1;
            }
            break;
        case 'a':
            if (n >= 12) paket(d, n, an);
            break;
        }
    }
    return NULL;
}

void ton_start(void)
{
    pthread_t z;
    pthread_attr_t a;
    if (fcntl(3, F_GETFD) == -1) return;    /* aelterer Dienst ohne Tonleitung */
    struct sched_param sp;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&z, &a, tonfaden, NULL);
    /* vor dem Dekodierfaden des Bildes: der Ton braucht ~1 % Prozessor, darf aber nie warten */
    sp.sched_priority = sched_get_priority_max(SCHED_OTHER);
    pthread_setschedparam(z, SCHED_OTHER, &sp);
}

/* je gezeigtem Bild: Termin des Bildes (Rahmenzeit, relativ wie im Dekodierfaden) gegen den Zeitpunkt der Anzeige */
void ton_bild(double sender, double gezeigt)
{
    double d;
    if (!Uhr_da || !Ts0) return;
    d = lokal(sender - 1.0 + Ts0, gezeigt) - gezeigt;
    if (d < -0.5 || d > 2) return;
    D_bild = D_da ? D_bild + 0.03 * (d - D_bild) : d;
    D_da = 1;
}

/* eine Zeile fuers Protokoll, 0 wenn es nichts zu sagen gibt */
int ton_zeile(char *z, int n)
{
    static unsigned long p0, r0, f0, l0, s0, v0, u0, fe0, h0, a0, nz0;
    unsigned long p = St_pakete, r = St_rahmen, f = St_fehler, l = St_luecken, s = St_spaet, v = St_voll,
                  u = St_unterlauf, fe = St_fein, h = St_hart, fn = St_fehler_n, a = St_aufgegeben, nz = St_nachzuegler;
    double mittel = fn ? St_fehler_summe / fn : 0, vor = St_vorlauf_min;
    if (p == p0) return 0;
    /* Ton gegen Bild: > 0 heisst, der Ton kommt spaeter als das Bild */
    snprintf(z, (size_t)n, "Ton: %lu Pakete, %lu Rahmen, %lu Fehler, %lu Luecken (%lu aufgegeben, %lu davon spaeter "
             "gekommen), %lu zu spaet, %lu voll, %lu Unterlaeufe, Korrekturen %lu fein %lu hart, Abweichung vom Plan %+.1f ms; "
             "Ton gegen Bild %+.0f ms (Bild %.0f, Ton %.0f ms vor Termin), Vorlauf beim Eintreffen min %.0f ms%s",
             p - p0, r - r0, f - f0, l - l0, a - a0, nz - nz0, s - s0, v - v0, u - u0,
             fe - fe0, h - h0, mittel * 1000, (D_bild - BILDSCHIRM - D_ton) * 1000, (D_bild - BILDSCHIRM) * 1000,
             D_ton * 1000, vor < 1e8 ? vor * 1000 : 0.0, Sync_da ? "" : ", ohne Synchronpakete");
    p0 = p; r0 = r; f0 = f; l0 = l; s0 = s; v0 = v; u0 = u; fe0 = fe; h0 = h; a0 = a; nz0 = nz;
    St_fehler_summe = 0;
    St_fehler_n = 0;
    St_vorlauf_min = 1e9;
    return 1;
}
