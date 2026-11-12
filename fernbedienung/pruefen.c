/* Prueft OPACK, SRP, HKDF und die Rahmenverschluesselung gegen die Vektoren aus atv-core. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "monocypher.h"
#include "opack.h"
#include "kopplung.h"

static int fehler;

static int hex(const char *h, unsigned char *o)
{
    int n = (int)strlen(h) / 2, i;
    for (i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); o[i] = (unsigned char)v; }
    return n;
}

static void gleich(const char *name, const unsigned char *a, int na, const char *erwartet)
{
    static unsigned char e[4096];
    int ne = hex(erwartet, e);
    int ok = na == ne && !memcmp(a, e, na);
    printf("%-34s %s\n", name, ok ? "richtig" : "FALSCH");
    if (!ok) fehler++;
}

static void ja(const char *name, int ok)
{
    printf("%-34s %s\n", name, ok ? "richtig" : "FALSCH");
    if (!ok) fehler++;
}

/* Baum als Text, um Lesen zu pruefen */
static void zeige(OPuffer *o, const OWert *w)
{
    char t[64];
    int i;
    if (!w) { op_roh(o, (const unsigned char *)"?", 1); return; }
    switch (w->art) {
        case O_NULL: snprintf(t, sizeof t, "null"); break;
        case O_BOOL: snprintf(t, sizeof t, "%s", w->i ? "true" : "false"); break;
        case O_INT: snprintf(t, sizeof t, "%lld", w->i); break;
        case O_FLOAT: snprintf(t, sizeof t, "%g", w->f); break;
        case O_STR: op_roh(o, (const unsigned char *)"'", 1); op_roh(o, w->p, w->n); snprintf(t, sizeof t, "'"); break;
        case O_DATA: case O_UUID: snprintf(t, sizeof t, "<%d>", w->n); break;
        case O_TIME: snprintf(t, sizeof t, "t%lld", w->i); break;
        case O_ARRAY:
            op_roh(o, (const unsigned char *)"[", 1);
            for (i = 0; i < w->m; i++) { if (i) op_roh(o, (const unsigned char *)",", 1); zeige(o, w->k[i]); }
            snprintf(t, sizeof t, "]");
            break;
        case O_DICT:
            op_roh(o, (const unsigned char *)"{", 1);
            for (i = 0; i < w->m; i++) {
                if (i) op_roh(o, (const unsigned char *)",", 1);
                zeige(o, w->k[2 * i]);
                op_roh(o, (const unsigned char *)":", 1);
                zeige(o, w->k[2 * i + 1]);
            }
            snprintf(t, sizeof t, "}");
            break;
    }
    op_roh(o, (const unsigned char *)t, (int)strlen(t));
}

static void lies(const char *name, const char *h, const char *erwartet)
{
    static unsigned char b[8192];
    int n = hex(h, b);
    OWert *w;
    OBaum *t = opack_lesen(b, n, &w);
    OPuffer o;
    op_init(&o);
    zeige(&o, w);
    op_roh(&o, (const unsigned char *)"", 1);
    o.b[o.n - 1] = 0;
    {
        int ok = !strcmp((char *)o.b, erwartet);
        printf("%-34s %s\n", name, ok ? "richtig" : "FALSCH");
        if (!ok) { printf("   bekommen: %s\n   erwartet: %s\n", o.b, erwartet); fehler++; }
    }
    op_frei(&o);
    opack_frei(t);
}

