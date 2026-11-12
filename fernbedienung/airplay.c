/* AirPlay-Empfaenger: Bildschirmsynchronisierung vom iPhone aufs iBook.
   Ablauf nach dem Vorbild von UxPlay (GPL-3): RTSP, Legacy-Pairing (Ed25519, X25519, AES-CTR),
   FairPlay (playfair, hier Big-Endian-fest), SETUP mit NTP-Zeitabgleich, Videostrom (H.264 in
   AES-CTR) -> Anzeigeprogramm Spiegel.app ueber eine Pipe mit Zeitstempeln. Ein Faden, CFRunLoop. */
#include <CoreFoundation/CoreFoundation.h>
#include <openssl/aes.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include "playfair/playfair.h"
#include "fairplay_antworten.h"
#include "airplay.h"

#define FEATURES 0x5A7FFEE6UL          /* Bit 27: Legacy-Pairing; kein AirPlay 2 */
#define AP_VERSION "220.68"
#define AP_MODELL "AppleTV3,2"
#define BREITE 1024
/* Hoehe so, dass das iPhone-Bild 1:1 ins Fenster passt (Bildschirm minus Menueleiste, Dock und Leiste) */
#define HOEHE 640
/* mit VLC schaffte der G4 bei Bewegung nur 30. Eigener Bildweg (FFmpeg direkt, 11-15 ms je Bild beim Scrollen):
   60 lief im echten Betrieb bei schnellem Scrollen ueber (Rueckstand bis 222 ms); bei 50 schickte iOS nur ~35
   (kein Teiler von 120 Hz); 40 = jedes dritte Bild des 120-Hz-iPhones */
#define MAX_FPS 40
#define MAX_RTSP 6
#define RUECKSTAU_MAX (3 << 20)

static ApMeld Meld;
static int Laut;
static unsigned char Ed_sk[64], Ed_pk[32], Mac[6];
static char Name[64], Kennung[40], MacText[20], RaopName[96], Anzeige_pfad[1024];

/* ---------- kleine Helfer ---------- */
static void zufall(unsigned char *b, int n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, b, n) != n) { Meld("AirPlay: kein Zufall"); exit(1); }
    close(fd);
}

static unsigned int le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static unsigned long long le64(const unsigned char *p) { return le32(p) | (unsigned long long)le32(p + 4) << 32; }
static unsigned int be32(const unsigned char *p) { return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static unsigned int be16(const unsigned char *p) { return p[0] << 8 | p[1]; }
static float lef(const unsigned char *p) { union { unsigned int u; float f; } x; x.u = le32(p); return x.f; }

static void sha16(unsigned char aus[16], const void *a, int na, const void *b, int nb)
{
    unsigned char h[64];
    crypto_sha512_ctx c;
    crypto_sha512_init(&c);
    crypto_sha512_update(&c, a, na);
    crypto_sha512_update(&c, b, nb);
    crypto_sha512_final(&c, h);
    memcpy(aus, h, 16);
}

/* AES-128-CTR als durchgehender Schluesselstrom, Zaehler 128 Bit Big-Endian */
typedef struct { AES_KEY k; unsigned char z[16], s[16]; int pos; } Ctr;

static void ctr_init(Ctr *c, const unsigned char key[16], const unsigned char iv[16])
{
    AES_set_encrypt_key(key, 128, &c->k);
    memcpy(c->z, iv, 16);
    c->pos = 16;
}

static void ctr_xor(Ctr *c, unsigned char *d, long n)
{
    long i = 0;
    while (i < n) {
        if (c->pos == 16) {
            int j;
            AES_encrypt(c->z, c->s, &c->k);
            for (j = 15; j >= 0 && ++c->z[j] == 0; j--) ;
            c->pos = 0;
        }
        d[i++] ^= c->s[c->pos++];
    }
}

/* ---------- TXT fuer Bonjour ---------- */
static int txt(unsigned char *aus, int max, int *n, const char *k, const char *v)
{
    int l = (int)(strlen(k) + 1 + strlen(v));
    if (l > 255 || *n + 1 + l > max) return -1;
    aus[(*n)++] = (unsigned char)l;
    memcpy(aus + *n, k, strlen(k)); *n += (int)strlen(k);
    aus[(*n)++] = '=';
    memcpy(aus + *n, v, strlen(v)); *n += (int)strlen(v);
    return 0;
}

static void pk_hex(char *o)
{
    int i;
    for (i = 0; i < 32; i++) sprintf(o + 2 * i, "%02x", Ed_pk[i]);
}

int airplay_txt(unsigned char *aus, int max)
{
    char f[32], pk[70];
    int n = 0;
    snprintf(f, sizeof f, "0x%lX,0x0", (unsigned long)FEATURES);
    pk_hex(pk);
    txt(aus, max, &n, "deviceid", MacText);
    txt(aus, max, &n, "features", f);
    txt(aus, max, &n, "pw", "false");
    txt(aus, max, &n, "flags", "0x4");
    txt(aus, max, &n, "model", AP_MODELL);
    txt(aus, max, &n, "pk", pk);
    txt(aus, max, &n, "pi", Kennung);
    txt(aus, max, &n, "srcvers", AP_VERSION);
    txt(aus, max, &n, "vv", "2");
    return n;
}

int raop_txt(unsigned char *aus, int max)
{
    char f[32], pk[70];
    int n = 0;
    snprintf(f, sizeof f, "0x%lX,0x0", (unsigned long)FEATURES);
    pk_hex(pk);
    txt(aus, max, &n, "ch", "2");
    txt(aus, max, &n, "cn", "0,1,2,3");
    txt(aus, max, &n, "da", "true");
    txt(aus, max, &n, "et", "0,3,5");
    txt(aus, max, &n, "vv", "2");
    txt(aus, max, &n, "ft", f);
    txt(aus, max, &n, "am", AP_MODELL);
    txt(aus, max, &n, "md", "0,1,2");
    txt(aus, max, &n, "rhd", "5.6.0.0");
    txt(aus, max, &n, "pw", "false");
    txt(aus, max, &n, "sf", "0x4");
    txt(aus, max, &n, "sr", "44100");
    txt(aus, max, &n, "ss", "16");
    txt(aus, max, &n, "sv", "false");
    txt(aus, max, &n, "tp", "UDP");
    txt(aus, max, &n, "txtvers", "1");
    txt(aus, max, &n, "vs", AP_VERSION);
    txt(aus, max, &n, "vn", "65537");
    txt(aus, max, &n, "pk", pk);
    return n;
}

const char *raop_name(void) { return RaopName; }

/* ---------- Plists ---------- */
static CFDictionaryRef plist_lesen(const unsigned char *d, int n)
{
    CFDataRef daten;
    CFPropertyListRef p;
    if (n <= 0) return NULL;
    daten = CFDataCreate(NULL, d, n);
    p = CFPropertyListCreateFromXMLData(NULL, daten, kCFPropertyListImmutable, NULL);
    CFRelease(daten);
    if (p && CFGetTypeID(p) != CFDictionaryGetTypeID()) { CFRelease(p); p = NULL; }
    return p;
}

static unsigned char *plist_binaer(CFPropertyListRef p, int *n)
{
    CFWriteStreamRef s = CFWriteStreamCreateWithAllocatedBuffers(NULL, NULL);
    CFDataRef d;
    unsigned char *aus = NULL;
    *n = 0;
    CFWriteStreamOpen(s);
    CFPropertyListWriteToStream(p, s, kCFPropertyListBinaryFormat_v1_0, NULL);
    d = CFWriteStreamCopyProperty(s, kCFStreamPropertyDataWritten);
    CFWriteStreamClose(s);
    CFRelease(s);
    if (d) {
        *n = (int)CFDataGetLength(d);
        aus = malloc(*n);
        CFDataGetBytes(d, CFRangeMake(0, *n), aus);
        CFRelease(d);
    }
    return aus;
}

static void setz(CFMutableDictionaryRef d, const char *k, CFTypeRef v)
{
    CFStringRef s = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8);
    CFDictionarySetValue(d, s, v);
    CFRelease(s);
    CFRelease(v);
}

