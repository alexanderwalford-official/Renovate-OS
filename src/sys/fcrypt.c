/*
    fcrypt.c - password protected files (see fcrypt.h for the file format)
*/

#include "fcrypt.h"
#include "../lib/primary_definitions.h"
#include "../lib/crypto.h"
#include "f_sys.h"

#define MAGIC        "AERENC01"
#define MAGIC_SIZE   8
#define SALT_SIZE    16
#define NONCE_SIZE   8
#define HEADER_SIZE  (MAGIC_SIZE + 4 + SALT_SIZE + NONCE_SIZE) // 36
#define TAG_SIZE     SHA256_SIZE                               // 32
#define ROUNDS       100000
#define ROUNDS_MAX   5000000 // refuse headers asking for more, or a damaged file could hang the OS

typedef struct {
    uint8_t aes[32];
    uint8_t mac[32];
} keys_t;

// password + salt -> master key (PBKDF2, the slow part) -> one key for AES and one for the HMAC
static void derive_keys (const char* password, const uint8_t* salt, uint32_t rounds, keys_t* keys) {
    uint8_t master[SHA256_SIZE];
    pbkdf2_sha256(password, strlen(password), salt, SALT_SIZE, rounds, master, sizeof(master));
    hmac_sha256(master, sizeof(master), "AerOS file key", 14, keys->aes);
    hmac_sha256(master, sizeof(master), "AerOS file mac", 14, keys->mac);
    crypto_wipe(master, sizeof(master));
}

static uint32_t read_u32 (const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool fcrypt_is_sealed (const void* data, uint32_t length) {
    return length >= HEADER_SIZE + TAG_SIZE && memcmp(data, MAGIC, MAGIC_SIZE) == 0;
}

int fcrypt_seal (const void* plain, uint32_t length, const char* password, uint8_t** out, uint32_t* out_length) {
    if (password[0] == '\0') return FCRYPT_ERR_NO_PASSWORD;
    uint32_t total = HEADER_SIZE + length + TAG_SIZE;
    uint8_t* sealed = malloc(total);
    if (sealed == 0) return FCRYPT_ERR_NO_MEMORY;

    memcpy(sealed, MAGIC, MAGIC_SIZE);
    for (int i = 0; i < 4; i++) {
        sealed[MAGIC_SIZE + i] = (uint8_t)(ROUNDS >> (8 * i));
    }
    uint8_t* salt = sealed + MAGIC_SIZE + 4;
    uint8_t* nonce = salt + SALT_SIZE;
    crypto_random(salt, SALT_SIZE);
    crypto_random(nonce, NONCE_SIZE);

    keys_t keys;
    aes256_t aes;
    derive_keys(password, salt, ROUNDS, &keys);
    aes256_init(&aes, keys.aes);
    memcpy(sealed + HEADER_SIZE, plain, length);
    aes256_ctr(&aes, nonce, sealed + HEADER_SIZE, length);
    hmac_sha256(keys.mac, sizeof(keys.mac), sealed, HEADER_SIZE + length, sealed + HEADER_SIZE + length);

    crypto_wipe(&keys, sizeof(keys));
    crypto_wipe(&aes, sizeof(aes));
    *out = sealed;
    *out_length = total;
    return FCRYPT_OK;
}

int fcrypt_open (const void* sealed_data, uint32_t length, const char* password, uint8_t** out, uint32_t* out_length) {
    const uint8_t* sealed = sealed_data;
    if (!fcrypt_is_sealed(sealed, length)) return FCRYPT_ERR_NOT_ENCRYPTED;
    uint32_t rounds = read_u32(sealed + MAGIC_SIZE);
    if (rounds == 0 || rounds > ROUNDS_MAX) return FCRYPT_ERR_DAMAGED;
    const uint8_t* salt = sealed + MAGIC_SIZE + 4;
    const uint8_t* nonce = salt + SALT_SIZE;
    uint32_t plain_length = length - HEADER_SIZE - TAG_SIZE;

    keys_t keys;
    derive_keys(password, salt, rounds, &keys);

    // check the tag first: only decrypt what we know is exactly what was encrypted
    uint8_t tag[TAG_SIZE];
    hmac_sha256(keys.mac, sizeof(keys.mac), sealed, HEADER_SIZE + plain_length, tag);
    bool ok = crypto_equal(tag, sealed + HEADER_SIZE + plain_length, TAG_SIZE);
    crypto_wipe(tag, sizeof(tag));
    if (!ok) {
        crypto_wipe(&keys, sizeof(keys));
        return FCRYPT_ERR_PASSWORD;
    }

    uint8_t* plain = malloc(plain_length + 1);
    if (plain == 0) {
        crypto_wipe(&keys, sizeof(keys));
        return FCRYPT_ERR_NO_MEMORY;
    }
    aes256_t aes;
    aes256_init(&aes, keys.aes);
    memcpy(plain, sealed + HEADER_SIZE, plain_length);
    aes256_ctr(&aes, nonce, plain, plain_length);
    plain[plain_length] = '\0';
    crypto_wipe(&keys, sizeof(keys));
    crypto_wipe(&aes, sizeof(aes));
    *out = plain;
    *out_length = plain_length;
    return FCRYPT_OK;
}

// read a whole file the user named, NULL if it isn't there
static char* load (const char* path, char* dir, char* name, uint32_t* size) {
    if (!fs_split_path(path, dir, FS_PATH_MAX, name, FS_NAME_MAX) || !fs_file_exists(dir, name)) return 0;
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
    if (data == 0) return FCRYPT_ERR_NOT_FOUND;
    if (fcrypt_is_sealed(data, size)) {
        free(data);
        return FCRYPT_ERR_ENCRYPTED;
    }
    uint8_t* sealed;
    uint32_t sealed_length;
    int result = fcrypt_seal(data, size, password, &sealed, &sealed_length);
    crypto_wipe(data, size);
    free(data);
    if (result != FCRYPT_OK) return result;
    // true = zero the old (unencrypted) contents before they are freed
    result = fs_write_bytes(dir, name, sealed, sealed_length, true) == FS_OK ? FCRYPT_OK : FCRYPT_ERR_SAVE;
    free(sealed);
    return result;
}

int fcrypt_read_file (const char* path, const char* password, uint8_t** out, uint32_t* out_length) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    uint32_t size = 0;
    char* data = load(path, dir, name, &size);
    if (data == 0) return FCRYPT_ERR_NOT_FOUND;
    int result = fcrypt_open(data, size, password, out, out_length);
    free(data);
    return result;
}

int fcrypt_decrypt_file (const char* path, const char* password) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    uint8_t* plain;
    uint32_t length;
    int result = fcrypt_read_file(path, password, &plain, &length);
    if (result != FCRYPT_OK) return result;
    fs_split_path(path, dir, sizeof(dir), name, sizeof(name));
    result = fs_write_bytes(dir, name, plain, length, false) == FS_OK ? FCRYPT_OK : FCRYPT_ERR_SAVE;
    crypto_wipe(plain, length);
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
