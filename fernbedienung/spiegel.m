/* Spiegel: Fenster der Bildschirmsynchronisierung des iPhones (gestartet vom Fernbedienungsdienst).
   Bild: Rahmen von der Standardeingabe [8 Byte Zeit in us][4 Byte Laenge][H.264 Annex B] -> VLC (imem, nur dekodieren)
   -> planar I420/J420 -> AltiVec nach UYVY -> Grafikkarte (GL_YCBCR_422_APPLE, Client-Speicher per DMA):
   Farbumrechnung, Skalierung, runde Ecken und Bildsynchronisierung macht die Radeon.
   Eingabe: Maus, Rollen und Tastatur gehen als Zeilen an "geraet steuern" (Entwickler-Tunnel zum iPhone).
   Aufruf: Spiegel [Adresse des iPhones] */
#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>
#import <OpenGL/gl.h>
#import <OpenGL/glext.h>
#import <OpenGL/OpenGL.h>
#include <vlc/vlc.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <math.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>
#include "uyvy.h"
#include "dekoder.h"
#include "ton.h"

#define VLC_PFAD "/Applications/PowerVLC.app/Contents/MacOS"
#define LEISTE 26.0
#define ABSTAND 6.0
#define PUFFER 3

/* ---------- Bildrahmen von der Standardeingabe ---------- */

