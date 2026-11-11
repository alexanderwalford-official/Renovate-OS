/*
    firewall.c - the firewall's rules (see firewall.h)

    They are read from /acc/firewall the first time anything asks about them, and written back
    whenever one is added or removed:

        # AerOS firewall: everything is blocked except these
        allow 10.0.2.2 5000 3f9c...e1                (address, port, shared key in hex)
        allow 93.184.215.14 5000 77a0...12 example.com   (and the name, if it was given as one)
*/

#include "firewall.h"
#include "net.h"
#include "f_sys.h"
#include "../lib/crypto.h"
#include "../lib/primary_definitions.h"

#define RULES_FOLDER "/acc/"
#define RULES_FILE   "firewall"

// the rules in memory, in the order they are listed (FW numbers them from 1)
static fw_rule_t rules[FW_RULES_MAX];
static int rule_count = 0;
static bool loaded = false;   // has /acc/firewall been read yet? (see load)

// "allow 10.0.2.2 5000 <64 hex digits> [name]" -> a rule. False if the line isn't one
static bool parse_rule (char* line, fw_rule_t* rule) {
    // split the line into its words (strtok_r writes '\0's into the line as it goes)
    char* save = NULL;
    const char* word = strtok_r(line, " ", &save);
    const char* address = strtok_r(NULL, " ", &save);
    const char* port = strtok_r(NULL, " ", &save);
    const char* key = strtok_r(NULL, " ", &save);
    const char* name = strtok_r(NULL, " ", &save);
    if (word == NULL || !seq(word, "allow") || address == NULL || port == NULL || key == NULL) {
        return false;
    }
    // Every part must make sense: a real address, a port from 1 to 65535 (0 isn't one), and
    // a key of exactly 32 bytes written as 64 hex digits. A line that is wrong anywhere is
    // left out entirely, rather than become a rule that allows something unexpected
    int port_number = atoi(port);
    bool good = net_parse_address(address, &rule->address)
                && port_number > 0 && port_number <= 65535
                && slen(key) == NETSEAL_KEY_SIZE * 2
                && crypto_from_hex(key, rule->key, NETSEAL_KEY_SIZE);
    // (filled in even when the line is bad: load only counts the rule if `good`, so a bad
    // one is simply written over by the next line)
    rule->port = (uint16_t)port_number;
    scpy(rule->name, name != NULL ? name : "", sizeof(rule->name));
    return good;
}

// Read the rules from /acc/firewall, the first time anything asks about them. Waiting until
// then means the file system is sure to be ready, and the file is only read once: after that
// the copy in memory is the one that counts, and save() keeps the file the same as it
static void load (void) {
    if (loaded) {
        return;
    }
    loaded = true;
    rule_count = 0;
    char* text = fs_read_f(RULES_FOLDER, RULES_FILE);
    if (text == NULL) {
        return; // no rules yet: everything is blocked
    }
    // one rule a line, skipping "#" comments, up to FW_RULES_MAX (any more are ignored). Each
    // line is parsed straight into the next free place, which only counts if it was good
    char* save = NULL;
    for (char* line = strtok_r(text, "\n", &save); line != NULL && rule_count < FW_RULES_MAX;
         line = strtok_r(NULL, "\n", &save)) {
        if (line[0] != '#' && parse_rule(line, &rules[rule_count])) {
            rule_count++;
        }
    }
    wipe(text, slen(text)); // it held the keys
    free(text);
}