static CFNumberRef zahl(long long v) { return CFNumberCreate(NULL, kCFNumberSInt64Type, &v); }
static CFNumberRef kommazahl(double v) { return CFNumberCreate(NULL, kCFNumberDoubleType, &v); }
static CFStringRef text(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }
static CFBooleanRef wahr(int b) { return CFRetain(b ? kCFBooleanTrue : kCFBooleanFalse); }
static CFMutableDictionaryRef woerterbuch(void)
{
    return CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}

static CFTypeRef hole(CFDictionaryRef d, const char *k)
{
    CFStringRef s = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8);
    CFTypeRef v = d ? CFDictionaryGetValue(d, s) : NULL;
    CFRelease(s);
    return v;
}

static int hole_zahl(CFDictionaryRef d, const char *k, long long *aus)
{
    CFTypeRef v = hole(d, k);
    if (!v || CFGetTypeID(v) != CFNumberGetTypeID()) return 0;
    return CFNumberGetValue(v, kCFNumberSInt64Type, aus) ? 1 : 1;
}

static const unsigned char *hole_daten(CFDictionaryRef d, const char *k, int *n)
{
    CFTypeRef v = hole(d, k);
    if (!v || CFGetTypeID(v) != CFDataGetTypeID()) return NULL;
    *n = (int)CFDataGetLength(v);
    return CFDataGetBytePtr(v);
}

/* ---------- Sitzung: Zeitabgleich, Ton, Video, Anzeige ---------- */
typedef struct Stueck { struct Stueck *nach; int n, pos; unsigned char d[1]; } Stueck;

static struct {
    unsigned char aeskey[16], aesiv[16];
    int aeskey_da;
    /* NTP */
    int ntp_fd; CFSocketRef ntp_s; CFRunLoopTimerRef ntp_uhr; struct sockaddr_in ntp_ziel; int ntp_port;
    double uhr_ab; int uhr_da;
    /* Ton: Pakete gehen verschluesselt ueber eine zweite Pipe (fd 3) an die Anzeige, die dekodiert und spielt */
    int ton_fd[2]; CFSocketRef ton_s[2]; int ton_port[2];
    int ton_pipe; char ton_format[96]; double ton_vol; int ton_vol_da; long ton_pakete, ton_verloren;
    /* Video */
    int dl_fd; CFSocketRef dl_s; int daten_port;
    int dv_fd; CFSocketRef dv_s; unsigned char *db; int dn, dcap;
    Ctr video; int video_da;
    unsigned char *spspps; int nspspps, spspps_offen;
    unsigned long long ts0; int ts0_da;
    /* Anzeige */
    pid_t anz_pid; int anz_fd; Stueck *kopf, *ende; long rueckstau; int bis_idr; CFRunLoopTimerRef anz_uhr;
    long bilder, verworfen;
} S = { .ntp_fd = -1, .ton_fd = { -1, -1 }, .ton_pipe = -1, .dl_fd = -1, .dv_fd = -1, .anz_fd = -1 };

static CFSocketRef quelle(int fd, CFSocketCallBack f, void *info)
{
    CFSocketContext c = { 0, NULL, NULL, NULL, NULL };
    CFSocketRef s;
    CFRunLoopSourceRef q;
    c.info = info;
    s = CFSocketCreateWithNative(NULL, fd, kCFSocketReadCallBack, f, &c);
    q = CFSocketCreateRunLoopSource(NULL, s, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), q, kCFRunLoopDefaultMode);
    CFRelease(q);
    return s;
}

static void sock_zu(CFSocketRef *s, int *fd)
{
    if (*s) { CFSocketInvalidate(*s); CFRelease(*s); *s = NULL; *fd = -1; }   /* schliesst auch fd */
    else if (*fd >= 0) { close(*fd); *fd = -1; }
}

static int udp_offen(int *port)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    getsockname(fd, (struct sockaddr *)&a, &l);
    *port = ntohs(a.sin_port);
    return fd;
}

/* ---------- Ton an die Anzeige ----------
   Satz: [2 Byte Laenge][1 Byte Art][Inhalt]; Arten: k Schluessel und IV (je 16), f Format (Text),
   a RTP-Paket (verschluesselt, wie empfangen), s Synchronpaket des Steuerports, u Uhr (Text: Abstand ts0),
   v Lautstaerke (Text, dB). Saetze bis 512 Byte schreibt eine Pipe am Stueck; ist sie voll, entfaellt der Satz. */
static void ton_schicken(int art, const void *d, int n)
{
    unsigned char b[512];
    if (S.ton_pipe < 0 || n < 0 || n + 3 > (int)sizeof b) return;
    b[0] = (unsigned char)(n >> 8);
    b[1] = (unsigned char)n;
    b[2] = (unsigned char)art;
    memcpy(b + 3, d, (size_t)n);
    if (write(S.ton_pipe, b, (size_t)n + 3) != n + 3) S.ton_verloren++;
}

static void ton_text(int art, const char *t) { ton_schicken(art, t, (int)strlen(t)); }

static void ton_uhr_schicken(void)
{
    char t[64];
    if (!S.uhr_da) return;
    snprintf(t, sizeof t, "%.9f %llu", S.uhr_ab, S.ts0_da ? S.ts0 : 0ULL);
    ton_text('u', t);
}

/* alles, was die Anzeige zum Ton wissen muss (nach SETUP und wenn die Anzeige startet) */
static void ton_infos_schicken(void)
{
    unsigned char k[32];
    char t[32];
    if (!S.aeskey_da || !S.ton_format[0]) return;
    memcpy(k, S.aeskey, 16);        /* derselbe gehashte Schluessel wie fuers Bild, dazu eiv aus SETUP */
    memcpy(k + 16, S.aesiv, 16);
    ton_schicken('k', k, 32);
    ton_text('f', S.ton_format);
    ton_uhr_schicken();
    if (S.ton_vol_da) { snprintf(t, sizeof t, "%.3f", S.ton_vol); ton_text('v', t); }
}

