/* H.264-Glaettungsfilter (Deblocking, Luma, 8 Bit) mit AltiVec fuer den G4.
   PowerVLCs FFmpeg rechnet diese drei Faelle in einfachem C (rund 13 % der Dekodierzeit beim iPhone-Strom):
   - lf_h_luma:        senkrechte Kante, normaler Filter (bS < 4); uebernommen aus FFmpeg 4.4.5 libavcodec/ppc/h264dsp.c
                       (LGPL 2.1 oder spaeter, Autoren siehe dort)
   - lf_v_luma_intra:  waagerechte Kante, starker Filter (bS = 4), eigene Fassung
   - lf_h_luma_intra:  senkrechte Kante, starker Filter, eigene Fassung (16x8 transponieren)
   Alle drei rechnen bitgleich wie FFmpegs C-Fassung (geprueft mit Zufallsdaten in dektest). */
#include <stdint.h>
#include <string.h>

typedef vector unsigned char vu8;
typedef vector signed char vs8;
typedef vector unsigned short vu16;

static inline vu8 laden(int off, const uint8_t *p)
{
    vu8 a = vec_ld(off, p), b = vec_ld(off + 15, p);
    return vec_perm(a, b, vec_lvsl(off, p));
}

static inline vu8 diff_lt(vu8 x, vu8 y, vu8 a)
{
    vu8 d = vec_or(vec_subs(x, y), vec_subs(y, x));
    return (vu8)vec_cmplt(d, a);
}

/* ---------- normaler Filter (aus FFmpeg) ---------- */

static inline vu8 deblock_q1(vu8 p0, vu8 p1, vu8 p2, vu8 q0, vu8 tc0)
{
    vu8 average = vec_avg(p0, q0), temp, unclipped, ones, max, min, newp1;
    temp = vec_xor(average, p2);
    average = vec_avg(average, p2);
    ones = vec_splat_u8(1);
    temp = vec_and(temp, ones);
    unclipped = vec_subs(average, temp);
    max = vec_adds(p1, tc0);
    min = vec_subs(p1, tc0);
    newp1 = vec_max(min, unclipped);
    newp1 = vec_min(max, newp1);
    return newp1;
}

#define DEBLOCK_P0_Q0(p0, p1, q0, q1, tc0masked) {                     \
    vu8 vec160 = vec_sl(vec_splat_u8(10), vec_splat_u8(4));           \
    vu8 pq0bit = vec_xor(p0, q0), q1minus, p0minus, stage1, stage2;   \
    vu8 delta, deltaneg;                                              \
    q1minus = vec_nor(q1, q1);                                        \
    stage1 = vec_avg(p1, q1minus);                                    \
    stage2 = vec_sr(stage1, vec_splat_u8(1));                         \
    p0minus = vec_nor(p0, p0);                                        \
    stage1 = vec_avg(q0, p0minus);                                    \
    pq0bit = vec_and(pq0bit, vec_splat_u8(1));                        \
    stage2 = vec_avg(stage2, pq0bit);                                 \
    stage2 = vec_adds(stage2, stage1);                                \
    deltaneg = vec_subs(vec160, stage2);                              \
    delta = vec_subs(stage2, vec160);                                 \
    deltaneg = vec_min(tc0masked, deltaneg);                          \
    delta = vec_min(tc0masked, delta);                                \
    p0 = vec_subs(p0, deltaneg);                                      \
    q0 = vec_subs(q0, delta);                                         \
    p0 = vec_adds(p0, delta);                                         \
    q0 = vec_adds(q0, deltaneg);                                      \
}

