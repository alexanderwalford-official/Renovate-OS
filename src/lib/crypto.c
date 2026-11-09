/*
    crypto.c - SHA-256, HMAC, PBKDF2, AES-256, CTR mode and random numbers (see crypto.h)

    Everything here is written from the published standards, with no tables or code copied from
    elsewhere, and is checked against the standards' own test vectors by crypto_selftest() at
    every boot. docs/SECURITY.md explains how the pieces fit together, with diagrams.

    The file is in the order the pieces build on each other:

        SHA-256  ->  HMAC  ->  PBKDF2        (hashing: passwords and tamper checks)
        AES-256  ->  CTR mode                (encryption)
        random numbers                        (salts and nonces, uses SHA-256)
        helpers, self-test

    Two habits run through all of it:
      - every buffer that held a secret is wiped with wipe() before it goes out of scope,
        so keys and passwords don't linger in memory for something else to find later
      - nothing branches on secret data, so how long an operation takes says nothing about it
*/

#include "crypto.h"
#include "aer_string.h"
#include "busy.h"

// ---- small helpers ----------------------------------------------------------------------------

// SHA-256 and AES both treat 4 bytes as one big endian number (first byte = most significant)
static uint32_t load_be32 (const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void store_be32 (uint8_t* p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

// rotate right: the bits that fall off the bottom come back in at the top
static uint32_t ror (uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

// ---- SHA-256 (FIPS 180-4) ---------------------------------------------------------------------
//
// SHA-256 turns any amount of data into a 32 byte "digest". The data is cut into 64 byte blocks
// and each block is stirred into a state of eight 32 bit words (a to h) by 64 rounds of adding,
// rotating and mixing. The final state is the digest.
//
//     state = 8 fixed starting words
//     for each 64 byte block:  state = state + 64 rounds(state, block)
//     digest = state, written out as 32 bytes
//
// The last block is padded so every message ends the same way: a 1 bit, zeros, and the
// message's length in bits. That stops two different messages from padding into the same blocks.

// the round constants: the first 32 bits of the fractional parts of the cube roots of the first
// 64 primes (a "nothing up my sleeve" choice, nobody could have picked them to hide a weakness)
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

// stir one 64 byte block into the state
static void sha256_block (uint32_t state[8], const uint8_t* block) {
    // the "message schedule": the block's 16 words, stretched to 64 by mixing earlier words
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = load_be32(block + i * 4);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    // 64 rounds. Each round works out two values from the current words, then shifts every word
    // along one place (h falls off, a gets the new value) like a conveyor belt:
    //   choose:   (e & f) ^ (~e & g)          for each bit, e picks f or g
    //   majority: (a & b) ^ (a & c) ^ (b & c)  each bit is whatever most of a, b and c have
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    // adding the old state back in is what makes the step impossible to run backwards
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    wipe(w, sizeof(w));
}

void sha256_init (sha256_t* s) {
    // the starting state: the first 32 bits of the fractional parts of the square roots of the
    // first 8 primes
    static const uint32_t initial[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    memcpy(s->state, initial, sizeof(initial));
    s->length = 0;
    s->used = 0;
}

// data can arrive in any size of piece: it is collected in s->block and every full 64 bytes
// are stirred in straight away
void sha256_update (sha256_t* s, const void* data, size_t length) {
    const uint8_t* p = data;
    s->length += length;
    while (length > 0) {
        uint32_t take = 64 - s->used;
        if (take > length) {
            take = (uint32_t)length;
        }
        memcpy(s->block + s->used, p, take);
        s->used += take;
        p += take;
        length -= take;
        if (s->used == 64) {
            sha256_block(s->state, s->block);
            s->used = 0;
        }
    }
}

// pad, stir in the last block(s) and write the digest. The context is wiped afterwards
void sha256_final (sha256_t* s, uint8_t out[SHA256_SIZE]) {
    // padding: one 1 bit (the byte 0x80), zeros until the block has 8 bytes left, then the
    // message length in bits as a 64 bit big endian number
    //
    //     | message ... | 0x80 | 0x00 0x00 ... | length in bits (8 bytes) |
    //                                           ^ ends exactly on a 64 byte boundary
    uint64_t bits = s->length * 8;
    static const uint8_t one_bit = 0x80, zero = 0;
    sha256_update(s, &one_bit, 1);
    while (s->used != 56) {
        sha256_update(s, &zero, 1);
    }
    uint8_t length[8];
    store_be32(length, (uint32_t)(bits >> 32));
    store_be32(length + 4, (uint32_t)bits);
    sha256_update(s, length, 8);

    for (int i = 0; i < 8; i++) {
        store_be32(out + i * 4, s->state[i]);
    }
    wipe(s, sizeof(*s));
}

void sha256 (const void* data, size_t length, uint8_t out[SHA256_SIZE]) {
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, data, length);
    sha256_final(&s, out);
}

// ---- HMAC-SHA-256 (RFC 2104) ------------------------------------------------------------------
//
// A plain hash can be worked out by anyone. HMAC mixes in a secret key, so only someone who has
// the key can make the right answer, and anyone changing the data without the key can't fix the
// HMAC up to match. It hashes twice, with the key mixed in differently each time:
//
//     HMAC(key, data) = SHA-256( (key ^ opad) || SHA-256( (key ^ ipad) || data ) )
//
//     key is padded with zeros to 64 bytes (or hashed first if it is longer)
//     ipad = 64 bytes of 0x36, opad = 64 bytes of 0x5c, || means "followed by"
//
// The two padded keys are each exactly one SHA-256 block, so hmac_init() hashes them once and
// keeps the two half finished SHA-256 states. Every HMAC with the same key then starts from a
// copy of those states instead of hashing the key again, which roughly halves the work in
// PBKDF2 (tens of thousands of HMACs with the same key).

void hmac_init (hmac_t* h, const void* key, size_t key_length) {
    uint8_t block[64];
    memset(block, 0, sizeof(block));
    if (key_length > 64) {
        sha256(key, key_length, block); // long keys are hashed down to 32 bytes first
    } else {
        memcpy(block, key, key_length);
    }

    uint8_t pad[64];
    for (int i = 0; i < 64; i++) pad[i] = block[i] ^ 0x36;
    sha256_init(&h->inner);
    sha256_update(&h->inner, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = block[i] ^ 0x5c;
    sha256_init(&h->outer);
    sha256_update(&h->outer, pad, 64);

    wipe(block, sizeof(block));
    wipe(pad, sizeof(pad));
}

// one HMAC over first || second (second may be empty), starting from a prepared context, which
// is left unchanged so it can be used again. out may be the same buffer as first
static void hmac_run (const hmac_t* h, const void* first, size_t first_length,
                      const void* second, size_t second_length, uint8_t out[SHA256_SIZE]) {
    uint8_t inner[SHA256_SIZE];
    sha256_t s = h->inner;
    sha256_update(&s, first, first_length);
    sha256_update(&s, second, second_length);
    sha256_final(&s, inner);

    s = h->outer;
    sha256_update(&s, inner, sizeof(inner));
    sha256_final(&s, out);
    wipe(inner, sizeof(inner));
}

void hmac_sha256 (const void* key, size_t key_length, const void* data, size_t length, uint8_t out[SHA256_SIZE]) {
    hmac_t h;
    hmac_init(&h, key, key_length);
    hmac_run(&h, data, length, 0, 0, out);
    wipe(&h, sizeof(h));
}

// ---- PBKDF2-HMAC-SHA-256 (RFC 8018) -----------------------------------------------------------
//
// Turns a password into a key. It is deliberately slow: the HMAC is run "iterations" times in a
// chain, so anyone guessing passwords has to do all that work for every single guess.
//
//     U1 = HMAC(password, salt || block number)       (block number: 4 bytes, starting at 1)
//     U2 = HMAC(password, U1)
//     ...
//     Un = HMAC(password, Un-1)                        n = iterations
//     block = U1 ^ U2 ^ ... ^ Un                       32 bytes of key
//
// More than 32 bytes of key just means more blocks (block number 2, 3...). The salt is random
// and different for every password, so the same password never gives the same key twice and
// an attacker can't work out a table of common passwords in advance.

void pbkdf2_sha256 (const void* password, size_t password_length, const void* salt, size_t salt_length,
                    uint32_t iterations, uint8_t* out, size_t length) {
    hmac_t h;
    hmac_init(&h, password, password_length); // the password is the HMAC key every time

    for (uint32_t block = 1; length > 0; block++) {
        uint8_t u[SHA256_SIZE], t[SHA256_SIZE], number[4];
        store_be32(number, block);
        hmac_run(&h, salt, salt_length, number, sizeof(number), u); // U1
        memcpy(t, u, sizeof(t));
        for (uint32_t i = 1; i < iterations; i++) {
            if ((i & 1023) == 0) {
                busy(); // this is the slow part: let a spinner turn
            }
            hmac_run(&h, u, sizeof(u), 0, 0, u); // U(i+1) from Ui
            for (int k = 0; k < SHA256_SIZE; k++) t[k] ^= u[k];
        }

        size_t take = length < SHA256_SIZE ? length : SHA256_SIZE;
        memcpy(out, t, take);
        out += take;
        length -= take;
        wipe(u, sizeof(u));
        wipe(t, sizeof(t));
    }
    wipe(&h, sizeof(h));
}

// ---- AES-256 (FIPS 197) -----------------------------------------------------------------------
//
// AES encrypts one 16 byte block at a time with a key (32 bytes for AES-256). The block is laid
// out as a 4 x 4 grid of bytes, filled column by column, and goes through 14 rounds:
//
//     add round key 0
//     rounds 1 to 13:  SubBytes -> ShiftRows -> MixColumns -> add round key
//     round 14:        SubBytes -> ShiftRows ->               add round key
//
//     SubBytes     swap every byte for another using a fixed table (the S-box), the only step
//                  that isn't simple xor-ing, it is what makes AES hard to undo without the key
//     ShiftRows    rotate row r of the grid r places to the left, so each column's bytes get
//                  spread over all four columns
//     MixColumns   mix the 4 bytes of each column together, so a change to one byte spreads
//                  to the whole column. After a few rounds every output byte depends on every
//                  input byte
//     add round key  xor in 16 bytes of the expanded key (a different 16 for every round)
//
// Only encryption is needed: CTR mode (below) uses AES encryption for decrypting too.

static uint8_t sbox[256];
static bool sbox_ready = false;

static uint8_t rotl8 (uint8_t x, int n) {
    return (uint8_t)((x << n) | (x >> (8 - n)));
}

// multiply by 2 in GF(2^8), the field AES does its maths in: shift left, and if a bit fell off
// the top, xor with 0x1B (the field's reduction polynomial x^8 + x^4 + x^3 + x + 1, minus x^8)
static uint8_t xtime (uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0));
}

// The S-box maps each byte to its multiplicative inverse in GF(2^8), then scrambles the bits
// with a fixed "affine transform" (xor of the byte with four rotations of itself, then 0x63).
// Rather than typing in a 256 entry table (and risking a typo), it is worked out at start up:
// p walks through every non zero byte by repeatedly multiplying by 3 (which reaches all 255 of
// them), while q walks backwards dividing by 3, so q is always the inverse of p.
static void aes_build_sbox (void) {
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ xtime(p)); // p * 3 = p * 2 + p
        // q / 3: these shifts and xors undo one "multiply by 3" (the well known short form of it,
        // see https://en.wikipedia.org/wiki/Rijndael_S-box)
        q ^= (uint8_t)(q << 1);
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80) {
            q ^= 0x09;
        }
        sbox[p] = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
    } while (p != 1);
    sbox[0] = 0x63; // 0 has no inverse, the standard defines its entry as 0x63
    sbox_ready = true;
}