static int lies_voll(void *b, size_t n)
{
    unsigned char *p = b;
    while (n) {
        ssize_t r = read(0, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* sichtbare Bildgroesse aus dem SPS (H.264), damit VLC nicht zusaetzlich skaliert; Sicht_voll = voller Wertebereich */
static volatile unsigned Sicht_w, Sicht_h;
static volatile int Sicht_voll;

typedef struct { const unsigned char *p; size_t n, bit; } Bits;
static unsigned b_u(Bits *b, int n)
{
    unsigned v = 0;
    while (n--) {
        size_t i = b->bit >> 3;
        v = v << 1 | (i < b->n ? (b->p[i] >> (7 - (b->bit & 7))) & 1 : 0);
        b->bit++;
    }
    return v;
}
static unsigned b_ue(Bits *b)
{
    int z = 0;
    while (!b_u(b, 1) && z < 32) z++;
    return ((1u << z) - 1) + b_u(b, z);
}
static int b_se(Bits *b)
{
    unsigned v = b_ue(b);
    return v & 1 ? (int)((v + 1) / 2) : -(int)(v / 2);
}

static void sps_lesen(const unsigned char *s, size_t n)
{
    unsigned char r[256];
    size_t i, m = 0;
    Bits b;
    unsigned prof, cf = 1, pw, ph, fmo, cl = 0, cr = 0, ct = 0, cb = 0, k;
    for (i = 1; i < n && m < sizeof r; i++) {
        if (i + 2 < n && s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 3) { r[m++] = 0; if (m < sizeof r) r[m++] = 0; i += 2; continue; }
        r[m++] = s[i];
    }
    b.p = r; b.n = m; b.bit = 0;
    prof = b_u(&b, 8);
    b_u(&b, 16);
    b_ue(&b);
    if (prof == 100 || prof == 110 || prof == 122 || prof == 244 || prof == 44 || prof == 83 || prof == 86 ||
        prof == 118 || prof == 128 || prof == 138 || prof == 139 || prof == 134 || prof == 135) {
        cf = b_ue(&b);
        if (cf == 3) b_u(&b, 1);
        b_ue(&b);
        b_ue(&b);
        b_u(&b, 1);
        if (b_u(&b, 1)) {
            for (k = 0; k < (cf != 3 ? 8u : 12u); k++)
                if (b_u(&b, 1)) {
                    int last = 8, next = 8, j, groesse = k < 6 ? 16 : 64;
                    for (j = 0; j < groesse; j++) {
                        if (next) next = (last + b_se(&b) + 256) % 256;
                        last = next ? next : last;
                    }
                }
        }
    }
    b_ue(&b);
    k = b_ue(&b);
    if (k == 0) b_ue(&b);
    else if (k == 1) {
        unsigned z;
        b_u(&b, 1);
        b_se(&b);
        b_se(&b);
        z = b_ue(&b);
        while (z--) b_se(&b);
    }
    b_ue(&b);
    b_u(&b, 1);
    pw = b_ue(&b);
    ph = b_ue(&b);
    fmo = b_u(&b, 1);
    if (!fmo) b_u(&b, 1);
    b_u(&b, 1);
    if (b_u(&b, 1)) { cl = b_ue(&b); cr = b_ue(&b); ct = b_ue(&b); cb = b_ue(&b); }
    /* VUI: voller Wertebereich (0..255, wie iOS ihn schickt) oder Fernsehbereich (16..235) */
    if (b_u(&b, 1)) {
        if (b_u(&b, 1) && b_u(&b, 8) == 255) { b_u(&b, 16); b_u(&b, 16); }
        if (b_u(&b, 1)) b_u(&b, 1);
        if (b_u(&b, 1)) { b_u(&b, 3); Sicht_voll = (int)b_u(&b, 1); }
        else Sicht_voll = 0;
    } else Sicht_voll = 0;
    {
        unsigned ux = cf == 0 || cf == 3 ? 1 : 2, uy = (cf == 1 ? 2 : 1) * (2 - fmo);
        unsigned w = (pw + 1) * 16 - (cl + cr) * ux, h = (2 - fmo) * (ph + 1) * 16 - (ct + cb) * uy;
        if (w >= 16 && h >= 16 && w <= 4096 && h <= 4096) { Sicht_w = w; Sicht_h = h; }
    }
}

static void sps_suchen(const unsigned char *d, size_t n)
{
    size_t i;
    for (i = 0; i + 4 < n; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if ((d[i + 3] & 0x1f) == 7) { sps_lesen(d + i + 3, n - i - 3); return; }
            i += 2;
        }
}

static int hole(void *data, const char *cookie, int64_t *dts, int64_t *pts, unsigned *flags, size_t *len, void **buf)
{
    unsigned char k[12];
    unsigned long long t = 0;
    unsigned int n;
    int i;
    void *d;
    if (lies_voll(k, 12)) return 1;
    for (i = 0; i < 8; i++) t = t << 8 | k[i];
    n = (unsigned)k[8] << 24 | k[9] << 16 | k[10] << 8 | k[11];
    if (n > (8u << 20)) return 1;
    d = malloc(n);
    if (lies_voll(d, n)) { free(d); return 1; }
    sps_suchen(d, n < 512 ? n : 512);
    /* Zeitstempel = Ankunftszeit: jedes Bild erscheint nach fester, kurzer Zeit statt nach der Uhr des iPhones */
    {
        struct timeval tv;
        static long long start;
        long long jetzt;
        gettimeofday(&tv, NULL);
        jetzt = (long long)tv.tv_sec * 1000000 + tv.tv_usec;
        if (!start) start = jetzt;
        t = (unsigned long long)(jetzt - start + 1000000);
    }
    *dts = *pts = (int64_t)t;
    *flags = 0;
    *len = n;
    *buf = d;
    return 0;
}

static void freigeben(void *data, const char *cookie, size_t n, void *buf) { free(buf); }

/* ---------- Bildspeicher ---------- */

static pthread_mutex_t Sperre = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *Ebene[3];         /* planar von VLC */
static unsigned Ep[3];
static unsigned char *Puf[PUFFER];      /* UYVY fuer die Grafikkarte */
static unsigned Pa, Bw, Bh;
static int Voll, Fertig = -1, Neuestes = -1, Gezeichnet = -1, Angefordert;
static unsigned Generation;
static id Ich;

static unsigned format_cb(void **opaque, char *chroma, unsigned *width, unsigned *height, unsigned *pitches, unsigned *lines)
{
    unsigned w = (Sicht_w && Sicht_w <= *width ? Sicht_w : *width) & ~1u;
    unsigned h = (Sicht_h && Sicht_h <= *height ? Sicht_h : *height) & ~1u, py = (w + 31) & ~31u, i, j;
    /* VLC legt den Fourcc als Zahl ab: auf Big-Endian stehen die Zeichen rueckwaerts */
    Voll = !memcmp(chroma, "J420", 4) || !memcmp(chroma, "024J", 4);
    fprintf(stderr, "Spiegel: Decoder %.4s, %ux%u, sichtbar %ux%u\n", chroma, *width, *height, w, h);
    memcpy(chroma, Voll ? "J420" : "I420", 4);
    pthread_mutex_lock(&Sperre);
    for (i = 0; i < 3; i++) free(Ebene[i]);
    for (i = 0; i < PUFFER; i++) free(Puf[i]);
    Ep[0] = py;
    Ep[1] = Ep[2] = py / 2;
    Ebene[0] = valloc(py * h);
    Ebene[1] = valloc(py / 2 * (h / 2 + 1));
    Ebene[2] = valloc(py / 2 * (h / 2 + 1));
    Pa = py * 2;
    for (i = 0; i < PUFFER; i++) {
        Puf[i] = valloc(Pa * h);
        for (j = 0; j < Pa * h; j += 2) { Puf[i][j] = 0x80; Puf[i][j + 1] = 0x10; }
    }
    Bw = w;
    Bh = h;
    Fertig = Neuestes = Gezeichnet = -1;
    Generation++;
    pthread_mutex_unlock(&Sperre);
    *width = w;
    *height = h;
    pitches[0] = py;
    pitches[1] = pitches[2] = py / 2;
    lines[0] = h;
    lines[1] = lines[2] = h / 2;
    [Ich performSelectorOnMainThread:@selector(formatNeu) withObject:nil waitUntilDone:NO];
    return 1;
}

static void format_ende(void *opaque) { }

static void *sperren(void *opaque, void **planes)
{
    planes[0] = Ebene[0];
    planes[1] = Ebene[1];
    planes[2] = Ebene[2];
    return NULL;
}

/* Bild ist kopiert: in einen freien Puffer nach UYVY umsetzen */
static void entsperren(void *opaque, void *bild, void *const *planes)
{
    int k;
    pthread_mutex_lock(&Sperre);
    for (k = 0; k < PUFFER; k++) if (k != Neuestes && k != Gezeichnet) break;
    pthread_mutex_unlock(&Sperre);
    zu_uyvy(Ebene[0], Ep[0], Ebene[1], Ep[1], Ebene[2], Ep[2], Puf[k], Pa, Bw, Bh, Voll);
    pthread_mutex_lock(&Sperre);
    Fertig = k;
    pthread_mutex_unlock(&Sperre);
}

/* Anzeige: VLC meldet nur "neues Bild" und arbeitet sofort weiter; ein eigener Zeichenfaden zeigt
   im Bildschirmtakt (vsync) das jeweils neueste Bild. So wartet weder VLC noch der Hauptfaden auf vsync. */
static pthread_cond_t Neu_bedingung = PTHREAD_COND_INITIALIZER;
static int Neu_da;

static void zeigen_cb(void *opaque, void *bild)
{
    pthread_mutex_lock(&Sperre);
    if (Fertig >= 0) Neuestes = Fertig;
    Neu_da = 1;
    pthread_cond_signal(&Neu_bedingung);
    pthread_mutex_unlock(&Sperre);
}

static void *zeichenfaden(void *x)
{
    for (;;) {
        NSAutoreleasePool *p;
        pthread_mutex_lock(&Sperre);
        while (!Neu_da) pthread_cond_wait(&Neu_bedingung, &Sperre);
        Neu_da = 0;
        pthread_mutex_unlock(&Sperre);
        p = [[NSAutoreleasePool alloc] init];
        [Ich bildNeu];
        [p release];
    }
    return NULL;
}

/* ---------- Eigener Bildweg: FFmpeg direkt, ohne VLC ----------
   Jeder Rahmen wird sofort dekodiert (kein Eingangsbild geht verloren, sonst steht das Bild bis zum naechsten
   Schluesselbild), und jedes fertige Bild wird sofort gezeigt (keine 50 ms Vorlauf). Kommt der G4 nicht
   hinterher, wird weiter alles dekodiert, aber nur das jeweils neueste Bild umgerechnet und gezeigt. */

static double hochzeit(void);
static int Eigener_weg;
/* Farbumrechnung bleibt auf dem Prozessor (AltiVec, ~1,5 ms): der Shader-Weg ueber GL_ATI_text_fragment_shader
   scheiterte an Eigenheiten des Radeon-9200-Treibers (erstes Programm ohne Texturkoordinaten, Festfunktion
   dazwischen verstellt Einheit 0, DMA aus dem Hauptspeicher gibt Streifen); Probe in gputest.m */

static unsigned zweierpotenz(unsigned n) { unsigned p = 1; while (p < n) p <<= 1; return p; }
static volatile unsigned long St_rahmen, St_bilder, St_gezeigt, St_ausgelassen, St_fehler;
static volatile double St_dek_summe, St_dek_max, St_verzug_summe, St_verzug_max;
static unsigned char *Alt_puf[PUFFER];   /* alte Puffer erst beim naechsten Wechsel freigeben (Grafikkarte liest per DMA) */
static unsigned long Bild_nr, Puf_nr[PUFFER];   /* laufende Nummer des Bildes in jedem Puffer (fuer die Messung) */

static void puffer_neu(unsigned w, unsigned h)
{
    unsigned py = (w + 31) & ~31u, i, j;
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    pthread_mutex_lock(&Sperre);
    for (i = 0; i < PUFFER; i++) { free(Alt_puf[i]); Alt_puf[i] = Puf[i]; }
    Pa = py * 2;
    for (i = 0; i < PUFFER; i++) {
        Puf[i] = valloc(Pa * h);
        for (j = 0; j < Pa * h; j += 2) { Puf[i][j] = 0x80; Puf[i][j + 1] = 0x10; }
    }
    Bw = w;
    Bh = h;
    Fertig = Neuestes = Gezeichnet = -1;
    Generation++;
    pthread_mutex_unlock(&Sperre);
    fprintf(stderr, "Spiegel: eigener Bildweg, %ux%u\n", w, h);
    [Ich performSelectorOnMainThread:@selector(formatNeu) withObject:nil waitUntilDone:NO];
    [pool release];
}

static void bild_zeigen(const Dek_bild *b)
{
    unsigned w = (unsigned)b->breite & ~1u, h = (unsigned)b->hoehe & ~1u;
    int k, voll = b->format == 12 || Sicht_voll;   /* 12 = AV_PIX_FMT_YUVJ420P */
    if (w != Bw || h != Bh || !Puf[0]) puffer_neu(w, h);
    pthread_mutex_lock(&Sperre);
    for (k = 0; k < PUFFER; k++) if (k != Neuestes && k != Gezeichnet) break;
    pthread_mutex_unlock(&Sperre);
    zu_uyvy(b->ebene[0], b->zeile[0], b->ebene[1], b->zeile[1], b->ebene[2], b->zeile[2], Puf[k], (int)Pa, (int)w, (int)h, voll);
    pthread_mutex_lock(&Sperre);
    Neuestes = k;
    Puf_nr[k] = ++Bild_nr;
    Neu_da = 1;
    pthread_cond_signal(&Neu_bedingung);
    pthread_mutex_unlock(&Sperre);
}

/* Rueckstand gegenueber dem iPhone: Aufnahmezeit des iPhones (im Rahmenkopf) gegen die Uhr des iBooks.
   Grundwert = kuerzeste Laufzeit der letzten ~10 s (Uhrengang wird so nachgefuehrt); was darueber liegt,
   ist Verzug durch Netz oder Dekodieren. */
#define RUECKSTAND 0.040
static double Basis = 1e9, Basis_kandidat = 1e9, Basis_seit;

static void basis_lernen(double sender, double lokal)
{
    double d = lokal - sender;
    if (d < Basis) Basis = d;
    if (d < Basis_kandidat) Basis_kandidat = d;
    if (lokal - Basis_seit > 10) { Basis = Basis_kandidat; Basis_kandidat = 1e9; Basis_seit = lokal; }
}

static double verzug(double sender, double jetzt) { return jetzt - sender - Basis; }

/* erster Bildausschnitt eines Rahmens: Nicht-Referenzbild (darf beim Aufholen ganz entfallen)? */
static int nur_nebenbild(const unsigned char *d, int n)
{
    int i;
    for (i = 0; i + 3 < n; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            int t = d[i + 3] & 0x1f;
            if (t == 1) return (d[i + 3] & 0x60) == 0;
            if (t == 5) return 0;
            i += 2;
        }
    return 0;
}

/* ---------- Messung der Gesamtverzoegerung: Eingabe -> geaendertes Bild ----------
   Start: /tmp/spiegel_messen anlegen. Zeile "runden N", dann je Schritt "Name|Befehl|Nachbefehl" (Zeilen fuer
   "geraet steuern"; der Nachbefehl geht 60 ms spaeter, z. B. Taste loslassen). Je Schritt: warten, bis das Bild
   0,35 s ruhig ist, Befehl schicken, Zeit bis zum ersten deutlich geaenderten Bild nehmen (angekommen, dekodiert,
   gezeichnet). Mit dem Uhrabgleich des AirPlay-Dienstes (/tmp/airplay_uhr) auch der Zeitstempel des iPhones.
   Ergebnis ins Protokoll und nach /tmp/spiegel_messung.txt. Datei auf dem iBook per printf schreiben (aus PowerShell
   gepipet kommt ein Byte-Order-Mark mit). */

#define MESS_DATEI "/tmp/spiegel_messen"
#define MESS_STOPP "/tmp/spiegel_messen_stopp"   /* anlegen bricht eine laufende Messung vor dem naechsten Schritt ab */
#define MESS_SCHWELLE 12        /* geaenderte Stichproben (jeder 4. Bildpunkt, Helligkeit um mehr als 32) */
#define GEZ_RING 64

static pthread_mutex_t Mess_sperre = PTHREAD_MUTEX_INITIALIZER;
static volatile int Mess_aktiv, Mess_zustand;    /* 0 beobachten, 1 Eingabe unterwegs, 2 Aenderung gefunden */
static unsigned char *Mess_alt;
static unsigned Mess_w, Mess_h;
static double Mess_ruhig_seit, Mess_an, Mess_dek, Mess_sender;
static unsigned long Mess_nr, Gez_nr[GEZ_RING];
static double Gez_zeit[GEZ_RING];
static unsigned Gez_pos;

/* Zeichenfaden: wann welches Bild an die Grafikkarte ging */
static void gez_merken(unsigned long nr, double t)
{
    static unsigned long zuletzt;
    if (nr == zuletzt) return;
    zuletzt = nr;
    pthread_mutex_lock(&Mess_sperre);
    Gez_nr[Gez_pos % GEZ_RING] = nr;
    Gez_zeit[Gez_pos % GEZ_RING] = t;
    Gez_pos++;
    pthread_mutex_unlock(&Mess_sperre);
}

/* Dekodierfaden: Bild mit dem Bezug vergleichen (obere 8 % mit der Uhr der Statusleiste ausgenommen) */
static void mess_bild(const Dek_bild *b, double an, double fertig, double sender, int gezeigt)
{
    unsigned w = (unsigned)b->breite / 2, h = (unsigned)b->hoehe / 2, oben = h * 8 / 100, n = 0, i, j;
    const unsigned char *y = b->ebene[0];
    size_t z = (size_t)b->zeile[0];
    pthread_mutex_lock(&Mess_sperre);
    if (!Mess_alt || w != Mess_w || h != Mess_h) {
        free(Mess_alt);
        Mess_alt = malloc((size_t)w * h);
        Mess_w = w;
        Mess_h = h;
        n = MESS_SCHWELLE;
    } else
        for (j = oben; j < h; j++) {
            const unsigned char *a = y + 2 * j * z;
            const unsigned char *p = Mess_alt + (size_t)j * w;
            for (i = 0; i < w; i++) { int d = a[2 * i] - p[i]; if (d > 32 || d < -32) n++; }
        }
    if (n >= MESS_SCHWELLE) {
        if (Mess_zustand == 1) {
            Mess_an = an;
            Mess_dek = fertig;
            Mess_sender = sender;
            Mess_nr = Bild_nr + !gezeigt;
            Mess_zustand = 2;
        }
        Mess_ruhig_seit = an;
    }
    if (Mess_zustand != 1)
        for (j = 0; j < h; j++) {
            const unsigned char *a = y + 2 * j * z;
            unsigned char *p = Mess_alt + (size_t)j * w;
            for (i = 0; i < w; i++) p[i] = a[2 * i];
        }
    pthread_mutex_unlock(&Mess_sperre);
}

typedef struct { char name[32], befehl[96], nach[96]; } Mess_schritt;
typedef struct { int schritt; double an, dek, gez, sender, t0; } Mess_wert;

static void mess_senden(const char *z)
{
    NSAutoreleasePool *p = [[NSAutoreleasePool alloc] init];
    [Ich performSelector:@selector(senden:) withObject:[NSString stringWithFormat:@"%s\n", z]];
    [p release];
}

static int doppel_vergleich(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* Kennwerte in ms: Median, Mittel, kleinster, groesster */
static void kennwerte(double *v, int n, double *med, double *mit, double *lo, double *hi)
{
    int i;
    double s = 0;
    qsort(v, (size_t)n, sizeof *v, doppel_vergleich);
    for (i = 0; i < n; i++) s += v[i];
    *med = (n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2) * 1000;
    *mit = s / n * 1000;
    *lo = v[0] * 1000;
    *hi = v[n - 1] * 1000;
}

static void mess_zeile(FILE *aus, const char *f, ...)
{
    char z[512];
    va_list a;
    va_start(a, f);
    vsnprintf(z, sizeof z, f, a);
    va_end(a);
    fprintf(stderr, "Spiegel: %s\n", z);
    if (aus) { fprintf(aus, "%s\n", z); fflush(aus); }
}

static void mess_sitzung(Mess_schritt *s, int ns, int runden)
{
    Mess_wert *w = calloc((size_t)(runden * ns), sizeof *w);
    double *v = calloc((size_t)(runden * ns), sizeof *v), abst = 0, lauf = 0, ts0 = 0, wand_mono;
    int nw = 0, r, i, j, uhr = 0, fehl[16] = { 0 }, abbruch = 0;
    FILE *aus = fopen("/tmp/spiegel_messung.txt", "w"), *f;
    struct timeval tv;
    if (!w || !v) { free(w); free(v); if (aus) fclose(aus); return; }
    srandom((unsigned)time(NULL));
    pthread_mutex_lock(&Mess_sperre);
    Mess_zustand = 0;
    Mess_ruhig_seit = hochzeit();
    pthread_mutex_unlock(&Mess_sperre);
    Mess_aktiv = 1;
    mess_zeile(aus, "Messung: %d Runden, %d Schritte", runden, ns);
    for (r = 0; r < runden && !abbruch; r++)
        for (i = 0; i < ns; i++) {
            double t0, ende, gez = 0;
            int nach = !s[i].nach[0], z = 0;
            if (!access(MESS_STOPP, F_OK)) { unlink(MESS_STOPP); abbruch = 1; mess_zeile(aus, "Messung abgebrochen"); break; }
            /* warten, bis das Bild 0,35 s ruhig ist (hoechstens 4 s), dann zufaellige Lage zum Bildtakt */
            ende = hochzeit() + 4;
            while (hochzeit() < ende) {
                double ruhig;
                pthread_mutex_lock(&Mess_sperre);
                ruhig = Mess_ruhig_seit;
                pthread_mutex_unlock(&Mess_sperre);
                if (hochzeit() - ruhig >= 0.35) break;
                usleep(20000);
            }
            usleep((useconds_t)(random() % 50000));
            pthread_mutex_lock(&Mess_sperre);
            Mess_zustand = 1;
            t0 = hochzeit();
            pthread_mutex_unlock(&Mess_sperre);
            mess_senden(s[i].befehl);
            ende = t0 + 1.5;
            for (;;) {
                double jetzt = hochzeit();
                if (!nach && jetzt - t0 >= 0.06) { mess_senden(s[i].nach); nach = 1; }
                pthread_mutex_lock(&Mess_sperre);
                z = Mess_zustand;
                if (z == 2)
                    for (j = 0; j < GEZ_RING; j++)
                        if (Gez_nr[j] >= Mess_nr && (gez == 0 || Gez_zeit[j] < gez)) gez = Gez_zeit[j];
                pthread_mutex_unlock(&Mess_sperre);
                if ((z == 2 && gez > 0 && nach) || jetzt > ende) break;
                usleep(2000);
            }
            pthread_mutex_lock(&Mess_sperre);
            if (z == 2 && gez > 0) {
                w[nw].schritt = i;
                w[nw].an = Mess_an - t0;
                w[nw].dek = Mess_dek - t0;
                w[nw].gez = gez - t0;
                w[nw].sender = Mess_sender;
                w[nw].t0 = t0;
                nw++;
            } else fehl[i]++;
            Mess_zustand = 0;
            pthread_mutex_unlock(&Mess_sperre);
            if (z == 2 && gez > 0)
                mess_zeile(aus, "%s: angekommen %.1f ms, dekodiert %.1f, gezeichnet %.1f", s[i].name,
                           w[nw - 1].an * 1000, w[nw - 1].dek * 1000, w[nw - 1].gez * 1000);
            else mess_zeile(aus, "%s: keine Aenderung", s[i].name);
        }
    Mess_aktiv = 0;

    /* Zeitstempel des Rahmens (Uhr des iPhones, Sekunden seit Start; NTP-Antworten = Start + 2208988800) ueber den
       NTP-Abstand auf die eigene Uhr. Gemessen 12.11.: liegt ~85 ms NACH der Ankunft, ist also keine Aufnahmezeit,
       sondern ein Wiedergabetermin; taugt nicht zum Aufteilen des iPhone-Anteils. */
    f = fopen("/tmp/airplay_uhr", "r");
    if (f) {
        unsigned long long t = 0;
        uhr = fscanf(f, "%lf %lf %llu", &abst, &lauf, &t) == 3 && t;
        ts0 = t / 1e6;
        fclose(f);
    }
    gettimeofday(&tv, NULL);
    wand_mono = (double)tv.tv_sec + 2208988800.0 + tv.tv_usec / 1e6 - hochzeit();
    if (uhr) {
        mess_zeile(aus, "Uhr des iPhones %+.6f s, Laufzeit der Abfrage %.1f ms", abst, lauf * 1000);
        for (j = 0; j < nw; j++) {
            double auf = w[j].sender - 1.0 + ts0 - abst - wand_mono, d = w[j].t0 + w[j].an - auf;
            if (fabs(d - 2208988800.0) < 1e6) auf += 2208988800.0;
            else if (fabs(d + 2208988800.0) < 1e6) auf -= 2208988800.0;
            d = w[j].t0 + w[j].an - auf;
            if (d < -0.5 || d > 5) { mess_zeile(aus, "Aufnahmezeit nicht bestimmbar (Rahmen %.3f s vor Ankunft)", d); uhr = 0; break; }
            w[j].sender = auf - w[j].t0;   /* ab hier: Aufnahme relativ zur Eingabe */
        }
    }
    for (i = 0; i < ns; i++) {
        double med, mit, lo, hi, m_an, m_dek, m_auf = 0, x;
        int n = 0;
        for (j = 0; j < nw; j++) if (w[j].schritt == i) v[n++] = w[j].gez;
        if (!n) { mess_zeile(aus, "%s: nie geaendert", s[i].name); continue; }
        kennwerte(v, n, &med, &mit, &lo, &hi);
        n = 0;
        for (j = 0; j < nw; j++) if (w[j].schritt == i) v[n++] = w[j].an;
        kennwerte(v, n, &m_an, &x, &x, &x);
        n = 0;
        for (j = 0; j < nw; j++) if (w[j].schritt == i) v[n++] = w[j].dek;
        kennwerte(v, n, &m_dek, &x, &x, &x);
        if (uhr) {
            n = 0;
            for (j = 0; j < nw; j++) if (w[j].schritt == i) v[n++] = w[j].sender;
            kennwerte(v, n, &m_auf, &x, &x, &x);
        }
        mess_zeile(aus, "== %s: %d von %d, gezeichnet nach %.0f ms (Mittel %.0f, %.0f bis %.0f); Mediane: %s%.0f, angekommen %.0f, dekodiert %.0f",
                   s[i].name, n, n + fehl[i], med, mit, lo, hi, uhr ? "Zeitstempel des iPhones " : "", uhr ? m_auf : 0.0, m_an, m_dek);
    }
    if (aus) fclose(aus);
    free(w);
    free(v);
}

static void *messfaden(void *x)
{
    for (;;) {
        Mess_schritt s[16];
        char z[256];
        int ns = 0, runden = 10;
        FILE *f;
        sleep(1);
        if (access(MESS_DATEI, F_OK)) continue;
        f = fopen(MESS_DATEI, "r");
        unlink(MESS_DATEI);
        if (!f) continue;
        while (ns < 16 && fgets(z, sizeof z, f)) {
            char *a, *b;
            z[strcspn(z, "\r\n")] = 0;
            if (strstr(z, "runden ") && sscanf(strstr(z, "runden "), "runden %d", &runden) == 1) continue;
            if (!(a = strchr(z, '|'))) continue;
            *a++ = 0;
            if ((b = strchr(a, '|'))) *b++ = 0;
            snprintf(s[ns].name, sizeof s[ns].name, "%s", z);
            snprintf(s[ns].befehl, sizeof s[ns].befehl, "%s", a);
            snprintf(s[ns].nach, sizeof s[ns].nach, "%s", b ? b : "");
            ns++;
        }
        fclose(f);
        if (ns && runden > 0 && runden <= 100) mess_sitzung(s, ns, runden);
    }
    return NULL;
}

/* Liegt der naechste Rahmen schon in der Leitung? Nur dann lohnt Auslassen: sonst gibt es nichts aufzuholen, und ein
   ausgelassenes Bild stuende bis zum naechsten Rahmen (gemessen 12.11.: 2 von 75 Eingaben +40 bis +100 ms). */
static int rueckstau(void)
{
    int n = 0;
    return ioctl(0, FIONREAD, &n) == 0 && n > 0;
}

static void *dekodierfaden(void *x)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    double zuletzt_gezeigt = 0;
    int entlastet = 0;
    /* Mitschnitt fuer Messungen mit echtem iPhone-Bild: nur wenn /tmp/spiegel_mitschnitt beim Start da ist,
       ab dem ersten Rahmen (mit SPS/PPS und Schluesselbild), hoechstens 60 MB */
    FILE *mit = access("/tmp/spiegel_mitschnitt", F_OK) == 0 ? fopen("/tmp/mitschnitt.h264", "wb") : NULL;
    long mit_bytes = 0;
    if (mit) fprintf(stderr, "Spiegel: Mitschnitt nach /tmp/mitschnitt.h264\n");
    for (;;) {
        unsigned char k[12], *d;
        unsigned n, i;
        unsigned long long us = 0;
        double an, fertig, sender;
        Dek_bild b;
        int r;
        if (lies_voll(k, 12)) break;
        for (i = 0; i < 8; i++) us = us << 8 | k[i];
        n = (unsigned)k[8] << 24 | k[9] << 16 | k[10] << 8 | k[11];
        if (n > (8u << 20)) break;
        d = dek_puffer((int)n);
        if (!d) break;
        if (lies_voll(d, n)) { dek_puffer_weg(d); break; }
        an = hochzeit();
        sender = us / 1e6;
        basis_lernen(sender, an);
        St_rahmen++;
        if (mit) {
            fwrite(d, 1, n, mit);
            mit_bytes += n;
            if (mit_bytes > (60L << 20)) { fclose(mit); mit = NULL; fprintf(stderr, "Spiegel: Mitschnitt voll\n"); }
        }
        sps_suchen(d, n < 512 ? n : 512);
        /* beim Aufholen: Nicht-Referenzbilder gar nicht erst dekodieren (nur wenn schon Neueres wartet),
           bei Referenzbildern den Filter sparen */
        if (verzug(sender, an) > RUECKSTAND) {
            if (nur_nebenbild(d, (int)n) && rueckstau()) { dek_puffer_weg(d); St_ausgelassen++; continue; }
            if (!entlastet) { dek_entlasten(1); entlastet = 1; }
        } else if (entlastet) { dek_entlasten(0); entlastet = 0; }
        if (dek_schicken(d, (int)n) < 0) St_fehler++;
        while ((r = dek_holen(&b)) == 1) {
            fertig = hochzeit();
            St_bilder++;
            St_dek_summe += fertig - an;
            if (fertig - an > St_dek_max) St_dek_max = fertig - an;
            /* hinterher und schon der naechste Rahmen da: nur zeigen, wenn das letzte gezeigte Bild laenger als
               50 ms her ist (Aufholen geht vor) */
            if (verzug(sender, fertig) > RUECKSTAND && fertig - zuletzt_gezeigt < 0.05 && rueckstau()) {
                if (Mess_aktiv) mess_bild(&b, an, fertig, sender, 0);
                dek_fertig();
                continue;
            }
            bild_zeigen(&b);
            if (Mess_aktiv) mess_bild(&b, an, fertig, sender, 1);
            dek_fertig();
            zuletzt_gezeigt = hochzeit();
            ton_bild(sender, zuletzt_gezeigt);
            St_gezeigt++;
            St_verzug_summe += verzug(sender, zuletzt_gezeigt);
            if (verzug(sender, zuletzt_gezeigt) > St_verzug_max) St_verzug_max = verzug(sender, zuletzt_gezeigt);
        }
        if (r < 0) St_fehler++;
    }
    if (mit) fclose(mit);
    [Ich performSelectorOnMainThread:@selector(ende:) withObject:nil waitUntilDone:NO];
    [pool release];
    return NULL;
}

/* ---------- Tastencodes (Mac, positionsgleich) -> HID Seite 7 ---------- */

static unsigned hid_code(unsigned short k, int iso)
{
    static const unsigned char t[128] = {
        0x04, 0x16, 0x07, 0x09, 0x0B, 0x0A, 0x1D, 0x1B, 0x06, 0x19, 0x35, 0x05, 0x14, 0x1A, 0x08, 0x15,
        0x1C, 0x17, 0x1E, 0x1F, 0x20, 0x21, 0x23, 0x22, 0x2E, 0x26, 0x24, 0x2D, 0x25, 0x27, 0x30, 0x12,
        0x18, 0x2F, 0x0C, 0x13, 0x28, 0x0F, 0x0D, 0x34, 0x0E, 0x33, 0x31, 0x36, 0x38, 0x11, 0x10, 0x37,
        0x2B, 0x2C, 0x35, 0x2A, 0x00, 0x29, 0x00, 0xE3, 0xE1, 0x39, 0xE2, 0xE0, 0xE5, 0xE6, 0xE4, 0x00,
        0x00, 0x63, 0x00, 0x55, 0x00, 0x57, 0x00, 0x53, 0x00, 0x00, 0x00, 0x54, 0x58, 0x00, 0x56, 0x00,
        0x00, 0x67, 0x62, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x00, 0x60, 0x61, 0x00, 0x00, 0x00,
        0x3E, 0x3F, 0x40, 0x3C, 0x41, 0x42, 0x00, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x43, 0x00, 0x45,
        0x00, 0x00, 0x00, 0x4A, 0x4B, 0x4C, 0x3D, 0x4D, 0x3B, 0x4E, 0x3A, 0x50, 0x4F, 0x51, 0x52, 0x00 };
    if (k >= 128) return 0;
    if (iso && k == 0x0A) return 0x35;   /* Taste links der 1 */
    if (iso && k == 0x32) return 0x64;   /* Taste <> */
    if (iso && k == 0x2A) return 0x32;   /* Taste # */
    return t[k];
}

/* eigener Trackpad-Treiber: liegen Finger auf? (-1 = Treiber fehlt) */
static int trackpad_beruehrt(void)
{
    int v = 0;
    size_t n = sizeof v;
    if (sysctlbyname("debug.trackpad.beruehrt", &v, &n, NULL, 0)) return -1;
    return v;
}

/* Nachlauf des Treibers: aus, solange das iPhone-Fenster vorn ist (iOS rollt selbst aus) */
static int Schwung_alt = -1;
static void trackpad_schwung(int an)
{
    int v, alt = 0;
    size_t n = sizeof alt;
    if (sysctlbyname("debug.trackpad.schwung", &alt, &n, NULL, 0)) return;
    if (!an) {
        if (Schwung_alt < 0) Schwung_alt = alt;
        v = 0;
    } else {
        if (Schwung_alt < 0) return;
        v = Schwung_alt;
        Schwung_alt = -1;
    }
    sysctlbyname("debug.trackpad.schwung", NULL, NULL, &v, sizeof v);
}

/* ⌘Leertaste gehoert sonst dem Spotlight-Kuerzel des iBooks (Nr. 64): aus, solange das iPhone-Fenster vorn ist */
extern bool CGSIsSymbolicHotKeyEnabled(int k);
extern int CGSSetSymbolicHotKeyEnabled(int k, bool an);
static int Suche_alt = -1;
static void spotlight_kuerzel(int an)
{
    if (!an) {
        if (Suche_alt < 0) Suche_alt = CGSIsSymbolicHotKeyEnabled(64);
        if (Suche_alt) CGSSetSymbolicHotKeyEnabled(64, false);
    } else {
        if (Suche_alt > 0) CGSSetSymbolicHotKeyEnabled(64, true);
        Suche_alt = -1;
    }
}

static void vorn(int ja) { trackpad_schwung(!ja); spotlight_kuerzel(!ja); }

/* Sekunden seit dem Start, gleiche Zeitbasis wie die Zeitstempel von NSEvent */
static double hochzeit(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

/* ---------- Klassen ---------- */

@interface Rahmen : NSWindow
@end
@implementation Rahmen
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

@interface Telefon : NSOpenGLView {
    float breite, hoehe;
    GLuint tex[PUFFER], maske;
    int maske_p;
    int tex_da, maske_r;
    unsigned gen;
    BOOL gedrueckt, rollen, ber_gesehen;
    NSPoint rollpos, rollstart, rp[8];
    NSTimeInterval rz[8];
    int rn;
    NSTimer *rolluhr, *beruehr_uhr;
    double letzte_bewegung;
    unsigned tasten[16], wahl;
    int ntasten, iso;
    BOOL zoomen, zber;
    NSPoint zmitte;
    double zabst;
    NSTimer *zoomuhr, *zber_uhr;
}
- (void)zeichnen;
- (void)alleLoslassen;
- (void)zoomEnde;
- (void)zoomRollen:(NSEvent *)e dy:(double)py;
- (NSPoint)anteil:(NSPoint)p;
@end

@interface LeistenAnsicht : NSView
- (NSRect)vorschau;
@end

@interface Steuerung : NSObject {
@public
    libvlc_instance_t *vlc;
    libvlc_media_player_t *mp;
    Rahmen *fenster;
    NSWindow *leiste;
    Telefon *telefon;
    LeistenAnsicht *leisten_ansicht;
    BOOL leiste_sichtbar, maus_telefon, maus_leiste;
    NSTimer *leisten_uhr;
    int steuer_fd, aus_fd;
    pid_t steuer_pid;
    BOOL bereit;
    int versuche;
    NSString *adresse;
    NSTimer *steueruhr;
    char aus_puf[4096];
    int aus_n;
    /* gemeinsame Zwischenablage */
    NSInteger pb_stand, pb_eigen;
    int pb_nr;
    NSString *mitbringsel;       /* zuletzt vom iPhone kopiert (Datei zum Herausziehen) */
    NSImage *mitbringsel_bild;
}
- (void)senden:(NSString *)zeile;
- (NSString *)ausfuhr:(NSPasteboard *)pb;
- (void)gesendet:(NSString *)z;
- (void)maus:(int)wo drin:(BOOL)drin;
- (void)fensterZiehen;
- (void)formatNeu;
- (void)bildNeu;
- (void)ende:(id)x;
- (void)home:(id)x;
- (void)appUmschalter:(id)x;
- (void)suchen:(id)x;
- (void)menueBauen;
@end

static Steuerung *St;
static void vlc_ende(const libvlc_event_t *e, void *info);

/* ---------- Telefonbild (OpenGL) ---------- */

@implementation Telefon

+ (NSOpenGLPixelFormat *)format
{
    NSOpenGLPixelFormatAttribute a[] = {
        NSOpenGLPFADoubleBuffer, NSOpenGLPFAAccelerated, NSOpenGLPFANoRecovery,
        NSOpenGLPFAColorSize, 24, NSOpenGLPFAAlphaSize, 8, 0 };
    return [[[NSOpenGLPixelFormat alloc] initWithAttributes:a] autorelease];
}

- (id)initWithFrame:(NSRect)r
{
    self = [super initWithFrame:r pixelFormat:[Telefon format]];
    iso = KBGetLayoutType(LMGetKbdType()) == kKeyboardISO;
    [self registerForDraggedTypes:[NSArray arrayWithObjects:NSFilenamesPboardType, NSTIFFPboardType, NSStringPboardType, nil]];
    return self;
}

/* Drag & Drop vom iBook: an der Stelle einsetzen, an der losgelassen wird */
- (NSDragOperation)draggingEntered:(id <NSDraggingInfo>)s
{
    fprintf(stderr, "Spiegel: Ziehen ueber dem Fenster (Steuerung %s)\n", St->bereit ? "bereit" : "nicht bereit");
    return St->bereit ? NSDragOperationCopy : NSDragOperationNone;
}
- (NSDragOperation)draggingUpdated:(id <NSDraggingInfo>)s { return St->bereit ? NSDragOperationCopy : NSDragOperationNone; }

- (BOOL)performDragOperation:(id <NSDraggingInfo>)s
{
    NSPoint a = [self anteil:[self convertPoint:[s draggingLocation] fromView:nil]];
    NSString *z = [St ausfuhr:[s draggingPasteboard]];
    fprintf(stderr, "Spiegel: abgelegt: %s\n", z ? [z UTF8String] : "(nichts Passendes)");
    if (!z) return NO;
    [St senden:[NSString stringWithFormat:@"einsetzen %u %u %@\n", (unsigned)(a.x * 65535 + 0.5), (unsigned)(a.y * 65535 + 0.5), z]];
    [St gesendet:z];
    [[self window] makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    return YES;
}

- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }

- (void)prepareOpenGL
{
    GLint null = 0, eins = 1;
    [[self openGLContext] setValues:&null forParameter:NSOpenGLCPSurfaceOpacity];
    [[self openGLContext] setValues:&eins forParameter:NSOpenGLCPSwapInterval];
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_DITHER);
    glDisable(GL_LIGHTING);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    breite = [self bounds].size.width;
    hoehe = [self bounds].size.height;
}

/* Alphamaske fuer eine Ecke (Radius r, kantengeglaettet), als 2D-Textur in Zweierpotenz-Groesse: eine
   Rechteck-Textur auf Einheit 0 verstellt beim Radeon die Texturkoordinaten des YUV-Shaders */
- (void)maskeBauen:(int)r
{
    unsigned char *m;
    int i, j, p = (int)zweierpotenz((unsigned)r);
    if (r == maske_r && maske) return;
    if (maske) glDeleteTextures(1, &maske);
    m = calloc((size_t)(p * p), 1);
    for (j = 0; j < r; j++)
        for (i = 0; i < r; i++) {
            double dx = i + 0.5 - r, dy = j + 0.5 - r, c = r - sqrt(dx * dx + dy * dy) + 0.5;
            m[j * p + i] = (unsigned char)(c <= 0 ? 0 : c >= 1 ? 255 : c * 255);
        }
    glGenTextures(1, &maske);
    glBindTexture(GL_TEXTURE_2D, maske);
    glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_FALSE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA8, p, p, 0, GL_ALPHA, GL_UNSIGNED_BYTE, m);
    free(m);
    maske_r = r;
    maske_p = p;
}

/* Texturen direkt auf den UYVY-Puffern (Client-Speicher, DMA) */
- (void)texturenBauen
{
    int k;
    if (tex_da) glDeleteTextures(PUFFER, tex);
    glGenTextures(PUFFER, tex);
    for (k = 0; k < PUFFER; k++) {
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex[k]);
        glTextureRangeAPPLE(GL_TEXTURE_RECTANGLE_ARB, Pa * Bh, Puf[k]);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_STORAGE_HINT_APPLE, GL_STORAGE_SHARED_APPLE);
        glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, Pa / 2);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGB8, Bw, Bh, 0, GL_YCBCR_422_APPLE, GL_UNSIGNED_SHORT_8_8_REV_APPLE, Puf[k]);
    }
    tex_da = 1;
}

