/* iPhone-Tastatur (RTI, Remote Text Input) */
#ifndef RTI_H
#define RTI_H

typedef struct {
    int zusichern;              /* textToAssert vorhanden: Feld auf zusicherung setzen */
    int loeschen;               /* Zeichen rueckwaerts loeschen */
    int hat_uuid;
    int von_tastatur;           /* echte Tastatureingabe (inputSourceState); sonst Ganztext-Abgleich */
    char zusicherung[2048];
    char einfuegen[2048];
    unsigned char uuid[16];
} RtiEingabe;

/* Archiv fuer _tiStart/_tiStarted; malloc-Puffer in *aus, Laenge zurueck (0: Fehler) */
int rti_archiv(unsigned char **aus, const unsigned char uuid[16], const char *titel,
               const char *vor, const char *auswahl, const char *nach);

/* _tiD aus _tiC lesen; roh: lesbare Fassung fuers Protokoll (darf NULL sein). 0: gut */
int rti_lesen(const unsigned char *d, int n, RtiEingabe *e, char *roh, int rmax);

#endif
