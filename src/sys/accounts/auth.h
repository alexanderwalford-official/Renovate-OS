/*
    auth.h
    User accounts and logging in.

    Accounts are kept in /acc/users, one line per user:

        name:iterations:salt:hash

    The password itself is never stored. Instead PBKDF2 (lib/crypto.c) turns the password and a
    random salt into a hash, thousands of rounds deep. To log in, the typed password goes through
    the same steps and the two hashes are compared. The salt means two users with the same
    password still get different hashes, and the rounds make guessing passwords slow.

    Accounts need somewhere to be saved, so logging in is only required in disk mode. With no
    AEROS disk (and in demo mode) everyone is "guest".
*/
#ifndef AUTH_H
#define AUTH_H

#include <stdbool.h>

#define AUTH_NAME_MAX 16

#define AUTH_OK             0
#define AUTH_ERR_NAME      -1 // names are 1 to 16 of a-z, 0-9, _ and -
#define AUTH_ERR_EXISTS    -2
#define AUTH_ERR_FULL      -3
#define AUTH_ERR_NOT_FOUND -4
#define AUTH_ERR_SAVE      -5

// who is logged in ("guest" when accounts aren't in use)
extern char auth_user[AUTH_NAME_MAX + 1];

// true when there is a disk to keep accounts on, so logging in is required
bool auth_required (void);

// show the login screen until someone logs in (or, the very first time, set up the first account)
void auth_login (void);

int auth_add (const char* name, const char* password);
int auth_delete (const char* name);
int auth_set_password (const char* name, const char* password);
bool auth_check (const char* name, const char* password);
bool auth_exists (const char* name);

// call fn with every user name
void auth_list (void (*fn)(const char* name, void* ctx), void* ctx);

const char* auth_strerror (int error);

#endif /* AUTH_H */