static void ecke(float x0, float y0, float x1, float y1, float s0, float t0, float s1, float t1)
{
    glTexCoord2f(s0, t0); glVertex2f(x0, y0);
    glTexCoord2f(s1, t0); glVertex2f(x1, y0);
    glTexCoord2f(s1, t1); glVertex2f(x1, y1);
    glTexCoord2f(s0, t1); glVertex2f(x0, y1);
}

- (void)zeichnen
{
    NSOpenGLContext *c = [self openGLContext];
    CGLContextObj cgl = [c CGLContextObj];
    float W, H, r;
    int k;
    unsigned w, h;
    unsigned long nr;
    CGLLockContext(cgl);
    [c makeCurrentContext];
    W = breite;
    H = hoehe;
    if (W < 1 || H < 1) { CGLUnlockContext(cgl); return; }
    pthread_mutex_lock(&Sperre);
    Angefordert = 0;
    if (gen != Generation && Puf[0]) { [self texturenBauen]; gen = Generation; }
    k = gen == Generation ? Neuestes : -1;
    if (k >= 0) Gezeichnet = k;
    nr = k >= 0 ? Puf_nr[k] : 0;
    w = Bw;
    h = Bh;
    pthread_mutex_unlock(&Sperre);
    r = floorf(W * 0.125f);
    [self maskeBauen:(int)r];
    glViewport(0, 0, (GLsizei)W, (GLsizei)H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (k >= 0) {
        glEnable(GL_TEXTURE_RECTANGLE_ARB);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex[k]);
        glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, Pa / 2);
        glTexSubImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, 0, 0, w, h, GL_YCBCR_422_APPLE, GL_UNSIGNED_SHORT_8_8_REV_APPLE, Puf[k]);
        glBegin(GL_QUADS);
        ecke(0, 0, W, H, 0, h, w, 0);
        glEnd();
        glDisable(GL_TEXTURE_RECTANGLE_ARB);
    }
    /* runde Ecken: Ziel mit der Maske multiplizieren (vormultipliziertes Alpha); Maske als 2D-Textur */
    {
        float q = r / (float)maske_p;
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, maske);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ZERO, GL_SRC_ALPHA);
        glBegin(GL_QUADS);
        ecke(0, 0, r, r, 0, 0, q, q);
        ecke(W - r, 0, W, r, q, 0, 0, q);
        ecke(0, H - r, r, H, 0, q, q, 0);
        ecke(W - r, H - r, W, H, q, q, 0, 0);
        glEnd();
        glDisable(GL_BLEND);
        glDisable(GL_TEXTURE_2D);
    }
    [c flushBuffer];
    CGLUnlockContext(cgl);
    if (nr && Mess_aktiv) gez_merken(nr, hochzeit());
}