// SubBytes on each of the 4 bytes of a word (used by the key expansion)
static uint32_t sub_word (uint32_t w) {
    return ((uint32_t)sbox[w >> 24] << 24) | ((uint32_t)sbox[(w >> 16) & 0xFF] << 16) |
           ((uint32_t)sbox[(w >> 8) & 0xFF] << 8) | sbox[w & 0xFF];
}

// Key expansion: stretch the 32 byte key into 15 round keys of 16 bytes (60 words).
// Words 0-7 are the key itself, and every word after that is the word 8 places back xor-ed
// with the word just before it, with extra mixing every 4th word:
//     every 8th word: rotate one byte left, SubBytes, xor the round constant (1, 2, 4, 8...)
//     the 4th word in between: SubBytes only
void aes256_init (aes256_t* aes, const uint8_t key[32]) {
    if (!sbox_ready) {
        aes_build_sbox();
    }
    uint32_t* w = aes->round_keys;
    for (int i = 0; i < 8; i++) {
        w[i] = load_be32(key + i * 4);
    }
    uint8_t rcon = 1;
    for (int i = 8; i < 60; i++) {
        uint32_t t = w[i - 1];
        if (i % 8 == 0) {
            t = sub_word((t << 8) | (t >> 24)) ^ ((uint32_t)rcon << 24);
            rcon = xtime(rcon);
        } else if (i % 8 == 4) {
            t = sub_word(t);
        }
        w[i] = w[i - 8] ^ t;
    }
}

