/*
    fcrypt.c - password protected files (see fcrypt.h for the file format, and docs/SECURITY.md)

    Encrypting ("sealing") a file:

        password --+
                   +--> PBKDF2 (100000 rounds) --> master key --+--> HMAC(master, "AerOS file key") --> AES key
        salt ------+    (salt: 16 random bytes)                 +--> HMAC(master, "AerOS file mac") --> MAC key

        contents --> AES-256-CTR(AES key, nonce) --> encrypted contents
        header || encrypted contents --> HMAC-SHA-256(MAC key) --> tag

        file = header || encrypted contents || tag

    Opening it again runs the same key steps with the salt from the header, checks the tag, and
    only if the tag matches decrypts the contents. This order ("encrypt then MAC") means nothing
    is ever decrypted from a file that was changed or a password that was wrong.

    Why two keys? Using one key for two different jobs (encrypting and checking) is a classic way
    for a weakness in one to leak into the other, so the master key is split into two that have
    nothing to do with each other. The labels make sure the two can never come out the same.
*/

#include "fcrypt.h"
#include "../lib/primary_definitions.h"
#include "../lib/crypto.h"
#include "f_sys.h"

#define MAGIC          "AERENC01" // "AerOS encrypted, format 01"
#define MAGIC_SIZE     8
#define SALT_SIZE      16
#define NONCE_SIZE     8
#define TAG_SIZE       SHA256_SIZE

// where each part of the header starts (see fcrypt.h for the picture)
#define OFFSET_ROUNDS  MAGIC_SIZE                  // 8
#define OFFSET_SALT    (OFFSET_ROUNDS + 4)         // 12
#define OFFSET_NONCE   (OFFSET_SALT + SALT_SIZE)   // 28
#define HEADER_SIZE    (OFFSET_NONCE + NONCE_SIZE) // 36, the contents start here

#define ROUNDS         100000  // PBKDF2 rounds for newly encrypted files (stored in each file)
#define ROUNDS_MAX     5000000 // refuse a header asking for more, or a damaged file could hang the OS

typedef struct {
    uint8_t aes[32]; // encrypts the contents
    uint8_t mac[32]; // makes and checks the tag
} keys_t;

// password + salt -> master key (PBKDF2, the slow part) -> the AES key and the MAC key
static void derive_keys (const char* password, const uint8_t* salt, uint32_t rounds, keys_t* keys) {
    uint8_t master[SHA256_SIZE];
    pbkdf2_sha256(password, slen(password), salt, SALT_SIZE, rounds, master, sizeof(master));
    hmac_sha256(master, sizeof(master), "AerOS file key", 14, keys->aes);
    hmac_sha256(master, sizeof(master), "AerOS file mac", 14, keys->mac);
    wipe(master, sizeof(master));
}

// the rounds count is stored little endian, like the rest of the PC
static uint32_t read_u32 (const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_u32 (uint8_t* p, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(value >> (8 * i));
    }
}

bool fcrypt_is_sealed (const void* data, uint32_t length) {
    return length >= HEADER_SIZE + TAG_SIZE && memcmp(data, MAGIC, MAGIC_SIZE) == 0;
}

int fcrypt_seal (const void* plain, uint32_t length, const char* password, uint8_t** out, uint32_t* out_length) {
    if (password[0] == '\0') {
        return FCRYPT_ERR_NO_PASSWORD;
    }
    uint32_t total = HEADER_SIZE + length + TAG_SIZE;
    uint8_t* sealed = malloc(total);
    if (sealed == 0) {
        return FCRYPT_ERR_NO_MEMORY;
    }

    // the header: magic, rounds, and a fresh random salt and nonce
    memcpy(sealed, MAGIC, MAGIC_SIZE);
    write_u32(sealed + OFFSET_ROUNDS, ROUNDS);
    crypto_random(sealed + OFFSET_SALT, SALT_SIZE);
    crypto_random(sealed + OFFSET_NONCE, NONCE_SIZE);

    keys_t keys;
    aes256_t aes;
    derive_keys(password, sealed + OFFSET_SALT, ROUNDS, &keys);
    aes256_init(&aes, keys.aes);

    // encrypt a copy of the contents in place, then tag the header and the encrypted contents
    uint8_t* contents = sealed + HEADER_SIZE;
    memcpy(contents, plain, length);
    aes256_ctr(&aes, sealed + OFFSET_NONCE, contents, length);
    hmac_sha256(keys.mac, sizeof(keys.mac), sealed, HEADER_SIZE + length, contents + length);

    wipe(&keys, sizeof(keys));
    wipe(&aes, sizeof(aes));
    *out = sealed;
    *out_length = total;
    return FCRYPT_OK;
}

