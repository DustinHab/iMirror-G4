/* Fernbedienung: Die Apple-TV-Fernbedienung im Kontrollzentrum des iPhones steuert das iBook.
   Meldet sich per Bonjour wie ein Apple TV (Companion-Protokoll), koppelt mit Code,
   und gibt Tasten, Wischgesten und Lautstaerke an das vordere Programm weiter.
   Ein Faden, CFRunLoop; ohne Verbindung keine Rechenzeit. */
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include "opack.h"
#include "kopplung.h"
#include "eingabe.h"
#include "rti.h"
#include "airplay.h"

#define APP CFSTR("local.fernbedienung")
#define MODELL "AppleTV14,1"
#define VERSION "715.2"
#define MC_LAUT 0x100
#define MAX_GERAETE 16
#define MAX_VERB 12
#define MAX_RAHMEN (1 << 20)

enum { A_COMPANION, A_MRP, A_AIRPLAY };

/* --- Protokoll ------------------------------------------------------ */
static void meld(const char *f, ...)
{
    char z[32];
    time_t t = time(NULL);
    va_list a;
    strftime(z, sizeof z, "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(stderr, "%s ", z);
    va_start(a, f);
    vfprintf(stderr, f, a);
    va_end(a);
    fputc('\n', stderr);
}

static void zufall(unsigned char *b, int n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, b, n) != n) { meld("kein Zufall"); exit(1); }
    close(fd);
}

static void hexen(char *o, const unsigned char *b, int n, int gross)
{
    static const char *kl = "0123456789abcdef", *gr = "0123456789ABCDEF";
    const char *z = gross ? gr : kl;
    int i;
    for (i = 0; i < n; i++) { o[2 * i] = z[b[i] >> 4]; o[2 * i + 1] = z[b[i] & 15]; }
    o[2 * n] = 0;
}

/* --- Identitaet und gekoppelte Geraete ------------------------------ */
static unsigned char Ich_sk[64], Ich_pk[32], Ich_mac[6];
static char Ich_id[40], Ich_name[64];

typedef struct { unsigned char id[64]; int nid; unsigned char pk[32]; } Geraet;
static Geraet Geraete[MAX_GERAETE];
static int nGeraete;

static void geraete_sichern(void)
{
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int i;
    for (i = 0; i < nGeraete; i++) {
        char h[132];
        CFStringRef k;
        CFDataRef v;
        hexen(h, Geraete[i].id, Geraete[i].nid, 0);
        k = CFStringCreateWithCString(NULL, h, kCFStringEncodingASCII);
        v = CFDataCreate(NULL, Geraete[i].pk, 32);
        CFDictionarySetValue(d, k, v);
        CFRelease(k); CFRelease(v);
    }
    CFPreferencesSetAppValue(CFSTR("Geraete"), d, APP);
    CFPreferencesAppSynchronize(APP);
    CFRelease(d);
}

static void geraet_merken(const unsigned char *id, int nid, const unsigned char pk[32])
{
    int i;
    for (i = 0; i < nGeraete; i++)
        if (Geraete[i].nid == nid && !memcmp(Geraete[i].id, id, nid)) break;
    if (i == nGeraete) {
        if (nGeraete == MAX_GERAETE) { memmove(Geraete, Geraete + 1, sizeof(Geraet) * (MAX_GERAETE - 1)); i = MAX_GERAETE - 1; }
        else nGeraete++;
    }
    memcpy(Geraete[i].id, id, nid);
    Geraete[i].nid = nid;
    memcpy(Geraete[i].pk, pk, 32);
    geraete_sichern();
}

static const Geraet *geraet_finden(const unsigned char *id, int nid)
{
    int i;
    for (i = 0; i < nGeraete; i++)
        if (Geraete[i].nid == nid && !memcmp(Geraete[i].id, id, nid)) return &Geraete[i];
    return NULL;
}

static int unhex(const char *h, unsigned char *o, int max)
{
    int n = (int)strlen(h) / 2, i;
    if (n > max) return -1;
    for (i = 0; i < n; i++) { unsigned v; if (sscanf(h + 2 * i, "%2x", &v) != 1) return -1; o[i] = (unsigned char)v; }
    return n;
}

static void identitaet_laden(void)
{
    unsigned char seed[32], h[64];
    CFPropertyListRef s = CFPreferencesCopyAppValue(CFSTR("Schluessel"), APP);
    CFPropertyListRef k, g;
    CFStringRef n;

    if (s && CFGetTypeID(s) == CFDataGetTypeID() && CFDataGetLength(s) == 32) {
        CFDataGetBytes(s, CFRangeMake(0, 32), seed);
    } else {
        CFDataRef d;
        zufall(seed, 32);
        d = CFDataCreate(NULL, seed, 32);
        CFPreferencesSetAppValue(CFSTR("Schluessel"), d, APP);
        CFRelease(d);
    }
    if (s) CFRelease(s);

    k = CFPreferencesCopyAppValue(CFSTR("Kennung"), APP);
    if (!k || CFGetTypeID(k) != CFStringGetTypeID()) {
        CFUUIDRef u = CFUUIDCreate(NULL);
        if (k) CFRelease(k);
        k = CFUUIDCreateString(NULL, u);
        CFRelease(u);
        CFPreferencesSetAppValue(CFSTR("Kennung"), k, APP);
    }
    CFStringGetCString(k, Ich_id, sizeof Ich_id, kCFStringEncodingASCII);
    CFRelease(k);
    CFPreferencesAppSynchronize(APP);

    crypto_ed25519_key_pair(Ich_sk, Ich_pk, seed);   /* loescht seed */

    /* feste, lokal verwaltete MAC aus der Kennung */
    crypto_sha512(h, (const unsigned char *)Ich_id, strlen(Ich_id));
    memcpy(Ich_mac, h, 6);
    Ich_mac[0] = (unsigned char)((Ich_mac[0] & 0xFE) | 0x02);

    n = SCDynamicStoreCopyComputerName(NULL, NULL);
    if (!n || !CFStringGetCString(n, Ich_name, sizeof Ich_name, kCFStringEncodingUTF8) || !Ich_name[0])
        strcpy(Ich_name, "iBook");
    if (n) CFRelease(n);

    g = CFPreferencesCopyAppValue(CFSTR("Geraete"), APP);
    if (g && CFGetTypeID(g) == CFDictionaryGetTypeID()) {
        CFIndex c = CFDictionaryGetCount(g), i;
        const void **ks = malloc(sizeof(void *) * (c + 1)), **vs = malloc(sizeof(void *) * (c + 1));
        CFDictionaryGetKeysAndValues(g, ks, vs);
        for (i = 0; i < c && nGeraete < MAX_GERAETE; i++) {
            char hx[140];
            Geraet *e = &Geraete[nGeraete];
            if (CFGetTypeID(ks[i]) != CFStringGetTypeID() || CFGetTypeID(vs[i]) != CFDataGetTypeID()) continue;
            if (CFDataGetLength(vs[i]) != 32) continue;
            if (!CFStringGetCString(ks[i], hx, sizeof hx, kCFStringEncodingASCII)) continue;
            e->nid = unhex(hx, e->id, sizeof e->id);
            if (e->nid <= 0) continue;
            CFDataGetBytes(vs[i], CFRangeMake(0, 32), e->pk);
            nGeraete++;
        }
        free(ks); free(vs);
    }
    if (g) CFRelease(g);
}

/* --- Code-Anzeige --------------------------------------------------- */
static CFUserNotificationRef Codefenster;
static void *Codeverb;

static void code_weg(void)
{
    if (Codefenster) { CFUserNotificationCancel(Codefenster); CFRelease(Codefenster); Codefenster = NULL; }
    Codeverb = NULL;
}

static void code_zeigen(void *verb, int pin)
{
    char t[32];
    CFStringRef kopf;
    CFMutableDictionaryRef d;
    SInt32 f = 0;
    code_weg();
    snprintf(t, sizeof t, "%04d", pin);
    kopf = CFStringCreateWithCString(NULL, t, kCFStringEncodingASCII);
    d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(d, kCFUserNotificationAlertHeaderKey, kopf);
    CFDictionarySetValue(d, kCFUserNotificationAlertMessageKey, CFSTR("iPhone-Fernbedienung"));
    Codefenster = CFUserNotificationCreate(NULL, 120, kCFUserNotificationPlainAlertLevel, &f, d);
    Codeverb = verb;
    CFRelease(d); CFRelease(kopf);
}

/* --- Verbindungen --------------------------------------------------- */
typedef struct {
    int fd, art, zu;
    CFSocketRef sock;
    CFRunLoopSourceRef quelle;
    CFRunLoopTimerRef frist;
    struct in_addr ip;
    unsigned char *ein;
    int nein, capein;
    /* Kopplung */
    Srp srp;
    int srp_an;
    unsigned char pv_geheim[32], pv_eigen[32], pv_fremd[32], pv_schl[32];
    int pv_stufe;
    /* verschluesselter Kanal */
    int verschl;
    unsigned char k_aus[32], k_ein[32], k_aus_neu[32], k_ein_neu[32];
    unsigned long long z_aus, z_ein;
    /* Sitzung */
    int sitzung;
    char kennung[64], geraet[96];
    double tx0, ty0, tx, ty, mx, my, rest;
    int rand;
    int beruehrt, ti_abo;
} Verb;

static Verb *Verbs[MAX_VERB];
static int Fehlversuche;
static time_t Sperre_bis;
static int Bildschirm_aus, Mausmodus;
static CFAbsoluteTime Tv_unten, Wahl_unten;
static pid_t Zeiger_pid;
static char Ordner[1024] = ".";

typedef struct { char geraet[96]; long long sid; } Sid;
static Sid Sids[8];
static int nSids;

static long long sid_fuer(const char *geraet)
{
    unsigned char z[4];
    int i;
    for (i = 0; i < nSids; i++) if (!strcmp(Sids[i].geraet, geraet)) return Sids[i].sid;
    if (nSids == 8) { memmove(Sids, Sids + 1, sizeof(Sid) * 7); nSids = 7; }
    zufall(z, 4);
    snprintf(Sids[nSids].geraet, sizeof Sids[nSids].geraet, "%s", geraet);
    Sids[nSids].sid = ((long long)(z[0] & 0x7F) << 24) | (z[1] << 16) | (z[2] << 8) | z[3];
    return Sids[nSids++].sid;
}

