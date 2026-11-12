/* Ton der Bildschirmsynchronisierung: Pakete vom Dienst ueber fd 3, AAC-ELD, CoreAudio, im Takt mit dem Bild */
#ifndef TON_H
#define TON_H

void ton_start(void);                          /* nach dek_an; ohne fd 3 bleibt es still */
void ton_bild(double sender, double gezeigt);  /* je gezeigtem Bild: Rahmenzeit (relativ) und Zeit der Anzeige */
int ton_zeile(char *z, int n);                 /* Protokollzeile seit dem letzten Aufruf, 0 = nichts Neues */

#endif
