/* Prueft den Tonweg am Mitschnitt /tmp/ton_proben.bin (Saetze wie vom Dienst: k Schluessel und IV, f Format,
   a RTP-Paket): entschluesseln, dekodieren mit PowerVLCs FFmpeg, FFmpeg-Meldungen sichtbar.
   Mitschnitt: /tmp/ton_proben anlegen, dann verbinden (das Fenster schreibt 600 Saetze).
   Aufruf: tontest [ASC hex, Vorgabe f8e85000 = AAC-ELD 44,1 kHz Stereo 480] [Anzahl Pakete] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <openssl/aes.h>
#include <mach/mach_time.h>
#include "dekoder.h"

static double jetzt(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

#define MODUL "/Applications/PowerVLC.app/Contents/MacOS/plugins/libavcodec_plugin.dylib"

int main(int argc, char **argv)
{
    const char *asc_text = argc > 1 ? argv[1] : "f8e85000";
    int anzahl = argc > 2 ? atoi(argv[2]) : 300, n, i, ok = 0, fehler = 0, nasc = 0;
    unsigned char asc[16], d[600], kopf[3], schl[32], iv[16];
    char f[200];
    AES_KEY k;
    unsigned last = 0;
    double summe = 0, zeit = 0, zeit_max = 0;
    int gross = 0;
    FILE *ein = fopen("/tmp/ton_proben.bin", "rb");
    void (*log_level)(int);
    if (!ein) { printf("kein Mitschnitt\n"); return 1; }
    for (i = 0; asc_text[2 * i] && i < 16; i++) { unsigned v; sscanf(asc_text + 2 * i, "%2x", &v); asc[i] = (unsigned char)v; nasc++; }
    if (dek_an(MODUL, f, sizeof f)) { printf("dek_an: %s\n", f); return 1; }
    log_level = (void (*)(int))dek_symbol("_av_log_set_level");
    if (log_level) log_level(24);   /* AV_LOG_WARNING */
    if (dek_ton_an(asc, nasc, 44100, 2, f, sizeof f)) { printf("dek_ton_an: %s\n", f); return 1; }
    memset(schl, 0, sizeof schl);
    while (fread(kopf, 1, 3, ein) == 3) {
        n = kopf[0] << 8 | kopf[1];
        if (n > (int)sizeof d || fread(d, 1, (size_t)n, ein) != (size_t)n) break;
        if (kopf[2] == 'k' && n == 32) {
            memcpy(schl, d, 32);
            AES_set_decrypt_key(schl, 128, &k);
        } else if (kopf[2] == 'f') {
            d[n] = 0;
            printf("Format %s\n", (char *)d);
        } else if (kopf[2] == 'a' && n > 12 && anzahl > 0) {
            int nutz = n - 12, r;
            unsigned char *b = dek_puffer(nutz), *o = dek_puffer(nutz);
            unsigned ts = (unsigned)d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7];
            double t0 = jetzt();
            Dek_ton t;
            if (ts == last) { dek_puffer_weg(b); dek_puffer_weg(o); continue; }
            last = ts;
            anzahl--;
            memcpy(b, d + 12, (size_t)nutz);
            memcpy(iv, schl + 16, 16);
            AES_cbc_encrypt(b, o, (unsigned long)(nutz / 16 * 16), &k, iv, AES_DECRYPT);
            memcpy(b, o, (size_t)(nutz / 16 * 16));
            dek_puffer_weg(o);
            r = dek_ton_schicken(b, nutz);
            if (r >= 0) while ((r = dek_ton_holen(&t)) == 1) {
                for (i = 0; i < t.anzahl; i++) summe += t.links[i] * t.links[i];
                ok++;
                dek_ton_fertig();
            }
            if (r < 0) { fehler++; if (fehler <= 3) printf("Fehler %d bei ts %u (%d Byte)\n", r, ts, nutz); }
            if (nutz > 16) {
                double dt = jetzt() - t0;
                zeit += dt;
                if (dt > zeit_max) zeit_max = dt;
                gross++;
            }
        }
    }
    printf("ASC %s: %d Rahmen, %d Fehler, Pegel (RMS links) %.4f\n", asc_text, ok, fehler, ok ? sqrt(summe / (ok * 480.0)) : 0.0);
    if (gross) printf("Entschluesseln + Dekodieren je Paket mit Ton: %.2f ms (max %.2f), bei 92 Paketen/s %.1f %% Prozessor\n",
                      zeit / gross * 1000, zeit_max * 1000, zeit / gross * 92 * 100);
    return 0;
}