static void frist_weg(Verb *v)
{
    if (v->frist) { CFRunLoopTimerInvalidate(v->frist); CFRelease(v->frist); v->frist = NULL; }
}

static int Laut;   /* ausfuehrliches Protokoll: defaults write local.fernbedienung Ausfuehrlich -bool YES */

static void anh(char *o, int *n, int max, const char *s, int l)
{
    if (*n + l >= max) l = max - 1 - *n;
    if (l > 0) { memcpy(o + *n, s, l); *n += l; }
    o[*n] = 0;
}

static void wert_text(const OWert *w, char *o, int *n, int max, int tiefe)
{
    char t[48];
    int i;
    if (!w) { anh(o, n, max, "?", 1); return; }
    if (tiefe > 4) { anh(o, n, max, "..", 2); return; }
    switch (w->art) {
        case O_NULL: anh(o, n, max, "null", 4); break;
        case O_BOOL: anh(o, n, max, w->i ? "true" : "false", w->i ? 4 : 5); break;
        case O_INT: case O_TIME: snprintf(t, sizeof t, "%lld", w->i); anh(o, n, max, t, (int)strlen(t)); break;
        case O_FLOAT: snprintf(t, sizeof t, "%g", w->f); anh(o, n, max, t, (int)strlen(t)); break;
        case O_STR: anh(o, n, max, "'", 1); anh(o, n, max, (const char *)w->p, w->n > 40 ? 40 : w->n); anh(o, n, max, "'", 1); break;
        case O_DATA: case O_UUID: snprintf(t, sizeof t, "<%d B>", w->n); anh(o, n, max, t, (int)strlen(t)); break;
        case O_ARRAY:
            anh(o, n, max, "[", 1);
            for (i = 0; i < w->m; i++) { if (i) anh(o, n, max, ",", 1); wert_text(w->k[i], o, n, max, tiefe + 1); }
            anh(o, n, max, "]", 1);
            break;
        case O_DICT:
            anh(o, n, max, "{", 1);
            for (i = 0; i < w->m; i++) {
                if (i) anh(o, n, max, ",", 1);
                wert_text(w->k[2 * i], o, n, max, tiefe + 1);
                anh(o, n, max, ":", 1);
                wert_text(w->k[2 * i + 1], o, n, max, tiefe + 1);
            }
            anh(o, n, max, "}", 1);
            break;
    }
}

static void zeiger_programm(int an);
static void zeiger_anzeigen(void);

static void verb_schliessen(Verb *v, const char *grund)
{
    int i, sitzungen = 0, hatte = v->sitzung;
    if (Laut && v->art == A_COMPANION) meld("Verbindung %s zu: %s", inet_ntoa(v->ip), grund);
    for (i = 0; i < MAX_VERB; i++) if (Verbs[i] == v) Verbs[i] = NULL;
    frist_weg(v);
    if (Codeverb == v) code_weg();
    CFSocketInvalidate(v->sock);   /* schliesst auch fd */
    CFRelease(v->quelle);
    CFRelease(v->sock);
    if (v->srp_an) srp_frei(&v->srp);
    free(v->ein);
    crypto_wipe(v, sizeof *v);
    free(v);
    /* letzte Fernbedienungssitzung weg: runder Zeiger weg */
    for (i = 0; i < MAX_VERB; i++) if (Verbs[i] && Verbs[i]->sitzung) sitzungen++;
    if (hatte && !sitzungen) zeiger_programm(0);
}

static void frist_ab(CFRunLoopTimerRef t, void *info)
{
    Verb *v = info;
    (void)t;
    if (!v->sitzung) verb_schliessen(v, "Frist ohne Sitzung");
}

static void frist_setzen(Verb *v, double s)
{
    CFRunLoopTimerContext c = { 0, NULL, NULL, NULL, NULL };
    frist_weg(v);
    c.info = v;
    v->frist = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + s, 0, 0, 0, frist_ab, &c);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), v->frist, kCFRunLoopDefaultMode);
}

static void senden(Verb *v, const unsigned char *b, int n)
{
    while (n > 0 && !v->zu) {
        ssize_t r = send(v->fd, b, n, 0);
        if (r < 0) { if (errno == EINTR) continue; v->zu = 1; return; }
        b += r;
        n -= (int)r;
    }
}

static void rahmen(Verb *v, int typ, const unsigned char *d, int n)
{
    unsigned char *b = malloc(4 + n + 16);
    int l = n;
    if (typ == 8) {
        unsigned char nonce[12];
        l = n + 16;
        b[0] = 8; b[1] = (unsigned char)(l >> 16); b[2] = (unsigned char)(l >> 8); b[3] = (unsigned char)l;
        nonce_zaehler(nonce, v->z_aus++);
        chacha_zu(b + 4, v->k_aus, nonce, b, 4, d, n);
    } else {
        b[0] = (unsigned char)typ; b[1] = (unsigned char)(l >> 16); b[2] = (unsigned char)(l >> 8); b[3] = (unsigned char)l;
        memcpy(b + 4, d, n);
    }
    senden(v, b, 4 + l);
    free(b);
}

/* Kopplungsantwort: {_pd: TLV} */
static void pd_senden(Verb *v, int typ, const OPuffer *tlv, int pwty)
{
    OPuffer o;
    op_init(&o);
    op_dict(&o, pwty ? 2 : 1);
    op_str(&o, "_pd");
    op_data(&o, tlv->b, tlv->n);
    if (pwty) { op_str(&o, "_pwTy"); op_int(&o, 1); }
    rahmen(v, typ, o.b, o.n);
    op_frei(&o);
}

static void pd_fehler(Verb *v, int typ, int seq, int fehler)
{
    OPuffer t;
    unsigned char s = (unsigned char)seq, f = (unsigned char)fehler;
    op_init(&t);
    tlv_schreib(&t, 6, &s, 1);
    tlv_schreib(&t, 7, &f, 1);
    pd_senden(v, typ, &t, 0);
    op_frei(&t);
}

/* --- Pair-Setup ----------------------------------------------------- */
static void ps_m1(Verb *v)
{
    unsigned char b[32], salz[16], B[SRP_NB], z[2];
    OPuffer t;
    int pin, nB;
    if (time(NULL) < Sperre_bis) { pd_fehler(v, 4, 2, 3); return; }
    zufall(z, 2);
    pin = 1000 + ((z[0] << 8) | z[1]) % 9000;
    zufall(b, 32);
    zufall(salz, 16);
    if (v->srp_an) srp_frei(&v->srp);
    srp_anfang(&v->srp, pin, b, salz);
    v->srp_an = 1;
    crypto_wipe(b, 32);
    nB = srp_B(&v->srp, B);
    op_init(&t);
    tlv_schreib(&t, 6, (const unsigned char *)"\x02", 1);
    tlv_schreib(&t, 2, salz, 16);
    tlv_schreib(&t, 3, B, nB);
    tlv_schreib(&t, 0x1B, (const unsigned char *)"\x01", 1);
    pd_senden(v, 4, &t, 1);
    op_frei(&t);
    code_zeigen(v, pin);
    frist_setzen(v, 180.0);   /* Zeit zum Eintippen */
    meld("Kopplung angefragt von %s", inet_ntoa(v->ip));
}

static void ps_m3(Verb *v, const unsigned char *pd, int npd)
{
    unsigned char A[SRP_NB + 8], M[80], h[64];
    int nA = tlv_hole(pd, npd, 3, A, sizeof A), nM = tlv_hole(pd, npd, 4, M, sizeof M);
    OPuffer t;
    if (!v->srp_an || nA <= 0 || srp_A(&v->srp, A, nA) || !srp_beweis_ok(&v->srp, M, nM)) {
        pd_fehler(v, 4, 4, 2);
        code_weg();
        Fehlversuche++;
        Sperre_bis = time(NULL) + (Fehlversuche < 30 ? 2 * Fehlversuche : 60);
        meld("Kopplung: falscher Code");
        return;
    }
    srp_hamk(&v->srp, h);
    op_init(&t);
    tlv_schreib(&t, 6, (const unsigned char *)"\x04", 1);
    tlv_schreib(&t, 4, h, 64);
    pd_senden(v, 4, &t, 0);
    op_frei(&t);
}

