/* Bausteine der Apple-Kopplung (siehe kopplung.h).
   SRP wie srptools in Apples Variante: Zahlen in minimaler Byteform, nur PAD() auf Primzahlbreite. */
#include <stdio.h>
#include <string.h>
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include "kopplung.h"

static const char *N_HEX =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3BE39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";

static BIGNUM *N, *G, *KF;
static BN_CTX *CTX;
static unsigned char HXOR[64];
static int nHXOR;

/* Zahl als Bytes: minimal oder links auf 384 aufgefuellt */
static int bn_bytes(const BIGNUM *a, unsigned char *o, int pad)
{
    int n = BN_num_bytes(a);
    if (n == 0) { if (pad) { memset(o, 0, SRP_NB); return SRP_NB; } o[0] = 0; return 1; }
    if (pad && n < SRP_NB) { memset(o, 0, SRP_NB - n); BN_bn2bin(a, o + SRP_NB - n); return SRP_NB; }
    BN_bn2bin(a, o);
    return n;
}

static void grundlagen(void)
{
    unsigned char nb[SRP_NB], gb[SRP_NB], k[64], hn[64], hg[64], fuenf = 5, x[64];
    crypto_sha512_ctx c;
    int i, j;
    if (N) return;
    BN_hex2bn(&N, N_HEX);
    G = BN_new();
    BN_set_word(G, 5);
    CTX = BN_CTX_new();
    bn_bytes(N, nb, 1);
    bn_bytes(G, gb, 1);
    /* k = H(N | PAD(g)) */
    crypto_sha512_init(&c);
    crypto_sha512_update(&c, nb, SRP_NB);
    crypto_sha512_update(&c, gb, SRP_NB);
    crypto_sha512_final(&c, k);
    KF = BN_bin2bn(k, 64, NULL);
    /* H(N) xor H(g), minimal */
    crypto_sha512(hn, nb, SRP_NB);
    crypto_sha512(hg, &fuenf, 1);
    for (i = 0; i < 64; i++) x[i] = hn[i] ^ hg[i];
    for (j = 0; j < 63 && x[j] == 0; j++) ;
    nHXOR = 64 - j;
    memcpy(HXOR, x + j, nHXOR);
}

void srp_anfang(Srp *s, int pin, const unsigned char b[32], const unsigned char salz[16])
{
    unsigned char inner[64], hx[64];
    char ip[32];
    crypto_sha512_ctx c;
    BIGNUM *x, *t;
    int n;
    grundlagen();
    memset(s, 0, sizeof *s);
    memcpy(s->salz, salz, 16);
    /* x = H(s | H("Pair-Setup:" PIN)) */
    n = snprintf(ip, sizeof ip, "Pair-Setup:%d", pin);
    crypto_sha512(inner, (const unsigned char *)ip, n);
    crypto_sha512_init(&c);
    crypto_sha512_update(&c, salz, 16);
    crypto_sha512_update(&c, inner, 64);
    crypto_sha512_final(&c, hx);
    x = BN_bin2bn(hx, 64, NULL);
    s->v = BN_new();
    BN_mod_exp(s->v, G, x, N, CTX);
    /* B = (k v + g^b) mod N */
    s->b = BN_bin2bn(b, 32, NULL);
    s->B = BN_new();
    t = BN_new();
    BN_mod_exp(s->B, G, s->b, N, CTX);
    BN_mod_mul(t, KF, s->v, N, CTX);
    BN_mod_add(s->B, s->B, t, N, CTX);
    BN_clear_free(x);
    BN_free(t);
    crypto_wipe(hx, 64);
}

int srp_B(const Srp *s, unsigned char *aus) { return bn_bytes(s->B, aus, 0); }