/* das iPhone schickt jedes Tonpaket dreimal (versetzt mit den Nachbarn): nur die erste Ankunft weitergeben */
static int ton_neu(const unsigned char *p)
{
    static unsigned letzte[16];
    static int pos;
    unsigned ts = (unsigned)p[4] << 24 | (unsigned)p[5] << 16 | (unsigned)p[6] << 8 | p[7];
    int k;
    for (k = 0; k < 16; k++) if (letzte[k] == ts) return 0;
    letzte[pos] = ts;
    pos = (pos + 1) % 16;
    return 1;
}

static void ton_daten(CFSocketRef s, CFSocketCallBackType t, CFDataRef a, const void *d, void *i)
{
    unsigned char b[2048];
    ssize_t r = recv(CFSocketGetNative(s), b, sizeof b, 0);
    (void)t; (void)a; (void)d; (void)i;
    if (r < 12 || !ton_neu(b)) return;
    S.ton_pakete++;
    ton_schicken('a', b, (int)r);
}

/* Steuerport: 0x54 Synchronpaket (RTP-Zeit, NTP-Zeit), 0x56 nachgereichtes Paket ab Byte 4 */
static void ton_steuer(CFSocketRef s, CFSocketCallBackType t, CFDataRef a, const void *d, void *i)
{
    unsigned char b[2048];
    ssize_t r = recv(CFSocketGetNative(s), b, sizeof b, 0);
    (void)t; (void)a; (void)d; (void)i;
    if (r < 4) return;
    if ((b[1] & 0x7f) == 0x54 && r >= 20) ton_schicken('s', b, (int)r);
    else if ((b[1] & 0x7f) == 0x56 && r >= 16 && ton_neu(b + 4)) ton_schicken('a', b + 4, (int)r - 4);
}

/* NTP-Antwort des iPhones (80 d3 00 07, 0000, Ursprung, Empfang, Senden): Uhrabstand (Uhr des iPhones minus eigene,
   NTP-Sekunden) aus der Antwort mit der kuerzesten Laufzeit der letzten 30. Nur fuer Messungen: /tmp/airplay_uhr
   ("abstand laufzeit ts0"); Empfangszeit vom Kern (SO_TIMESTAMP), damit die Wartezeit im Runloop nicht mitzaehlt. */
static double ntp_sek(const unsigned char *p)
{
    unsigned long s = (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3];
    unsigned long f = (unsigned long)p[4] << 24 | (unsigned long)p[5] << 16 | (unsigned long)p[6] << 8 | p[7];
    return (double)s + (double)f / 4294967296.0;
}

static double Ntp_abst[30], Ntp_lauf[30];
static int Ntp_n, Ntp_pos;

static void ntp_antwort(CFSocketRef s, CFSocketCallBackType t, CFDataRef a, const void *d, void *i)
{
    double *abst = Ntp_abst, *lauf = Ntp_lauf;
    static int gemeldet;
    unsigned char b[256];
    union { struct cmsghdr h; char b[64]; } k;
    struct iovec io;
    struct msghdr m;
    struct cmsghdr *c;
    struct timeval an;
    double t1, t2, t3, t4, beste = 1e9, ab = 0;
    ssize_t r;
    int j;
    FILE *f;
    (void)t; (void)a; (void)d; (void)i;
    io.iov_base = b;
    io.iov_len = sizeof b;
    memset(&m, 0, sizeof m);
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    m.msg_control = k.b;
    m.msg_controllen = sizeof k.b;
    r = recvmsg(CFSocketGetNative(s), &m, 0);
    gettimeofday(&an, NULL);
    if (m.msg_controllen)
        for (c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMP) memcpy(&an, CMSG_DATA(c), sizeof an);
    if (r < 32 || (b[1] & 0x7f) != 0x53) return;
    t1 = ntp_sek(b + 8);
    t2 = ntp_sek(b + 16);
    t3 = ntp_sek(b + 24);
    t4 = (double)an.tv_sec + 2208988800.0 + an.tv_usec / 1e6;
    abst[Ntp_pos] = ((t2 - t1) + (t3 - t4)) / 2;
    lauf[Ntp_pos] = (t4 - t1) - (t3 - t2);
    Ntp_pos = (Ntp_pos + 1) % 30;
    if (Ntp_n < 30) Ntp_n++;
    for (j = 0; j < Ntp_n; j++) if (lauf[j] < beste) { beste = lauf[j]; ab = abst[j]; }
    if (!gemeldet) { Meld("AirPlay: Uhr des iPhones %+.6f s, Laufzeit %.1f ms", ab, beste * 1000); gemeldet = 1; }
    S.uhr_ab = ab;
    S.uhr_da = 1;
    ton_uhr_schicken();
    f = fopen("/tmp/airplay_uhr", "w");
    if (f) { fprintf(f, "%.6f %.6f %llu\n", ab, beste, S.ts0_da ? S.ts0 : 0ULL); fclose(f); }
}

/* NTP: Anfrage an den Zeitport des iPhones jede Sekunde */
static void ntp_fragen(CFRunLoopTimerRef t, void *i)
{
    unsigned char q[32];
    struct timeval tv;
    unsigned long long s, f;
    (void)t; (void)i;
    if (S.ntp_fd < 0) return;
    memset(q, 0, sizeof q);
    q[0] = 0x80; q[1] = 0xd2; q[2] = 0x00; q[3] = 0x07;
    gettimeofday(&tv, NULL);
    s = (unsigned long long)tv.tv_sec + 2208988800ULL;
    f = ((unsigned long long)tv.tv_usec << 32) / 1000000ULL;
    q[24] = s >> 24; q[25] = s >> 16; q[26] = s >> 8; q[27] = s;
    q[28] = f >> 24; q[29] = f >> 16; q[30] = f >> 8; q[31] = f;
    sendto(S.ntp_fd, q, sizeof q, 0, (struct sockaddr *)&S.ntp_ziel, sizeof S.ntp_ziel);
}

static void ntp_start(struct in_addr wer, int port)
{
    int ja = 1;
    if (S.ntp_fd >= 0) return;
    Ntp_n = Ntp_pos = 0;     /* neue Sitzung: das iPhone kann inzwischen neu gestartet sein (Uhr ab Start) */
    S.uhr_da = 0;
    S.ntp_fd = udp_offen(&S.ntp_port);
    if (S.ntp_fd < 0) return;
    setsockopt(S.ntp_fd, SOL_SOCKET, SO_TIMESTAMP, &ja, sizeof ja);
    S.ntp_s = quelle(S.ntp_fd, ntp_antwort, NULL);
    memset(&S.ntp_ziel, 0, sizeof S.ntp_ziel);
    S.ntp_ziel.sin_family = AF_INET;
    S.ntp_ziel.sin_addr = wer;
    S.ntp_ziel.sin_port = htons(port);
    S.ntp_uhr = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.2, 1.0, 0, 0, ntp_fragen, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), S.ntp_uhr, kCFRunLoopDefaultMode);
}

