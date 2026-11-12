/* OPACK (Apple Companion): Lesen in einen Baum, Schreiben ueber einen Puffer.
   Alle Mehrbyte-Werte little-endian, Ganzzahlen vorzeichenbehaftet (wie Apple). */
#ifndef OPACK_H
#define OPACK_H

typedef enum { O_NULL, O_BOOL, O_INT, O_FLOAT, O_STR, O_DATA, O_UUID, O_TIME, O_ARRAY, O_DICT } OArt;

typedef struct OWert {
    OArt art;
    long long i;                 /* O_INT, O_BOOL, O_TIME */
    double f;                    /* O_FLOAT */
    unsigned char *p;            /* O_STR (mit NUL), O_DATA, O_UUID */
    int n;                       /* Laenge von p */
    struct OWert **k;            /* O_ARRAY: m Elemente; O_DICT: 2*m (Schluessel, Wert) */
    int m;
} OWert;

typedef struct OBaum OBaum;      /* haelt alle Knoten eines Lesevorgangs */

OBaum *opack_lesen(const unsigned char *b, int n, OWert **wurzel);
void opack_frei(OBaum *t);
OWert *opack_hole(const OWert *dict, const char *schluessel);
const char *opack_text(const OWert *w);
long long opack_zahl(const OWert *w, long long std);
double opack_kommazahl(const OWert *w, double std);
const unsigned char *opack_daten(const OWert *w, int *n);

typedef struct { unsigned char *b; int n, cap; } OPuffer;
void op_init(OPuffer *o);
void op_frei(OPuffer *o);
void op_dict(OPuffer *o, int n);         /* n Paare folgen; n >= 15: mit op_ende abschliessen */
void op_array(OPuffer *o, int n);
void op_ende(OPuffer *o);
void op_str(OPuffer *o, const char *s);
void op_data(OPuffer *o, const unsigned char *d, int n);
void op_int(OPuffer *o, long long v);
void op_double(OPuffer *o, double v);
void op_bool(OPuffer *o, int v);
void op_null(OPuffer *o);
void op_roh(OPuffer *o, const unsigned char *d, int n);

#endif