static void ps_m5(Verb *v, const unsigned char *pd, int npd)
{
    unsigned char ks[32], nonce[12], enc[4096], dec[4096], id[64], ltpk[40], sig[72], x[32], msg[32 + 64 + 32];
    unsigned char s2[64], *c;
    int ne, nd, nid, nl, ns, nm;
    OPuffer t, info, aus;

    if (!v->srp_an || !v->srp.bereit) return;
    hkdf32(ks, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info", v->srp.K, 64);
    ne = tlv_hole(pd, npd, 5, enc, sizeof enc);
    if (ne < 16) { pd_fehler(v, 4, 6, 2); return; }
    nonce_text(nonce, "PS-Msg05");
    if (chacha_auf(dec, ks, nonce, NULL, 0, enc, ne)) { pd_fehler(v, 4, 6, 2); meld("Kopplung: M5 nicht lesbar"); return; }
    nd = ne - 16;
    nid = tlv_hole(dec, nd, 1, id, sizeof id);
    nl = tlv_hole(dec, nd, 3, ltpk, sizeof ltpk);
    ns = tlv_hole(dec, nd, 0x0A, sig, sizeof sig);
    if (nid <= 0 || nl != 32 || ns != 64) { pd_fehler(v, 4, 6, 2); meld("Kopplung: M5 unvollstaendig"); return; }
    hkdf32(x, "Pair-Setup-Controller-Sign-Salt", "Pair-Setup-Controller-Sign-Info", v->srp.K, 64);
    memcpy(msg, x, 32); memcpy(msg + 32, id, nid); memcpy(msg + 32 + nid, ltpk, 32);
    if (crypto_ed25519_check(sig, ltpk, msg, 32 + nid + 32)) { pd_fehler(v, 4, 6, 2); meld("Kopplung: Signatur des iPhones falsch"); return; }
    geraet_merken(id, nid, ltpk);

    /* eigene Antwort */
    hkdf32(x, "Pair-Setup-Accessory-Sign-Salt", "Pair-Setup-Accessory-Sign-Info", v->srp.K, 64);
    nm = 0;
    memcpy(msg, x, 32); nm += 32;
    memcpy(msg + nm, Ich_id, strlen(Ich_id)); nm += (int)strlen(Ich_id);
    memcpy(msg + nm, Ich_pk, 32); nm += 32;
    crypto_ed25519_sign(s2, Ich_sk, msg, nm);

    op_init(&info);
    {
        unsigned char irk[16] = { 0x2d, 0x54, 0xe0, 0x7a, 0x88, 0x2a, 0x65, 0x6e, 0x11, 0xab, 0x82, 0x76, 0x2d, 0x27, 0x25, 0xc5 };
        CFUUIDRef u = CFUUIDCreate(NULL);
        CFStringRef us = CFUUIDCreateString(NULL, u);
        char konto[40];
        CFStringGetCString(us, konto, sizeof konto, kCFStringEncodingASCII);
        CFRelease(us); CFRelease(u);
        op_dict(&info, 6);
        op_str(&info, "altIRK"); op_data(&info, irk, 16);
        op_str(&info, "accountID"); op_str(&info, konto);
        op_str(&info, "model"); op_str(&info, MODELL);
        op_str(&info, "wifiMAC"); op_data(&info, Ich_mac, 6);
        op_str(&info, "name"); op_str(&info, Ich_name);
        op_str(&info, "mac"); op_data(&info, Ich_mac, 6);
    }
    op_init(&t);
    tlv_schreib(&t, 1, (const unsigned char *)Ich_id, (int)strlen(Ich_id));
    tlv_schreib(&t, 3, Ich_pk, 32);
    tlv_schreib(&t, 0x0A, s2, 64);
    tlv_schreib(&t, 0x11, info.b, info.n);
    c = malloc(t.n + 16);
    nonce_text(nonce, "PS-Msg06");
    chacha_zu(c, ks, nonce, NULL, 0, t.b, t.n);
    op_init(&aus);
    tlv_schreib(&aus, 6, (const unsigned char *)"\x06", 1);
    tlv_schreib(&aus, 5, c, t.n + 16);
    pd_senden(v, 4, &aus, 0);
    free(c);
    op_frei(&aus); op_frei(&t); op_frei(&info);
    crypto_wipe(ks, 32);
    srp_frei(&v->srp);
    v->srp_an = 0;
    code_weg();
    Fehlversuche = 0;
    meld("Kopplung abgeschlossen (%d Geraete)", nGeraete);
}

/* --- Pair-Verify ---------------------------------------------------- */
static void pv_m1(Verb *v, const unsigned char *pd, int npd)
{
    unsigned char fremd[40], geteilt[32], msg[32 + 40 + 32], sig[64], nonce[12], *c;
    int nf = tlv_hole(pd, npd, 3, fremd, sizeof fremd), nm = 0, il = (int)strlen(Ich_id);
    OPuffer t, aus;
    if (nf != 32) return;
    zufall(v->pv_geheim, 32);
    crypto_x25519_public_key(v->pv_eigen, v->pv_geheim);
    crypto_x25519(geteilt, v->pv_geheim, fremd);
    crypto_wipe(v->pv_geheim, 32);
    memcpy(v->pv_fremd, fremd, 32);
    hkdf32(v->pv_schl, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", geteilt, 32);
    hkdf32(v->k_aus_neu, "", "ServerEncrypt-main", geteilt, 32);
    hkdf32(v->k_ein_neu, "", "ClientEncrypt-main", geteilt, 32);
    crypto_wipe(geteilt, 32);

    memcpy(msg, v->pv_eigen, 32); nm = 32;
    memcpy(msg + nm, Ich_id, il); nm += il;
    memcpy(msg + nm, fremd, 32); nm += 32;
    crypto_ed25519_sign(sig, Ich_sk, msg, nm);

    op_init(&t);
    tlv_schreib(&t, 1, (const unsigned char *)Ich_id, il);
    tlv_schreib(&t, 0x0A, sig, 64);
    c = malloc(t.n + 16);
    nonce_text(nonce, "PV-Msg02");
    chacha_zu(c, v->pv_schl, nonce, NULL, 0, t.b, t.n);
    op_init(&aus);
    tlv_schreib(&aus, 6, (const unsigned char *)"\x02", 1);
    tlv_schreib(&aus, 3, v->pv_eigen, 32);
    tlv_schreib(&aus, 5, c, t.n + 16);
    pd_senden(v, 6, &aus, 0);
    free(c);
    op_frei(&t); op_frei(&aus);
    v->pv_stufe = 1;
}

static void pv_m3(Verb *v, const unsigned char *pd, int npd)
{
    unsigned char enc[2048], dec[2048], id[64], sig[72], nonce[12], msg[32 + 64 + 32];
    int ne, nd, nid, ns;
    const Geraet *g;
    OPuffer t;
    if (v->pv_stufe != 1) return;
    v->pv_stufe = 0;
    ne = tlv_hole(pd, npd, 5, enc, sizeof enc);
    nonce_text(nonce, "PV-Msg03");
    if (ne < 16 || chacha_auf(dec, v->pv_schl, nonce, NULL, 0, enc, ne)) {
        pd_fehler(v, 6, 4, 2); meld("Anmeldung: M3 nicht lesbar"); return;
    }
    nd = ne - 16;
    nid = tlv_hole(dec, nd, 1, id, sizeof id);
    ns = tlv_hole(dec, nd, 0x0A, sig, sizeof sig);
    g = nid > 0 ? geraet_finden(id, nid) : NULL;
    if (!g || ns != 64) { pd_fehler(v, 6, 4, 2); meld("Anmeldung: unbekanntes Geraet von %s", inet_ntoa(v->ip)); return; }
    memcpy(msg, v->pv_fremd, 32);
    memcpy(msg + 32, id, nid);
    memcpy(msg + 32 + nid, v->pv_eigen, 32);
    if (crypto_ed25519_check(sig, g->pk, msg, 32 + nid + 32)) {
        pd_fehler(v, 6, 4, 2); meld("Anmeldung: Signatur falsch"); return;
    }
    op_init(&t);
    tlv_schreib(&t, 6, (const unsigned char *)"\x04", 1);
    pd_senden(v, 6, &t, 0);
    op_frei(&t);
    memcpy(v->k_aus, v->k_aus_neu, 32);
    memcpy(v->k_ein, v->k_ein_neu, 32);
    v->z_aus = v->z_ein = 0;
    v->verschl = 1;
    if (Laut) meld("angemeldet: %s", inet_ntoa(v->ip));
    if (!v->sitzung) frist_setzen(v, 3.0);
}

static void kopplung(Verb *v, int typ, const OWert *w)
{
    int npd, seq = 0, ns, i;
    unsigned char s[8];
    const unsigned char *pd = opack_daten(opack_hole(w, "_pd"), &npd);
    if (!pd) return;
    ns = tlv_hole(pd, npd, 6, s, sizeof s);
    for (i = 0; i < ns && i < 4; i++) seq |= s[i] << (8 * i);
    if (typ == 3 && seq == 1) ps_m1(v);
    else if (typ == 4 && seq == 3) ps_m3(v, pd, npd);
    else if (typ == 4 && seq == 5) ps_m5(v, pd, npd);
    else if (typ == 5 && seq == 1) pv_m1(v, pd, npd);
    else if (typ == 6 && seq == 3) pv_m3(v, pd, npd);
}

/* --- Steuerkanal ---------------------------------------------------- */
static void x_schreiben(OPuffer *o, const OWert *x)
{
    op_int(o, opack_zahl(x, 0));
}

static void antwort(Verb *v, const OWert *x, const OPuffer *inhalt)
{
    OPuffer o;
    op_init(&o);
    op_dict(&o, 3 + (v->kennung[0] ? 1 : 0) + (x ? 1 : 0));
    op_str(&o, "_c");
    if (inhalt) op_roh(&o, inhalt->b, inhalt->n); else op_dict(&o, 0);
    op_str(&o, "_rT"); op_int(&o, 0);
    op_str(&o, "_t"); op_int(&o, 3);
    if (v->kennung[0]) { op_str(&o, "_i"); op_str(&o, v->kennung); }
    if (x) { op_str(&o, "_x"); x_schreiben(&o, x); }
    rahmen(v, 8, o.b, o.n);
    op_frei(&o);
}

static void ereignis(Verb *v, const char *name, const OWert *x, const OPuffer *inhalt)
{
    OPuffer o;
    op_init(&o);
    op_dict(&o, 3 + (x ? 1 : 0));
    op_str(&o, "_i"); op_str(&o, name);
    op_str(&o, "_t"); op_int(&o, 1);
    op_str(&o, "_c");
    if (inhalt) op_roh(&o, inhalt->b, inhalt->n); else op_dict(&o, 0);
    if (x) { op_str(&o, "_x"); x_schreiben(&o, x); }
    rahmen(v, 8, o.b, o.n);
    op_frei(&o);
}

/* Inhalt {k: Zahl} */
static void ein_paar(OPuffer *o, const char *k, long long z)
{
    op_init(o);
    op_dict(o, 1);
    op_str(o, k);
    op_int(o, z);
}

static void zustand_ereignis(Verb *v, const char *name, const OWert *x)
{
    OPuffer c;
    ein_paar(&c, "state", 3);
    ereignis(v, name, x, &c);
    op_frei(&c);
}

static void medien_ereignisse(Verb *v, const OWert *x, int mit_stumm)
{
    OPuffer c;
    op_init(&c);
    op_dict(&c, mit_stumm ? 2 : 1);
    op_str(&c, "MediaControlFlags"); op_int(&c, MC_LAUT);
    if (mit_stumm) { op_str(&c, "Muted"); op_bool(&c, eingabe_ist_stumm()); }
    ereignis(v, "MediaControlStatus", x, &c);
    op_frei(&c);
    ein_paar(&c, "_mcF", MC_LAUT);
    ereignis(v, "_iMC", x, &c);
    op_frei(&c);
}

static void sitzung_an(Verb *v)
{
    v->sitzung = 1;
    frist_weg(v);
}

static void taste(Verb *v, int code, const OWert *x)
{
    if (Laut) meld("Taste %d", code);
    if (Bildschirm_aus && ((code >= 1 && code <= 7) || code == 14)) { eingabe_bildschirm(1); Bildschirm_aus = 0; }
    switch (code) {
        case 1: eingabe_taste(E_HOCH); break;
        case 2: eingabe_taste(E_RUNTER); break;
        case 3: eingabe_taste(E_LINKS); break;
        case 4: eingabe_taste(E_RECHTS); break;
        case 5: eingabe_taste(E_ZURUECK); break;
        case 6: eingabe_taste(E_WAHL); break;
        case 7: eingabe_taste(E_WECHSEL); break;
        case 8: eingabe_lauter(1); medien_ereignisse(v, x, 0); break;
        case 9: eingabe_lauter(-1); medien_ereignisse(v, x, 0); break;
        case 12: eingabe_bildschirm(0); Bildschirm_aus = 1; break;
        case 13: eingabe_bildschirm(1); Bildschirm_aus = 0; break;
        case 14: eingabe_taste(E_PAUSE); break;
        case 18: eingabe_stumm(!eingabe_ist_stumm()); medien_ereignisse(v, x, 1); break;
        case 19: Bildschirm_aus = !Bildschirm_aus; eingabe_bildschirm(!Bildschirm_aus); break;
    }
}

/* Mausmodus: Wischflaeche bewegt den Zeiger, rechter Rand oder zwei Finger rollen */
static void maus_beruehrung(Verb *v, long long ph, double x, double y, long long finger)
{
    double dx, dy, g;
    int zeilen;
    if (ph == 1) {
        v->tx = x; v->ty = y;
        v->beruehrt = 1;
        v->rand = x > 850 || finger >= 2;
        v->rest = 0;
        eingabe_zeiger_lage(&v->mx, &v->my);
        zeiger_anzeigen();
        if (Bildschirm_aus) { eingabe_bildschirm(1); Bildschirm_aus = 0; }
        return;
    }
    if (!v->beruehrt) return;
    if (ph != 2) { v->beruehrt = 0; if (ph != 4) return; }
    dx = x - v->tx;
    dy = y - v->ty;
    v->tx = x; v->ty = y;
    if (dx == 0 && dy == 0) return;
    if (v->rand) {
        /* pixelgenau, Reste mitnehmen; schneller gewischt rollt weiter */
        v->rest += dy * (1.2 + fabs(dy) / 25);
        zeilen = (int)v->rest;
        if (zeilen) { eingabe_rollen(zeilen); v->rest -= zeilen; }
        return;
    }
    /* schnell gewischt: weiter, langsam: genau */
    g = 0.8 + sqrt(dx * dx + dy * dy) / 30;
    if (g > 3.5) g = 3.5;
    v->mx += dx * g;
    v->my += dy * g;
    eingabe_zeiger(&v->mx, &v->my);
}

static void beruehrung(Verb *v, const OWert *c)
{
    long long ph = opack_zahl(opack_hole(c, "_tPh"), 0);
    const OWert *ox = opack_hole(c, "_cx"), *oy = opack_hole(c, "_cy");
    double x, y, dx, dy, d;
    if (!ox || !oy) return;
    x = opack_kommazahl(ox, 0);
    y = opack_kommazahl(oy, 0);
    if (Mausmodus) { maus_beruehrung(v, ph, x, y, opack_zahl(opack_hole(c, "_tFg"), 1)); return; }
    if (ph == 1) { v->tx0 = v->tx = x; v->ty0 = v->ty = y; v->beruehrt = 1; return; }
    if (!v->beruehrt) return;
    if (ph != 4) { v->tx = x; v->ty = y; if (ph != 2) v->beruehrt = 0; return; }
    v->beruehrt = 0;
    dx = x - v->tx0;
    dy = y - v->ty0;
    d = fabs(dx) > fabs(dy) ? fabs(dx) : fabs(dy);
    if (d < 150) return;   /* Antippen kommt gesondert als Auswahltaste */
    if (fabs(dx) > fabs(dy)) eingabe_taste(dx > 0 ? E_RECHTS : E_LINKS);
    else eingabe_taste(dy > 0 ? E_RUNTER : E_HOCH);
    Bildschirm_aus = 0;
}

/* Mausmodus an/aus: TV-Taste lang. Ton und runder Zeiger zeigen den Wechsel. */
static void zeiger_programm(int an)
{
    if (an && !Zeiger_pid) {
        char pfad[1100];
        pid_t p;
        snprintf(pfad, sizeof pfad, "%s/Zeiger.app/Contents/MacOS/Zeiger", Ordner);
        p = fork();
        if (p == 0) {
            int fd;
            for (fd = 3; fd < 256; fd++) close(fd);
            execl(pfad, pfad, (char *)NULL);
            _exit(1);
        }
        if (p > 0) Zeiger_pid = p;
    } else if (!an && Zeiger_pid) {
        if (waitpid(Zeiger_pid, NULL, WNOHANG) == 0) {   /* laeuft noch */
            kill(Zeiger_pid, SIGTERM);
            waitpid(Zeiger_pid, NULL, 0);
        }
        Zeiger_pid = 0;
    }
}

/* im Mausmodus bei Benutzung: Kreis zeigen, falls er weg ist (Trackpad benutzt, Sitzung neu) */
static void zeiger_anzeigen(void)
{
    if (!Mausmodus) return;
    if (Zeiger_pid && waitpid(Zeiger_pid, NULL, WNOHANG) != 0) Zeiger_pid = 0;
    if (!Zeiger_pid) zeiger_programm(1);
}

static void modus_wechseln(void)
{
    Mausmodus = !Mausmodus;
    CFPreferencesSetAppValue(CFSTR("Mausmodus"), Mausmodus ? kCFBooleanTrue : kCFBooleanFalse, APP);
    CFPreferencesAppSynchronize(APP);
    zeiger_programm(Mausmodus);
    eingabe_ton(Mausmodus);
    if (Laut) meld("Mausmodus %s", Mausmodus ? "an" : "aus");
}

/* Taste gedrueckt (1) oder losgelassen (2). TV lang: Modus; im Mausmodus Auswahl = Klick, lang = Rechtsklick */
static void knopf(Verb *v, int code, int zustand, const OWert *x)
{
    CFAbsoluteTime jetzt = CFAbsoluteTimeGetCurrent();
    if (code == 7) {
        if (zustand == 1) Tv_unten = jetzt;
        else if (zustand == 2) {
            if (Tv_unten > 0 && jetzt - Tv_unten >= 0.8) modus_wechseln();
            else taste(v, 7, x);
            Tv_unten = 0;
        }
        return;
    }
    if (code == 6 && Mausmodus) {
        if (zustand == 1) { Wahl_unten = jetzt; zeiger_anzeigen(); }
        else if (zustand == 2) {
            if (Bildschirm_aus) { eingabe_bildschirm(1); Bildschirm_aus = 0; }
            eingabe_klick(Wahl_unten > 0 && jetzt - Wahl_unten >= 0.6);
            Wahl_unten = 0;
        }
        return;
    }
    if (zustand == 2) taste(v, code, x);
}

static void medien(Verb *v, const OWert *c, const OWert *x)
{
    const OWert *k = opack_hole(c, "_mcc");
    long long cmd;
    OPuffer r;
    if (!k) k = opack_hole(c, "MediaControlCommand");
    cmd = opack_zahl(k, -1);
    op_init(&r);
    if (cmd == 5) {
        op_dict(&r, 1);
        op_str(&r, "_vol");
        op_double(&r, eingabe_ist_stumm() ? 0.0 : eingabe_lautstaerke());
    } else if (cmd == 6) {
        const OWert *vol = opack_hole(c, "_vol");
        if (vol) {
            double l = opack_kommazahl(vol, 0.5);
            eingabe_setze_lautstaerke((float)(l < 0 ? 0 : l > 1 ? 1 : l));
        }
        op_dict(&r, 0);
    } else if (cmd == 12) {
        op_dict(&r, 1);
        op_str(&r, "_cse"); op_bool(&r, 0);
    } else {
        op_dict(&r, 0);
    }
    antwort(v, x, &r);
    op_frei(&r);
}

/* --- iPhone-Tastatur (RTI) ------------------------------------------- */
static AXUIElementRef Ti_el;          /* aktives Textfeld */
static unsigned char Ti_uuid[16];
static CFRunLoopTimerRef Fokus_uhr;
static long long Ti_x = 0x40000000LL;

static void ti_ereignis(Verb *v, const char *name, const unsigned char *d, int n)
{
    OPuffer o;
    op_init(&o);
    op_dict(&o, 4);
    op_str(&o, "_i"); op_str(&o, name);
    op_str(&o, "_t"); op_int(&o, 1);
    op_str(&o, "_c");
    if (d) { op_dict(&o, 1); op_str(&o, "_tiD"); op_data(&o, d, n); }
    else op_dict(&o, 0);
    op_str(&o, "_x"); op_int(&o, Ti_x++);
    rahmen(v, 8, o.b, o.n);
    op_frei(&o);
}

static void ti_an_alle(const char *name, const unsigned char *d, int n)
{
    int i;
    for (i = 0; i < MAX_VERB; i++)
        if (Verbs[i] && Verbs[i]->art == A_COMPANION && Verbs[i]->verschl && Verbs[i]->ti_abo)
            ti_ereignis(Verbs[i], name, d, n);
}

static void stuecke_leeren(void);

/* Fokus pruefen: neues Textfeld -> _tiStarted, keins mehr -> _tiStopped.
   Gibt das Archiv des aktiven Feldes zurueck (Laenge, 0 wenn keins). */
static int fokus_pruefen(unsigned char **archiv)
{
    char titel[160], vor[1500], ausw[300], nach[600];
    AXUIElementRef el = NULL;
    unsigned char *a = NULL;
    int ja = eingabe_textfeld(&el, titel, sizeof titel, vor, sizeof vor, ausw, sizeof ausw, nach, sizeof nach);
    int gleich = ja && Ti_el && CFEqual(el, Ti_el), n = 0;
    if (Laut) meld("Fokus: Fehler %d, Rolle %s", eingabe_ax_fehler, eingabe_ax_rolle);
    if (ja) {
        if (gleich) CFRelease(el);
        else {
            if (Ti_el) { ti_an_alle("_tiStopped", NULL, 0); CFRelease(Ti_el); }
            Ti_el = el;
            zufall(Ti_uuid, 16);
            stuecke_leeren();
        }
        n = rti_archiv(&a, Ti_uuid, titel, vor, ausw, nach);
        if (!gleich && n) {
            ti_an_alle("_tiStarted", a, n);
            if (Laut) meld("Textfeld aktiv: %s (%d Bytes)", titel, n);
        }
    } else if (Ti_el) {
        CFRelease(Ti_el);
        Ti_el = NULL;
        stuecke_leeren();
        ti_an_alle("_tiStopped", NULL, 0);
        if (Laut) meld("Textfeld verlassen");
    }
    if (archiv) *archiv = a; else free(a);
    return n;
}

static void fokus_uhr_ab(CFRunLoopTimerRef t, void *i)
{
    CFRunLoopTimerRef u = Fokus_uhr;
    (void)t; (void)i;
    Fokus_uhr = NULL;
    fokus_pruefen(NULL);
    if (u) CFRelease(u);
}

/* kurz nach einer Eingabe nachsehen, ob ein Textfeld den Fokus hat; nur mit Abo */
static void fokus_spaeter(void)
{
    int i, abo = 0;
    for (i = 0; i < MAX_VERB; i++) if (Verbs[i] && Verbs[i]->ti_abo) abo = 1;
    if (!abo) return;
    if (Fokus_uhr) { CFRunLoopTimerInvalidate(Fokus_uhr); CFRelease(Fokus_uhr); }
    Fokus_uhr = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.35, 0, 0, 0, fokus_uhr_ab, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), Fokus_uhr, kCFRunLoopDefaultMode);
}