/* ---------- Anzeige (Spiegel.app) ---------- */
static void anzeige_aus(void)
{
    Stueck *s;
    if (S.anz_uhr) { CFRunLoopTimerInvalidate(S.anz_uhr); CFRelease(S.anz_uhr); S.anz_uhr = NULL; }
    if (S.anz_fd >= 0) { close(S.anz_fd); S.anz_fd = -1; }
    if (S.ton_pipe >= 0) { close(S.ton_pipe); S.ton_pipe = -1; }
    if (S.ton_pakete) Meld("AirPlay: Ton %ld Pakete, %ld nicht an die Anzeige", S.ton_pakete, S.ton_verloren);
    S.ton_pakete = S.ton_verloren = 0;
    while ((s = S.kopf)) { S.kopf = s->nach; free(s); }
    S.ende = NULL;
    S.rueckstau = 0;
    if (S.anz_pid > 0) { waitpid(S.anz_pid, NULL, WNOHANG); S.anz_pid = 0; }
}

static void video_aus(const char *grund);

static void anzeige_leeren(void);
static void anzeige_uhr_ab(CFRunLoopTimerRef t, void *i)
{
    (void)t; (void)i;
    if (S.anz_uhr) { CFRelease(S.anz_uhr); S.anz_uhr = NULL; }
    anzeige_leeren();
}

static void anzeige_leeren(void)
{
    while (S.kopf && S.anz_fd >= 0) {
        Stueck *s = S.kopf;
        ssize_t r = write(S.anz_fd, s->d + s->pos, s->n - s->pos);
        if (r < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                if (!S.anz_uhr) {
                    S.anz_uhr = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.005, 0, 0, 0, anzeige_uhr_ab, NULL);
                    CFRunLoopAddTimer(CFRunLoopGetCurrent(), S.anz_uhr, kCFRunLoopDefaultMode);
                }
                return;
            }
            /* Anzeige geschlossen (Esc): Spiegelung beenden */
            video_aus("Anzeige geschlossen");
            return;
        }
        s->pos += (int)r;
        S.rueckstau -= r;
        if (s->pos == s->n) { S.kopf = s->nach; if (!S.kopf) S.ende = NULL; free(s); }
    }
}

static int anzeige_an(void)
{
    int p[2], q[2];
    pid_t pid;
    if (S.anz_fd >= 0) return 0;
    if (pipe(p)) return -1;
    if (pipe(q)) { close(p[0]); close(p[1]); return -1; }
    pid = fork();
    if (pid == 0) {
        int fd;
        dup2(p[0], 0);
        dup2(q[0], 3);      /* Ton */
        /* keine Netzverbindungen des Dienstes an die Anzeige vererben */
        for (fd = 4; fd < 256; fd++) close(fd);
        /* Adresse des iPhones: fuer die Eingabe ueber den Entwickler-Tunnel per WLAN */
        execl(Anzeige_pfad, Anzeige_pfad, inet_ntoa(S.ntp_ziel.sin_addr), (char *)NULL);
        _exit(1);
    }
    close(p[0]);
    close(q[0]);
    if (pid < 0) { close(p[1]); close(q[1]); return -1; }
    S.anz_pid = pid;
    S.anz_fd = p[1];
    fcntl(S.anz_fd, F_SETFL, O_NONBLOCK);
    S.ton_pipe = q[1];
    fcntl(S.ton_pipe, F_SETFL, O_NONBLOCK);
    fcntl(S.ton_pipe, F_SETFD, FD_CLOEXEC);
    S.bis_idr = 1;
    ton_infos_schicken();
    return 0;
}

/* Rahmen an die Anzeige: [8 Byte Zeit in us][4 Byte Laenge] + Annex-B, beides Big-Endian */
static void anzeige_schicken(unsigned long long us, const unsigned char *a, int na, const unsigned char *b, int nb, int idr)
{
    Stueck *s;
    int n = na + nb, i;
    if (S.anz_fd < 0) return;
    if (S.rueckstau > RUECKSTAU_MAX) S.bis_idr = 1;
    if (S.bis_idr && !idr) { S.verworfen++; return; }
    S.bis_idr = 0;
    s = malloc(sizeof(Stueck) + 12 + n);
    s->nach = NULL; s->n = 12 + n; s->pos = 0;
    for (i = 0; i < 8; i++) s->d[i] = (unsigned char)(us >> (56 - 8 * i));
    s->d[8] = n >> 24; s->d[9] = n >> 16; s->d[10] = n >> 8; s->d[11] = n;
    if (na) memcpy(s->d + 12, a, na);
    if (nb) memcpy(s->d + 12 + na, b, nb);
    if (S.ende) S.ende->nach = s; else S.kopf = s;
    S.ende = s;
    S.rueckstau += s->n;
    S.bilder++;
    anzeige_leeren();
}

/* ---------- Videostrom ---------- */
static void video_paket(const unsigned char *k, unsigned char *p, int n)
{
    if (k[4] == 0x01) {
        /* SPS + PPS (unverschluesselt, avcC-Form) -> Annex-B, vor das naechste Bild */
        int sps, pps;
        if (n < 11) return;
        sps = (int)be16(p + 6);
        if (8 + sps + 3 > n) return;
        pps = (int)be16(p + sps + 9);
        if (11 + sps + pps > n) return;
        free(S.spspps);
        S.nspspps = 8 + sps + pps;
        S.spspps = malloc(S.nspspps);
        memcpy(S.spspps, "\0\0\0\1", 4);
        memcpy(S.spspps + 4, p + 8, sps);
        memcpy(S.spspps + 4 + sps, "\0\0\0\1", 4);
        memcpy(S.spspps + 8 + sps, p + sps + 11, pps);
        S.spspps_offen = 1;
        if (Laut) Meld("AirPlay: Bildformat %.0fx%.0f (Quelle %.0fx%.0f)", lef(k + 56), lef(k + 60), lef(k + 40), lef(k + 44));
        anzeige_an();
    } else if (k[4] == 0x00 && S.video_da) {
        unsigned long long ts = le64(k + 8), us;
        int pos = 0, idr = k[5] == 0x10;
        ctr_xor(&S.video, p, n);
        /* Laengenpraefixe (4 Byte Big-Endian) -> Startcodes */
        while (pos + 4 <= n) {
            unsigned int l = be32(p + pos);
            if (l > (unsigned)(n - pos - 4)) { if (Laut) Meld("AirPlay: Bild beschaedigt"); return; }
            if ((p[pos + 4] & 0x1f) == 5) idr = 1;
            memcpy(p + pos, "\0\0\0\1", 4);
            pos += 4 + (int)l;
        }
        us = (ts >> 32) * 1000000ULL + (((ts & 0xffffffffULL) * 1000000ULL) >> 32);
        if (!S.ts0_da) { S.ts0 = us; S.ts0_da = 1; }
        us = us >= S.ts0 ? us - S.ts0 + 1000000ULL : 1000000ULL;
        if (S.spspps_offen) { anzeige_schicken(us, S.spspps, S.nspspps, p, n, 1); S.spspps_offen = 0; }
        else anzeige_schicken(us, NULL, 0, p, n, idr);
    }
}