void lf_h_luma(uint8_t *pix, int stride, int alpha, int beta, int8_t *tc0)
{
    unsigned char temp[16] __attribute__((aligned(16)));
    unsigned char erg[64] __attribute__((aligned(16)));
    const uint8_t *src = pix - 3;
    vu8 r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13, r14, r15;
    vu8 p2, p1, p0, q0, q1, q2, alphavec, betavec, mask, p1mask, q1mask, finaltc0, tc0masked, newp1, newq1;
    vs8 tc0vec;
    int i;
    if ((tc0[0] & tc0[1] & tc0[2] & tc0[3]) < 0) return;
    /* 16 Zeilen x 6 Spalten einlesen und transponieren */
    r0 = laden(0, src); r1 = laden(stride, src); r2 = laden(2 * stride, src); r3 = laden(3 * stride, src);
    r4 = laden(4 * stride, src); r5 = laden(5 * stride, src); r6 = laden(6 * stride, src); r7 = laden(7 * stride, src);
    r8 = laden(8 * stride, src); r9 = laden(9 * stride, src); r10 = laden(10 * stride, src); r11 = laden(11 * stride, src);
    r12 = laden(12 * stride, src); r13 = laden(13 * stride, src); r14 = laden(14 * stride, src); r15 = laden(15 * stride, src);
    r0 = vec_mergeh(r0, r8); r1 = vec_mergeh(r1, r9); r2 = vec_mergeh(r2, r10); r3 = vec_mergeh(r3, r11);
    r4 = vec_mergeh(r4, r12); r5 = vec_mergeh(r5, r13); r6 = vec_mergeh(r6, r14); r7 = vec_mergeh(r7, r15);
    r8 = vec_mergeh(r0, r4); r9 = vec_mergel(r0, r4); r10 = vec_mergeh(r1, r5); r11 = vec_mergel(r1, r5);
    r12 = vec_mergeh(r2, r6); r13 = vec_mergel(r2, r6); r14 = vec_mergeh(r3, r7); r15 = vec_mergel(r3, r7);
    r0 = vec_mergeh(r8, r12); r1 = vec_mergel(r8, r12); r2 = vec_mergeh(r9, r13);
    r4 = vec_mergeh(r10, r14); r5 = vec_mergel(r10, r14); r6 = vec_mergeh(r11, r15);
    p2 = vec_mergeh(r0, r4); p1 = vec_mergel(r0, r4); p0 = vec_mergeh(r1, r5);
    q0 = vec_mergel(r1, r5); q1 = vec_mergeh(r2, r6); q2 = vec_mergel(r2, r6);
    /* Filter */
    temp[0] = (unsigned char)alpha;
    temp[1] = (unsigned char)beta;
    alphavec = vec_ld(0, temp);
    betavec = vec_splat(alphavec, 1);
    alphavec = vec_splat(alphavec, 0);
    mask = vec_and(vec_and(diff_lt(p0, q0, alphavec), diff_lt(p1, p0, betavec)), diff_lt(q1, q0, betavec));
    memcpy(temp, tc0, 4);
    tc0vec = vec_ld(0, (signed char *)temp);
    tc0vec = vec_mergeh(tc0vec, tc0vec);
    tc0vec = vec_mergeh(tc0vec, tc0vec);
    mask = vec_and(mask, (vu8)vec_cmpgt(tc0vec, vec_splat_s8(-1)));
    finaltc0 = vec_and((vu8)tc0vec, mask);
    p1mask = vec_and(diff_lt(p2, p0, betavec), mask);
    tc0masked = vec_and(p1mask, (vu8)tc0vec);
    finaltc0 = vec_sub(finaltc0, p1mask);
    newp1 = deblock_q1(p0, p1, p2, q0, tc0masked);
    q1mask = vec_and(diff_lt(q2, q0, betavec), mask);
    tc0masked = vec_and(q1mask, (vu8)tc0vec);
    finaltc0 = vec_sub(finaltc0, q1mask);
    newq1 = deblock_q1(p0, q1, q2, q0, tc0masked);
    DEBLOCK_P0_Q0(p0, p1, q0, q1, finaltc0);
    p1 = newp1;
    q1 = newq1;
    /* 4 Spalten zurueck in 16 Zeilen */
    r4 = vec_mergeh(p1, q0); r5 = vec_mergel(p1, q0); r6 = vec_mergeh(p0, q1); r7 = vec_mergel(p0, q1);
    p1 = vec_mergeh(r4, r6); p0 = vec_mergel(r4, r6); q0 = vec_mergeh(r5, r7); q1 = vec_mergel(r5, r7);
    vec_st(p1, 0, erg); vec_st(p0, 16, erg); vec_st(q0, 32, erg); vec_st(q1, 48, erg);
    for (i = 0; i < 16; i++) *(uint32_t *)(pix - 2 + i * stride) = ((uint32_t *)erg)[i];
}

/* ---------- starker Filter (bS = 4), eigene Fassung ----------
   Rechnung in 16 Bit, je Haelfte 8 Spuren. S = p1+p0+q0, T = p0+q0+q1:
   p0a = (p2 + 2S + q1 + 4) >> 3    p1a = (p2 + S + 2) >> 2    p2a = (2p3 + 3p2 + S + 4) >> 3    p0b = (2p1 + p0 + q1 + 2) >> 2
   q0a = (p1 + 2T + q2 + 4) >> 3    q1a = (T + q2 + 2) >> 2    q2a = (2q3 + 3q2 + T + 4) >> 3    q0b = (2q1 + q0 + p1 + 2) >> 2 */

