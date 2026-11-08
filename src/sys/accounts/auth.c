/*
    auth.c - user accounts and the login screen (see auth.h)
*/

#include "auth.h"
#include <stdint.h>
#include "../../lib/primary_definitions.h"
#include "../../lib/crypto.h"
#include "../../drivers/video/VGA_linear.h"
#include "../../drivers/input/IO.h"
#include "../f_sys.h"
#include "../sys_clck.h"

#define AUTH_DIR        "/acc/"
#define AUTH_FILE       "users"
#define AUTH_MAX_USERS  32
#define AUTH_SALT_SIZE  16
#define AUTH_ITERATIONS 100000 // PBKDF2 rounds for new passwords (stored per user, so it can grow later)
#define PASSWORD_MAX    64

typedef struct {
    char name[AUTH_NAME_MAX + 1];
    uint32_t iterations;
    uint8_t salt[AUTH_SALT_SIZE];
    uint8_t hash[SHA256_SIZE];
} account_t;

char auth_user[AUTH_NAME_MAX + 1] = "guest";

static account_t accounts[AUTH_MAX_USERS];
static int account_count = 0;

bool auth_required (void) {
    return fs_mode == FS_MODE_DISK;
}

static bool valid_name (const char* name) {
    size_t length = strlen(name);
    if (length == 0 || length > AUTH_NAME_MAX) return false;
    for (const char* p = name; *p; p++) {
        if (!islower((unsigned char)*p) && !isdigit((unsigned char)*p) && *p != '_' && *p != '-') return false;
    }
    return true;
}

// read /acc/users into accounts[], skipping lines that don't make sense
static void load (void) {
    account_count = 0;
    char* text = fs_read_f(AUTH_DIR, AUTH_FILE);
    if (text == 0) return;
    char* save;
    for (char* line = strtok_r(text, "\n", &save); line && account_count < AUTH_MAX_USERS; line = strtok_r(0, "\n", &save)) {
        char* parts[4];
        if (split(StrTrim(line), ':', parts, 4) != 4) continue;
        account_t* a = &accounts[account_count];
        if (!valid_name(parts[0]) || strlen(parts[2]) != AUTH_SALT_SIZE * 2 || strlen(parts[3]) != SHA256_SIZE * 2) continue;
        strlcpy(a->name, parts[0], sizeof(a->name));
        a->iterations = (uint32_t)strtoul(parts[1], 0, 10);
        if (a->iterations == 0 || !crypto_from_hex(parts[2], a->salt, AUTH_SALT_SIZE) ||
            !crypto_from_hex(parts[3], a->hash, SHA256_SIZE)) continue;
        account_count++;
    }
    free(text);
}

static int save (void) {
    char* text = malloc((size_t)account_count * 128 + 1);
    if (text == 0) return AUTH_ERR_SAVE;
    text[0] = '\0';
    for (int i = 0; i < account_count; i++) {
        char salt[AUTH_SALT_SIZE * 2 + 1], hash[SHA256_SIZE * 2 + 1], line[128];
        crypto_to_hex(accounts[i].salt, AUTH_SALT_SIZE, salt);
        crypto_to_hex(accounts[i].hash, SHA256_SIZE, hash);
        snprintf(line, sizeof(line), "%s:%u:%s:%s\n", accounts[i].name, accounts[i].iterations, salt, hash);
        strcat(text, line);
    }
    int result = fs_file_exists(AUTH_DIR, AUTH_FILE) ? fs_write_f(AUTH_DIR, AUTH_FILE, text, false)
                                                     : fs_create_f(AUTH_DIR, AUTH_FILE, text);
    free(text);
    return result >= 0 ? AUTH_OK : AUTH_ERR_SAVE;
}

static int find (const char* name) {
    for (int i = 0; i < account_count; i++) {
        if (StrCompare(accounts[i].name, name)) return i;
    }
    return -1;
}

static void set_password (account_t* a, const char* password) {
    a->iterations = AUTH_ITERATIONS;
    crypto_random(a->salt, AUTH_SALT_SIZE);
    pbkdf2_sha256(password, strlen(password), a->salt, AUTH_SALT_SIZE, a->iterations, a->hash, SHA256_SIZE);
}

int auth_add (const char* name, const char* password) {
    if (!valid_name(name)) return AUTH_ERR_NAME;
    load();
    if (find(name) >= 0) return AUTH_ERR_EXISTS;
    if (account_count == AUTH_MAX_USERS) return AUTH_ERR_FULL;
    account_t* a = &accounts[account_count++];
    strlcpy(a->name, name, sizeof(a->name));
    set_password(a, password);
    return save();
}