static void video_lesen(CFSocketRef s, CFSocketCallBackType t, CFDataRef a, const void *d, void *i)
{
    ssize_t r;
    (void)s; (void)t; (void)a; (void)d; (void)i;
    if (S.dn + 65536 > S.dcap) { S.dcap = (S.dn + 65536) * 2; S.db = realloc(S.db, S.dcap); }
    r = recv(S.dv_fd, S.db + S.dn, S.dcap - S.dn, 0);
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (r <= 0) { video_aus("Videostrom zu"); return; }
    S.dn += (int)r;
    for (;;) {
        unsigned int n;
        if (S.dn < 128) break;
        n = le32(S.db);
        if (n > (16u << 20)) { video_aus("Videopaket zu gross"); return; }
        if (S.dn < 128 + (int)n) break;
        video_paket(S.db, S.db + 128, (int)n);
        if (S.dv_fd < 0) return;
        memmove(S.db, S.db + 128 + n, S.dn - 128 - n);
        S.dn -= 128 + (int)n;
    }
}

static void video_annehmen(CFSocketRef s, CFSocketCallBackType t, CFDataRef a, const void *d, void *i)
{
    int fd = accept(CFSocketGetNative(s), NULL, NULL), ja = 1;
    (void)t; (void)a; (void)d; (void)i;
    if (fd < 0) return;
    if (S.dv_fd >= 0) { close(fd); return; }
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &ja, sizeof ja);
    S.dv_fd = fd;
    S.dn = 0;
    S.dv_s = quelle(fd, video_lesen, NULL);
    if (Laut) Meld("AirPlay: Videostrom verbunden");
}

static int video_start(unsigned long long strom_id)
{
    char k[64], v[64];
    unsigned char key[16], iv[16];
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    int ja = 1;
    snprintf(k, sizeof k, "AirPlayStreamKey%llu", strom_id);
    snprintf(v, sizeof v, "AirPlayStreamIV%llu", strom_id);
    sha16(key, k, (int)strlen(k), S.aeskey, 16);
    sha16(iv, v, (int)strlen(v), S.aeskey, 16);
    ctr_init(&S.video, key, iv);
    S.video_da = S.aeskey_da;
    S.ts0_da = 0;
    if (S.dl_fd < 0) {
        S.dl_fd = socket(AF_INET, SOCK_STREAM, 0);
        setsockopt(S.dl_fd, SOL_SOCKET, SO_REUSEADDR, &ja, sizeof ja);
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(S.dl_fd, (struct sockaddr *)&a, sizeof a) || listen(S.dl_fd, 2)) { close(S.dl_fd); S.dl_fd = -1; return -1; }
        getsockname(S.dl_fd, (struct sockaddr *)&a, &l);
        S.daten_port = ntohs(a.sin_port);
        S.dl_s = quelle(S.dl_fd, video_annehmen, NULL);
    }
    return S.daten_port;
}

static void video_aus(const char *grund)
{
    if (S.dv_fd >= 0 || S.anz_fd >= 0) {
        if (Laut || S.bilder) Meld("AirPlay: Spiegelung aus (%s), %ld Bilder, %ld verworfen", grund, S.bilder, S.verworfen);
    }
    sock_zu(&S.dv_s, &S.dv_fd);
    sock_zu(&S.dl_s, &S.dl_fd);
    free(S.db); S.db = NULL; S.dn = S.dcap = 0;
    free(S.spspps); S.spspps = NULL; S.nspspps = 0; S.spspps_offen = 0;
    S.video_da = 0;
    S.bilder = S.verworfen = 0;
    anzeige_aus();
}

static void sitzung_aus(void)
{
    int k;
    video_aus("Sitzung zu");
    if (S.ntp_uhr) { CFRunLoopTimerInvalidate(S.ntp_uhr); CFRelease(S.ntp_uhr); S.ntp_uhr = NULL; }
    sock_zu(&S.ntp_s, &S.ntp_fd);
    for (k = 0; k < 2; k++) sock_zu(&S.ton_s[k], &S.ton_fd[k]);
    S.aeskey_da = 0;
}

/* ---------- RTSP ---------- */
typedef struct {
    int fd;
    CFSocketRef s;
    struct in_addr ip;
    unsigned char *ein;
    int nein, capein, zu;
    /* Legacy-Pairing */
    unsigned char x_geheim[32], x_eigen[32], x_fremd[32], ed_fremd[32], geteilt[32];
    int pv_stufe, geteilt_da;
    /* FairPlay */
    unsigned char keymsg[164];
    int keymsg_da;
} ApVerb;

static ApVerb *Verbs[MAX_RTSP];

typedef struct {
    char methode[24], url[256], proto[16], cseq[24], typ[96];
    const unsigned char *rumpf;
    int nrumpf;
} Anfrage;

static void senden(ApVerb *v, const void *b, int n)
{
    const unsigned char *p = b;
    while (n > 0) {
        ssize_t r = send(v->fd, p, n, 0);
        if (r < 0) { if (errno == EINTR) continue; v->zu = 1; return; }
        p += r; n -= (int)r;
    }
}

static void antwort(ApVerb *v, const Anfrage *a, int code, const char *grund, const char *koepfe,
                    const unsigned char *rumpf, int n, const char *typ)
{
    char k[1024];
    int l = snprintf(k, sizeof k, "%s %d %s\r\n", a->proto[0] ? a->proto : "RTSP/1.0", code, grund);
    if (a->cseq[0]) l += snprintf(k + l, sizeof k - l, "CSeq: %s\r\n", a->cseq);
    l += snprintf(k + l, sizeof k - l, "Server: AirTunes/%s\r\n", AP_VERSION);
    if (typ) l += snprintf(k + l, sizeof k - l, "Content-Type: %s\r\n", typ);
    if (koepfe) l += snprintf(k + l, sizeof k - l, "%s", koepfe);
    l += snprintf(k + l, sizeof k - l, "Content-Length: %d\r\n\r\n", n);
    senden(v, k, l);
    if (n) senden(v, rumpf, n);
}

static void antwort_plist(ApVerb *v, const Anfrage *a, CFPropertyListRef p)
{
    int n;
    unsigned char *b = plist_binaer(p, &n);
    antwort(v, a, 200, "OK", NULL, b, n, "application/x-apple-binary-plist");
    free(b);
}