/* Das iPhone loescht in Einheiten seiner Einfuegungen: ohne Rueckmeldung vom Feld sendet es pro
   Tastendruck "1 loeschen" und dann den ganzen bisherigen Text neu. Deshalb werden die eingefuegten
   Stuecke gemerkt; folgt auf eine Loeschung gleich eine Einfuegung, wird nur der Unterschied getippt. */
static CFMutableArrayRef Stuecke;     /* CFString, seit das Feld aktiv ist */
static int Offen_loeschen;
static CFRunLoopTimerRef Loesch_uhr;

static void stuecke_leeren(void)
{
    if (Stuecke) CFArrayRemoveAllValues(Stuecke);
    Offen_loeschen = 0;
}

/* Zeichen, wie ein Rueckschritt sie zaehlt (zusammengesetzte Zeichen als eins) */
static int zeichen_ab(CFStringRef s, CFIndex ab)
{
    CFIndex i = ab, n = CFStringGetLength(s);
    int z = 0;
    while (i < n) { CFRange r = CFStringGetRangeOfComposedCharactersAtIndex(s, i); i = r.location + r.length; z++; }
    return z;
}

static void tippen_ab(CFStringRef s, CFIndex ab)
{
    CFIndex n = CFStringGetLength(s) - ab;
    UniChar *b;
    if (n <= 0) return;
    b = malloc(sizeof(UniChar) * n);
    CFStringGetCharacters(s, CFRangeMake(ab, n), b);
    eingabe_text(b, (int)n);
    free(b);
}

