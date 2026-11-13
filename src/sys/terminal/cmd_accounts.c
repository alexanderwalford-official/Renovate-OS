/*
    cmd_accounts.c - the terminal's commands for user accounts, and what programs may do

        ME, USRS             who is logged in, and every account
        MKU, DELU            make an account (with a home folder of its own), delete another
        PW                   change your own password
        OUT                  log out, back to the login screen
        PERM                 the programs you have always allowed to use your files (sys/perm.h)

    How passwords are kept, and the login screen itself, are in sys/accounts/auth.c: these
    commands ask the questions and pass the answers on, and wipe every password from memory
    once it has been used. Each command is a row in the table in t_int.c, and commands.h says
    how the files of the terminal fit together.
*/

#include "commands.h"
#include <stdint.h>
#include <stddef.h>
#include "../../lib/primary_definitions.h"
#include "../../lib/crypto.h"
#include "../../drivers/video/VGA_linear.h"
#include "../../drivers/input/IO.h"
#include "../f_sys.h"
#include "../ui.h"
#include "../explain.h"
#include "../perm.h"
#include "../accounts/auth.h"
#include "../../lang/aerlang.h"


// =================================================================================================
//  Accounts (how passwords are kept: sys/accounts/auth.h)
// =================================================================================================

// the account commands need a disk to keep the accounts on. Without one, say so and return false
static bool accounts_available (void) {
    if (auth_required()) {
        return true;
    }
    prtf("Accounts need an AEROS disk, without one everyone is guest.\n");
    return false;
}

// ask for a new password twice. False, after saying why, if it was empty or they didn't match
static bool ask_new_password (char* password, int size) {
    char again[CLI_PASSWORD_MAX];
    ask_secret("New password: ", password, size);
    ask_secret("Again:        ", again, sizeof(again));

    // `again` is only needed for the comparison, so it is wiped straight away. `password`
    // is the caller's, and the caller wipes it once it has been used
    bool empty = password[0] == '\0';
    bool match = scmp(password, again) == 0;
    wipe(again, sizeof(again));

    if (empty) {
        prtf("The passwords can't be empty, nothing was changed.\n");
        return false;
    }
    if (!match) {
        prtf("The passwords don't match, nothing was changed.\n");
        return false;
    }
    return true;
}

// before doing something drastic, ask for the logged in user's own password.
// True if it was right
static bool confirm_own_password (void) {
    char password[CLI_PASSWORD_MAX];
    ask_secret("Your password: ", password, sizeof(password));

    ui_busy_begin("Checking the password");
    bool right = auth_check(auth_user, password);
    ui_busy_end();
    wipe(password, sizeof(password));

    if (!right) {
        prtf("Wrong password.\n");
    }
    return right;
}

// ME: print who is logged in
void cli_whoami (char* arguments) {
    (void)arguments;
    prtf("%s\n", auth_user);
}

// auth_list() calls this for every account
static void print_user (const char* name, void* context) {
    (void)context;
    if (seq(name, auth_user)) {
        prtf("%s  (you)\n", name);
    } else {
        prtf("%s\n", name);
    }
}

// USRS: list every account
void cli_users (char* arguments) {
    (void)arguments;
    if (accounts_available()) {
        auth_list(print_user, NULL);
    }
}

// MKU <NAME>: make a new account, with a home folder of its own. Its password is asked for
// twice, to catch typing mistakes
void cli_add_user (char* arguments) {
    if (!accounts_available()) {
        return;
    }
    char* name = strlwr(arguments); // names are kept in lower case
    if (name[0] == '\0') {
        show_usage(NULL);
        return;
    }
    // check the name before asking for a password, not after
    if (!auth_valid_new_name(name)) {
        prtf("Can't use that name: %s\n", auth_strerror(AUTH_ERR_NAME));
        return;
    }
    if (auth_exists(name)) {
        prtf("%s already exists.\n", name);
        return;
    }

    char password[CLI_PASSWORD_MAX];
    if (ask_new_password(password, sizeof(password))) {
        ui_busy_begin("Saving the account"); // hashing the password takes a moment
        int result = auth_add(name, password);
        ui_busy_end();

        if (result == AUTH_OK) {
            char home[FS_PATH_MAX];
            if (auth_home(name, home, sizeof(home))) {
                prtf("Made the account %s, with the home folder %s.\n", name, home);
            } else {
                prtf("Made the account %s.\n", name);
            }
        } else {
            prtf("Couldn't make the account: %s\n", auth_strerror(result));
        }
    }
    wipe(password, sizeof(password));
}

