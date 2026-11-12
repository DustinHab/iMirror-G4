/* Entwickler-Tunnel: nach dem CDTunnel-Handschlag fliessen rohe IPv6-Pakete ueber die TLS-Verbindung.
   Darauf ein kleines TCP (nur aktive Verbindungen, Fenster der Gegenseite, einfache Wiederholung);
   UDP (Bildstrom fuer die Freigabe der Eingabe) wird gezaehlt und verworfen. */
#include "tunnel.h"
#include <arpa/inet.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TCP 16

struct Tcp {
    int zustand;                 /* 0 frei, 1 SYN gesendet, 2 offen, 3 Gegenseite zu, 4 abgebrochen */
    uint16_t lport, rport;
    uint32_t snd_una, snd_nxt, rcv_nxt, isn;
    uint32_t peer_fenster;
    int mss;
    unsigned char *ein;
    size_t ein_n, ein_pos, ein_cap;
    unsigned char *aus;          /* noch nicht bestaetigte Daten ab snd_una */
    size_t aus_n, aus_cap;
    double bewegt;               /* letzte Bestaetigung oder Sendung */
};

static TunnelWeg *Weg;
static unsigned char Ich[16], Er[16];
static char Ich_text[64], Er_text[64];
static int Mtu = 1280, Laut;
static Tcp Verb[MAX_TCP];
static uint16_t Naechster_port;
static unsigned long Udp_bytes;

double jetzt(void)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    return t.tv_sec + t.tv_usec / 1e6;
}

