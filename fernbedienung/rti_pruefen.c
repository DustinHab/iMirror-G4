/* Prueft rti.m: Archiv bauen (Aufbau als XML zeigen) und pyatv-Eingabepakete lesen. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rti.h"
#include "rti_vektoren.h"

static int hex(const char *h, unsigned char *o)
{
    int n = (int)strlen(h) / 2, i;
    for (i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); o[i] = (unsigned char)v; }
    return n;
}

int main(void)
{
    unsigned char uuid[16], *a = NULL, b[2048];
    RtiEingabe e;
    char roh[2000];
    int n, fehler = 0;
    FILE *f;
    memcpy(uuid, "0123456789abcdef", 16);

    n = rti_archiv(&a, uuid, "Suchen", "Hallo", "", " Welt");
    printf("Archiv: %d Bytes\n", n);
    f = fopen("/tmp/rti_archiv.plist", "wb");
    fwrite(a, 1, n, f);
    fclose(f);
    free(a);

    n = hex(TIPPEN, b);
    if (rti_lesen(b, n, &e, roh, sizeof roh)) { printf("TIPPEN nicht lesbar\n"); fehler++; }
    else {
        int ok = !e.zusichern && !strcmp(e.einfuegen, "Hallo Welt") && e.hat_uuid && !memcmp(e.uuid, uuid, 16);
        printf("%-28s %s (einfuegen '%s')\n", "Eingabe tippen", ok ? "richtig" : "FALSCH", e.einfuegen);
        if (!ok) fehler++;
    }
    n = hex(LEEREN, b);
    if (rti_lesen(b, n, &e, roh, sizeof roh)) { printf("LEEREN nicht lesbar\n"); fehler++; }
    else {
        int ok = e.zusichern && !e.zusicherung[0] && !e.einfuegen[0] && e.hat_uuid;
        printf("%-28s %s\n", "Eingabe leeren", ok ? "richtig" : "FALSCH");
        if (!ok) fehler++;
    }
    printf("roh: %.300s\n", roh);
    printf("%s\n", fehler ? "FEHLER" : "ALLES RICHTIG");
    return fehler != 0;
}
