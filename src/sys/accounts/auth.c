/*
    auth.c - user accounts and the login screen
    (the overview is in auth.h, and the security design in docs/SECURITY.md)

    THE ACCOUNTS FILE

    /acc/users is plain text with one line per user, five fields split by ':'

        alex:100000:ddc76e8311a11dfcb75bdea5ba79573a:7bf4cd0afe31...(64 hex digits):1
        |    |      |                                |                              |
        |    |      the salt: 16 random bytes,       the hash: what PBKDF2 made     wrong passwords
        |    |      written as 32 hex digits         from the password and salt,    since the last
        |    |                                       32 bytes as 64 hex digits      good login
        |    how many PBKDF2 rounds this password was hashed with
        user name

    (Files from before the last field was added have four fields; the count is then 0.)

    Every function here reads the whole file fresh (load) and, if it changes anything, writes
    the whole file back (save). The file is tiny, so this costs nothing, and it means the file
    on the disk is always the one truth: there is no copy in memory that could fall out of step.

    WRONG PASSWORDS

    Each account counts the wrong passwords typed for it since its last good login. The count
    is saved straight away, so turning the computer off and on again doesn't reset it:

        1st and 2nd wrong password   a warning on the login screen
        3rd                          everything in the account's home folder is securely
                                     deleted (each file is overwritten with zeros, then
                                     deleted), with a progress bar
        a good login                 the count goes back to 0, and the terminal says how many
                                     wrong passwords there were

    The count keeps going past 3 (the 6th wipes again, and so on) until a good login, so the
    banner can say how often the folder was wiped. Names that don't exist aren't counted.
*/

#include "auth.h"
#include <stdint.h>
#include "../../lib/primary_definitions.h"
#include "../../lib/crypto.h"
#include "../../drivers/video/VGA_linear.h"
#include "../../drivers/input/IO.h"
#include "../f_sys.h"
#include "../sys_clck.h"
#include "../ui.h"
#include "../rt.h"

#define ACCOUNTS_FOLDER  "/acc/"
#define ACCOUNTS_FILE    "users"
#define MAX_ACCOUNTS     32
#define SALT_SIZE        16     // bytes of random salt per password

// How many PBKDF2 rounds new passwords get. About a second in QEMU and much less on a real
// PC, while costing anyone trying to guess a password 100000 rounds per guess. The number is
// saved with each password, so raising it later only affects passwords set after that
#define PBKDF2_ROUNDS    100000

// the longest line in the accounts file: name (16) + rounds (10) + salt (32) + hash (64)
// + wrong passwords (10) + 4 ':' + '\n' + '\0'
#define ACCOUNT_LINE_MAX 160

#define PASSWORD_MAX     64     // the longest password the login screen reads, counting the '\0'
#define WRONG_PASSWORD_DELAY_MS 1000 // a pause after each wrong password, so guessing by hand is slow
#define HOMES_FOLDER     "/home/"

// one line of the accounts file, read into its parts
typedef struct {
    char name[AUTH_NAME_MAX + 1];
    uint32_t rounds;                 // PBKDF2 rounds
    uint8_t salt[SALT_SIZE];
    uint8_t hash[SHA256_SIZE];
    uint32_t wrong_passwords;        // since the last good login
} account_t;

char auth_user[AUTH_NAME_MAX + 1] = "guest";
uint32_t auth_failures_seen = 0;

// the accounts, as load() last read them
static account_t accounts[MAX_ACCOUNTS];
static int account_count = 0;

// logins are only needed with a real disk: without one there is nowhere to keep accounts
bool auth_required (void) {
    return fs_mode == FS_MODE_DISK;
}