static void be16(unsigned char *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static void be32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static int nach(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }   /* a liegt nach b */

/* ---------- Handschlag ---------- */

static int json_text(const char *j, const char *schl, char *aus, size_t n)
{
    char m[64];
    const char *p, *e;
    snprintf(m, sizeof m, "\"%s\"", schl);
    p = strstr(j, m);
    if (!p) return -1;
    p = strchr(p + strlen(m), ':');
    if (!p) return -1;
    p++;
    while (*p == ' ') p++;
    if (*p == '"') {
        e = strchr(++p, '"');
        if (!e) return -1;
    } else {
        e = p;
        while (*e && *e != ',' && *e != '}') e++;
    }
    if ((size_t)(e - p) >= n) return -1;
    memcpy(aus, p, (size_t)(e - p));
    aus[e - p] = 0;
    return 0;
}

int tunnel_start(TunnelWeg *w, int *rsd_port, int laut)
{
    const char *frage = "{\"type\":\"clientHandshakeRequest\",\"mtu\":16000}";
    unsigned char h[10];
    size_t n = strlen(frage);
    unsigned m;
    char *antwort, wert[64];
    Weg = w;
    Laut = laut;
    memcpy(h, "CDTunnel", 8);
    be16(h + 8, (unsigned)n);
    if (w->schreiben(w->ctx, h, 10) || w->schreiben(w->ctx, frage, n) || w->lesen(w->ctx, h, 10)
        || memcmp(h, "CDTunnel", 8)) return -1;
    m = rd16(h + 8);
    antwort = calloc(1, m + 1);
    if (w->lesen(w->ctx, antwort, m)) { free(antwort); return -1; }
    if (Laut) fprintf(stderr, "Tunnel: %s\n", antwort);
    if (json_text(antwort, "address", Ich_text, sizeof Ich_text) || json_text(antwort, "serverAddress", Er_text, sizeof Er_text)
        || inet_pton(AF_INET6, Ich_text, Ich) != 1 || inet_pton(AF_INET6, Er_text, Er) != 1) { free(antwort); return -1; }
    if (!json_text(antwort, "mtu", wert, sizeof wert)) Mtu = atoi(wert);
    if (Mtu < 1280 || Mtu > 16000) Mtu = 1280;
    if (json_text(antwort, "serverRSDPort", wert, sizeof wert)) { free(antwort); return -1; }
    *rsd_port = atoi(wert);
    free(antwort);
    srandom((unsigned)(jetzt() * 1000));
    Naechster_port = (uint16_t)(50000 + random() % 10000);
    return 0;
}

static int Rtp_port, Rtp_quelle;
static unsigned long Rtp_seq, Rtp_pakete;

int tunnel_rtp_quelle(void) { return Rtp_quelle; }

void tunnel_rtp_port(int port) { Rtp_port = port; Rtp_seq = Rtp_pakete = 0; }
unsigned long tunnel_rtp_seq(void) { return Rtp_seq; }
unsigned long tunnel_rtp_pakete(void) { return Rtp_pakete; }

const char *tunnel_ich(void) { return Ich_text; }
const char *tunnel_geraet(void) { return Er_text; }
unsigned long tunnel_udp_bytes(void) { return Udp_bytes; }

/* ---------- Senden ---------- */

static uint16_t pruefsumme(const unsigned char *d, size_t n, uint8_t proto)
{
    uint32_t s = 0;
    size_t i;
    for (i = 0; i < 16; i += 2) s += (uint32_t)Ich[i] << 8 | Ich[i + 1];
    for (i = 0; i < 16; i += 2) s += (uint32_t)Er[i] << 8 | Er[i + 1];
    s += (uint32_t)(n >> 16) + (uint32_t)(n & 0xffff) + proto;
    for (i = 0; i + 1 < n; i += 2) s += (uint32_t)d[i] << 8 | d[i + 1];
    if (n & 1) s += (uint32_t)d[n - 1] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

static int segment(Tcp *c, uint8_t flags, uint32_t seq, const unsigned char *d, size_t n)
{
    unsigned char *p;
    size_t kopf = (flags & 0x02) ? 24 : 20, gesamt = 40 + kopf + n;
    unsigned char *t;
    int r;
    p = malloc(gesamt);
    p[0] = 0x60; p[1] = p[2] = p[3] = 0;
    be16(p + 4, (unsigned)(kopf + n));
    p[6] = 6;
    p[7] = 64;
    memcpy(p + 8, Ich, 16);
    memcpy(p + 24, Er, 16);
    t = p + 40;
    be16(t, c->lport);
    be16(t + 2, c->rport);
    be32(t + 4, seq);
    be32(t + 8, (flags & 0x10) ? c->rcv_nxt : 0);
    t[12] = (unsigned char)((kopf / 4) << 4);
    t[13] = flags;
    be16(t + 14, 65535);
    t[16] = t[17] = t[18] = t[19] = 0;
    if (kopf == 24) { t[20] = 2; t[21] = 4; be16(t + 22, (unsigned)(Mtu - 60)); }
    if (n) memcpy(t + kopf, d, n);
    be16(t + 16, pruefsumme(t, kopf + n, 6));
    r = Weg->schreiben(Weg->ctx, p, gesamt);
    free(p);
    return r;
}

static void ack(Tcp *c) { segment(c, 0x10, c->snd_nxt, NULL, 0); }

int tunnel_udp_senden(int lport, int rport, const void *d, size_t n)
{
    size_t gesamt = 48 + n;
    unsigned char *p = malloc(gesamt), *u = p + 40;
    unsigned s;
    int r;
    p[0] = 0x60; p[1] = p[2] = p[3] = 0;
    be16(p + 4, (unsigned)(8 + n));
    p[6] = 17;
    p[7] = 64;
    memcpy(p + 8, Ich, 16);
    memcpy(p + 24, Er, 16);
    be16(u, (unsigned)lport);
    be16(u + 2, (unsigned)rport);
    be16(u + 4, (unsigned)(8 + n));
    u[6] = u[7] = 0;
    memcpy(u + 8, d, n);
    s = pruefsumme(u, 8 + n, 17);
    if (!s) s = 0xffff;
    be16(u + 6, s);
    r = Weg->schreiben(Weg->ctx, p, gesamt);
    free(p);
    return r;
}

/* ab Position snd_nxt so viel senden, wie das Fenster erlaubt */
static int nachschieben(Tcp *c)
{
    for (;;) {
        size_t unterwegs = c->snd_nxt - c->snd_una, rest, stueck;
        size_t fenster = c->peer_fenster ? c->peer_fenster : 1;
        if (unterwegs >= c->aus_n || unterwegs >= fenster) return 0;
        rest = c->aus_n - unterwegs;
        stueck = fenster - unterwegs;
        if (stueck > rest) stueck = rest;
        if (stueck > (size_t)c->mss) stueck = (size_t)c->mss;
        if (segment(c, 0x18, c->snd_nxt, c->aus + unterwegs, stueck)) return -1;
        c->snd_nxt += (uint32_t)stueck;
        c->bewegt = jetzt();
    }
}

static void wiederholen(void)
{
    int i;
    double t = jetzt();
    for (i = 0; i < MAX_TCP; i++) {
        Tcp *c = &Verb[i];
        if (c->zustand == 1 && t - c->bewegt > 1.0) {
            segment(c, 0x02, c->isn, NULL, 0);
            c->bewegt = t;
        } else if ((c->zustand == 2 || c->zustand == 3) && c->snd_nxt != c->snd_una && t - c->bewegt > 1.0) {
            size_t stueck = c->snd_nxt - c->snd_una;
            if (stueck > (size_t)c->mss) stueck = (size_t)c->mss;
            if (Laut) fprintf(stderr, "Tunnel: wiederhole %lu Bytes an Port %u\n", (unsigned long)stueck, c->rport);
            segment(c, 0x18, c->snd_una, c->aus, stueck);
            c->bewegt = t;
        }
    }
}

/* ---------- Empfang ---------- */

static void tcp_ein(const unsigned char *t, size_t n)
{
    unsigned sport, dport, kopf, flags;
    uint32_t seq, an;
    size_t laenge;
    Tcp *c = NULL;
    int i;
    if (n < 20) return;
    sport = rd16(t);
    dport = rd16(t + 2);
    seq = rd32(t + 4);
    an = rd32(t + 8);
    kopf = (unsigned)(t[12] >> 4) * 4;
    flags = t[13];
    if (kopf < 20 || kopf > n) return;
    laenge = n - kopf;
    for (i = 0; i < MAX_TCP; i++)
        if (Verb[i].zustand && Verb[i].lport == dport && Verb[i].rport == sport) { c = &Verb[i]; break; }
    if (!c) return;
    if (flags & 0x04) {
        if (Laut) fprintf(stderr, "Tunnel: Port %u vom iPhone zurueckgesetzt\n", c->rport);
        c->zustand = 4;
        return;
    }
    if (c->zustand == 1) {
        if ((flags & 0x12) == 0x12 && an == c->isn + 1) {
            unsigned o = 20;
            c->rcv_nxt = seq + 1;
            c->snd_una = c->snd_nxt = an;
            c->peer_fenster = rd16(t + 14);
            c->mss = 1220;
            while (o + 1 < kopf) {
                if (t[o] == 0) break;
                if (t[o] == 1) { o++; continue; }
                if (t[o] == 2 && t[o + 1] == 4 && o + 3 < kopf) c->mss = (int)rd16(t + o + 2);
                if (t[o + 1] < 2) break;
                o += t[o + 1];
            }
            if (c->mss > Mtu - 60) c->mss = Mtu - 60;
            c->zustand = 2;
            ack(c);
        }
        return;
    }
    if (flags & 0x10) {
        if (nach(an, c->snd_una) && !nach(an, c->snd_nxt)) {
            size_t weg = an - c->snd_una;
            memmove(c->aus, c->aus + weg, c->aus_n - weg);
            c->aus_n -= weg;
            c->snd_una = an;
            c->bewegt = jetzt();
        }
        c->peer_fenster = rd16(t + 14);
    }
    if (laenge) {
        if (seq == c->rcv_nxt) {
            if (c->ein_pos && c->ein_pos == c->ein_n) c->ein_pos = c->ein_n = 0;
            if (c->ein_n + laenge > c->ein_cap) {
                if (c->ein_pos) {
                    memmove(c->ein, c->ein + c->ein_pos, c->ein_n - c->ein_pos);
                    c->ein_n -= c->ein_pos;
                    c->ein_pos = 0;
                }
                if (c->ein_n + laenge > c->ein_cap) {
                    c->ein_cap = (c->ein_n + laenge) * 2;
                    c->ein = realloc(c->ein, c->ein_cap);
                }
            }
            memcpy(c->ein + c->ein_n, t + kopf, laenge);
            c->ein_n += laenge;
            c->rcv_nxt += (uint32_t)laenge;
        }
        ack(c);
    }
    if ((flags & 0x01) && seq + laenge == c->rcv_nxt) {
        c->rcv_nxt++;
        c->zustand = 3;
        ack(c);
        if (Laut) fprintf(stderr, "Tunnel: Port %u vom iPhone geschlossen\n", c->rport);
    } else if (!laenge && !(flags & 0x01) && seq != c->rcv_nxt) {
        /* TCP-Keepalive (Folgenummer eins zurueck, ohne Daten): bestaetigen, sonst bricht iOS ab */
        ack(c);
    }
    nachschieben(c);
}

int tunnel_pumpen(int ms)
{
    unsigned char k[40], *p;
    unsigned n;
    int r;
    wiederholen();
    r = Weg->bereit(Weg->ctx, ms);
    if (r <= 0) return r;
    if (Weg->lesen(Weg->ctx, k, 40)) return -1;
    if ((k[0] >> 4) != 6) { fprintf(stderr, "Tunnel: kein IPv6-Paket\n"); return -1; }
    n = rd16(k + 4);
    p = malloc(n ? n : 1);
    if (Weg->lesen(Weg->ctx, p, n)) { free(p); return -1; }
    if (k[6] == 6) tcp_ein(p, n);
    else if (k[6] == 17) {
        Udp_bytes += n;
        if (n >= 8 + 12 && (int)rd16(p + 2) == Rtp_port && ((p[9] & 0x7f) < 64 || (p[9] & 0x7f) > 95)) {
            unsigned seq = rd16(p + 8 + 2), letzte = Rtp_seq & 0xffff;
            unsigned long zyklen = Rtp_seq >> 16;
            Rtp_quelle = (int)rd16(p);
            if (!Rtp_pakete) Rtp_seq = seq;
            else if (seq < letzte && letzte - seq > 0x8000) Rtp_seq = (zyklen + 1) << 16 | seq;
            else if (seq > letzte && seq - letzte < 0x8000) Rtp_seq = zyklen << 16 | seq;
            Rtp_pakete++;
        }
    }
    free(p);
    return 1;
}

/* ---------- Verbindungen ---------- */

Tcp *tcp_auf(int port)
{
    Tcp *c = NULL;
    int i;
    double start;
    for (i = 0; i < MAX_TCP; i++) if (!Verb[i].zustand) { c = &Verb[i]; break; }
    if (!c) return NULL;
    memset(c, 0, sizeof *c);
    c->zustand = 1;
    c->lport = Naechster_port++;
    if (Naechster_port > 60000) Naechster_port = 50000;
    c->rport = (uint16_t)port;
    c->isn = (uint32_t)random();
    c->snd_una = c->snd_nxt = c->isn;
    c->mss = 1220;
    c->bewegt = start = jetzt();
    if (segment(c, 0x02, c->isn, NULL, 0)) { c->zustand = 0; return NULL; }
    while (c->zustand == 1 && jetzt() - start < 8) if (tunnel_pumpen(200) < 0) break;
    if (c->zustand != 2) {
        if (Laut) fprintf(stderr, "Tunnel: Port %d nicht erreichbar\n", port);
        c->zustand = 0;
        return NULL;
    }
    return c;
}

int tcp_offen(Tcp *c) { return c && c->zustand == 2; }
int tcp_zustand(Tcp *c) { return c ? c->zustand : -1; }

int tcp_schreiben(Tcp *c, const void *b, size_t n)
{
    double start = jetzt();
    if (!c || c->zustand != 2) return -1;
    if (c->aus_n + n > c->aus_cap) {
        c->aus_cap = (c->aus_n + n) * 2;
        c->aus = realloc(c->aus, c->aus_cap);
    }
    memcpy(c->aus + c->aus_n, b, n);
    c->aus_n += n;
    if (nachschieben(c)) return -1;
    /* nicht unbegrenzt puffern: warten, bis alles unterwegs ist */
    while (c->zustand == 2 && (size_t)(c->snd_nxt - c->snd_una) < c->aus_n) {
        if (tunnel_pumpen(50) < 0) return -1;
        if (nachschieben(c)) return -1;
        if (jetzt() - start > 30) { fprintf(stderr, "Tunnel: Gegenseite nimmt nichts an\n"); return -1; }
    }
    return c->zustand == 2 ? 0 : -1;
}

size_t tcp_verfuegbar(Tcp *c) { return c->ein_n - c->ein_pos; }

/* ansehen ohne zu verbrauchen */
size_t tcp_spaehen(Tcp *c, void *b, size_t n)
{
    size_t v = tcp_verfuegbar(c);
    if (n > v) n = v;
    memcpy(b, c->ein + c->ein_pos, n);
    return n;
}

int tcp_lesen_voll(Tcp *c, void *b, size_t n, int ms)
{
    double ende = jetzt() + ms / 1000.0;
    while (tcp_verfuegbar(c) < n) {
        if (c->zustand != 2) return -1;
        if (jetzt() > ende) return -1;
        if (tunnel_pumpen(50) < 0) return -1;
    }
    memcpy(b, c->ein + c->ein_pos, n);
    c->ein_pos += n;
    return 0;
}

void tcp_zu(Tcp *c)
{
    if (!c) return;
    if (c->zustand == 2 || c->zustand == 3) segment(c, 0x11, c->snd_nxt, NULL, 0);
    free(c->ein);
    free(c->aus);
    memset(c, 0, sizeof *c);
}
