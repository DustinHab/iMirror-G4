/* Probe: YUV -> RGB mit GL_ATI_text_fragment_shader, Texturen wie im Fenster: 512x1024 / 256x512 (Zweierpotenz),
   Bild 64x64 links oben, wahlweise Client-Speicher mit DMA (wie im Fenster) oder normales Hochladen. */
#import <Cocoa/Cocoa.h>
#import <OpenGL/gl.h>
#import <OpenGL/glext.h>
#include <stdio.h>
#include <math.h>

#ifndef GL_TEXT_FRAGMENT_SHADER_ATI
#define GL_TEXT_FRAGMENT_SHADER_ATI 0x8200
#endif

/* mit Eckenmaske auf Einheit 3 (Alpha): Farbe mal Maske, Alpha = Maske */
static const char *Prog =
    "!!ATIfs1.0\nStartConstants;\nCONSTANT c0 = {0.701, 0.0, 0.0, 0.0};\nCONSTANT c1 = {0.0, 0.0, 0.886, 0.0};\n"
    "CONSTANT c2 = {0.0, 0.357068, 0.0, 0.0};\nCONSTANT c3 = {0.0, 0.172068, 0.0, 0.0};\n"
    "EndConstants;\nStartOutputPass;\nSampleMap r0, t0.str;\nSampleMap r1, t1.str;\nSampleMap r2, t2.str;\nSampleMap r5, t3.str;\n"
    "MUL r3.rgb, r2.bias, c0;\nMAD r3.rgb, r1.bias, c1, r3;\nMUL r4.rgb, r2.bias, c2;\nMAD r4.rgb, r1.bias, c3, r4;\n"
    "SUB r3.rgb, r3, r4;\nADD r3.rgb, r3, r3;\nADD r0.rgb, r0, r3;\nMUL r0.rgb, r0, r5.a;\nMOV r0.a, r5.a;\nEndPass;\n";

#define W 332
#define H 720
static int TW = 512, TH = 1024;
static int Client, Kanal = -1, Runden = 1, Laden = 1, Neu_laden, Maske, Linear;

