/*
    firewall.h - nothing goes in or out of the network unless you have allowed it

    The firewall is always on, and starts with no rules: everything is blocked, both ways. A rule
    allows one other computer (an address, or a DNS name looked up when the rule is made) on one
    port, with a password both ends know:

        fw allow 10.0.2.2 5000          asks for the password, then allows 10.0.2.2 port 5000
        fw allow example.com 5000       looks example.com up first (see "DNS" in sys/net.h)
        fw                              lists the rules
        fw del 1                        removes the first one

    What a rule allows:

        sending       a sealed message (sys/netseal.h) to that address, on that port
        receiving     a sealed message from that address, arriving on that port, and only if it
                      was sealed with the rule's password. Anything else is thrown away unread

    So a computer with no rule can't send AerOS anything, and a computer with a rule still has
    to know the password. There is no way to send anything that isn't sealed.

    The rules are kept in /acc/firewall, one a line, with each rule's shared key (made from its
    password, sys/netseal.h). /acc/ is the one folder no program can ever read or change
    (sys/perm.h), so programs can't open the firewall, or read the keys. Anyone at the
    terminal can, as the terminal is trusted.
*/
#ifndef FIREWALL_H
#define FIREWALL_H

#include <stdint.h>
#include <stdbool.h>
#include "netseal.h"

#define FW_RULES_MAX 16   // the most rules there can be
#define FW_NAME_MAX  64   // the longest DNS name a rule can be made for, with its '\0'

typedef struct {
    uint32_t address;               // the other computer's IPv4 address (10.0.2.2 is 0x0A000202)
    uint16_t port;                  // the port, the same at both ends
    char name[FW_NAME_MAX];         // the DNS name it was allowed as, or "" for an address
    uint8_t key[NETSEAL_KEY_SIZE];  // the shared key, made from the password
} fw_rule_t;

// the rule that allows this address and port, NULL if there is none (so it is blocked)
const fw_rule_t* fw_find (uint32_t address, uint16_t port);

// the rule for a DNS name and port (as it was allowed), NULL if there is none
const fw_rule_t* fw_find_name (const char* name, uint16_t port);

// add a rule (or replace the one for the same address and port) and save the rules.
// name is "" for a rule made for an address. False if there are FW_RULES_MAX already
bool fw_allow (uint32_t address, uint16_t port, const char* name, const uint8_t key[NETSEAL_KEY_SIZE]);

// remove a rule (numbered from 1, the order fw_rule lists them in) and save. False if no such rule
bool fw_remove (int number);

// how many rules there are, and rule number n (from 1), NULL past the end
int fw_count (void);
const fw_rule_t* fw_rule (int number);

// is anything allowed on this port at all? (if not, nothing can ever arrive on it)
bool fw_port_open (uint16_t port);

// the address to use for `host` on `port`: host is an address ("10.0.2.2"), or a DNS name a
// rule was made for. Names are never looked up here, only when a rule is made (FW ALLOW), so
// false if no rule was made for that name
bool fw_resolve (const char* host, uint16_t port, uint32_t* address);

#endif /* FIREWALL_H */
