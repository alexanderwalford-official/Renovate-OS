/*
    auth.h
    User accounts, home folders and logging in.

    Accounts are kept in /acc/users, one line per user:

        name:iterations:salt:hash:wrong passwords

    The password itself is never stored. Instead PBKDF2 (lib/crypto.c) turns the password and a
    random salt into a hash, thousands of rounds deep. To log in, the typed password goes through
    the same steps and the two hashes are compared. The salt means two users with the same
    password still get different hashes, and the rounds make guessing passwords slow.

    Every account has a home folder, /home/<name>/, where the terminal starts after logging in.
    After AUTH_MAX_FAILURES wrong passwords in a row for an account, everything in its home
    folder is securely deleted (the first ones only bring a warning). See auth.c for the details.

    Accounts need somewhere to be saved, so logging in is only required in disk mode. With no
    AEROS disk (and in demo mode) everyone is "guest".
*/
#ifndef AUTH_H
#define AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUTH_NAME_MAX      16 // longest name in the accounts file (older accounts)
#define AUTH_HOME_NAME_MAX 8  // longest name for a new account: it has to fit as a folder name (8.3)
#define AUTH_MAX_FAILURES  3  // wrong passwords in a row before the home folder is wiped

#define AUTH_OK             0
#define AUTH_ERR_NAME      -1 // names are 1 to 8 of a-z, 0-9, _ and -
#define AUTH_ERR_EXISTS    -2
#define AUTH_ERR_FULL      -3
#define AUTH_ERR_NOT_FOUND -4
#define AUTH_ERR_SAVE      -5

// who is logged in ("guest" when accounts aren't in use)
extern char auth_user[AUTH_NAME_MAX + 1];

// how many wrong passwords were typed for this account before the last login (the terminal
// banner shows it, so you know if someone has been trying)
extern uint32_t auth_failures_seen;

// true when there is a disk to keep accounts on, so logging in is required
bool auth_required (void);

// show the login screen until someone logs in (or, the very first time, set up the first account),
// then make the user's home folder the current directory
void auth_login (void);

// can a new account have this name? (1 to 8 of a-z, 0-9, _ and -)
bool auth_valid_new_name (const char* name);

// make a new account and its home folder, returns AUTH_OK or an AUTH_ERR_ code
// (name must already be lower case)
int auth_add (const char* name, const char* password);
// delete an account (the caller decides who is allowed to, see DELU in sys/terminal/cmd_accounts.c); its home
// folder is left as it is
int auth_delete (const char* name);
// give an existing account a new password (and a new salt)
int auth_set_password (const char* name, const char* password);
// is this the right password for this user? Takes as long for unknown users as for real ones
bool auth_check (const char* name, const char* password);
bool auth_exists (const char* name);

// the user's home folder, "/home/<name>/", false if the name is too long to have one
bool auth_home (const char* name, char* out, size_t size);

// call fn with every user name, ctx is passed through to it
void auth_list (void (*fn)(const char* name, void* ctx), void* ctx);

// a short description of an AUTH_ result, for messages
const char* auth_strerror (int error);

#endif /* AUTH_H */