// Write every rule back to /acc/firewall, in the form load reads. The text holds the keys, so
// every copy of it is wiped once it has been written, in memory and on the disk
static void save (void) {
    // a header line, then up to FW_RULES_MAX lines of about 180 characters: room for the
    // address (16), the port (8), the key in hex, the name, and "allow", spaces and '\n' (16)
    size_t size = 128 + FW_RULES_MAX * (16 + 8 + NETSEAL_KEY_SIZE * 2 + FW_NAME_MAX + 16);
    char* text = malloc(size);
    if (text == NULL) {
        return;
    }
    scpy(text, "# AerOS firewall: everything is blocked except these (see FW)\n", size);
    for (int i = 0; i < rule_count; i++) {
        char address[16];
        char key[NETSEAL_KEY_SIZE * 2 + 1];
        char line[16 + 8 + NETSEAL_KEY_SIZE * 2 + FW_NAME_MAX + 16];
        net_format_address(rules[i].address, address);
        crypto_to_hex(rules[i].key, NETSEAL_KEY_SIZE, key);
        // the name, if there is one, goes on the end after a space; otherwise nothing at all,
        // so the line doesn't end with a stray space
        sfmt(line, sizeof(line), "allow %s %u %s%s%s\n", address, rules[i].port, key,
             rules[i].name[0] != '\0' ? " " : "", rules[i].name);
        scat(text, line, size);
        // these held the key too, and are on the stack where the next function would find them
        wipe(key, sizeof(key));
        wipe(line, sizeof(line));
    }
    if (fs_file_exists(RULES_FOLDER, RULES_FILE)) {
        // the old rules held keys: zero them on the disk, rather than leave them in free space
        bool wipe_old = true;
        fs_write_bytes(RULES_FOLDER, RULES_FILE, text, (uint32_t)slen(text), wipe_old);
    } else {
        fs_create_f(RULES_FOLDER, RULES_FILE, text);
    }
    // (the whole buffer, not just the text: malloc doesn't clear memory when it is freed)
    wipe(text, size);
    free(text);
}

// the rule for exactly this address and port, or NULL: the one test everything sent or
// received has to pass
const fw_rule_t* fw_find (uint32_t address, uint16_t port) {
    load();
    for (int i = 0; i < rule_count; i++) {
        if (rules[i].address == address && rules[i].port == port) {
            return &rules[i];
        }
    }
    return NULL;
}

// the rule made for this name (example.com) and port, or NULL. Upper and lower case don't
// matter, as in DNS itself
const fw_rule_t* fw_find_name (const char* name, uint16_t port) {
    load();
    for (int i = 0; i < rule_count; i++) {
        // (a rule made for an address has no name, and must never match an empty one)
        if (rules[i].port == port && rules[i].name[0] != '\0' && strcasecmp(rules[i].name, name) == 0) {
            return &rules[i];
        }
    }
    return NULL;
}

// add a rule (FW ALLOW), or replace the one for the same address and port, and save them all
bool fw_allow (uint32_t address, uint16_t port, const char* name, const uint8_t key[NETSEAL_KEY_SIZE]) {
    load();
    // the same address and port again replaces the old rule (a new password, say). fw_find
    // hands out a const pointer so nobody else changes a rule; here it is ours to change
    fw_rule_t* rule = (fw_rule_t*)fw_find(address, port);
    if (rule == NULL) {
        if (rule_count == FW_RULES_MAX) {
            return false;
        }
        rule = &rules[rule_count++];
    }
    rule->address = address;
    rule->port = port;
    scpy(rule->name, name, sizeof(rule->name));
    memcpy(rule->key, key, NETSEAL_KEY_SIZE);
    save();
    return true;
}

// remove rule `number` (counting from 1, as FW lists them), and save the rest
bool fw_remove (int number) {
    load();
    if (number < 1 || number > rule_count) {
        return false;
    }
    // wipe its key first, then close the gap so the rules after it move up one
    wipe(&rules[number - 1], sizeof(fw_rule_t));
    for (int i = number - 1; i < rule_count - 1; i++) {
        rules[i] = rules[i + 1];
    }
    // (the last place still holds a copy of the last rule, but it is past rule_count, and
    // the next rule added writes over it)
    rule_count--;
    save();
    return true;
}

// how many rules there are
int fw_count (void) {
    load();
    return rule_count;
}

// rule `number`, counting from 1, or NULL if there is no such rule
const fw_rule_t* fw_rule (int number) {
    load();
    return number >= 1 && number <= rule_count ? &rules[number - 1] : NULL;
}

// does any rule, for any address, allow this port? If none does, nothing can ever arrive on it
bool fw_port_open (uint16_t port) {
    load();
    for (int i = 0; i < rule_count; i++) {
        if (rules[i].port == port) {
            return true;
        }
    }
    return false;
}

// "10.0.2.2" or a name a rule was made for -> an address. Never asks DNS: a name only works
// once FW ALLOW has looked it up, and the address it got then is the one used
bool fw_resolve (const char* host, uint16_t port, uint32_t* address) {
    // an address written out already needs nothing looking up
    if (net_parse_address(host, address)) {
        return true;
    }
    const fw_rule_t* rule = fw_find_name(host, port);
    if (rule == NULL) {
        return false;
    }
    *address = rule->address;
    return true;
}
