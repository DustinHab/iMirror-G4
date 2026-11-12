/* geraet: spricht mit dem iPhone ueber USB (usbmuxd) oder WLAN (Lockdown, Port 62078).
   Kopplung wie iTunes, Entwicklermodus-Schalter sichtbar machen, WLAN-Verbindungen erlauben.
   Aufruf: geraet [-w adresse] info | koppeln | entwickler | wlan
   Kopplungsdaten: ~/tools/fernbedienung/iphone/paarung.plist */
#include <CoreFoundation/CoreFoundation.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <signal.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <sys/select.h>
#include <zlib.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include "tunnel.h"
#include "xpc.h"

#if OPENSSL_VERSION_NUMBER >= 0x00908000L
#define HASH() EVP_sha256()
#else
#define HASH() EVP_sha1()
#endif

#define LOCKDOWN 62078
#define ETIKETT "geraet"

static const char *Wlan;
static char Paar_pfad[1024];
static CFMutableDictionaryRef Paar;
static int Laut;

static void meld(const char *f, ...)
{
    va_list a;
    va_start(a, f);
    vfprintf(stderr, f, a);
    va_end(a);
    fputc('\n', stderr);
}

/* ---------- Plist ---------- */

static CFMutableDictionaryRef d_neu(void)
{
    return CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}

static CFStringRef s(const char *c) { return CFStringCreateWithCString(NULL, c, kCFStringEncodingUTF8); }

static void d_obj(CFMutableDictionaryRef d, const char *k, CFTypeRef v)
{
    CFStringRef ks = s(k);
    CFDictionarySetValue(d, ks, v);
    CFRelease(ks);
}

static void d_str(CFMutableDictionaryRef d, const char *k, const char *v)
{
    CFStringRef vs = s(v);
    d_obj(d, k, vs);
    CFRelease(vs);
}

static void d_int(CFMutableDictionaryRef d, const char *k, long long v)
{
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberLongLongType, &v);
    d_obj(d, k, n);
    CFRelease(n);
}

static CFTypeRef g(CFDictionaryRef d, const char *k)
{
    CFStringRef ks;
    CFTypeRef v;
    if (!d) return NULL;
    ks = s(k);
    v = CFDictionaryGetValue(d, ks);
    CFRelease(ks);
    return v;
}

static int g_str(CFDictionaryRef d, const char *k, char *b, size_t n)
{
    CFTypeRef v = g(d, k);
    b[0] = 0;
    if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return -1;
    return CFStringGetCString(v, b, (CFIndex)n, kCFStringEncodingUTF8) ? 0 : -1;
}

static long long g_int(CFDictionaryRef d, const char *k, long long std)
{
    CFTypeRef v = g(d, k);
    long long x;
    if (!v || CFGetTypeID(v) != CFNumberGetTypeID()) return std;
    CFNumberGetValue(v, kCFNumberLongLongType, &x);
    return x;
}

static int g_bool(CFDictionaryRef d, const char *k)
{
    CFTypeRef v = g(d, k);
    return v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(v);
}

static CFDataRef g_daten(CFDictionaryRef d, const char *k)
{
    CFTypeRef v = g(d, k);
    return v && CFGetTypeID(v) == CFDataGetTypeID() ? v : NULL;
}

static CFPropertyListRef plist_aus(const void *p, size_t n)
{
    CFDataRef x = CFDataCreate(NULL, p, (CFIndex)n);
    CFPropertyListRef r = CFPropertyListCreateFromXMLData(NULL, x, kCFPropertyListMutableContainers, NULL);
    CFRelease(x);
    return r;
}

/* einfache Werte als Text; Rueckgabe 0 wenn darstellbar */
static int wert_text(CFTypeRef v, char *b, size_t n)
{
    CFTypeID t = CFGetTypeID(v);
    if (t == CFStringGetTypeID()) return CFStringGetCString(v, b, (CFIndex)n, kCFStringEncodingUTF8) ? 0 : -1;
    if (t == CFBooleanGetTypeID()) { snprintf(b, n, "%s", CFBooleanGetValue(v) ? "ja" : "nein"); return 0; }
    if (t == CFNumberGetTypeID()) {
        if (CFNumberIsFloatType(v)) { double x; CFNumberGetValue(v, kCFNumberDoubleType, &x); snprintf(b, n, "%g", x); }
        else { long long x; CFNumberGetValue(v, kCFNumberLongLongType, &x); snprintf(b, n, "%lld", x); }
        return 0;
    }
    if (t == CFDataGetTypeID()) { snprintf(b, n, "<%ld Bytes>", (long)CFDataGetLength(v)); return 0; }
    if (t == CFArrayGetTypeID()) { snprintf(b, n, "<%ld Eintraege>", (long)CFArrayGetCount(v)); return 0; }
    if (t == CFDictionaryGetTypeID()) { snprintf(b, n, "<%ld Schluessel>", (long)CFDictionaryGetCount(v)); return 0; }
    return -1;
}

static CFComparisonResult vergl(const void *a, const void *b, void *x)
{
    return CFStringCompare(a, b, 0);
}

static void zeigen(CFDictionaryRef d)
{
    CFIndex n = CFDictionaryGetCount(d), i;
    const void **k = malloc(sizeof(void *) * (size_t)(n + 1)), **v = malloc(sizeof(void *) * (size_t)(n + 1));
    CFMutableArrayRef sk = CFArrayCreateMutable(NULL, n, &kCFTypeArrayCallBacks);
    CFDictionaryGetKeysAndValues(d, k, v);
    for (i = 0; i < n; i++) CFArrayAppendValue(sk, k[i]);
    CFArraySortValues(sk, CFRangeMake(0, n), vergl, NULL);
    for (i = 0; i < n; i++) {
        char kb[256], vb[1024];
        CFStringRef ks = CFArrayGetValueAtIndex(sk, i);
        CFTypeRef w = CFDictionaryGetValue(d, ks);
        if (!CFStringGetCString(ks, kb, sizeof kb, kCFStringEncodingUTF8)) continue;
        if (wert_text(w, vb, sizeof vb)) continue;
        printf("%-34s %s\n", kb, vb);
    }
    CFRelease(sk);
    free(k);
    free(v);
}

/* ---------- Kanal: Socket, wahlweise mit TLS ---------- */

typedef struct { int fd; SSL *ssl; } Kanal;