#define HAELFTE(m, P3, P2, P1, P0, Q0, Q1, Q2, Q3, o0, o1, o2, o3, o4, o5, o6, o7) {     \
    vu16 hz = vec_splat_u16(0);                                                           \
    vu16 hp3 = (vu16)m((vu8)hz, P3), hp2 = (vu16)m((vu8)hz, P2), hp1 = (vu16)m((vu8)hz, P1); \
    vu16 hp0 = (vu16)m((vu8)hz, P0), hq0 = (vu16)m((vu8)hz, Q0), hq1 = (vu16)m((vu8)hz, Q1); \
    vu16 hq2 = (vu16)m((vu8)hz, Q2), hq3 = (vu16)m((vu8)hz, Q3);                          \
    vu16 zwei = vec_splat_u16(2), vier = vec_splat_u16(4), eins = vec_splat_u16(1);       \
    vu16 drei = vec_splat_u16(3);                                                         \
    vu16 S = vec_add(vec_add(hp1, hp0), hq0), T = vec_add(vec_add(hp0, hq0), hq1);        \
    o0 = vec_sr(vec_add(vec_add(hp2, vec_sl(S, eins)), vec_add(hq1, vier)), drei);        \
    o1 = vec_sr(vec_add(vec_add(hp2, S), zwei), zwei);                                    \
    o2 = vec_sr(vec_add(vec_add(vec_sl(hp3, eins), vec_add(vec_sl(hp2, eins), hp2)),      \
                        vec_add(S, vier)), drei);                                         \
    o3 = vec_sr(vec_add(vec_add(vec_sl(hp1, eins), hp0), vec_add(hq1, zwei)), zwei);      \
    o4 = vec_sr(vec_add(vec_add(hp1, vec_sl(T, eins)), vec_add(hq2, vier)), drei);        \
    o5 = vec_sr(vec_add(vec_add(T, hq2), zwei), zwei);                                    \
    o6 = vec_sr(vec_add(vec_add(vec_sl(hq3, eins), vec_add(vec_sl(hq2, eins), hq2)),      \
                        vec_add(T, vier)), drei);                                         \
    o7 = vec_sr(vec_add(vec_add(vec_sl(hq1, eins), hq0), vec_add(hp1, zwei)), zwei);      \
}

/* p3..q3 werden ueberschrieben (p2..q2 neu) */
static inline void stark(vu8 p3, vu8 *p2, vu8 *p1, vu8 *p0, vu8 *q0, vu8 *q1, vu8 *q2, vu8 q3, int alpha, int beta)
{
    unsigned char temp[16] __attribute__((aligned(16)));
    vu8 av, bv, a2v, mask, strong, ap, aq;
    vu16 h0, h1, h2, h3, h4, h5, h6, h7, l0, l1, l2, l3, l4, l5, l6, l7;
    vu8 P0a, P1a, P2a, P0b, Q0a, Q1a, Q2a, Q0b;
    vu8 vp2 = *p2, vp1 = *p1, vp0 = *p0, vq0 = *q0, vq1 = *q1, vq2 = *q2;
    temp[0] = (unsigned char)alpha;
    temp[1] = (unsigned char)beta;
    temp[2] = (unsigned char)((alpha >> 2) + 2);
    av = vec_ld(0, temp);
    bv = vec_splat(av, 1);
    a2v = vec_splat(av, 2);
    av = vec_splat(av, 0);
    mask = vec_and(vec_and(diff_lt(vp0, vq0, av), diff_lt(vp1, vp0, bv)), diff_lt(vq1, vq0, bv));
    strong = vec_and(mask, diff_lt(vp0, vq0, a2v));
    ap = vec_and(strong, diff_lt(vp2, vp0, bv));
    aq = vec_and(strong, diff_lt(vq2, vq0, bv));
    HAELFTE(vec_mergeh, p3, vp2, vp1, vp0, vq0, vq1, vq2, q3, h0, h1, h2, h3, h4, h5, h6, h7);
    HAELFTE(vec_mergel, p3, vp2, vp1, vp0, vq0, vq1, vq2, q3, l0, l1, l2, l3, l4, l5, l6, l7);
    P0a = vec_packsu(h0, l0); P1a = vec_packsu(h1, l1); P2a = vec_packsu(h2, l2); P0b = vec_packsu(h3, l3);
    Q0a = vec_packsu(h4, l4); Q1a = vec_packsu(h5, l5); Q2a = vec_packsu(h6, l6); Q0b = vec_packsu(h7, l7);
    *p0 = vec_sel(vp0, vec_sel(P0b, P0a, ap), mask);
    *p1 = vec_sel(vp1, P1a, ap);
    *p2 = vec_sel(vp2, P2a, ap);
    *q0 = vec_sel(vq0, vec_sel(Q0b, Q0a, aq), mask);
    *q1 = vec_sel(vq1, Q1a, aq);
    *q2 = vec_sel(vq2, Q2a, aq);
}

