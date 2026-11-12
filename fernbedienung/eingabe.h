#ifndef EINGABE_H
#define EINGABE_H
#include <CoreFoundation/CoreFoundation.h>

enum { E_HOCH, E_RUNTER, E_LINKS, E_RECHTS, E_WAHL, E_ZURUECK, E_PAUSE, E_WECHSEL };

void eingabe_taste(int art);
void eingabe_text(const UniChar *text, int n);
void eingabe_loeschen(int n);
float eingabe_lautstaerke(void);
void eingabe_setze_lautstaerke(float v);
void eingabe_lauter(int richtung);
void eingabe_stumm(int an);
int eingabe_ist_stumm(void);
void eingabe_bildschirm(int an);
void eingabe_zeiger_lage(double *x, double *y);
void eingabe_zeiger(double *x, double *y);
void eingabe_klick(int rechts);
void eingabe_rollen(int zeilen);
void eingabe_ton(int welcher);
void eingabe_text_utf8(const char *s);
void eingabe_alles_markieren(void);
#include <ApplicationServices/ApplicationServices.h>
extern int eingabe_ax_fehler;
extern char eingabe_ax_rolle[64];
int eingabe_textfeld(AXUIElementRef *el, char *titel, int tmax, char *vor, int vmax, char *ausw, int amax, char *nach, int nmax);

#endif