static void info(ApVerb *v, const Anfrage *a)
{
    CFMutableDictionaryRef d = woerterbuch();
    CFMutableArrayRef arr;
    unsigned char t[700];
    int n;
    if (a->nrumpf && strstr(a->typ, "binary-plist")) {
        /* erste Anfrage: nur der TXT-Eintrag */
        CFDictionaryRef q = plist_lesen(a->rumpf, a->nrumpf);
        CFTypeRef qual = hole(q, "qualifier");
        char was[32] = "";
        if (qual && CFGetTypeID(qual) == CFArrayGetTypeID() && CFArrayGetCount(qual) > 0) {
            CFTypeRef s = CFArrayGetValueAtIndex(qual, 0);
            if (CFGetTypeID(s) == CFStringGetTypeID()) CFStringGetCString(s, was, sizeof was, kCFStringEncodingUTF8);
        }
        if (!strcmp(was, "txtAirPlay")) { n = airplay_txt(t, sizeof t); setz(d, "txtAirPlay", CFDataCreate(NULL, t, n)); }
        else if (!strcmp(was, "txtRAOP")) { n = raop_txt(t, sizeof t); setz(d, "txtRAOP", CFDataCreate(NULL, t, n)); }
        if (q) CFRelease(q);
        antwort_plist(v, a, d);
        CFRelease(d);
        return;
    }
    setz(d, "deviceID", text(MacText));
    setz(d, "macAddress", text(MacText));
    setz(d, "pk", CFDataCreate(NULL, Ed_pk, 32));
    setz(d, "features", zahl((long long)FEATURES));
    setz(d, "name", text(Name));
    setz(d, "pi", text(Kennung));
    setz(d, "vv", zahl(2));
    setz(d, "statusFlags", zahl(68));
    setz(d, "keepAliveLowPower", zahl(1));
    setz(d, "sourceVersion", text(AP_VERSION));
    setz(d, "keepAliveSendStatsAsBody", wahr(1));
    setz(d, "model", text(AP_MODELL));
    setz(d, "initialVolume", kommazahl(0.0));
    {
        int typ;
        CFMutableArrayRef lat = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
        CFMutableArrayRef fmt = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
        for (typ = 100; typ <= 101; typ++) {
            CFMutableDictionaryRef l = woerterbuch(), f = woerterbuch();
            setz(l, "type", zahl(typ));
            setz(l, "inputLatencyMicros", zahl(0));
            setz(l, "audioType", text("default"));
            setz(l, "outputLatencyMicros", wahr(0));
            CFArrayAppendValue(lat, l); CFRelease(l);
            setz(f, "audioOutputFormats", zahl(0x3fffffc));
            setz(f, "type", zahl(typ));
            setz(f, "audioInputFormats", zahl(0x3fffffc));
            CFArrayAppendValue(fmt, f); CFRelease(f);
        }
        setz(d, "audioLatencies", lat);
        setz(d, "audioFormats", fmt);
    }
    {
        CFMutableDictionaryRef s = woerterbuch();
        arr = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
        setz(s, "uuid", text("e0ff8a27-6738-3d56-8a16-cc53aacee925"));
        setz(s, "widthPhysical", zahl(0));
        setz(s, "heightPhysical", zahl(0));
        setz(s, "width", zahl(BREITE));
        setz(s, "height", zahl(HOEHE));
        setz(s, "widthPixels", zahl(BREITE));
        setz(s, "heightPixels", zahl(HOEHE));
        setz(s, "rotation", wahr(0));
        setz(s, "refreshRate", kommazahl(1.0 / 60.0));
        setz(s, "maxFPS", zahl(MAX_FPS));
        setz(s, "overscanned", wahr(0));
        setz(s, "features", zahl(14));
        CFArrayAppendValue(arr, s); CFRelease(s);
        setz(d, "displays", arr);
    }
    antwort_plist(v, a, d);
    CFRelease(d);
}

/* Legacy-Pairing: Signaturen mit AES-128-CTR aus SHA-512(Salz | gemeinsames Geheimnis) */
static void pv_ctr(ApVerb *v, Ctr *c)
{
    unsigned char key[16], iv[16];
    sha16(key, "Pair-Verify-AES-Key", 19, v->geteilt, 32);
    sha16(iv, "Pair-Verify-AES-IV", 18, v->geteilt, 32);
    ctr_init(c, key, iv);
}

static void pair_verify(ApVerb *v, const Anfrage *a)
{
    const unsigned char *d = a->rumpf;
    unsigned char msg[64], sig[64], aus[96];
    Ctr c;
    if (a->nrumpf == 4 + 32 + 32 && d[0] == 1) {
        memcpy(v->x_fremd, d + 4, 32);
        memcpy(v->ed_fremd, d + 36, 32);
        zufall(v->x_geheim, 32);
        crypto_x25519_public_key(v->x_eigen, v->x_geheim);
        crypto_x25519(v->geteilt, v->x_geheim, v->x_fremd);
        crypto_wipe(v->x_geheim, 32);
        v->geteilt_da = 1;
        memcpy(msg, v->x_eigen, 32);
        memcpy(msg + 32, v->x_fremd, 32);
        crypto_ed25519_sign(sig, Ed_sk, msg, 64);
        pv_ctr(v, &c);
        ctr_xor(&c, sig, 64);
        memcpy(aus, v->x_eigen, 32);
        memcpy(aus + 32, sig, 64);
        v->pv_stufe = 1;
        antwort(v, a, 200, "OK", NULL, aus, 96, "application/octet-stream");
    } else if (a->nrumpf == 4 + 64 && d[0] == 0 && v->pv_stufe == 1) {
        unsigned char leer[64];
        memset(leer, 0, 64);
        pv_ctr(v, &c);
        ctr_xor(&c, leer, 64);          /* erste 64 Bytes gehoeren zur eigenen Signatur */
        memcpy(sig, d + 4, 64);
        ctr_xor(&c, sig, 64);
        memcpy(msg, v->x_fremd, 32);
        memcpy(msg + 32, v->x_eigen, 32);
        v->pv_stufe = 0;
        if (crypto_ed25519_check(sig, v->ed_fremd, msg, 64)) {
            Meld("AirPlay: Kopplung abgelehnt (Signatur)");
            antwort(v, a, 470, "Connection Authorization Required", "Connection: close\r\n", NULL, 0, NULL);
            v->zu = 1;
            return;
        }
        if (Laut) Meld("AirPlay: gekoppelt mit %s", inet_ntoa(v->ip));
        antwort(v, a, 200, "OK", NULL, NULL, 0, "application/octet-stream");
    } else {
        antwort(v, a, 400, "Bad Request", NULL, NULL, 0, NULL);
    }
}

static void fp_setup(ApVerb *v, const Anfrage *a)
{
    const unsigned char *d = a->rumpf;
    if (a->nrumpf == 16 && d[4] == 0x03 && d[14] < 4) {
        v->keymsg_da = 0;
        antwort(v, a, 200, "OK", NULL, fp_antwort[d[14]], 142, "application/octet-stream");
    } else if (a->nrumpf == 164 && d[4] == 0x03) {
        unsigned char aus[32];
        memcpy(v->keymsg, d, 164);
        v->keymsg_da = 1;
        memcpy(aus, fp_kopf, 12);
        memcpy(aus + 12, d + 144, 20);
        antwort(v, a, 200, "OK", NULL, aus, 32, "application/octet-stream");
    } else {
        antwort(v, a, 400, "Bad Request", NULL, NULL, 0, NULL);
    }
}

