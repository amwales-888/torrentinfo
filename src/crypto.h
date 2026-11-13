/*
 * crypto.h - The pieces of BitTorrent protocol encryption (MSE/PE).
 *
 * Message Stream Encryption agrees a key with Diffie-Hellman over the
 * 768-bit prime below (generator 2), then encrypts with RC4, discarding the
 * first 1024 bytes of each key stream. This is obfuscation, not security:
 * it hides BitTorrent traffic from simple traffic shaping, nothing more.
 *
 * The Diffie-Hellman arithmetic uses OpenSSL's BIGNUM functions. RC4 is
 * implemented here (OpenSSL 3 keeps it only in its "legacy" provider).
 */
#ifndef CRYPTO_H
#define CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CRYPTO_DH_LENGTH        96u     /* public keys and the shared secret */
#define CRYPTO_PRIVATE_LENGTH   20u     /* 160-bit private keys              */

typedef struct cryptoRc4 {
    uint8_t s[256];
    uint8_t i;
    uint8_t j;
} cryptoRc4;

/* Random bytes from the operating system. 0 on success. */
int cryptoRandom(void *buffer, size_t length);

/* A new key pair: a random private key and 2^private mod P. 0 on success. */
int cryptoDhKeyPair(uint8_t privateKey[CRYPTO_PRIVATE_LENGTH],
                    uint8_t publicKey[CRYPTO_DH_LENGTH]);

/*
 * The shared secret peerPublic^privateKey mod P, as 96 big-endian bytes.
 * -1 if the peer's key is not a sensible value (0, 1 or P-1 and above, which
 * would make the secret guessable).
 */
int cryptoDhSecret(const uint8_t privateKey[CRYPTO_PRIVATE_LENGTH],
                   const uint8_t peerPublic[CRYPTO_DH_LENGTH],
                   uint8_t secret[CRYPTO_DH_LENGTH]);

/* RC4 with the given key, its first 'discard' bytes of key stream thrown away. */
void cryptoRc4Init(cryptoRc4 *rc4, const uint8_t *key, size_t keyLength, uint32_t discard);

/* Encrypt or decrypt (the same thing) in place. */
void cryptoRc4Apply(cryptoRc4 *rc4, uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* CRYPTO_H */
