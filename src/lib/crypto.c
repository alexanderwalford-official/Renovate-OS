/*
    crypto.c - SHA-256, HMAC, PBKDF2, AES-256 and random numbers (see crypto.h)
*/

#include "crypto.h"
#include "ren_string.h"

// SHA-256 (FIPS 180-4)

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

static uint32_t ror(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

// mix one 64 byte block into the state
static void sha256_block(uint32_t state[8], const uint8_t* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256_init(sha256_t* s) {
    static const uint32_t initial[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    memcpy(s->state, initial, sizeof(initial));
    s->length = 0;
    s->used = 0;
}

void sha256_update(sha256_t* s, const void* data, size_t length) {
    const uint8_t* p = data;
    s->length += length;
    while (length > 0) {
        uint32_t take = 64 - s->used;
        if (take > length) take = (uint32_t)length;
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

void sha256_final(sha256_t* s, uint8_t out[SHA256_SIZE]) {
    // padding: a 1 bit, zeros, then the length in bits as a 64 bit big endian number
    uint64_t bits = s->length * 8;
    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    uint8_t zero = 0;
    while (s->used != 56) {
        sha256_update(s, &zero, 1);
    }
    uint8_t length[8];
    for (int i = 0; i < 8; i++) {
        length[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha256_update(s, length, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(s->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)s->state[i];
    }
    crypto_wipe(s, sizeof(*s));
}

void sha256(const void* data, size_t length, uint8_t out[SHA256_SIZE]) {
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, data, length);
    sha256_final(&s, out);
}

// HMAC (RFC 2104): H((key ^ opad) || H((key ^ ipad) || data))

void hmac_init(hmac_t* h, const void* key, size_t key_length) {
    uint8_t block[64];
    memset(block, 0, sizeof(block));
    if (key_length > 64) {
        sha256(key, key_length, block); // long keys are hashed first
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
    crypto_wipe(block, sizeof(block));
    crypto_wipe(pad, sizeof(pad));
}

// finish one HMAC over data, starting from a prepared context (which is left unchanged)
static void hmac_run(const hmac_t* h, const void* data, size_t length, uint8_t out[SHA256_SIZE]) {
    sha256_t s = h->inner;
    uint8_t inner[SHA256_SIZE];
    sha256_update(&s, data, length);
    sha256_final(&s, inner);
    s = h->outer;
    sha256_update(&s, inner, sizeof(inner));
    sha256_final(&s, out);
}

void hmac_sha256(const void* key, size_t key_length, const void* data, size_t length, uint8_t out[SHA256_SIZE]) {
    hmac_t h;
    hmac_init(&h, key, key_length);
    hmac_run(&h, data, length, out);
    crypto_wipe(&h, sizeof(h));
}

// PBKDF2 (RFC 8018): block i is U1 ^ U2 ^ ... where U1 = HMAC(salt || i) and Un = HMAC(Un-1)

void pbkdf2_sha256(const void* password, size_t password_length, const void* salt, size_t salt_length,
                   uint32_t iterations, uint8_t* out, size_t length) {
    hmac_t h;
    hmac_init(&h, password, password_length);
    for (uint32_t block = 1; length > 0; block++) {
        uint8_t u[SHA256_SIZE], t[SHA256_SIZE];
        uint8_t counter[4] = { (uint8_t)(block >> 24), (uint8_t)(block >> 16), (uint8_t)(block >> 8), (uint8_t)block };
        sha256_t s = h.inner;
        uint8_t inner[SHA256_SIZE];
        sha256_update(&s, salt, salt_length);
        sha256_update(&s, counter, 4);
        sha256_final(&s, inner);
        s = h.outer;
        sha256_update(&s, inner, sizeof(inner));
        sha256_final(&s, u);
        memcpy(t, u, sizeof(t));
        for (uint32_t i = 1; i < iterations; i++) {
            hmac_run(&h, u, sizeof(u), u);
            for (int k = 0; k < SHA256_SIZE; k++) t[k] ^= u[k];
        }
        size_t take = length < SHA256_SIZE ? length : SHA256_SIZE;
        memcpy(out, t, take);
        out += take;
        length -= take;
        crypto_wipe(u, sizeof(u));
        crypto_wipe(t, sizeof(t));
    }
    crypto_wipe(&h, sizeof(h));
}

// AES-256 (FIPS 197)

static uint8_t sbox[256];
static bool sbox_ready = false;

static uint8_t rotl8(uint8_t x, int n) {
    return (uint8_t)((x << n) | (x >> (8 - n)));
}

// the S-box is the multiplicative inverse in GF(2^8) followed by an affine transform. Rather
// than a 256 entry table typed in by hand, walk the field with the generator 3 to build it.
static void aes_build_sbox(void) {
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));  // p * 3
        q ^= (uint8_t)(q << 1);                                // q / 3
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        sbox[p] = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
    } while (p != 1);
    sbox[0] = 0x63; // 0 has no inverse
    sbox_ready = true;
}

static uint32_t sub_word(uint32_t w) {
    return ((uint32_t)sbox[w >> 24] << 24) | ((uint32_t)sbox[(w >> 16) & 0xFF] << 16) |
           ((uint32_t)sbox[(w >> 8) & 0xFF] << 8) | sbox[w & 0xFF];
}

void aes256_init(aes256_t* aes, const uint8_t key[32]) {
    if (!sbox_ready) aes_build_sbox();
    uint32_t* w = aes->round_keys;
    for (int i = 0; i < 8; i++) {
        w[i] = ((uint32_t)key[i * 4] << 24) | ((uint32_t)key[i * 4 + 1] << 16) |
               ((uint32_t)key[i * 4 + 2] << 8) | key[i * 4 + 3];
    }
    uint8_t rcon = 1;
    for (int i = 8; i < 60; i++) {
        uint32_t t = w[i - 1];
        if (i % 8 == 0) {
            t = sub_word((t << 8) | (t >> 24)) ^ ((uint32_t)rcon << 24);
            rcon = (uint8_t)((rcon << 1) ^ ((rcon & 0x80) ? 0x1B : 0));
        } else if (i % 8 == 4) {
            t = sub_word(t);
        }
        w[i] = w[i - 8] ^ t;
    }
}

static uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0));
}

static void add_round_key(uint8_t* s, const uint32_t* k) {
    for (int c = 0; c < 4; c++) {
        s[c * 4] ^= (uint8_t)(k[c] >> 24);
        s[c * 4 + 1] ^= (uint8_t)(k[c] >> 16);
        s[c * 4 + 2] ^= (uint8_t)(k[c] >> 8);
        s[c * 4 + 3] ^= (uint8_t)k[c];
    }
}

void aes256_encrypt_block(const aes256_t* aes, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    add_round_key(s, aes->round_keys);
    for (int round = 1; round <= 14; round++) {
        // SubBytes and ShiftRows together: row r moves r columns to the left
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) {
                t[c * 4 + r] = sbox[s[((c + r) % 4) * 4 + r]];
            }
        }
        // MixColumns (skipped in the last round)
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
    crypto_wipe(s, sizeof(s));
    crypto_wipe(t, sizeof(t));
}

