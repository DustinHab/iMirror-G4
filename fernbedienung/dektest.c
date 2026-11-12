/* Messprogramm fuer den eigenen Bildweg: H.264-Datei (Annex B) rahmenweise dekodieren und umrechnen.
   Aufruf: dektest datei.h264 [filter]   filter: default | nonref | all (skip_loop_filter, nur zum Messen) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach_time.h>
#include "dekoder.h"
#include "uyvy.h"

static double uhr(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

/* naechster Startcode ab i, liefert Position des 00 00 01 oder n */
static long start(const unsigned char *d, long n, long i)
{
    for (; i + 3 <= n; i++) if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) return i;
    return n;
}

int main(int argc, char **argv)
{
    FILE *f;
    unsigned char *d, *aus = NULL;
    long n, i, rahmen_anf = 0;
    char fehler[200];
    int bilder = 0, rahmen = 0, form = -1, w = 0, h = 0, runde, runden = 1;
    unsigned summe = 2166136261u;
    double t0, dek = 0, dek_max = 0, umr = 0;
    if (argc < 2 || !(f = fopen(argv[1], "rb"))) { fprintf(stderr, "Datei?\n"); return 1; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    d = malloc(n); fread(d, 1, n, f); fclose(f);
    if (dek_an("/Applications/PowerVLC.app/Contents/MacOS/plugins/libavcodec_plugin.dylib", fehler, sizeof fehler)) {
        fprintf(stderr, "dek_an: %s\n", fehler);
        return 1;
    }
    /* zweites Argument: Anzahl Durchlaeufe der Datei (genauere Zeitmessung) */
    if (argc > 2) runden = atoi(argv[2]);
    for (runde = 0; runde < runden; runde++) {
    /* Rahmen = alles bis einschliesslich eines Bildausschnitts (NAL 1 oder 5) */
    rahmen_anf = 0;
    i = start(d, n, 0);
    while (i < n) {
        long j = start(d, n, i + 3);
        int typ = d[i + 3] & 0x1f;
        if (typ == 1 || typ == 5) {
            int len = (int)(j - rahmen_anf), r;
            unsigned char *p = dek_puffer(len);
            Dek_bild b;
            double a = uhr(), e;
            memcpy(p, d + rahmen_anf, len);
            if (dek_schicken(p, len) < 0) fprintf(stderr, "schicken Fehler bei Rahmen %d\n", rahmen);
            rahmen++;
            while ((r = dek_holen(&b)) == 1) {
                e = uhr();
                dek += e - a;
                if (e - a > dek_max) dek_max = e - a;
                if (!aus) {
                    w = b.breite; h = b.hoehe; form = b.format;
                    aus = valloc(((w + 31) & ~31) * 2 * h);
                    printf("Bild %dx%d, Format %d, Zeilen %d/%d/%d\n", w, h, form, b.zeile[0], b.zeile[1], b.zeile[2]);
                }
                zu_uyvy(b.ebene[0], b.zeile[0], b.ebene[1], b.zeile[1], b.ebene[2], b.zeile[2], aus, ((w + 31) & ~31) * 2,
                        w & ~1, h & ~1, form == 12);
                umr += uhr() - e;
                {
                    /* Pruefsumme ueber alle sichtbaren Bildpunkte (FNV-1a), zum Vergleich zweier Dekoder */
                    int p, zz;
                    for (p = 0; p < 3; p++) {
                        int pw = p ? w / 2 : w, ph = p ? h / 2 : h;
                        for (zz = 0; zz < ph; zz++) {
                            const unsigned char *q = b.ebene[p] + (long)zz * b.zeile[p];
                            int s;
                            for (s = 0; s < pw; s++) { summe ^= q[s]; summe *= 16777619u; }
                        }
                    }
                }
                dek_fertig();
                bilder++;
                a = uhr();
            }
            if (r < 0) fprintf(stderr, "holen Fehler %d bei Rahmen %d\n", r, rahmen);
            rahmen_anf = j;
        }
        i = j;
    }
    }
    t0 = dek + umr;
    printf("Filtertausch %d; %d Rahmen, %d Bilder; Dekodieren %.2f ms je Bild (max %.1f), Umrechnen %.2f ms; zusammen %.1f Bilder/s moeglich; "
           "Pruefsumme %08x\n", dek_filter_aktiv(), rahmen, bilder, bilder ? dek * 1000 / bilder : 0, dek_max * 1000, bilder ? umr * 1000 / bilder : 0,
           t0 > 0 ? bilder / t0 : 0, summe);
    return 0;
}