@interface Ansicht : NSOpenGLView @end
@implementation Ansicht
- (void)pruefen:(NSTimer *)t
{
    static unsigned char rgb[W * H * 4];
    unsigned char *buf = valloc(TW * TH + 2 * (TW / 2) * (TH / 2)), *pl[3];
    GLuint tex[3], prog = 0;
    int i, j, fmax = 0;
    double fsum = 0;
    [[self openGLContext] makeCurrentContext];
    pl[0] = buf; pl[1] = buf + TW * TH; pl[2] = pl[1] + (TW / 2) * (TH / 2);
    memset(buf, 0, TW * TH); memset(pl[1], 128, 2 * (TW / 2) * (TH / 2));
    for (j = 0; j < H; j++) for (i = 0; i < W; i++) pl[0][j * TW + i] = (unsigned char)((i * 3 + j) & 255);
    for (j = 0; j < H / 2; j++) for (i = 0; i < W / 2; i++) {
        pl[1][j * (TW / 2) + i] = (unsigned char)(64 + (j & 127));
        pl[2][j * (TW / 2) + i] = (unsigned char)(64 + (i & 127));
    }
    glGenTextures(4, tex);
    {
        /* Maske: ueberall 255, linke untere Ecke 0 (zum Pruefen) */
        static unsigned char m[512 * 1024];
        memset(m, 255, sizeof m);
        for (j = 0; j < 8; j++) for (i = 0; i < 8; i++) m[j * 512 + i] = 0;
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, tex[3]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA8, 512, 1024, 0, GL_ALPHA, GL_UNSIGNED_BYTE, m);
        glEnable(GL_TEXTURE_2D);
    }
    for (i = 0; i < 3; i++) {
        int tw = i ? TW / 2 : TW, th = i ? TH / 2 : TH;
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        if (Client) {
            glTextureRangeAPPLE(GL_TEXTURE_2D, tw * th, pl[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_STORAGE_HINT_APPLE, GL_STORAGE_SHARED_APPLE);
            glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE);
        } else glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_FALSE);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, tw);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, Linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, Linear ? GL_LINEAR : GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE8, tw, th, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, pl[i]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, i ? W / 2 : W, i ? H / 2 : H, GL_LUMINANCE, GL_UNSIGNED_BYTE, pl[i]);
        glEnable(GL_TEXTURE_2D);
    }
    glGenProgramsARB(1, &prog);
    glBindProgramARB(GL_TEXT_FRAGMENT_SHADER_ATI, prog);
    if (Kanal >= 0) {
        static char p[512];
        snprintf(p, sizeof p, "!!ATIfs1.0\nStartOutputPass;\nSampleMap r0, t0.str;\nSampleMap r1, t1.str;\nSampleMap r2, t2.str;\n"
                 "MOV r0, r%d;\nEndPass;\n", Kanal);
        glProgramStringARB(GL_TEXT_FRAGMENT_SHADER_ATI, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)strlen(p), p);
    } else {
        int l;
        static const char *weg = "!!ATIfs1.0\nStartOutputPass;\nSampleMap r0, t0.str;\nSampleMap r1, t1.str;\nSampleMap r2, t2.str;\n"
                                 "SampleMap r5, t3.str;\nMOV r0, r1;\nEndPass;\n";
        /* Reihenfolge wie im Fenster: erst das richtige Programm (zur Pruefung), dann Aufwaermen, dann neu laden */
        if (Neu_laden >= 4)
            glProgramStringARB(GL_TEXT_FRAGMENT_SHADER_ATI, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)strlen(Prog), Prog);
        for (l = 1; l < Laden; l++) {
            glProgramStringARB(GL_TEXT_FRAGMENT_SHADER_ATI, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)strlen(weg), weg);
            if (Neu_laden >= 2) {
                /* einmal mit dem Wegwerf-Programm vollstaendig zeichnen, wie im funktionierenden Test */
                GLint pos2;
                unsigned char px[4];
                int q;
                glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &pos2);
                glEnable(GL_TEXT_FRAGMENT_SHADER_ATI);
                glViewport(0, 0, W, H);
                glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, 0, H, -1, 1);
                glMatrixMode(GL_MODELVIEW); glLoadIdentity();
                glClear(GL_COLOR_BUFFER_BIT);
                glBegin(GL_QUADS);
                for (q = 0; q < 4; q++) glMultiTexCoord2f(GL_TEXTURE0 + q, 0, 0);
                glVertex2f(0, 0);
                for (q = 0; q < 4; q++) glMultiTexCoord2f(GL_TEXTURE0 + q, 1, 0);
                glVertex2f(W, 0);
                for (q = 0; q < 4; q++) glMultiTexCoord2f(GL_TEXTURE0 + q, 1, 1);
                glVertex2f(W, H);
                for (q = 0; q < 4; q++) glMultiTexCoord2f(GL_TEXTURE0 + q, 0, 1);
                glVertex2f(0, H);
                glEnd();
                glDisable(GL_TEXT_FRAGMENT_SHADER_ATI);
                glFinish();
                if (Neu_laden >= 3) glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            }
        }
        glProgramStringARB(GL_TEXT_FRAGMENT_SHADER_ATI, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)strlen(Prog), Prog);
    }
    glEnable(GL_TEXT_FRAGMENT_SHADER_ATI);
    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    int runde;
    for (runde = 0; runde < Runden; runde++) {
    if (runde && Neu_laden) {
        glDisable(GL_TEXT_FRAGMENT_SHADER_ATI);
        glProgramStringARB(GL_TEXT_FRAGMENT_SHADER_ATI, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)strlen(Prog), Prog);
        glEnable(GL_TEXT_FRAGMENT_SHADER_ATI);
    }
    glClear(GL_COLOR_BUFFER_BIT);
    {
        float s = (float)W / TW, tt = (float)H / TH, ms = (float)W / 512, mt2 = (float)H / 1024;
        glBegin(GL_QUADS);
        for (i = 0; i < 3; i++) glMultiTexCoord2f(GL_TEXTURE0 + i, 0, 0);
        glMultiTexCoord2f(GL_TEXTURE3, 0, 0);
        glVertex2f(0, 0);
        for (i = 0; i < 3; i++) glMultiTexCoord2f(GL_TEXTURE0 + i, s, 0);
        glMultiTexCoord2f(GL_TEXTURE3, ms, 0);
        glVertex2f(W, 0);
        for (i = 0; i < 3; i++) glMultiTexCoord2f(GL_TEXTURE0 + i, s, tt);
        glMultiTexCoord2f(GL_TEXTURE3, ms, mt2);
        glVertex2f(W, H);
        for (i = 0; i < 3; i++) glMultiTexCoord2f(GL_TEXTURE0 + i, 0, tt);
        glMultiTexCoord2f(GL_TEXTURE3, 0, mt2);
        glVertex2f(0, H);
        glEnd();
    }
    glFinish();
    if (Maske && runde + 1 < Runden) {
        /* wie im Fenster: danach Festfunktion auf Einheit 0 (Alphamaske, Mischen), dann wieder der Shader */
        static GLuint mt;
        static unsigned char ma[64 * 64];
        int q;
        glDisable(GL_TEXT_FRAGMENT_SHADER_ATI);
        for (q = 2; q >= 0; q--) { glActiveTexture(GL_TEXTURE0 + q); glDisable(GL_TEXTURE_2D); }
        if (!mt) {
            memset(ma, 255, sizeof ma);
            glGenTextures(1, &mt);
            glBindTexture(GL_TEXTURE_2D, mt);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA8, 64, 64, 0, GL_ALPHA, GL_UNSIGNED_BYTE, ma);
        }
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, mt);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ZERO, GL_SRC_ALPHA);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(0, 0);
        glTexCoord2f(0.5, 0); glVertex2f(30, 0);
        glTexCoord2f(0.5, 0.5); glVertex2f(30, 30);
        glTexCoord2f(0, 0.5); glVertex2f(0, 30);
        glEnd();
        glDisable(GL_BLEND);
        glDisable(GL_TEXTURE_2D);
        for (q = 2; q >= 0; q--) {
            glActiveTexture(GL_TEXTURE0 + q);
            glBindTexture(GL_TEXTURE_2D, tex[q]);
            glEnable(GL_TEXTURE_2D);
        }
        glBindProgramARB(GL_TEXT_FRAGMENT_SHADER_ATI, prog);
        if (Maske == 2) glProgramStringARB(GL_TEXT_FRAGMENT_SHADER_ATI, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)strlen(Prog), Prog);
        glEnable(GL_TEXT_FRAGMENT_SHADER_ATI);
    }
    }
    glDisable(GL_TEXT_FRAGMENT_SHADER_ATI);
    glFinish();
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgb);
    for (j = 0; j < H; j++) for (i = 0; i < W; i++) {
        double Y = pl[0][j * TW + i], U = pl[1][(j / 2) * (TW / 2) + i / 2] - 128.0, V = pl[2][(j / 2) * (TW / 2) + i / 2] - 128.0;
        double e[3] = { Y + 1.402 * V, Y - 0.344136 * U - 0.714136 * V, Y + 1.772 * U };
        int k;
        for (k = 0; k < 3; k++) {
            int s2 = (int)floor(e[k] + 0.5), d;
            if (s2 < 0) s2 = 0;
            if (s2 > 255) s2 = 255;
            d = abs(s2 - rgb[(j * W + i) * 4 + k]);
            fsum += d;
            if (d > fmax) fmax = d;
        }
    }
    {
        /* Spalten mit grosser Abweichung (Streifen?) */
        int x, yy, n = 0;
        printf("Spalten mit Abweichung > 8:");
        for (x = 0; x < W; x++) {
            int m = 0;
            for (yy = 0; yy < H; yy += 7) {
                double Y = pl[0][yy * TW + x], U = pl[1][(yy / 2) * (TW / 2) + x / 2] - 128.0, V = pl[2][(yy / 2) * (TW / 2) + x / 2] - 128.0;
                double g = Y - 0.344136 * U - 0.714136 * V;
                int s2 = (int)floor(g + 0.5), d;
                if (s2 < 0) s2 = 0;
                if (s2 > 255) s2 = 255;
                d = abs(s2 - rgb[(yy * W + x) * 4 + 1]);
                if (d > m) m = d;
            }
            if (m > 8 && n++ < 40) printf(" %d(%d)", x, m);
        }
        printf("  [%d Spalten]\n", n);
    }
    {
        int x, amin = 255, n0 = 0;
        for (x = 0; x < W * H; x++) { if (rgb[x * 4 + 3] < amin) amin = rgb[x * 4 + 3]; if (rgb[x * 4 + 3] == 0) n0++; }
        printf("kleinstes Alpha %d, Bildpunkte mit Alpha 0: %d (erwartet 64), Ecke (2,2) = %d %d %d %d\n", amin, n0,
               rgb[(2 * W + 2) * 4], rgb[(2 * W + 2) * 4 + 1], rgb[(2 * W + 2) * 4 + 2], rgb[(2 * W + 2) * 4 + 3]);
    }
    printf("%dx%d %s Kanal %d: Abweichung max %d, Mittel %.2f; (10,10) = %d %d %d %d, (40,20) = %d %d %d\n", TW, TH,
           Client ? "Client" : "normal", Kanal, fmax, fsum / (W * H * 3),
           rgb[(10 * W + 10) * 4], rgb[(10 * W + 10) * 4 + 1], rgb[(10 * W + 10) * 4 + 2], rgb[(10 * W + 10) * 4 + 3],
           rgb[(20 * W + 40) * 4], rgb[(20 * W + 40) * 4 + 1], rgb[(20 * W + 40) * 4 + 2]);
    fflush(stdout);
    exit(0);
}
@end