// xor the 4 words of one round key into the grid (column c is bytes 4c to 4c + 3)
static void add_round_key (uint8_t* s, const uint32_t* k) {
    for (int c = 0; c < 4; c++) {
        s[c * 4] ^= (uint8_t)(k[c] >> 24);
        s[c * 4 + 1] ^= (uint8_t)(k[c] >> 16);
        s[c * 4 + 2] ^= (uint8_t)(k[c] >> 8);
        s[c * 4 + 3] ^= (uint8_t)k[c];
    }
}

void aes256_encrypt_block (const aes256_t* aes, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16]; // the grid, s[c * 4 + r] is row r of column c
    memcpy(s, in, 16);
    add_round_key(s, aes->round_keys);
    for (int round = 1; round <= 14; round++) {
        // SubBytes and ShiftRows in one go: the byte that ends up in row r of column c came
        // from row r of column c + r
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) {
                t[c * 4 + r] = sbox[s[((c + r) % 4) * 4 + r]];
            }
        }
        // MixColumns (not in the last round): each column (a0 a1 a2 a3) is multiplied by a
        // fixed matrix in GF(2^8). Written out, each new byte is
        //     a[i] ^ (a0 ^ a1 ^ a2 ^ a3) ^ 2 * (a[i] ^ a[i + 1])
        // which is the standard 2 3 1 1 matrix rearranged to need only one xtime per byte
        if (round < 14) {
            for (int c = 0; c < 4; c++) {
                uint8_t* a = &t[c * 4];
                uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                uint8_t all = a0 ^ a1 ^ a2 ^ a3;
                a[0] ^= all ^ xtime(a0 ^ a1);
                a[1] ^= all ^ xtime(a1 ^ a2);
                a[2] ^= all ^ xtime(a2 ^ a3);
                a[3] ^= all ^ xtime(a3 ^ a0);
            }
        }
        add_round_key(t, aes->round_keys + round * 4);
        memcpy(s, t, 16);
    }
    memcpy(out, s, 16);
    wipe(s, sizeof(s));
    wipe(t, sizeof(t));
}