void aes256_ctr(const aes256_t* aes, const uint8_t nonce[8], uint8_t* data, size_t length) {
    uint8_t counter[16], stream[16];
    memcpy(counter, nonce, 8);
    for (uint64_t block = 0; length > 0; block++) {
        for (int i = 0; i < 8; i++) {
            counter[8 + i] = (uint8_t)(block >> (56 - 8 * i));
        }
        aes256_encrypt_block(aes, counter, stream);
        size_t take = length < 16 ? length : 16;
        for (size_t i = 0; i < take; i++) data[i] ^= stream[i];
        data += take;
        length -= take;
    }
    crypto_wipe(stream, sizeof(stream));
}

// random numbers: a SHA-256 pool, stirred with the CPU's hardware generator (RDRAND) when it
// has one, the time stamp counter, and the timing of key presses and timer interrupts

static uint8_t pool[SHA256_SIZE];
static uint32_t pool_counter = 0;
static volatile uint32_t entropy = 0;

static void cpuid(uint32_t leaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

// the CPU's cycle counter: exactly when an interrupt arrives, in cycles, is hard to predict
static uint64_t read_tsc(void) {
    uint64_t t;
    __asm__ volatile ("rdtsc" : "=A"(t));
    return t;
}

void crypto_add_entropy(uint32_t value) {
    entropy = ((entropy << 7) | (entropy >> 25)) ^ value ^ (uint32_t)read_tsc();
}

static bool hardware_random(uint32_t* value) {
    static int available = -1;
    if (available < 0) {
        uint32_t a, b, c, d;
        cpuid(1, &a, &b, &c, &d);
        available = (c >> 30) & 1;
    }
    if (!available) return false;
    for (int tries = 0; tries < 10; tries++) {
        uint8_t ok;
        __asm__ volatile ("rdrand %0; setc %1" : "=r"(*value), "=qm"(ok));
        if (ok) return true;
    }
    return false;
}

void crypto_random(void* buffer, size_t length) {
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
        if (hardware_random(&hw)) sha256_update(&s, &hw, sizeof(hw));
        sha256_final(&s, digest);
        pool_counter++;

        // half of the hash is handed out, the pool moves on to the hash of the hash,
        // so earlier output can't be worked out from the pool
        size_t take = length < 16 ? length : 16;
        memcpy(out, digest, take);
        sha256(digest, sizeof(digest), pool);
        out += take;
        length -= take;
        crypto_wipe(digest, sizeof(digest));
    }
}