- (void)drawRect:(NSRect)r { [self zeichnen]; }

- (void)reshape
{
    CGLContextObj cgl = [[self openGLContext] CGLContextObj];
    NSRect b = [self bounds];
    [super reshape];
    CGLLockContext(cgl);
    breite = b.size.width;
    hoehe = b.size.height;
    CGLUnlockContext(cgl);
    [self setNeedsDisplay:YES];
}

- (void)updateTrackingAreas
{
    NSArray *alt = [self trackingAreas];
    unsigned i;
    for (i = 0; i < [alt count]; i++) [self removeTrackingArea:[alt objectAtIndex:i]];
    [self addTrackingArea:[[[NSTrackingArea alloc] initWithRect:[self bounds]
        options:NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect owner:self userInfo:nil] autorelease]];
    [super updateTrackingAreas];
}

- (void)mouseEntered:(NSEvent *)e { [St maus:0 drin:YES]; }
- (void)mouseExited:(NSEvent *)e { [St maus:0 drin:NO]; }

/* Mausposition -> Anteil im Telefonbild (oben = 0) */
- (NSPoint)anteil:(NSPoint)p
{
    NSRect t = [self bounds];
    CGFloat x = p.x / t.size.width, y = 1 - p.y / t.size.height;
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    if (y < 0) y = 0;
    if (y > 1) y = 1;
    return NSMakePoint(x, y);
}

/* Fingerzeile mit der Zeit des Ereignisses (ns), damit iOS gleichmaessige Abstaende sieht */
- (void)finger:(char)art bei:(NSPoint)a zeit:(NSTimeInterval)t
{
    char z[64];
    snprintf(z, sizeof z, "%c %u %u %llu\n", art, (unsigned)(a.x * 65535 + 0.5), (unsigned)(a.y * 65535 + 0.5),
             (unsigned long long)(t * 1e9));
    [St senden:[NSString stringWithUTF8String:z]];
}