// a name is 1 to `longest` characters of a-z, 0-9, _ and -
// (so it is safe to use in folder names, in the prompt and in the accounts file)
static bool valid_name (const char* name, size_t longest) {
    size_t length = slen(name);
    if (length == 0 || length > longest) {
        return false;
    }
    // the (unsigned char) casts matter: a char above 127 would be negative, and the is...()
    // functions only promise to work for 0 to 255
    for (const char* c = name; *c != '\0'; c++) {
        bool allowed = islower((unsigned char)*c) || isdigit((unsigned char)*c) || *c == '_' || *c == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}


// ---- reading and writing the accounts file --------------------------------------------------

// read one line of the accounts file into an account. False if the line doesn't make sense
static bool parse_account_line (char* line, account_t* account) {
    char* fields[5]; // name, rounds, salt, hash, wrong passwords
    // trim first, so a '\r' from a file edited on another computer doesn't end up in the count
    int field_count = split(trim(line), ':', fields, 5);
    // 4 is enough: the wrong passwords field is newer, and older files don't have it
    if (field_count < 4) {
        return false;
    }

    const char* name = fields[0];
    const char* salt_hex = fields[2];
    const char* hash_hex = fields[3];
    // AUTH_NAME_MAX rather than the shorter limit for new names, so accounts made before home
    // folders existed (with longer names) still load
    if (!valid_name(name, AUTH_NAME_MAX)) {
        return false;
    }
    if (slen(salt_hex) != SALT_SIZE * 2 || slen(hash_hex) != SHA256_SIZE * 2) {
        return false; // each byte is two hex digits
    }

    // the numbers are written in decimal (base 10)
    scpy(account->name, name, sizeof(account->name));
    account->rounds = (uint32_t)strtoul(fields[1], NULL, 10);
    if (field_count == 5) {
        account->wrong_passwords = (uint32_t)strtoul(fields[4], NULL, 10);
    } else {
        account->wrong_passwords = 0; // an older file without the count
    }

    // 0 rounds isn't a real PBKDF2 (and is what strtoul gives for text that isn't a number)
    if (account->rounds == 0) {
        return false;
    }
    // the lengths were checked above, so these only fail on a character that isn't hex
    if (!crypto_from_hex(salt_hex, account->salt, SALT_SIZE)) {
        return false;
    }
    if (!crypto_from_hex(hash_hex, account->hash, SHA256_SIZE)) {
        return false;
    }
    return true;
}

// read /acc/users into accounts[].
// A line that doesn't make sense (a damaged or hand edited file) is skipped rather than
// stopping everything, so one bad line can't lock everyone out
static void load (void) {
    account_count = 0;
    char* text = fs_read_f(ACCOUNTS_FOLDER, ACCOUNTS_FILE);
    if (text == NULL) {
        return; // no file yet, so no accounts
    }

    // one line at a time. Anything past MAX_ACCOUNTS is ignored, as accounts[] has no room
    char* position; // strtok_r keeps its place in the text here
    char* line = strtok_r(text, "\n", &position);
    while (line != NULL && account_count < MAX_ACCOUNTS) {
        if (parse_account_line(line, &accounts[account_count])) {
            account_count++; // only a good line keeps its place in the array
        }
        line = strtok_r(NULL, "\n", &position);
    }
    free(text);
}

// write accounts[] back to /acc/users (making the file the first time)
static int save (void) {
    // room for every account at its longest possible line, plus the '\0' (the + 1 also means
    // a file with no accounts still gets a buffer)
    char* text = malloc((size_t)account_count * ACCOUNT_LINE_MAX + 1);
    if (text == NULL) {
        return AUTH_ERR_SAVE;
    }
    text[0] = '\0';

    // each account back into a "name:rounds:salt:hash:wrong" line, with the bytes as hex
    for (int i = 0; i < account_count; i++) {
        const account_t* account = &accounts[i];
        char salt_hex[SALT_SIZE * 2 + 1];
        char hash_hex[SHA256_SIZE * 2 + 1];
        crypto_to_hex(account->salt, SALT_SIZE, salt_hex);
        crypto_to_hex(account->hash, SHA256_SIZE, hash_hex);

        char line[ACCOUNT_LINE_MAX];
        sfmt(line, sizeof(line), "%s:%u:%s:%s:%u\n",
                 account->name, account->rounds, salt_hex, hash_hex, account->wrong_passwords);
        strcat(text, line);
    }

    // false = replace the whole file, not add to the end of it
    int result;
    if (fs_file_exists(ACCOUNTS_FOLDER, ACCOUNTS_FILE)) {
        result = fs_write_f(ACCOUNTS_FOLDER, ACCOUNTS_FILE, text, false);
    } else {
        result = fs_create_f(ACCOUNTS_FOLDER, ACCOUNTS_FILE, text);
    }
    free(text);

    if (result < 0) {
        return AUTH_ERR_SAVE;
    }
    return AUTH_OK;
}

// where a user is in accounts[], or -1 if there is no such user
static int find (const char* name) {
    for (int i = 0; i < account_count; i++) {
        if (seq(accounts[i].name, name)) {
            return i;
        }
    }
    return -1;
}

// give an account a new password: a fresh random salt, and the hash of the password with it.
// The password itself is never kept anywhere
static void set_password (account_t* account, const char* password) {
    // today's round count, whatever the old password was hashed with
    account->rounds = PBKDF2_ROUNDS;
    // a new salt every time, so the same password set twice still gives a different hash
    crypto_random(account->salt, SALT_SIZE);
    pbkdf2_sha256(password, slen(password), account->salt, SALT_SIZE,
                  account->rounds, account->hash, SHA256_SIZE);
}


// ---- home folders ---------------------------------------------------------------------------

// a user's home folder, "/home/alex/", into out. False if the name is too long to have one
bool auth_home (const char* name, char* out, size_t size) {
    // A folder name on the disk has to fit the 8.3 format, so only names of up to 8 characters
    // get a home folder. New names can't be longer; only accounts made before home folders
    // existed can be
    if (slen(name) > AUTH_HOME_NAME_MAX) {
        return false;
    }
    // HOMES_FOLDER "%s/" is two strings side by side, which C joins into "/home/%s/"
    sfmt(out, size, HOMES_FOLDER "%s/", name);
    return true;
}

// make /home/<name>/ if it isn't there yet
static void make_home (const char* name) {
    char home[FS_PATH_MAX];
    if (!auth_home(name, home, sizeof(home))) {
        return;
    }
    // /home/ itself first, as the first account on a new disk finds it missing
    if (!fs_dir_exists(HOMES_FOLDER)) {
        fs_create_d(HOMES_FOLDER, "home folders");
    }
    if (!fs_dir_exists(home)) {
        fs_create_d(home, name);
    }
}

// after logging in, the terminal starts in the user's home folder
static void enter_home (const char* name) {
    make_home(name); // accounts made before home folders existed get theirs now

    // checked again, as making it can fail (a long old name, or a full disk). If so the
    // terminal just stays where it was
    char home[FS_PATH_MAX];
    if (auth_home(name, home, sizeof(home)) && fs_dir_exists(home)) {
        scpy(cd, home, sizeof(cd));
    }
}


// ---- the accounts ---------------------------------------------------------------------------

// can a new account have this name? New names must be short enough for a home folder
bool auth_valid_new_name (const char* name) {
    return valid_name(name, AUTH_HOME_NAME_MAX);
}

// make a new account with this password, and its home folder
int auth_add (const char* name, const char* password) {
    if (!auth_valid_new_name(name)) {
        return AUTH_ERR_NAME;
    }
    // fresh from the disk, so the name check and the save see the file as it is now
    load();
    if (find(name) >= 0) {
        return AUTH_ERR_EXISTS;
    }
    if (account_count == MAX_ACCOUNTS) {
        return AUTH_ERR_FULL;
    }

    // the new account goes on the end, cleared so nothing from an old load is left in it
    // (a wrong passwords count of 0, for one)
    account_t* account = &accounts[account_count];
    account_count++;
    memset(account, 0, sizeof(*account));
    scpy(account->name, name, sizeof(account->name));
    set_password(account, password);

    // only make the home folder once the account is safely saved
    int result = save();
    if (result == AUTH_OK) {
        make_home(name);
    }
    return result;
}

// remove an account from the accounts file (its home folder is left alone)
int auth_delete (const char* name) {
    load();
    int index = find(name);
    if (index < 0) {
        return AUTH_ERR_NOT_FOUND;
    }

    // close the gap: move every account after it down one place (memmove, not memcpy, because
    // the two areas overlap)
    int after = account_count - index - 1;
    memmove(&accounts[index], &accounts[index + 1], (size_t)after * sizeof(account_t));
    account_count--;
    return save();
}

// give an existing account a new password (and a new salt)
int auth_set_password (const char* name, const char* password) {
    load();
    int index = find(name);
    if (index < 0) {
        return AUTH_ERR_NOT_FOUND;
    }
    set_password(&accounts[index], password);
    return save();
}

// hash the typed password with the user's own salt and rounds, and compare it with the hash
// that was saved when they set their password
bool auth_check (const char* name, const char* password) {
    load();
    int index = find(name);
    const account_t* account = NULL;
    if (index >= 0) {
        account = &accounts[index];
    }

    // An unknown name still costs a full PBKDF2, with a made up salt. That way a wrong name
    // takes exactly as long as a wrong password, and the time can't give away which names exist
    static const uint8_t made_up_salt[SALT_SIZE] = { 0 };
    const uint8_t* salt = made_up_salt;
    uint32_t rounds = PBKDF2_ROUNDS;
    if (account != NULL) {
        salt = account->salt;
        rounds = account->rounds;
    }

    uint8_t hash[SHA256_SIZE];
    pbkdf2_sha256(password, slen(password), salt, SALT_SIZE, rounds, hash, SHA256_SIZE);

    // crypto_equal takes the same time however many bytes match, which a plain memcmp doesn't
    bool right = account != NULL && crypto_equal(hash, account->hash, SHA256_SIZE);
    // the hash of a right password is as good as the saved one, so it isn't left on the stack
    wipe(hash, sizeof(hash));
    return right;
}

// is there an account with this name?
bool auth_exists (const char* name) {
    load();
    return find(name) >= 0;
}

// call fn once for each account's name, in the order they are in the file
void auth_list (void (*fn)(const char* name, void* ctx), void* ctx) {
    load();
    for (int i = 0; i < account_count; i++) {
        fn(accounts[i].name, ctx);
    }
}

// a short, plain message for one of the AUTH_ codes, for showing to the user
const char* auth_strerror (int error) {
    switch (error) {
        case AUTH_OK:            return "ok";
        case AUTH_ERR_NAME:      return "names are 1 to 8 of a-z, 0-9, _ and -";
        case AUTH_ERR_EXISTS:    return "that user already exists";
        case AUTH_ERR_FULL:      return "too many users";
        case AUTH_ERR_NOT_FOUND: return "no such user";
        default:                 return "couldn't save the accounts file";
    }
}


// ---- wrong passwords ------------------------------------------------------------------------

// count one more wrong password for an account and save it straight away.
// Returns the new count (0 if there is no such user)
static uint32_t count_wrong_password (const char* name) {
    load();
    int index = find(name);
    if (index < 0) {
        return 0;
    }
    accounts[index].wrong_passwords++;
    // saved before anything else happens, so pulling the plug can't undo the count
    save();
    return accounts[index].wrong_passwords;
}

// a good login: forget the wrong passwords. Returns how many there were
static uint32_t forget_wrong_passwords (const char* name) {
    load();
    int index = find(name);
    // nothing to forget means nothing to save, so the usual login doesn't write to the disk
    if (index < 0 || accounts[index].wrong_passwords == 0) {
        return 0;
    }
    uint32_t how_many = accounts[index].wrong_passwords;
    accounts[index].wrong_passwords = 0;
    save();
    return how_many;
}

// what the progress bar needs while a home folder is wiped: the bar, and how far along it is
typedef struct {
    ui_progress_t bar;
    uint32_t files_done;
} wipe_progress_t;

// fs_empty_dir() calls this after each file it wipes, to move the progress bar along.
// `context` is the wipe_progress_t that wipe_home() handed to fs_empty_dir
static void wipe_progress_step (const char* name, void* context) {
    wipe_progress_t* progress = context;
    progress->files_done++;
    ui_progress_update(&progress->bar, progress->files_done, name);
}

// overwrite every file in a home folder with zeros and delete it, with a progress bar.
// Returns how many files were wiped
static int wipe_home (const char* home) {
    // counted first, so the progress bar knows where its end is. An empty folder (or an error,
    // below 0) needs no bar at all
    int total = fs_count_files(home);
    if (total <= 0) {
        return 0;
    }

    wipe_progress_t progress = { { 0, 0, 0 }, 0 };
    bool zero_files_first = true; // the secure wipe: the contents can't be read off the disk later
    ui_progress_begin(&progress.bar, (uint32_t)total);
    int wiped = fs_empty_dir(home, zero_files_first, wipe_progress_step, &progress);
    ui_progress_end(&progress.bar);
    return wiped;
}

// after a wrong password for an account that exists: a warning, or, on every third one in a
// row, wipe the home folder
static void after_wrong_password (const char* name, uint32_t wrong_passwords) {
    // an account with no home folder still gets the warnings, there is just nothing to wipe
    char home[FS_PATH_MAX];
    bool has_home = auth_home(name, home, sizeof(home)) && fs_dir_exists(home);

    // where this one falls in the current run of three: 1, 2, then 0 on the third
    uint32_t in_this_run = wrong_passwords % AUTH_MAX_FAILURES;

    // 1st or 2nd of a run: just the warning
    if (in_this_run != 0) {
        colour(YELLOW, BLACK);
        prtf("  Warning: that is %u of %u wrong passwords in a row for %s.\n",
               in_this_run, AUTH_MAX_FAILURES, name);
        if (has_home) {
            prtf("  After %u, everything in %s is deleted.\n", AUTH_MAX_FAILURES, home);
        }
        return;
    }

    // the 3rd (or 6th, 9th...): wipe the home folder
    colour(LIGHT_RED, BLACK);
    prtf("  %u wrong passwords in a row for %s.\n", AUTH_MAX_FAILURES, name);
    if (has_home) {
        // below 0 is an error from the file system: report it as no files rather than a
        // strange negative number
        int wiped = wipe_home(home);
        if (wiped < 0) {
            wiped = 0;
        }
        // set again after the progress bar (drawn in its own green and grey), to be sure the
        // message comes out red
        colour(LIGHT_RED, BLACK);
        prtf("  The %d file(s) in %s have been securely deleted.\n", wiped, home);
    }
}


// ---- the login screen -----------------------------------------------------------------------

#define TITLE           "aerOS" // the big title at the top of the login screen
#define TITLE_COLOUR    ORANGE
#define TYPING_DELAY_MS 120     // how long each letter of the title takes to "type"

// Draw the title in big letters as if it is being typed: a big "_" cursor runs ahead, and the
// letters appear one at a time behind it, left to right. It runs once, each time the login
// screen is drawn. (Keys pressed meanwhile aren't lost: they turn up in the Username box)
static void type_title (uint8_t x, uint8_t y) {
    const char* title = TITLE;
    char letter[2] = { 0, 0 }; // one letter at a time, as a string for big_text

    // hide the console's blinking cursor while the title types (it would sit in the top left
    // corner). The next console_set_cursor() shows it again
    vga_disable_cursor();

    for (int i = 0; title[i] != '\0'; i++) {
        uint8_t letter_x = (uint8_t)(x + i * BIG_TEXT_COLS);

        // the cursor where the next letter goes, a moment's pause, then the letter over it
        // (big_text draws every cell of a letter, background too, so the cursor is gone)
        big_text(letter_x, y, TITLE_COLOUR, BLACK, "_");
        sleep_ms(TYPING_DELAY_MS);
        letter[0] = title[i];
        big_text(letter_x, y, TITLE_COLOUR, BLACK, letter);
    }

    // the cursor after the last letter for a moment, then take it away
    uint8_t end_x = (uint8_t)(x + slen(title) * BIG_TEXT_COLS);
    big_text(end_x, y, TITLE_COLOUR, BLACK, "_");
    sleep_ms(TYPING_DELAY_MS * 3);
    big_text(end_x, y, TITLE_COLOUR, BLACK, " ");
}

// clear the screen and draw the big title, with a line under it
static void heading (const char* subtitle) {
    colour(BRIGHT, BLACK);
    console_clear();

    // the title in big letters from row 1 (big_text is in drivers/video/VGA_linear.h)
    uint8_t title_row = 1;
    type_title(2, title_row);

    // carry on printing on the line under the title
    console_set_cursor(0, (uint8_t)(title_row + BIG_TEXT_ROWS));
    prtf("  %s\n\n", subtitle);
}

// print a question and read the answer. A secret answer (a password) shows as *
static void ask (const char* question, char* answer, int size, bool secret) {
    colour(WHITE, BLACK);
    prtf("  %s", question);
    colour(BRIGHT, BLACK);
    if (secret) {
        GetSecret(answer, size);
    } else {
        getln(answer, size);
    }
}

// a brand new disk has no accounts yet: ask for the first one, and log in as it
static void create_first_account (void) {
    heading("Welcome! Create the first account for this computer.");

    for (;;) {
        char name[64];
        ask("User name: ", name, sizeof(name), false);
        char* user = strlwr(trim(name)); // names are kept in lower case
        // the same rule as auth_add() uses, checked here so a bad name is caught before the
        // passwords are typed
        if (!valid_name(user, AUTH_HOME_NAME_MAX)) {
            prtf("  Names are 1 to 8 of a-z, 0-9, _ and -\n\n");
            continue;
        }

        // typed twice, as a typo here would lock the only account out of the computer
        char password[PASSWORD_MAX];
        char again[PASSWORD_MAX];
        ask("Password:  ", password, sizeof(password), true);
        ask("Again:     ", again, sizeof(again), true);

        if (password[0] == '\0') {
            prtf("  The passwords can't be empty, try again.\n\n");
            continue;
        }
        if (scmp(password, again) != 0) {
            prtf("  The passwords don't match, try again.\n\n");
            continue;
        }

        ui_busy_begin("Saving the account"); // hashing the password takes a moment
        int result = auth_add(user, password);
        ui_busy_end();
        // the typed passwords are no use once hashed, and mustn't stay in memory
        wipe(password, sizeof(password));
        wipe(again, sizeof(again));

        if (result != AUTH_OK) {
            prtf("  Couldn't create the account: %s\n\n", auth_strerror(result));
            continue;
        }

        // made it: log straight in as the new account
        scpy(auth_user, user, sizeof(auth_user));
        enter_home(user);
        return;
    }
}

// the login screen: keeps asking until someone logs in (or makes the first account)
void auth_login (void) {
    auth_failures_seen = 0;

    // no disk: nowhere to keep accounts, so everyone is guest
    if (!auth_required()) {
        scpy(auth_user, "guest", sizeof(auth_user));
        return;
    }

    // no accounts at all (a new disk, or a damaged file): there is nobody to log in as yet
    load();
    if (account_count == 0) {
        create_first_account();
        return;
    }

    heading("[ ! ] Authentication required");

    // RT mode was picked in the boot options (sys/rt.h): say so, before anyone logs in
    if (rt_on()) {
        colour(YELLOW, BLACK);
        prtf("  [ RT ] Real-time mode is on (experimental)\n\n");
        colour(BRIGHT, BLACK);
    }

    for (;;) {
        char name[64];
        char password[PASSWORD_MAX];
        ask("Username: ", name, sizeof(name), false);
        ask("Password: ", password, sizeof(password), true);
        // lower case, so "Alex" logs in as alex
        char* user = strlwr(trim(name));

        // PBKDF2 takes about a second, so show that something is happening
        ui_busy_begin("Checking the password");
        bool right = auth_check(user, password);
        ui_busy_end();
        wipe(password, sizeof(password));

        if (right) {
            auth_failures_seen = forget_wrong_passwords(user); // the terminal banner reports them
            scpy(auth_user, user, sizeof(auth_user));
            enter_home(user);
            return;
        }

        // wrong: wait a moment, count it, and say so. The message doesn't say whether it was
        // the name or the password that was wrong, so it gives nothing away
        sleep_ms(WRONG_PASSWORD_DELAY_MS);
        uint32_t wrong_passwords = count_wrong_password(user);
        colour(LIGHT_RED, BLACK);
        prtf("  Wrong user name or password.\n");
        // 0 means the name doesn't exist, so there is no account to warn about or wipe
        if (wrong_passwords > 0) {
            after_wrong_password(user, wrong_passwords);
        }
        prtf("\n");
    }
}
