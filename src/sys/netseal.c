/*
    netseal.c - sealed messages (see netseal.h for the format)
*/

#include "netseal.h"
#include "../lib/crypto.h"
#include "../lib/primary_definitions.h"

#define MAGIC        "AERN"
#define MAGIC_SIZE   4
#define NONCE_SIZE   8
#define TAG_SIZE     SHA256_SIZE
#define ROUNDS       100000
#define SALT         "AerOS network"

// the two keys every message uses, both made from the shared key
typedef struct {
    uint8_t aes[32];
    uint8_t check[32];
} keys_t;

static void make_keys (const uint8_t key[NETSEAL_KEY_SIZE], keys_t* keys) {
    hmac_sha256(key, NETSEAL_KEY_SIZE, "AerOS net key", 13, keys->aes);
    hmac_sha256(key, NETSEAL_KEY_SIZE, "AerOS net mac", 13, keys->check);
}

void netseal_key (const char* password, uint8_t key[NETSEAL_KEY_SIZE]) {
    pbkdf2_sha256(password, slen(password), SALT, slen(SALT), ROUNDS, key, NETSEAL_KEY_SIZE);
}

size_t netseal_seal (const uint8_t key[NETSEAL_KEY_SIZE], const void* message, size_t length, uint8_t* out) {
    keys_t keys;
    make_keys(key, &keys);

    // "AERN", a new nonce, then the message scrambled with AES in counter mode
    memcpy(out, MAGIC, MAGIC_SIZE);
    uint8_t* nonce = out + MAGIC_SIZE;
    crypto_random(nonce, NONCE_SIZE);
    uint8_t* scrambled = nonce + NONCE_SIZE;
    memcpy(scrambled, message, length);
    aes256_t aes;
    aes256_init(&aes, keys.aes);
    aes256_ctr(&aes, nonce, scrambled, length);

    // and the tag over all of that
    size_t sealed_length = MAGIC_SIZE + NONCE_SIZE + length;
    hmac_sha256(keys.check, sizeof(keys.check), out, sealed_length, out + sealed_length);

    wipe(&aes, sizeof(aes));
    wipe(&keys, sizeof(keys));
    return sealed_length + TAG_SIZE;
}

bool netseal_open (const uint8_t key[NETSEAL_KEY_SIZE], const uint8_t* packet, size_t length,
                   uint8_t* out, size_t* out_length) {
    if (length < NETSEAL_OVERHEAD || memcmp(packet, MAGIC, MAGIC_SIZE) != 0) {
        return false;
    }
    keys_t keys;
    make_keys(key, &keys);

    // check the tag BEFORE unscrambling anything (crypto_equal takes the same time however
    // much of it matches, so the timing gives nothing away)
    size_t sealed_length = length - TAG_SIZE;
    uint8_t tag[TAG_SIZE];
    hmac_sha256(keys.check, sizeof(keys.check), packet, sealed_length, tag);
    bool genuine = crypto_equal(tag, packet + sealed_length, TAG_SIZE);

    if (genuine) {
        const uint8_t* nonce = packet + MAGIC_SIZE;
        size_t message_length = sealed_length - MAGIC_SIZE - NONCE_SIZE;
        memcpy(out, nonce + NONCE_SIZE, message_length);
        aes256_t aes;
        aes256_init(&aes, keys.aes);
        aes256_ctr(&aes, nonce, out, message_length);
        wipe(&aes, sizeof(aes));
        *out_length = message_length;
    }
    wipe(&keys, sizeof(keys));
    return genuine;
}
