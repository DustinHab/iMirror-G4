/* H.264 direkt mit dem FFmpeg in PowerVLCs avcodec-Modul.
   Das Modul exportiert nur VLCs Einstiegspunkte, die FFmpeg-Funktionen stehen aber in seiner Symboltabelle.
   Nach dlopen werden sie dort per Name gesucht: Adresse aus der Tabelle plus Verschiebung des geladenen Bildes.
   Von den FFmpeg-Strukturen wird nur der seit Jahren feste Anfang von AVFrame gelesen. */
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <malloc/malloc.h>
#include <stdlib.h>
#include "dekoder.h"

/* AltiVec-Glaettungsfilter (filter.c) statt der C-Fassungen in PowerVLCs FFmpeg */
extern void lf_h_luma(uint8_t *, int, int, int, int8_t *);
extern void lf_v_luma_intra(uint8_t *, int, int, int);
extern void lf_h_luma_intra(uint8_t *, int, int, int);
extern void lf_v_luma_intra_rueckfall(void (*)(uint8_t *, int, int, int));
extern void lf_h_chroma(uint8_t *, int, int, int, int8_t *);
extern void lf_h_chroma_intra(uint8_t *, int, int, int);
extern void lf_v_chroma_intra(uint8_t *, int, int, int);
static const struct mach_header *Mh;
static intptr_t Slide;
static void *C_h, *C_vi, *C_hi;     /* C-Fassungen im Modul */
static void *C_hc, *C_hci, *C_vci;  /* Chroma: h normal, h stark, v stark */
static void **Slot;                 /* &h264dsp.h264_h_loop_filter_luma im H264-Kontext */
static int I_hc = -1, I_hci = -1, I_vci = -1;   /* Fundstellen der Chroma-Zeiger relativ zu Slot */
/* welche Filter getauscht werden. Gemessen am Mitschnitt des echten iPhone-Stroms (3353 Bilder, je 3 Laeufe):
   alles C 7,30 ms/Bild; h_luma + v_luma_intra + v_chroma_intra 7,08 ms (-3 %, bitgleich); mit den beiden
   senkrechten Chroma-Filtern wieder langsamer (C bricht dort frueh ab), h_luma_intra ohne Gewinn */
static int Auswahl = 1 | 2 | 16;
static int Tausch = -1;             /* -1 noch nicht gesucht, 0 aus, 1 aktiv */
#ifdef EIGENES_FFMPEG
/* eigenes FFmpeg 4.4.5 (nur H.264, -O3, AltiVec fuer den 7450), fest eingebunden */
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/mem.h>
#include <libavutil/log.h>
#endif

#define POLSTER 64          /* AV_INPUT_BUFFER_PADDING_SIZE */
#define NOCHMAL (-35)       /* AVERROR(EAGAIN) auf Darwin */

static struct {
    const void *(*find_by_name)(const char *);
    void *(*alloc_ctx)(const void *);
    int (*open2)(void *, const void *, void **);
    int (*send)(void *, const void *);
    int (*recv)(void *, void *);
    void *(*packet_alloc)(void);
    int (*from_data)(void *, unsigned char *, int);
    void (*packet_unref)(void *);
    void *(*frame_alloc)(void);
    void (*frame_unref)(void *);
    void *(*malloc_)(size_t);
    void (*free_)(void *);
    int (*opt_set)(void *, const char *, const char *, int);
    void (*log_level)(int);
} F;
static void *Ctx, *Pkt, *Frame;

