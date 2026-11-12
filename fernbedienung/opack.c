/* OPACK lesen und schreiben (siehe opack.h) */
#include <stdlib.h>
#include <string.h>
#include "opack.h"

struct OBaum {
    OWert **alle; int nalle, capalle;     /* zum Freigeben */
    OWert **tab; int ntab, captab;        /* Rueckverweise */
    const unsigned char *b, *e;
    int fehler;
};

static void *mehr(void *p, int *cap, int n, size_t gr)
{
    if (n < *cap) return p;
    *cap = *cap ? *cap * 2 : 16;
    while (*cap <= n) *cap *= 2;
    return realloc(p, *cap * gr);
}

static OWert *neu(OBaum *t, OArt a)
{
    OWert *w = calloc(1, sizeof(OWert));
    t->alle = mehr(t->alle, &t->capalle, t->nalle, sizeof(OWert *));
    t->alle[t->nalle++] = w;
    w->art = a;
    return w;
}

static int gleich(const OWert *a, const OWert *b)
{
    if (a->art != b->art) return 0;
    switch (a->art) {
        case O_INT: case O_TIME: return a->i == b->i;
        case O_FLOAT: return a->f == b->f;
        case O_STR: case O_DATA: case O_UUID: return a->n == b->n && !memcmp(a->p, b->p, a->n);
        default: return 0;
    }
}

/* in die Verweistabelle: Reihenfolge des ersten Auftretens, ohne Doppelte */
static void merken(OBaum *t, OWert *w)
{
    int i;
    for (i = 0; i < t->ntab; i++) if (gleich(t->tab[i], w)) return;
    t->tab = mehr(t->tab, &t->captab, t->ntab, sizeof(OWert *));
    t->tab[t->ntab++] = w;
}

static unsigned long long le(OBaum *t, int n)
{
    unsigned long long v = 0;
    int i;
    if (t->e - t->b < n) { t->fehler = 1; return 0; }
    for (i = 0; i < n; i++) v |= (unsigned long long)t->b[i] << (8 * i);
    t->b += n;
    return v;
}

static OWert *wert(OBaum *t);

static OWert *bytes(OBaum *t, OArt a, unsigned long long n)
{
    OWert *w;
    if ((unsigned long long)(t->e - t->b) < n) { t->fehler = 1; return NULL; }
    w = neu(t, a);
    w->n = (int)n;
    w->p = malloc(n + 1);
    memcpy(w->p, t->b, n);
    w->p[n] = 0;
    t->b += n;
    merken(t, w);
    return w;
}

static OWert *behaelter(OBaum *t, OArt a, int anzahl)
{
    OWert *w = neu(t, a);
    int cap = 0, je = a == O_DICT ? 2 : 1;
    for (;;) {
        if (anzahl >= 0 && w->m >= anzahl) break;
        if (anzahl < 0) {
            if (t->b >= t->e) { t->fehler = 1; break; }
            if (*t->b == 0x03) { t->b++; break; }
        }
        w->k = mehr(w->k, &cap, (w->m + 1) * je, sizeof(OWert *));
        w->k[w->m * je] = wert(t);
        if (je == 2) w->k[w->m * je + 1] = wert(t);
        if (t->fehler) break;
        w->m++;
    }
    return w;
}

static OWert *wert(OBaum *t)
{
    unsigned c;
    OWert *w;
    if (t->fehler || t->b >= t->e) { t->fehler = 1; return NULL; }
    c = *t->b++;
    if (c == 0x01 || c == 0x02) { w = neu(t, O_BOOL); w->i = c == 0x01; return w; }
    if (c == 0x04) return neu(t, O_NULL);
    if (c == 0x05) return bytes(t, O_UUID, 16);
    if (c == 0x06) { w = neu(t, O_TIME); w->i = (long long)le(t, 8); merken(t, w); return w; }
    if (c >= 0x08 && c <= 0x2F) { w = neu(t, O_INT); w->i = c - 8; return w; }
    if (c >= 0x30 && c <= 0x33) {
        int n = 1 << (c - 0x30);
        unsigned long long u = le(t, n);
        w = neu(t, O_INT);
        if (n == 1) w->i = (signed char)u;
        else if (n == 2) w->i = (short)u;
        else if (n == 4) w->i = (int)(unsigned)u;
        else w->i = (long long)u;
        merken(t, w);
        return w;
    }
    if (c == 0x35) {
        union { unsigned u; float f; } x;
        x.u = (unsigned)le(t, 4);
        w = neu(t, O_FLOAT); w->f = x.f; merken(t, w); return w;
    }
    if (c == 0x36) {
        union { unsigned long long u; double f; } x;
        x.u = le(t, 8);
        w = neu(t, O_FLOAT); w->f = x.f; merken(t, w); return w;
    }
    if (c >= 0x40 && c <= 0x60) return bytes(t, O_STR, c - 0x40);
    if (c >= 0x61 && c <= 0x64) return bytes(t, O_STR, le(t, c - 0x60));
    if (c >= 0x70 && c <= 0x90) return bytes(t, O_DATA, c - 0x70);
    if (c >= 0x91 && c <= 0x94) { static const int lg[4] = { 1, 2, 4, 8 }; return bytes(t, O_DATA, le(t, lg[c - 0x91])); }
    if (c >= 0xA0 && c <= 0xC4) {
        unsigned long long idx = c <= 0xC0 ? c - 0xA0 : le(t, c - 0xC0);
        if (idx >= (unsigned long long)t->ntab) { t->fehler = 1; return NULL; }
        return t->tab[idx];
    }
    if (c >= 0xD0 && c <= 0xDF) return behaelter(t, O_ARRAY, c == 0xDF ? -1 : (int)(c - 0xD0));
    if (c >= 0xE0 && c <= 0xEF) return behaelter(t, O_DICT, c == 0xEF ? -1 : (int)(c - 0xE0));
    t->fehler = 1;
    return NULL;
}

