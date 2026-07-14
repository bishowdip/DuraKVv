/*
 * crypto.c - the only crypto in the project, wrapped so nothing else
 * touches raw libsodium.
 * - seal/open = XChaCha20-Poly1305 AEAD: hides the plaintext AND detects
 *   tampering (poly1305 tag fails on any modified byte).
 * - Argon2id for passwords/keys: memory hard, so gpu farms cant cheaply
 *   brute force it like a plain hash.
 */
#include "crypto.h"

#include <sodium.h>
#include <string.h>

/* Initialise libsodium (seeds the RNG, selects CPU-optimised implementations).
 * Safe to call more than once. Returns -1 only on a genuine init failure. */
int crypto_init(void)
{
    return sodium_init() < 0 ? -1 : 0;     /* idempotent; -1 only on failure */
}

/* random salt from the OS csprng. per-db salt = same password still gives
 * different keys on different dbs (kills rainbow tables). */
void crypto_random_salt(uint8_t salt[CRYPTO_SALTBYTES])
{
    randombytes_buf(salt, CRYPTO_SALTBYTES);
}

/* password + salt -> key (argon2id). deterministic: same inputs = same key,
 * which is how the right password unlocks an existing db. */
int crypto_derive_key(uint8_t key[CRYPTO_KEYBYTES], const char *password,
                      const uint8_t salt[CRYPTO_SALTBYTES])
{
    return crypto_pwhash(key, CRYPTO_KEYBYTES, password, strlen(password), salt,
                         crypto_pwhash_OPSLIMIT_INTERACTIVE,
                         crypto_pwhash_MEMLIMIT_INTERACTIVE,
                         crypto_pwhash_ALG_ARGON2ID13);
}

/* encrypt+tag into out = [24B nonce][cipher+16B tag]. nonce is stored in
 * the clear -- it only has to be unique, not secret. */
size_t crypto_seal(uint8_t *out, const uint8_t *plain, size_t plen,
                   const uint8_t key[CRYPTO_KEYBYTES])
{
    uint8_t *nonce = out;                          /* layout: [nonce][cipher] */
    uint8_t *cipher = out + CRYPTO_NONCEBYTES;
    randombytes_buf(nonce, CRYPTO_NONCEBYTES);     /* fresh nonce every time  */

    unsigned long long clen = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        cipher, &clen, plain, plen, NULL, 0, NULL, nonce, key);
    return CRYPTO_NONCEBYTES + (size_t)clen;
}

/* verify+decrypt. -1 for too-short, tampered, or wrong key -- deliberately
 * indistinguishable, the tag check just fails. */
long crypto_open(uint8_t *out, const uint8_t *in, size_t inlen,
                 const uint8_t key[CRYPTO_KEYBYTES])
{
    if (inlen < CRYPTO_SEAL_OVERHEAD) return -1;
    const uint8_t *nonce  = in;
    const uint8_t *cipher = in + CRYPTO_NONCEBYTES;
    size_t clen = inlen - CRYPTO_NONCEBYTES;

    unsigned long long mlen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            out, &mlen, NULL, cipher, clen, NULL, 0, nonce, key) != 0)
        return -1;                                  /* tampered or wrong key */
    return (long)mlen;
}

/* Produce a self-describing Argon2id hash string (embeds algorithm, params and
 * salt) suitable for storing in a user record. */
int crypto_password_hash(char out[CRYPTO_PWHASH_STRBYTES], const char *password)
{
    return crypto_pwhash_str(out, password, strlen(password),
                             crypto_pwhash_OPSLIMIT_INTERACTIVE,
                             crypto_pwhash_MEMLIMIT_INTERACTIVE);
}

/* verify against the stored hash. constant time, leaks nothing. 0 = match. */
int crypto_password_verify(const char *hash_str, const char *password)
{
    return crypto_pwhash_str_verify(hash_str, password, strlen(password)) == 0 ? 0 : -1;
}