- (void)finger:(char)art bei:(NSPoint)a { [self finger:art bei:a zeit:hochzeit()]; }

- (void)mouseDown:(NSEvent *)e
{
    if (zoomen) [self zoomEnde];
    gedrueckt = YES;
    letzte_bewegung = 0;
    [self finger:'d' bei:[self anteil:[self convertPoint:[e locationInWindow] fromView:nil]] zeit:[e timestamp]];
}

- (void)mouseDragged:(NSEvent *)e
{
    if (!gedrueckt || [e timestamp] - letzte_bewegung < 0.008) return;
    letzte_bewegung = [e timestamp];
    [self finger:'m' bei:[self anteil:[self convertPoint:[e locationInWindow] fromView:nil]] zeit:[e timestamp]];
}

- (void)mouseUp:(NSEvent *)e
{
    if (!gedrueckt) return;
    gedrueckt = NO;
    [self finger:'u' bei:[self anteil:[self convertPoint:[e locationInWindow] fromView:nil]] zeit:[e timestamp]];
}

/* Rollen = Wischen mit einem Finger.
   Mit dem eigenen Trackpad-Treiber (sysctl debug.trackpad.beruehrt) wird das Abheben der Finger genau erkannt:
   dann laesst der Finger mit seiner Geschwindigkeit los und iOS rollt selbst aus (Nachlauf des Treibers ist aus).
   Ohne Treiber (Mausrad): nach einer Pause kurz stillhalten und ohne Schwung loslassen. */
- (void)rollenLos:(NSTimer *)t
{
    rolluhr = nil;
    if (rollen) { rollen = NO; [self finger:'u' bei:rollpos]; }
}

- (void)rollenEnde:(NSTimer *)t
{
    rolluhr = nil;
    if (!rollen) return;
    if (ber_gesehen && trackpad_beruehrt() == 1) {
        /* Finger liegen still auf dem Trackpad: Finger auf dem iPhone bleibt liegen */
        rolluhr = [NSTimer scheduledTimerWithTimeInterval:0.12 target:self selector:@selector(rollenEnde:) userInfo:nil repeats:NO];
        return;
    }
    [self finger:'m' bei:rollpos];
    rolluhr = [NSTimer scheduledTimerWithTimeInterval:0.09 target:self selector:@selector(rollenLos:) userInfo:nil repeats:NO];
}

/* Finger vom Trackpad: mit der Geschwindigkeit der letzten ~50 ms loslassen */
- (void)rollenMitSchwung
{
    NSPoint p = rollpos;
    NSTimeInterval tl = rz[(rn - 1) & 7];
    int i, k = -1;
    for (i = 2; i <= 8 && i <= rn; i++) {
        if (tl - rz[(rn - i) & 7] > 0.05) break;
        k = (rn - i) & 7;
    }
    if (k >= 0 && hochzeit() - tl < 0.06 && tl > rz[k]) {
        double dt = tl - rz[k], f = 0.008 / dt;
        p.x += (rp[(rn - 1) & 7].x - rp[k].x) * f;
        p.y += (rp[(rn - 1) & 7].y - rp[k].y) * f;
        if (p.x < 0) p.x = 0;
        if (p.x > 1) p.x = 1;
        if (p.y < 0) p.y = 0;
        if (p.y > 1) p.y = 1;
    }
    [rolluhr invalidate];
    rolluhr = nil;
    rollen = NO;
    [self finger:'u' bei:p zeit:tl + 0.008];
}

- (void)rollBeruehrung:(NSTimer *)t
{
    int b = trackpad_beruehrt();
    if (!rollen) { [t invalidate]; beruehr_uhr = nil; return; }
    if (b == 1) ber_gesehen = YES;
    else if (b == 0 && ber_gesehen) { [t invalidate]; beruehr_uhr = nil; [self rollenMitSchwung]; }
}

/* Ansatzpunkt fuer einen Rollstrich: an der Zeigerposition, aber mit moeglichst viel Weg in Rollrichtung */
- (NSPoint)rollAnsatz:(NSPoint)a dx:(CGFloat)dx dy:(CGFloat)dy
{
    if (fabs(dy) >= fabs(dx)) {
        if (dy < 0 && a.y < 0.78) a.y = 0.78;
        if (dy > 0 && a.y > 0.25) a.y = 0.25;
    } else {
        if (dx < 0 && a.x < 0.8) a.x = 0.8;
        if (dx > 0 && a.x > 0.2) a.x = 0.2;
    }
    return a;
}

/* Zoomen: ⌥ + Zwei-Finger-Scrollen (oder Mausrad). Das Trackpad des iBooks meldet keine zwei Positionen,
   deshalb setzt das Fenster zwei Finger senkrecht um die Mausposition; Finger auf dem Trackpad nach oben
   = auseinander (vergroessern). Ende: Finger vom Trackpad (Treiber) oder 0,15 s ohne Scrollen. */
#define ZOOM_START 0.08     /* halber Fingerabstand beim Ansetzen, Anteil der Bildhoehe */
#define ZOOM_MIN 0.02
#define ZOOM_K 0.006        /* je Pixel Scrollweg: 150 Pixel = Faktor 2,5 */

- (void)zoomSenden:(char)art zeit:(NSTimeInterval)t
{
    char z[96];
    unsigned s = (unsigned)(zabst * 65535 + 0.5);
    if (art == 'd')
        snprintf(z, sizeof z, "zd %u %u %u %llu\n", (unsigned)(zmitte.x * 65535 + 0.5), (unsigned)(zmitte.y * 65535 + 0.5),
                 s, (unsigned long long)(t * 1e9));
    else
        snprintf(z, sizeof z, "z%c %u %llu\n", art, s, (unsigned long long)(t * 1e9));
    [St senden:[NSString stringWithUTF8String:z]];
}

- (void)zoomEnde
{
    [zoomuhr invalidate];
    zoomuhr = nil;
    [zber_uhr invalidate];
    zber_uhr = nil;
    if (!zoomen) return;
    zoomen = NO;
    [self zoomSenden:'u' zeit:hochzeit()];
}

- (void)zoomRuhe:(NSTimer *)t
{
    zoomuhr = nil;
    if (zber && trackpad_beruehrt() == 1) {
        /* Finger liegen still auf: beide Finger auf dem iPhone bleiben liegen */
        zoomuhr = [NSTimer scheduledTimerWithTimeInterval:0.12 target:self selector:@selector(zoomRuhe:) userInfo:nil repeats:NO];
        return;
    }
    [self zoomEnde];
}

- (void)zoomBeruehrung:(NSTimer *)t
{
    int b = trackpad_beruehrt();
    if (!zoomen) return;
    if (b == 1) zber = YES;
    else if (b == 0 && zber) [self zoomEnde];
}

- (void)zoomRollen:(NSEvent *)e dy:(double)py
{
    NSTimeInterval t = [e timestamp];
    double smax;
    if (!zoomen) {
        zmitte = [self anteil:[self convertPoint:[e locationInWindow] fromView:nil]];
        if (zmitte.x < 0.05) zmitte.x = 0.05;
        if (zmitte.x > 0.95) zmitte.x = 0.95;
        if (zmitte.y < 0.15) zmitte.y = 0.15;
        if (zmitte.y > 0.85) zmitte.y = 0.85;
        zabst = ZOOM_START;
        zoomen = YES;
        zber = trackpad_beruehrt() == 1;
        [self zoomSenden:'d' zeit:t];
        if (trackpad_beruehrt() >= 0) {
            zber_uhr = [NSTimer timerWithTimeInterval:0.01 target:self selector:@selector(zoomBeruehrung:) userInfo:nil repeats:YES];
            [[NSRunLoop currentRunLoop] addTimer:zber_uhr forMode:NSRunLoopCommonModes];
        }
    }
    smax = (zmitte.y < 1 - zmitte.y ? zmitte.y : 1 - zmitte.y) - 0.01;
    zabst *= exp(-py * ZOOM_K);
    if (zabst < ZOOM_MIN) zabst = ZOOM_MIN;
    if (zabst > smax) zabst = smax;
    [self zoomSenden:'m' zeit:t];
    [zoomuhr invalidate];
    zoomuhr = [NSTimer scheduledTimerWithTimeInterval:0.15 target:self selector:@selector(zoomRuhe:) userInfo:nil repeats:NO];
}

- (void)scrollWheel:(NSEvent *)e
{
    NSRect b = [self bounds];
    CGEventRef cg = [e CGEvent];
    NSTimeInterval t = [e timestamp];
    /* pixelgenaue Werte des Trackpads (Leopard), sonst Zeilen */
    double py = cg ? (double)CGEventGetIntegerValueField(cg, kCGScrollWheelEventPointDeltaAxis1) : 0;
    double px = cg ? (double)CGEventGetIntegerValueField(cg, kCGScrollWheelEventPointDeltaAxis2) : 0;
    CGFloat dx, dy;
    if (py == 0 && px == 0) { py = [e deltaY] * 8; px = [e deltaX] * 8; }
    if (py == 0 && px == 0) return;
    /* ⌥ beim Ansetzen: Zoomen statt Rollen; ein laufender Strich bleibt, was er ist */
    if (zoomen || (!rollen && ([e modifierFlags] & NSAlternateKeyMask))) { [self zoomRollen:e dy:py]; return; }
    dx = px / b.size.width;
    dy = py / b.size.height;
    if (!rollen) {
        rollstart = rollpos = [self rollAnsatz:[self anteil:[self convertPoint:[e locationInWindow] fromView:nil]] dx:dx dy:dy];
        rollen = YES;
        rn = 0;
        ber_gesehen = trackpad_beruehrt() == 1;
        [self finger:'d' bei:rollpos zeit:t];
        if (trackpad_beruehrt() >= 0 && !beruehr_uhr) {
            beruehr_uhr = [NSTimer timerWithTimeInterval:0.01 target:self selector:@selector(rollBeruehrung:) userInfo:nil repeats:YES];
            [[NSRunLoop currentRunLoop] addTimer:beruehr_uhr forMode:NSRunLoopCommonModes];
        }
    }
    rollpos.x += dx;
    rollpos.y += dy;
    rz[rn & 7] = t;
    rp[rn & 7] = rollpos;
    rn++;
    if (rollpos.x < 0.03 || rollpos.x > 0.97 || rollpos.y < 0.1 || rollpos.y > 0.9) {
        /* Rand erreicht: neu ansetzen */
        if (rollpos.x < 0.03) rollpos.x = 0.03;
        if (rollpos.x > 0.97) rollpos.x = 0.97;
        if (rollpos.y < 0.1) rollpos.y = 0.1;
        if (rollpos.y > 0.9) rollpos.y = 0.9;
        [self finger:'u' bei:rollpos zeit:t];
        rollpos = [self rollAnsatz:rollstart dx:dx dy:dy];
        [self finger:'d' bei:rollpos zeit:t];
        rz[0] = t;
        rp[0] = rollpos;
        rn = 1;
    } else {
        [self finger:'m' bei:rollpos zeit:t];
    }
    [rolluhr invalidate];
    rolluhr = [NSTimer scheduledTimerWithTimeInterval:0.12 target:self selector:@selector(rollenEnde:) userInfo:nil repeats:NO];
}

/* Tastatur: vollstaendige Menge gedrueckter Tasten an das iPhone */
- (void)tastenSenden
{
    NSMutableString *z = [NSMutableString stringWithString:@"k"];
    int i;
    for (i = 0; i < ntasten; i++) [z appendFormat:@" %x", tasten[i]];
    [z appendString:@"\n"];
    [St senden:z];
}

- (void)taste:(unsigned)u an:(BOOL)an
{
    int i;
    for (i = 0; i < ntasten; i++) if (tasten[i] == u) break;
    if (an && i == ntasten && ntasten < 16) tasten[ntasten++] = u;
    else if (!an && i < ntasten) tasten[i] = tasten[--ntasten];
    else return;
    [self tastenSenden];
}

- (void)alleLoslassen
{
    [self zoomEnde];
    wahl = 0;
    if (!ntasten) return;
    ntasten = 0;
    [self tastenSenden];
}

- (void)keyDown:(NSEvent *)e
{
    unsigned u = hid_code([e keyCode], iso);
    if ([e isARepeat] || !u) return;
    if (wahl) [self taste:wahl an:YES];
    [self taste:u an:YES];
}