/* offene Loeschungen: so viele Stuecke vom Ende nehmen; was darueber hinausgeht, sind echte Zeichen */
static CFStringRef stuecke_nehmen(int *rest)
{
    CFMutableStringRef weg = CFStringCreateMutable(NULL, 0);
    while (Offen_loeschen > 0 && Stuecke && CFArrayGetCount(Stuecke) > 0) {
        CFIndex k = CFArrayGetCount(Stuecke) - 1;
        CFStringInsert(weg, 0, CFArrayGetValueAtIndex(Stuecke, k));
        CFArrayRemoveValueAtIndex(Stuecke, k);
        Offen_loeschen--;
    }
    *rest = Offen_loeschen;
    Offen_loeschen = 0;
    return weg;
}

static void loeschen_jetzt(void)
{
    int rest;
    CFStringRef weg = stuecke_nehmen(&rest);
    eingabe_loeschen(zeichen_ab(weg, 0) + rest);
    CFRelease(weg);
}

static void loesch_uhr_ab(CFRunLoopTimerRef t, void *i)
{
    CFRunLoopTimerRef u = Loesch_uhr;
    (void)t; (void)i;
    Loesch_uhr = NULL;
    if (Offen_loeschen > 0) loeschen_jetzt();
    if (u) CFRelease(u);
    fokus_spaeter();
}

static void loesch_uhr_weg(void)
{
    if (Loesch_uhr) { CFRunLoopTimerInvalidate(Loesch_uhr); CFRelease(Loesch_uhr); Loesch_uhr = NULL; }
}

static void einfuegen(const char *utf8)
{
    CFStringRef neu = CFStringCreateWithCString(NULL, utf8, kCFStringEncodingUTF8);
    if (!neu) return;
    if (!Stuecke) Stuecke = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    loesch_uhr_weg();
    if (Offen_loeschen > 0) {
        int rest;
        CFStringRef weg = stuecke_nehmen(&rest);
        CFIndex g = 0, na = CFStringGetLength(weg), nb = CFStringGetLength(neu);
        if (rest == 0) {
            /* gemeinsamen Anfang behalten, an einer Zeichengrenze */
            while (g < na && g < nb && CFStringGetCharacterAtIndex(weg, g) == CFStringGetCharacterAtIndex(neu, g)) g++;
            if (g < na) g = CFStringGetRangeOfComposedCharactersAtIndex(weg, g).location;
        }
        eingabe_loeschen(zeichen_ab(weg, g) + rest);
        tippen_ab(neu, g);
        CFRelease(weg);
    } else {
        tippen_ab(neu, 0);
    }
    CFArrayAppendValue(Stuecke, neu);
    if (CFArrayGetCount(Stuecke) > 200) CFArrayRemoveValueAtIndex(Stuecke, 0);
    CFRelease(neu);
}

/* _tiC: Text vom iPhone ins aktive Feld */
static void tastatur_eingabe(const OWert *c)
{
    int n;
    const unsigned char *d = opack_daten(opack_hole(c, "_tiD"), &n);
    RtiEingabe e;
    char roh[700];
    if (!d) return;
    if (rti_lesen(d, n, &e, Laut ? roh : NULL, sizeof roh)) {
        if (Laut) meld("Tastatur: nicht lesbar: %s", roh);
        return;
    }
    if (Laut) meld("Tastatur: loeschen %d, einfuegen '%s'%s%s", e.loeschen, e.einfuegen, e.zusichern ? ", Feld setzen" : "",
                   e.von_tastatur ? "" : " (Abgleich, uebergangen)");
    /* Pro Tastendruck schickt das iPhone die Eingabe (mit inputSourceState) und zusaetzlich
       den ganzen Text als Einfuegung (ohne). Nur die Eingabe zaehlt. */
    if (!e.von_tastatur && !e.zusichern) return;
    if (e.zusichern) {
        loesch_uhr_weg();
        stuecke_leeren();
        eingabe_alles_markieren();
        if (e.zusicherung[0]) eingabe_text_utf8(e.zusicherung);
        else eingabe_loeschen(1);
    }
    if (e.loeschen > 0) {
        /* kurz warten, ob gleich die Einfuegung folgt */
        Offen_loeschen += e.loeschen;
        loesch_uhr_weg();
        Loesch_uhr = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.08, 0, 0, 0, loesch_uhr_ab, NULL);
        CFRunLoopAddTimer(CFRunLoopGetCurrent(), Loesch_uhr, kCFRunLoopDefaultMode);
    }
    if (e.einfuegen[0]) einfuegen(e.einfuegen);
    if (!Loesch_uhr) fokus_spaeter();
}

