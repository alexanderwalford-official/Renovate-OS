/*
    auth.c - user accounts and the login screen (see auth.h, and docs/SECURITY.md for the design)

    The accounts file, /acc/users, is plain text with one line per user:

        alex:100000:ddc76e8311a11dfcb75bdea5ba79573a:7bf4cd0afe31...(64 hex digits):1
        |    |      |                                |                              |
        |    |      salt, 16 random bytes as hex     |                              wrong passwords
        |    PBKDF2 rounds for this password         PBKDF2(password, salt, rounds) since the last
        user name                                    32 bytes as hex                good login

    (lines from before the last field was added have only four fields, the count is then 0)

    Every function reads the file fresh (load) and writes it back whole when it changes (save).
    The file is tiny, so that costs nothing, and the file on the disk is always the one truth:
    there is no copy in memory that could get out of step with it.

    Wrong passwords. Each account counts the wrong passwords typed for it since its last good
    login, and the count is saved, so turning the computer off and on doesn't reset it:

        1st and 2nd wrong password   a warning on the login screen
        3rd                          everything in the account's home folder is securely deleted
                                     (fs_wipe_dir: zeroed on the disk, then deleted)
        good login                   the count goes back to 0, and the terminal says how many
                                     wrong passwords there were

    The count keeps going past 3 (6 wipes again, and so on) until a good login, so the banner
    can say how often the folder was wiped. Names that don't exist aren't counted.
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
// PBKDF2 rounds for new passwords. About 1 second in QEMU and far less on real hardware, while
// costing anyone guessing passwords 100000 HMACs per guess. The count is saved with each
// password, so raising it later only affects passwords set after the change
#define AUTH_ITERATIONS 100000
#define AUTH_LINE_MAX   160 // name (16) + rounds (10) + salt (32) + hash (64) + count (10) + 4 ':' + '\n' + '\0'
#define PASSWORD_MAX    64  // longest password the login screen reads, including the '\0'
#define WRONG_DELAY_MS  1000 // pause after a wrong password, so guessing by hand is slow
#define HOME_ROOT       "/home/"

// one line of the accounts file, decoded
typedef struct {
    char name[AUTH_NAME_MAX + 1];
    uint32_t iterations;
    uint8_t salt[AUTH_SALT_SIZE];
    uint8_t hash[SHA256_SIZE];
    uint32_t failures; // wrong passwords since the last good login
} account_t;

char auth_user[AUTH_NAME_MAX + 1] = "guest";
uint32_t auth_failures_seen = 0;

// the accounts, as last read by load()
static account_t accounts[AUTH_MAX_USERS];
static int account_count = 0;

bool auth_required (void) {
    return fs_mode == FS_MODE_DISK;
}

// 1 to max characters of a-z, 0-9, _ and - (so names are safe in paths, prompts and the file format)
static bool valid_name (const char* name, size_t max) {
    size_t length = strlen(name);
    if (length == 0 || length > max) return false;
    for (const char* p = name; *p; p++) {
        if (!islower((unsigned char)*p) && !isdigit((unsigned char)*p) && *p != '_' && *p != '-') return false;
    }
    return true;
}

// read /acc/users into accounts[]. A line that doesn't make sense (a damaged or hand edited file)
// is skipped rather than stopping everything, so one bad line can't lock everyone out
static void load (void) {
    account_count = 0;
    char* text = fs_read_f(AUTH_DIR, AUTH_FILE);
    if (text == 0) return; // no file yet: no accounts
    char* rest;
    for (char* line = strtok_r(text, "\n", &rest); line && account_count < AUTH_MAX_USERS;
         line = strtok_r(0, "\n", &rest)) {
        char* parts[5]; // name, rounds, salt, hash, wrong passwords
        int fields = split(StrTrim(line), ':', parts, 5);
        if (fields < 4) continue;
        if (!valid_name(parts[0], AUTH_NAME_MAX) || strlen(parts[2]) != AUTH_SALT_SIZE * 2 ||
            strlen(parts[3]) != SHA256_SIZE * 2) continue;

        account_t* a = &accounts[account_count];
        strlcpy(a->name, parts[0], sizeof(a->name));
        a->iterations = (uint32_t)strtoul(parts[1], 0, 10);
        a->failures = fields == 5 ? (uint32_t)strtoul(parts[4], 0, 10) : 0;
        if (a->iterations == 0 || !crypto_from_hex(parts[2], a->salt, AUTH_SALT_SIZE) ||
            !crypto_from_hex(parts[3], a->hash, SHA256_SIZE)) continue;
        account_count++; // only now is the slot kept
    }
    free(text);
}

// write accounts[] back to /acc/users (creating it the first time)
static int save (void) {
    char* text = malloc((size_t)account_count * AUTH_LINE_MAX + 1);
    if (text == 0) return AUTH_ERR_SAVE;
    text[0] = '\0';
    for (int i = 0; i < account_count; i++) {
        char salt[AUTH_SALT_SIZE * 2 + 1], hash[SHA256_SIZE * 2 + 1], line[AUTH_LINE_MAX];
        crypto_to_hex(accounts[i].salt, AUTH_SALT_SIZE, salt);
        crypto_to_hex(accounts[i].hash, SHA256_SIZE, hash);
        snprintf(line, sizeof(line), "%s:%u:%s:%s:%u\n", accounts[i].name, accounts[i].iterations,
                 salt, hash, accounts[i].failures);
        strcat(text, line);
    }
    int result = fs_file_exists(AUTH_DIR, AUTH_FILE) ? fs_write_f(AUTH_DIR, AUTH_FILE, text, false)
                                                     : fs_create_f(AUTH_DIR, AUTH_FILE, text);
    free(text);
    return result >= 0 ? AUTH_OK : AUTH_ERR_SAVE;
}

// the index of name in accounts[], -1 if there is no such user
static int find (const char* name) {
    for (int i = 0; i < account_count; i++) {
        if (StrCompare(accounts[i].name, name)) return i;
    }
    return -1;
}

// a fresh random salt and the hash of password with it (the password itself is never kept)
static void set_password (account_t* a, const char* password) {
    a->iterations = AUTH_ITERATIONS;
    crypto_random(a->salt, AUTH_SALT_SIZE);
    pbkdf2_sha256(password, strlen(password), a->salt, AUTH_SALT_SIZE, a->iterations, a->hash, SHA256_SIZE);
}

// ---- home folders -----------------------------------------------------------------------------

bool auth_home (const char* name, char* out, size_t size) {
    // a folder name on the disk has to fit 8.3, so only names of up to 8 characters get one
    // (new names can't be longer, only accounts made before home folders existed can be)
    if (strlen(name) > AUTH_HOME_NAME_MAX) return false;
    snprintf(out, size, HOME_ROOT "%s/", name);
    return true;
}

// make /home/<name>/ if it isn't there yet
static void make_home (const char* name) {
    char home[FS_PATH_MAX];
    if (!auth_home(name, home, sizeof(home))) return;
    if (!fs_dir_exists(HOME_ROOT)) fs_create_d(HOME_ROOT, "home folders");
    if (!fs_dir_exists(home)) fs_create_d(home, name);
}

// start the terminal in the user's home folder
static void enter_home (const char* name) {
    char home[FS_PATH_MAX];
    make_home(name); // accounts made before home folders existed get theirs now
    if (auth_home(name, home, sizeof(home)) && fs_dir_exists(home)) {
        strlcpy(cd, home, sizeof(cd));
    }
}

// ---- the accounts -----------------------------------------------------------------------------

bool auth_valid_new_name (const char* name) {
    return valid_name(name, AUTH_HOME_NAME_MAX);
}

int auth_add (const char* name, const char* password) {
    if (!auth_valid_new_name(name)) return AUTH_ERR_NAME;
    load();
    if (find(name) >= 0) return AUTH_ERR_EXISTS;
    if (account_count == AUTH_MAX_USERS) return AUTH_ERR_FULL;
    account_t* a = &accounts[account_count++];
    memset(a, 0, sizeof(*a));
    strlcpy(a->name, name, sizeof(a->name));
    set_password(a, password);
    int result = save();
    if (result == AUTH_OK) make_home(name);
    return result;
}

int auth_delete (const char* name) {
    load();
    int i = find(name);
    if (i < 0) return AUTH_ERR_NOT_FOUND;
    // close the gap by moving everyone after it down one place
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

// hash the typed password with the user's salt and rounds, and compare with the stored hash
bool auth_check (const char* name, const char* password) {
    load();
    int i = find(name);
    const account_t* a = i >= 0 ? &accounts[i] : 0;
    // an unknown user still costs a full PBKDF2, so how long a failed login takes doesn't
    // reveal whether the name exists
    static const uint8_t dummy_salt[AUTH_SALT_SIZE] = { 0 };
    uint8_t hash[SHA256_SIZE];
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
        case AUTH_ERR_NAME: return "names are 1 to 8 of a-z, 0-9, _ and -";
        case AUTH_ERR_EXISTS: return "that user already exists";
        case AUTH_ERR_FULL: return "too many users";
        case AUTH_ERR_NOT_FOUND: return "no such user";
        default: return "couldn't save the accounts file";
    }
}

// ---- wrong passwords --------------------------------------------------------------------------

// one more wrong password for name, saved straight away; returns the new count (0 for no such user)
static uint32_t count_failure (const char* name) {
    load();
    int i = find(name);
    if (i < 0) return 0;
    accounts[i].failures++;
    save();
    return accounts[i].failures;
}

// a good login: forget the wrong passwords, returns how many there were
static uint32_t clear_failures (const char* name) {
    load();
    int i = find(name);
    if (i < 0 || accounts[i].failures == 0) return 0;
    uint32_t failures = accounts[i].failures;
    accounts[i].failures = 0;
    save();
    return failures;
}

// after a wrong password for an existing account: warn, or on every third one wipe the home folder
static void failed_login (const char* name, uint32_t failures) {
    char home[FS_PATH_MAX];
    bool has_home = auth_home(name, home, sizeof(home)) && fs_dir_exists(home);
    uint32_t in_a_row = failures % AUTH_MAX_FAILURES; // 1, 2, then 0 on the third
    if (in_a_row != 0) {
        console_set_color(YELLOW, BLACK);
        printf("  Warning: that is %u of %u wrong passwords in a row for %s.\n", in_a_row, AUTH_MAX_FAILURES, name);
        if (has_home) {
            printf("  After %u, everything in %s is deleted.\n", AUTH_MAX_FAILURES, home);
        }
        return;
    }
    int wiped = has_home ? fs_wipe_dir(home) : 0;
    console_set_color(LIGHT_RED, BLACK);
    printf("  %u wrong passwords in a row for %s.\n", AUTH_MAX_FAILURES, name);
    if (has_home) {
        printf("  The %d file(s) in %s have been securely deleted.\n", wiped < 0 ? 0 : wiped, home);
    }
}

// ---- the login screen -------------------------------------------------------------------------

// clear the screen and draw the "AerOS" title with a line under it
static void heading (const char* subtitle) {
    console_set_color(BRIGHT, BLACK);
    console_clear();
    printf("\n");
    console_set_color(YELLOW, BLACK);
    printf("  AerOS\n");
    console_set_color(BRIGHT, BLACK);
    printf("  %s\n\n", subtitle);
}

// print a question and read the answer (shown as * for secrets)
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

// a brand new disk has no accounts: ask for the first one, and log in as it
static void first_account (void) {
    char name[64], password[PASSWORD_MAX], again[PASSWORD_MAX];
    heading("Welcome! Create the first account for this computer.");
    for (;;) {
        ask("User name: ", name, sizeof(name), false);
        char* user = strlwr(StrTrim(name)); // names are stored in lower case
        if (!valid_name(user, AUTH_HOME_NAME_MAX)) {
            printf("  Names are 1 to 8 of a-z, 0-9, _ and -\n\n");
            continue;
        }
        ask("Password:  ", password, sizeof(password), true);
        ask("Again:     ", again, sizeof(again), true);
        if (password[0] == '\0' || strcmp(password, again) != 0) {
            printf("  The passwords %s, try again.\n\n", password[0] ? "don't match" : "can't be empty");
            continue;
        }
        printf("  Saving...\n"); // PBKDF2 takes a moment
        int result = auth_add(user, password);
        crypto_wipe(password, sizeof(password));
        crypto_wipe(again, sizeof(again));
        if (result != AUTH_OK) {
            printf("  Couldn't create the account: %s\n\n", auth_strerror(result));
            continue;
        }
        strlcpy(auth_user, user, sizeof(auth_user));
        enter_home(user);
        return;
    }
}

void auth_login (void) {
    auth_failures_seen = 0;
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
            auth_failures_seen = clear_failures(user);
            strlcpy(auth_user, user, sizeof(auth_user));
            enter_home(user);
            return;
        }
        sleep_ms(WRONG_DELAY_MS);
        uint32_t failures = count_failure(user);
        console_set_color(LIGHT_RED, BLACK);
        printf("  Wrong user name or password.\n");
        if (failures > 0) {
            failed_login(user, failures);
        }
        printf("\n");
    }
}