static void *suchen(const struct mach_header *mh, intptr_t slide, const char *name)
{
    const struct load_command *lc = (const struct load_command *)(mh + 1);
    const struct segment_command *le = NULL;
    const struct symtab_command *st = NULL;
    const char *basis, *str;
    const struct nlist *sym;
    uint32_t i;
    for (i = 0; i < mh->ncmds; i++) {
        if (lc->cmd == LC_SEGMENT && !strcmp(((const struct segment_command *)lc)->segname, "__LINKEDIT"))
            le = (const struct segment_command *)lc;
        else if (lc->cmd == LC_SYMTAB)
            st = (const struct symtab_command *)lc;
        lc = (const struct load_command *)((const char *)lc + lc->cmdsize);
    }
    if (!le || !st) return NULL;
    basis = (const char *)(slide + le->vmaddr - le->fileoff);
    sym = (const struct nlist *)(basis + st->symoff);
    str = basis + st->stroff;
    for (i = 0; i < st->nsyms; i++) {
        if (sym[i].n_type & N_STAB) continue;
        if ((sym[i].n_type & N_TYPE) != N_SECT) continue;
        if (!strcmp(str + sym[i].n_un.n_strx, name)) return (void *)(sym[i].n_value + slide);
    }
    return NULL;
}

int dek_an(const char *modul, char *fehler, int nf)
{
    const struct mach_header *mh = NULL;
    intptr_t slide = 0;
    uint32_t i, n;
    const void *codec;
#ifdef EIGENES_FFMPEG
    (void)modul; (void)mh; (void)slide; (void)i; (void)n;
#define FEST(feld, fn) F.feld = (__typeof__(F.feld))(fn)
    FEST(find_by_name, avcodec_find_decoder_by_name);
    FEST(alloc_ctx, avcodec_alloc_context3);
    FEST(open2, avcodec_open2);
    FEST(send, avcodec_send_packet);
    FEST(recv, avcodec_receive_frame);
    FEST(packet_alloc, av_packet_alloc);
    FEST(from_data, av_packet_from_data);
    FEST(packet_unref, av_packet_unref);
    FEST(frame_alloc, av_frame_alloc);
    FEST(frame_unref, av_frame_unref);
    FEST(malloc_, av_malloc);
    FEST(free_, av_free);
    FEST(opt_set, av_opt_set);
    FEST(log_level, av_log_set_level);
#undef FEST
    goto oeffnen;
#endif
    if (!dlopen(modul, RTLD_NOW | RTLD_LOCAL)) { snprintf(fehler, nf, "dlopen: %s", dlerror()); return -1; }
    n = _dyld_image_count();
    for (i = 0; i < n; i++) {
        const char *nm = _dyld_get_image_name(i);
        if (nm && strstr(nm, "libavcodec_plugin")) {
            mh = _dyld_get_image_header(i);
            slide = _dyld_get_image_vmaddr_slide(i);
            break;
        }
    }
    if (!mh) { snprintf(fehler, nf, "Modul nicht unter den geladenen Bildern"); return -1; }
#define HOL(feld, name) \
    if (!(F.feld = (__typeof__(F.feld))suchen(mh, slide, name))) { snprintf(fehler, nf, "%s fehlt", name); return -1; }
    HOL(find_by_name, "_avcodec_find_decoder_by_name");
    HOL(alloc_ctx, "_avcodec_alloc_context3");
    HOL(open2, "_avcodec_open2");
    HOL(send, "_avcodec_send_packet");
    HOL(recv, "_avcodec_receive_frame");
    HOL(packet_alloc, "_av_packet_alloc");
    HOL(from_data, "_av_packet_from_data");
    HOL(packet_unref, "_av_packet_unref");
    HOL(frame_alloc, "_av_frame_alloc");
    HOL(frame_unref, "_av_frame_unref");
    HOL(malloc_, "_av_malloc");
    HOL(free_, "_av_free");
    HOL(opt_set, "_av_opt_set");
    HOL(log_level, "_av_log_set_level");
#undef HOL
    Mh = mh;
    Slide = slide;
    C_h = suchen(mh, slide, "_h264_h_loop_filter_luma_8_c");
    C_vi = suchen(mh, slide, "_h264_v_loop_filter_luma_intra_8_c");
    C_hi = suchen(mh, slide, "_h264_h_loop_filter_luma_intra_8_c");
    C_hc = suchen(mh, slide, "_h264_h_loop_filter_chroma_8_c");
    C_hci = suchen(mh, slide, "_h264_h_loop_filter_chroma_intra_8_c");
    C_vci = suchen(mh, slide, "_h264_v_loop_filter_chroma_intra_8_c");
    if (C_vi) lf_v_luma_intra_rueckfall((void (*)(uint8_t *, int, int, int))C_vi);
    if (getenv("DEK_OHNE_FILTER")) Tausch = 0;
    if (getenv("DEK_FILTER")) Auswahl = atoi(getenv("DEK_FILTER"));
#ifdef EIGENES_FFMPEG
oeffnen:
#endif
    F.log_level(-8);    /* AV_LOG_QUIET: Fehler zaehlt der Aufrufer selbst */
    codec = F.find_by_name("h264");
    if (!codec) { snprintf(fehler, nf, "kein H.264-Decoder"); return -1; }
    Ctx = F.alloc_ctx(codec);
    if (!Ctx) { snprintf(fehler, nf, "kein Kontext"); return -1; }
    /* jedes Bild sofort ausgeben (kein Umsortieren, der Strom hat keine B-Bilder), Abkuerzungen ohne
       sichtbaren Unterschied, ein Faden (ein Prozessor) */
    F.opt_set(Ctx, "flags", "+low_delay", 0);
    F.opt_set(Ctx, "flags2", "+fast", 0);
    F.opt_set(Ctx, "threads", "1", 0);
    if (F.open2(Ctx, codec, NULL) < 0) { snprintf(fehler, nf, "avcodec_open2"); return -1; }
    Pkt = F.packet_alloc();
    Frame = F.frame_alloc();
    if (!Pkt || !Frame) { snprintf(fehler, nf, "kein Paket/Bild"); return -1; }
    return 0;
}