int main(void)
{
    OPuffer o;
    int i;

    /* --- OPACK schreiben (Faelle ohne Rueckverweise) --- */
    op_init(&o); op_null(&o); gleich("OPACK null", o.b, o.n, "04"); op_frei(&o);
    op_init(&o); op_array(&o, 2); op_bool(&o, 1); op_bool(&o, 0); gleich("OPACK bool", o.b, o.n, "d20102"); op_frei(&o);
    op_init(&o); op_array(&o, 3); op_int(&o, 0); op_int(&o, 1); op_int(&o, 39); gleich("OPACK kleine Zahlen", o.b, o.n, "d308092f"); op_frei(&o);
    op_init(&o); op_array(&o, 4); op_double(&o, 0.5); op_double(&o, -1.25); op_double(&o, 3.141592653589793); op_double(&o, 1e300);
    gleich("OPACK Kommazahlen", o.b, o.n, "d436000000000000e03f36000000000000f4bf36182d4454fb210940369c7500883ce4377e"); op_frei(&o);
    {
        char s[300];
        op_init(&o); op_array(&o, 4);
        op_str(&o, "");
        op_str(&o, "a");
        memset(s, 'x', 32); s[32] = 0; op_str(&o, s);
        memset(s, 'y', 33); s[33] = 0; op_str(&o, s);
        gleich("OPACK Texte", o.b, o.n, "d44041616078787878787878787878787878787878787878787878787878787878787878786121797979797979797979797979797979797979797979797979797979797979797979");
        op_frei(&o);
        op_init(&o); op_str(&o, "\xe9\x81\xa5\xe6\x8e\xa7\xe5\x99\xa8\xf0\x9f\x94\xa5"); gleich("OPACK Text UTF-8", o.b, o.n, "4de981a5e68ea7e599a8f09f94a5"); op_frei(&o);
    }
    {
        unsigned char d[512];
        for (i = 0; i < 512; i++) d[i] = (unsigned char)i;
        op_init(&o); op_array(&o, 3); op_data(&o, d, 0); op_data(&o, d, 1); op_data(&o, d, 32);
        gleich("OPACK Daten kurz", o.b, o.n, "d37071009000" "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
        op_frei(&o);
        op_init(&o); op_data(&o, d, 33);
        gleich("OPACK Daten 33", o.b, o.n, "9121000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20");
        op_frei(&o);
        op_init(&o); op_data(&o, d, 512);
        ja("OPACK Daten 512", o.n == 515 && o.b[0] == 0x92 && o.b[1] == 0 && o.b[2] == 2 && o.b[3] == 0 && o.b[514] == 0xff);
        op_frei(&o);
    }
    op_init(&o); op_array(&o, 6); op_int(&o, 1); op_str(&o, "two"); op_data(&o, (const unsigned char *)"\x03", 1); op_null(&o); op_bool(&o, 1); op_double(&o, 2.5);
    gleich("OPACK Liste", o.b, o.n, "d6094374776f71030401360000000000000440"); op_frei(&o);
    op_init(&o); op_array(&o, 20); for (i = 0; i < 20; i++) op_int(&o, i); op_ende(&o);
    gleich("OPACK Liste offen", o.b, o.n, "df08090a0b0c0d0e0f101112131415161718191a1b03"); op_frei(&o);
    op_init(&o);
    op_dict(&o, 2);
    op_str(&o, "a"); op_array(&o, 3); op_int(&o, 1);
    op_dict(&o, 1); op_str(&o, "b"); op_array(&o, 2); op_int(&o, 2); op_dict(&o, 1); op_str(&o, "c"); op_null(&o);
    op_str(&o, "not-a-tuple");
    op_str(&o, "d"); op_dict(&o, 1); op_str(&o, "e"); op_array(&o, 1); op_dict(&o, 1); op_str(&o, "f"); op_double(&o, 1.5);
    gleich("OPACK verschachtelt", o.b, o.n, "e24161d309e14162d20ae14163044b6e6f742d612d7475706c654164e14165d1e1416636000000000000f83f");
    op_frei(&o);
    op_init(&o); op_dict(&o, 16);
    for (i = 0; i < 16; i++) { char k[8]; snprintf(k, sizeof k, "key%02d", i); op_str(&o, k); op_int(&o, i * 7); }
    op_ende(&o);
    gleich("OPACK Woerterbuch offen", o.b, o.n, "ef456b6579303008456b657930310f456b6579303216456b657930331d456b6579303424456b657930352b456b65793036302a456b657930373031456b657930383038456b65793039303f456b657931303046456b65793131304d456b657931323054456b65793133305b456b657931343062456b65793135306903");
    op_frei(&o);
    op_init(&o); op_int(&o, -1); op_int(&o, 200); op_int(&o, 40000); op_int(&o, 3000000000LL);
    gleich("OPACK Zahlen mit Vorzeichen", o.b, o.n, "30ff31c80032409c0000" "33005ed0b200000000");
    op_frei(&o);

    /* --- OPACK lesen, auch mit Rueckverweisen --- */
    lies("OPACK lesen Grenzen", "d8302830ff31000131ffff320000010032ffffffff33000000000100000033ffffffffffffffff",
         "[40,-1,256,-1,65536,-1,4294967296,-1]");
    lies("OPACK lesen Woerterbuch", "e4425f69455f68696443425f740a425f63e2a109455f6842745309425f78313930",
         "{'_i':'_hidC','_t':2,'_c':{'_hidC':1,'_hBtS':1},'_x':12345}");
    lies("OPACK lesen Verweis kurz", "e2426b315572657065617465642d737472696e672d76616c7565426b32a1",
         "{'k1':'repeated-string-value','k2':'repeated-string-value'}");
    lies("OPACK lesen Verweis tief", "d2d353736861726564206279746573206f626a656374a0a0e14178a0",
         "[['shared bytes object','shared bytes object','shared bytes object'],{'x':'shared bytes object'}]");
    lies("OPACK lesen UUID", "055d797fd33538427ea47ba32fc6cf3a6a", "<16>");
    lies("OPACK lesen Liste", "d6094374776f71030401360000000000000440", "[1,'two',<1>,null,true,2.5]");
    lies("OPACK lesen offen", "ef456b6579303008456b657930310f03", "{'key00':0,'key01':7}");
    {
        /* langer Index: 40 Paare, dann 'repeat' -> Verweis a1 */
        OPuffer h; char k[8], w[24];
        op_init(&h);
        op_dict(&h, 41);
        for (i = 0; i < 40; i++) { snprintf(k, sizeof k, "k%02d", i); snprintf(w, sizeof w, "distinct-value-%02d", i); op_str(&h, k); op_str(&h, w); }
        op_str(&h, "repeat");
        op_roh(&h, (const unsigned char *)"\xa1\x03", 2);
        {
            OWert *r; OBaum *t = opack_lesen(h.b, h.n, &r);
            ja("OPACK lesen Verweis lang", r && r->m == 41 && !strcmp(opack_text(opack_hole(r, "repeat")), "distinct-value-00")
                                           && !strcmp(opack_text(opack_hole(r, "k39")), "distinct-value-39"));
            opack_frei(t);
        }
        op_frei(&h);
    }

    /* --- SRP (srptools-Vektor) --- */
    {
        Srp s;
        unsigned char b[32], salz[16], B[SRP_NB], A[SRP_NB], M[64], h[64];
        int nB, nA, nM;
        memset(b, 0xaa, 32);
        hex("00112233445566778899aabbccddeeff", salz);
        srp_anfang(&s, 1111, b, salz);
        nB = srp_B(&s, B);
        gleich("SRP B", B, nB, "60436d941aaa598a0f600edc71bb2d8a2e6336d5cd931a13d30c99c3167e863b09b26750f433c5ef5865f57d63b3d2faaadd9ddaf0514f91833db5740f1981f07439b3b8d253ff8ad318f897803cf9a6e362d509be9b11dd76926a947f5c9263be1b9706e50102e527af8cb29deb24a69f355e62229b6e09c41a31e116c780011817ffb3b44b8ab5b895a73887676f7636d2aa85b8ca6f9fa33622678ef752498857b7fa5fd9f57307ce3cc637fcc3d4f45d71524955d71901344123f9099495cb3ec1df716d60230a06649f0e33a5dd8b81207630d498fb7f21663b53f762c2f0ba8d09ea8e7be09aa487843c47e30e87e6b7ecc47d667ed4b71ab470f95ee9596f34f655eead21d54ae77f022557ba12fb5e688b40b9fc83384be8bdbbff6462a8982cb6b3f644b36b38fd52f99084e53e3d3014c15286b377e47fa4f637cc781d0339a2a7df9130a6b71e162816a10d3ef539f06dc54d41f50de56bc91c69f52e7112547e4f9709c693820f25365f55f1b429d60167a87e7e4607ae0849cd");
        nA = hex("bc0e7cf5dc3babf67dcedbb3b140aacc6cac43f4336b43bbd5de48d6ea7c8eda66924e354255225bccad9debe21182e6bb050f3ff3e6cfbb62c229379968c70ca436ad649a0b051373184215eef046f6f1f2256838f958581f6c7b2b85fa4afe326a0e8a951d4489305331aff88a136fd8d108bcc95fceb7e557c889c828bd23fb0702f053e1ca6470fb3c76bce4843fc005c7ea675740f8550212656cfc8919d9db805a434a68229e0d9dfe43fc16dc680a5ce74b77cf374353b05759bc1da3a9dabde30a4209381c87ca83d9483abdf66b86f9b1cbda9ad82c62712b87ce6fb7069b8fc8df344261821a06d0dc5106af76d4245f3f7737a94dbc484b415555dc401842d3011204553ba9f611b02bc38de26eba1a76bf8350205a62c436ba1c3c7c69d59318bd107fd1c1f5d846b3142e85a5d49e522655e020ed1bfe1e186cf923bf328f0b9b4c6a8aa3266ed9125bb98d63827110713be7803122ee4603c54ea31863ce4b10aff31f9073cf63b94733b4f066e72d4ec35687047d5d0db160", A);
        ja("SRP A verarbeiten", srp_A(&s, A, nA) == 0);
        nM = hex("609fc3b240f3b8e8932fc26b1ce3a9a901a1c6024239e5618d086a46c8709d8b635524d90c41f6c465626128242151ca32f57f5a6a14dea074e4cbe08832bd03", M);
        ja("SRP Beweis M1", srp_beweis_ok(&s, M, nM));
        memset(M, 0, 64);
        ja("SRP falscher Beweis abgelehnt", !srp_beweis_ok(&s, M, 64));
        srp_hamk(&s, h);
        gleich("SRP H(A|M|K)", h, 64, "be9f8aa7f23a0826316277b2dc767d1dae1490c28e4f6c9859347ea67331f8ee1cd445ebc982a9f773c8bc2c3dbfc73271ec4e6245d8e3a7e5104503fff27e57");
        gleich("SRP Schluessel K", s.K, 64, "43cfd784738cbb616f7d1b921b1cfd7df0b0f6b6377c89aad7ac070d86b8b3663729cf32db89c33fa7500fd9977ed262becbaeb29f90580777acb4e622f0a85a");
        srp_frei(&s);
    }

    /* --- HKDF --- */
    {
        unsigned char g[64], k[32];
        for (i = 0; i < 64; i++) g[i] = (unsigned char)i;
        hkdf32(k, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", g, 64);
        gleich("HKDF Pair-Verify", k, 32, "8f806335e1847b59c5004b131c4689e951ce9808993aed540856475e0dee353b");
        hkdf32(k, "", "ServerEncrypt-main", g, 64);
        gleich("HKDF ServerEncrypt-main", k, 32, "43020386f29e4f8016eadaf9233973760ec466e2af1668eb85f31e4d90231649");
    }

    /* --- Rahmenverschluesselung mit Zaehler --- */
    {
        unsigned char ka[32], ke[32], n[12], kopf[4], ein[256], aus[256], zurueck[256];
        const char *koerper[3] = { "7b225f69223a225f68696443227d", "e3425f69455f68696443425f740a425f63e2a109455f6842745309", NULL };
        const char *kopfe[3] = { "0800001e", "0800002b", "08000074" };
        const char *zu[3] = { "639a1d588fdc848e7b0838228d3ef0d3a815a78969c8f73f1ab9b3078947",
                              "777d248781bcf443131891ac19359f04425a51d168a6b17fe113fe14f0c5806fa93c695e074d466e0af007",
                              "8feae02842e728362c58729271d52803dfef493a3f654da549020b052395bfa2ffbebd50df7ebed12229cfc03d570be13511ed612eaba962a2c0bcbc585cff39d3dce7bfdfd5bf6351dd39d5e6dd844fa9b403558e6a34ed86b8c945122af8357d30cefb0d71f305ac2f1899655c57766293c0a2" };
        const char *auf[3] = { "fb4100b531b2f542f2419da2c7f478456b4ec04fba8bdb3419744e6389e0",
                               "26ec4e5547aae3f18fd5bef9b8459b5dca6a46bd82548deb9786f0d1b2d940781df26f6ad9e8f970ee3a26",
                               "93a73cb588876c4628b0fafff0f8def58b38708abbe832c9d8faf236ba45f6492715a57f0e2456d49376caf6efa795c45c52de2f94cb39bd5f8a54347a9c1eb86d866d0998159961c06e75f712cf2c8eed94307410066da5def385d396c21727f4dd57258100a81b31c7a45a770c4fef45e91dbb" };
        for (i = 0; i < 32; i++) { ka[i] = (unsigned char)i; ke[i] = (unsigned char)(32 + i); }
        for (i = 0; i < 3; i++) {
            int ne, nc;
            char name[48];
            if (koerper[i]) ne = hex(koerper[i], ein); else { ne = 100; memset(ein, 0, ne); }
            hex(kopfe[i], kopf);
            nonce_zaehler(n, i);
            chacha_zu(aus, ka, n, kopf, 4, ein, ne);
            snprintf(name, sizeof name, "Rahmen %d verschluesseln", i);
            gleich(name, aus, ne + 16, zu[i]);
            nc = hex(auf[i], aus);
            snprintf(name, sizeof name, "Rahmen %d entschluesseln", i);
            ja(name, chacha_auf(zurueck, ke, n, kopf, 4, aus, nc) == 0 && !memcmp(zurueck, ein, ne));
        }
        hex("0109636c69656e742d69640320000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", ein);
        nonce_text(n, "PS-Msg05");
        chacha_zu(aus, ke, n, NULL, 0, ein, 45);
        gleich("PS-Msg05 verschluesseln", aus, 61, "198f7ba9f7542940bae690be2ab48d4fcba60ebded08c2ada6fadd22b21bb728ffcf12fcfabcacca79eaebde164ade58e30dac75b4026063287b26db6e");
        ja("PS-Msg05 entschluesseln", chacha_auf(zurueck, ke, n, NULL, 0, aus, 61) == 0 && !memcmp(zurueck, ein, 45));
        aus[3] ^= 1;
        ja("verfaelschter Rahmen abgelehnt", chacha_auf(zurueck, ke, n, NULL, 0, aus, 61) != 0);
    }

    /* --- TLV8 mit Stuecken --- */
    {
        unsigned char d[600], z[600];
        int n;
        for (i = 0; i < 600; i++) d[i] = (unsigned char)(i * 7);
        op_init(&o);
        tlv_schreib(&o, 6, (const unsigned char *)"\x02", 1);
        tlv_schreib(&o, 3, d, 600);
        tlv_schreib(&o, 0x1b, (const unsigned char *)"\x01", 1);
        n = tlv_hole(o.b, o.n, 3, z, sizeof z);
        ja("TLV8 Stuecke", o.n == 3 + 606 + 3 && o.b[3] == 3 && o.b[4] == 255 && n == 600 && !memcmp(d, z, 600));
        op_frei(&o);
    }

    printf("%s\n", fehler ? "FEHLER" : "ALLES RICHTIG");
    return fehler != 0;
}
