/* H.264 direkt mit dem FFmpeg aus PowerVLCs avcodec-Modul, ohne VLCs Uhr, Puffer und Bildverwerfen */
#ifndef DEKODER_H
#define DEKODER_H

typedef struct {
    const unsigned char *ebene[3];
    int zeile[3];
    int breite, hoehe, format;
} Dek_bild;

int dek_an(const char *modul, char *fehler, int nfehler);
unsigned char *dek_puffer(int n);             /* Speicher fuer einen Rahmen (mit FFmpeg-Polster) */
void dek_puffer_weg(unsigned char *p);        /* ungenutzt freigeben */
int dek_schicken(unsigned char *p, int n);    /* uebernimmt p; < 0 Fehler */
int dek_holen(Dek_bild *b);                   /* 1 Bild da, 0 keins, < 0 Fehler */
void dek_fertig(void);                        /* Bild aus dek_holen freigeben */
void dek_entlasten(int an);                   /* Glaettungsfilter fuer Nicht-Referenzbilder auslassen */
int dek_filter_aktiv(void);                   /* 1 = AltiVec-Glaettungsfilter eingesetzt */
void *dek_symbol(const char *name);           /* Funktion im PowerVLC-Modul (fuer Tests) */

/* Ton: AAC mit AudioSpecificConfig (AAC-ELD bei der Bildschirmsynchronisierung), Ausgabe Gleitkomma je Kanal */
typedef struct {
    const float *links, *rechts;
    int anzahl, format;
} Dek_ton;

int dek_ton_an(const unsigned char *asc, int n, int rate, int kanaele, char *fehler, int nfehler);   /* nach dek_an */
int dek_ton_schicken(unsigned char *p, int n);   /* p aus dek_puffer, wird uebernommen */
int dek_ton_holen(Dek_ton *t);                  /* 1 Rahmen da, 0 keiner, < 0 Fehler */
void dek_ton_fertig(void);

#endif
