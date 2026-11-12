/* Bausteine der Apple-Kopplung (Companion): SRP-6a 3072/SHA-512, TLV8, HKDF, ChaCha20-Poly1305 */
#ifndef KOPPLUNG_H
#define KOPPLUNG_H
#include <openssl/bn.h>
#include "opack.h"

#define SRP_NB 384

typedef struct {
    BIGNUM *b, *B, *v;
    unsigned char salz[16];
    unsigned char K[64], M[64];
    unsigned char A[SRP_NB];
    int nA, bereit;
} Srp;

void srp_anfang(Srp *s, int pin, const unsigned char b[32], const unsigned char salz[16]);
int srp_B(const Srp *s, unsigned char *aus);                     /* minimale Bytes, gibt Laenge */
int srp_A(Srp *s, const unsigned char *A, int n);                /* 0: gut */
int srp_beweis_ok(const Srp *s, const unsigned char *m, int n);
void srp_hamk(const Srp *s, unsigned char aus[64]);
void srp_frei(Srp *s);

/* TLV8: alle Stuecke eines Typs aneinander; Laenge oder -1 */
int tlv_hole(const unsigned char *d, int n, int typ, unsigned char *aus, int max);
void tlv_schreib(OPuffer *o, int typ, const unsigned char *d, int n);

void hkdf32(unsigned char aus[32], const char *salz, const char *info, const unsigned char *ikm, int n);

void nonce_zaehler(unsigned char n[12], unsigned long long z);  /* LE64 + 4 Nullbytes */
void nonce_text(unsigned char n[12], const char *t);            /* rechtsbuendig, links Nullen */
void chacha_zu(unsigned char *aus, const unsigned char k[32], const unsigned char n[12],
               const unsigned char *ad, int nad, const unsigned char *ein, int len);   /* aus: len + 16 */
int chacha_auf(unsigned char *aus, const unsigned char k[32], const unsigned char n[12],
               const unsigned char *ad, int nad, const unsigned char *ein, int len);   /* len mit Marke; 0: gut */

#endif