/* Rueckfall fuer nicht ausgerichtete Zeilen (kommt bei FFmpeg-Bildern nicht vor) */
static void (*V_intra_c)(uint8_t *, int, int, int);
void lf_v_luma_intra_rueckfall(void (*f)(uint8_t *, int, int, int)) { V_intra_c = f; }

void lf_v_luma_intra(uint8_t *pix, int stride, int alpha, int beta)
{
    vu8 p3, p2, p1, p0, q0, q1, q2, q3;
    if ((((uintptr_t)pix | (uintptr_t)stride) & 15) && V_intra_c) { V_intra_c(pix, stride, alpha, beta); return; }
    p3 = vec_ld(-4 * stride, pix); p2 = vec_ld(-3 * stride, pix); p1 = vec_ld(-2 * stride, pix); p0 = vec_ld(-stride, pix);
    q0 = vec_ld(0, pix); q1 = vec_ld(stride, pix); q2 = vec_ld(2 * stride, pix); q3 = vec_ld(3 * stride, pix);
    stark(p3, &p2, &p1, &p0, &q0, &q1, &q2, q3, alpha, beta);
    vec_st(p2, -3 * stride, pix); vec_st(p1, -2 * stride, pix); vec_st(p0, -stride, pix);
    vec_st(q0, 0, pix); vec_st(q1, stride, pix); vec_st(q2, 2 * stride, pix);
}

/* ---------- Chroma (4:2:0, 8 Zeilen bzw. 8 Spalten je Kante), eigene Fassungen ----------
   Senkrechte Kanten: je Zeile 4 Bytes (p1 p0 q0 q1) mit normalen Ladebefehlen in einen ausgerichteten
   Zwischenspeicher, ein vec_perm sortiert sie in Spalten; Spuren 0..7 = Zeilen 0..7, 8..15 unbenutzt.
   Normaler Filter: tc = tc0 (je 2 Zeilen), nur wenn tc0 > 0; Rechnung wie beim Luma-Filter (DEBLOCK_P0_Q0).
   Starker Filter: p0' = (2p1 + p0 + q1 + 2) >> 2, q0' = (2q1 + q0 + p1 + 2) >> 2, in 16 Bit. */

static inline void chroma_normal(vu8 p1, vu8 *p0, vu8 *q0, vu8 q1, int alpha, int beta, int8_t *tc0)
{
    unsigned char temp[16] __attribute__((aligned(16)));
    vu8 av, bv, mask, tc0masked, vp0 = *p0, vq0 = *q0;
    vs8 tc0vec;
    temp[0] = (unsigned char)alpha;
    temp[1] = (unsigned char)beta;
    av = vec_ld(0, temp);
    bv = vec_splat(av, 1);
    av = vec_splat(av, 0);
    mask = vec_and(vec_and(diff_lt(vp0, vq0, av), diff_lt(p1, vp0, bv)), diff_lt(q1, vq0, bv));
    memcpy(temp, tc0, 4);
    tc0vec = vec_ld(0, (signed char *)temp);
    tc0vec = vec_mergeh(tc0vec, tc0vec);                       /* t0 t0 t1 t1 t2 t2 t3 t3 ... */
    mask = vec_and(mask, (vu8)vec_cmpgt(tc0vec, vec_splat_s8(0)));
    tc0masked = vec_and((vu8)tc0vec, mask);
    DEBLOCK_P0_Q0(vp0, p1, vq0, q1, tc0masked);
    *p0 = vp0;
    *q0 = vq0;
}

