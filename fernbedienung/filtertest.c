/* Prueft die AltiVec-Glaettungsfilter (filter.c) gegen die C-Fassungen in PowerVLCs FFmpeg:
   gleiche Zufallsdaten, zufaellige alpha/beta/tc0, jeder Bildpunkt muss gleich sein. Danach Zeitvergleich. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mach/mach_time.h>
#include "dekoder.h"

extern void lf_h_luma(uint8_t *, int, int, int, int8_t *);
extern void lf_v_luma_intra(uint8_t *, int, int, int);
extern void lf_h_luma_intra(uint8_t *, int, int, int);
extern void lf_h_chroma(uint8_t *, int, int, int, int8_t *);
extern void lf_h_chroma_intra(uint8_t *, int, int, int);
extern void lf_v_chroma_intra(uint8_t *, int, int, int);

typedef void (*f_norm)(uint8_t *, int, int, int, int8_t *);
typedef void (*f_intra)(uint8_t *, int, int, int);

#define ST 64
static unsigned char A[ST * 40] __attribute__((aligned(16))), B[ST * 40] __attribute__((aligned(16)));

static double uhr(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

/* Bildinhalt mit kleinen Spruengen, damit die Filter tatsaechlich greifen */
static void fuellen(void)
{
    int i, basis = rand() & 255, streu = 1 + (rand() % 40);
    for (i = 0; i < (int)sizeof A; i++) {
        int v = basis + (rand() % (2 * streu + 1)) - streu;
        if ((rand() & 15) == 0) v = rand() & 255;
        A[i] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    memcpy(B, A, sizeof A);
}

int main(void)
{
    char fehler[200];
    f_norm c_h;
    f_intra c_vi, c_hi;
    int n, fehl_h = 0, fehl_vi = 0, fehl_hi = 0;
    double t0, tc, ta;
    if (dek_an("/Applications/PowerVLC.app/Contents/MacOS/plugins/libavcodec_plugin.dylib", fehler, sizeof fehler)) {
        printf("dek_an: %s\n", fehler);
        return 1;
    }
    c_h = (f_norm)dek_symbol("_h264_h_loop_filter_luma_8_c");
    c_vi = (f_intra)dek_symbol("_h264_v_loop_filter_luma_intra_8_c");
    c_hi = (f_intra)dek_symbol("_h264_h_loop_filter_luma_intra_8_c");
    if (!c_h || !c_vi || !c_hi) { printf("C-Fassungen nicht gefunden\n"); return 1; }
    srand(12345);
    for (n = 0; n < 200000; n++) {
        int alpha = rand() % 256, beta = rand() % 64, x = 8 + 4 * (rand() % 10), y = 8 + (rand() % 8);
        int8_t tc0[4];
        int k;
        for (k = 0; k < 4; k++) tc0[k] = (int8_t)((rand() % 6) == 0 ? -1 : rand() % 26);
        if ((n & 7) == 0) { alpha = 255; beta = 255; }
        fuellen();
        /* senkrechte Kante, normaler Filter: pix an Spalte x (Vielfaches von 4), 16 Zeilen */
        c_h(A + y * ST + x, ST, alpha, beta, tc0);
        lf_h_luma(B + y * ST + x, ST, alpha, beta, tc0);
        if (memcmp(A, B, sizeof A)) { if (fehl_h++ < 3) printf("h_luma abweichend: alpha %d beta %d tc0 %d %d %d %d x %d\n", alpha, beta, tc0[0], tc0[1], tc0[2], tc0[3], x); }
        fuellen();
        /* waagerechte Kante, stark: 16 Spalten ab x (Vielfaches von 16), Zeile y >= 4 */
        c_vi(A + (y + 8) * ST + 16, ST, alpha, beta);
        lf_v_luma_intra(B + (y + 8) * ST + 16, ST, alpha, beta);
        if (memcmp(A, B, sizeof A)) { if (fehl_vi++ < 3) printf("v_luma_intra abweichend: alpha %d beta %d\n", alpha, beta); }
        fuellen();
        c_hi(A + y * ST + x, ST, alpha, beta);
        lf_h_luma_intra(B + y * ST + x, ST, alpha, beta);
        if (memcmp(A, B, sizeof A)) { if (fehl_hi++ < 3) printf("h_luma_intra abweichend: alpha %d beta %d x %d\n", alpha, beta, x); }
    }
    printf("200000 Faelle: Abweichungen h_luma %d, v_luma_intra %d, h_luma_intra %d\n", fehl_h, fehl_vi, fehl_hi);
    {
        /* Chroma */
        f_norm c_hc = (f_norm)dek_symbol("_h264_h_loop_filter_chroma_8_c");
        f_intra c_hci = (f_intra)dek_symbol("_h264_h_loop_filter_chroma_intra_8_c");
        f_intra c_vci = (f_intra)dek_symbol("_h264_v_loop_filter_chroma_intra_8_c");
        int f_hc = 0, f_hci = 0, f_vci = 0;
        if (!c_hc || !c_hci || !c_vci) { printf("Chroma-C-Fassungen nicht gefunden\n"); return 1; }
        for (n = 0; n < 200000; n++) {
            int alpha = rand() % 256, beta = rand() % 64, x = 8 + 4 * (rand() % 10), y = 8 + (rand() % 8), x8 = 8 * (1 + rand() % 5);
            int8_t tc0[4];
            int k;
            for (k = 0; k < 4; k++) tc0[k] = (int8_t)((rand() % 5) == 0 ? -1 : rand() % 26);
            if ((n & 7) == 0) { alpha = 255; beta = 255; }
            fuellen();
            c_hc(A + y * ST + x, ST, alpha, beta, tc0);
            lf_h_chroma(B + y * ST + x, ST, alpha, beta, tc0);
            if (memcmp(A, B, sizeof A)) { if (f_hc++ < 3) printf("h_chroma abweichend: alpha %d beta %d tc0 %d %d %d %d\n", alpha, beta, tc0[0], tc0[1], tc0[2], tc0[3]); }
            fuellen();
            c_hci(A + y * ST + x, ST, alpha, beta);
            lf_h_chroma_intra(B + y * ST + x, ST, alpha, beta);
            if (memcmp(A, B, sizeof A)) { if (f_hci++ < 3) printf("h_chroma_intra abweichend: alpha %d beta %d\n", alpha, beta); }
            fuellen();
            c_vci(A + (y + 8) * ST + x8, ST, alpha, beta);
            lf_v_chroma_intra(B + (y + 8) * ST + x8, ST, alpha, beta);
            if (memcmp(A, B, sizeof A)) { if (f_vci++ < 3) printf("v_chroma_intra abweichend: alpha %d beta %d x %d\n", alpha, beta, x8); }
        }
        printf("200000 Faelle: Abweichungen h_chroma %d, h_chroma_intra %d, v_chroma_intra %d\n", f_hc, f_hci, f_vci);
        {
            int8_t tc0[4] = { 2, 3, 4, 5 };
            fuellen();
            t0 = uhr(); for (n = 0; n < 200000; n++) c_hc(A + 8 * ST + 16, ST, 40, 10, tc0); tc = uhr() - t0;
            t0 = uhr(); for (n = 0; n < 200000; n++) lf_h_chroma(B + 8 * ST + 16, ST, 40, 10, tc0); ta = uhr() - t0;
            printf("h_chroma:       C %.2f us, AltiVec %.2f us je Aufruf\n", tc * 5, ta * 5);
            t0 = uhr(); for (n = 0; n < 200000; n++) c_hci(A + 8 * ST + 16, ST, 40, 10); tc = uhr() - t0;
            t0 = uhr(); for (n = 0; n < 200000; n++) lf_h_chroma_intra(B + 8 * ST + 16, ST, 40, 10); ta = uhr() - t0;
            printf("h_chroma_intra: C %.2f us, AltiVec %.2f us je Aufruf\n", tc * 5, ta * 5);
            t0 = uhr(); for (n = 0; n < 200000; n++) c_vci(A + 16 * ST + 16, ST, 40, 10); tc = uhr() - t0;
            t0 = uhr(); for (n = 0; n < 200000; n++) lf_v_chroma_intra(B + 16 * ST + 16, ST, 40, 10); ta = uhr() - t0;
            printf("v_chroma_intra: C %.2f us, AltiVec %.2f us je Aufruf\n", tc * 5, ta * 5);
        }
    }
    /* Zeit: je 200000 Aufrufe */
    {
        int8_t tc0[4] = { 3, 5, 7, 9 };
        fuellen();
        t0 = uhr(); for (n = 0; n < 200000; n++) c_h(A + 8 * ST + 16, ST, 40, 10, tc0); tc = uhr() - t0;
        t0 = uhr(); for (n = 0; n < 200000; n++) lf_h_luma(B + 8 * ST + 16, ST, 40, 10, tc0); ta = uhr() - t0;
        printf("h_luma:       C %.2f us, AltiVec %.2f us je Aufruf\n", tc * 5, ta * 5);
        t0 = uhr(); for (n = 0; n < 200000; n++) c_vi(A + 16 * ST + 16, ST, 40, 10); tc = uhr() - t0;
        t0 = uhr(); for (n = 0; n < 200000; n++) lf_v_luma_intra(B + 16 * ST + 16, ST, 40, 10); ta = uhr() - t0;
        printf("v_luma_intra: C %.2f us, AltiVec %.2f us je Aufruf\n", tc * 5, ta * 5);
        t0 = uhr(); for (n = 0; n < 200000; n++) c_hi(A + 8 * ST + 16, ST, 40, 10); tc = uhr() - t0;
        t0 = uhr(); for (n = 0; n < 200000; n++) lf_h_luma_intra(B + 8 * ST + 16, ST, 40, 10); ta = uhr() - t0;
        printf("h_luma_intra: C %.2f us, AltiVec %.2f us je Aufruf\n", tc * 5, ta * 5);
    }
    return 0;
}
