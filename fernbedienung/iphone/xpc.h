/* XPC-Werte im RemoteXPC-Format (Little-Endian) und RemoteXPC-Verbindungen ueber HTTP/2 */
#ifndef XPC_H
#define XPC_H
#include <stddef.h>
#include <stdint.h>
#include "tunnel.h"

enum {
    XW_NULL = 0x1000, XW_BOOL = 0x2000, XW_INT = 0x3000, XW_UINT = 0x4000, XW_DOUBLE = 0x5000,
    XW_DATE = 0x7000, XW_DATA = 0x8000, XW_STRING = 0x9000, XW_UUID = 0xA000, XW_FD = 0xB000,
    XW_SHMEM = 0xC000, XW_ARRAY = 0xE000, XW_DICT = 0xF000, XW_DATEI = 0x1A000
};

typedef struct Xw Xw;
struct Xw {
    uint32_t typ;
    long long i;            /* Bool, Zahlen, Datum, Dateiuebertragung: Groesse */
    unsigned long long id;  /* Dateiuebertragung: Kennung */
    double d;
    char *s;
    unsigned char *b;
    size_t n;               /* Laenge von b, Anzahl der Kinder */
    Xw **kind;
    char **schl;
};

typedef struct { unsigned char *p; size_t n, cap; } Puffer;
void pf_rein(Puffer *p, const void *d, size_t n);
void pf_frei(Puffer *p);

Xw *xw_dict(void);
Xw *xw_array(void);
Xw *xw_str(const char *s);
Xw *xw_daten(const void *b, size_t n);
Xw *xw_int(long long v);
Xw *xw_uint(unsigned long long v);
Xw *xw_bool(int v);
Xw *xw_uuid(const unsigned char u[16]);
Xw *xw_null(void);
Xw *xw_datei(unsigned long long id, unsigned long long groesse);
Xw *xw_setzen(Xw *dict, const char *schl, Xw *wert);   /* gibt dict zurueck */
Xw *xw_anhaengen(Xw *arr, Xw *wert);
Xw *xw_holen(const Xw *dict, const char *schl);
const char *xw_text(const Xw *dict, const char *schl);
long long xw_zahl(const Xw *dict, const char *schl, long long std);
void xw_frei(Xw *w);
void xw_zeigen(const Xw *w, int tiefe);
void xw_kodieren(const Xw *w, Puffer *p);
Xw *xw_dekodieren(const unsigned char *p, size_t n, size_t *pos);

/* RemoteXPC-Verbindung */
#define RX_STROEME 64
typedef struct {
    Tcp *t;
    unsigned long long naechste_id;
    long long fenster_aus, peer_start;
    long long strom_fenster[RX_STROEME];
    long long offen_ein[RX_STROEME];
    Puffer ein[RX_STROEME];
    int naechster_strom;
    int laut;
    int letzter_typ, letzte_flags;
    int behalten;   /* grosse Antworten nicht verwerfen (Zwischenablage) */
} Rx;

int rx_auf(Rx *r, Tcp *t);
int rx_senden(Rx *r, Xw *d, int antwort);
Xw *rx_empfangen(Rx *r, int ms);
int rx_datei(Rx *r, unsigned long long kennung, const unsigned char *b, size_t n);
int rx_pflegen(Rx *r);
Xw *rx_abholen(Rx *r);   /* fertige Nachricht aus dem schon Empfangenen, ohne zu warten */
void rx_zu(Rx *r);
#endif
