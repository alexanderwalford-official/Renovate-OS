/*
    crypto.h
    The cryptography AerOS uses for passwords and encrypted files.

    - SHA-256: a 32 byte fingerprint ("digest") of any data. Change one bit of the input and
      the digest changes completely, and there is no way back from a digest to the data.
    - HMAC-SHA-256: a digest that also depends on a secret key, so only someone with the key
      can make or check it. Used to detect tampering and wrong passwords.
    - PBKDF2-HMAC-SHA-256: turns a password into a key by running HMAC thousands of times.
      That is slow on purpose: anyone guessing passwords has to pay that cost for every guess.
    - AES-256 in CTR mode: the standard cipher, used to encrypt file contents.
    - crypto_random: unpredictable bytes for salts and nonces.

    They follow the published standards (FIPS 180-4, RFC 2104, RFC 8018, FIPS 197 and NIST
    SP 800-38A for CTR) and crypto_selftest() checks them against the standards' test vectors
    at every boot. docs/SECURITY.md explains how accounts and encrypted files use them.

    None of this is safe to call from two places at once (there is only one CPU thread in AerOS),
    except crypto_add_entropy(), which interrupt handlers call.
*/
#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SHA256_SIZE 32 // bytes in a SHA-256 digest (and in an HMAC-SHA-256)

// ---- SHA-256 ----

// a hash in progress: data can be fed in with any number of sha256_update calls
typedef struct {
    uint32_t state[8];    // the eight working words, the digest once finished
    uint64_t length;      // bytes hashed so far (the padding needs it)
    uint8_t block[64];    // the block being filled
    uint32_t used;        // bytes in block
} sha256_t;

void sha256_init(sha256_t* s);
void sha256_update(sha256_t* s, const void* data, size_t length);
void sha256_final(sha256_t* s, uint8_t out[SHA256_SIZE]); // also wipes s
// all three in one go
void sha256(const void* data, size_t length, uint8_t out[SHA256_SIZE]);

// ---- HMAC-SHA-256 ----

// a key, prepared once so many HMACs with the same key are cheaper (PBKDF2 does tens of thousands)
typedef struct {
    sha256_t inner; // SHA-256 already fed the key xor 0x36 block
    sha256_t outer; // SHA-256 already fed the key xor 0x5c block
} hmac_t;

void hmac_init(hmac_t* h, const void* key, size_t key_length);
void hmac_sha256(const void* key, size_t key_length, const void* data, size_t length, uint8_t out[SHA256_SIZE]);

// ---- PBKDF2 ----

// derive length bytes of key from a password and salt, running HMAC "iterations" times per block
void pbkdf2_sha256(const void* password, size_t password_length, const void* salt, size_t salt_length,
                   uint32_t iterations, uint8_t* out, size_t length);

// ---- AES-256 ----

// a key expanded into its 15 round keys (aes256_init), wipe it when done
typedef struct {
    uint32_t round_keys[60];
} aes256_t;

void aes256_init(aes256_t* aes, const uint8_t key[32]);
void aes256_encrypt_block(const aes256_t* aes, const uint8_t in[16], uint8_t out[16]);
// encrypt or decrypt (it is the same operation) in counter mode: data is xor-ed with
// AES(nonce || counter) for counter = 0, 1, 2 ... A key and nonce pair must only ever be used once
void aes256_ctr(const aes256_t* aes, const uint8_t nonce[8], uint8_t* data, size_t length);

// ---- random numbers ----

// fill buffer with unpredictable bytes
void crypto_random(void* buffer, size_t length);
// mix a little unpredictability into the random generator (called from the keyboard and timer interrupts)
void crypto_add_entropy(uint32_t value);

// ---- helpers ----

// compare without stopping at the first difference, so timing doesn't reveal how much matched
bool crypto_equal(const void* a, const void* b, size_t length);
// overwrite secrets so they don't linger in memory (the compiler can't optimise it away)
void crypto_wipe(void* buffer, size_t length);

// bytes <-> lower case hex text (out needs 2 * length + 1 bytes), from_hex is false on a bad digit
void crypto_to_hex(const uint8_t* bytes, size_t length, char* out);
bool crypto_from_hex(const char* hex, uint8_t* out, size_t length);

// check everything above against the official test vectors, true if all of them match
bool crypto_selftest(void);

#endif /* CRYPTO_H */