static void steuerung(Verb *v, const OWert *w)
{
    const OWert *iw = opack_hole(w, "_i"), *x = opack_hole(w, "_x"), *c = opack_hole(w, "_c");
    const char *id;
    OPuffer r;
    int i;
    if (!iw) iw = opack_hole(w, "model");
    id = opack_text(iw);
    snprintf(v->kennung, sizeof v->kennung, "%s", id ? id : "");
    if (!id) id = "";
    if (Laut) {
        char t[600];
        int n = 0;
        t[0] = 0;
        wert_text(c, t, &n, sizeof t, 0);
        meld("<- %s x=%lld %s", id, opack_zahl(x, -1), t);
    }

    if (!strcmp(id, "_systemInfo") || !strcmp(id, "SystemInfo")) {
        const char *g = opack_text(opack_hole(c, "_idsID"));
        if (!g) g = opack_text(opack_hole(c, "_pubID"));
        if (g) {
            snprintf(v->geraet, sizeof v->geraet, "%s", g);
            for (i = 0; i < MAX_VERB; i++)
                if (Verbs[i] && Verbs[i] != v && Verbs[i]->art == A_COMPANION && !strcmp(Verbs[i]->geraet, v->geraet))
                    verb_schliessen(Verbs[i], "ersetzt durch neue Verbindung desselben Geraets");
        } else {
            snprintf(v->geraet, sizeof v->geraet, "%s", inet_ntoa(v->ip));
        }
        antwort(v, x, NULL);
        zustand_ereignis(v, "SystemStatus", NULL);
        zustand_ereignis(v, "TVSystemStatus", NULL);
        if (!v->sitzung) frist_setzen(v, (opack_zahl(x, 0) & 0x80000000LL) ? 1.0 : 2.5);
    } else if (!strcmp(id, "_sessionStart")) {
        sitzung_an(v);
        ein_paar(&r, "_sid", sid_fuer(v->geraet[0] ? v->geraet : inet_ntoa(v->ip)));
        antwort(v, x, &r);
        op_frei(&r);
    } else if (!strcmp(id, "TVRCSessionStart")) {
        const char *ver = opack_text(opack_hole(c, "ProtocolVersionKey"));
        sitzung_an(v);
        op_init(&r);
        op_dict(&r, 1);
        op_str(&r, "ProtocolVersionKey"); op_str(&r, ver ? ver : "1.2");
        antwort(v, x, &r);
        op_frei(&r);
        medien_ereignisse(v, x, 0);
    } else if (!strcmp(id, "_sessionStop")) {
        v->sitzung = 0;
        zeiger_programm(0);
        antwort(v, x, NULL);
    } else if (!strcmp(id, "_hidC")) {
        sitzung_an(v);
        antwort(v, x, NULL);
        knopf(v, (int)opack_zahl(opack_hole(c, "_hidC"), 0), (int)opack_zahl(opack_hole(c, "_hBtS"), 0), x);
        fokus_spaeter();
    } else if (!strcmp(id, "_touchStart")) {
        sitzung_an(v);
        ein_paar(&r, "_i", 1);
        antwort(v, x, &r);
        op_frei(&r);
    } else if (!strcmp(id, "_hidT")) {
        sitzung_an(v);
        antwort(v, x, NULL);
        beruehrung(v, c);
        if (opack_zahl(opack_hole(c, "_tPh"), 0) == 4) fokus_spaeter();
    } else if (!strcmp(id, "_touchStop") || !strcmp(id, "_touchMove")) {
        sitzung_an(v);
        antwort(v, x, NULL);
        if (!strcmp(id, "_touchStop")) zeiger_programm(0);   /* Fernbedienung am iPhone zu */
    } else if (!strcmp(id, "_tiStop")) {
        v->ti_abo = 0;
        antwort(v, x, NULL);
    } else if (!strcmp(id, "_tiC")) {
        tastatur_eingabe(c);
    } else if (!strcmp(id, "_interest")) {
        const OWert *reg = opack_hole(c, "_regEvents");
        if (reg && reg->art == O_ARRAY)
            for (i = 0; i < reg->m; i++) {
                const char *e = opack_text(reg->k[i]);
                if (!e) continue;
                if (!strcmp(e, "MediaControlStatus")) {
                    ein_paar(&r, "MediaControlFlags", MC_LAUT);
                    ereignis(v, e, x, &r);
                    op_frei(&r);
                } else if (!strcmp(e, "NowPlayingInfo")) {
                    ereignis(v, e, x, NULL);
                } else if (!strcmp(e, "SystemStatus") || !strcmp(e, "TVSystemStatus")) {
                    zustand_ereignis(v, e, x);
                }
            }
    } else if (!strcmp(id, "FetchMediaControlStatus")) {
        ein_paar(&r, "MediaControlFlags", MC_LAUT);
        antwort(v, x, &r);
        op_frei(&r);
    } else if (!strcmp(id, "_mcc") || !strcmp(id, "MediaControlCommand")) {
        medien(v, c, x);
    } else if (!strcmp(id, "_tiStart")) {
        /* Tastatur-Abo; ist gerade ein Textfeld aktiv, gleich dessen Archiv */
        unsigned char *a = NULL;
        int n = fokus_pruefen(&a);
        v->ti_abo = 1;
        op_init(&r);
        op_dict(&r, 1);
        if (n) { op_str(&r, "_tiD"); op_data(&r, a, n); }
        else { op_str(&r, "_tiE"); op_bool(&r, 0); }
        antwort(v, x, &r);
        op_frei(&r);
        free(a);
    } else if (!strcmp(id, "FetchAttentionState")) {
        ein_paar(&r, "state", 3);
        antwort(v, x, &r);
        op_frei(&r);
    } else if (x && opack_zahl(opack_hole(w, "_t"), 0) == 2) {
        antwort(v, x, NULL);   /* nur Anfragen bekommen eine Antwort, Ereignisse nicht */
    }
}

/* --- Rahmen lesen --------------------------------------------------- */
static void rahmen_verarbeiten(Verb *v, int typ, const unsigned char *kopf, unsigned char *d, int n)
{
    unsigned char *klar = NULL;
    OWert *w;
    OBaum *t;
    if (typ == 8) {
        unsigned char nonce[12];
        if (!v->verschl || n < 16) return;
        klar = malloc(n);
        nonce_zaehler(nonce, v->z_ein);
        if (chacha_auf(klar, v->k_ein, nonce, kopf, 4, d, n)) { free(klar); meld("Rahmen nicht entschluesselbar"); v->zu = 1; return; }
        v->z_ein++;
        d = klar;
        n -= 16;
    }
    if (n > 0) {
        t = opack_lesen(d, n, &w);
        if (w && w->art == O_DICT) {
            if (typ >= 3 && typ <= 6) kopplung(v, typ, w);
            else if (typ == 8) steuerung(v, w);
        }
        opack_frei(t);
    }
    free(klar);
}

static void lesen(CFSocketRef s, CFSocketCallBackType art, CFDataRef adr, const void *daten, void *info)
{
    Verb *v = info;
    unsigned char b[4096];
    ssize_t r;
    (void)s; (void)art; (void)adr; (void)daten;
    r = recv(v->fd, b, sizeof b, 0);
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (r <= 0) { verb_schliessen(v, r == 0 ? "vom iPhone geschlossen" : strerror(errno)); return; }
    if (v->art == A_MRP) return;
    if (v->art == A_AIRPLAY) {
        static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nOK";
        senden(v, (const unsigned char *)ok, sizeof ok - 1);
        verb_schliessen(v, "AirPlay beantwortet");
        return;
    }
    if (v->nein + r > v->capein) {
        while (v->nein + r > v->capein) v->capein = v->capein ? v->capein * 2 : 8192;
        v->ein = realloc(v->ein, v->capein);
    }
    memcpy(v->ein + v->nein, b, r);
    v->nein += (int)r;
    while (v->nein >= 4 && !v->zu) {
        unsigned char kopf[4];
        int l = (v->ein[1] << 16) | (v->ein[2] << 8) | v->ein[3];
        if (v->ein[0] < 1 || v->ein[0] > 9 || l > MAX_RAHMEN) { meld("unbekannter Rahmen Typ %d, Laenge %d", v->ein[0], l); v->zu = 1; break; }
        if (v->nein < 4 + l) break;
        memcpy(kopf, v->ein, 4);
        rahmen_verarbeiten(v, kopf[0], kopf, v->ein + 4, l);
        memmove(v->ein, v->ein + 4 + l, v->nein - 4 - l);
        v->nein -= 4 + l;
    }
    if (v->zu) verb_schliessen(v, "Fehler im Rahmen");
}

static void annehmen(CFSocketRef s, CFSocketCallBackType art, CFDataRef adr, const void *daten, void *info)
{
    int typ = (int)(long)info, fd, i, frei = -1, ja = 1;
    struct sockaddr_in sa;
    socklen_t l = sizeof sa;
    struct timeval tv = { 2, 0 };
    CFSocketContext c = { 0, NULL, NULL, NULL, NULL };
    Verb *v;
    (void)art; (void)adr; (void)daten;
    fd = accept(CFSocketGetNative(s), (struct sockaddr *)&sa, &l);
    if (fd < 0) return;
    if (typ == A_COMPANION)
        for (i = 0; i < MAX_VERB; i++)
            if (Verbs[i] && Verbs[i]->art == A_COMPANION && !Verbs[i]->sitzung && !Verbs[i]->srp_an
                && Verbs[i]->ip.s_addr == sa.sin_addr.s_addr)
                verb_schliessen(Verbs[i], "ersetzt durch neue Verbindung");
    for (i = 0; i < MAX_VERB; i++) if (!Verbs[i]) { frei = i; break; }
    if (frei < 0) { close(fd); return; }
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &ja, sizeof ja);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &ja, sizeof ja);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &ja, sizeof ja);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    v = calloc(1, sizeof *v);
    v->fd = fd;
    v->art = typ;
    v->ip = sa.sin_addr;
    c.info = v;
    v->sock = CFSocketCreateWithNative(NULL, fd, kCFSocketReadCallBack, lesen, &c);
    v->quelle = CFSocketCreateRunLoopSource(NULL, v->sock, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), v->quelle, kCFRunLoopDefaultMode);
    Verbs[frei] = v;
    if (typ == A_COMPANION) frist_setzen(v, 30.0);
    if (Laut && typ == A_COMPANION) meld("Verbindung von %s", inet_ntoa(v->ip));   /* ohne Anmeldung nicht ewig offen */
}

static int lauschen(int port, int typ)
{
    struct sockaddr_in sa;
    socklen_t l = sizeof sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0), ja = 1;
    CFSocketContext c = { 0, NULL, NULL, NULL, NULL };
    CFSocketRef s;
    CFRunLoopSourceRef q;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &ja, sizeof ja);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa)) {
        sa.sin_port = 0;
        if (bind(fd, (struct sockaddr *)&sa, sizeof sa)) { meld("bind: %s", strerror(errno)); exit(1); }
    }
    listen(fd, 8);
    getsockname(fd, (struct sockaddr *)&sa, &l);
    c.info = (void *)(long)typ;
    s = CFSocketCreateWithNative(NULL, fd, kCFSocketReadCallBack, annehmen, &c);
    q = CFSocketCreateRunLoopSource(NULL, s, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), q, kCFRunLoopDefaultMode);
    return ntohs(sa.sin_port);
}

/* --- eigenes Bonjour (mDNS nach RFC 6762, nur IPv4) ------------------ */
/* Beantwortet Anfragen nach den drei Diensten und dem Rechnernamen, kuendigt sie bei
   Start und Netzwechsel an und meldet sie beim Beenden ab. Der Systemdienst wird nicht gebraucht. */
#define MDNS_PORT 5353
#define T_LANG 4500
#define T_KURZ 120
enum { R_PTR = 1, R_SRV = 2, R_TXT = 4, R_ENUM = 8 };

typedef struct {
    int port, ntxt;
    unsigned char txt[400];
    char typ_lokal[48], inst[160], label[100];
} MDienst;

typedef struct { unsigned char b[1500]; int n, an, ar, voll; } Paket;

#define N_DIENSTE 4
static MDienst MD[N_DIENSTE];
static char Inst[64], Host_lokal[80];
static int Mfd = -1, Signalrohr[2];
static struct in_addr Mgruppe;

static void txt_dazu(MDienst *d, const char *k, const char *v)
{
    int l = (int)(strlen(k) + 1 + strlen(v));
    if (l > 255 || d->ntxt + 2 + l > (int)sizeof d->txt) return;
    d->txt[d->ntxt++] = (unsigned char)l;
    sprintf((char *)d->txt + d->ntxt, "%s=%s", k, v);
    d->ntxt += l;
}

