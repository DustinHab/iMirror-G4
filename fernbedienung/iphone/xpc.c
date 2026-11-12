/* XPC-Werte (RemoteXPC-Format) und RemoteXPC ueber HTTP/2 auf einer Tunnel-Verbindung.
   Aufbau nach pymobiledevice3 (remotexpc.py, xpc_message.py). */
#include "xpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WRAPPER_MAGIC 0x29B00B92u
#define PAYLOAD_MAGIC 0x42133742u
#define F_ALWAYS 0x1u
#define F_DATA 0x100u
#define F_WANTING_REPLY 0x10000u
#define F_FILE_TX_REQUEST 0x100000u
#define F_INIT_HANDSHAKE 0x400000u
#define FENSTER_START (16 * 1024 * 1024)
#define MAX_RAHMEN 16384

/* ---------- Puffer ---------- */

void pf_rein(Puffer *p, const void *d, size_t n)
{
    if (p->n + n > p->cap) {
        p->cap = (p->n + n) * 2 + 64;
        p->p = realloc(p->p, p->cap);
    }
    if (n) memcpy(p->p + p->n, d, n);
    p->n += n;
}

void pf_frei(Puffer *p) { free(p->p); p->p = NULL; p->n = p->cap = 0; }

static void pf_u32(Puffer *p, uint32_t v)
{
    unsigned char b[4] = { v, v >> 8, v >> 16, v >> 24 };
    pf_rein(p, b, 4);
}

static void pf_u64(Puffer *p, unsigned long long v)
{
    unsigned char b[8];
    int i;
    for (i = 0; i < 8; i++) b[i] = (unsigned char)(v >> (8 * i));
    pf_rein(p, b, 8);
}

static void pf_null4(Puffer *p, size_t start)
{
    static const unsigned char z[4];
    size_t l = p->n - start;
    if (l % 4) pf_rein(p, z, 4 - l % 4);
}

