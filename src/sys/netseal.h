/*
    netseal.h - sealed messages: the only thing AerOS sends over a network

    AerOS never sends anything readable over a network. Every message is sealed with a key
    both ends share, which they make from the same password:

        password --PBKDF2, 100 000 rounds, salt "AerOS network"--> shared key (kept by the firewall)
        shared key --HMAC "AerOS net key"--> AES key
        shared key --HMAC "AerOS net mac"--> check key

    and goes in a UDP packet like this:

        +------+-------+------------------------------+------+
        | AERN | nonce | message, AES-256-CTR         | tag  |
        +------+-------+------------------------------+------+
           4      8      same length as the message     32

    The nonce is 8 new random bytes for every message, so the same message never looks the same
    twice. The tag is HMAC-SHA-256 with the check key over everything in front of it. A message
    is only accepted if its tag matches, which proves it was sealed by someone who knows the
    password and wasn't changed on the way. Anything else is thrown away unread.

    This is the same construction as encrypted files (sys/fcrypt.h): encrypt, then MAC. The
    tools/aernet.py script on another computer speaks it too.

    Not covered: an old message recorded and sent again (a "replay") is accepted again, and
    the password is the only thing that says who is at the other end.
*/
#ifndef NETSEAL_H
#define NETSEAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define NETSEAL_KEY_SIZE  32
#define NETSEAL_OVERHEAD  (4 + 8 + 32)   // what sealing adds to a message: "AERN", nonce, tag

// make the shared key from a password. Slow on purpose (PBKDF2), so guessing is slow too
void netseal_key (const char* password, uint8_t key[NETSEAL_KEY_SIZE]);

// seal `length` bytes of message into `out`, which needs length + NETSEAL_OVERHEAD bytes.
// Returns the sealed packet's length
size_t netseal_seal (const uint8_t key[NETSEAL_KEY_SIZE], const void* message, size_t length, uint8_t* out);

// open a sealed packet: the message goes into `out` (room for length - NETSEAL_OVERHEAD
// bytes) and its length into *out_length. False, with nothing in out, if the packet isn't a
// sealed message, wasn't sealed with this key, or was changed on the way
bool netseal_open (const uint8_t key[NETSEAL_KEY_SIZE], const uint8_t* packet, size_t length,
                   uint8_t* out, size_t* out_length);

#endif /* NETSEAL_H */