static void p_bytes(Paket *p, const void *d, int n)
{
    if (p->n + n > (int)sizeof p->b) { p->voll = 1; return; }
    memcpy(p->b + p->n, d, n);
    p->n += n;
}

static void p_u16(Paket *p, unsigned v) { unsigned char b[2]; b[0] = (unsigned char)(v >> 8); b[1] = (unsigned char)v; p_bytes(p, b, 2); }
static void p_u32(Paket *p, unsigned v) { p_u16(p, v >> 16); p_u16(p, v & 0xFFFF); }

/* Name: optional ein freies erstes Label (Instanz), dann punktgetrennte Labels */
static void p_name(Paket *p, const char *erst, const char *rest)
{
    unsigned char l;
    if (erst) { l = (unsigned char)strlen(erst); p_bytes(p, &l, 1); p_bytes(p, erst, l); }
    while (*rest) {
        const char *pkt = strchr(rest, '.');
        int n = pkt ? (int)(pkt - rest) : (int)strlen(rest);
        l = (unsigned char)n;
        p_bytes(p, &l, 1);
        p_bytes(p, rest, n);
        rest += n;
        if (*rest == '.') rest++;
    }
    l = 0;
    p_bytes(p, &l, 1);
}

static int p_rr(Paket *p, int typ, int eindeutig, unsigned ttl)
{
    int pos;
    p_u16(p, typ);
    p_u16(p, 1 | (eindeutig ? 0x8000 : 0));
    p_u32(p, ttl);
    pos = p->n;
    p_u16(p, 0);
    return pos;
}

static void p_rr_ende(Paket *p, int pos)
{
    int l = p->n - pos - 2;
    if (!p->voll) { p->b[pos] = (unsigned char)(l >> 8); p->b[pos + 1] = (unsigned char)l; }
}

static void rr_enum(Paket *p, const MDienst *d, unsigned ttl)
{
    int s;
    p_name(p, NULL, "_services._dns-sd._udp.local");
    s = p_rr(p, 12, 0, ttl);
    p_name(p, NULL, d->typ_lokal);
    p_rr_ende(p, s);
}

static void rr_ptr(Paket *p, const MDienst *d, unsigned ttl)
{
    int s;
    p_name(p, NULL, d->typ_lokal);
    s = p_rr(p, 12, 0, ttl);
    p_name(p, d->label, d->typ_lokal);
    p_rr_ende(p, s);
}

static void rr_srv(Paket *p, const MDienst *d, unsigned ttl)
{
    int s;
    p_name(p, d->label, d->typ_lokal);
    s = p_rr(p, 33, 1, ttl);
    p_u16(p, 0);
    p_u16(p, 0);
    p_u16(p, d->port);
    p_name(p, NULL, Host_lokal);
    p_rr_ende(p, s);
}

static void rr_txt(Paket *p, const MDienst *d, unsigned ttl)
{
    int s;
    p_name(p, d->label, d->typ_lokal);
    s = p_rr(p, 16, 1, ttl);
    p_bytes(p, d->txt, d->ntxt);
    p_rr_ende(p, s);
}

static void rr_a(Paket *p, struct in_addr ip, unsigned ttl)
{
    int s;
    p_name(p, NULL, Host_lokal);
    s = p_rr(p, 1, 1, ttl);
    p_bytes(p, &ip.s_addr, 4);
    p_rr_ende(p, s);
}

/* Antwortpaket: ans[] je Dienst (R_*), a = A-Satz; Zusatzsaetze folgen von selbst. gehen: TTL 0 */
static void paket_bauen(Paket *p, const int ans[N_DIENSTE], int a, struct in_addr ip, int gehen)
{
    int zus[N_DIENSTE] = { 0 }, zus_a = 0, k;
    unsigned lang = gehen ? 0 : T_LANG, kurz = gehen ? 0 : T_KURZ;
    memset(p, 0, sizeof *p);
    p->n = 12;
    for (k = 0; k < N_DIENSTE; k++) if (ans[k] & R_ENUM) { rr_enum(p, &MD[k], lang); p->an++; }
    for (k = 0; k < N_DIENSTE; k++) if (ans[k] & R_PTR) { rr_ptr(p, &MD[k], lang); p->an++; zus[k] |= R_SRV | R_TXT; }
    for (k = 0; k < N_DIENSTE; k++) if (ans[k] & R_SRV) { rr_srv(p, &MD[k], kurz); p->an++; zus_a = 1; }
    for (k = 0; k < N_DIENSTE; k++) if (ans[k] & R_TXT) { rr_txt(p, &MD[k], lang); p->an++; }
    if (a) { rr_a(p, ip, kurz); p->an++; }
    for (k = 0; k < N_DIENSTE; k++) {
        if ((zus[k] & R_SRV) && !(ans[k] & R_SRV)) { rr_srv(p, &MD[k], kurz); p->ar++; zus_a = 1; }
        if ((zus[k] & R_TXT) && !(ans[k] & R_TXT)) { rr_txt(p, &MD[k], lang); p->ar++; }
    }
    if (zus_a && !a) { rr_a(p, ip, kurz); p->ar++; }
    p->b[2] = 0x84;   /* Antwort, massgeblich */
    p->b[6] = (unsigned char)(p->an >> 8); p->b[7] = (unsigned char)p->an;
    p->b[10] = (unsigned char)(p->ar >> 8); p->b[11] = (unsigned char)p->ar;
}

static void paket_senden(const Paket *p, struct in_addr ueber, const struct sockaddr_in *an)
{
    struct sockaddr_in ziel;
    if (p->voll) { meld("Bonjour-Paket zu gross"); return; }
    if (!an) {
        memset(&ziel, 0, sizeof ziel);
        ziel.sin_family = AF_INET;
        ziel.sin_port = htons(MDNS_PORT);
        ziel.sin_addr = Mgruppe;
        an = &ziel;
    }
    setsockopt(Mfd, IPPROTO_IP, IP_MULTICAST_IF, &ueber, sizeof ueber);
    sendto(Mfd, p->b, p->n, 0, (const struct sockaddr *)an, sizeof *an);
}

/* IPv4-Schnittstellen mit Multicast */
static int schnittstellen(struct in_addr *adr, struct in_addr *maske, int max)
{
    struct ifaddrs *l, *i;
    int n = 0;
    if (getifaddrs(&l)) return 0;
    for (i = l; i && n < max; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_MULTICAST) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        adr[n] = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
        if (i->ifa_netmask) maske[n] = ((struct sockaddr_in *)i->ifa_netmask)->sin_addr;
        else maske[n].s_addr = htonl(0xFFFFFF00);
        n++;
    }
    freeifaddrs(l);
    return n;
}

static int eigene_adresse(struct in_addr von, struct in_addr *eigen)
{
    struct in_addr a[8], m[8];
    int n = schnittstellen(a, m, 8), i;
    for (i = 0; i < n; i++)
        if ((von.s_addr & m[i].s_addr) == (a[i].s_addr & m[i].s_addr)) { *eigen = a[i]; return 1; }
    return 0;
}

static void beitreten(void)
{
    struct in_addr a[8], m[8];
    int n = schnittstellen(a, m, 8), i;
    for (i = 0; i < n; i++) {
        struct ip_mreq r;
        r.imr_multiaddr = Mgruppe;
        r.imr_interface = a[i];
        setsockopt(Mfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &r, sizeof r);   /* schon Mitglied: egal */
    }
}

static void alle_senden(int gehen)
{
    struct in_addr a[8], m[8];
    int n = schnittstellen(a, m, 8), i, k;
    for (i = 0; i < n; i++)
        for (k = 0; k < N_DIENSTE; k++) {
            Paket p;
            int ans[N_DIENSTE] = { 0 };
            ans[k] = R_PTR | R_SRV | R_TXT | (gehen ? 0 : R_ENUM);
            paket_bauen(&p, ans, 1, a[i], gehen);
            paket_senden(&p, a[i], NULL);
        }
}

static void zweite_ankuendigung(CFRunLoopTimerRef t, void *i) { (void)t; (void)i; alle_senden(0); }

static void ankuendigen(void)
{
    CFRunLoopTimerRef t;
    alle_senden(0);
    t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1.0, 0, 0, 0, zweite_ankuendigung, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), t, kCFRunLoopDefaultMode);
    CFRelease(t);   /* einmalig, die Laufschleife haelt ihn bis zum Ausloesen */
}

static int name_lesen(const unsigned char *b, int n, int *pos, char *aus, int max)
{
    int p = *pos, o = 0, gesprungen = 0, schritte = 0;
    for (;;) {
        int l;
        if (p >= n || ++schritte > 128) return -1;
        l = b[p];
        if ((l & 0xC0) == 0xC0) {
            if (p + 1 >= n) return -1;
            if (!gesprungen) *pos = p + 2;
            gesprungen = 1;
            p = ((l & 0x3F) << 8) | b[p + 1];
            continue;
        }
        if (l & 0xC0) return -1;
        p++;
        if (l == 0) break;
        if (p + l > n || o + l + 2 > max) return -1;
        if (o) aus[o++] = '.';
        memcpy(aus + o, b + p, l);
        o += l;
        p += l;
    }
    if (!gesprungen) *pos = p;
    aus[o] = 0;
    return o;
}