unsigned char *dek_puffer(int n)
{
    unsigned char *p = F.malloc_((size_t)n + POLSTER);
    if (p) memset(p + n, 0, POLSTER);
    return p;
}

void dek_puffer_weg(unsigned char *p) { F.free_(p); }

int dek_schicken(unsigned char *p, int n)
{
    int r;
    if (F.from_data(Pkt, p, n) < 0) { F.free_(p); return -1; }
    r = F.send(Ctx, Pkt);
    F.packet_unref(Pkt);
    return r == NOCHMAL ? 0 : r;
}

/* Glaettungsfilter tauschen: im H264-Kontext (AVCodecContext.priv_data, Versatz 24 seit Jahren fest) die Stelle
   suchen, an der die drei C-Fassungen in der Reihenfolge des H264DSPContext stehen (h_luma, h_luma_mbaff,
   v_luma_intra, h_luma_intra). Nur wenn genau das passt, wird getauscht. FFmpeg setzt die Zeiger bei neuem
   SPS neu, deshalb nach jedem Bild nachsehen (drei Vergleiche). */
static void filter_tauschen(void)
{
    if (Tausch == 0) return;
    if (Tausch < 0) {
        void **priv = *(void ***)((char *)Ctx + 24), **p, **ende;
        size_t n = priv ? malloc_size(priv) : 0;
        Tausch = 0;
        if (!n || !C_h || !C_vi || !C_hi) return;
        ende = (void **)((char *)priv + n) - 4;
        for (p = priv; p < ende; p++)
            if (p[0] == C_h && p[2] == C_vi && p[3] == C_hi) { Slot = p; break; }
        if (!Slot) return;
        Tausch = 1;
        /* Chroma-Filter stehen im selben H264DSPContext direkt dahinter: jeden Zeiger genau einmal suchen */
        {
            int j, n_hc = 0, n_hci = 0, n_vci = 0;
            for (j = 4; j < 16 && Slot + j < ende + 4; j++) {
                if (C_hc && Slot[j] == C_hc) { I_hc = j; n_hc++; }
                if (C_hci && Slot[j] == C_hci) { I_hci = j; n_hci++; }
                if (C_vci && Slot[j] == C_vci) { I_vci = j; n_vci++; }
            }
            if (n_hc != 1) I_hc = -1;
            if (n_hci != 1) I_hci = -1;
            if (n_vci != 1) I_vci = -1;
        }
    }
    /* Auswahl (Bitmaske, zum Messen per DEK_FILTER): 1 h_luma, 2 v_luma_intra, 4 h_chroma, 8 h_chroma_intra,
       16 v_chroma_intra, 32 h_luma_intra */
    if ((Auswahl & 1) && Slot[0] == C_h) Slot[0] = (void *)lf_h_luma;
    if ((Auswahl & 2) && Slot[2] == C_vi) Slot[2] = (void *)lf_v_luma_intra;
    if ((Auswahl & 32) && Slot[3] == C_hi) Slot[3] = (void *)lf_h_luma_intra;
    if ((Auswahl & 4) && I_hc >= 0 && Slot[I_hc] == C_hc) Slot[I_hc] = (void *)lf_h_chroma;
    if ((Auswahl & 8) && I_hci >= 0 && Slot[I_hci] == C_hci) Slot[I_hci] = (void *)lf_h_chroma_intra;
    if ((Auswahl & 16) && I_vci >= 0 && Slot[I_vci] == C_vci) Slot[I_vci] = (void *)lf_v_chroma_intra;
}

