/*
    crypto.h
    The cryptography AerOS uses for passwords and encrypted files:

    - SHA-256: a 32 byte fingerprint of any data. Change one bit of the input and the hash
      changes completely, and there is no way back from a hash to the data.
    - HMAC-SHA-256: a hash that also depends on a secret key, so only someone with the key can
      make or check it. Used to detect tampering and wrong passwords.
    - PBKDF2: turns a password into a key by running HMAC thousands of times. That is slow on
      purpose: anyone trying to guess passwords has to pay that cost for every guess.
    - AES-256: the standard block cipher, used in counter (CTR) mode to encrypt files.
    - crypto_random: unpredictable bytes for salts and keys.

    These follow the published standards (FIPS 180-4, RFC 2104, RFC 8018, FIPS 197) and are
    checked against their official test vectors by crypto_selftest() at every boot.
*/
#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SHA256_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t length;      // bytes hashed so far
    uint8_t block[64];    // the block being filled
    uint32_t used;        // bytes in block
} sha256_t;

void sha256_init(sha256_t* s);
void sha256_update(sha256_t* s, const void* data, size_t length);
void sha256_final(sha256_t* s, uint8_t out[SHA256_SIZE]);
void sha256(const void* data, size_t length, uint8_t out[SHA256_SIZE]);

typedef struct {
    sha256_t inner; // already fed the key xor 0x36 block
    sha256_t outer; // already fed the key xor 0x5c block
} hmac_t;

void hmac_init(hmac_t* h, const void* key, size_t key_length);
void hmac_sha256(const void* key, size_t key_length, const void* data, size_t length, uint8_t out[SHA256_SIZE]);

// derive length bytes of key from a password and salt (RFC 8018 PBKDF2 with HMAC-SHA-256)
void pbkdf2_sha256(const void* password, size_t password_length, const void* salt, size_t salt_length,
                   uint32_t iterations, uint8_t* out, size_t length);

typedef struct {
    uint32_t round_keys[60];
} aes256_t;

void aes256_init(aes256_t* aes, const uint8_t key[32]);
void aes256_encrypt_block(const aes256_t* aes, const uint8_t in[16], uint8_t out[16]);
// encrypt or decrypt (the same operation) in counter mode: data is xored with
// AES(nonce || counter) for counter = 0, 1, 2 ...
void aes256_ctr(const aes256_t* aes, const uint8_t nonce[8], uint8_t* data, size_t length);

// fill buffer with unpredictable bytes
void crypto_random(void* buffer, size_t length);
// mix a little unpredictability into the random generator (called from interrupt handlers)
void crypto_add_entropy(uint32_t value);

// compare without stopping at the first difference, so timing doesn't reveal how much matched
bool crypto_equal(const void* a, const void* b, size_t length);
// overwrite secrets so they don't linger in memory
void crypto_wipe(void* buffer, size_t length);

// bytes <-> lower case hex text (out needs 2 * length + 1 bytes)
void crypto_to_hex(const uint8_t* bytes, size_t length, char* out);
bool crypto_from_hex(const char* hex, uint8_t* out, size_t length);

// check everything above against the official test vectors
bool crypto_selftest(void);

#endif /* CRYPTO_H */
