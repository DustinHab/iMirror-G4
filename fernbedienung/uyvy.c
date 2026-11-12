/* I420/J420 (planar) -> UYVY (2vuy) fuer GL_YCBCR_422_APPLE, mit AltiVec.
   Volle Spanne (J420, 0..255) wird dabei auf die Videospanne gestaucht, die die Grafikkarte erwartet.
   Alle Zeiger 16-Byte-ausgerichtet, Zeilenbreiten Vielfache von 32 (Y) bzw. 16 (U, V). */
#include "uyvy.h"

static inline vector unsigned char stauchen(vector unsigned char x, vector unsigned short k)
{
    const vector unsigned char null = vec_splat_u8(0);
    const vector unsigned short n16 = vec_splat_u16(0), acht = vec_splat_u16(8);
    const vector unsigned short sechzehn = { 16, 16, 16, 16, 16, 16, 16, 16 };
    vector unsigned short a = (vector unsigned short)vec_mergeh(null, x);
    vector unsigned short b = (vector unsigned short)vec_mergel(null, x);
    a = vec_add(vec_sr(vec_mladd(a, k, n16), acht), sechzehn);
    b = vec_add(vec_sr(vec_mladd(b, k, n16), acht), sechzehn);
    return vec_pack(a, b);
}

void zu_uyvy(const unsigned char *y, int py, const unsigned char *u, int pu, const unsigned char *v, int pv,
             unsigned char *aus, int pa, int w, int h, int voll)
{
    const vector unsigned short ky = { 220, 220, 220, 220, 220, 220, 220, 220 };
    const vector unsigned short kc = { 225, 225, 225, 225, 225, 225, 225, 225 };
    int r, x;
    for (r = 0; r < h; r++) {
        const vector unsigned char *Y = (const vector unsigned char *)(y + r * py);
        const vector unsigned char *U = (const vector unsigned char *)(u + (r >> 1) * pu);
        const vector unsigned char *V = (const vector unsigned char *)(v + (r >> 1) * pv);
        vector unsigned char *A = (vector unsigned char *)(aus + r * pa);
        for (x = 0; x < w; x += 32) {
            vector unsigned char y0 = Y[0], y1 = Y[1], uu = U[0], vv = V[0], uvh, uvl;
            if (voll) {
                y0 = stauchen(y0, ky);
                y1 = stauchen(y1, ky);
                uu = stauchen(uu, kc);
                vv = stauchen(vv, kc);
            }
            uvh = vec_mergeh(uu, vv);
            uvl = vec_mergel(uu, vv);
            A[0] = vec_mergeh(uvh, y0);
            A[1] = vec_mergel(uvh, y0);
            A[2] = vec_mergeh(uvl, y1);
            A[3] = vec_mergel(uvl, y1);
            Y += 2;
            U++;
            V++;
            A += 4;
        }
    }
}