// ---- CTR mode ---------------------------------------------------------------------------------
//
// AES on its own only handles 16 bytes. Counter mode turns it into a stream cipher: AES
// encrypts a counter, and the result (the "key stream") is xor-ed with the data.
//
//     counter block n = nonce (8 bytes) || n (8 bytes, big endian), n = 0, 1, 2...
//     data[16n ... 16n + 15] ^= AES(key, counter block n)
//
// xor-ing twice with the same stream gives the original back, so the same function encrypts
// and decrypts, and the output is exactly as long as the input (no padding). The one rule is
// that a key and nonce pair must never be used for two different messages, or xor-ing the two
// encrypted messages together would cancel the key stream out. fcrypt.c makes a fresh random
// key for every file it encrypts, so that can't happen.

void aes256_ctr (const aes256_t* aes, const uint8_t nonce[8], uint8_t* data, size_t length) {
    uint8_t counter[16], stream[16];
    memcpy(counter, nonce, 8);
    for (uint32_t block = 0; length > 0; block++) {
        // the top 4 counter bytes stay 0: 2^32 blocks is 64GB, far more than a file can hold
        store_be32(counter + 8, 0);
        store_be32(counter + 12, block);
        aes256_encrypt_block(aes, counter, stream);
        size_t take = length < 16 ? length : 16;
        for (size_t i = 0; i < take; i++) data[i] ^= stream[i];
        data += take;
        length -= take;
    }
    wipe(stream, sizeof(stream));
}