int fcrypt_open (const void* sealed_data, uint32_t length, const char* password, uint8_t** out, uint32_t* out_length) {
    const uint8_t* sealed = sealed_data;
    if (!fcrypt_is_sealed(sealed, length)) {
        return FCRYPT_ERR_NOT_ENCRYPTED;
    }
    uint32_t rounds = read_u32(sealed + OFFSET_ROUNDS);
    if (rounds == 0 || rounds > ROUNDS_MAX) {
        return FCRYPT_ERR_DAMAGED;
    }
    uint32_t plain_length = length - HEADER_SIZE - TAG_SIZE;
    const uint8_t* stored_tag = sealed + HEADER_SIZE + plain_length;

    keys_t keys;
    derive_keys(password, sealed + OFFSET_SALT, rounds, &keys);

    // check the tag first: only decrypt what is exactly what was encrypted, with this password.
    // A wrong password gives different keys, so the tag doesn't match either
    uint8_t tag[TAG_SIZE];
    hmac_sha256(keys.mac, sizeof(keys.mac), sealed, HEADER_SIZE + plain_length, tag);
    bool ok = crypto_equal(tag, stored_tag, TAG_SIZE);
    wipe(tag, sizeof(tag));
    if (!ok) {
        wipe(&keys, sizeof(keys));
        return FCRYPT_ERR_PASSWORD;
    }

    uint8_t* plain = malloc(plain_length + 1); // + 1 for a '\0', so text can be printed as it is
    if (plain == 0) {
        wipe(&keys, sizeof(keys));
        return FCRYPT_ERR_NO_MEMORY;
    }
    aes256_t aes;
    aes256_init(&aes, keys.aes);
    memcpy(plain, sealed + HEADER_SIZE, plain_length);
    aes256_ctr(&aes, sealed + OFFSET_NONCE, plain, plain_length); // CTR decrypts the same way it encrypts
    plain[plain_length] = '\0';
    wipe(&keys, sizeof(keys));
    wipe(&aes, sizeof(aes));
    *out = plain;
    *out_length = plain_length;
    return FCRYPT_OK;
}

// ---- whole files ------------------------------------------------------------------------------

// read the whole file the user named, filling in its directory and name for writing it back
// NULL if it isn't there
static char* load (const char* path, char* dir, char* name, uint32_t* size) {
    if (!fs_split_path(path, dir, FS_PATH_MAX, name, FS_NAME_MAX) || !fs_file_exists(dir, name)) {
        return 0;
    }
    return fs_read_bytes(dir, name, size);
}

bool fcrypt_file_is_encrypted (const char* path) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    uint32_t size = 0;
    char* data = load(path, dir, name, &size);
    bool sealed = data && fcrypt_is_sealed(data, size);
    free(data);
    return sealed;
}

int fcrypt_encrypt_file (const char* path, const char* password) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    uint32_t size = 0;
    char* data = load(path, dir, name, &size);
    if (data == 0) {
        return FCRYPT_ERR_NOT_FOUND;
    }
    if (fcrypt_is_sealed(data, size)) {
        free(data);
        return FCRYPT_ERR_ENCRYPTED; // encrypting twice would work, but is never what anyone means
    }
    uint8_t* sealed;
    uint32_t sealed_length;
    int result = fcrypt_seal(data, size, password, &sealed, &sealed_length);
    wipe(data, size); // our copy of the unencrypted contents
    free(data);
    if (result != FCRYPT_OK) {
        return result;
    }
    // wipe_old = true: the old (unencrypted) contents are zeroed on the disk before being freed,
    // otherwise they would still be readable there until something else happened to overwrite them
    result = fs_write_bytes(dir, name, sealed, sealed_length, true) == FS_OK ? FCRYPT_OK : FCRYPT_ERR_SAVE;
    free(sealed);
    return result;
}

int fcrypt_read_file (const char* path, const char* password, uint8_t** out, uint32_t* out_length) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    uint32_t size = 0;
    char* data = load(path, dir, name, &size);
    if (data == 0) {
        return FCRYPT_ERR_NOT_FOUND;
    }
    int result = fcrypt_open(data, size, password, out, out_length);
    free(data);
    return result;
}

int fcrypt_decrypt_file (const char* path, const char* password) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    uint32_t size = 0;
    char* data = load(path, dir, name, &size);
    if (data == 0) {
        return FCRYPT_ERR_NOT_FOUND;
    }
    uint8_t* plain;
    uint32_t length;
    int result = fcrypt_open(data, size, password, &plain, &length);
    free(data);
    if (result != FCRYPT_OK) {
        return result;
    }
    // the old contents were encrypted, so they don't need wiping
    result = fs_write_bytes(dir, name, plain, length, false) == FS_OK ? FCRYPT_OK : FCRYPT_ERR_SAVE;
    wipe(plain, length);
    free(plain);
    return result;
}

const char* fcrypt_strerror (int error) {
    switch (error) {
        case FCRYPT_OK: return "ok";
        case FCRYPT_ERR_NOT_FOUND: return "no such file";
        case FCRYPT_ERR_ENCRYPTED: return "the file is already encrypted";
        case FCRYPT_ERR_NOT_ENCRYPTED: return "the file isn't encrypted";
        case FCRYPT_ERR_PASSWORD: return "wrong password, or the file has been changed";
        case FCRYPT_ERR_DAMAGED: return "the file is damaged";
        case FCRYPT_ERR_NO_MEMORY: return "not enough memory";
        case FCRYPT_ERR_NO_PASSWORD: return "the password can't be empty";
        default: return "couldn't save the file";
    }
}