// DELU <NAME>: delete someone else's account (your own password is asked for first, so nobody
// can do it at a computer you left logged in). Their home folder is left as it is
void cli_delete_user (char* arguments) {
    if (!accounts_available()) {
        return;
    }
    char* name = strlwr(arguments); // names are kept in lower case, so DELU ALEX finds alex
    if (name[0] == '\0') {
        show_usage(NULL);
        return;
    }
    if (seq(name, auth_user)) {
        prtf("You can't delete the account you are logged in as.\n");
        return;
    }
    if (!auth_exists(name)) {
        prtf("There is no user called %s.\n", name);
        return;
    }
    if (!confirm_own_password()) {
        return;
    }

    if (auth_delete(name) == AUTH_OK) {
        prtf("Deleted %s.\n", name);
    } else {
        prtf("Couldn't delete %s.\n", name);
    }
}

// PW: change your own password. The old one is asked for first, then the new one twice
void cli_password (char* arguments) {
    (void)arguments;
    if (!accounts_available() || !confirm_own_password()) {
        return;
    }

    char password[CLI_PASSWORD_MAX];
    if (ask_new_password(password, sizeof(password))) {
        ui_busy_begin("Saving the password");
        int result = auth_set_password(auth_user, password);
        ui_busy_end();

        if (result == AUTH_OK) {
            prtf("Password changed.\n");
        } else {
            prtf("Couldn't save the new password.\n");
        }
    }
    wipe(password, sizeof(password));
}

// OUT: log out, back to the login screen
void cli_logout (char* arguments) {
    (void)arguments;
    // without a disk there is no login screen to go back to: everyone is guest
    if (!accounts_available()) {
        return;
    }
    // the next person shouldn't be able to see what this one did: forget the commands for
    // the up arrow, the output for Page Up, the last problem (WHY) and the AerLang variables
    // made at the prompt, and start them in the top folder
    ClearInputHistory();
    console_forget_scrollback();
    explain_forget();
    aer_shell_stop();
    scpy(cd, "/", sizeof(cd));
    cli_logged_in = false; // ends the inner loop in cli_init()
}


// ---- PERM: what programs may always do with your files (the rules: sys/perm.h) ---------------------

// perm_each() calls this for every "always" answer
static void print_permission (int number, const char* program, perm_kind_t kind, const char* folder, void* context) {
    (void)context;
    prtf("  %2d. %-26s may %-6s files in %s\n", number, program, kind == PERM_CHANGE ? "change" : "read", folder);
}

// PERM [DEL <N>]: list the programs you answered A (always) for when they asked to use your
// files. PERM DEL 2 forgets the second answer, PERM DEL ALL forgets them all
void cli_permissions (char* arguments) {
    char* rest;
    char* word = split_first_word(arguments, &rest);

    // PERM on its own: the list, numbered for PERM DEL
    if (word[0] == '\0') {
        prtf("Programs you have always allowed:\n");
        if (perm_each(print_permission, NULL) == 0) {
            prtf("  none yet. Programs ask before they read your files or change any file,\n"
                 "  and the ones you answer A (always) are listed here.\n");
        } else {
            prtf("%s DEL <number> forgets one, %s DEL ALL forgets them all.\n",
                 command_name(cli_permissions), command_name(cli_permissions));
        }
        return;
    }

    // PERM DEL <N> or PERM DEL ALL. perm_forget(0) forgets every answer, but atoi also gives
    // 0 for anything that isn't a number, so "PERM DEL x" is turned away below rather than
    // forgetting everything
    bool all = strcasecmp(rest, "all") == 0;
    int number = all ? 0 : atoi(rest);
    if (strcasecmp(word, "del") != 0 || (!all && number < 1)) {
        show_usage("e.g. PERM DEL 2, or PERM DEL ALL");
        return;
    }
    int forgotten = perm_forget(number);
    if (forgotten == 0) {
        prtf("There is no answer %s to forget (%s lists them).\n", rest, command_name(cli_permissions));
    } else {
        prtf("Forgot %d answer(s): those programs will ask again.\n", forgotten);
    }
}