static inline void chroma_stark(vu8 p1, vu8 *p0, vu8 *q0, vu8 q1, int alpha, int beta)
{
    unsigned char temp[16] __attribute__((aligned(16)));
    vu8 av, bv, mask, vp0 = *p0, vq0 = *q0;
    vu16 z = vec_splat_u16(0), zwei = vec_splat_u16(2), eins = vec_splat_u16(1);
    vu16 hp1 = (vu16)vec_mergeh((vu8)z, p1), hp0 = (vu16)vec_mergeh((vu8)z, vp0);
    vu16 hq0 = (vu16)vec_mergeh((vu8)z, vq0), hq1 = (vu16)vec_mergeh((vu8)z, q1);
    vu16 np0, nq0;
    temp[0] = (unsigned char)alpha;
    temp[1] = (unsigned char)beta;
    av = vec_ld(0, temp);
    bv = vec_splat(av, 1);
    av = vec_splat(av, 0);
    mask = vec_and(vec_and(diff_lt(vp0, vq0, av), diff_lt(p1, vp0, bv)), diff_lt(q1, vq0, bv));
    np0 = vec_sr(vec_add(vec_add(vec_sl(hp1, eins), hp0), vec_add(hq1, zwei)), zwei);
    nq0 = vec_sr(vec_add(vec_add(vec_sl(hq1, eins), hq0), vec_add(hp1, zwei)), zwei);
    *p0 = vec_sel(vp0, vec_packsu(np0, np0), mask);
    *q0 = vec_sel(vq0, vec_packsu(nq0, nq0), mask);
}

/* 8 Zeilen x 4 Bytes ab pix-2 holen: p1, p0, q0, q1 je in Spuren 0..7 */
static inline void chroma_h_laden(const uint8_t *pix, int stride, vu8 *p1, vu8 *p0, vu8 *q0, vu8 *q1)
{
    uint32_t z[8] __attribute__((aligned(16)));
    const vu8 sp01 = (vu8)(0, 4, 8, 12, 16, 20, 24, 28, 1, 5, 9, 13, 17, 21, 25, 29);
    const vu8 sp23 = (vu8)(2, 6, 10, 14, 18, 22, 26, 30, 3, 7, 11, 15, 19, 23, 27, 31);
    vu8 a, b, c, d;
    int i;
    for (i = 0; i < 8; i++) memcpy(&z[i], pix - 2 + i * stride, 4);
    a = vec_ld(0, (unsigned char *)z);
    b = vec_ld(16, (unsigned char *)z);
    c = vec_perm(a, b, sp01);              /* Spalte 0 (p1) | Spalte 1 (p0) */
    d = vec_perm(a, b, sp23);              /* Spalte 2 (q0) | Spalte 3 (q1) */
    *p1 = c;
    *p0 = vec_sld(c, c, 8);
    *q0 = d;
    *q1 = vec_sld(d, d, 8);
}

/* p0 und q0 zurueck: je Zeile 2 Bytes ab pix-1 */
static inline void chroma_h_schreiben(uint8_t *pix, int stride, vu8 p0, vu8 q0)
{
    unsigned char erg[16] __attribute__((aligned(16)));
    int i;
    vec_st(vec_mergeh(p0, q0), 0, erg);
    for (i = 0; i < 8; i++) {
        uint8_t *d = pix - 1 + i * stride;
        d[0] = erg[2 * i];
        d[1] = erg[2 * i + 1];
    }
}

void lf_h_chroma(uint8_t *pix, int stride, int alpha, int beta, int8_t *tc0)
{
    vu8 p1, p0, q0, q1;
    if (tc0[0] <= 0 && tc0[1] <= 0 && tc0[2] <= 0 && tc0[3] <= 0) return;
    chroma_h_laden(pix, stride, &p1, &p0, &q0, &q1);
    chroma_normal(p1, &p0, &q0, q1, alpha, beta, tc0);
    chroma_h_schreiben(pix, stride, p0, q0);
}

void lf_h_chroma_intra(uint8_t *pix, int stride, int alpha, int beta)
{
    vu8 p1, p0, q0, q1;
    chroma_h_laden(pix, stride, &p1, &p0, &q0, &q1);
    chroma_stark(p1, &p0, &q0, q1, alpha, beta);
    chroma_h_schreiben(pix, stride, p0, q0);
}

/* waagerechte Kante, 8 Spalten: Zeilen p1, p0, q0, q1 je 8 Bytes */
void lf_v_chroma_intra(uint8_t *pix, int stride, int alpha, int beta)
{
    unsigned char erg[32] __attribute__((aligned(16)));
    vu8 p1 = laden(-2 * stride, pix), p0 = laden(-stride, pix), q0 = laden(0, pix), q1 = laden(stride, pix);
    chroma_stark(p1, &p0, &q0, q1, alpha, beta);
    vec_st(p0, 0, erg);
    vec_st(q0, 16, erg);
    memcpy(pix - stride, erg, 8);
    memcpy(pix, erg + 16, 8);
}