- (void)keyUp:(NSEvent *)e
{
    unsigned u = hid_code([e keyCode], iso);
    if (u) [self taste:u an:NO];
}

- (void)flagsChanged:(NSEvent *)e
{
    unsigned u = hid_code([e keyCode], iso), f = (unsigned)[e modifierFlags], maske_bit = 0;
    BOOL an;
    if (!u) return;
    if (u == 0xE1 || u == 0xE5) maske_bit = NSShiftKeyMask;
    else if (u == 0xE0 || u == 0xE4) maske_bit = NSControlKeyMask;
    else if (u == 0xE2 || u == 0xE6) maske_bit = NSAlternateKeyMask;
    else if (u == 0xE3) maske_bit = NSCommandKeyMask;
    else if (u == 0x39) maske_bit = NSAlphaShiftKeyMask;
    an = (f & maske_bit) != 0;
    if (u == 0xE3 && !an) {
        /* nach der Befehlstaste kommen nicht immer keyUp: alles loslassen */
        ntasten = 0;
        [self tastenSenden];
        return;
    }
    if (u == 0x39) { [self taste:u an:YES]; [self taste:u an:NO]; return; }
    if (u == 0xE2 || u == 0xE6) {
        /* ⌥ erst mit der naechsten Taste ans iPhone (z. B. @ = ⌥L); allein dient es im Fenster zum Zoomen */
        wahl = an ? u : 0;
        if (!an) [self taste:u an:NO];
        return;
    }
    [self taste:u an:an];
}

@end

/* ---------- Leiste ueber dem Telefon (eigenes Fenster, erscheint beim Darueberfahren) ---------- */

@implementation LeistenAnsicht

- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }

- (NSRect)knopf:(int)i
{
    NSRect l = [self bounds];
    if (i == 0) return NSMakeRect(8, (LEISTE - 14) / 2, 14, 14);
    return NSMakeRect(NSMaxX(l) - 8 - 18 * i - 4 * (i - 1), (LEISTE - 18) / 2, 18, 18);
}

- (void)updateTrackingAreas
{
    NSArray *alt = [self trackingAreas];
    unsigned i;
    for (i = 0; i < [alt count]; i++) [self removeTrackingArea:[alt objectAtIndex:i]];
    [self addTrackingArea:[[[NSTrackingArea alloc] initWithRect:[self bounds]
        options:NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect owner:self userInfo:nil] autorelease]];
    [super updateTrackingAreas];
}

- (void)mouseEntered:(NSEvent *)e { [St maus:1 drin:YES]; }
- (void)mouseExited:(NSEvent *)e { [St maus:1 drin:NO]; }

- (void)drawRect:(NSRect)r
{
    NSRect l = [self bounds], k;
    NSBezierPath *p;
    NSDictionary *a;
    NSString *titel = @"iPhone";
    NSSize ts;
    [[NSColor clearColor] set];
    NSRectFillUsingOperation(l, NSCompositeCopy);
    if (!St->leiste_sichtbar) return;
    [[NSColor colorWithCalibratedWhite:0.16 alpha:0.92] set];
    [[NSBezierPath bezierPathWithRoundedRect:l xRadius:8 yRadius:8] fill];
    [[NSColor colorWithCalibratedRed:1.0 green:0.37 blue:0.34 alpha:1] set];
    [[NSBezierPath bezierPathWithOvalInRect:[self knopf:0]] fill];
    a = [NSDictionary dictionaryWithObjectsAndKeys:[NSFont systemFontOfSize:12], NSFontAttributeName,
         [NSColor colorWithCalibratedWhite:St->bereit ? 0.95 : 0.55 alpha:1], NSForegroundColorAttributeName, nil];
    ts = [titel sizeWithAttributes:a];
    [titel drawAtPoint:NSMakePoint(NSMidX(l) - ts.width / 2, NSMidY(l) - ts.height / 2) withAttributes:a];
    [[NSColor colorWithCalibratedWhite:0.9 alpha:1] set];
    k = NSInsetRect([self knopf:1], 4, 4);
    p = [NSBezierPath bezierPathWithRoundedRect:k xRadius:3 yRadius:3];
    [p setLineWidth:1.5];
    [p stroke];
    k = NSInsetRect([self knopf:2], 4, 4);
    p = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(k.origin.x, k.origin.y, k.size.width - 3, k.size.height - 3) xRadius:2 yRadius:2];
    [p setLineWidth:1.5];
    [p stroke];
    p = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(k.origin.x + 3, k.origin.y + 3, k.size.width - 3, k.size.height - 3) xRadius:2 yRadius:2];
    [p setLineWidth:1.5];
    [p stroke];
    /* zuletzt vom iPhone kopiert: Vorschaubild zum Herausziehen */
    if (St->mitbringsel_bild) {
        NSRect v = [self vorschau];
        NSSize g = [St->mitbringsel_bild size];
        NSRect q = NSMakeRect(0, 0, g.width, g.height);
        if (g.width > g.height) { q.origin.x = (g.width - g.height) / 2; q.size.width = g.height; }
        else { q.origin.y = (g.height - g.width) / 2; q.size.height = g.width; }
        [NSGraphicsContext saveGraphicsState];
        [[NSBezierPath bezierPathWithRoundedRect:v xRadius:3 yRadius:3] addClip];
        [[NSGraphicsContext currentContext] setImageInterpolation:NSImageInterpolationHigh];
        [St->mitbringsel_bild drawInRect:v fromRect:q operation:NSCompositeSourceOver fraction:1.0];
        [NSGraphicsContext restoreGraphicsState];
    }
}

- (NSRect)vorschau { return NSMakeRect(30, (LEISTE - 20) / 2, 20, 20); }

- (NSDragOperation)draggingSourceOperationMaskForLocal:(BOOL)lokal { return NSDragOperationCopy; }

- (void)mouseDown:(NSEvent *)e
{
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    if (!St->leiste_sichtbar) return;
    if (NSPointInRect(p, NSInsetRect([self knopf:0], -4, -4))) [NSApp terminate:nil];
    else if (NSPointInRect(p, [self knopf:1])) [St home:nil];
    else if (NSPointInRect(p, [self knopf:2])) [St appUmschalter:nil];
    else if (St->mitbringsel && NSPointInRect(p, NSInsetRect([self vorschau], -3, -3))) {
        /* vom iPhone kopierte Datei auf den Schreibtisch oder in den Finder ziehen */
        NSPasteboard *pb = [NSPasteboard pasteboardWithName:NSDragPboard];
        NSImage *zug = [[[NSImage alloc] initWithSize:NSMakeSize(64, 64)] autorelease];
        NSSize g = [St->mitbringsel_bild size];
        [pb declareTypes:[NSArray arrayWithObject:NSFilenamesPboardType] owner:nil];
        [pb setPropertyList:[NSArray arrayWithObject:St->mitbringsel] forType:NSFilenamesPboardType];
        [zug lockFocus];
        [[NSGraphicsContext currentContext] setImageInterpolation:NSImageInterpolationHigh];
        [St->mitbringsel_bild drawInRect:NSMakeRect(0, 0, 64, 64) fromRect:NSMakeRect(0, 0, g.width, g.height)
                                operation:NSCompositeSourceOver fraction:0.85];
        [zug unlockFocus];
        [self dragImage:zug at:NSMakePoint(p.x - 32, p.y - 32) offset:NSZeroSize event:e pasteboard:pb source:self slideBack:YES];
    } else [St fensterZiehen];
}

@end

/* ---------- Steuerung ---------- */

@implementation Steuerung

- (void)ende:(id)x { [NSApp terminate:nil]; }

- (void)senden:(NSString *)zeile
{
    const char *z = [zeile UTF8String];
    if (!bereit || steuer_fd < 0) return;
    if (write(steuer_fd, z, strlen(z)) < 0 && errno != EAGAIN) { close(steuer_fd); steuer_fd = -1; bereit = NO; }
}

/* vorher alle Tasten am iPhone loslassen (die gedrueckte Befehlstaste von ⌘1/⌘2 stoert sonst) */
- (void)home:(id)x { [self senden:@"k\nH\n"]; }
- (void)appUmschalter:(id)x { [self senden:@"k\na\n"]; }
- (void)suchen:(id)x { [self senden:@"k\nf\n"]; }

/* Leiste zeigen, solange die Maus ueber Telefon oder Leiste ist */
- (void)leisteAus:(NSTimer *)t
{
    leisten_uhr = nil;
    if (maus_telefon || maus_leiste || !leiste_sichtbar) return;
    leiste_sichtbar = NO;
    [leisten_ansicht setNeedsDisplay:YES];
}

- (void)maus:(int)wo drin:(BOOL)drin
{
    if (wo == 0) maus_telefon = drin; else maus_leiste = drin;
    if (drin && !leiste_sichtbar) { leiste_sichtbar = YES; [leisten_ansicht setNeedsDisplay:YES]; }
    if (!drin && !leisten_uhr)
        leisten_uhr = [NSTimer scheduledTimerWithTimeInterval:0.35 target:self selector:@selector(leisteAus:) userInfo:nil repeats:NO];
}

- (void)fensterZiehen
{
    NSPoint start = [NSEvent mouseLocation], o = [fenster frame].origin;
    for (;;) {
        NSEvent *n = [NSApp nextEventMatchingMask:NSLeftMouseDraggedMask | NSLeftMouseUpMask untilDate:[NSDate distantFuture]
                                           inMode:NSEventTrackingRunLoopMode dequeue:YES];
        NSPoint m = [NSEvent mouseLocation];
        [fenster setFrameOrigin:NSMakePoint(o.x + m.x - start.x, o.y + m.y - start.y)];
        if ([n type] == NSLeftMouseUp) break;
    }
    [fenster saveFrameUsingName:@"iPhone"];
}

/* ---------- Gemeinsame Zwischenablage und Drag & Drop ---------- */

static NSString *uti_fuer(NSString *pfad)
{
    CFStringRef u = UTTypeCreatePreferredIdentifierForTag(kUTTagClassFilenameExtension, (CFStringRef)[[pfad pathExtension] lowercaseString], NULL);
    NSString *s = u ? [NSString stringWithString:(NSString *)u] : @"public.data";
    if (u) CFRelease(u);
    if ([s hasPrefix:@"dyn."]) s = @"public.data";
    return s;
}

/* Inhalt einer Zwischenablage als "uti pfad" (Datei fuer die Steuerung); nil wenn nichts Passendes */
- (NSString *)ausfuhr:(NSPasteboard *)pb
{
    NSArray *typen = [pb types];
    NSString *ordner = @"/tmp/ibook_ablage", *ziel;
    [[NSFileManager defaultManager] createDirectoryAtPath:ordner withIntermediateDirectories:YES attributes:nil error:NULL];
    if ([typen containsObject:NSFilenamesPboardType]) {
        NSArray *dateien = [pb propertyListForType:NSFilenamesPboardType];
        BOOL verzeichnis = NO;
        if ([dateien count]) {
            NSString *d = [dateien objectAtIndex:0];
            if ([[NSFileManager defaultManager] fileExistsAtPath:d isDirectory:&verzeichnis]) {
                if (!verzeichnis) return [NSString stringWithFormat:@"%@ %s", uti_fuer(d), [d fileSystemRepresentation]];
                /* Ordner (auch ueber einen Verweis): als ZIP schicken, die Dateien-App entpackt es */
                {
                    NSString *quelle = [d stringByResolvingSymlinksInPath];
                    NSString *zip = [ordner stringByAppendingPathComponent:
                                     [[[d lastPathComponent] stringByDeletingPathExtension] stringByAppendingPathExtension:@"zip"]];
                    NSTask *t = [[[NSTask alloc] init] autorelease];
                    [[NSFileManager defaultManager] removeItemAtPath:zip error:NULL];
                    [t setLaunchPath:@"/usr/bin/ditto"];
                    [t setArguments:[NSArray arrayWithObjects:@"-c", @"-k", @"--sequesterRsrc", @"--keepParent", quelle, zip, nil]];
                    [t launch];
                    [t waitUntilExit];
                    if ([t terminationStatus] == 0) return [NSString stringWithFormat:@"public.zip-archive %s", [zip fileSystemRepresentation]];
                }
            }
        }
    }
    if ([typen containsObject:NSTIFFPboardType] || [typen containsObject:NSPICTPboardType]) {
        NSImage *b = [[[NSImage alloc] initWithPasteboard:pb] autorelease];
        NSBitmapImageRep *r = b ? [NSBitmapImageRep imageRepWithData:[b TIFFRepresentation]] : nil;
        NSData *png = r ? [r representationUsingType:NSPNGFileType properties:nil] : nil;
        if (png) {
            ziel = [ordner stringByAppendingPathComponent:[NSString stringWithFormat:@"%d.png", ++pb_nr]];
            if ([png writeToFile:ziel atomically:NO]) return [NSString stringWithFormat:@"public.png %@", ziel];
        }
    }
    if ([typen containsObject:NSStringPboardType]) {
        NSString *s = [pb stringForType:NSStringPboardType];
        ziel = [ordner stringByAppendingPathComponent:[NSString stringWithFormat:@"%d.txt", ++pb_nr]];
        if (s && [s writeToFile:ziel atomically:NO encoding:NSUTF8StringEncoding error:NULL])
            return [NSString stringWithFormat:@"public.utf8-plain-text %@", ziel];
    }
    return nil;
}