int main(int argc, char **argv)
{
    NSAutoreleasePool *p = [[NSAutoreleasePool alloc] init];
    NSOpenGLPixelFormatAttribute a[] = { NSOpenGLPFAAccelerated, NSOpenGLPFANoRecovery, NSOpenGLPFAColorSize, 24, NSOpenGLPFAAlphaSize, 8, 0 };
    NSWindow *f;
    Ansicht *v;
    /* gputest TW TH client(0/1) kanal(-1 = ganzes Programm) */
    if (argc > 2) { TW = atoi(argv[1]); TH = atoi(argv[2]); }
    if (argc > 3) Client = atoi(argv[3]);
    if (argc > 4) Kanal = atoi(argv[4]);
    if (argc > 5) Runden = atoi(argv[5]);
    if (argc > 6) Laden = atoi(argv[6]);
    if (argc > 7) Neu_laden = atoi(argv[7]);
    if (argc > 8) Maske = atoi(argv[8]);
    if (argc > 9) Linear = atoi(argv[9]);
    [NSApplication sharedApplication];
    f = [[NSWindow alloc] initWithContentRect:NSMakeRect(10, 10, W, H) styleMask:NSBorderlessWindowMask backing:NSBackingStoreBuffered defer:NO];
    v = [[Ansicht alloc] initWithFrame:NSMakeRect(0, 0, W, H) pixelFormat:[[[NSOpenGLPixelFormat alloc] initWithAttributes:a] autorelease]];
    [f setContentView:v];
    [f orderFront:nil];
    [NSTimer scheduledTimerWithTimeInterval:0.3 target:v selector:@selector(pruefen:) userInfo:nil repeats:NO];
    [NSApp run];
    [p release];
    return 0;
}
