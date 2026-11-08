/*
    fcrypt.h
    Password protected files.

    An encrypted file keeps its name, only its contents change. They become:

        0        8        12                28       36                      36+n      36+n+32
        +--------+--------+-----------------+--------+-----------------------+---------+
        | magic  | rounds | salt            | nonce  | encrypted contents    | tag     |
        +--------+--------+-----------------+--------+-----------------------+---------+
         8 bytes  4 bytes  16 bytes          8 bytes  n bytes (same as before) 32 bytes

        magic     "AERENC01", so an encrypted file can be told from any other
        rounds    PBKDF2 rounds used for this file (little endian)
        salt      random for every encryption, so the same password gives different keys
        nonce     random, the first half of every AES-CTR counter block
        tag       HMAC-SHA-256 of everything before it (header and encrypted contents)

    So an encrypted file is exactly 68 bytes longer than the original.

    The password and salt go through PBKDF2 to make a master key, and two keys are made from
    that: one for AES (scrambles the contents) and one for the HMAC (detects any change). On
    decryption the tag is checked before anything is decrypted, so a wrong password or a file
    that was tampered with is caught without ever producing garbage. The two cases can't be
    told apart, which is on purpose.

    Encrypting zeros the original contents on the disk, so they can't be read back later.
*/
#ifndef FCRYPT_H
#define FCRYPT_H

#include <stdint.h>
#include <stdbool.h>

#define FCRYPT_OK                 0
#define FCRYPT_ERR_NOT_FOUND     -1
#define FCRYPT_ERR_ENCRYPTED     -2 // asked to encrypt a file that already is
#define FCRYPT_ERR_NOT_ENCRYPTED -3 // asked to decrypt a file that isn't
#define FCRYPT_ERR_PASSWORD      -4 // wrong password, or the file was changed since
#define FCRYPT_ERR_DAMAGED       -5 // too short, or the header makes no sense
#define FCRYPT_ERR_NO_MEMORY     -6
#define FCRYPT_ERR_SAVE          -7
#define FCRYPT_ERR_NO_PASSWORD   -8

// does this data start like an encrypted file?
bool fcrypt_is_sealed (const void* data, uint32_t length);

// encrypt length bytes into a new buffer (*out, free it when done)
int fcrypt_seal (const void* plain, uint32_t length, const char* password, uint8_t** out, uint32_t* out_length);

// check and decrypt into a new buffer (*out, with a '\0' after the contents; wipe and free it when done)
int fcrypt_open (const void* sealed, uint32_t length, const char* password, uint8_t** out, uint32_t* out_length);

// the same for whole files, path is as the user typed it ("notes.txt", "../docs/a.txt")
bool fcrypt_file_is_encrypted (const char* path);
int fcrypt_encrypt_file (const char* path, const char* password);
int fcrypt_decrypt_file (const char* path, const char* password);

// decrypt a file into memory without changing it on the disk (same rules as fcrypt_open)
int fcrypt_read_file (const char* path, const char* password, uint8_t** out, uint32_t* out_length);

const char* fcrypt_strerror (int error);

#endif /* FCRYPT_H */
