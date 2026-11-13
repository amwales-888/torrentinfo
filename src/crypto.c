/*
 * crypto.c - Diffie-Hellman and RC4 for protocol encryption. See crypto.h.
 */
#include <string.h>
#include <openssl/bn.h>
#include <uv.h>

#include "crypto.h"

/* The 768-bit MSE prime (the Oakley group 1 prime of RFC 2409). */
static const uint8_t cryptoPrime[CRYPTO_DH_LENGTH] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC9, 0x0F, 0xDA, 0xA2,
    0x21, 0x68, 0xC2, 0x34, 0xC4, 0xC6, 0x62, 0x8B, 0x80, 0xDC, 0x1C, 0xD1,
    0x29, 0x02, 0x4E, 0x08, 0x8A, 0x67, 0xCC, 0x74, 0x02, 0x0B, 0xBE, 0xA6,
    0x3B, 0x13, 0x9B, 0x22, 0x51, 0x4A, 0x08, 0x79, 0x8E, 0x34, 0x04, 0xDD,
    0xEF, 0x95, 0x19, 0xB3, 0xCD, 0x3A, 0x43, 0x1B, 0x30, 0x2B, 0x0A, 0x6D,
    0xF2, 0x5F, 0x14, 0x37, 0x4F, 0xE1, 0x35, 0x6D, 0x6D, 0x51, 0xC2, 0x45,
    0xE4, 0x85, 0xB5, 0x76, 0x62, 0x5E, 0x7E, 0xC6, 0xF4, 0x4C, 0x42, 0xE9,
    0xA6, 0x3A, 0x36, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x05, 0x63
};

int cryptoRandom(void *buffer, size_t length)
{
    return (uv_random(NULL, NULL, buffer, length, 0, NULL) == 0) ? 0 : -1;
}

/* base^exponent mod P into out (96 bytes, big-endian). */
static int modExp(const BIGNUM *base, const uint8_t *exponent, size_t exponentLength,
                  uint8_t out[CRYPTO_DH_LENGTH], int checkBase)
{
    BN_CTX *bnContext = BN_CTX_new();
    BIGNUM *prime = BN_bin2bn(cryptoPrime, (int)sizeof(cryptoPrime), NULL);
    BIGNUM *power = BN_bin2bn(exponent, (int)exponentLength, NULL);
    BIGNUM *result = BN_new();
    BIGNUM *limit = NULL;
    int     ok = 0;

    if (bnContext != NULL && prime != NULL && power != NULL && result != NULL) {
        ok = 1;
        if (checkBase) {
            /* The peer's key must be in [2, P-2]. */
            limit = BN_dup(prime);
            ok = limit != NULL && BN_sub_word(limit, 1) &&
                 BN_cmp(base, limit) < 0 && !BN_is_zero(base) && !BN_is_one(base);
        }
        ok = ok && BN_mod_exp(result, base, power, prime, bnContext) &&
             BN_bn2binpad(result, out, (int)CRYPTO_DH_LENGTH) == (int)CRYPTO_DH_LENGTH;
    }
    BN_free(limit);
    BN_free(result);
    BN_clear_free(power);
    BN_free(prime);
    BN_CTX_free(bnContext);
    return ok ? 0 : -1;
}

int cryptoDhKeyPair(uint8_t privateKey[CRYPTO_PRIVATE_LENGTH],
                    uint8_t publicKey[CRYPTO_DH_LENGTH])
{
    BIGNUM *two = BN_new();
    int     result = -1;

    if (two != NULL && BN_set_word(two, 2) && cryptoRandom(privateKey, CRYPTO_PRIVATE_LENGTH) == 0) {
        result = modExp(two, privateKey, CRYPTO_PRIVATE_LENGTH, publicKey, 0);
    }
    BN_free(two);
    return result;
}

int cryptoDhSecret(const uint8_t privateKey[CRYPTO_PRIVATE_LENGTH],
                   const uint8_t peerPublic[CRYPTO_DH_LENGTH],
                   uint8_t secret[CRYPTO_DH_LENGTH])
{
    BIGNUM *base = BN_bin2bn(peerPublic, (int)CRYPTO_DH_LENGTH, NULL);
    int     result = -1;

    if (base != NULL) {
        result = modExp(base, privateKey, CRYPTO_PRIVATE_LENGTH, secret, 1);
    }
    BN_free(base);
    return result;
}

void cryptoRc4Init(cryptoRc4 *rc4, const uint8_t *key, size_t keyLength, uint32_t discard)
{
    uint8_t  j = 0;
    uint8_t  swap;
    uint8_t  scratch[256];
    uint32_t i;

    for (i = 0; i < 256u; i++) {
        rc4->s[i] = (uint8_t)i;
    }
    for (i = 0; i < 256u; i++) {
        j = (uint8_t)(j + rc4->s[i] + key[i % keyLength]);
        swap = rc4->s[i];
        rc4->s[i] = rc4->s[j];
        rc4->s[j] = swap;
    }
    rc4->i = 0;
    rc4->j = 0;
    while (discard > 0) {
        uint32_t chunk = (discard < sizeof(scratch)) ? discard : (uint32_t)sizeof(scratch);

        cryptoRc4Apply(rc4, scratch, chunk);
        discard -= chunk;
    }
}

void cryptoRc4Apply(cryptoRc4 *rc4, uint8_t *data, size_t length)
{
    uint8_t i = rc4->i;
    uint8_t j = rc4->j;
    uint8_t swap;
    size_t  n;

    for (n = 0; n < length; n++) {
        i = (uint8_t)(i + 1u);
        j = (uint8_t)(j + rc4->s[i]);
        swap = rc4->s[i];
        rc4->s[i] = rc4->s[j];
        rc4->s[j] = swap;
        data[n] ^= rc4->s[(uint8_t)(rc4->s[i] + rc4->s[j])];
    }
    rc4->i = i;
    rc4->j = j;
}