/* Fundstellen fuer die Pruefung: Luma gefunden, Chroma-Versaetze */
void dek_filter_stellen(int *hc, int *hci, int *vci) { *hc = I_hc; *hci = I_hci; *vci = I_vci; }

int dek_filter_aktiv(void) { return Tausch; }

void *dek_symbol(const char *name) { return Mh ? suchen(Mh, Slide, name) : NULL; }

int dek_holen(Dek_bild *b)
{
    int r = F.recv(Ctx, Frame), i;
    if (r == 0) filter_tauschen();
    unsigned char **daten;
    int *zeilen, *wh;
    if (r == NOCHMAL) return 0;
    if (r < 0) return r;
    /* AVFrame: data[8], linesize[8], extended_data, width, height, nb_samples, format */
    daten = (unsigned char **)Frame;
    zeilen = (int *)((char *)Frame + 8 * sizeof(void *));
    wh = (int *)((char *)Frame + 8 * sizeof(void *) + 8 * sizeof(int) + sizeof(void *));
    for (i = 0; i < 3; i++) { b->ebene[i] = daten[i]; b->zeile[i] = zeilen[i]; }
    b->breite = wh[0];
    b->hoehe = wh[1];
    b->format = wh[3];
    if (b->breite < 16 || b->hoehe < 16 || b->breite > 4096 || b->hoehe > 4096 || b->zeile[0] < b->breite || !b->ebene[2]) {
        F.frame_unref(Frame);
        return -1;
    }
    return 1;
}

void dek_fertig(void) { F.frame_unref(Frame); }

void dek_entlasten(int an) { F.opt_set(Ctx, "skip_loop_filter", an ? "nonref" : "default", 0); }

/* nur fuer Messungen */
void dek_filter(const char *wert) { F.opt_set(Ctx, "skip_loop_filter", wert, 0); }

/* ---------- Ton: AAC (auch AAC-ELD) mit demselben FFmpeg ----------
   Die AudioSpecificConfig kommt ueber AVCodecParameters in den Kontext: dessen Anfang (codec_type, codec_id,
   codec_tag, extradata, extradata_size) ist seit FFmpeg 3.1 fest; vor dem Schreiben wird er an den Vorgaben von
   avcodec_parameters_alloc geprueft. */
static void *TCtx, *TPkt, *TFrame;