static void setze_u32(unsigned char *b, uint32_t v) { b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }
static uint32_t lies_u32(const unsigned char *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static unsigned long long lies_u64(const unsigned char *b)
{
    unsigned long long v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = v << 8 | b[i];
    return v;
}

/* ---------- Werte ---------- */

static Xw *neu(uint32_t typ)
{
    Xw *w = calloc(1, sizeof *w);
    w->typ = typ;
    return w;
}

Xw *xw_dict(void) { return neu(XW_DICT); }
Xw *xw_array(void) { return neu(XW_ARRAY); }
Xw *xw_null(void) { return neu(XW_NULL); }
Xw *xw_str(const char *s) { Xw *w = neu(XW_STRING); w->s = strdup(s); return w; }
Xw *xw_int(long long v) { Xw *w = neu(XW_INT); w->i = v; return w; }
Xw *xw_uint(unsigned long long v) { Xw *w = neu(XW_UINT); w->i = (long long)v; return w; }
Xw *xw_bool(int v) { Xw *w = neu(XW_BOOL); w->i = v ? 1 : 0; return w; }

Xw *xw_daten(const void *b, size_t n)
{
    Xw *w = neu(XW_DATA);
    w->b = malloc(n ? n : 1);
    if (n) memcpy(w->b, b, n);
    w->n = n;
    return w;
}

Xw *xw_uuid(const unsigned char u[16])
{
    Xw *w = neu(XW_UUID);
    w->b = malloc(16);
    memcpy(w->b, u, 16);
    w->n = 16;
    return w;
}

Xw *xw_datei(unsigned long long id, unsigned long long groesse)
{
    Xw *w = neu(XW_DATEI);
    w->id = id;
    w->i = (long long)groesse;
    return w;
}

static void kind_rein(Xw *c, const char *schl, Xw *wert)
{
    c->kind = realloc(c->kind, sizeof(Xw *) * (c->n + 1));
    if (c->typ == XW_DICT) {
        c->schl = realloc(c->schl, sizeof(char *) * (c->n + 1));
        c->schl[c->n] = strdup(schl);
    }
    c->kind[c->n++] = wert;
}

Xw *xw_setzen(Xw *d, const char *schl, Xw *wert) { kind_rein(d, schl, wert); return d; }
Xw *xw_anhaengen(Xw *a, Xw *wert) { kind_rein(a, NULL, wert); return a; }

Xw *xw_holen(const Xw *d, const char *schl)
{
    size_t i;
    if (!d || d->typ != XW_DICT) return NULL;
    for (i = 0; i < d->n; i++) if (!strcmp(d->schl[i], schl)) return d->kind[i];
    return NULL;
}

const char *xw_text(const Xw *d, const char *schl)
{
    Xw *w = xw_holen(d, schl);
    return w && w->typ == XW_STRING ? w->s : NULL;
}

long long xw_zahl(const Xw *d, const char *schl, long long std)
{
    Xw *w = xw_holen(d, schl);
    if (!w) return std;
    if (w->typ == XW_INT || w->typ == XW_UINT || w->typ == XW_BOOL) return w->i;
    if (w->typ == XW_STRING) return atoll(w->s);
    return std;
}

void xw_frei(Xw *w)
{
    size_t i;
    if (!w) return;
    if (w->typ == XW_DICT || w->typ == XW_ARRAY) {
        for (i = 0; i < w->n; i++) {
            xw_frei(w->kind[i]);
            if (w->schl) free(w->schl[i]);
        }
        free(w->kind);
        free(w->schl);
    }
    free(w->s);
    free(w->b);
    free(w);
}

void xw_zeigen(const Xw *w, int tiefe)
{
    size_t i;
    switch (w->typ) {
    case XW_DICT:
        printf("{\n");
        for (i = 0; i < w->n; i++) {
            printf("%*s%s: ", (tiefe + 1) * 2, "", w->schl[i]);
            xw_zeigen(w->kind[i], tiefe + 1);
        }
        printf("%*s}\n", tiefe * 2, "");
        break;
    case XW_ARRAY:
        printf("[\n");
        for (i = 0; i < w->n; i++) {
            printf("%*s", (tiefe + 1) * 2, "");
            xw_zeigen(w->kind[i], tiefe + 1);
        }
        printf("%*s]\n", tiefe * 2, "");
        break;
    case XW_STRING: printf("\"%s\"\n", w->s); break;
    case XW_INT: printf("%lld\n", w->i); break;
    case XW_UINT: printf("%llu\n", (unsigned long long)w->i); break;
    case XW_BOOL: printf("%s\n", w->i ? "ja" : "nein"); break;
    case XW_DOUBLE: printf("%g\n", w->d); break;
    case XW_NULL: printf("null\n"); break;
    case XW_UUID:
        for (i = 0; i < 16; i++) printf("%02X%s", w->b[i], i == 3 || i == 5 || i == 7 || i == 9 ? "-" : "");
        printf("\n");
        break;
    case XW_DATA:
        printf("<%lu Bytes", (unsigned long)w->n);
        for (i = 0; i < w->n && i < 24; i++) printf("%s%02x", i ? "" : " ", w->b[i]);
        printf("%s>\n", w->n > 24 ? "..." : "");
        break;
    case XW_DATEI: printf("<Datei %llu Bytes>\n", (unsigned long long)w->i); break;
    default: printf("<Typ %x>\n", w->typ);
    }
}

/* ---------- Kodierung ---------- */

void xw_kodieren(const Xw *w, Puffer *p)
{
    size_t i, start, laenge_pos;
    unsigned char *b;
    pf_u32(p, w->typ);
    switch (w->typ) {
    case XW_NULL: break;
    case XW_BOOL: pf_u32(p, w->i ? 1 : 0); break;
    case XW_INT: case XW_UINT: case XW_DATE: pf_u64(p, (unsigned long long)w->i); break;
    case XW_DOUBLE: {
        union { double d; unsigned long long u; } x;
        x.d = w->d;
        pf_u64(p, x.u);
        break;
    }
    case XW_STRING:
        start = p->n;
        pf_u32(p, (uint32_t)strlen(w->s) + 1);
        pf_rein(p, w->s, strlen(w->s) + 1);
        pf_null4(p, start);
        break;
    case XW_DATA:
        start = p->n;
        pf_u32(p, (uint32_t)w->n);
        pf_rein(p, w->b, w->n);
        pf_null4(p, start);
        break;
    case XW_UUID: pf_rein(p, w->b, 16); break;
    case XW_ARRAY: case XW_DICT:
        laenge_pos = p->n;
        pf_u32(p, 0);
        pf_u32(p, (uint32_t)w->n);
        for (i = 0; i < w->n; i++) {
            if (w->typ == XW_DICT) {
                start = p->n;
                pf_rein(p, w->schl[i], strlen(w->schl[i]) + 1);
                pf_null4(p, start);
            }
            xw_kodieren(w->kind[i], p);
        }
        b = p->p + laenge_pos;
        setze_u32(b, (uint32_t)(p->n - laenge_pos - 4));
        break;
    case XW_DATEI: {
        Xw *d = xw_dict();
        xw_setzen(d, "s", xw_uint((unsigned long long)w->i));
        pf_u64(p, w->id);
        xw_kodieren(d, p);
        xw_frei(d);
        break;
    }
    }
}

static size_t auf4(size_t x) { return (x + 3) & ~(size_t)3; }

Xw *xw_dekodieren(const unsigned char *p, size_t n, size_t *pos)
{
    uint32_t typ, l, anz, k;
    size_t a = *pos;
    Xw *w;
    if (a + 4 > n) return NULL;
    typ = lies_u32(p + a);
    a += 4;
    w = neu(typ);
    switch (typ) {
    case XW_NULL: break;
    case XW_BOOL: case XW_FD:
        if (a + 4 > n) goto kaputt;
        w->i = lies_u32(p + a);
        a += 4;
        break;
    case XW_INT: case XW_UINT: case XW_DATE:
        if (a + 8 > n) goto kaputt;
        w->i = (long long)lies_u64(p + a);
        a += 8;
        break;
    case XW_DOUBLE: {
        union { double d; unsigned long long u; } x;
        if (a + 8 > n) goto kaputt;
        x.u = lies_u64(p + a);
        w->d = x.d;
        a += 8;
        break;
    }
    case XW_STRING: case XW_DATA:
        if (a + 4 > n) goto kaputt;
        l = lies_u32(p + a);
        if (a + 4 + l > n) goto kaputt;
        if (typ == XW_STRING) {
            w->s = malloc(l + 1);
            memcpy(w->s, p + a + 4, l);
            w->s[l] = 0;
        } else {
            w->b = malloc(l ? l : 1);
            memcpy(w->b, p + a + 4, l);
            w->n = l;
        }
        a += auf4(4 + l);
        break;
    case XW_UUID:
        if (a + 16 > n) goto kaputt;
        w->b = malloc(16);
        memcpy(w->b, p + a, 16);
        w->n = 16;
        a += 16;
        break;
    case XW_SHMEM:
        a += 8;
        break;
    case XW_ARRAY: case XW_DICT: {
        size_t ende;
        if (a + 8 > n) goto kaputt;
        l = lies_u32(p + a);
        anz = lies_u32(p + a + 4);
        ende = a + 4 + l;
        if (ende > n) goto kaputt;
        a += 8;
        for (k = 0; k < anz; k++) {
            const char *schl = NULL;
            Xw *kind;
            if (typ == XW_DICT) {
                const unsigned char *e = memchr(p + a, 0, ende - a);
                if (!e) goto kaputt;
                schl = (const char *)(p + a);
                a += auf4((size_t)(e - (p + a)) + 1);
            }
            kind = xw_dekodieren(p, ende, &a);
            if (!kind) goto kaputt;
            kind_rein(w, schl, kind);
        }
        a = ende;
        break;
    }
    case XW_DATEI: {
        Xw *d;
        if (a + 8 > n) goto kaputt;
        w->id = lies_u64(p + a);
        a += 8;
        d = xw_dekodieren(p, n, &a);
        if (!d) goto kaputt;
        w->i = xw_zahl(d, "s", 0);
        xw_frei(d);
        break;
    }
    default: break;
    }
    *pos = a;
    return w;
kaputt:
    xw_frei(w);
    return NULL;
}

/* ---------- HTTP/2 ---------- */

static int rahmen(Rx *r, int typ, int flags, int strom, const void *d, size_t n)
{
    unsigned char k[9];
    k[0] = (unsigned char)(n >> 16); k[1] = (unsigned char)(n >> 8); k[2] = (unsigned char)n;
    k[3] = (unsigned char)typ;
    k[4] = (unsigned char)flags;
    k[5] = (unsigned char)(strom >> 24); k[6] = (unsigned char)(strom >> 16); k[7] = (unsigned char)(strom >> 8); k[8] = (unsigned char)strom;
    if (typ == 0) {
        r->fenster_aus -= (long long)n;
        if (strom < RX_STROEME) r->strom_fenster[strom] -= (long long)n;
    }
    if (n == 0) return tcp_schreiben(r->t, k, 9);
    {
        unsigned char *b = malloc(9 + n);
        int e;
        memcpy(b, k, 9);
        memcpy(b + 9, d, n);
        e = tcp_schreiben(r->t, b, 9 + n);
        free(b);
        return e;
    }
}

static void wrapper(Puffer *p, uint32_t flags, unsigned long long id, const Xw *nutz)
{
    Puffer inhalt = { 0 };
    if (nutz) {
        pf_u32(&inhalt, PAYLOAD_MAGIC);
        pf_u32(&inhalt, 5);
        xw_kodieren(nutz, &inhalt);
    }
    pf_u32(p, WRAPPER_MAGIC);
    pf_u32(p, flags);
    pf_u64(p, inhalt.n);
    pf_u64(p, id);
    pf_rein(p, inhalt.p, inhalt.n);
    pf_frei(&inhalt);
}

/* ein Rahmen von der Gegenseite; Daten landen in r->ein[strom]; 1 = Datenrahmen, 0 = anderes, -1 Fehler */
static int rahmen_lesen(Rx *r, int ms, int *strom_aus)
{
    unsigned char k[9], *d;
    size_t n;
    int typ, flags, strom;
    if (tcp_lesen_voll(r->t, k, 9, ms)) return -2;
    n = (size_t)k[0] << 16 | (size_t)k[1] << 8 | k[2];
    typ = k[3];
    flags = k[4];
    strom = (k[5] & 0x7f) << 24 | k[6] << 16 | k[7] << 8 | k[8];
    d = malloc(n ? n : 1);
    if (tcp_lesen_voll(r->t, d, n, 10000)) { free(d); return -1; }
    r->letzter_typ = typ;
    r->letzte_flags = flags;
    if (r->laut > 1) fprintf(stderr, "HTTP/2 ein: Typ %d Flags %x Strom %d, %lu Bytes\n", typ, flags, strom, (unsigned long)n);
    switch (typ) {
    case 0: {
        size_t start = 0, ende = n;
        if (flags & 0x08) { start = 1; ende = n - d[0]; }
        if (strom < RX_STROEME) {
            pf_rein(&r->ein[strom], d + start, ende > start ? ende - start : 0);
            r->offen_ein[strom] += (long long)n;
            if (r->offen_ein[strom] >= 1024 * 1024) {
                unsigned char w[4];
                long long x = r->offen_ein[strom];
                w[0] = (unsigned char)(x >> 24); w[1] = (unsigned char)(x >> 16); w[2] = (unsigned char)(x >> 8); w[3] = (unsigned char)x;
                rahmen(r, 8, 0, 0, w, 4);
                rahmen(r, 8, 0, strom, w, 4);
                r->offen_ein[strom] = 0;
            }
        }
        *strom_aus = strom;
        free(d);
        return 1;
    }
    case 4:
        if (!(flags & 1)) {
            size_t i;
            for (i = 0; i + 6 <= n; i += 6) {
                unsigned id = (unsigned)d[i] << 8 | d[i + 1];
                long long v = (long long)((uint32_t)d[i + 2] << 24 | d[i + 3] << 16 | d[i + 4] << 8 | d[i + 5]);
                if (id == 4) {
                    int s;
                    for (s = 0; s < RX_STROEME; s++) r->strom_fenster[s] += v - r->peer_start;
                    r->peer_start = v;
                }
            }
            rahmen(r, 4, 1, 0, NULL, 0);
        }
        break;
    case 6:
        if (!(flags & 1)) rahmen(r, 6, 1, 0, d, n);
        break;
    case 7:
        fprintf(stderr, "RemoteXPC: Gegenseite beendet (GOAWAY)\n");
        free(d);
        return -1;
    case 8:
        if (n >= 4) {
            long long v = (long long)(((uint32_t)d[0] & 0x7f) << 24 | d[1] << 16 | d[2] << 8 | d[3]);
            if (strom == 0) r->fenster_aus += v;
            else if (strom < RX_STROEME) r->strom_fenster[strom] += v;
        }
        break;
    case 3:
        if (r->laut) fprintf(stderr, "RemoteXPC: Strom %d zurueckgesetzt\n", strom);
        break;
    default: break;
    }
    free(d);
    return 0;
}

int rx_auf(Rx *r, Tcp *t)
{
    static const char magie[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    unsigned char e[12] = { 0, 3, 0, 0, 0, 100, 0, 4, 0, 0, 0, 0 }, w[4];
    Puffer p = { 0 };
    Xw *leer;
    int s, gesehen = 0, strom;
    double ende;
    memset(r, 0, sizeof *r);
    r->t = t;
    r->fenster_aus = r->peer_start = 65535;
    for (s = 0; s < RX_STROEME; s++) r->strom_fenster[s] = 65535;
    r->naechster_strom = 5;
    e[8] = FENSTER_START >> 24; e[9] = (FENSTER_START >> 16) & 0xff; e[10] = (FENSTER_START >> 8) & 0xff; e[11] = FENSTER_START & 0xff;
    if (tcp_schreiben(t, magie, sizeof magie - 1)) return -1;
    rahmen(r, 4, 0, 0, e, 12);
    s = FENSTER_START - 65535;
    w[0] = (unsigned char)(s >> 24); w[1] = (unsigned char)(s >> 16); w[2] = (unsigned char)(s >> 8); w[3] = (unsigned char)s;
    rahmen(r, 8, 0, 0, w, 4);
    rahmen(r, 1, 4, 1, NULL, 0);
    leer = xw_dict();
    wrapper(&p, F_ALWAYS, 0, leer);
    xw_frei(leer);
    rahmen(r, 0, 0, 1, p.p, p.n);
    p.n = 0;
    r->naechste_id = 1;
    rahmen(r, 1, 4, 3, NULL, 0);
    wrapper(&p, 0x0201, 0, NULL);
    rahmen(r, 0, 0, 1, p.p, p.n);
    p.n = 0;
    wrapper(&p, F_ALWAYS | F_INIT_HANDSHAKE, 0, NULL);
    if (rahmen(r, 0, 0, 3, p.p, p.n)) { pf_frei(&p); return -1; }
    pf_frei(&p);
    /* auf die Einstellungen der Gegenseite warten (rahmen_lesen bestaetigt sie) */
    ende = jetzt() + 5;
    while (!gesehen && jetzt() < ende) {
        int r2 = rahmen_lesen(r, 1000, &strom);
        if (r2 == -1) return -1;
        if (r2 == 0 && r->letzter_typ == 4 && !(r->letzte_flags & 1)) gesehen = 1;
    }
    if (!gesehen) fprintf(stderr, "RemoteXPC: keine Einstellungen von der Gegenseite\n");
    return gesehen ? 0 : -1;
}

int rx_senden(Rx *r, Xw *d, int antwort)
{
    Puffer p = { 0 };
    size_t pos = 0;
    uint32_t flags = F_ALWAYS;
    if (d && d->n) flags |= F_DATA;
    if (antwort) flags |= F_WANTING_REPLY;
    wrapper(&p, flags, r->naechste_id, d);
    r->naechste_id = (r->naechste_id + 1) | 1;
    while (pos < p.n) {
        size_t stueck = p.n - pos;
        long long budget = r->fenster_aus < r->strom_fenster[1] ? r->fenster_aus : r->strom_fenster[1];
        int strom;
        if (budget > MAX_RAHMEN) budget = MAX_RAHMEN;
        if (budget <= 0) {
            if (rahmen_lesen(r, 30000, &strom) < 0) { pf_frei(&p); return -1; }
            continue;
        }
        if ((long long)stueck > budget) stueck = (size_t)budget;
        if (rahmen(r, 0, 0, 1, p.p + pos, stueck)) { pf_frei(&p); return -1; }
        pos += stueck;
    }
    pf_frei(&p);
    return 0;
}

/* naechste nichtleere Nachricht (Woerterbuch) von irgendeinem Strom */
Xw *rx_empfangen(Rx *r, int ms)
{
    double ende = jetzt() + ms / 1000.0;
    for (;;) {
        int s, strom = -1, e;
        for (s = 0; s < RX_STROEME; s++) {
            Puffer *b = &r->ein[s];
            while (b->n >= 24) {
                unsigned long long groesse = lies_u64(b->p + 8);
                size_t pos = 24;
                Xw *w = NULL;
                if (lies_u32(b->p) != WRAPPER_MAGIC) { fprintf(stderr, "RemoteXPC: falsche Kennung\n"); b->n = 0; break; }
                if (b->n < 24 + groesse) break;
                if (groesse >= 8 && lies_u32(b->p + 24) == PAYLOAD_MAGIC) {
                    pos = 32;
                    w = xw_dekodieren(b->p, 24 + (size_t)groesse, &pos);
                }
                memmove(b->p, b->p + 24 + groesse, b->n - 24 - (size_t)groesse);
                b->n -= 24 + (size_t)groesse;
                if (w && w->typ == XW_DICT && w->n) return w;
                xw_frei(w);
            }
        }
        if (jetzt() > ende) return NULL;
        e = rahmen_lesen(r, 200, &strom);
        if (e == -1) return NULL;
    }
}

/* Inhalt einer angekuendigten Dateiuebertragung auf eigenem Strom */
int rx_datei(Rx *r, unsigned long long kennung, const unsigned char *b, size_t n)
{
    int strom = r->naechster_strom, x;
    Puffer p = { 0 };
    size_t pos = 0;
    r->naechster_strom += 2;
    if (strom >= RX_STROEME) return -1;
    rahmen(r, 1, 4, strom, NULL, 0);
    wrapper(&p, F_FILE_TX_REQUEST | F_ALWAYS, kennung, NULL);
    if (rahmen(r, 0, 0, strom, p.p, p.n)) { pf_frei(&p); return -1; }
    pf_frei(&p);
    while (pos < n) {
        size_t stueck = n - pos;
        long long budget = r->fenster_aus < r->strom_fenster[strom] ? r->fenster_aus : r->strom_fenster[strom];
        if (budget > MAX_RAHMEN) budget = MAX_RAHMEN;
        if (budget <= 0) {
            if (rahmen_lesen(r, 30000, &x) < 0) return -1;
            continue;
        }
        if ((long long)stueck > budget) stueck = (size_t)budget;
        if (rahmen(r, 0, 0, strom, b + pos, stueck)) return -1;
        pos += stueck;
    }
    return rahmen(r, 0, 1, strom, NULL, 0);
}

/* alles bereits Empfangene verarbeiten, ohne zu warten: PING beantworten, Einstellungen und
   Fenster uebernehmen. Ohne das schliesst das iPhone nach einer Weile die Verbindung.
   -1 wenn die Verbindung zu ist. */
int rx_pflegen(Rx *r)
{
    int s, strom;
    if (!r->t || !tcp_offen(r->t)) return -1;
    for (;;) {
        unsigned char k[9];
        size_t n;
        if (tcp_spaehen(r->t, k, 9) < 9) break;
        n = (size_t)k[0] << 16 | (size_t)k[1] << 8 | k[2];
        if (tcp_verfuegbar(r->t) < 9 + n) break;
        if (rahmen_lesen(r, 0, &strom) == -1) return -1;
    }
    /* Antworten, die niemand abholt, nicht endlos sammeln */
    if (!r->behalten) for (s = 0; s < RX_STROEME; s++) if (r->ein[s].n > 256 * 1024) r->ein[s].n = 0;
    return tcp_offen(r->t) ? 0 : -1;
}

Xw *rx_abholen(Rx *r)
{
    int s;
    for (s = 0; s < RX_STROEME; s++) {
        Puffer *b = &r->ein[s];
        while (b->n >= 24) {
            unsigned long long groesse = lies_u64(b->p + 8);
            size_t pos = 32;
            Xw *w = NULL;
            if (lies_u32(b->p) != WRAPPER_MAGIC) { b->n = 0; break; }
            if (b->n < 24 + groesse) break;
            if (groesse >= 8 && lies_u32(b->p + 24) == PAYLOAD_MAGIC) w = xw_dekodieren(b->p, 24 + (size_t)groesse, &pos);
            memmove(b->p, b->p + 24 + groesse, b->n - 24 - (size_t)groesse);
            b->n -= 24 + (size_t)groesse;
            if (w && w->typ == XW_DICT && w->n) return w;
            xw_frei(w);
        }
    }
    return NULL;
}

void rx_zu(Rx *r)
{
    int s;
    tcp_zu(r->t);
    r->t = NULL;
    for (s = 0; s < RX_STROEME; s++) pf_frei(&r->ein[s]);
}