void lf_h_luma_intra(uint8_t *pix, int stride, int alpha, int beta)
{
    unsigned char erg[128] __attribute__((aligned(16)));
    const uint8_t *src = pix - 4;
    vu8 r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13, r14, r15;
    vu8 c0, c1, c2, c3, c4, c5, c6, c7;
    int i;
    /* 16 Zeilen x 8 Spalten (p3..q3) einlesen und transponieren */
    r0 = laden(0, src); r1 = laden(stride, src); r2 = laden(2 * stride, src); r3 = laden(3 * stride, src);
    r4 = laden(4 * stride, src); r5 = laden(5 * stride, src); r6 = laden(6 * stride, src); r7 = laden(7 * stride, src);
    r8 = laden(8 * stride, src); r9 = laden(9 * stride, src); r10 = laden(10 * stride, src); r11 = laden(11 * stride, src);
    r12 = laden(12 * stride, src); r13 = laden(13 * stride, src); r14 = laden(14 * stride, src); r15 = laden(15 * stride, src);
    r0 = vec_mergeh(r0, r8); r1 = vec_mergeh(r1, r9); r2 = vec_mergeh(r2, r10); r3 = vec_mergeh(r3, r11);
    r4 = vec_mergeh(r4, r12); r5 = vec_mergeh(r5, r13); r6 = vec_mergeh(r6, r14); r7 = vec_mergeh(r7, r15);
    r8 = vec_mergeh(r0, r4); r9 = vec_mergel(r0, r4); r10 = vec_mergeh(r1, r5); r11 = vec_mergel(r1, r5);
    r12 = vec_mergeh(r2, r6); r13 = vec_mergel(r2, r6); r14 = vec_mergeh(r3, r7); r15 = vec_mergel(r3, r7);
    r0 = vec_mergeh(r8, r12); r1 = vec_mergel(r8, r12); r2 = vec_mergeh(r9, r13); r3 = vec_mergel(r9, r13);
    r4 = vec_mergeh(r10, r14); r5 = vec_mergel(r10, r14); r6 = vec_mergeh(r11, r15); r7 = vec_mergel(r11, r15);
    c0 = vec_mergeh(r0, r4); c1 = vec_mergel(r0, r4); c2 = vec_mergeh(r1, r5); c3 = vec_mergel(r1, r5);
    c4 = vec_mergeh(r2, r6); c5 = vec_mergel(r2, r6); c6 = vec_mergeh(r3, r7); c7 = vec_mergel(r3, r7);
    stark(c0, &c1, &c2, &c3, &c4, &c5, &c6, c7, alpha, beta);
    /* 8 Spalten zurueck in 16 Zeilen (je Vektor zwei Zeilen) */
    r0 = vec_mergeh(c0, c4); r4 = vec_mergel(c0, c4); r1 = vec_mergeh(c1, c5); r5 = vec_mergel(c1, c5);
    r2 = vec_mergeh(c2, c6); r6 = vec_mergel(c2, c6); r3 = vec_mergeh(c3, c7); r7 = vec_mergel(c3, c7);
    r8 = vec_mergeh(r0, r2); r10 = vec_mergel(r0, r2); r9 = vec_mergeh(r1, r3); r11 = vec_mergel(r1, r3);
    r12 = vec_mergeh(r4, r6); r14 = vec_mergel(r4, r6); r13 = vec_mergeh(r5, r7); r15 = vec_mergel(r5, r7);
    vec_st(vec_mergeh(r8, r9), 0, erg);    vec_st(vec_mergel(r8, r9), 16, erg);
    vec_st(vec_mergeh(r10, r11), 32, erg); vec_st(vec_mergel(r10, r11), 48, erg);
    vec_st(vec_mergeh(r12, r13), 64, erg); vec_st(vec_mergel(r12, r13), 80, erg);
    vec_st(vec_mergeh(r14, r15), 96, erg); vec_st(vec_mergel(r14, r15), 112, erg);
    for (i = 0; i < 16; i++) {
        const unsigned char *z = erg + i * 8;
        uint8_t *d = pix + i * stride;
        d[-3] = z[1];
        *(uint32_t *)(d - 2) = *(const uint32_t *)(z + 2);
        d[2] = z[6];
    }
}