// ---- random numbers ---------------------------------------------------------------------------
//
// Salts and nonces have to be unpredictable. Computers are built to be predictable, so the
// generator collects small amounts of unpredictability ("entropy") from the outside world and
// hashes it all together:
//
//     key presses and timer ticks  --crypto_add_entropy()-->  entropy (mixed with the exact
//                                                              CPU cycle they arrived on)
//     RDRAND (the CPU's own hardware random generator, on most CPUs since 2012)
//     the CPU cycle counter at the moment of asking
//            |
//            v
//     digest = SHA-256(pool || counter || cycles || entropy || RDRAND)
//     hand out the first 16 bytes of digest
//     pool   = SHA-256(digest)                (so the pool can't be wound back to earlier output)
//
// Even if one source is weak (say an old CPU with no RDRAND), the others still make the output
// unpredictable, and SHA-256 makes sure no pattern in the inputs shows in the output.

static uint8_t pool[SHA256_SIZE];  // carries everything mixed in so far from one call to the next
static uint32_t pool_counter = 0;  // makes every call's input different, even with no new entropy
static volatile uint32_t entropy = 0; // written by interrupt handlers, so volatile

static void cpuid (uint32_t leaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

// the CPU's cycle counter (billions a second): exactly which cycle an interrupt lands on jitters
static uint64_t read_tsc (void) {
    uint64_t t;
    __asm__ volatile ("rdtsc" : "=A"(t));
    return t;
}

// called from the keyboard and timer interrupts, so it is kept to a few instructions
void crypto_add_entropy (uint32_t value) {
    entropy = ((entropy << 7) | (entropy >> 25)) ^ value ^ (uint32_t)read_tsc();
}

// RDRAND, if the CPU has it (CPUID leaf 1, ECX bit 30). It can briefly run dry, in which case
// it says so with the carry flag, so it is tried a few times
static bool hardware_random (uint32_t* value) {
    static int available = -1; // -1 = not checked yet
    if (available < 0) {
        uint32_t a, b, c, d;
        cpuid(1, &a, &b, &c, &d);
        available = (c >> 30) & 1;
    }
    if (!available) {
        return false;
    }
    for (int tries = 0; tries < 10; tries++) {
        uint8_t ok;
        __asm__ volatile ("rdrand %0; setc %1" : "=r"(*value), "=qm"(ok));
        if (ok) {
            return true;
        }
    }
    return false;
}

void crypto_random (void* buffer, size_t length) {
    uint8_t* out = buffer;
    while (length > 0) {
        sha256_t s;
        uint8_t digest[SHA256_SIZE];
        uint64_t tsc = read_tsc();
        uint32_t stir = entropy;
        uint32_t hw = 0;
        sha256_init(&s);
        sha256_update(&s, pool, sizeof(pool));
        sha256_update(&s, &pool_counter, sizeof(pool_counter));
        sha256_update(&s, &tsc, sizeof(tsc));
        sha256_update(&s, &stir, sizeof(stir));
        if (hardware_random(&hw)) {
            sha256_update(&s, &hw, sizeof(hw));
        }
        sha256_final(&s, digest);
        pool_counter++;

        // only half of the digest is handed out, and the pool moves on to the hash of the whole
        // digest, so nobody who sees the output can work out the pool from it
        size_t take = length < 16 ? length : 16;
        memcpy(out, digest, take);
        sha256(digest, sizeof(digest), pool);
        out += take;
        length -= take;
        wipe(digest, sizeof(digest));
    }
}

// ---- helpers ----------------------------------------------------------------------------------

// memcmp stops at the first difference, so the time it takes would tell an attacker how many
// leading bytes of a guessed tag were right. This always looks at every byte
bool crypto_equal (const void* a, const void* b, size_t length) {
    const uint8_t* x = a;
    const uint8_t* y = b;
    uint8_t difference = 0;
    for (size_t i = 0; i < length; i++) difference |= x[i] ^ y[i];
    return difference == 0;
}

void wipe (void* buffer, size_t length) {
    // volatile, so the compiler can't decide the writes are pointless (the buffer is about to
    // go out of scope) and leave them out
    volatile uint8_t* p = buffer;
    while (length--) {
        *p++ = 0;
    }
}

void crypto_to_hex (const uint8_t* bytes, size_t length, char* out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; i++) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 15];
    }
    out[length * 2] = '\0';
}