/* iBook kopiert etwas: ans iPhone (eigene Eintraege nicht zurueckschicken) */
- (void)ablageSehen:(NSTimer *)t
{
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    NSInteger c = [pb changeCount];
    NSString *z;
    if (c == pb_stand) return;
    pb_stand = c;
    if (c == pb_eigen || !bereit) return;
    z = [self ausfuhr:pb];
    fprintf(stderr, "Spiegel: kopiert, ans iPhone: %s\n", z ? [z UTF8String] : "(nichts Passendes)");
    if (z) {
        [self senden:[NSString stringWithFormat:@"setze %@\n", z]];
        [self gesendet:z];
    }
}

/* Rueckmeldung: Leiste kurz mit dem Symbol des Gesendeten zeigen */
- (void)gesendet:(NSString *)z
{
    NSRange r = [z rangeOfString:@" "];
    NSString *pfad = r.location != NSNotFound ? [z substringFromIndex:r.location + 1] : z;
    NSImage *b = [[NSImage alloc] initByReferencingFile:pfad];
    if (!b || ![b isValid]) { [b release]; b = [[[NSWorkspace sharedWorkspace] iconForFile:pfad] retain]; }
    [mitbringsel release];
    mitbringsel = [pfad retain];
    [mitbringsel_bild release];
    mitbringsel_bild = b;
    leiste_sichtbar = YES;
    [leisten_ansicht setNeedsDisplay:YES];
    [leisten_uhr invalidate];
    leisten_uhr = [NSTimer scheduledTimerWithTimeInterval:3.0 target:self selector:@selector(leisteAus:) userInfo:nil repeats:NO];
}

/* Bilder erst beim Einsetzen in TIFF umwandeln (Fotos vom iPhone sind gross) */
- (void)pasteboard:(NSPasteboard *)pb provideDataForType:(NSString *)typ
{
    if ([typ isEqual:NSTIFFPboardType] && mitbringsel) {
        NSImage *b = [[[NSImage alloc] initWithContentsOfFile:mitbringsel] autorelease];
        [pb setData:b ? [b TIFFRepresentation] : [NSData data] forType:NSTIFFPboardType];
    }
}

/* iPhone hat etwas kopiert: in die Zwischenablage des iBooks, Dateien und Bilder auch zum Herausziehen */
- (void)vomIPhone:(NSString *)uti pfad:(NSString *)pfad
{
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    if ([uti hasSuffix:@"text"] || [uti isEqual:@"public.url"]) {
        NSString *s = [NSString stringWithContentsOfFile:pfad encoding:NSUTF8StringEncoding error:NULL];
        if (!s) return;
        [pb declareTypes:[NSArray arrayWithObject:NSStringPboardType] owner:nil];
        [pb setString:s forType:NSStringPboardType];
    } else {
        char name[64];
        time_t jetzt_t = time(NULL);
        NSString *ziel;
        NSImage *bild;
        BOOL ist_bild;
        strftime(name, sizeof name, "iPhone %Y-%m-%d um %H.%M.%S", localtime(&jetzt_t));
        ziel = [[pfad stringByDeletingLastPathComponent] stringByAppendingPathComponent:
                [[NSString stringWithUTF8String:name] stringByAppendingPathExtension:[pfad pathExtension]]];
        /* nur Dateien ohne eigenen Namen (Nummer) bekommen "iPhone Datum" */
        if ([[[pfad lastPathComponent] stringByDeletingPathExtension] rangeOfCharacterFromSet:
                [[NSCharacterSet decimalDigitCharacterSet] invertedSet]].location != NSNotFound) ziel = pfad;
        else if (rename([pfad fileSystemRepresentation], [ziel fileSystemRepresentation])) ziel = pfad;
        ist_bild = [uti isEqual:@"public.png"] || [uti isEqual:@"public.jpeg"] || [uti isEqual:@"public.tiff"] || [uti isEqual:@"com.compuserve.gif"];
        [mitbringsel release];
        mitbringsel = [ziel retain];
        bild = ist_bild ? [[NSImage alloc] initByReferencingFile:ziel] : [[[NSWorkspace sharedWorkspace] iconForFile:ziel] retain];
        [mitbringsel_bild release];
        mitbringsel_bild = bild;
        [pb declareTypes:ist_bild ? [NSArray arrayWithObjects:NSTIFFPboardType, NSFilenamesPboardType, nil]
                                  : [NSArray arrayWithObject:NSFilenamesPboardType] owner:self];
        [pb setPropertyList:[NSArray arrayWithObject:ziel] forType:NSFilenamesPboardType];
        /* Leiste kurz zeigen: dort liegt das Vorschaubild zum Herausziehen */
        leiste_sichtbar = YES;
        [leisten_ansicht setNeedsDisplay:YES];
        [leisten_uhr invalidate];
        leisten_uhr = [NSTimer scheduledTimerWithTimeInterval:4.0 target:self selector:@selector(leisteAus:) userInfo:nil repeats:NO];
    }
    pb_eigen = pb_stand = [pb changeCount];
}

- (void)steuerZeile:(char *)z
{
    if (!strncmp(z, "bereit", 6)) {
        bereit = YES;
        versuche = 0;
        fprintf(stderr, "Spiegel: Steuerung %s\n", z);
        [leisten_ansicht setNeedsDisplay:YES];
    } else if (!strncmp(z, "ablage ", 7)) {
        char *uti = z + 7, *pfad = strchr(uti, ' ');
        if (!pfad) return;
        *pfad++ = 0;
        [self vomIPhone:[NSString stringWithUTF8String:uti] pfad:[NSString stringWithUTF8String:pfad]];
    }
}

/* Meldungen der Steuerung abfragen (nicht blockierend) */
- (void)steuerAusgabe:(NSTimer *)t
{
    char *b, *nl;
    ssize_t r;
    if (aus_fd < 0) return;
    if (aus_n >= (int)sizeof aus_puf - 1) aus_n = 0;
    r = read(aus_fd, aus_puf + aus_n, sizeof aus_puf - 1 - aus_n);
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (r <= 0) {
        /* Steuerung beendet: spaeter neu versuchen */
        [steueruhr invalidate];
        steueruhr = nil;
        close(aus_fd);
        aus_fd = -1;
        bereit = NO;
        if (steuer_fd >= 0) { close(steuer_fd); steuer_fd = -1; }
        if (steuer_pid > 0) { waitpid(steuer_pid, NULL, 0); steuer_pid = 0; }
        fprintf(stderr, "Spiegel: Steuerung beendet\n");
        [leisten_ansicht setNeedsDisplay:YES];
        if (++versuche < 5)
            [[NSRunLoop currentRunLoop] addTimer:[NSTimer timerWithTimeInterval:4.0 target:self selector:@selector(steuerungStarten)
                                                                       userInfo:nil repeats:NO] forMode:NSRunLoopCommonModes];
        return;
    }
    aus_n += (int)r;
    aus_puf[aus_n] = 0;
    b = aus_puf;
    while ((nl = strchr(b, '\n'))) {
        *nl = 0;
        [self steuerZeile:b];
        b = nl + 1;
    }
    aus_n = (int)strlen(b);
    memmove(aus_puf, b, (size_t)aus_n + 1);
}

- (void)steuerungStarten
{
    NSString *pfad = [@"~/tools/fernbedienung/iphone/geraet" stringByExpandingTildeInPath];
    const char *g = [pfad fileSystemRepresentation];
    const char *w = adresse ? [adresse UTF8String] : NULL;
    int ein[2], aus[2];
    pid_t pid;
    if (steuer_pid > 0 || access(g, X_OK) || getenv("SPIEGEL_OHNE_STEUERUNG")) return;
    if (pipe(ein)) return;
    if (pipe(aus)) { close(ein[0]); close(ein[1]); return; }
    pid = fork();
    if (pid == 0) {
        int fd;
        dup2(ein[0], 0);
        dup2(aus[1], 1);
        for (fd = 3; fd < 256; fd++) close(fd);
        if (w) execl(g, g, "-w", w, "steuern", (char *)NULL);
        else execl(g, g, "steuern", (char *)NULL);
        _exit(1);
    }
    close(ein[0]);
    close(aus[1]);
    if (pid < 0) { close(ein[1]); close(aus[0]); return; }
    steuer_pid = pid;
    aus_n = 0;
    steuer_fd = ein[1];
    fcntl(steuer_fd, F_SETFL, O_NONBLOCK);
    fcntl(steuer_fd, F_SETFD, FD_CLOEXEC);
    aus_fd = aus[0];
    fcntl(aus_fd, F_SETFL, O_NONBLOCK);
    fcntl(aus_fd, F_SETFD, FD_CLOEXEC);
    steueruhr = [NSTimer timerWithTimeInterval:0.2 target:self selector:@selector(steuerAusgabe:) userInfo:nil repeats:YES];
    [[NSRunLoop currentRunLoop] addTimer:steueruhr forMode:NSRunLoopCommonModes];
}

- (void)leisteAnlegen
{
    NSRect f = [fenster frame];
    [leiste setFrame:NSMakeRect(f.origin.x, NSMaxY(f) + ABSTAND, f.size.width, LEISTE) display:YES];
}

/* Fenstergroesse passend zum Bild; oben Mitte bleibt stehen */
- (void)groesseFuer:(unsigned)w :(unsigned)h zentrieren:(BOOL)z
{
    NSScreen *bs = [fenster screen] ? [fenster screen] : [NSScreen mainScreen];
    NSRect sicht = [bs visibleFrame], alt = [fenster frame], neu;
    CGFloat s = (sicht.size.height - LEISTE - ABSTAND - 8) / h, tw, th;
    if ((sicht.size.width - 16) / w < s) s = (sicht.size.width - 16) / w;
    if (s > 1) s = 1;
    tw = floor(w * s);
    th = floor(h * s);
    neu.size = NSMakeSize(tw, th);
    if (z) {
        neu.origin.x = floor(NSMidX(sicht) - tw / 2);
        neu.origin.y = NSMaxY(sicht) - th - LEISTE - ABSTAND - 2;
    } else {
        neu.origin.x = floor(NSMidX(alt) - tw / 2);
        neu.origin.y = NSMaxY(alt) - th;
    }
    if (neu.origin.y < sicht.origin.y) neu.origin.y = sicht.origin.y;
    if (NSMaxY(neu) + ABSTAND + LEISTE > NSMaxY(sicht)) neu.origin.y = NSMaxY(sicht) - th - ABSTAND - LEISTE;
    [fenster setFrame:neu display:YES];
    [self leisteAnlegen];
}

- (void)formatNeu
{
    unsigned w, h;
    pthread_mutex_lock(&Sperre);
    w = Bw;
    h = Bh;
    pthread_mutex_unlock(&Sperre);
    if (w && h) [self groesseFuer:w :h zentrieren:NO];
}

- (void)bildNeu { [telefon zeichnen]; }