int srp_A(Srp *s, const unsigned char *A, int n)
{
    unsigned char pa[SRP_NB], pb[SRP_NB], u[64], sb[SRP_NB], hi[64];
    crypto_sha512_ctx c;
    BIGNUM *a, *ub, *t, *S;
    int ns, nb;
    if (!s->B || n <= 0 || n > SRP_NB) return -1;
    a = BN_bin2bn(A, n, NULL);
    t = BN_new();
    BN_mod(t, a, N, CTX);
    if (BN_is_zero(t)) { BN_free(a); BN_free(t); return -1; }
    /* u = H(PAD(A) | PAD(B)) */
    bn_bytes(a, pa, 1);
    bn_bytes(s->B, pb, 1);
    crypto_sha512_init(&c);
    crypto_sha512_update(&c, pa, SRP_NB);
    crypto_sha512_update(&c, pb, SRP_NB);
    crypto_sha512_final(&c, u);
    ub = BN_bin2bn(u, 64, NULL);
    /* S = (A v^u)^b mod N, K = H(S) */
    S = BN_new();
    BN_mod_exp(t, s->v, ub, N, CTX);
    BN_mod_mul(t, a, t, N, CTX);
    BN_mod_exp(S, t, s->b, N, CTX);
    ns = bn_bytes(S, sb, 0);
    crypto_sha512(s->K, sb, ns);
    /* M = H(H(N) xor H(g) | H(I) | s | A | B | K) */
    s->nA = bn_bytes(a, s->A, 0);
    nb = bn_bytes(s->B, pb, 0);
    crypto_sha512(hi, (const unsigned char *)"Pair-Setup", 10);
    crypto_sha512_init(&c);
    crypto_sha512_update(&c, HXOR, nHXOR);
    crypto_sha512_update(&c, hi, 64);
    crypto_sha512_update(&c, s->salz, 16);
    crypto_sha512_update(&c, s->A, s->nA);
    crypto_sha512_update(&c, pb, nb);
    crypto_sha512_update(&c, s->K, 64);
    crypto_sha512_final(&c, s->M);
    s->bereit = 1;
    BN_free(a); BN_free(ub); BN_free(t); BN_clear_free(S);
    crypto_wipe(sb, sizeof sb);
    return 0;
}

int srp_beweis_ok(const Srp *s, const unsigned char *m, int n)
{
    return s->bereit && n == 64 && crypto_verify64(s->M, m) == 0;
}

void srp_hamk(const Srp *s, unsigned char aus[64])
{
    crypto_sha512_ctx c;
    crypto_sha512_init(&c);
    crypto_sha512_update(&c, s->A, s->nA);
    crypto_sha512_update(&c, s->M, 64);
    crypto_sha512_update(&c, s->K, 64);
    crypto_sha512_final(&c, aus);
}

void srp_frei(Srp *s)
{
    if (s->b) BN_clear_free(s->b);
    if (s->B) BN_free(s->B);
    if (s->v) BN_clear_free(s->v);
    crypto_wipe(s, sizeof *s);
}

/* --- TLV8 --- */
int tlv_hole(const unsigned char *d, int n, int typ, unsigned char *aus, int max)
{
    int p = 0, m = -1;
    while (p + 2 <= n) {
        int t = d[p], l = d[p + 1];
        if (p + 2 + l > n) l = n - p - 2;
        if (t == typ) {
            if (m < 0) m = 0;
            if (m + l > max) return -1;
            memcpy(aus + m, d + p + 2, l);
            m += l;
        }
        p += 2 + l;
    }
    return m;
}

void tlv_schreib(OPuffer *o, int typ, const unsigned char *d, int n)
{
    unsigned char k[2];
    int p = 0;
    do {
        int l = n - p > 255 ? 255 : n - p;
        k[0] = (unsigned char)typ;
        k[1] = (unsigned char)l;
        op_roh(o, k, 2);
        if (l) op_roh(o, d + p, l);
        p += l;
    } while (p < n);
}

/* --- HKDF-SHA512, 32 Bytes --- */
void hkdf32(unsigned char aus[32], const char *salz, const char *info, const unsigned char *ikm, int n)
{
    crypto_sha512_hkdf(aus, 32, ikm, n, (const unsigned char *)salz, strlen(salz),
                       (const unsigned char *)info, strlen(info));
}

/* --- ChaCha20-Poly1305 (IETF) --- */
void nonce_zaehler(unsigned char n[12], unsigned long long z)
{
    int i;
    for (i = 0; i < 8; i++) n[i] = (unsigned char)(z >> (8 * i));
    n[8] = n[9] = n[10] = n[11] = 0;
}

void nonce_text(unsigned char n[12], const char *t)
{
    int l = (int)strlen(t);
    if (l > 12) l = 12;
    memset(n, 0, 12);
    memcpy(n + 12 - l, t, l);
}

void chacha_zu(unsigned char *aus, const unsigned char k[32], const unsigned char n[12],
               const unsigned char *ad, int nad, const unsigned char *ein, int len)
{
    crypto_aead_ctx c;
    crypto_aead_init_ietf(&c, k, n);
    crypto_aead_write(&c, aus, aus + len, ad, nad, ein, len);
    crypto_wipe(&c, sizeof c);
}

int chacha_auf(unsigned char *aus, const unsigned char k[32], const unsigned char n[12],
               const unsigned char *ad, int nad, const unsigned char *ein, int len)
{
    crypto_aead_ctx c;
    int r;
    if (len < 16) return -1;
    crypto_aead_init_ietf(&c, k, n);
    r = crypto_aead_read(&c, aus, ein + len - 16, ad, nad, ein, len - 16);
    crypto_wipe(&c, sizeof c);
    return r;
}