int auth_delete (const char* name) {
    load();
    int i = find(name);
    if (i < 0) return AUTH_ERR_NOT_FOUND;
    memmove(&accounts[i], &accounts[i + 1], (size_t)(account_count - i - 1) * sizeof(account_t));
    account_count--;
    return save();
}

int auth_set_password (const char* name, const char* password) {
    load();
    int i = find(name);
    if (i < 0) return AUTH_ERR_NOT_FOUND;
    set_password(&accounts[i], password);
    return save();
}

bool auth_check (const char* name, const char* password) {
    load();
    int i = find(name);
    uint8_t hash[SHA256_SIZE];
    // an unknown user still costs a full PBKDF2, so the time taken doesn't reveal which names exist
    static const uint8_t dummy_salt[AUTH_SALT_SIZE] = { 0 };
    const account_t* a = i >= 0 ? &accounts[i] : 0;
    pbkdf2_sha256(password, strlen(password), a ? a->salt : dummy_salt, AUTH_SALT_SIZE,
                  a ? a->iterations : AUTH_ITERATIONS, hash, SHA256_SIZE);
    bool ok = a && crypto_equal(hash, a->hash, SHA256_SIZE);
    crypto_wipe(hash, sizeof(hash));
    return ok;
}

bool auth_exists (const char* name) {
    load();
    return find(name) >= 0;
}

void auth_list (void (*fn)(const char* name, void* ctx), void* ctx) {
    load();
    for (int i = 0; i < account_count; i++) {
        fn(accounts[i].name, ctx);
    }
}

const char* auth_strerror (int error) {
    switch (error) {
        case AUTH_OK: return "ok";
        case AUTH_ERR_NAME: return "names are 1 to 16 of a-z, 0-9, _ and -";
        case AUTH_ERR_EXISTS: return "that user already exists";
        case AUTH_ERR_FULL: return "too many users";
        case AUTH_ERR_NOT_FOUND: return "no such user";
        default: return "couldn't save the accounts file";
    }
}

// the login screen

static void heading (const char* subtitle) {
    console_set_color(BRIGHT, BLACK);
    console_clear();
    printf("\n");
    console_set_color(YELLOW, BLACK);
    printf("  AerOS\n");
    console_set_color(BRIGHT, BLACK);
    printf("  %s\n\n", subtitle);
}

static void ask (const char* question, char* answer, int size, bool secret) {
    console_set_color(WHITE, BLACK);
    printf("  %s", question);
    console_set_color(BRIGHT, BLACK);
    if (secret) {
        GetSecret(answer, size);
    } else {
        GetString(answer, size);
    }
}

static void first_account (void) {
    char name[64], password[PASSWORD_MAX], again[PASSWORD_MAX];
    heading("Welcome! Create the first account for this computer.");
    for (;;) {
        ask("User name: ", name, sizeof(name), false);
        strlwr(StrTrim(name));
        if (!valid_name(StrTrim(name))) {
            printf("  Names are 1 to 16 of a-z, 0-9, _ and -\n\n");
            continue;
        }
        ask("Password:  ", password, sizeof(password), true);
        ask("Again:     ", again, sizeof(again), true);
        if (password[0] == '\0' || strcmp(password, again) != 0) {
            printf("  The passwords %s, try again.\n\n", password[0] ? "don't match" : "can't be empty");
            continue;
        }
        printf("  Saving...\n");
        int result = auth_add(StrTrim(name), password);
        crypto_wipe(password, sizeof(password));
        crypto_wipe(again, sizeof(again));
        if (result != AUTH_OK) {
            printf("  Couldn't create the account: %s\n\n", auth_strerror(result));
            continue;
        }
        strlcpy(auth_user, StrTrim(name), sizeof(auth_user));
        return;
    }
}

void auth_login (void) {
    if (!auth_required()) {
        strlcpy(auth_user, "guest", sizeof(auth_user));
        return;
    }
    load();
    if (account_count == 0) {
        first_account();
        return;
    }

    heading("Log in");
    for (;;) {
        char name[64], password[PASSWORD_MAX];
        ask("User:     ", name, sizeof(name), false);
        ask("Password: ", password, sizeof(password), true);
        char* user = strlwr(StrTrim(name));
        bool ok = auth_check(user, password);
        crypto_wipe(password, sizeof(password));
        if (ok) {
            strlcpy(auth_user, user, sizeof(auth_user));
            return;
        }
        sleep_ms(1000); // slows down anyone trying passwords by hand
        console_set_color(LIGHT_RED, BLACK);
        printf("  Wrong user name or password.\n\n");
    }
}