/* alle 5 s: dekodierte, gezeigte und verworfene Bilder ins Protokoll */
- (void)statistik:(NSTimer *)t
{
    static int dek, gez, verl;
    libvlc_media_t *m;
    libvlc_media_stats_t s;
    if (Eigener_weg) {
        /* eigener Bildweg: Rahmen, Bilder, gezeigt, ausgelassen (Nicht-Referenz beim Aufholen), Zeiten */
        static unsigned long r0, b0, g0, a0, f0, ruhig;
        unsigned long r = St_rahmen - r0, b = St_bilder - b0, g = St_gezeigt - g0, a = St_ausgelassen - a0, f = St_fehler - f0;
        if (r && (g < b || a || f || ++ruhig >= 6)) {
            fprintf(stderr, "Spiegel: 5 s: %lu Rahmen, %lu dekodiert, %lu gezeigt, %lu ausgelassen%s, Dekodieren %.0f ms (max %.0f), "
                    "Verzug ueber Grundlaufzeit %.0f ms (max %.0f)\n", r, b, g, a, f ? ", Fehler" : "",
                    b ? St_dek_summe * 1000 / b : 0.0, St_dek_max * 1000, g ? St_verzug_summe * 1000 / g : 0.0, St_verzug_max * 1000);
            ruhig = 0;
        }
        r0 = St_rahmen; b0 = St_bilder; g0 = St_gezeigt; a0 = St_ausgelassen; f0 = St_fehler;
        St_dek_summe = St_dek_max = St_verzug_summe = St_verzug_max = 0;
        {
            /* Ton: in der ersten Minute alle 5 s, danach alle 30 s */
            static int takt, zeilen;
            char z[400];
            if ((zeilen < 12 || ++takt >= 6) && ton_zeile(z, sizeof z)) {
                fprintf(stderr, "Spiegel: %s\n", z);
                zeilen++;
                takt = 0;
            }
        }
        return;
    }
    m = mp ? libvlc_media_player_get_media(mp) : NULL;
    if (!m) return;
    if (libvlc_media_get_stats(m, &s)) {
        /* nur melden, wenn Bilder verworfen wurden */
        if (s.i_lost_pictures != verl)
            fprintf(stderr, "Spiegel: 5 s: %d dekodiert, %d gezeigt, %d verworfen\n",
                    s.i_decoded_video - dek, s.i_displayed_pictures - gez, s.i_lost_pictures - verl);
        dek = s.i_decoded_video;
        gez = s.i_displayed_pictures;
        verl = s.i_lost_pictures;
    }
    libvlc_media_release(m);
}

- (void)windowDidResignKey:(NSNotification *)n { [telefon alleLoslassen]; vorn(0); }
- (void)windowDidBecomeKey:(NSNotification *)n { vorn(1); }
- (void)windowDidMove:(NSNotification *)n { [self leisteAnlegen]; }

- (void)menueBauen
{
    NSMenu *haupt = [[[NSMenu alloc] init] autorelease], *app = [[[NSMenu alloc] initWithTitle:@"iPhone"] autorelease];
    NSMenu *st = [[[NSMenu alloc] initWithTitle:@"Steuerung"] autorelease];
    NSMenuItem *i;
    [app addItemWithTitle:@"iPhone ausblenden" action:@selector(hide:) keyEquivalent:@"h"];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItemWithTitle:@"iPhone beenden" action:@selector(terminate:) keyEquivalent:@"q"];
    i = [haupt addItemWithTitle:@"iPhone" action:nil keyEquivalent:@""];
    [i setSubmenu:app];
    [[st addItemWithTitle:@"Home" action:@selector(home:) keyEquivalent:@"1"] setTarget:self];
    [[st addItemWithTitle:@"App-Umschalter" action:@selector(appUmschalter:) keyEquivalent:@"2"] setTarget:self];
    [[st addItemWithTitle:@"Suchen" action:@selector(suchen:) keyEquivalent:@" "] setTarget:self];
    i = [haupt addItemWithTitle:@"Steuerung" action:nil keyEquivalent:@""];
    [i setSubmenu:st];
    [NSApp setMainMenu:haupt];
    /* nur zusammen mit dem Aufruf vor [NSApp run] richtig: Apfel, „iPhone“, „Steuerung“ (per Probe geprueft) */
    if ([NSApp respondsToSelector:@selector(setAppleMenu:)]) [NSApp performSelector:@selector(setAppleMenu:) withObject:app];
}

- (void)symbolZeichnen
{
    NSImage *b = [[[NSImage alloc] initWithSize:NSMakeSize(128, 128)] autorelease];
    [b lockFocus];
    [[NSColor colorWithCalibratedWhite:0.22 alpha:1] set];
    [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(34, 4, 60, 120) xRadius:14 yRadius:14] fill];
    [[NSColor colorWithCalibratedRed:0.16 green:0.42 blue:0.86 alpha:1] set];
    [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(38, 8, 52, 112) xRadius:10 yRadius:10] fill];
    [[NSColor blackColor] set];
    [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(56, 110, 16, 6) xRadius:3 yRadius:3] fill];
    [b unlockFocus];
    [NSApp setApplicationIconImage:b];
}

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    const char *argumente[] = {
        "--no-video-title-show", "--no-audio", "--no-osd", "--no-snapshot-preview",
        "--no-sub-autodetect-file", "--avcodec-hw=none",
        /* nie Bilder im Decoder auslassen: fast alle iPhone-Bilder sind Referenzbilder */
        "--no-avcodec-hurry-up", "--avcodec-skip-frame=0", "--avcodec-skip-idct=0",
        /* ein Decoder-Faden ohne Rahmenverzoegerung */
        "--avcodec-threads=1",
        /* H.264-Abkuerzungen ohne sichtbaren Unterschied */
        "--avcodec-fast",
    };
    char o[64];
    libvlc_media_t *m;
    libvlc_event_manager_t *em;
    ProcessSerialNumber psn = { 0, kCurrentProcess };

    /* kein TransformProcessType und Menue schon in main: sonst legt Leopard ein zweites Programm-Menue an
       (doppeltes „iPhone“ oder zweites Apfel-Logo, mit einer Probe geprueft) */
    [self symbolZeichnen];
    steuer_fd = aus_fd = -1;

    fenster = [[Rahmen alloc] initWithContentRect:NSMakeRect(0, 0, 330, 700) styleMask:NSBorderlessWindowMask
                                          backing:NSBackingStoreBuffered defer:NO];
    [fenster setOpaque:NO];
    [fenster setBackgroundColor:[NSColor clearColor]];
    [fenster setHasShadow:NO];
    [fenster setTitle:@"iPhone"];
    [fenster setDelegate:(id)self];
    telefon = [[Telefon alloc] initWithFrame:NSMakeRect(0, 0, 330, 700)];
    [telefon setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [fenster setContentView:telefon];

    leiste = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 330, LEISTE) styleMask:NSBorderlessWindowMask
                                           backing:NSBackingStoreBuffered defer:NO];
    [leiste setOpaque:NO];
    [leiste setBackgroundColor:[NSColor clearColor]];
    [leiste setHasShadow:NO];
    leisten_ansicht = [[LeistenAnsicht alloc] initWithFrame:NSMakeRect(0, 0, 330, LEISTE)];
    [leisten_ansicht setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [leiste setContentView:leisten_ansicht];

    [self groesseFuer:354 :768 zentrieren:YES];
    if ([fenster setFrameUsingName:@"iPhone"]) [self groesseFuer:354 :768 zentrieren:NO];
    [fenster makeKeyAndOrderFront:nil];
    [fenster addChildWindow:leiste ordered:NSWindowAbove];
    [self leisteAnlegen];
    [fenster makeFirstResponder:telefon];
    [NSApp activateIgnoringOtherApps:YES];
    SetFrontProcess(&psn);

    {
        char fehler[200] = "abgeschaltet";
        if (!getenv("SPIEGEL_VLC") && !dek_an(VLC_PFAD "/plugins/libavcodec_plugin.dylib", fehler, sizeof fehler))
            Eigener_weg = 1;
        else
            fprintf(stderr, "Spiegel: eigener Bildweg nicht moeglich (%s), nehme VLC\n", fehler);
    }
    {
        pthread_t z;
        pthread_attr_t a;
        struct sched_param sp;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_create(&z, &a, zeichenfaden, NULL);
        /* Zeichenfaden etwas bevorzugen */
        sp.sched_priority = sched_get_priority_max(SCHED_OTHER);
        pthread_setschedparam(z, SCHED_OTHER, &sp);
        if (Eigener_weg) {
            pthread_create(&z, &a, dekodierfaden, NULL);
            pthread_create(&z, &a, messfaden, NULL);
            ton_start();
        }
    }
    if (Eigener_weg) goto weiter;

    setenv("VLC_PLUGIN_PATH", VLC_PFAD "/plugins", 1);
    setenv("VLC_DATA_PATH", VLC_PFAD "/share", 1);
    vlc = libvlc_new(sizeof argumente / sizeof argumente[0], argumente);
    if (!vlc) { [NSApp terminate:nil]; return; }
    m = libvlc_media_new_location(vlc, "imem://");
    snprintf(o, sizeof o, ":imem-get=%lld", (long long)(intptr_t)hole);
    libvlc_media_add_option(m, o);
    snprintf(o, sizeof o, ":imem-release=%lld", (long long)(intptr_t)freigeben);
    libvlc_media_add_option(m, o);
    libvlc_media_add_option(m, ":imem-data=0");
    libvlc_media_add_option(m, ":imem-cat=2");
    libvlc_media_add_option(m, ":imem-codec=h264");
    libvlc_media_add_option(m, ":imem-id=0");
    libvlc_media_add_option(m, ":imem-caching=50");
    mp = libvlc_media_player_new_from_media(m);
    libvlc_media_release(m);
    libvlc_video_set_callbacks(mp, sperren, entsperren, zeigen_cb, NULL);
    libvlc_video_set_format_callbacks(mp, format_cb, format_ende);
    em = libvlc_media_player_event_manager(mp);
    libvlc_event_attach(em, libvlc_MediaPlayerEndReached, vlc_ende, self);
    libvlc_event_attach(em, libvlc_MediaPlayerEncounteredError, vlc_ende, self);
    libvlc_media_player_play(mp);
weiter:
    [self steuerungStarten];
    /* gemeinsame Zwischenablage: was beim Start schon drin ist, bleibt auf dem iBook */
    pb_stand = pb_eigen = [[NSPasteboard generalPasteboard] changeCount];
    [[NSRunLoop currentRunLoop] addTimer:[NSTimer timerWithTimeInterval:0.5 target:self selector:@selector(ablageSehen:)
                                                               userInfo:nil repeats:YES] forMode:NSRunLoopCommonModes];
    [NSTimer scheduledTimerWithTimeInterval:5.0 target:self selector:@selector(statistik:) userInfo:nil repeats:YES];
}

- (void)applicationWillTerminate:(NSNotification *)n
{
    vorn(0);
    if (steuer_fd >= 0) {
        fcntl(steuer_fd, F_SETFL, 0);
        write(steuer_fd, "ende\n", 5);
        close(steuer_fd);
        steuer_fd = -1;
    }
    close(0);   /* der Lesefaden von VLC bekommt Dateiende */
    if (mp) { libvlc_media_player_stop(mp); libvlc_media_player_release(mp); }
    if (vlc) libvlc_release(vlc);
}

@end

static void vlc_ende(const libvlc_event_t *e, void *info)
{
    [(id)info performSelectorOnMainThread:@selector(ende:) withObject:nil waitUntilDone:NO];
}

/* auch bei kill oder Absturz Trackpad-Nachlauf und Spotlight-Kuerzel wiederherstellen */
static void beenden_signal(int s)
{
    vorn(0);
    if (s == SIGTERM || s == SIGINT) _exit(0);
    signal(s, SIG_DFL);
    raise(s);
}

int main(int argc, char **argv)
{
    NSAutoreleasePool *p = [[NSAutoreleasePool alloc] init];
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, beenden_signal);
    signal(SIGINT, beenden_signal);
    signal(SIGSEGV, beenden_signal);
    signal(SIGBUS, beenden_signal);
    signal(SIGABRT, beenden_signal);
    signal(SIGILL, beenden_signal);
    /* Cocoa in den Mehrfaden-Betrieb schalten (gezeichnet wird im VLC-Faden) */
    [NSThread detachNewThreadSelector:@selector(class) toTarget:[NSObject class] withObject:nil];
    [NSApplication sharedApplication];
    St = [[Steuerung alloc] init];
    Ich = St;
    if (argc > 1) St->adresse = [[NSString alloc] initWithUTF8String:argv[1]];
    [NSApp setDelegate:St];
    [St menueBauen];
    [NSApp run];
    [p release];
    return 0;
}
