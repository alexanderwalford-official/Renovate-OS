# Accounts and encryption

AerOS keeps passwords safe with standard, well tested cryptography, written from the
published standards and checked against their official test results every time AerOS
starts (the boot log line `[ OK ] Crypto self-test`).

## Accounts

You need to log in only in DISK mode, because accounts are saved on the disk. The first
time, AerOS asks you to make an account. After that the login screen appears at every
start.

**Your password is never saved.** AerOS saves a *hash* of it instead: a fingerprint that
can be checked but not turned back into the password. The accounts live in `/acc/users`,
one line each:

```
alex:100000:ddc76e83...:7bf4cd0a...
 |     |       |          |
 name  rounds  salt       hash of the password
```

To check a login, the password you type goes through the same steps and the two hashes are
compared.

| Protection | How |
| --- | --- |
| no password on disk | only a salted PBKDF2 hash is kept |
| same password, new hash | each user gets a random salt |
| slow to guess | 100 000 rounds of hashing per try |
| slow at the keyboard | a 1 second pause after a wrong try |
| no hints | "Wrong user name or password", either way |
| hidden typing | passwords show as `*` and stay out of the history |

Commands: `whoami`, `users`, `adduser`, `deluser`, `passwd`, `logout`.

## Encrypted files

`encrypt notes.txt` locks a file with a password. It keeps its name, only its contents
change. `rd notes.txt` asks for the password and shows it without unlocking it, and
`decrypt notes.txt` turns it back into a normal file.

**There is no way back without the password.** Nobody can unlock a file whose password is
lost, not even AerOS.

### What happens

```
password + salt --PBKDF2, 100 000 rounds--> master key
master key --HMAC "file key"--> AES key
master key --HMAC "file mac"--> check key

contents --AES-256-CTR, AES key--> scrambled contents
everything so far --HMAC-SHA-256, check key--> seal (32 bytes)
```

The file then looks like this, 68 bytes longer than before:

```
+----------+--------+------+-------+--------------------+------+
| AERENC01 | rounds | salt | nonce | scrambled contents | seal |
+----------+--------+------+-------+--------------------+------+
   8 bytes    4       16     8       same size as before   32
```

To open it, AerOS makes the keys again from your password and the salt, and checks the seal
**before** unscrambling anything. A wrong password gives different keys, so the seal doesn't
match. A file that has been changed doesn't match either. Both say "wrong password, or the
file has been changed".

After encrypting, the old unscrambled contents are overwritten with zeros on the disk, not
just marked as free space.

## The algorithms

| Name | Standard | Job |
| --- | --- | --- |
| SHA-256 | FIPS 180-4 | the fingerprint everything uses |
| HMAC-SHA-256 | RFC 2104 | a fingerprint that needs a key |
| PBKDF2 | RFC 8018 | turns a password into a key, slowly |
| AES-256 | FIPS 197 | the cipher that scrambles files |
| CTR mode | NIST SP 800-38A | lets AES scramble any length |

Salts and nonces come from a random number generator that mixes the CPU's own hardware
random numbers with the exact timing of key presses and timer ticks.

## What it doesn't protect

- **No file permissions yet**: anyone who can log in can read or delete any file, including
  `/acc/users`. Encrypted files stay safe, because they have their own passwords.
- **The disk isn't encrypted**: on another computer, file names, sizes and unencrypted files
  can be read. Use long passwords, because the hashes in `/acc/users` can be guessed at.
- **Older copies**: files saved many times before being encrypted can leave old copies in
  free space on the disk. Encrypt secrets early.
- **No administrators**: every user can add and delete users.

The full design, with every algorithm explained step by step, is in `docs/SECURITY.md` in
the AerOS source, and in the comments of `src/lib/crypto.c`.
