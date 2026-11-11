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

// Make the AES key and the check key from the shared key. Two different keys, so the one that
// scrambles never also signs: each is HMAC of the shared key over its own label (13 is the
// labels' length). Cheap, unlike netseal_key, so it is done afresh for every message
static void make_keys (const uint8_t key[NETSEAL_KEY_SIZE], keys_t* keys) {
    hmac_sha256(key, NETSEAL_KEY_SIZE, "AerOS net key", 13, keys->aes);
    hmac_sha256(key, NETSEAL_KEY_SIZE, "AerOS net mac", 13, keys->check);
}

// password -> shared key, with PBKDF2: 100 000 rounds of HMAC-SHA-256, so it takes a moment
// once, but guessing passwords one after another takes far longer. The salt is fixed rather
// than random because both ends must arrive at the same key from the password alone
void netseal_key (const char* password, uint8_t key[NETSEAL_KEY_SIZE]) {
    pbkdf2_sha256(password, slen(password), SALT, slen(SALT), ROUNDS, key, NETSEAL_KEY_SIZE);
}

// seal a message: "AERN", a new nonce, the message scrambled, and the tag (see netseal.h)
size_t netseal_seal (const uint8_t key[NETSEAL_KEY_SIZE], const void* message, size_t length, uint8_t* out) {
    keys_t keys;
    make_keys(key, &keys);

    // "AERN", a new nonce, then the message scrambled with AES in counter mode
    //
    // Counter mode turns the nonce (and a counter) into a stream of random looking bytes, and
    // XORs the message with it: scrambling and unscrambling are the same step, and the
    // scrambled message is exactly as long as the message. A nonce must never be used twice
    // with the same key, which is why it is 8 fresh random bytes every time
    memcpy(out, MAGIC, MAGIC_SIZE);
    uint8_t* nonce = out + MAGIC_SIZE;
    crypto_random(nonce, NONCE_SIZE);
    uint8_t* scrambled = nonce + NONCE_SIZE;
    memcpy(scrambled, message, length);
    aes256_t aes;
    aes256_init(&aes, keys.aes);
    aes256_ctr(&aes, nonce, scrambled, length);

    // and the tag over all of that: "AERN", the nonce and the scrambled message, so changing
    // any byte of them breaks it. It goes on the end, straight after what it covers
    size_t sealed_length = MAGIC_SIZE + NONCE_SIZE + length;
    hmac_sha256(keys.check, sizeof(keys.check), out, sealed_length, out + sealed_length);

    // the keys and AES's expanded key are on the stack: don't leave them there
    wipe(&aes, sizeof(aes));
    wipe(&keys, sizeof(keys));
    return sealed_length + TAG_SIZE;
}

// open a sealed packet into out: false, and nothing in out, unless its tag matches this key
bool netseal_open (const uint8_t key[NETSEAL_KEY_SIZE], const uint8_t* packet, size_t length,
                   uint8_t* out, size_t* out_length) {
    // At least "AERN", a nonce and a tag (an empty message is allowed), starting with "AERN".
    // Without the length check, length - TAG_SIZE below could wrap round to a huge number
    if (length < NETSEAL_OVERHEAD || memcmp(packet, MAGIC, MAGIC_SIZE) != 0) {
        return false;
    }
    keys_t keys;
    make_keys(key, &keys);

    // check the tag BEFORE unscrambling anything (crypto_equal takes the same time however
    // much of it matches, so the timing gives nothing away)
    //
    // The tag is worked out again from what arrived, the same way netseal_seal did, and
    // compared with the last 32 bytes of the packet
    size_t sealed_length = length - TAG_SIZE;
    uint8_t tag[TAG_SIZE];
    hmac_sha256(keys.check, sizeof(keys.check), packet, sealed_length, tag);
    bool genuine = crypto_equal(tag, packet + sealed_length, TAG_SIZE);

    // genuine: copy the scrambled message out and run counter mode over it again with the
    // same nonce, which gives back the message itself
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