static int k_schreiben(Kanal *k, const void *b, size_t n)
{
    const unsigned char *p = b;
    while (n) {
        int r = k->ssl ? SSL_write(k->ssl, p, (int)n) : (int)write(k->fd, p, n);
        if (r <= 0) {
            if (!k->ssl && r < 0 && errno == EINTR) continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int k_lesen(Kanal *k, void *b, size_t n)
{
    unsigned char *p = b;
    while (n) {
        int r = k->ssl ? SSL_read(k->ssl, p, (int)n) : (int)read(k->fd, p, n);
        if (r <= 0) {
            if (!k->ssl && r < 0 && errno == EINTR) continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static void k_zu(Kanal *k)
{
    if (k->ssl) { SSL_shutdown(k->ssl); SSL_free(k->ssl); k->ssl = NULL; }
    if (k->fd >= 0) close(k->fd);
    k->fd = -1;
}

static void wartezeit(int fd, int sek)
{
    struct timeval t = { sek, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t);
}

/* ---------- usbmuxd ---------- */

/* Kopf der usbmuxd-Nachrichten in Rechner-Bytefolge (Apples usbmuxd auf PowerPC liest Big-Endian) */
static void kopf32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t kopf32_lesen(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static int mux_sock(void)
{
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, "/var/run/usbmuxd");
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    wartezeit(fd, 5);
    return fd;
}

static int mux_senden(int fd, CFMutableDictionaryRef d, uint32_t tag)
{
    CFDataRef x;
    unsigned char h[16];
    Kanal k = { fd, NULL };
    int r;
    d_str(d, "ClientVersionString", ETIKETT);
    d_str(d, "ProgName", ETIKETT);
    x = CFPropertyListCreateXMLData(NULL, d);
    kopf32(h, 16 + (uint32_t)CFDataGetLength(x));
    kopf32(h + 4, 1);
    kopf32(h + 8, 8);
    kopf32(h + 12, tag);
    r = k_schreiben(&k, h, 16) || k_schreiben(&k, CFDataGetBytePtr(x), (size_t)CFDataGetLength(x));
    CFRelease(x);
    return r ? -1 : 0;
}

static CFDictionaryRef mux_empfangen(int fd)
{
    unsigned char h[16], *b;
    uint32_t n;
    Kanal k = { fd, NULL };
    CFPropertyListRef p;
    if (k_lesen(&k, h, 16)) return NULL;
    n = kopf32_lesen(h);
    if (n < 16 || n > (1u << 20)) return NULL;
    n -= 16;
    b = malloc(n + 1);
    if (k_lesen(&k, b, n)) { free(b); return NULL; }
    if (kopf32_lesen(h + 8) != 8) { free(b); return d_neu(); }   /* alte Binaernachricht: ignorieren */
    p = plist_aus(b, n);
    free(b);
    if (p && CFGetTypeID(p) != CFDictionaryGetTypeID()) { CFRelease(p); p = NULL; }
    return p;
}

/* erstes per USB angeschlossenes Geraet; -1 wenn keins */
static int mux_geraet(char *udid, size_t un)
{
    int fd = mux_sock(), id = -1;
    CFMutableDictionaryRef d;
    CFDictionaryRef r;
    if (fd < 0) { meld("usbmuxd laeuft nicht (/var/run/usbmuxd fehlt)"); return -1; }
    d = d_neu();
    d_str(d, "MessageType", "Listen");
    mux_senden(fd, d, 1);
    CFRelease(d);
    wartezeit(fd, 2);
    while ((r = mux_empfangen(fd))) {
        char typ[64];
        g_str(r, "MessageType", typ, sizeof typ);
        if (Laut) { meld("usbmuxd: %s", typ); }
        if (!strcmp(typ, "Result") && g_int(r, "Number", 0) != 0) {
            meld("usbmuxd lehnt ab: %lld", g_int(r, "Number", 0));
            CFRelease(r);
            break;
        }
        if (!strcmp(typ, "Attached")) {
            CFTypeRef pr = g(r, "Properties");
            if (pr && CFGetTypeID(pr) == CFDictionaryGetTypeID()) {
                char art[32];
                g_str(pr, "ConnectionType", art, sizeof art);
                if (!art[0] || !strcmp(art, "USB")) {
                    id = (int)g_int(pr, "DeviceID", g_int(r, "DeviceID", -1));
                    g_str(pr, "SerialNumber", udid, un);
                    if (Laut) zeigen(pr);
                    CFRelease(r);
                    break;
                }
            }
        }
        CFRelease(r);
    }
    close(fd);
    return id;
}

static int mux_verbinden(int id, int port)
{
    int fd = mux_sock();
    CFMutableDictionaryRef d;
    CFDictionaryRef r;
    long long e;
    if (fd < 0) return -1;
    d = d_neu();
    d_str(d, "MessageType", "Connect");
    d_int(d, "DeviceID", id);
    d_int(d, "PortNumber", htons((unsigned short)port));
    mux_senden(fd, d, 2);
    CFRelease(d);
    r = mux_empfangen(fd);
    e = r ? g_int(r, "Number", -1) : -1;
    if (r) CFRelease(r);
    if (e != 0) { meld("usbmuxd: Verbindung zu Port %d abgelehnt (%lld)", port, e); close(fd); return -1; }
    return fd;
}

static int tcp_verbinden(const char *wer, int port)
{
    struct addrinfo h, *a, *i;
    char ps[16];
    int fd = -1;
    memset(&h, 0, sizeof h);
    h.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(wer, ps, &h, &a)) return -1;
    for (i = a; i; i = i->ai_next) {
        int puffer = 262144, an = 1;
        fd = socket(i->ai_family, i->ai_socktype, i->ai_protocol);
        if (fd < 0) continue;
        wartezeit(fd, 10);
        /* WLAN: grosser Empfangspuffer, damit sich der Tunnel nicht staut, wenn eine Abfrage kurz aufhaelt
           (ein Stau bremst die Bildschirmaufnahme des iPhones und laesst die AirPlay-Spiegelung haengen);
           Touch-Berichte ohne Nagle-Verzoegerung senden */
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &puffer, sizeof puffer);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &an, sizeof an);
        if (!connect(fd, i->ai_addr, i->ai_addrlen)) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(a);
    return fd;
}

static int Geraet_id = -2;
static char Geraet_udid[64];

static int geraet_kanal(Kanal *k, int port)
{
    k->ssl = NULL;
    if (Wlan) {
        k->fd = tcp_verbinden(Wlan, port);
        if (k->fd < 0) meld("%s:%d nicht erreichbar", Wlan, port);
    } else {
        if (Geraet_id == -2) {
            Geraet_id = mux_geraet(Geraet_udid, sizeof Geraet_udid);
            if (Geraet_id < 0) meld("kein iPhone am USB");
        }
        k->fd = Geraet_id < 0 ? -1 : mux_verbinden(Geraet_id, port);
    }
    if (k->fd >= 0) wartezeit(k->fd, 30);
    return k->fd < 0 ? -1 : 0;
}

/* ---------- Lockdown ---------- */

static int ld_senden(Kanal *k, CFDictionaryRef d)
{
    CFDataRef x = CFPropertyListCreateXMLData(NULL, d);
    uint32_t n = (uint32_t)CFDataGetLength(x);
    unsigned char h[4] = { n >> 24, n >> 16, n >> 8, n };
    int r = k_schreiben(k, h, 4) || k_schreiben(k, CFDataGetBytePtr(x), n);
    CFRelease(x);
    return r ? -1 : 0;
}

static CFMutableDictionaryRef ld_empfangen(Kanal *k)
{
    unsigned char h[4], *b;
    uint32_t n;
    CFPropertyListRef p;
    if (k_lesen(k, h, 4)) return NULL;
    n = (uint32_t)h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3];
    if (n > (16u << 20)) return NULL;
    b = malloc(n + 1);
    if (k_lesen(k, b, n)) { free(b); return NULL; }
    p = plist_aus(b, n);
    free(b);
    if (p && CFGetTypeID(p) != CFDictionaryGetTypeID()) { CFRelease(p); p = NULL; }
    return (CFMutableDictionaryRef)p;
}

static CFMutableDictionaryRef ld_anfrage(const char *was)
{
    CFMutableDictionaryRef d = d_neu();
    d_str(d, "Label", ETIKETT);
    d_str(d, "Request", was);
    return d;
}

/* sendet d (und gibt es frei), liefert Antwort; meldet Error */
static CFMutableDictionaryRef ld_frage(Kanal *k, CFMutableDictionaryRef d, int still)
{
    CFMutableDictionaryRef r = NULL;
    char e[128], w[64];
    g_str(d, "Request", w, sizeof w);
    if (!ld_senden(k, d)) r = ld_empfangen(k);
    CFRelease(d);
    if (!r) { meld("Lockdown %s: keine Antwort", w); return NULL; }
    if (!g_str(r, "Error", e, sizeof e) && !still) meld("Lockdown %s: %s", w, e);
    return r;
}

static CFTypeRef ld_wert(Kanal *k, const char *domain, const char *schl)
{
    CFMutableDictionaryRef d = ld_anfrage("GetValue"), r;
    CFTypeRef v;
    if (domain) d_str(d, "Domain", domain);
    if (schl) d_str(d, "Key", schl);
    r = ld_frage(k, d, 0);
    if (!r) return NULL;
    v = g(r, "Value");
    if (v) CFRetain(v);
    CFRelease(r);
    return v;
}

static int ld_typ(Kanal *k)
{
    CFMutableDictionaryRef r = ld_frage(k, ld_anfrage("QueryType"), 0);
    char t[128];
    if (!r) return -1;
    g_str(r, "Type", t, sizeof t);
    CFRelease(r);
    if (strcmp(t, "com.apple.mobile.lockdown")) { meld("unerwarteter Dienst: %s", t); return -1; }
    return 0;
}

/* ---------- Kopplungsdaten ---------- */

static int paar_laden(void)
{
    FILE *f = fopen(Paar_pfad, "rb");
    unsigned char *b;
    long n;
    CFPropertyListRef p;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return -1; }
    fclose(f);
    p = plist_aus(b, (size_t)n);
    free(b);
    if (!p || CFGetTypeID(p) != CFDictionaryGetTypeID()) { if (p) CFRelease(p); return -1; }
    Paar = (CFMutableDictionaryRef)p;
    return 0;
}

static int paar_speichern(void)
{
    CFDataRef x = CFPropertyListCreateXMLData(NULL, Paar);
    char tmp[1100];
    FILE *f;
    int ok;
    snprintf(tmp, sizeof tmp, "%s.neu", Paar_pfad);
    f = fopen(tmp, "wb");
    if (!f) { CFRelease(x); return -1; }
    chmod(tmp, 0600);
    ok = fwrite(CFDataGetBytePtr(x), 1, (size_t)CFDataGetLength(x), f) == (size_t)CFDataGetLength(x);
    ok = !fclose(f) && ok;
    CFRelease(x);
    if (!ok || rename(tmp, Paar_pfad)) return -1;
    return 0;
}

static void uuid_text(char *b, size_t n)
{
    CFUUIDRef u = CFUUIDCreate(NULL);
    CFStringRef t = CFUUIDCreateString(NULL, u);
    CFStringGetCString(t, b, (CFIndex)n, kCFStringEncodingASCII);
    CFRelease(t);
    CFRelease(u);
}

/* ---------- Zertifikate (wie libimobiledevice) ---------- */

static EVP_PKEY *schluessel_neu(void)
{
    EVP_PKEY *p = EVP_PKEY_new();
    EVP_PKEY_assign_RSA(p, RSA_generate_key(2048, 65537, NULL, NULL));
    return p;
}

static void erweiterung(X509 *x, X509 *aussteller, int nid, const char *wert)
{
    X509V3_CTX c;
    X509_EXTENSION *e;
    X509V3_set_ctx(&c, aussteller, x, NULL, NULL, 0);
    e = X509V3_EXT_conf_nid(NULL, &c, nid, (char *)wert);
    if (e) { X509_add_ext(x, e, -1); X509_EXTENSION_free(e); }
}

static X509 *zertifikat(EVP_PKEY *oeff, X509 *aussteller, EVP_PKEY *signierer)
{
    X509 *x = X509_new();
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 0);
    X509_gmtime_adj(X509_get_notBefore(x), 0);
    X509_gmtime_adj(X509_get_notAfter(x), 60L * 60 * 24 * 365 * 10);
    X509_set_pubkey(x, oeff);
    if (!aussteller) {
        erweiterung(x, x, NID_basic_constraints, "critical,CA:TRUE");
    } else {
        X509_set_issuer_name(x, X509_get_subject_name(aussteller));
        erweiterung(x, aussteller, NID_basic_constraints, "critical,CA:FALSE");
        erweiterung(x, aussteller, NID_subject_key_identifier, "hash");
        erweiterung(x, aussteller, NID_key_usage, "critical,digitalSignature,keyEncipherment");
    }
    X509_sign(x, signierer, HASH());
    return x;
}

static CFDataRef bio_daten(BIO *b)
{
    char *p;
    long n = BIO_get_mem_data(b, &p);
    CFDataRef d = CFDataCreate(NULL, (UInt8 *)p, n);
    BIO_free(b);
    return d;
}

static CFDataRef pem_zert(X509 *x)
{
    BIO *b = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(b, x);
    return bio_daten(b);
}

static CFDataRef pem_schl(EVP_PKEY *k)
{
    BIO *b = BIO_new(BIO_s_mem());
    RSA *r = EVP_PKEY_get1_RSA(k);
    PEM_write_bio_RSAPrivateKey(b, r, NULL, NULL, 0, NULL, NULL);
    RSA_free(r);
    return bio_daten(b);
}

static void paar_obj(const char *k, CFDataRef v)
{
    d_obj(Paar, k, v);
    CFRelease(v);
}

/* neue Kopplungsdaten aus dem oeffentlichen Schluessel des Geraets */
static int paar_erzeugen(CFDataRef geraet_pem)
{
    BIO *b = BIO_new_mem_buf((void *)CFDataGetBytePtr(geraet_pem), (int)CFDataGetLength(geraet_pem));
    RSA *gr = PEM_read_bio_RSAPublicKey(b, NULL, NULL, NULL);
    EVP_PKEY *gk, *wk, *hk;
    X509 *wz, *hz, *gz;
    char id[64];
    BIO_free(b);
    if (!gr) { meld("Schluessel des iPhones unlesbar"); ERR_print_errors_fp(stderr); return -1; }
    gk = EVP_PKEY_new();
    EVP_PKEY_assign_RSA(gk, gr);
    meld("erzeuge Schluessel ...");
    wk = schluessel_neu();
    hk = schluessel_neu();
    wz = zertifikat(wk, NULL, wk);
    hz = zertifikat(hk, wz, wk);
    gz = zertifikat(gk, wz, wk);
    if (Paar) CFRelease(Paar);
    Paar = d_neu();
    paar_obj("RootCertificate", pem_zert(wz));
    paar_obj("RootPrivateKey", pem_schl(wk));
    paar_obj("HostCertificate", pem_zert(hz));
    paar_obj("HostPrivateKey", pem_schl(hk));
    paar_obj("DeviceCertificate", pem_zert(gz));
    uuid_text(id, sizeof id);
    d_str(Paar, "HostID", id);
    uuid_text(id, sizeof id);
    d_str(Paar, "SystemBUID", id);
    X509_free(wz); X509_free(hz); X509_free(gz);
    EVP_PKEY_free(wk); EVP_PKEY_free(hk); EVP_PKEY_free(gk);
    return 0;
}

/* ---------- TLS mit dem Zertifikat des Rechners ---------- */

static SSL_CTX *Tls;

static int tls_an(Kanal *k)
{
    if (!Tls) {
        CFDataRef hz = g_daten(Paar, "HostCertificate"), hs = g_daten(Paar, "HostPrivateKey");
        BIO *b;
        X509 *x;
        RSA *r;
        if (!hz || !hs) return -1;
        SSL_library_init();
        SSL_load_error_strings();
        Tls = SSL_CTX_new(SSLv23_client_method());
        /* AES-128 rechnet auf dem G4 deutlich schneller als AES-256 (Bildstrom im Tunnel) */
        SSL_CTX_set_cipher_list(Tls, "AES128-SHA:AES256-SHA:DES-CBC3-SHA");
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
        SSL_CTX_set_security_level(Tls, 0);
#endif
        b = BIO_new_mem_buf((void *)CFDataGetBytePtr(hz), (int)CFDataGetLength(hz));
        x = PEM_read_bio_X509(b, NULL, NULL, NULL);
        BIO_free(b);
        b = BIO_new_mem_buf((void *)CFDataGetBytePtr(hs), (int)CFDataGetLength(hs));
        r = PEM_read_bio_RSAPrivateKey(b, NULL, NULL, NULL);
        BIO_free(b);
        if (!x || !r || SSL_CTX_use_certificate(Tls, x) != 1 || SSL_CTX_use_RSAPrivateKey(Tls, r) != 1) {
            ERR_print_errors_fp(stderr);
            return -1;
        }
        SSL_CTX_set_verify(Tls, SSL_VERIFY_NONE, NULL);
    }
    k->ssl = SSL_new(Tls);
    SSL_set_fd(k->ssl, k->fd);
    if (SSL_connect(k->ssl) != 1) {
        meld("TLS-Aufbau gescheitert");
        ERR_print_errors_fp(stderr);
        SSL_free(k->ssl);
        k->ssl = NULL;
        return -1;
    }
    if (Laut) meld("TLS: %s %s", SSL_get_version(k->ssl), SSL_get_cipher(k->ssl));
    return 0;
}

/* Lockdown-Verbindung mit Sitzung (verschluesselt) */
static int ld_sitzung(Kanal *k)
{
    CFMutableDictionaryRef d, r;
    char hid[64], buid[64];
    if (!Paar && paar_laden()) { meld("noch nicht gekoppelt: zuerst \"geraet koppeln\""); return -1; }
    if (geraet_kanal(k, LOCKDOWN) || ld_typ(k)) return -1;
    g_str(Paar, "HostID", hid, sizeof hid);
    g_str(Paar, "SystemBUID", buid, sizeof buid);
    d = ld_anfrage("StartSession");
    d_str(d, "HostID", hid);
    d_str(d, "SystemBUID", buid);
    r = ld_frage(k, d, 0);
    if (!r || g(r, "Error")) { if (r) CFRelease(r); k_zu(k); return -1; }
    if (g_bool(r, "EnableSessionSSL") && tls_an(k)) { CFRelease(r); k_zu(k); return -1; }
    CFRelease(r);
    return 0;
}

/* startet einen Dienst ueber Lockdown und verbindet sich damit */
static int dienst(Kanal *ld, const char *name, Kanal *k)
{
    CFMutableDictionaryRef d = ld_anfrage("StartService"), r;
    long long port;
    int ssl;
    d_str(d, "Service", name);
    r = ld_frage(ld, d, 0);
    if (!r) return -1;
    port = g_int(r, "Port", -1);
    ssl = g_bool(r, "EnableServiceSSL");
    CFRelease(r);
    if (port <= 0) return -1;
    if (Laut) meld("%s auf Port %lld%s", name, port, ssl ? " (TLS)" : "");
    if (geraet_kanal(k, (int)port)) return -1;
    if (ssl && tls_an(k)) { k_zu(k); return -1; }
    return 0;
}

/* ---------- Befehle ---------- */

static int befehl_info(void)
{
    Kanal k;
    CFTypeRef v;
    if (!paar_laden()) {
        if (ld_sitzung(&k)) return 1;
    } else {
        if (geraet_kanal(&k, LOCKDOWN) || ld_typ(&k)) return 1;
    }
    v = ld_wert(&k, NULL, NULL);
    k_zu(&k);
    if (!v || CFGetTypeID(v) != CFDictionaryGetTypeID()) return 1;
    zeigen(v);
    CFRelease(v);
    return 0;
}

static int befehl_koppeln(void)
{
    Kanal k;
    CFTypeRef pk, w;
    int versuch, hinweis = 0;
    if (geraet_kanal(&k, LOCKDOWN) || ld_typ(&k)) return 1;
    pk = ld_wert(&k, NULL, "DevicePublicKey");
    if (!pk || CFGetTypeID(pk) != CFDataGetTypeID()) { meld("iPhone liefert keinen Schluessel"); return 1; }
    if (paar_erzeugen(pk)) return 1;
    CFRelease(pk);
    w = ld_wert(&k, NULL, "UniqueDeviceID");
    if (w) { d_obj(Paar, "UDID", w); CFRelease(w); }
    w = ld_wert(&k, NULL, "WiFiAddress");
    if (w) { d_obj(Paar, "WiFiMACAddress", w); CFRelease(w); }
    for (versuch = 0; versuch < 180; versuch++) {
        CFMutableDictionaryRef d = ld_anfrage("Pair"), r, pr = d_neu(), opt = d_neu();
        const char *schl[] = { "DeviceCertificate", "HostCertificate", "RootCertificate", "HostID", "SystemBUID" };
        unsigned i;
        char e[128];
        for (i = 0; i < 5; i++) d_obj(pr, schl[i], g(Paar, schl[i]));
        d_obj(d, "PairRecord", pr);
        CFRelease(pr);
        d_str(d, "ProtocolVersion", "2");
        d_obj(opt, "ExtendedPairingErrors", kCFBooleanTrue);
        d_obj(d, "PairingOptions", opt);
        CFRelease(opt);
        r = ld_frage(&k, d, 1);
        if (!r) { k_zu(&k); return 1; }
        if (g_str(r, "Error", e, sizeof e)) {
            CFDataRef eb = g_daten(r, "EscrowBag");
            if (eb) d_obj(Paar, "EscrowBag", eb);
            CFRelease(r);
            k_zu(&k);
            if (paar_speichern()) { meld("Kopplung nicht speicherbar: %s", Paar_pfad); return 1; }
            printf("gekoppelt\n");
            return 0;
        }
        CFRelease(r);
        if (!strcmp(e, "PairingDialogResponsePending") || !strcmp(e, "PasswordProtected")) {
            int art = strcmp(e, "PasswordProtected") ? 1 : 2;
            if (hinweis != art) {
                hinweis = art;
                printf("%s\n", art == 1 ? "auf dem iPhone \"Vertrauen\" waehlen und Code eingeben" : "iPhone entsperren");
                fflush(stdout);
            }
            sleep(1);
            continue;
        }
        meld("Kopplung abgelehnt: %s", e);
        k_zu(&k);
        return 1;
    }
    meld("keine Bestaetigung am iPhone");
    k_zu(&k);
    return 1;
}

static int befehl_entwickler(void)
{
    Kanal ld, k;
    CFMutableDictionaryRef d, r;
    if (ld_sitzung(&ld)) return 1;
    if (dienst(&ld, "com.apple.amfi.lockdown", &k)) { k_zu(&ld); return 1; }
    k_zu(&ld);
    d = d_neu();
    d_int(d, "action", 0);
    if (ld_senden(&k, d)) { CFRelease(d); k_zu(&k); return 1; }
    CFRelease(d);
    r = ld_empfangen(&k);
    k_zu(&k);
    if (!r) { meld("keine Antwort"); return 1; }
    if (g_bool(r, "success")) { printf("Schalter Entwicklermodus ist jetzt sichtbar\n"); CFRelease(r); return 0; }
    zeigen(r);
    CFRelease(r);
    return 1;
}

static int befehl_wlan(void)
{
    Kanal k;
    CFMutableDictionaryRef d, r;
    CFTypeRef v;
    int ok;
    if (ld_sitzung(&k)) return 1;
    d = ld_anfrage("SetValue");
    d_str(d, "Domain", "com.apple.mobile.wireless_lockdown");
    d_str(d, "Key", "EnableWifiConnections");
    d_obj(d, "Value", kCFBooleanTrue);
    r = ld_frage(&k, d, 0);
    if (!r) { k_zu(&k); return 1; }
    CFRelease(r);
    v = ld_wert(&k, "com.apple.mobile.wireless_lockdown", "EnableWifiConnections");
    ok = v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(v);
    if (v) CFRelease(v);
    k_zu(&k);
    printf("%s\n", ok ? "WLAN-Verbindungen erlaubt" : "WLAN-Verbindungen nicht bestaetigt");
    return ok ? 0 : 1;
}

/* Entwickler-Tunnel (CoreDeviceProxy): Handschlag pruefen */
static int befehl_tunnel(void)
{
    Kanal ld, k;
    const char *frage = "{\"type\":\"clientHandshakeRequest\",\"mtu\":16000}";
    unsigned char h[10];
    size_t n = strlen(frage);
    unsigned m;
    char *antwort;
    if (ld_sitzung(&ld)) return 1;
    if (dienst(&ld, "com.apple.internal.devicecompute.CoreDeviceProxy", &k)) { k_zu(&ld); return 1; }
    memcpy(h, "CDTunnel", 8);
    h[8] = (unsigned char)(n >> 8);
    h[9] = (unsigned char)n;
    if (k_schreiben(&k, h, 10) || k_schreiben(&k, frage, n) || k_lesen(&k, h, 10) || memcmp(h, "CDTunnel", 8)) {
        meld("Tunnel: kein Handschlag");
        k_zu(&k);
        k_zu(&ld);
        return 1;
    }
    m = (unsigned)h[8] << 8 | h[9];
    antwort = calloc(1, m + 1);
    if (k_lesen(&k, antwort, m)) { k_zu(&k); k_zu(&ld); return 1; }
    printf("%s\n", antwort);
    free(antwort);
    k_zu(&k);
    k_zu(&ld);
    return 0;
}

/* ---------- Entwickler-Tunnel und Dienstsuche (RSD) ---------- */

static Kanal Tk;   /* Tunnel-Verbindung */
static Kanal Herz = { -1, NULL };   /* Herzschlag, nur ueber WLAN */
static int Nur_wlan;

static int tw_schreiben(void *c, const void *b, size_t n) { return k_schreiben(c, b, n); }
static int tw_lesen(void *c, void *b, size_t n) { return k_lesen(c, b, n); }
static int tw_bereit(void *c, int ms)
{
    Kanal *k = c;
    fd_set f;
    struct timeval t;
    if (k->ssl && SSL_pending(k->ssl) > 0) return 1;
    FD_ZERO(&f);
    FD_SET(k->fd, &f);
    t.tv_sec = ms / 1000;
    t.tv_usec = (ms % 1000) * 1000;
    return select(k->fd + 1, &f, NULL, NULL, &t);
}
static TunnelWeg Tw = { tw_schreiben, tw_lesen, tw_bereit, &Tk };

static Rx Rsd;
static Xw *Rsd_info;

static void host_uuid(unsigned char u[16])
{
    char hid[64];
    CFStringRef s;
    CFUUIDRef x;
    CFUUIDBytes b;
    g_str(Paar, "HostID", hid, sizeof hid);
    s = CFStringCreateWithCString(NULL, hid, kCFStringEncodingASCII);
    x = CFUUIDCreateFromString(NULL, s);
    b = CFUUIDGetUUIDBytes(x);
    memcpy(u, &b, 16);
    CFRelease(x);
    CFRelease(s);
}

/* Tunnel aufbauen und bei der Dienstsuche anmelden */
static int tunnel_an(void)
{
    Kanal ld;
    int rsd_port;
    Tcp *t;
    Xw *h, *p;
    unsigned char u[16];
    if (ld_sitzung(&ld)) return -1;
    /* ueber WLAN schliesst iOS 27 den Tunnel sofort, wenn kein Herzschlag (Marco/Polo) offen ist */
    if (Wlan && Herz.fd < 0 && dienst(&ld, "com.apple.mobile.heartbeat", &Herz)) { meld("Herzschlag nicht verfuegbar"); k_zu(&ld); return -1; }
    if (dienst(&ld, "com.apple.internal.devicecompute.CoreDeviceProxy", &Tk)) { k_zu(&ld); return -1; }
    k_zu(&ld);
    if (tunnel_start(&Tw, &rsd_port, Laut)) { meld("Tunnel: kein Handschlag"); return -1; }
    if (Laut) meld("Tunnel: ich %s, iPhone %s, Dienstsuche auf Port %d", tunnel_ich(), tunnel_geraet(), rsd_port);
    t = tcp_auf(rsd_port);
    if (!t) { meld("Dienstsuche nicht erreichbar"); return -1; }
    if (rx_auf(&Rsd, t)) { meld("Dienstsuche: kein HTTP/2-Handschlag"); return -1; }
    Rsd.laut = Laut;
    host_uuid(u);
    h = xw_dict();
    xw_setzen(h, "MessageType", xw_str("Handshake"));
    xw_setzen(h, "MessagingProtocolVersion", xw_uint(7));
    xw_setzen(h, "UUID", xw_uuid(u));
    p = xw_dict();
    xw_setzen(p, "RemoteXPCVersionFlags", xw_uint(0x0100000000000006ULL));
    xw_setzen(p, "SensitivePropertiesVisible", xw_bool(1));
    xw_setzen(h, "Properties", p);
    xw_setzen(h, "Services", xw_dict());
    if (rx_senden(&Rsd, h, 0)) { xw_frei(h); return -1; }
    xw_frei(h);
    Rsd_info = rx_empfangen(&Rsd, 10000);
    if (!Rsd_info) { meld("Dienstsuche: keine Antwort"); return -1; }
    return 0;
}

static int dienst_port(const char *name)
{
    Xw *d = xw_holen(xw_holen(Rsd_info, "Services"), name);
    return d ? (int)xw_zahl(d, "Port", -1) : -1;
}

/* RemoteXPC-Dienst im Tunnel oeffnen */
static int xpc_dienst(const char *name, Rx *r)
{
    int port = dienst_port(name);
    Tcp *t;
    if (port <= 0) { meld("Dienst fehlt: %s", name); return -1; }
    t = tcp_auf(port);
    if (!t) return -1;
    if (rx_auf(r, t)) { tcp_zu(t); return -1; }
    r->laut = Laut;
    return 0;
}

static int befehl_dienste(void)
{
    Xw *s, *pr;
    size_t i;
    if (tunnel_an()) return 1;
    pr = xw_holen(Rsd_info, "Properties");
    if (pr) {
        const char *k[] = { "ProductType", "OSVersion", "BuildVersion", "UniqueDeviceID" };
        for (i = 0; i < 4; i++) if (xw_text(pr, k[i])) printf("%-16s %s\n", k[i], xw_text(pr, k[i]));
    }
    s = xw_holen(Rsd_info, "Services");
    if (!s) { xw_zeigen(Rsd_info, 0); return 1; }
    for (i = 0; i < s->n; i++) {
        Xw *e = s->kind[i], *p = xw_holen(e, "Properties");
        printf("%-62s %5lld%s\n", s->schl[i], xw_zahl(e, "Port", -1),
               p && xw_zahl(p, "UsesRemoteXPC", 0) ? "  xpc" : "");
    }
    return 0;
}

/* ---------- Entwickler-Abbild (Cryptex, ab iOS 26.4) ---------- */

#define CRYPTEXD "com.apple.security.cryptexd.remote"
#define DDI_KENNUNG "com.apple.MobileAsset.DDI"

/* eine Routine von cryptexd auf eigener Verbindung; argv geht in den Besitz ueber */
static Xw *cryptexd(const char *routine, Xw *argv)
{
    Rx r;
    Xw *d, *a;
    if (xpc_dienst(CRYPTEXD, &r)) { xw_frei(argv); return NULL; }
    d = xw_dict();
    xw_setzen(d, "routine", xw_str(routine));
    xw_setzen(d, "argv", argv);
    if (rx_senden(&r, d, 1)) { xw_frei(d); rx_zu(&r); return NULL; }
    xw_frei(d);
    a = rx_empfangen(&r, 20000);
    rx_zu(&r);
    if (!a) { meld("cryptexd %s: keine Antwort", routine); return NULL; }
    if (xw_holen(a, "cferr") || xw_zahl(a, "error", 0)) {
        meld("cryptexd %s abgelehnt:", routine);
        xw_zeigen(a, 0);
        xw_frei(a);
        return NULL;
    }
    return a;
}

static int ddi_geladen(void)
{
    Xw *a = cryptexd("copy-installed", xw_dict()), *liste;
    size_t i;
    int da = 0;
    if (!a) return -1;
    liste = xw_holen(xw_holen(a, "argv"), "remote-cryptex-array");
    for (i = 0; liste && i < liste->n; i++) {
        const char *k = xw_text(liste->kind[i], "remote-cryptex-identifier");
        if (Laut) printf("Cryptex: %s %s\n", k ? k : "?", xw_text(liste->kind[i], "remote-cryptex-version") ? xw_text(liste->kind[i], "remote-cryptex-version") : "");
        if (k && !strcmp(k, DDI_KENNUNG)) da = 1;
    }
    xw_frei(a);
    return da;
}

static CFDataRef datei_lesen(const char *pfad)
{
    FILE *f = fopen(pfad, "rb");
    long n;
    UInt8 *b;
    CFDataRef d;
    if (!f) { meld("fehlt: %s", pfad); return NULL; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    d = CFDataCreate(NULL, b, n);
    free(b);
    return d;
}

/* Signatur bei Apple (TSS, einfaches HTTP wie Xcode es auch kann) */
static CFDictionaryRef tss_anfrage(CFDictionaryRef anfrage)
{
    CFDataRef x = CFPropertyListCreateXMLData(NULL, anfrage);
    char kopf[512], *antwort = NULL, *m, *rs;
    size_t n = 0, cap = 0;
    Kanal k = { -1, NULL };
    CFDictionaryRef ergebnis = NULL;
    k.fd = tcp_verbinden("gs.apple.com", 80);
    if (k.fd < 0) { meld("gs.apple.com nicht erreichbar"); CFRelease(x); return NULL; }
    wartezeit(k.fd, 60);
    snprintf(kopf, sizeof kopf,
             "POST /TSS/controller?action=2 HTTP/1.0\r\nHost: gs.apple.com\r\nCache-Control: no-cache\r\n"
             "Content-Type: text/xml; charset=\"utf-8\"\r\nUser-Agent: InetURL/1.0\r\nContent-Length: %ld\r\n\r\n",
             (long)CFDataGetLength(x));
    if (k_schreiben(&k, kopf, strlen(kopf)) || k_schreiben(&k, CFDataGetBytePtr(x), (size_t)CFDataGetLength(x))) {
        CFRelease(x);
        k_zu(&k);
        return NULL;
    }
    CFRelease(x);
    for (;;) {
        ssize_t r;
        if (n + 4096 + 1 > cap) { cap = (n + 4096) * 2; antwort = realloc(antwort, cap); }
        r = read(k.fd, antwort + n, 4096);
        if (r <= 0) break;
        n += (size_t)r;
    }
    k_zu(&k);
    if (!antwort) return NULL;
    antwort[n] = 0;
    m = strstr(antwort, "MESSAGE=");
    if (!m || strncmp(m + 8, "SUCCESS", 7)) {
        meld("Apple lehnt die Signatur ab: %.200s", m ? m : antwort);
        free(antwort);
        return NULL;
    }
    rs = strstr(antwort, "REQUEST_STRING=");
    if (rs) {
        CFPropertyListRef p = plist_aus(rs + 15, strlen(rs + 15));
        if (p && CFGetTypeID(p) == CFDictionaryGetTypeID()) ergebnis = p;
        else if (p) CFRelease(p);
    }
    free(antwort);
    return ergebnis;
}

static int befehl_ddi(void)
{
    char ordner[1024], pfad[1200], buid[64];
    CFDictionaryRef v, vtss, dateien, antwort;
    CFMutableDictionaryRef tss;
    CFDataRef ticket, inhalt[4];
    const char *felder[4] = { "image", "trustcache", "info", "volumehash" };
    Xw *a, *argv, *props, *d;
    unsigned long long chip, ecid;
    int cpro, i, domain;
    unsigned char udid[16];
    Rx r;
    const char *h = getenv("HOME");
    snprintf(ordner, sizeof ordner, "%s/tools/fernbedienung/iphone/ddi", h ? h : "");
    snprintf(pfad, sizeof pfad, "%s/ddi_vorlage.plist", ordner);
    {
        CFDataRef vd = datei_lesen(pfad);
        if (!vd) return 1;
        v = plist_aus(CFDataGetBytePtr(vd), (size_t)CFDataGetLength(vd));
        CFRelease(vd);
    }
    vtss = g(v, "TSS");
    dateien = g(v, "Dateien");
    domain = (int)g_int(v, "NonceDomain", 4);
    if (tunnel_an()) return 1;
    i = ddi_geladen();
    if (i < 0) return 1;
    if (i == 1) { printf("Entwickler-Abbild ist schon geladen\n"); return 0; }

    a = cryptexd("read-personalization-id", xw_dict());
    if (!a) return 1;
    argv = xw_holen(a, "argv");
    chip = (unsigned long long)xw_zahl(argv, "img4_chip_chip", -1);
    ecid = (unsigned long long)xw_zahl(argv, "img4_chip_ecid", -1);
    cpro = (int)xw_zahl(argv, "img4_chip_cpro", 1);
    if (Laut) { printf("Kennung: "); xw_zeigen(argv, 0); }
    xw_frei(a);
    for (i = 0; i < 8; i++) { udid[i] = (unsigned char)(chip >> (56 - 8 * i)); udid[8 + i] = (unsigned char)(ecid >> (56 - 8 * i)); }

    d = xw_dict();
    xw_setzen(d, "nonce-domain-handle", xw_uint((unsigned long long)domain));
    a = cryptexd("get-nonce", d);
    if (!a) return 1;
    {
        Xw *nb = xw_holen(xw_holen(a, "argv"), "nonce");
        uint32_t nl;
        CFDataRef nonce;
        if (!nb || nb->typ != XW_DATA || nb->n < 6) { meld("keine Nonce"); xw_frei(a); return 1; }
        nl = nb->b[nb->n - 4] | nb->b[nb->n - 3] << 8 | nb->b[nb->n - 2] << 16 | (uint32_t)nb->b[nb->n - 1] << 24;
        if (2 + nl > nb->n) { meld("Nonce unlesbar"); xw_frei(a); return 1; }
        nonce = CFDataCreate(NULL, nb->b + 2, nl);
        tss = CFDictionaryCreateMutableCopy(NULL, 0, vtss);
        d_obj(tss, "Cryptex1,Nonce", nonce);
        CFRelease(nonce);
        xw_frei(a);
    }
    d_str(tss, "@HostPlatformInfo", "mac");
    d_str(tss, "@VersionInfo", "libauthinstall-1104.0.9");
    uuid_text(buid, sizeof buid);
    d_str(tss, "@UUID", buid);
    d_obj(tss, "Cryptex1,ProductionMode", cpro ? kCFBooleanTrue : kCFBooleanFalse);
    {
        CFDataRef u = CFDataCreate(NULL, udid, 16);
        d_obj(tss, "Cryptex1,UDID", u);
        CFRelease(u);
    }
    printf("Signatur bei Apple anfragen ...\n");
    fflush(stdout);
    antwort = tss_anfrage(tss);
    CFRelease(tss);
    if (!antwort) return 1;
    ticket = g_daten(antwort, "Cryptex1,Ticket");
    if (!ticket) ticket = g_daten(antwort, "ApImg4Ticket");
    if (!ticket) { meld("Antwort ohne Ticket"); return 1; }
    printf("Ticket erhalten (%ld Bytes)\n", (long)CFDataGetLength(ticket));

    for (i = 0; i < 4; i++) {
        char name[256];
        g_str(dateien, felder[i], name, sizeof name);
        snprintf(pfad, sizeof pfad, "%s/%s", ordner, name);
        inhalt[i] = datei_lesen(pfad);
        if (!inhalt[i]) return 1;
    }

    props = xw_dict();
    xw_setzen(props, "Cryptex1,UseProductClass", xw_bool(g_bool(v, "UseProductClass")));
    xw_setzen(props, "MountedCryptex", xw_bool(0));
    xw_setzen(props, "Cryptex1,SubType", xw_uint((unsigned long long)g_int(v, "SubType", 2)));
    xw_setzen(props, "Cryptex1,NonceDomain", xw_uint((unsigned long long)domain));
    {
        char t[128];
        g_str(v, "Version", t, sizeof t);
        xw_setzen(props, "Cryptex1,Version", xw_str(t));
        g_str(v, "PreauthVersion", t, sizeof t);
        xw_setzen(props, "Cryptex1,PreauthVersion", xw_str(t));
    }
    argv = xw_dict();
    xw_setzen(argv, "auth", xw_uint(0));
    xw_setzen(argv, "client-version", xw_uint(3));
    xw_setzen(argv, "cryptex1-properties", props);
    xw_setzen(argv, "image-type-index", xw_int(10));
    xw_setzen(argv, "nonce-persistence", xw_uint(1));
    xw_setzen(argv, "persistence", xw_uint(2));
    xw_setzen(argv, "image", xw_datei(1, (unsigned long long)CFDataGetLength(inhalt[0])));
    xw_setzen(argv, "trustcache", xw_datei(2, (unsigned long long)CFDataGetLength(inhalt[1])));
    xw_setzen(argv, "im4m", xw_datei(3, (unsigned long long)CFDataGetLength(ticket)));
    xw_setzen(argv, "info", xw_datei(4, (unsigned long long)CFDataGetLength(inhalt[2])));
    xw_setzen(argv, "volumehash", xw_datei(5, (unsigned long long)CFDataGetLength(inhalt[3])));
    d = xw_dict();
    xw_setzen(d, "routine", xw_str("install"));
    xw_setzen(d, "argv", argv);
    if (xpc_dienst(CRYPTEXD, &r)) { xw_frei(d); return 1; }
    printf("Abbild uebertragen ...\n");
    fflush(stdout);
    {
        double t0 = jetzt();
        int e = rx_senden(&r, d, 1)
            || rx_datei(&r, 1, CFDataGetBytePtr(inhalt[0]), (size_t)CFDataGetLength(inhalt[0]))
            || rx_datei(&r, 2, CFDataGetBytePtr(inhalt[1]), (size_t)CFDataGetLength(inhalt[1]))
            || rx_datei(&r, 3, CFDataGetBytePtr(ticket), (size_t)CFDataGetLength(ticket))
            || rx_datei(&r, 4, CFDataGetBytePtr(inhalt[2]), (size_t)CFDataGetLength(inhalt[2]))
            || rx_datei(&r, 5, CFDataGetBytePtr(inhalt[3]), (size_t)CFDataGetLength(inhalt[3]));
        xw_frei(d);
        if (e) { meld("Uebertragung abgebrochen"); rx_zu(&r); return 1; }
        printf("uebertragen in %.1f s, warte auf das iPhone ...\n", jetzt() - t0);
        fflush(stdout);
    }
    a = rx_empfangen(&r, 90000);
    rx_zu(&r);
    if (!a) { meld("install: keine Antwort"); return 1; }
    if (xw_holen(a, "cferr") || xw_zahl(a, "error", 0)) { meld("install abgelehnt:"); xw_zeigen(a, 0); xw_frei(a); return 1; }
    xw_frei(a);
    if (ddi_geladen() == 1) { printf("Entwickler-Abbild geladen\n"); return 0; }
    meld("install gemeldet, Abbild aber nicht gefunden");
    return 1;
}

/* ---------- Bildstrom (Freigabe der Eingabe) und Beruehrungen ---------- */

static void pb_varint(Puffer *p, unsigned long long v)
{
    do {
        unsigned char b = v & 0x7f;
        v >>= 7;
        if (v) b |= 0x80;
        pf_rein(p, &b, 1);
    } while (v);
}
static void pb_int(Puffer *p, int f, unsigned long long v) { pb_varint(p, (unsigned long long)(f << 3)); pb_varint(p, v); }
static void pb_bytes(Puffer *p, int f, const void *b, size_t n) { pb_varint(p, (unsigned long long)(f << 3 | 2)); pb_varint(p, n); pf_rein(p, b, n); }
static void pb_str(Puffer *p, int f, const char *s) { pb_bytes(p, f, s, strlen(s)); }

static void codec_bank(Puffer *p, int pt, const char *merkmale, int f4, int paare)
{
    Puffer b = { 0 };
    int i;
    pb_int(&b, 1, (unsigned)pt);
    for (i = 0; i < paare; i++) {
        Puffer e = { 0 };
        pb_int(&e, 1, 1);
        pb_int(&e, 2, (unsigned)(1 + i % 2));
        pb_int(&e, 3, 50115);
        pb_int(&e, 4, 0);
        pb_bytes(&b, 2, e.p, e.n);
        pf_frei(&e);
    }
    pb_str(&b, 3, merkmale);
    pb_int(&b, 4, (unsigned)f4);
    pb_bytes(p, 3, b.p, b.n);
    pf_frei(&b);
}

static int Angebot_zeigen;
static unsigned long long Strom_bps;   /* 0 = Vorgabe wie Xcode (ueber WLAN WLAN_BPS); 300 kbit/s schaltet die Eingabe ab */
#ifndef WLAN_BPS
#define WLAN_BPS 1000000ULL
#endif

/* Angebot fuer den Bildstrom wie Xcode (media_stream_offer.py), als binaere Plist */
static CFDataRef bildstrom_angebot(uint32_t sitzung, const char *ruf_id)
{
    static const unsigned long long stufen[10][3] = {
        { 4074, 0, 16384 }, { 0, 75000000, 524288 }, { 0, 40000000, 12288 }, { 16, 4100, 0 }, { 0, 20000000, 98304 },
        { 4, 6500, 0 }, { 0, 6000000, 131072 }, { 0, 100000000, 1048576 }, { 0, 60000000, 262144 }, { 1, 299, 0 } };
    unsigned long long stu[10][3];
    Puffer vs = { 0 }, blob = { 0 }, ep = { 0 };
    unsigned char sid[5];
    uLongf zn;
    Bytef *z;
    int i;
    CFMutableDictionaryRef d;
    CFDataRef x;
    CFWriteStreamRef st;
    CFDataRef aus;
    /* VideoSettings */
    pb_varint(&vs, 1 << 3);
    {
        unsigned long long v = sitzung;
        for (i = 0; i < 5; i++) { sid[i] = (unsigned char)(v & 0x7f); v >>= 7; if (i < 4) sid[i] |= 0x80; }
        pf_rein(&vs, sid, 5);
    }
    pb_int(&vs, 2, 0);
    codec_bank(&vs, 123, "FLS;SW:1;", 1, 4);
    codec_bank(&vs, 100, "FLS;SW:1;", 14, 2);
    pb_int(&vs, 7, 0);
    pb_int(&vs, 8, 63);
    pb_int(&vs, 10, 1);
    pb_int(&vs, 12, 1);
    /* MediaBlob */
    pb_int(&blob, 1, 1);
    pb_int(&blob, 2, 1);
    pb_bytes(&blob, 5, vs.p, vs.n);
    pb_str(&blob, 6, "Viceroy 1.7.0");
    pb_int(&blob, 8, 0);
    /* der Bildstrom dient nur als Freigabe der Eingabe. Ueber WLAN verdraengt er sonst die AirPlay-Spiegelung
       (gemessen: Hilfsstrom 3,1 Mbit/s, AirPlay nur noch 250 kbit/s, Bild steht) */
    memcpy(stu, stufen, sizeof stu);
    {
        unsigned long long bps = Strom_bps ? Strom_bps : (Wlan ? WLAN_BPS : 0);
        for (i = 0; i < 10; i++) if (bps && stu[i][0] == 0 && stu[i][1]) stu[i][1] = bps;
    }
    for (i = 0; i < 10; i++) {
        Puffer t = { 0 };
        pb_int(&t, 1, stu[i][0]);
        pb_int(&t, 2, stu[i][1]);
        if (stu[i][2]) pb_int(&t, 3, stu[i][2]);
        pb_bytes(&blob, 9, t.p, t.n);
        pf_frei(&t);
    }
    pb_int(&blob, 13, 17137042128614416384ULL);
    pb_int(&blob, 14, 2);
    pb_int(&blob, 16, 0);
    pb_int(&blob, 18, 1);
    zn = compressBound(blob.n);
    z = malloc(zn);
    compress2(z, &zn, blob.p, blob.n, 9);
    if (Angebot_zeigen) {
        printf("%lu ", (unsigned long)blob.n);
        for (i = 0; i < (int)zn; i++) printf("%02x", z[i]);
        printf("\n");
    }
    /* RemoteEndpointInfo */
    pb_int(&ep, 1, 0);
    pb_int(&ep, 2, 1);
    pb_str(&ep, 3, "Mac15,9");
    pb_str(&ep, 4, "2205.3.1");
    pb_str(&ep, 5, "25F80");
    d = d_neu();
    x = CFDataCreate(NULL, ep.p, (CFIndex)ep.n);
    d_obj(d, "avcMediaStreamOptionRemoteEndpointInfo", x);
    CFRelease(x);
    d_int(d, "avcMediaStreamNegotiatorMode", 5);
    x = CFDataCreate(NULL, z, (CFIndex)zn);
    d_obj(d, "avcMediaStreamNegotiatorMediaBlob", x);
    CFRelease(x);
    d_str(d, "avcMediaStreamOptionCallID", ruf_id);
    free(z);
    pf_frei(&vs);
    pf_frei(&blob);
    pf_frei(&ep);
    st = CFWriteStreamCreateWithAllocatedBuffers(NULL, NULL);
    CFWriteStreamOpen(st);
    CFPropertyListWriteToStream(d, st, kCFPropertyListBinaryFormat_v1_0, NULL);
    aus = CFWriteStreamCopyProperty(st, kCFStreamPropertyDataWritten);
    CFWriteStreamClose(st);
    CFRelease(st);
    CFRelease(d);
    return aus;
}

static void klein(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'F') *s = (char)(*s - 'A' + 'a'); }

/* CoreDevice-Aufruf; liefert die ganze Antwort */
static Xw *coredevice(Rx *r, const char *merkmal, const char *aktion, Xw *eingabe)
{
    Xw *d = xw_dict(), *v = xw_dict(), *k = xw_array(), *a;
    char u1[64], u2[64];
    uuid_text(u1, sizeof u1);
    uuid_text(u2, sizeof u2);
    klein(u1);
    klein(u2);
    xw_anhaengen(k, xw_uint(629));
    xw_anhaengen(k, xw_uint(3));
    xw_setzen(v, "components", k);
    xw_setzen(v, "originalComponentsCount", xw_int(2));
    xw_setzen(v, "stringValue", xw_str("629.3"));
    xw_setzen(d, "CoreDevice.CoreDeviceDDIProtocolVersion", xw_int(2));
    xw_setzen(d, "CoreDevice.coreDeviceVersion", v);
    xw_setzen(d, "CoreDevice.deviceIdentifier", xw_str(u1));
    xw_setzen(d, "CoreDevice.input", eingabe);
    xw_setzen(d, "CoreDevice.invocationIdentifier", xw_str(u2));
    xw_setzen(d, "CoreDevice.featureIdentifier", xw_str(merkmal));
    xw_setzen(d, "CoreDevice.action", xw_dict());
    xw_setzen(d, "CoreDevice.actionIdentifier", xw_str(aktion));
    if (rx_senden(r, d, 1)) { xw_frei(d); return NULL; }
    xw_frei(d);
    a = rx_empfangen(r, 20000);
    if (!a) meld("%s: keine Antwort", merkmal);
    else if (!xw_holen(a, "CoreDevice.output")) { meld("%s abgelehnt:", merkmal); xw_zeigen(a, 0); xw_frei(a); a = NULL; }
    return a;
}

static double uhr_mach(void);
static Rx Anzeige, Hid;
static double Hid_seit;
static int Strom_port, Strom_quelle;
static unsigned long Ssrc_ich, Ssrc_er;
static double Rr_zeit;

/* RTCP-Empfangsbericht mit SDES wie Xcode, damit das iPhone den Bildstrom nicht beendet */
static void empfangsbericht(void)
{
    unsigned char p[40];
    unsigned long s = tunnel_rtp_seq();
    memset(p, 0, sizeof p);
    p[0] = 0x81; p[1] = 0xC9; p[3] = 7;
    p[4] = (unsigned char)(Ssrc_ich >> 24); p[5] = (unsigned char)(Ssrc_ich >> 16); p[6] = (unsigned char)(Ssrc_ich >> 8); p[7] = (unsigned char)Ssrc_ich;
    p[8] = (unsigned char)(Ssrc_er >> 24); p[9] = (unsigned char)(Ssrc_er >> 16); p[10] = (unsigned char)(Ssrc_er >> 8); p[11] = (unsigned char)Ssrc_er;
    p[16] = (unsigned char)(s >> 24); p[17] = (unsigned char)(s >> 16); p[18] = (unsigned char)(s >> 8); p[19] = (unsigned char)s;
    p[32] = 0x81; p[33] = 0xCA; p[35] = 2;
    memcpy(p + 36, p + 4, 4);
    {
        unsigned char q[44];
        memcpy(q, p, 40);
        q[40] = 1; q[41] = 0; q[42] = 0; q[43] = 0;
        tunnel_udp_senden(Strom_port, Strom_quelle, q, sizeof q);
    }
    Rr_zeit = jetzt();
}

static void warten(double s)
{
    double e = jetzt() + s;
    while (jetzt() < e) {
        if (tunnel_pumpen(20) < 0) break;
        if (Strom_quelle && jetzt() - Rr_zeit >= 1.0 && tunnel_rtp_pakete()) empfangsbericht();
    }
}

static int bildstrom_an(void)
{
    char ruf[64], *p;
    unsigned char sid[16];
    CFUUIDRef u;
    CFUUIDBytes ub;
    CFDataRef angebot;
    Xw *e, *o, *w;
    uuid_text(ruf, sizeof ruf);
    u = CFUUIDCreate(NULL);
    ub = CFUUIDGetUUIDBytes(u);
    memcpy(sid, &ub, 16);
    CFRelease(u);
    angebot = bildstrom_angebot((uint32_t)random(), ruf);
    if (xpc_dienst("com.apple.coredevice.displayservice", &Anzeige)) { CFRelease(angebot); return -1; }
    e = xw_dict();
    xw_setzen(e, "clientSupportedFeatures", xw_uint(140));
    xw_setzen(e, "direction", xw_str("output"));
    xw_setzen(e, "negotiatorOffer", xw_daten(CFDataGetBytePtr(angebot), (size_t)CFDataGetLength(angebot)));
    CFRelease(angebot);
    o = xw_dict();
    w = xw_dict(); xw_setzen(w, "int", xw_int(1)); xw_setzen(o, "AVCMediaStreamNegotiatorAccessNetworkType", w);
    w = xw_dict(); xw_setzen(w, "int", xw_int(2)); xw_setzen(o, "AVCMediaStreamNegotiatorTransportProtocolType", w);
    w = xw_dict(); xw_setzen(w, "string", xw_str("DisplayByID")); xw_setzen(o, "CoreDeviceVideoDisplayMode", w);
    w = xw_dict(); xw_setzen(w, "int", xw_int(1)); xw_setzen(o, "VideoStreamForDisplayID", w);
    w = xw_dict(); xw_setzen(w, "uuid", xw_uuid(sid)); xw_setzen(o, "avcMediaStreamOptionClientSessionID", w);
    xw_setzen(e, "options", o);
    Strom_port = 40000 + (int)(random() % 9000);
    tunnel_rtp_port(Strom_port);
    xw_setzen(e, "receiverIP", xw_str(tunnel_ich()));
    xw_setzen(e, "receiverPort", xw_uint((unsigned)Strom_port));
    xw_setzen(e, "senderIP", xw_str(tunnel_geraet()));
    xw_setzen(e, "timeout", xw_uint(20));
    xw_setzen(e, "type", xw_str("video"));
    (void)p;
    w = coredevice(&Anzeige, "com.apple.coredevice.feature.startmediastream", "com.apple.coredevice.action.mediastreamstart", e);
    if (!w) return -1;
    if (Laut > 1) xw_zeigen(w, 0);
    {
        Xw *sc = xw_holen(xw_holen(xw_holen(w, "CoreDevice.output"), "connection"), "streamConfig");
        Strom_quelle = (int)xw_zahl(sc, "SourcePort", 0);
        Ssrc_ich = (unsigned long)xw_zahl(sc, "RemoteSSRC", 0);
        Ssrc_er = (unsigned long)xw_zahl(sc, "LocalSSRC", 0);
        if (Laut) meld("Bildstrom: Quelle Port %d, SSRC ich %lu, iPhone %lu", Strom_quelle, Ssrc_ich, Ssrc_er);
        if (!Strom_quelle || !Ssrc_ich || !Ssrc_er) meld("Bildstrom: ohne Empfangsberichte");
        if (!Ssrc_ich || !Ssrc_er) Strom_quelle = 0;
    }
    xw_frei(w);
    warten(0.6);
    return 0;
}

static void bildstrom_aus(void)
{
    Xw *e = xw_dict(), *w;
    xw_setzen(e, "stopAll", xw_bool(1));
    w = coredevice(&Anzeige, "com.apple.coredevice.feature.stopmediastream", "com.apple.coredevice.action.mediastreamstop", e);
    xw_frei(w);
}

static int hid_bericht(const unsigned char *b, size_t n, unsigned long long dienst)
{
    Xw *d = xw_dict(), *p = xw_dict(), *s = xw_dict();
    int r;
    xw_setzen(s, "_0", xw_daten(b, n));
    xw_setzen(s, "_1", xw_uint(dienst));
    xw_setzen(p, "send", s);
    xw_setzen(d, "featureIdentifier", xw_str("com.apple.coredevice.feature.remote.universalhidservice"));
    xw_setzen(d, "messageType", xw_str("Request"));
    xw_setzen(d, "payload", p);
    r = rx_senden(&Hid, d, 0);
    xw_frei(d);
    return r;
}

/* ein Finger: an = 1 beruehrt, 0 losgelassen; x, y 0..65535; t in ns (0 = jetzt) */
static int finger_zeit(int an, unsigned x, unsigned y, unsigned long long t)
{
    unsigned char r[58];
    int i;
    /* immer dieselbe Uhr (Zeit seit Systemstart): springen die Zeitstempel zurueck, verwirft iOS die Punkte */
    if (!t) t = (unsigned long long)(uhr_mach() * 1e9);
    /* Zeitstempel in Takten der Systemuhr wie bei Xcode (24 MHz), nicht in ns */
    t = t / 125 * 3;
    memset(r, 0, sizeof r);
    r[0] = 0x09;
    r[1] = 1;
    r[2] = 5;
    r[3] = an ? 0xC2 : 0x02;
    r[4] = (unsigned char)x; r[5] = (unsigned char)(x >> 8);
    r[6] = (unsigned char)y; r[7] = (unsigned char)(y >> 8);
    r[40] = 2;
    for (i = 0; i < 6; i++) r[44 + i] = (unsigned char)(t >> (8 * i));
    return hid_bericht(r, sizeof r, 257);
}

static int finger(int an, unsigned x, unsigned y) { return finger_zeit(an, x, y, 0); }

/* zwei Finger (Kennungen 0 und 1) in einem Bericht, z. B. zum Zoomen; Loslassen = beide ohne Beruehrt-Bits */
static int zwei_zeit(int an, unsigned x1, unsigned y1, unsigned x2, unsigned y2, unsigned long long t)
{
    unsigned char r[58];
    int i;
    t = t / 125 * 3;
    memset(r, 0, sizeof r);
    r[0] = 0x09;
    r[1] = 2;
    r[2] = 5;
    r[3] = an ? 0xC0 : 0x00;
    r[4] = (unsigned char)x1; r[5] = (unsigned char)(x1 >> 8);
    r[6] = (unsigned char)y1; r[7] = (unsigned char)(y1 >> 8);
    r[8] = an ? 0xC1 : 0x01;
    r[9] = (unsigned char)x2; r[10] = (unsigned char)(x2 >> 8);
    r[11] = (unsigned char)y2; r[12] = (unsigned char)(y2 >> 8);
    r[40] = 2;
    for (i = 0; i < 6; i++) r[44 + i] = (unsigned char)(t >> (8 * i));
    return hid_bericht(r, sizeof r, 257);
}

/* Hardware-Tasten ueber com.apple.coredevice.hid.indigo (IndigoButtonEvent).
   Home = Consumer-Seite 0x0C, Menu 0x40, wie die klassische Home-Taste. */
static Rx Indigo;
static int Indigo_da;

static int taste_druck(unsigned seite, unsigned code, int zustand)
{
    Xw *d, *p;
    int r;
    if (!Indigo_da) {
        if (xpc_dienst("com.apple.coredevice.hid.indigo", &Indigo)) return -1;
        Indigo_da = 1;
    }
    p = xw_dict();
    xw_setzen(p, "state", xw_uint((unsigned)zustand));
    xw_setzen(p, "usagePage", xw_uint(seite));
    xw_setzen(p, "usageCode", xw_uint(code));
    d = xw_dict();
    xw_setzen(d, "messageType", xw_str("IndigoButtonEvent"));
    xw_setzen(d, "payload", p);
    xw_setzen(d, "featureIdentifier", xw_str("com.apple.coredevice.feature.remote.hid.button"));
    r = rx_senden(&Indigo, d, 0);
    xw_frei(d);
    if (r) { rx_zu(&Indigo); Indigo_da = 0; }
    return r;
}

static void taste_tippen(unsigned seite, unsigned code)
{
    taste_druck(seite, code, 1);
    warten(0.07);
    taste_druck(seite, code, 2);
}

static void home_taste(void) { taste_tippen(0x0C, 0x40); }

static int eingabe_an(void)
{
    if (tunnel_an() || bildstrom_an()) return -1;
    if (xpc_dienst("com.apple.coredevice.hid.universalhidservice", &Hid)) return -1;
    Hid_seit = jetzt();
    return 0;
}

static unsigned anteil(const char *s) { double v = atof(s); if (v < 0) v = 0; if (v > 1) v = 1; return (unsigned)(v * 65535 + 0.5); }

static int befehl_wischen(unsigned x1, unsigned y1, unsigned x2, unsigned y2, double dauer)
{
    int i, schritte = dauer > 0 ? 16 : 0;
    if (eingabe_an()) return 1;
    finger(1, x1, y1);
    warten(dauer > 0 ? 0.02 : 0.06);
    for (i = 1; i <= schritte; i++) {
        finger(1, x1 + (int)((int)x2 - (int)x1) * i / schritte, y1 + (int)((int)y2 - (int)y1) * i / schritte);
        warten(dauer / schritte);
    }
    finger(0, x2, y2);
    warten(0.5);
    printf("Bildstrom: %lu Bytes empfangen und verworfen\n", tunnel_udp_bytes());
    bildstrom_aus();
    return 0;
}

/* ---------- Tastatur (virtuelles HID-Geraet) ---------- */

static unsigned long long Tastatur_id;

static const unsigned char Tastatur_beschreibung[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x95, 0x08, 0x75, 0x01,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x05, 0x07, 0x19, 0x00, 0x29, 0xFF, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x95,
    0x06, 0x75, 0x08, 0x81, 0x00, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x91,
    0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0xC0 };

static Xw *codable(const char *art, Xw *w) { Xw *d = xw_dict(); return xw_setzen(d, art, w); }

static int tastatur_an(void)
{
    Xw *d, *p, *c, *n, *s, *paar, *arr, *a;
    unsigned long long sid = 0x100002001ULL;
    if (Tastatur_id) return 0;
    s = xw_dict();
    xw_setzen(s, "Manufacturer", codable("string", xw_str("iBook")));
    xw_setzen(s, "Product", codable("string", xw_str("iBook Tastatur")));
    xw_setzen(s, "ProductID", codable("int", xw_int(0x0250)));
    xw_setzen(s, "VendorID", codable("int", xw_int(0x05AC)));
    xw_setzen(s, "PrimaryUsage", codable("int", xw_int(6)));
    xw_setzen(s, "PrimaryUsagePage", codable("int", xw_int(1)));
    paar = xw_dict();
    xw_setzen(paar, "DeviceUsage", codable("int", xw_int(6)));
    xw_setzen(paar, "DeviceUsagePage", codable("int", xw_int(1)));
    arr = xw_array();
    xw_anhaengen(arr, codable("dictionary", paar));
    xw_setzen(s, "DeviceUsagePairs", codable("array", arr));
    xw_setzen(s, "Transport", codable("string", xw_str("USB")));
    xw_setzen(s, "ReportDescriptor", codable("data", xw_daten(Tastatur_beschreibung, sizeof Tastatur_beschreibung)));
    xw_setzen(s, "UniversalControlVirtualService", codable("bool", xw_bool(1)));
    xw_setzen(s, "_ServiceID", codable("uint", xw_uint(sid)));
    n = xw_dict();
    paar = xw_dict();
    xw_setzen(paar, "DeviceUsage", xw_int(6));
    xw_setzen(paar, "DeviceUsagePage", xw_int(1));
    arr = xw_array();
    xw_anhaengen(arr, paar);
    xw_setzen(n, "DeviceUsagePairs", arr);
    xw_setzen(n, "PrimaryUsage", xw_uint(6));
    xw_setzen(n, "PrimaryUsagePage", xw_uint(1));
    xw_setzen(n, "Product", xw_str("iBook Tastatur"));
    xw_setzen(n, "ProductID", xw_int(0x0250));
    xw_setzen(n, "VendorID", xw_int(0x05AC));
    xw_setzen(n, "_CoreDevice_codablePropertyStorage", s);
    xw_setzen(n, "_ServiceID", xw_uint(sid));
    c = xw_dict();
    xw_setzen(c, "_0", n);
    p = xw_dict();
    xw_setzen(p, "createService", c);
    d = xw_dict();
    xw_setzen(d, "featureIdentifier", xw_str("com.apple.coredevice.feature.remote.universalhidservice"));
    xw_setzen(d, "messageType", xw_str("Request"));
    xw_setzen(d, "payload", p);
    if (rx_senden(&Hid, d, 1)) { xw_frei(d); return -1; }
    xw_frei(d);
    a = rx_empfangen(&Hid, 5000);
    if (a) {
        long long v = xw_zahl(a, "serviceID", 0);
        if (v) sid = (unsigned long long)v;
        if (Laut) { meld("Tastatur angemeldet:"); xw_zeigen(a, 0); }
        xw_frei(a);
    }
    Tastatur_id = sid;
    warten(0.3);
    return 0;
}

/* gedrueckte Tasten (HID-Codes, Seite 7) als vollstaendige Menge */
static int tasten(const unsigned *codes, int n)
{
    unsigned char r[39];
    unsigned long long t = (unsigned long long)(uhr_mach() * 1e9) / 125 * 3;
    int i;
    if (tastatur_an()) return -1;
    memset(r, 0, sizeof r);
    r[0] = 0x01;
    for (i = 0; i < n; i++) if (codes[i] < 240) r[1 + codes[i] / 8] |= (unsigned char)(1 << (codes[i] % 8));
    for (i = 0; i < 6; i++) r[31 + i] = (unsigned char)(t >> (8 * i));
    return hid_bericht(r, sizeof r, Tastatur_id);
}

/* ---------- Herzschlag (WLAN-Verbindungen bleiben nur damit offen) ---------- */

static void herz_antworten(void)
{
    CFMutableDictionaryRef m = ld_empfangen(&Herz), d;
    if (!m) { k_zu(&Herz); return; }
    CFRelease(m);
    d = d_neu();
    d_str(d, "Command", "Polo");
    ld_senden(&Herz, d);
    CFRelease(d);
}

/* ---------- Dauerbetrieb fuer das Spiegel-Fenster ---------- */

/* Geste im 120-Hz-Takt wie ein echter Finger: weich abbremsen, beim Halten weiter Positionen melden */
static int finger_jetzt(int an, double x, double y);

static void finger_weg(unsigned x, unsigned y, unsigned x2, unsigned y2, double dauer, double halten)
{
    double t0 = uhr_mach(), t;
    int i = 0;
    finger_jetzt(1, x, y);
    for (;;) {
        double a, ziel = t0 + ++i * (1.0 / 120);
        while (uhr_mach() < ziel) tunnel_pumpen(1);
        t = uhr_mach() - t0;
        if (t >= dauer + halten) break;
        /* gleichmaessig: die Geschwindigkeit beim Loslassen entscheidet in iOS ueber Blaettern und Wischgesten */
        a = t >= dauer ? 1 : t / dauer;
        finger_jetzt(1, x + ((double)x2 - x) * a, y + ((double)y2 - y) * a);
    }
    finger_jetzt(0, x2, y2);
    if (Laut) meld("Geste %u,%u -> %u,%u: %d Punkte in %.2f s", x, y, x2, y2, i + 1, uhr_mach() - t0);
}

/* ---------- Fingerglaettung: Mauspunkte (unregelmaessig, ~60 Hz) -> gleichmaessig 120 Hz ----------
   Das iPhone-Display laeuft mit 120 Hz; ein echter Finger liefert mindestens so viele gleichmaessige Punkte.
   Ausgegeben wird die Position zur Zeit "jetzt - VERZUG", linear zwischen den echten Mauspunkten. */

#define TAKT (1.0 / 120)
/* gemessen: Mauspunkte im Mittel alle 11-18 ms, Luecken bis 33 ms; der Puffer muss groesser sein */
#define VERZUG 0.045

typedef struct { double t, x, y; } Punkt;
static Punkt Fp[128];
static int Fn, Finger_unten, Finger_los;
static double Takt_naechster;

/* Tippen ohne Bewegung: das Loslassen geht sofort ans iPhone statt nach dem Glaettungspuffer (VERZUG), denn ein
   Knopf loest erst beim Loslassen aus. Bewegt = Finger seit dem Aufsetzen um mehr als STILL (~2 Fensterpunkte)
   gewandert; dann bleibt alles wie gehabt, damit Wischen und Schieben weich auslaufen. */
#define STILL 450
static double Unten_x, Unten_y;
static int Bewegt;
static void bewegung_pruefen(double x, double y)
{
    if (fabs(x - Unten_x) > STILL || fabs(y - Unten_y) > STILL) Bewegt = 1;
}

static double uhr_mach(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

static unsigned long Berichte, Berichtfehler, Mauspunkte;
static double Maus_erster, Maus_letzter, Maus_luecke;

/* Zoom-Strich: zwei Finger senkrecht um eine feste Mitte, geglaettet wird der halbe Abstand (in x) */
static int Zoom;
static double Zoom_x, Zoom_y, Zoom_klein = 1e9, Zoom_gross;

static int finger_jetzt(int an, double x, double y)
{
    unsigned long long ns = (unsigned long long)(uhr_mach() * 1e9);
    int r;
    if (Zoom) {
        double y1 = Zoom_y - x, y2 = Zoom_y + x;
        if (x < Zoom_klein) Zoom_klein = x;
        if (x > Zoom_gross) Zoom_gross = x;
        if (!an) {
            /* eine Zeile je Zoom, damit sich pruefen laesst, was beim iPhone ankam */
            meld("Steuerung: Zoom bei %.0f/%.0f, Abstand %.0f bis %.0f, %lu Berichte%s", Zoom_x, Zoom_y, Zoom_klein,
                 Zoom_gross, Berichte + 1, Berichtfehler ? ", Sendefehler" : "");
            Zoom_klein = 1e9;
            Zoom_gross = 0;
        }
        if (y1 < 0) y1 = 0;
        if (y2 > 65535) y2 = 65535;
        r = zwei_zeit(an, (unsigned)(Zoom_x + 0.5), (unsigned)(y1 + 0.5), (unsigned)(Zoom_x + 0.5), (unsigned)(y2 + 0.5), ns);
    } else r = finger_zeit(an, (unsigned)(x + 0.5), (unsigned)(y + 0.5), ns);
    Berichte++;
    if (r) Berichtfehler++;
    if (!an) {
        double d = Maus_letzter - Maus_erster;
        if (Laut) meld("Steuerung: Strich %.2f s, %lu Mauspunkte (im Mittel %.1f ms, groesste Luecke %.1f ms), %lu Punkte ans iPhone%s",
             d, Mauspunkte, Mauspunkte > 1 ? d * 1000 / (Mauspunkte - 1) : 0.0, Maus_luecke * 1000, Berichte,
             Berichtfehler ? ", Sendefehler" : "");
        Berichte = Berichtfehler = Mauspunkte = 0;
        Maus_luecke = 0;
    }
    return r;
}

static void mauspunkt(double t)
{
    if (Mauspunkte && t - Maus_letzter > Maus_luecke) Maus_luecke = t - Maus_letzter;
    if (!Mauspunkte) Maus_erster = t;
    Maus_letzter = t;
    Mauspunkte++;
}

/* Testwischen ohne Maus: exakt 120 Hz, gleichmaessig, direkt ans iPhone */
static void testwischen(void)
{
    int i, n = 60;
    double t0 = uhr_mach();
    finger_jetzt(1, 32768, 50000);
    for (i = 1; i <= n; i++) {
        double ziel = t0 + i * TAKT;
        while (uhr_mach() < ziel) tunnel_pumpen(1);
        finger_jetzt(1, 32768, 50000 - 30000.0 * i / n);
    }
    {
        double ziel = uhr_mach() + 0.1;
        while (uhr_mach() < ziel) tunnel_pumpen(1);
    }
    finger_jetzt(0, 32768, 20000);
}

static void punkt_dazu(double t, double x, double y)
{
    if (Fn && t < Fp[Fn - 1].t) t = Fp[Fn - 1].t;
    if (Fn == (int)(sizeof Fp / sizeof Fp[0])) { memmove(Fp, Fp + 1, sizeof Fp - sizeof Fp[0]); Fn--; }
    Fp[Fn].t = t;
    Fp[Fn].x = x;
    Fp[Fn].y = y;
    Fn++;
}

/* Fingerzeilen, die waehrend eines noch ausstehenden Loslassens kommen (neu ansetzen beim Rollen),
   warten, bis das Loslassen im geglaetteten Ablauf erreicht ist */
static char Warte[96][64];
static int Wn;
static void zeile_ausfuehren(char *z);

static void warteschlange_abarbeiten(void)
{
    int i = 0, k;
    while (i < Wn && !Finger_los) {
        char z[64];
        memcpy(z, Warte[i], sizeof z);
        i++;
        zeile_ausfuehren(z);
    }
    for (k = 0; i + k < Wn; k++) memcpy(Warte[k], Warte[i + k], sizeof Warte[0]);
    Wn = k;
}

/* einen Takt ausgeben, wenn faellig */
static void glaetten_takt(void)
{
    double jetzt_t = uhr_mach(), T, x, y;
    int i;
    if (!Finger_unten || jetzt_t < Takt_naechster) return;
    T = jetzt_t - VERZUG;
    if (T <= Fp[0].t) { x = Fp[0].x; y = Fp[0].y; }
    else if (T >= Fp[Fn - 1].t) { x = Fp[Fn - 1].x; y = Fp[Fn - 1].y; }
    else {
        for (i = 1; i < Fn && Fp[i].t < T; i++) ;
        {
            /* kubische Hermite-Kurve zwischen p1 und p2, Tangenten aus den Nachbarn (ungleiche Abstaende) */
            const Punkt *p0 = &Fp[i >= 2 ? i - 2 : i - 1], *p1 = &Fp[i - 1], *p2 = &Fp[i], *p3 = &Fp[i + 1 < Fn ? i + 1 : i];
            double d = p2->t - p1->t, u = d > 0 ? (T - p1->t) / d : 1;
            double u2 = u * u, u3 = u2 * u;
            double h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
            double d0 = p2->t - p0->t, d1 = p3->t - p1->t;
            double mx1 = d0 > 0 ? (p2->x - p0->x) / d0 * d : 0, my1 = d0 > 0 ? (p2->y - p0->y) / d0 * d : 0;
            double mx2 = d1 > 0 ? (p3->x - p1->x) / d1 * d : 0, my2 = d1 > 0 ? (p3->y - p1->y) / d1 * d : 0;
            x = h00 * p1->x + h10 * mx1 + h01 * p2->x + h11 * mx2;
            y = h00 * p1->y + h10 * my1 + h01 * p2->y + h11 * my2;
            if (x < 0) x = 0;
            if (x > 65535) x = 65535;
            if (y < 0) y = 0;
            if (y > 65535) y = 65535;
        }
        /* verbrauchte Punkte entfernen, zwei vor T behalten (fuer die Tangente) */
        if (i > 2) { memmove(Fp, Fp + i - 2, sizeof(Punkt) * (size_t)(Fn - i + 2)); Fn -= i - 2; }
    }
    if (Finger_los && T >= Fp[Fn - 1].t) {
        finger_jetzt(0, Fp[Fn - 1].x, Fp[Fn - 1].y);
        Finger_unten = Finger_los = 0;
        Zoom = 0;
        Fn = 0;
        warteschlange_abarbeiten();
        return;
    }
    finger_jetzt(1, x, y);
    Takt_naechster += TAKT;
    if (Takt_naechster < jetzt_t) Takt_naechster = jetzt_t + TAKT;
}

static double bis_takt(void)
{
    double d;
    if (!Finger_unten) return 0.1;
    d = Takt_naechster - uhr_mach();
    return d < 0 ? 0 : d;
}

static void ablage_setzen(const char *uti, const char *pfad);
static int Einsetzen_x, Einsetzen_y, Ablage_wartet;

/* XPC-Wert vollstaendig in eine Datei (Daten ungekuerzt) */
static void xw_voll(FILE *f, const Xw *w, int tiefe)
{
    size_t i;
    switch (w->typ) {
    case XW_DICT:
        fprintf(f, "{\n");
        for (i = 0; i < w->n; i++) { fprintf(f, "%*s%s: ", (tiefe + 1) * 2, "", w->schl[i]); xw_voll(f, w->kind[i], tiefe + 1); }
        fprintf(f, "%*s}\n", tiefe * 2, "");
        break;
    case XW_ARRAY:
        fprintf(f, "[\n");
        for (i = 0; i < w->n; i++) { fprintf(f, "%*s", (tiefe + 1) * 2, ""); xw_voll(f, w->kind[i], tiefe + 1); }
        fprintf(f, "%*s]\n", tiefe * 2, "");
        break;
    case XW_STRING: fprintf(f, "\"%s\"\n", w->s); break;
    case XW_INT: fprintf(f, "%lld\n", w->i); break;
    case XW_UINT: fprintf(f, "%llu\n", (unsigned long long)w->i); break;
    case XW_BOOL: fprintf(f, "%s\n", w->i ? "ja" : "nein"); break;
    case XW_DOUBLE: fprintf(f, "%g\n", w->d); break;
    case XW_DATA:
        fprintf(f, "<%lu:", (unsigned long)w->n);
        for (i = 0; i < w->n; i++) fprintf(f, " %02x", w->b[i]);
        fprintf(f, ">\n");
        break;
    default: fprintf(f, "<Typ %x>\n", w->typ);
    }
}

/* Liste der HID-Geraete des iPhones nach /tmp/hid_dienste.txt */
static void hid_liste(void)
{
    Xw *d = xw_dict(), *p = xw_dict(), *a = NULL;
    FILE *f = fopen("/tmp/hid_dienste.txt", "w");
    xw_setzen(p, "connectedServices", xw_dict());
    xw_setzen(d, "featureIdentifier", xw_str("com.apple.coredevice.feature.remote.universalhidservice"));
    xw_setzen(d, "messageType", xw_str("Request"));
    xw_setzen(d, "payload", p);
    if (!rx_senden(&Hid, d, 1)) a = rx_empfangen(&Hid, 5000);
    xw_frei(d);
    if (f) {
        if (a) xw_voll(f, a, 0);
        else fprintf(f, "keine Antwort\n");
        fclose(f);
    }
    if (a) xw_frei(a);
}

static void zeile_ausfuehren(char *z)
{
    unsigned x = 0, y = 0;
    unsigned long long t = 0;
    double ts;
    if (Finger_los && (z[0] == 'd' || z[0] == 'm' || z[0] == 'u' || z[0] == 'z')) {
        if (Wn < 96) { strncpy(Warte[Wn], z, sizeof Warte[0] - 1); Warte[Wn][sizeof Warte[0] - 1] = 0; Wn++; }
        return;
    }
    switch (z[0]) {
    /* Zeit der Mausbewegung (ns seit Systemstart) vom Fenster */
    case 'd':
        if (sscanf(z + 1, "%u %u %llu", &x, &y, &t) < 2) break;
        ts = t ? t / 1e9 : uhr_mach();
        if (Finger_unten) finger_jetzt(0, Fp[Fn - 1].x, Fp[Fn - 1].y);
        Zoom = 0;
        Fn = 0;
        Mauspunkte = 0;
        Maus_luecke = 0;
        mauspunkt(ts);
        punkt_dazu(ts, x, y);
        Finger_unten = 1;
        Finger_los = 0;
        Unten_x = x;
        Unten_y = y;
        Bewegt = 0;
        finger_jetzt(1, x, y);
        Takt_naechster = uhr_mach() + TAKT;
        break;
    case 'z': {
        /* Zoomen mit zwei Fingern: "zd mitte_x mitte_y abstand t", "zm abstand t", "zu abstand t"
           (Abstand = halber Fingerabstand in iPhone-Einheiten 0..65535, senkrecht) */
        unsigned zx, zy, s;
        if (z[1] == 'd') {
            if (sscanf(z + 2, "%u %u %u %llu", &zx, &zy, &s, &t) < 3) break;
            ts = t ? t / 1e9 : uhr_mach();
            if (Finger_unten) finger_jetzt(0, Fp[Fn - 1].x, Fp[Fn - 1].y);
            Zoom = 1;
            Zoom_x = zx;
            Zoom_y = zy;
            Fn = 0;
            Mauspunkte = 0;
            Maus_luecke = 0;
            mauspunkt(ts);
            punkt_dazu(ts, s, 0);
            Finger_unten = 1;
            Finger_los = 0;
            finger_jetzt(1, s, 0);
            Takt_naechster = uhr_mach() + TAKT;
        } else if (z[1] == 'm' || z[1] == 'u') {
            if (sscanf(z + 2, "%u %llu", &s, &t) < 1 || !Finger_unten || !Zoom) break;
            ts = t ? t / 1e9 : uhr_mach();
            if (z[1] == 'm') mauspunkt(ts);
            punkt_dazu(ts, s, 0);
            if (z[1] == 'u') Finger_los = 1;
        }
        break;
    }
    case 'm':
        if (sscanf(z + 1, "%u %u %llu", &x, &y, &t) < 2 || !Finger_unten || Zoom) break;
        mauspunkt(t ? t / 1e9 : uhr_mach());
        punkt_dazu(t ? t / 1e9 : uhr_mach(), x, y);
        bewegung_pruefen(x, y);
        break;
    case 'u':
        if (sscanf(z + 1, "%u %u %llu", &x, &y, &t) < 2 || !Finger_unten || Zoom) break;
        bewegung_pruefen(x, y);
        if (!Bewegt) {
            finger_jetzt(0, Unten_x, Unten_y);
            Finger_unten = Finger_los = 0;
            Fn = 0;
            break;
        }
        punkt_dazu(t ? t / 1e9 : uhr_mach(), x, y);
        Finger_los = 1;
        break;
    case 'g': testwischen(); break;
    case 's': {
        /* setze <uti> <pfad> */
        char uti[128];
        int off = 0;
        if (!strncmp(z, "setze ", 6) && sscanf(z + 6, "%127s %n", uti, &off) >= 1 && off) ablage_setzen(uti, z + 6 + off);
        break;
    }
    case 'e': {
        /* einsetzen <x> <y> <uti> <pfad>: setzen, dort tippen, Befehl-V */
        char uti[128];
        int off = 0;
        unsigned ex, ey;
        if (!strncmp(z, "einsetzen ", 10) && sscanf(z + 10, "%u %u %127s %n", &ex, &ey, uti, &off) >= 3 && off) {
            Einsetzen_x = (int)ex;
            Einsetzen_y = (int)ey;
            ablage_setzen(uti, z + 10 + off);
            if (Ablage_wartet != 4) Einsetzen_x = -1;
        }
        break;
    }
    case 'H': home_taste(); break;
    case 'G': {
        /* frei einstellbare Geste zum Ausprobieren: G x y x2 y2 dauer halten */
        unsigned gx, gy, gx2, gy2;
        double gd, gh;
        if (sscanf(z + 1, "%u %u %u %u %lf %lf", &gx, &gy, &gx2, &gy2, &gd, &gh) == 6) finger_weg(gx, gy, gx2, gy2, gd, gh);
        break;
    }
    case 'w': finger_weg(52000, 32768, 13000, 32768, 0.25, 0); break;   /* Test: Seite weiterwischen */
    case 'L': hid_liste(); break;
    case 'B': {
        /* beliebige Taste zum Ausprobieren: B seite code (hex) */
        unsigned bs, bc;
        if (sscanf(z + 1, "%x %x", &bs, &bc) == 2) taste_tippen(bs, bc);
        break;
    }
    /* Wischen vom unteren Rand loest ueber den Touchscreen-Kanal keine Systemgeste aus (iOS gibt es an die App),
       deshalb die Taste der Apple-Tastatur: Seite 0xFF01, Exposé 0x10 */
    case 'a': taste_tippen(0xFF01, 0x10); break;
    case 'f': {
        /* Suchen: Befehl-Leertaste */
        static const unsigned c[2] = { 0xE3, 0x2C };
        tasten(c, 1); warten(0.05);
        tasten(c, 2); warten(0.07);
        tasten(c, 1); warten(0.05);
        tasten(c, 0);
        break;
    }
    case 'k': {
        unsigned codes[16];
        int n = 0;
        char *p = z + 1, *e;
        while (n < 16) {
            unsigned long v = strtoul(p, &e, 16);
            if (e == p) break;
            codes[n++] = (unsigned)v;
            p = e;
        }
        tasten(codes, n);
        break;
    }
    }
}

/* ---------- Gemeinsame Zwischenablage (com.apple.coredevice.pasteboardservice) ----------
   iPhone -> iBook: jede Sekunde nur die Kennung des Inhalts abfragen (allPromised, klein); hat sie sich
   geaendert und stammt nicht von uns, den Inhalt holen, in eine Datei schreiben, dem Fenster melden:
   "ablage <uti> <pfad>". iBook -> iPhone: Fenster schickt "setze <uti> <pfad>" bzw.
   "einsetzen <x> <y> <uti> <pfad>" (setzen, dort tippen, Befehl-V). */

static Rx Ablage;
static int Ablage_da, Ablage_wartet;          /* 1 Kennung, 2 Inhalt (erste Art), 3 Inhalt (alle Arten), 4 Setzen */
static double Ablage_zeit;
static long long Ablage_stand = -1, Ablage_eigen[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
static int Ablage_ring, Ablage_nr;
static int Einsetzen_x = -1, Einsetzen_y;

static int ablage_an(void)
{
    if (Ablage_da) return 0;
    if (xpc_dienst("com.apple.coredevice.pasteboardservice", &Ablage)) return -1;
    Ablage.behalten = 1;
    Ablage_da = 1;
    Ablage_wartet = 0;
    return 0;
}

static void ablage_holen(const char *art, int stufe)
{
    Xw *d = xw_dict(), *p = xw_dict();
    xw_setzen(p, art, xw_dict());
    xw_setzen(d, "command", xw_str("PULL"));
    xw_setzen(d, "pasteboardName", xw_str("general"));
    xw_setzen(d, "dataPolicy", p);
    if (!rx_senden(&Ablage, d, 1)) { Ablage_wartet = stufe; Ablage_zeit = jetzt(); }
    xw_frei(d);
}

static long long ablage_kennung(Xw *antwort)
{
    Xw *m = xw_holen(xw_holen(antwort, "pasteboard"), "metadata");
    return m ? xw_zahl(m, "changeCount", -1) : -1;
}

static int ablage_eigen(long long k)
{
    int i;
    for (i = 0; i < 8; i++) if (Ablage_eigen[i] == k) return 1;
    return 0;
}

static const char *endung(const char *uti)
{
    static const char *t[][2] = {
        { "public.png", "png" }, { "public.jpeg", "jpg" }, { "public.heic", "heic" }, { "public.tiff", "tiff" },
        { "com.compuserve.gif", "gif" }, { "com.adobe.pdf", "pdf" }, { "public.utf8-plain-text", "txt" },
        { "public.plain-text", "txt" }, { "public.text", "txt" }, { "public.url", "url" }, { "public.mpeg-4", "mp4" },
        { "com.apple.quicktime-movie", "mov" }, { "org.openxmlformats.wordprocessingml.document", "docx" },
        { "org.openxmlformats.spreadsheetml.sheet", "xlsx" }, { "org.openxmlformats.presentationml.presentation", "pptx" },
        { "com.microsoft.word.doc", "doc" }, { "com.microsoft.excel.xls", "xls" }, { "public.zip-archive", "zip" },
        { "com.pkware.zip-archive", "zip" }, { "public.rtf", "rtf" }, { "public.html", "html" }, { "public.mp3", "mp3" },
        { "com.apple.m4a-audio", "m4a" }, { "public.comma-separated-values-text", "csv" } };
    unsigned i;
    for (i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(uti, t[i][0])) return t[i][1];
    return "dat";
}

/* Leopard kann HEIC und WebP nicht lesen */
static int ablage_lesbar(const char *uti)
{
    return strcmp(uti, "public.heic") && strcmp(uti, "org.webmproject.webp") && strcmp(uti, "public.heif");
}

/* Dateiname aus dem Verweis der Dateien-App (Schluesselarchiv als binaere Plist): laengster Text mit passender Endung */
static void name_suchen(CFTypeRef v, const char *endung, char *best, size_t n)
{
    CFTypeID t = CFGetTypeID(v);
    if (t == CFStringGetTypeID()) {
        char s[512], *slash, *punkt;
        if (!CFStringGetCString(v, s, sizeof s, kCFStringEncodingUTF8)) return;
        slash = strrchr(s, '/');
        if (slash) memmove(s, slash + 1, strlen(slash + 1) + 1);
        /* Typkennungen (com.adobe.pdf, public.data ...) sind keine Dateinamen */
        if (!strncmp(s, "com.", 4) || !strncmp(s, "public.", 7) || !strncmp(s, "org.", 4) || !strncmp(s, "dyn.", 4)
            || !strncmp(s, "net.", 4)) return;
        punkt = strrchr(s, '.');
        if (!punkt || punkt == s) return;
        if (endung ? strcasecmp(punkt + 1, endung) : (strlen(punkt + 1) < 1 || strlen(punkt + 1) > 6)) return;
        if (strlen(s) > strlen(best) && strlen(s) < n) strcpy(best, s);
    } else if (t == CFArrayGetTypeID()) {
        CFIndex i, z = CFArrayGetCount(v);
        for (i = 0; i < z; i++) name_suchen(CFArrayGetValueAtIndex(v, i), endung, best, n);
    } else if (t == CFDictionaryGetTypeID()) {
        CFIndex i, z = CFDictionaryGetCount(v);
        const void **w = malloc(sizeof(void *) * (size_t)(z + 1));
        CFDictionaryGetKeysAndValues(v, NULL, w);
        for (i = 0; i < z; i++) name_suchen(w[i], endung, best, n);
        free(w);
    }
}

/* Inhalt aus PULL_REPLY: bevorzugt lesbare Bilder, dann Text, dann das Erste */
static void ablage_ausgeben(Xw *antwort, int alle)
{
    Xw *items = xw_holen(xw_holen(antwort, "pasteboard"), "items"), *item, *typen, *daten;
    static const char *vorzug[] = { "public.png", "public.jpeg", "public.tiff", "com.compuserve.gif", "com.adobe.pdf",
                                    "public.utf8-plain-text", "public.plain-text", "public.text", "public.url", NULL };
    const char *uti = NULL;
    Xw *wert = NULL;
    size_t i, k;
    char pfad[256];
    FILE *f;
    if (!items || items->typ != XW_ARRAY || !items->n) return;
    item = items->kind[0];
    typen = xw_holen(item, "types");
    daten = xw_holen(item, "data");
    if (!typen || !daten) return;
    for (k = 0; vorzug[k] && !uti; k++) {
        Xw *e = xw_holen(xw_holen(daten, vorzug[k]), "data");
        if (e && e->typ == XW_DATA) { uti = vorzug[k]; wert = e; }
    }
    if (!uti) {
        for (i = 0; i < typen->n && !uti; i++) {
            Xw *e = typen->kind[i]->typ == XW_STRING ? xw_holen(xw_holen(daten, typen->kind[i]->s), "data") : NULL;
            /* Verweise der Dateien-App sind nicht das Dokument */
            if (e && e->typ == XW_DATA && strncmp(typen->kind[i]->s, "com.apple.DocumentManager.", 26)) {
                uti = typen->kind[i]->s;
                wert = e;
            }
        }
    }
    if (!alle && (!uti || !ablage_lesbar(uti))) {
        /* nur ein Verweis (Dateien-App) oder nur HEIC: alle Darstellungen holen, darin steckt das Dokument bzw. JPEG/PNG */
        if (typen->n > 1 || !uti) ablage_holen("allResolved", 3);
        else if (uti) goto schreiben;
        return;
    }
    if (!uti) return;
schreiben:
    mkdir("/tmp/iphone_ablage", 0700);
    {
        /* urspruenglicher Dateiname aus dem Verweis der Dateien-App */
        char name[256] = "";
        Xw *verweis = xw_holen(xw_holen(daten, "com.apple.DocumentManager.FINode.File"), "data");
        if (verweis && verweis->typ == XW_DATA) {
            CFPropertyListRef pl = plist_aus(verweis->b, verweis->n);
            if (pl) {
                const char *e = endung(uti);
                name_suchen(pl, strcmp(e, "dat") ? e : NULL, name, sizeof name);
                CFRelease(pl);
            }
        }
        if (name[0]) snprintf(pfad, sizeof pfad, "/tmp/iphone_ablage/%s", name);
        else snprintf(pfad, sizeof pfad, "/tmp/iphone_ablage/%d.%s", ++Ablage_nr, endung(uti));
    }
    f = fopen(pfad, "wb");
    if (!f) return;
    fwrite(wert->b, 1, wert->n, f);
    fclose(f);
    printf("ablage %s %s\n", uti, pfad);
    if (Laut) meld("Zwischenablage vom iPhone: %s, %lu Bytes", uti, (unsigned long)wert->n);
}

static void einsetzen_ausfuehren(void)
{
    unsigned befehl_v[2] = { 0xE3, 0x19 }, befehl[1] = { 0xE3 };
    finger(1, (unsigned)Einsetzen_x, (unsigned)Einsetzen_y);
    warten(0.06);
    finger(0, (unsigned)Einsetzen_x, (unsigned)Einsetzen_y);
    warten(0.45);
    tasten(befehl, 1);
    warten(0.03);
    tasten(befehl_v, 2);
    warten(0.05);
    tasten(befehl, 1);
    tasten(NULL, 0);
    Einsetzen_x = -1;
}

/* regelmaessig aus der Hauptschleife */
static void ablage_takt(void)
{
    Xw *a;
    if (!Ablage_da) return;
    if (rx_pflegen(&Ablage)) { rx_zu(&Ablage); Ablage_da = 0; return; }
    while ((a = rx_abholen(&Ablage))) {
        const char *c = xw_text(a, "command");
        long long k = ablage_kennung(a);
        if (c && !strcmp(c, "SET_REPLY")) {
            if (k >= 0) { Ablage_eigen[Ablage_ring++ & 7] = k; Ablage_stand = k; }
            Ablage_wartet = 0;
            if (Einsetzen_x >= 0) einsetzen_ausfuehren();
        } else if (c && !strcmp(c, "PULL_REPLY")) {
            int stufe = Ablage_wartet;
            Ablage_wartet = 0;
            if (stufe == 1) {
                if (k >= 0 && k != Ablage_stand) {
                    int erstes = Ablage_stand < 0;
                    Ablage_stand = k;
                    if (!erstes && !ablage_eigen(k)) ablage_holen("promiseSecondary", 2);
                }
            } else if (stufe == 2 || stufe == 3) {
                ablage_ausgeben(a, stufe == 3);
            }
        }
        xw_frei(a);
    }
    /* haengende Anfrage nach 90 s aufgeben (der Dienst kann bei Notizen eine Minute brauchen) */
    if (Ablage_wartet == 4 && jetzt() - Ablage_zeit > 5) { Ablage_wartet = 0; if (Einsetzen_x >= 0) einsetzen_ausfuehren(); }
    if (Ablage_wartet && jetzt() - Ablage_zeit > 90) Ablage_wartet = 0;
    if (!Ablage_wartet && jetzt() - Ablage_zeit >= 1.0) ablage_holen("allPromised", 1);
}

static unsigned char *datei_laden(const char *pfad, size_t *n)
{
    FILE *f = fopen(pfad, "rb");
    long l;
    unsigned char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    l = ftell(f);
    rewind(f);
    if (l < 0 || l > 64L * 1024 * 1024) { fclose(f); return NULL; }
    b = malloc((size_t)l + 1);
    if (fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f);
    *n = (size_t)l;
    return b;
}

/* iBook -> iPhone: Inhalt einer Datei in die Zwischenablage des iPhones */
static void ablage_setzen(const char *uti, const char *pfad)
{
    size_t n = 0, i;
    unsigned char *b = datei_laden(pfad, &n);
    Xw *d, *items, *item, *typen, *daten;
    static const char *text[] = { "public.utf8-plain-text", "public.plain-text", "public.text" };
    int ist_text = !strcmp(uti, "public.utf8-plain-text");
    if (!b || ablage_an()) { free(b); return; }
    item = xw_dict();
    typen = xw_array();
    daten = xw_dict();
    for (i = 0; i < (ist_text ? 3u : 1u); i++) {
        const char *u = ist_text ? text[i] : uti;
        Xw *e = xw_dict();
        xw_anhaengen(typen, xw_str(u));
        xw_setzen(e, "data", xw_daten(b, n));
        xw_setzen(daten, u, e);
    }
    xw_setzen(item, "types", typen);
    xw_setzen(item, "data", daten);
    items = xw_array();
    xw_anhaengen(items, item);
    d = xw_dict();
    xw_setzen(d, "command", xw_str("SET"));
    xw_setzen(d, "pasteboardName", xw_str("general"));
    xw_setzen(d, "items", items);
    xw_setzen(d, "sourceMetadata", xw_null());
    if (Laut) meld("Zwischenablage ans iPhone: %s, %lu Bytes", uti, (unsigned long)n);
    if (!rx_senden(&Ablage, d, 1)) { Ablage_wartet = 4; Ablage_zeit = jetzt(); }
    xw_frei(d);
    free(b);
}

/* Test: Zwischenablage setzen (optional) und lesen */
static int befehl_ablage(const char *text)
{
    Xw *a;
    if (tunnel_an() || ablage_an()) return 1;
    if (text) {
        FILE *f = fopen("/tmp/ablage_test.txt", "wb");
        fputs(text, f);
        fclose(f);
        ablage_setzen("public.utf8-plain-text", "/tmp/ablage_test.txt");
        a = rx_empfangen(&Ablage, 8000);
        printf("== Antwort auf SET\n");
        if (a) { xw_zeigen(a, 0); xw_frei(a); } else printf("keine\n");
    }
    ablage_holen("promiseSecondary", 2);
    a = rx_empfangen(&Ablage, 8000);
    printf("== Inhalt\n");
    if (a) { xw_zeigen(a, 0); xw_frei(a); } else printf("keine\n");
    return 0;
}

static int Fifo = -1;

static int eingabe_wartet(void)
{
    fd_set f;
    struct timeval t = { 0, 0 };
    FD_ZERO(&f);
    FD_SET(0, &f);
    return select(1, &f, NULL, NULL, &t) > 0;
}

static int befehl_steuern(void)
{
    char puf[4096];
    size_t pn = 0;
    int ende = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* Kabel bevorzugen, sonst WLAN (-n: nur WLAN) */
    if (Wlan && !Nur_wlan) {
        const char *w = Wlan;
        char u[64];
        if (access("/var/run/usbmuxd", 0) == 0) {
            Wlan = NULL;
            if (mux_geraet(u, sizeof u) >= 0) Geraet_id = -2;
            else Wlan = w;
        }
    }
    if (eingabe_an()) { printf("fehler\n"); return 1; }
    if (ablage_an()) meld("Zwischenablage nicht verfuegbar");
    /* zweiter Befehlseingang zum Testen von aussen: echo g > /tmp/geraet.befehle */
    unlink("/tmp/geraet.befehle");
    if (!mkfifo("/tmp/geraet.befehle", 0600)) Fifo = open("/tmp/geraet.befehle", O_RDWR | O_NONBLOCK);
    printf("bereit %s\n", Wlan ? "wlan" : "usb");
    while (!ende) {
        fd_set f;
        struct timeval t = { 0, 100000 };
        int m = Tk.fd;
        FD_ZERO(&f);
        FD_SET(0, &f);
        FD_SET(Tk.fd, &f);
        if (Herz.fd >= 0) { FD_SET(Herz.fd, &f); if (Herz.fd > m) m = Herz.fd; }
        if (Fifo >= 0) { FD_SET(Fifo, &f); if (Fifo > m) m = Fifo; }
        {
            double w = bis_takt();
            t.tv_sec = 0;
            t.tv_usec = (int)(w * 1e6);
        }
        if (Tk.ssl && SSL_pending(Tk.ssl) > 0) t.tv_usec = 0;
        if (select(m + 1, &f, NULL, NULL, &t) < 0 && errno != EINTR) break;
        glaetten_takt();
        /* Fingerpunkte zuerst: sie sollen gleichmaessig beim iPhone ankommen, nicht gebuendelt
           hinter dem Freigabe-Bildstrom */
        if (FD_ISSET(0, &f)) {
            ssize_t r = read(0, puf + pn, sizeof puf - 1 - pn);
            char *a, *nl;
            if (r <= 0) break;
            pn += (size_t)r;
            puf[pn] = 0;
            a = puf;
            while ((nl = strchr(a, '\n'))) {
                *nl = 0;
                if (!strcmp(a, "ende")) ende = 1;
                else zeile_ausfuehren(a);
                a = nl + 1;
            }
            pn = strlen(a);
            memmove(puf, a, pn);
            if (pn >= sizeof puf - 1) pn = 0;
        }
        if (Fifo >= 0 && FD_ISSET(Fifo, &f)) {
            char b[256], *a, *nl;
            ssize_t r = read(Fifo, b, sizeof b - 1);
            if (r > 0) {
                b[r] = 0;
                for (a = b; (nl = strchr(a, '\n')); a = nl + 1) { *nl = 0; zeile_ausfuehren(a); }
            }
        }
        /* Tunnel in kleinen Portionen leeren und dazwischen nach neuen Fingerpunkten sehen */
        if (FD_ISSET(Tk.fd, &f) || (Tk.ssl && SSL_pending(Tk.ssl) > 0)) {
            int e = 0, n = 0;
            while (n++ < 24 && (e = tunnel_pumpen(0)) > 0) {
                if (eingabe_wartet()) break;
                glaetten_takt();
            }
            if (e < 0) { printf("fehler tunnel\n"); break; }
        }
        glaetten_takt();
        ablage_takt();
        if (Herz.fd >= 0 && (FD_ISSET(Herz.fd, &f) || (Herz.ssl && SSL_pending(Herz.ssl) > 0))) herz_antworten();
        if (Strom_quelle && jetzt() - Rr_zeit >= 1.0 && tunnel_rtp_pakete()) empfangsbericht();
        /* alle Dienstverbindungen bedienen (PING, Fenster), sonst schliesst das iPhone sie */
        if (rx_pflegen(&Rsd)) { meld("Steuerung: Dienstsuche getrennt"); printf("fehler dienstsuche\n"); break; }
        if (rx_pflegen(&Anzeige)) {
            meld("Steuerung: Bildstrom-Dienst getrennt (Zustand %d), neu", tcp_zustand(Anzeige.t));
            rx_zu(&Anzeige);
            if (bildstrom_an()) { printf("fehler bildstrom\n"); break; }
        }
        if (Indigo_da && rx_pflegen(&Indigo)) { rx_zu(&Indigo); Indigo_da = 0; }
        if (rx_pflegen(&Hid)) {
            meld("Steuerung: Eingabedienst getrennt (Zustand %d) nach %.1f s, neu", tcp_zustand(Hid.t), jetzt() - Hid_seit);
            rx_zu(&Hid);
            Tastatur_id = 0;
            if (xpc_dienst("com.apple.coredevice.hid.universalhidservice", &Hid)) { printf("fehler eingabe\n"); break; }
            Hid_seit = jetzt();
        }
    }
    if (Tastatur_id) { unsigned keine = 0; (void)keine; tasten(NULL, 0); }
    bildstrom_aus();
    return 0;
}

/* ---------- Dateidienst (CoreDevice fileservice) ---------- */

static Xw *datei_frage(Rx *r, Xw *d)
{
    Xw *a;
    if (rx_senden(r, d, 1)) { xw_frei(d); return NULL; }
    xw_frei(d);
    a = rx_empfangen(r, 15000);
    if (!a) meld("Dateidienst: keine Antwort");
    return a;
}

static char Datei_sitzung[128];

static int datei_sitzung(Rx *r, int domain, const char *kennung)
{
    Xw *d = xw_dict(), *a;
    const char *s;
    if (xpc_dienst("com.apple.coredevice.fileservice.control", r)) return -1;
    xw_setzen(d, "Cmd", xw_str("CreateSession"));
    xw_setzen(d, "Domain", xw_uint((unsigned long long)domain));
    xw_setzen(d, "Identifier", xw_str(kennung));
    xw_setzen(d, "Session", xw_str(""));
    xw_setzen(d, "User", xw_str("mobile"));
    a = datei_frage(r, d);
    if (!a) return -1;
    s = xw_text(a, "NewSessionID");
    if (!s) { meld("Dateidienst: keine Sitzung"); xw_zeigen(a, 0); xw_frei(a); return -1; }
    snprintf(Datei_sitzung, sizeof Datei_sitzung, "%s", s);
    xw_frei(a);
    return 0;
}

static int befehl_dateien(int domain, const char *kennung, const char *pfad)
{
    Rx r;
    Xw *d, *a;
    char u[64];
    if (tunnel_an() || datei_sitzung(&r, domain, kennung)) return 1;
    uuid_text(u, sizeof u);
    d = xw_dict();
    xw_setzen(d, "Cmd", xw_str("RetrieveDirectoryList"));
    xw_setzen(d, "MessageUUID", xw_str(u));
    xw_setzen(d, "Path", xw_str(pfad));
    xw_setzen(d, "SessionID", xw_str(Datei_sitzung));
    a = datei_frage(&r, d);
    if (a) { xw_zeigen(a, 0); xw_frei(a); }
    rx_zu(&r);
    return a ? 0 : 1;
}

/* Test: wie lange laeuft der Bildstrom ohne Rueckmeldung? */
static int befehl_strom(int sekunden)
{
    double t0, t;
    unsigned long vorher = 0;
    if (tunnel_an() || bildstrom_an()) return 1;
    t0 = t = jetzt();
    while (jetzt() - t0 < sekunden) {
        warten(2);
        printf("%3.0f s  %6.0f kbit/s  RTP %lu Pakete, Folge %lu, von Port %d\n", jetzt() - t0,
               (tunnel_udp_bytes() - vorher) * 8 / 1000.0 / (jetzt() - t), tunnel_rtp_pakete(), tunnel_rtp_seq(), tunnel_rtp_quelle());
        fflush(stdout);
        vorher = tunnel_udp_bytes();
        t = jetzt();
    }
    bildstrom_aus();
    return 0;
}

static void xml_zeigen(CFTypeRef v)
{
    CFDataRef x = CFPropertyListCreateXMLData(NULL, v);
    fwrite(CFDataGetBytePtr(x), 1, (size_t)CFDataGetLength(x), stdout);
    CFRelease(x);
}

/* Abbild-Dienst: was ist geladen, was verlangt das Geraet zur Personalisierung */
static int befehl_abbild(void)
{
    Kanal ld, k;
    const char *befehle[] = { "CopyDevices", "QueryDeveloperModeStatus", "QueryPersonalizationIdentifiers", "QueryNonce" };
    unsigned i;
    if (ld_sitzung(&ld)) return 1;
    if (dienst(&ld, "com.apple.mobile.mobile_image_mounter", &k)) { k_zu(&ld); return 1; }
    for (i = 0; i < 4; i++) {
        CFMutableDictionaryRef d = d_neu(), r;
        d_str(d, "Command", befehle[i]);
        if (i >= 2) d_str(d, "PersonalizedImageType", "DeveloperDiskImage");
        printf("== %s\n", befehle[i]);
        if (ld_senden(&k, d)) { CFRelease(d); break; }
        CFRelease(d);
        r = ld_empfangen(&k);
        if (!r) { printf("keine Antwort\n"); break; }
        xml_zeigen(r);
        CFRelease(r);
    }
    k_zu(&k);
    k_zu(&ld);
    return 0;
}

/* einzelner Wert: geraet wert [domain] schluessel */
static int befehl_wert(const char *domain, const char *schl)
{
    Kanal k;
    CFTypeRef v;
    char b[1024];
    if (ld_sitzung(&k)) return 1;
    v = ld_wert(&k, domain, schl);
    k_zu(&k);
    if (!v) return 1;
    if (CFGetTypeID(v) == CFDictionaryGetTypeID()) zeigen(v);
    else if (!wert_text(v, b, sizeof b)) printf("%s\n", b);
    CFRelease(v);
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1;
    const char *h = getenv("HOME");
    signal(SIGPIPE, SIG_IGN);
    snprintf(Paar_pfad, sizeof Paar_pfad, "%s/tools/fernbedienung/iphone/paarung.plist", h ? h : "");
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-w") && i + 1 < argc) Wlan = argv[++i];
        else if (!strcmp(argv[i], "-v")) Laut++;
        else if (!strcmp(argv[i], "-n")) Nur_wlan = 1;
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) Strom_bps = strtoull(argv[++i], NULL, 10);
    }
    if (i >= argc) { meld("geraet [-v] [-w adresse] info | koppeln | entwickler | wlan"); return 2; }
    if (!strcmp(argv[i], "info")) return befehl_info();
    if (!strcmp(argv[i], "koppeln")) return befehl_koppeln();
    if (!strcmp(argv[i], "entwickler")) return befehl_entwickler();
    if (!strcmp(argv[i], "wlan")) return befehl_wlan();
    if (!strcmp(argv[i], "tunnel")) return befehl_tunnel();
    if (!strcmp(argv[i], "abbild")) return befehl_abbild();
    if (!strcmp(argv[i], "dienste")) return befehl_dienste();
    if (!strcmp(argv[i], "ddi")) return befehl_ddi();
    if (!strcmp(argv[i], "strom")) return befehl_strom(i + 1 < argc ? atoi(argv[i + 1]) : 60);
    if (!strcmp(argv[i], "steuern")) return befehl_steuern();
    if (!strcmp(argv[i], "ablage")) return befehl_ablage(i + 1 < argc ? argv[i + 1] : NULL);
    if (!strcmp(argv[i], "dateien") && i + 2 < argc)
        return befehl_dateien(atoi(argv[i + 1]), argv[i + 2], i + 3 < argc ? argv[i + 3] : ".");
    if (!strcmp(argv[i], "angebot")) { Angebot_zeigen = 1; CFRelease(bildstrom_angebot(2368635137u, "X")); return 0; }
    if (!strcmp(argv[i], "tippen") && i + 2 < argc)
        return befehl_wischen(anteil(argv[i + 1]), anteil(argv[i + 2]), anteil(argv[i + 1]), anteil(argv[i + 2]), 0);
    if (!strcmp(argv[i], "wischen") && i + 4 < argc)
        return befehl_wischen(anteil(argv[i + 1]), anteil(argv[i + 2]), anteil(argv[i + 3]), anteil(argv[i + 4]), 0.3);
    if (!strcmp(argv[i], "wert") && i + 2 < argc) return befehl_wert(argv[i + 1], argv[i + 2]);
    if (!strcmp(argv[i], "wert") && i + 1 < argc) return befehl_wert(NULL, argv[i + 1]);
    meld("unbekannt: %s", argv[i]);
    return 2;
}