static void setup(ApVerb *v, const Anfrage *a)
{
    CFDictionaryRef q = plist_lesen(a->rumpf, a->nrumpf);
    CFMutableDictionaryRef d = woerterbuch();
    const unsigned char *ekey;
    int nk = 0;
    long long tport = 0;
    CFTypeRef streams;
    if (!q) { antwort(v, a, 400, "Bad Request", NULL, NULL, 0, NULL); CFRelease(d); return; }
    if ((ekey = hole_daten(q, "ekey", &nk)) && nk == 72 && v->keymsg_da) {
        unsigned char k72[72], k16[16];
        memcpy(k72, ekey, 72);
        playfair_decrypt(v->keymsg, k72, k16);
        if (v->geteilt_da) sha16(S.aeskey, k16, 16, v->geteilt, 32);
        else memcpy(S.aeskey, k16, 16);
        {
            int ni = 0;
            const unsigned char *eiv = hole_daten(q, "eiv", &ni);
            if (eiv && ni == 16) memcpy(S.aesiv, eiv, 16);
        }
        S.aeskey_da = 1;
        if (hole_zahl(q, "timingPort", &tport) && tport > 0) ntp_start(v->ip, (int)tport);
        setz(d, "eventPort", zahl(0));
        setz(d, "timingPort", zahl(S.ntp_port));
        if (Laut) {
            char m[64] = "", n[64] = "";
            CFTypeRef t = hole(q, "model"), t2 = hole(q, "name");
            if (t && CFGetTypeID(t) == CFStringGetTypeID()) CFStringGetCString(t, m, sizeof m, kCFStringEncodingUTF8);
            if (t2 && CFGetTypeID(t2) == CFStringGetTypeID()) CFStringGetCString(t2, n, sizeof n, kCFStringEncodingUTF8);
            Meld("AirPlay: Sitzung von %s (%s), Zeitport %lld", n, m, tport);
        }
    }
    streams = hole(q, "streams");
    if (streams && CFGetTypeID(streams) == CFArrayGetTypeID()) {
        CFMutableArrayRef aus = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
        CFIndex i;
        for (i = 0; i < CFArrayGetCount(streams); i++) {
            CFDictionaryRef s = CFArrayGetValueAtIndex(streams, i);
            long long typ = 0, id = 0;
            CFMutableDictionaryRef r;
            if (CFGetTypeID(s) != CFDictionaryGetTypeID() || !hole_zahl(s, "type", &typ)) continue;
            r = woerterbuch();
            if (typ == 110) {
                hole_zahl(s, "streamConnectionID", &id);
                setz(r, "dataPort", zahl(video_start((unsigned long long)id)));
                setz(r, "type", zahl(110));
            } else if (typ == 96) {
                int k;
                long long ct = 0, spf = 0, sr = 0, af = 0;
                hole_zahl(s, "ct", &ct);
                hole_zahl(s, "spf", &spf);
                hole_zahl(s, "sr", &sr);
                hole_zahl(s, "audioFormat", &af);
                snprintf(S.ton_format, sizeof S.ton_format, "ct=%lld spf=%lld sr=%lld af=%lld", ct, spf, sr, af);
                Meld("AirPlay: Ton %s", S.ton_format);
                for (k = 0; k < 2; k++) if (S.ton_fd[k] < 0) {
                    S.ton_fd[k] = udp_offen(&S.ton_port[k]);
                    if (S.ton_fd[k] >= 0) S.ton_s[k] = quelle(S.ton_fd[k], k ? ton_steuer : ton_daten, NULL);
                }
                {
                    int groesse = 256 * 1024;
                    setsockopt(S.ton_fd[0], SOL_SOCKET, SO_RCVBUF, &groesse, sizeof groesse);
                }
                ton_infos_schicken();
                setz(r, "dataPort", zahl(S.ton_port[0]));
                setz(r, "controlPort", zahl(S.ton_port[1]));
                setz(r, "type", zahl(96));
            } else {
                CFRelease(r);
                continue;
            }
            CFArrayAppendValue(aus, r);
            CFRelease(r);
        }
        setz(d, "streams", aus);
    }
    antwort_plist(v, a, d);
    CFRelease(d);
    CFRelease(q);
}

static void teardown(ApVerb *v, const Anfrage *a)
{
    CFDictionaryRef q = plist_lesen(a->rumpf, a->nrumpf);
    CFTypeRef streams = hole(q, "streams");
    int video = 0, ton = 0, k;
    if (streams && CFGetTypeID(streams) == CFArrayGetTypeID()) {
        CFIndex i;
        for (i = 0; i < CFArrayGetCount(streams); i++) {
            long long typ = 0;
            CFDictionaryRef s = CFArrayGetValueAtIndex(streams, i);
            if (CFGetTypeID(s) == CFDictionaryGetTypeID() && hole_zahl(s, "type", &typ)) {
                if (typ == 110) video = 1;
                if (typ == 96) ton = 1;
            }
        }
    }
    if (q) CFRelease(q);
    if (video) video_aus("vom iPhone beendet");
    if (ton) for (k = 0; k < 2; k++) sock_zu(&S.ton_s[k], &S.ton_fd[k]);
    if (!video && !ton) sitzung_aus();
    antwort(v, a, 200, "OK", "Connection: close\r\n", NULL, 0, NULL);
}

static void bearbeiten(ApVerb *v, const Anfrage *a)
{
    const char *m = a->methode, *u = a->url;
    if (Laut && strcmp(u, "/feedback")) Meld("AirPlay: %s %s (%d Bytes)", m, u, a->nrumpf);
    if (!strcmp(m, "GET") && !strncmp(u, "/info", 5)) info(v, a);
    else if (!strcmp(m, "POST") && !strcmp(u, "/pair-setup")) {
        if (a->nrumpf == 32) antwort(v, a, 200, "OK", NULL, Ed_pk, 32, "application/octet-stream");
        else antwort(v, a, 400, "Bad Request", NULL, NULL, 0, NULL);
    }
    else if (!strcmp(m, "POST") && !strcmp(u, "/pair-verify")) pair_verify(v, a);
    else if (!strcmp(m, "POST") && !strcmp(u, "/fp-setup")) fp_setup(v, a);
    else if (!strcmp(m, "SETUP")) setup(v, a);
    else if (!strcmp(m, "RECORD"))
        antwort(v, a, 200, "OK", "Audio-Latency: 0\r\nAudio-Jack-Status: connected; type=analog\r\n", NULL, 0, NULL);
    else if (!strcmp(m, "GET_PARAMETER")) {
        if (a->nrumpf >= 6 && !strncmp((const char *)a->rumpf, "volume", 6)) {
            const char *vol = "volume: 0.000000\r\n";
            antwort(v, a, 200, "OK", NULL, (const unsigned char *)vol, (int)strlen(vol), "text/parameters");
        } else antwort(v, a, 200, "OK", NULL, NULL, 0, NULL);
    }
    else if (!strcmp(m, "OPTIONS"))
        antwort(v, a, 200, "OK", "Public: SETUP, RECORD, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n", NULL, 0, NULL);
    else if (!strcmp(m, "TEARDOWN")) teardown(v, a);
    else if (!strcmp(m, "SET_PARAMETER") && a->nrumpf > 7 && a->nrumpf < 64 && !strncmp((const char *)a->rumpf, "volume:", 7)) {
        /* Lautstaerke des iPhones: -30..0 dB, -144 = stumm */
        char t[64];
        memcpy(t, a->rumpf, (size_t)a->nrumpf);
        t[a->nrumpf] = 0;
        S.ton_vol = atof(t + 7);
        S.ton_vol_da = 1;
        snprintf(t, sizeof t, "%.3f", S.ton_vol);
        ton_text('v', t);
        antwort(v, a, 200, "OK", NULL, NULL, 0, NULL);
    }
    else if (!strcmp(m, "SET_PARAMETER") || !strcmp(m, "FLUSH") || !strcmp(m, "POST"))
        antwort(v, a, 200, "OK", NULL, NULL, 0, NULL);
    else antwort(v, a, 501, "Not Implemented", NULL, NULL, 0, NULL);
}