OBaum *opack_lesen(const unsigned char *b, int n, OWert **wurzel)
{
    OBaum *t = calloc(1, sizeof(OBaum));
    t->b = b; t->e = b + n;
    *wurzel = wert(t);
    if (t->fehler) *wurzel = NULL;
    return t;
}

void opack_frei(OBaum *t)
{
    int i;
    if (!t) return;
    for (i = 0; i < t->nalle; i++) { free(t->alle[i]->p); free(t->alle[i]->k); free(t->alle[i]); }
    free(t->alle); free(t->tab); free(t);
}

OWert *opack_hole(const OWert *d, const char *s)
{
    int i;
    size_t n = strlen(s);
    if (!d || d->art != O_DICT) return NULL;
    for (i = 0; i < d->m; i++) {
        OWert *k = d->k[2 * i];
        if (k && k->art == O_STR && (size_t)k->n == n && !memcmp(k->p, s, n)) return d->k[2 * i + 1];
    }
    return NULL;
}

const char *opack_text(const OWert *w) { return w && w->art == O_STR ? (const char *)w->p : NULL; }

long long opack_zahl(const OWert *w, long long std)
{
    if (!w) return std;
    if (w->art == O_INT || w->art == O_BOOL) return w->i;
    if (w->art == O_FLOAT) return (long long)w->f;
    return std;
}

double opack_kommazahl(const OWert *w, double std)
{
    if (!w) return std;
    if (w->art == O_FLOAT) return w->f;
    if (w->art == O_INT) return (double)w->i;
    return std;
}

const unsigned char *opack_daten(const OWert *w, int *n)
{
    if (!w || (w->art != O_DATA && w->art != O_UUID)) return NULL;
    *n = w->n;
    return w->p;
}

/* --- Schreiben ----------------------------------------------------- */
void op_init(OPuffer *o) { o->b = NULL; o->n = o->cap = 0; }
void op_frei(OPuffer *o) { free(o->b); op_init(o); }

void op_roh(OPuffer *o, const unsigned char *d, int n)
{
    if (o->n + n > o->cap) {
        o->cap = o->cap ? o->cap * 2 : 256;
        while (o->cap < o->n + n) o->cap *= 2;
        o->b = realloc(o->b, o->cap);
    }
    memcpy(o->b + o->n, d, n);
    o->n += n;
}

static void byte(OPuffer *o, unsigned c) { unsigned char b = (unsigned char)c; op_roh(o, &b, 1); }

static void schreib_le(OPuffer *o, unsigned long long v, int n)
{
    int i;
    for (i = 0; i < n; i++) byte(o, (unsigned)(v >> (8 * i)) & 0xFF);
}

void op_dict(OPuffer *o, int n) { byte(o, n < 15 ? 0xE0 + n : 0xEF); }
void op_array(OPuffer *o, int n) { byte(o, n < 15 ? 0xD0 + n : 0xDF); }
void op_ende(OPuffer *o) { byte(o, 0x03); }
void op_null(OPuffer *o) { byte(o, 0x04); }
void op_bool(OPuffer *o, int v) { byte(o, v ? 0x01 : 0x02); }

void op_int(OPuffer *o, long long v)
{
    if (v >= 0 && v <= 0x27) byte(o, 0x08 + (unsigned)v);
    else if (v >= -128 && v <= 127) { byte(o, 0x30); schreib_le(o, (unsigned long long)v, 1); }
    else if (v >= -32768 && v <= 32767) { byte(o, 0x31); schreib_le(o, (unsigned long long)v, 2); }
    else if (v >= -2147483648LL && v <= 2147483647LL) { byte(o, 0x32); schreib_le(o, (unsigned long long)v, 4); }
    else { byte(o, 0x33); schreib_le(o, (unsigned long long)v, 8); }
}

void op_double(OPuffer *o, double v)
{
    union { unsigned long long u; double f; } x;
    x.f = v;
    byte(o, 0x36);
    schreib_le(o, x.u, 8);
}

void op_str(OPuffer *o, const char *s)
{
    int n = (int)strlen(s);
    if (n <= 0x20) byte(o, 0x40 + n);
    else if (n <= 0xFF) { byte(o, 0x61); schreib_le(o, n, 1); }
    else if (n <= 0xFFFF) { byte(o, 0x62); schreib_le(o, n, 2); }
    else { byte(o, 0x64); schreib_le(o, n, 4); }
    op_roh(o, (const unsigned char *)s, n);
}

void op_data(OPuffer *o, const unsigned char *d, int n)
{
    if (n <= 0x20) byte(o, 0x70 + n);
    else if (n <= 0xFF) { byte(o, 0x91); schreib_le(o, n, 1); }
    else if (n <= 0xFFFF) { byte(o, 0x92); schreib_le(o, n, 2); }
    else { byte(o, 0x93); schreib_le(o, n, 4); }
    op_roh(o, d, n);
}