int dek_ton_an(const unsigned char *asc, int n, int rate, int kanaele, char *fehler, int nf)
{
    void *(*par_alloc)(void);
    int (*par_to_ctx)(void *, const void *);
    void (*par_free)(void **);
    const void *codec;
    unsigned char *par, *ed;
    if (!F.find_by_name) { snprintf(fehler, nf, "Dekoder nicht geladen"); return -1; }
#ifdef EIGENES_FFMPEG
    par_alloc = (void *(*)(void))avcodec_parameters_alloc;
    par_to_ctx = (int (*)(void *, const void *))avcodec_parameters_to_context;
    par_free = (void (*)(void **))avcodec_parameters_free;
#else
    par_alloc = (void *(*)(void))suchen(Mh, Slide, "_avcodec_parameters_alloc");
    par_to_ctx = (int (*)(void *, const void *))suchen(Mh, Slide, "_avcodec_parameters_to_context");
    par_free = (void (*)(void **))suchen(Mh, Slide, "_avcodec_parameters_free");
#endif
    if (!par_alloc || !par_to_ctx || !par_free) { snprintf(fehler, nf, "avcodec_parameters fehlt"); return -1; }
    codec = F.find_by_name("aac");
    if (!codec) { snprintf(fehler, nf, "kein AAC-Decoder"); return -1; }
    TCtx = F.alloc_ctx(codec);
    par = par_alloc();
    if (!TCtx || !par) { snprintf(fehler, nf, "kein Kontext"); return -1; }
    if (*(int *)par != -1 || *(int *)(par + 4) != 0 || *(void **)(par + 12) || *(int *)(par + 16)) {
        snprintf(fehler, nf, "AVCodecParameters anders aufgebaut");
        par_free((void **)&par);
        return -1;
    }
    ed = F.malloc_((size_t)n + POLSTER);
    if (!ed) { par_free((void **)&par); snprintf(fehler, nf, "kein Speicher"); return -1; }
    memcpy(ed, asc, (size_t)n);
    memset(ed + n, 0, POLSTER);
    *(int *)par = 1;                    /* AVMEDIA_TYPE_AUDIO */
    *(int *)(par + 4) = 0x15002;        /* AV_CODEC_ID_AAC */
    *(unsigned char **)(par + 12) = ed;
    *(int *)(par + 16) = n;
    if (par_to_ctx(TCtx, par) < 0) { par_free((void **)&par); snprintf(fehler, nf, "parameters_to_context"); return -1; }
    par_free((void **)&par);            /* gibt auch ed frei, der Kontext hat eine Kopie */
    /* Abtastrate und Kanaele muss der Kontext selbst kennen, sonst gibt der Decoder Rahmen ohne beides aus
       ("An invalid frame was output by a decoder", AVERROR_BUG) */
    {
        char t[16];
        snprintf(t, sizeof t, "%d", rate);
        F.opt_set(TCtx, "ar", t, 0);
        F.opt_set(TCtx, "ch_layout", kanaele == 1 ? "mono" : "stereo", 0);
    }
    F.opt_set(TCtx, "threads", "1", 0);
    if (F.open2(TCtx, codec, NULL) < 0) { snprintf(fehler, nf, "avcodec_open2 (AAC)"); return -1; }
    TPkt = F.packet_alloc();
    TFrame = F.frame_alloc();
    if (!TPkt || !TFrame) { snprintf(fehler, nf, "kein Paket/Rahmen"); return -1; }
    return 0;
}

int dek_ton_schicken(unsigned char *p, int n)
{
    int r;
    if (F.from_data(TPkt, p, n) < 0) { F.free_(p); return -1; }
    r = F.send(TCtx, TPkt);
    F.packet_unref(TPkt);
    return r == NOCHMAL ? 0 : r;
}

int dek_ton_holen(Dek_ton *t)
{
    int r = F.recv(TCtx, TFrame), *w;
    float **daten;
    if (r == NOCHMAL) return 0;
    if (r < 0) return r;
    daten = (float **)TFrame;
    w = (int *)((char *)TFrame + 8 * sizeof(void *) + 8 * sizeof(int) + sizeof(void *));
    t->links = daten[0];
    t->rechts = daten[1] ? daten[1] : daten[0];
    t->anzahl = w[2];
    t->format = w[3];
    if (t->anzahl <= 0 || t->anzahl > 8192 || !t->links || t->format != 8) {   /* 8 = AV_SAMPLE_FMT_FLTP */
        F.frame_unref(TFrame);
        return -1;
    }
    return 1;
}

void dek_ton_fertig(void) { F.frame_unref(TFrame); }