// helpers

bool crypto_equal(const void* a, const void* b, size_t length) {
    const uint8_t* x = a;
    const uint8_t* y = b;
    uint8_t difference = 0;
    for (size_t i = 0; i < length; i++) difference |= x[i] ^ y[i];
    return difference == 0;
}

void crypto_wipe(void* buffer, size_t length) {
    volatile uint8_t* p = buffer; // volatile, so the compiler can't skip "pointless" writes
    while (length--) *p++ = 0;
}

void crypto_to_hex(const uint8_t* bytes, size_t length, char* out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; i++) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 15];
    }
    out[length * 2] = '\0';
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool crypto_from_hex(const char* hex, uint8_t* out, size_t length) {
    for (size_t i = 0; i < length; i++) {
        int hi = hex_digit(hex[i * 2]);
        int lo = hi < 0 ? -1 : hex_digit(hex[i * 2 + 1]);
        if (lo < 0) return false;
        out[i] = (uint8_t)(hi * 16 + lo);
    }
    return true;
}

// the official test vectors

static bool check_hex(const uint8_t* got, size_t length, const char* want) {
    uint8_t expected[64];
    return crypto_from_hex(want, expected, length) && memcmp(got, expected, length) == 0;
}

bool crypto_selftest(void) {
    uint8_t out[SHA256_SIZE];
    bool ok = true;

    sha256("abc", 3, out); // FIPS 180-4
    ok &= check_hex(out, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, out);
    ok &= check_hex(out, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out); // RFC 4231 test 2
    ok &= check_hex(out, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    pbkdf2_sha256("password", 8, "salt", 4, 1, out, 32); // RFC 7914 / known PBKDF2-SHA256 vectors
    ok &= check_hex(out, 32, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    pbkdf2_sha256("password", 8, "salt", 4, 2, out, 32);
    ok &= check_hex(out, 32, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");

    uint8_t key[32], block[16];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i; // FIPS 197 appendix C.3
    for (int i = 0; i < 16; i++) block[i] = (uint8_t)(i * 0x11);
    aes256_t aes;
    aes256_init(&aes, key);
    aes256_encrypt_block(&aes, block, block);
    ok &= check_hex(block, 16, "8ea2b7ca516745bfeafc49904b496089");
    crypto_wipe(&aes, sizeof(aes));
    return ok;
}
