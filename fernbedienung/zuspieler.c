/* Testzuspieler fuer das Spiegel-Fenster: H.264-Datei (Annex B) rahmenweise im Format des Empfangsdienstes
   ([8 Byte Zeit in us][4 Byte Laenge][Daten], Big-Endian) mit fester Bildrate auf die Standardausgabe.
   Aufruf: zuspieler datei.h264 bilder_pro_s durchlaeufe */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach_time.h>

static double uhr(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

static long start(const unsigned char *d, long n, long i)
{
    for (; i + 3 <= n; i++) if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) return i;
    return n;
}

int main(int argc, char **argv)
{
    FILE *f;
    unsigned char *d, k[12];
    long n, nr = 0;
    double fps, t0;
    int runden, r;
    if (argc < 4 || !(f = fopen(argv[1], "rb"))) { fprintf(stderr, "zuspieler datei fps runden\n"); return 1; }
    fps = atof(argv[2]);
    runden = atoi(argv[3]);
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    d = malloc(n); fread(d, 1, n, f); fclose(f);
    t0 = uhr();
    for (r = 0; r < runden; r++) {
        /* jede Runde beginnt mit dem Schluesselbild am Dateianfang, der Dekoder faengt also sauber neu an */
        long i = start(d, n, 0), anf = 0;
        while (i < n) {
            long j = start(d, n, i + 3);
            int typ = d[i + 3] & 0x1f;
            if (typ == 1 || typ == 5) {
                unsigned long long us = 1000000ULL + (unsigned long long)(nr * 1e6 / fps);
                unsigned len = (unsigned)(j - anf);
                double ziel = t0 + nr / fps;
                int b;
                while (uhr() < ziel) usleep(1000);
                for (b = 0; b < 8; b++) k[b] = (unsigned char)(us >> (56 - 8 * b));
                k[8] = len >> 24; k[9] = len >> 16; k[10] = len >> 8; k[11] = len;
                if (fwrite(k, 1, 12, stdout) != 12 || fwrite(d + anf, 1, len, stdout) != len) return 0;
                fflush(stdout);
                nr++;
                anf = j;
            }
            i = j;
        }
    }
    return 0;
}
