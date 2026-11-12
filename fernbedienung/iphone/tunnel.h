/* Entwickler-Tunnel (CoreDeviceProxy): IPv6-Pakete ueber die Lockdown-Verbindung, kleines TCP im Programm */
#ifndef TUNNEL_H
#define TUNNEL_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int (*schreiben)(void *ctx, const void *b, size_t n);
    int (*lesen)(void *ctx, void *b, size_t n);   /* liest genau n Bytes */
    int (*bereit)(void *ctx, int ms);             /* 1 Daten da, 0 Zeit um, -1 Fehler */
    void *ctx;
} TunnelWeg;

typedef struct Tcp Tcp;

/* Handschlag; liefert Port der Dienstsuche (RSD) */
int tunnel_start(TunnelWeg *w, int *rsd_port, int laut);
const char *tunnel_ich(void);       /* eigene Adresse als Text */
const char *tunnel_geraet(void);    /* Adresse des iPhones als Text */
int tunnel_pumpen(int ms);          /* ein Paket verarbeiten; 1 ja, 0 nichts, -1 Fehler */
unsigned long tunnel_udp_bytes(void);
/* Bildstrom-Empfang: RTP an diesen Port wird mitgezaehlt (fuer Empfangsberichte) */
void tunnel_rtp_port(int port);
unsigned long tunnel_rtp_seq(void);        /* erweiterte hoechste Folgenummer */
unsigned long tunnel_rtp_pakete(void);
int tunnel_rtp_quelle(void);               /* Absenderport der RTP-Pakete */
int tunnel_udp_senden(int lport, int rport, const void *d, size_t n);

Tcp *tcp_auf(int port);
int tcp_schreiben(Tcp *c, const void *b, size_t n);
int tcp_lesen_voll(Tcp *c, void *b, size_t n, int ms);
size_t tcp_verfuegbar(Tcp *c);
size_t tcp_spaehen(Tcp *c, void *b, size_t n);
int tcp_offen(Tcp *c);
int tcp_zustand(Tcp *c);   /* 2 offen, 3 Gegenseite zu (FIN), 4 zurueckgesetzt (RST) */
void tcp_zu(Tcp *c);
double jetzt(void);
#endif