static int hex_digit (char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool crypto_from_hex (const char* hex, uint8_t* out, size_t length) {
    for (size_t i = 0; i < length; i++) {
        int hi = hex_digit(hex[i * 2]);
        int lo = hi < 0 ? -1 : hex_digit(hex[i * 2 + 1]); // stops at a '\0' instead of reading past it
        if (lo < 0) {
            return false;
        }
        out[i] = (uint8_t)(hi * 16 + lo);
    }
    return true;
}

// ---- self-test --------------------------------------------------------------------------------
//
// Known answers published with each standard. A typo anywhere above (or a compiler problem)
// would change these, so the boot log shows [FAIL] instead of silently storing weak passwords.

static bool check_hex (const uint8_t* got, size_t length, const char* want) {
    uint8_t expected[64];
    return crypto_from_hex(want, expected, length) && memcmp(got, expected, length) == 0;
}

bool crypto_selftest (void) {
    uint8_t out[SHA256_SIZE];
    bool ok = true;

    // FIPS 180-4 examples: one block, and a message that needs two blocks
    sha256("abc", 3, out);
    ok &= check_hex(out, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, out);
    ok &= check_hex(out, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // RFC 4231 test case 2
    hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out);
    ok &= check_hex(out, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // the widely published PBKDF2-HMAC-SHA-256 answers for "password" and "salt", 1 and 2 rounds
    pbkdf2_sha256("password", 8, "salt", 4, 1, out, 32);
    ok &= check_hex(out, 32, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    pbkdf2_sha256("password", 8, "salt", 4, 2, out, 32);
    ok &= check_hex(out, 32, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");

    // FIPS 197 appendix C.3: key 00 01 02 ... 1f, block 00 11 22 ... ff
    uint8_t key[32], block[16];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    for (int i = 0; i < 16; i++) block[i] = (uint8_t)(i * 0x11);
    aes256_t aes;
    aes256_init(&aes, key);
    aes256_encrypt_block(&aes, block, block);
    ok &= check_hex(block, 16, "8ea2b7ca516745bfeafc49904b496089");
    wipe(&aes, sizeof(aes));
    return ok;
}