static void rtsp_schliessen(ApVerb *v)
{
    int i, rest = 0;
    for (i = 0; i < MAX_RTSP; i++) if (Verbs[i] == v) Verbs[i] = NULL;
    if (v->s) { CFSocketInvalidate(v->s); CFRelease(v->s); }
    free(v->ein);
    crypto_wipe(v, sizeof *v);
    free(v);
    for (i = 0; i < MAX_RTSP; i++) if (Verbs[i]) rest++;
    if (!rest) sitzung_aus();
}

/* "Name: Wert" in den Kopfzeilen suchen, Gross-/Kleinschreibung egal */
static void kopfwert(const char *k, int nk, const char *name, char *aus, int max)
{
    int l = (int)strlen(name), i = 0;
    aus[0] = 0;
    while (i < nk) {
        const char *z = k + i, *e = memchr(z, '\n', nk - i);
        int zl = e ? (int)(e - z) : nk - i;
        if (zl > l + 1 && !strncasecmp(z, name, l) && z[l] == ':') {
            int a = l + 1, b = zl;
            while (a < b && z[a] == ' ') a++;
            while (b > a && (z[b - 1] == '\r' || z[b - 1] == ' ')) b--;
            if (b - a >= max) b = a + max - 1;
            memcpy(aus, z + a, b - a);
            aus[b - a] = 0;
            return;
        }
        i += zl + 1;
    }
}

static void rtsp_lesen(CFSocketRef s, CFSocketCallBackType t, CFDataRef ad, const void *dd, void *info)
{
    ApVerb *v = info;
    ssize_t r;
    (void)s; (void)t; (void)ad; (void)dd;
    if (v->nein + 65536 > v->capein) { v->capein = (v->nein + 65536) * 2; v->ein = realloc(v->ein, v->capein); }
    r = recv(v->fd, v->ein + v->nein, v->capein - v->nein, 0);
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (r <= 0) { rtsp_schliessen(v); return; }
    v->nein += (int)r;
    for (;;) {
        unsigned char *e = NULL;
        int kopflaenge, i, rumpf;
        char cl[16];
        Anfrage a;
        for (i = 0; i + 3 < v->nein; i++)
            if (v->ein[i] == '\r' && v->ein[i + 1] == '\n' && v->ein[i + 2] == '\r' && v->ein[i + 3] == '\n') { e = v->ein + i; break; }
        if (!e) { if (v->nein > 65536) v->zu = 1; break; }
        kopflaenge = (int)(e - v->ein) + 4;
        memset(&a, 0, sizeof a);
        {
            char zeile[300];
            const unsigned char *nl = memchr(v->ein, '\n', kopflaenge);
            int zl = nl ? (int)(nl - v->ein) : 0;
            if (zl >= (int)sizeof zeile) zl = sizeof zeile - 1;
            memcpy(zeile, v->ein, zl);
            zeile[zl] = 0;
            sscanf(zeile, "%23s %255s %15s", a.methode, a.url, a.proto);
            kopfwert((const char *)v->ein, kopflaenge, "CSeq", a.cseq, sizeof a.cseq);
            kopfwert((const char *)v->ein, kopflaenge, "Content-Type", a.typ, sizeof a.typ);
            kopfwert((const char *)v->ein, kopflaenge, "Content-Length", cl, sizeof cl);
        }
        rumpf = atoi(cl);
        if (rumpf < 0 || rumpf > (1 << 20)) { v->zu = 1; break; }
        if (v->nein < kopflaenge + rumpf) break;
        a.rumpf = v->ein + kopflaenge;
        a.nrumpf = rumpf;
        bearbeiten(v, &a);
        if (v->zu) break;
        memmove(v->ein, v->ein + kopflaenge + rumpf, v->nein - kopflaenge - rumpf);
        v->nein -= kopflaenge + rumpf;
    }
    if (v->zu) rtsp_schliessen(v);
}

static void rtsp_annehmen(CFSocketRef s, CFSocketCallBackType t, CFDataRef ad, const void *dd, void *info)
{
    struct sockaddr_in sa;
    socklen_t l = sizeof sa;
    int fd = accept(CFSocketGetNative(s), (struct sockaddr *)&sa, &l), i, frei = -1, ja = 1;
    struct timeval tv = { 2, 0 };
    ApVerb *v;
    (void)t; (void)ad; (void)dd; (void)info;
    if (fd < 0) return;
    for (i = 0; i < MAX_RTSP; i++) if (!Verbs[i]) { frei = i; break; }
    if (frei < 0) { close(fd); return; }
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &ja, sizeof ja);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &ja, sizeof ja);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    v = calloc(1, sizeof *v);
    v->fd = fd;
    v->ip = sa.sin_addr;
    v->s = quelle(fd, rtsp_lesen, v);
    Verbs[frei] = v;
    if (Laut) Meld("AirPlay: Verbindung von %s", inet_ntoa(v->ip));
}

int airplay_start(int wunsch, const unsigned char ed_sk[64], const unsigned char ed_pk[32], const unsigned char mac[6],
                  const char *name, const char *kennung, const char *anzeige_pfad, ApMeld meld, int laut)
{
    struct sockaddr_in sa;
    socklen_t l = sizeof sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0), ja = 1;
    Meld = meld;
    Laut = laut;
    memcpy(Ed_sk, ed_sk, 64);
    memcpy(Ed_pk, ed_pk, 32);
    memcpy(Mac, mac, 6);
    snprintf(Name, sizeof Name, "%s", name);
    snprintf(Kennung, sizeof Kennung, "%s", kennung);
    snprintf(Anzeige_pfad, sizeof Anzeige_pfad, "%s", anzeige_pfad);
    snprintf(MacText, sizeof MacText, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(RaopName, sizeof RaopName, "%02X%02X%02X%02X%02X%02X@%s", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], name);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &ja, sizeof ja);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(wunsch);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa)) {
        sa.sin_port = 0;
        if (bind(fd, (struct sockaddr *)&sa, sizeof sa)) { close(fd); return -1; }
    }
    listen(fd, 8);
    getsockname(fd, (struct sockaddr *)&sa, &l);
    quelle(fd, rtsp_annehmen, NULL);
    return ntohs(sa.sin_port);
}
