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

static fw_rule_t rules[FW_RULES_MAX];
static int rule_count = 0;
static bool loaded = false;

// "allow 10.0.2.2 5000 <64 hex digits> [name]" -> a rule. False if the line isn't one
static bool parse_rule (char* line, fw_rule_t* rule) {
    char* save = NULL;
    const char* word = strtok_r(line, " ", &save);
    const char* address = strtok_r(NULL, " ", &save);
    const char* port = strtok_r(NULL, " ", &save);
    const char* key = strtok_r(NULL, " ", &save);
    const char* name = strtok_r(NULL, " ", &save);
    if (word == NULL || !seq(word, "allow") || address == NULL || port == NULL || key == NULL) {
        return false;
    }
    int port_number = atoi(port);
    bool good = net_parse_address(address, &rule->address)
                && port_number > 0 && port_number <= 65535
                && slen(key) == NETSEAL_KEY_SIZE * 2
                && crypto_from_hex(key, rule->key, NETSEAL_KEY_SIZE);
    rule->port = (uint16_t)port_number;
    scpy(rule->name, name != NULL ? name : "", sizeof(rule->name));
    return good;
}

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

static void save (void) {
    // a header line, then up to FW_RULES_MAX lines of about 180 characters
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
        sfmt(line, sizeof(line), "allow %s %u %s%s%s\n", address, rules[i].port, key,
             rules[i].name[0] != '\0' ? " " : "", rules[i].name);
        scat(text, line, size);
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
    wipe(text, size);
    free(text);
}

const fw_rule_t* fw_find (uint32_t address, uint16_t port) {
    load();
    for (int i = 0; i < rule_count; i++) {
        if (rules[i].address == address && rules[i].port == port) {
            return &rules[i];
        }
    }
    return NULL;
}

const fw_rule_t* fw_find_name (const char* name, uint16_t port) {
    load();
    for (int i = 0; i < rule_count; i++) {
        if (rules[i].port == port && rules[i].name[0] != '\0' && strcasecmp(rules[i].name, name) == 0) {
            return &rules[i];
        }
    }
    return NULL;
}

bool fw_allow (uint32_t address, uint16_t port, const char* name, const uint8_t key[NETSEAL_KEY_SIZE]) {
    load();
    // the same address and port again replaces the old rule (a new password, say)
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

bool fw_remove (int number) {
    load();
    if (number < 1 || number > rule_count) {
        return false;
    }
    wipe(&rules[number - 1], sizeof(fw_rule_t));
    for (int i = number - 1; i < rule_count - 1; i++) {
        rules[i] = rules[i + 1];
    }
    rule_count--;
    save();
    return true;
}

int fw_count (void) {
    load();
    return rule_count;
}

const fw_rule_t* fw_rule (int number) {
    load();
    return number >= 1 && number <= rule_count ? &rules[number - 1] : NULL;
}

bool fw_port_open (uint16_t port) {
    load();
    for (int i = 0; i < rule_count; i++) {
        if (rules[i].port == port) {
            return true;
        }
    }
    return false;
}

bool fw_resolve (const char* host, uint16_t port, uint32_t* address) {
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