static void mdns_lesen(CFSocketRef s, CFSocketCallBackType art, CFDataRef adr, const void *daten, void *info)
{
    unsigned char b[9000];
    char name[300];
    struct sockaddr_in von;
    socklen_t l = sizeof von;
    struct in_addr eigen;
    int n, pos = 12, qd, q, k, uni = 0, ans[N_DIENSTE] = { 0 }, a = 0;
    Paket p;
    (void)s; (void)art; (void)adr; (void)daten; (void)info;
    n = (int)recvfrom(Mfd, b, sizeof b, 0, (struct sockaddr *)&von, &l);
    if (n < 12 || (b[2] & 0xF8)) return;                 /* nur Anfragen, Opcode 0 */
    if (ntohs(von.sin_port) != MDNS_PORT) return;
    if (!eigene_adresse(von.sin_addr, &eigen)) return;
    qd = (b[4] << 8) | b[5];
    for (q = 0; q < qd; q++) {
        int typ, kl, frage = 0;
        if (name_lesen(b, n, &pos, name, sizeof name) < 0 || pos + 4 > n) return;
        typ = (b[pos] << 8) | b[pos + 1];
        kl = (b[pos + 2] << 8) | b[pos + 3];
        pos += 4;
        if ((kl & 0x7FFF) != 1 && (kl & 0x7FFF) != 255) continue;
        if (!strcasecmp(name, "_services._dns-sd._udp.local") && (typ == 12 || typ == 255)) {
            for (k = 0; k < N_DIENSTE; k++) ans[k] |= R_ENUM;
            frage = 1;
        }
        for (k = 0; k < N_DIENSTE; k++) {
            if (!strcasecmp(name, MD[k].typ_lokal) && (typ == 12 || typ == 255)) { ans[k] |= R_PTR; frage = 1; }
            if (!strcasecmp(name, MD[k].inst)) {
                if (typ == 33 || typ == 255) { ans[k] |= R_SRV; frage = 1; }
                if (typ == 16 || typ == 255) { ans[k] |= R_TXT; frage = 1; }
            }
        }
        if (!strcasecmp(name, Host_lokal) && (typ == 1 || typ == 255)) { a = 1; frage = 1; }
        if (frage && (kl & 0x8000)) uni = 1;
    }
    if (!(ans[0] | ans[1] | ans[2] | ans[3] | a)) return;
    paket_bauen(&p, ans, a, eigen, 0);
    paket_senden(&p, eigen, uni ? &von : NULL);
}

static void netz_geaendert(SCDynamicStoreRef s, CFArrayRef k, void *i)
{
    (void)s; (void)k; (void)i;
    beitreten();
    ankuendigen();
}

static void signal_ab(int s)
{
    char c = 1;
    (void)s;
    write(Signalrohr[1], &c, 1);
}

static void beenden(CFSocketRef s, CFSocketCallBackType art, CFDataRef adr, const void *daten, void *info)
{
    (void)s; (void)art; (void)adr; (void)daten; (void)info;
    alle_senden(1);   /* abmelden */
    meld("beendet");
    exit(0);
}

static void quelle_dazu(int fd, CFSocketCallBack f)
{
    CFSocketRef s = CFSocketCreateWithNative(NULL, fd, kCFSocketReadCallBack, f, NULL);
    CFRunLoopSourceRef q = CFSocketCreateRunLoopSource(NULL, s, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), q, kCFRunLoopDefaultMode);
}

static void kurz_hash(char *o, const char *vorsatz, int n, int gross)
{
    unsigned char h[64];
    crypto_sha512_ctx c;
    crypto_sha512_init(&c);
    crypto_sha512_update(&c, (const unsigned char *)vorsatz, strlen(vorsatz));
    crypto_sha512_update(&c, (const unsigned char *)Ich_id, strlen(Ich_id));
    crypto_sha512_final(&c, h);
    hexen(o, h, n, gross);
}

static void bonjour(int p_comp, int p_mrp, int p_air)
{
    char mac[20], ha[16], hn[16], ad[16], hi[16], apid[20], uid[40], pk[70], host[64];
    const char *typen[N_DIENSTE] = { "_companion-link._tcp.local", "_mediaremotetv._tcp.local", "_airplay._tcp.local", "_raop._tcp.local" };
    int ports[N_DIENSTE], i, j, ja = 1, ttl = 255;
    unsigned char mttl = 255, schleife = 0;
    struct sockaddr_in sa;
    CFStringRef hn_cf;
    SCDynamicStoreRef st;
    CFStringRef muster;
    CFArrayRef mu;
    CFRunLoopSourceRef q;

    ports[0] = p_comp; ports[1] = p_mrp; ports[2] = ports[3] = p_air;
    for (i = j = 0; Ich_name[i] && j < (int)sizeof Inst - 1; i++) Inst[j++] = Ich_name[i] == '.' ? '-' : Ich_name[i];
    Inst[j] = 0;
    hn_cf = SCDynamicStoreCopyLocalHostName(NULL);
    if (!hn_cf || !CFStringGetCString(hn_cf, host, sizeof host, kCFStringEncodingUTF8) || !host[0]) strcpy(host, "iBook");
    if (hn_cf) CFRelease(hn_cf);
    snprintf(Host_lokal, sizeof Host_lokal, "%s.local", host);
    for (i = 0; i < N_DIENSTE; i++) {
        MD[i].port = ports[i];
        snprintf(MD[i].typ_lokal, sizeof MD[i].typ_lokal, "%s", typen[i]);
        snprintf(MD[i].label, sizeof MD[i].label, "%s", i == 3 ? raop_name() : Inst);
        snprintf(MD[i].inst, sizeof MD[i].inst, "%s.%s", MD[i].label, typen[i]);
    }

    snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X", Ich_mac[0], Ich_mac[1], Ich_mac[2], Ich_mac[3], Ich_mac[4], Ich_mac[5]);
    hexen(ha, Ich_mac, 6, 1);
    kurz_hash(hn, "rpHN:", 6, 1);
    kurz_hash(ad, "rpAD:", 6, 1);
    kurz_hash(hi, "rpHI:", 6, 1);
    kurz_hash(apid, "airplay-identity:", 8, 1);
    for (i = j = 0; Ich_id[i]; i++) if (Ich_id[i] != '-') uid[j++] = Ich_id[i];
    uid[j] = 0;
    hexen(pk, Ich_pk, 32, 0);

    txt_dazu(&MD[0], "rpMac", "1");
    txt_dazu(&MD[0], "rpHA", ha);
    txt_dazu(&MD[0], "rpHN", hn);
    txt_dazu(&MD[0], "rpVr", VERSION);
    txt_dazu(&MD[0], "rpMd", MODELL);
    txt_dazu(&MD[0], "rpFl", "0x36782");
    txt_dazu(&MD[0], "rpAD", ad);
    txt_dazu(&MD[0], "rpHI", hi);
    txt_dazu(&MD[0], "rpBA", mac);

    txt_dazu(&MD[1], "Name", Ich_name);
    txt_dazu(&MD[1], "UniqueIdentifier", uid);
    txt_dazu(&MD[1], "SystemBuildVersion", "22K160");
    txt_dazu(&MD[1], "LocalAirPlayReceiverPairingIdentity", apid);
    txt_dazu(&MD[1], "ModelName", "Apple TV");
    txt_dazu(&MD[1], "AllowPairing", "YES");

    /* AirPlay und RAOP: Eintraege kommen vom AirPlay-Empfaenger (Legacy-Pairing, wie UxPlay) */
    MD[2].ntxt = airplay_txt(MD[2].txt, sizeof MD[2].txt);
    MD[3].ntxt = raop_txt(MD[3].txt, sizeof MD[3].txt);
    (void)pk;

    inet_aton("224.0.0.251", &Mgruppe);
    Mfd = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(Mfd, SOL_SOCKET, SO_REUSEADDR, &ja, sizeof ja);
    setsockopt(Mfd, SOL_SOCKET, SO_REUSEPORT, &ja, sizeof ja);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(MDNS_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(Mfd, (struct sockaddr *)&sa, sizeof sa)) { meld("Bonjour-Port: %s", strerror(errno)); exit(1); }
    setsockopt(Mfd, IPPROTO_IP, IP_MULTICAST_TTL, &mttl, sizeof mttl);
    setsockopt(Mfd, IPPROTO_IP, IP_MULTICAST_LOOP, &schleife, sizeof schleife);
    setsockopt(Mfd, IPPROTO_IP, IP_TTL, &ttl, sizeof ttl);
    beitreten();
    quelle_dazu(Mfd, mdns_lesen);

    /* Netzwechsel (WLAN neu, Aufwachen): neu beitreten und ankuendigen */
    st = SCDynamicStoreCreate(NULL, CFSTR("fernbedienung"), netz_geaendert, NULL);
    muster = SCDynamicStoreKeyCreateNetworkInterfaceEntity(NULL, kSCDynamicStoreDomainState, kSCCompAnyRegex, kSCEntNetIPv4);
    mu = CFArrayCreate(NULL, (const void **)&muster, 1, &kCFTypeArrayCallBacks);
    SCDynamicStoreSetNotificationKeys(st, NULL, mu);
    q = SCDynamicStoreCreateRunLoopSource(NULL, st, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), q, kCFRunLoopDefaultMode);
    CFRelease(mu); CFRelease(muster);

    /* Beenden (Abmelden, launchd): erst abmelden, dann Ende */
    pipe(Signalrohr);
    quelle_dazu(Signalrohr[0], beenden);
    signal(SIGTERM, signal_ab);
    signal(SIGINT, signal_ab);

    ankuendigen();
}

int main(int argc, char **argv)
{
    int p_comp, p_mrp, p_air;
    char *s;
    (void)argc;
    signal(SIGPIPE, SIG_IGN);
    {
        /* Protokoll klein halten: ueber 1 MB beim Start leeren (launchd haengt an) */
        struct stat st;
        if (fstat(2, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > (1 << 20)) ftruncate(2, 0);
    }
    snprintf(Ordner, sizeof Ordner, "%s", argv[0]);
    s = strrchr(Ordner, '/');
    if (s) *s = 0; else strcpy(Ordner, ".");
    Laut = CFPreferencesGetAppBooleanValue(CFSTR("Ausfuehrlich"), APP, NULL);
    Mausmodus = CFPreferencesGetAppBooleanValue(CFSTR("Mausmodus"), APP, NULL);
    identitaet_laden();
    p_comp = lauschen(49153, A_COMPANION);
    p_mrp = lauschen(49152, A_MRP);
    {
        char spiegel[1100];
        snprintf(spiegel, sizeof spiegel, "%s/Spiegel.app/Contents/MacOS/Spiegel", Ordner);
        p_air = airplay_start(7000, Ich_sk, Ich_pk, Ich_mac, Ich_name, Ich_id, spiegel, meld, Laut);
        if (p_air < 0) { meld("AirPlay: kein Port"); exit(1); }
    }
    bonjour(p_comp, p_mrp, p_air);
    meld("bereit als \"%s\" (%s), Port %d, %d gekoppelte Geraete", Ich_name, Host_lokal, p_comp, nGeraete);
    CFRunLoopRun();
    return 0;
}
