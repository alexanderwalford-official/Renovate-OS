/*
    cmd_network.c - the terminal's commands for the network and its firewall

        NET                  the network card, AerOS's address, what was let through or kept out
        FW                   the firewall's rules: which computers may talk to AerOS, on which port
        MSG                  send a sealed message to a computer the firewall allows
        LSN                  wait for sealed messages arriving on a port

    The firewall blocks everything until a rule allows it, and messages are sealed with a
    password both ends know (sys/netseal.h). The network itself is sys/net.c, and the rules are
    kept by sys/firewall.c. Each command is a row in the table in t_int.c, and commands.h says
    how the files of the terminal fit together.
*/

#include "commands.h"
#include <stdint.h>
#include <stddef.h>
#include "../../lib/primary_definitions.h"
#include "../../lib/crypto.h"
#include "../../drivers/video/VGA_linear.h"
#include "../../drivers/input/IO.h"
#include "../ui.h"
#include "../explain.h"
#include "../net.h"
#include "../firewall.h"
#include "../netseal.h"


// =================================================================================================
//  The network (sys/net.h) and its firewall (sys/firewall.h)
// =================================================================================================

// the longest password a firewall rule can have, counting the '\0'
#define NET_PASSWORD_MAX 64

// NET on its own: the card, AerOS's address, and what has been let through or kept out
static void show_network (void) {
    net_status_t status;
    net_status(&status);
    if (!status.card) {
        explain_say(PROBLEM_NO_NETWORK, NULL, "There is no network card AerOS can use.");
        return;
    }
    // 16 characters is room for the longest address, "255.255.255.255", and its '\0'
    char address[16], netmask[16], gateway[16], dns[16];
    net_format_address(status.address, address);
    net_format_address(status.netmask, netmask);
    net_format_address(status.gateway, gateway);
    net_format_address(status.dns, dns);

    prtf("Card:         %s, %s\n", status.card_name, status.link ? "link up" : "no link (no cable?)");
    prtf("MAC address:  %02x:%02x:%02x:%02x:%02x:%02x\n", status.mac[0], status.mac[1],
         status.mac[2], status.mac[3], status.mac[4], status.mac[5]);
    prtf("Address:      %s, netmask %s\n", address, netmask);
    prtf("Gateway:      %s, DNS server %s\n", gateway, dns);
    prtf("Firewall:     on, %d rule(s) (%s lists them)\n", fw_count(), command_name(cli_firewall));
    prtf("Messages:     %u sent, %u received, all sealed\n", status.sent, status.received);
    prtf("Kept out:     %u packet(s) no rule allows, %u with the wrong password\n",
         status.blocked, status.rejected);
    // the last computer the firewall turned away, if any. A port of 0 means it wasn't a UDP
    // message (a ping, say), so there is no port to show
    if (status.last_blocked != 0) {
        char from[16];
        net_format_address(status.last_blocked, from);
        if (status.last_blocked_port != 0) {
            prtf("Last blocked: from %s, to port %u\n", from, status.last_blocked_port);
        } else {
            prtf("Last blocked: from %s, not a UDP message (TCP, ping...)\n", from);
        }
    }
}

// NET IP <ADDRESS>[/BITS] <GATEWAY> <DNS>: set AerOS's address. There is no DHCP: asking the
// network for an address would mean sending something before anything is allowed
static void set_network_address (char* arguments) {
    // room for 4 pieces though only 3 are wanted: anything extra ends up in a 4th, so
    // count comes out as 4 and the line is turned away rather than quietly cut short
    char* words[4] = { 0 };
    int count = split(arguments, ' ', words, 4);

    // "10.0.2.15/24": the address, and how many of its bits are the network's part.
    // Without a "/", 24 (a netmask of 255.255.255.0) is the usual choice
    uint32_t address, gateway, dns;
    uint32_t bits = 24;
    char* slash = count > 0 ? strchr(words[0], '/') : NULL;
    if (slash != NULL) {
        *slash = '\0'; // cut the "/24" off, so words[0] is just the address
        bits = (uint32_t)atoi(slash + 1);
    }
    // bits from 1 to 30: 0 would mean shifting by 32 below, which C doesn't allow, and 31 or 32
    // leave no addresses for a gateway and the other computers
    bool good = count == 3 && bits >= 1 && bits <= 30
                && net_parse_address(words[0], &address)
                && net_parse_address(trim(words[1]), &gateway)
                && net_parse_address(trim(words[2]), &dns);
    if (!good) {
        show_usage("NET IP 10.0.2.15/24 10.0.2.2 10.0.2.3: address, gateway, DNS server");
        return;
    }
    // `bits` ones from the left, then zeros: 24 gives 0xFFFFFF00, 255.255.255.0
    uint32_t netmask = 0xFFFFFFFFu << (32 - bits);
    net_set_address(address, netmask, gateway, dns);
    prtf("AerOS is %s now (kept in /acc/network).\n", words[0]);
}

// NET [IP ...]: see show_network and set_network_address
void cli_network (char* arguments) {
    char* rest;
    char* word = split_first_word(arguments, &rest);
    if (word[0] == '\0') {
        show_network();
    } else if (strcasecmp(word, "ip") == 0) {
        set_network_address(rest);
    } else {
        show_usage("NET on its own, or NET IP <address> <gateway> <DNS>");
    }
}

// FW on its own: the rules
static void list_rules (void) {
    int count = fw_count();
    if (count == 0) {
        prtf("The firewall blocks everything: nothing has been allowed yet.\n");
        prtf("%s ALLOW <address or name> <port> allows one computer on one port.\n",
             command_name(cli_firewall));
        return;
    }
    prtf("The firewall blocks everything, except sealed messages to and from:\n");
    for (int i = 1; i <= count; i++) {
        const fw_rule_t* rule = fw_rule(i);
        char address[16];
        net_format_address(rule->address, address);
        prtf("  %2d. %-15s port %-5u %s\n", i, address, rule->port, rule->name);
    }
    prtf("%s DEL <number> removes one.\n", command_name(cli_firewall));
}

// FW ALLOW <ADDRESS OR NAME> <PORT>: allow one computer on one port, with a password both ends
// know. A name is looked up now, the one time DNS is used (DNS can't be sealed)
static void allow_computer (char* arguments) {
    char* port_text;
    char* host = split_first_word(arguments, &port_text);
    int port = atoi(port_text);
    if (host[0] == '\0' || port < 1 || port > 65535) {
        show_usage("e.g. FW ALLOW 10.0.2.2 5000, or FW ALLOW example.com 5000");
        return;
    }

    // an address like 10.0.2.2 is used as it is, and the rule has no name. Anything else is
    // taken to be a name, looked up, and kept with the rule so FW can show it
    uint32_t address;
    char name[FW_NAME_MAX] = "";
    if (!net_parse_address(host, &address)) {
        net_status_t status;
        net_status(&status);
        char dns[16];
        net_format_address(status.dns, dns);
        prtf("Asking the DNS server (%s) for %s. That question isn't sealed: DNS can't be.\n", dns, host);
        ui_busy_begin("Looking it up");
        int result = net_lookup(host, &address);
        ui_busy_end();
        if (result != NET_OK) {
            explain_say(explain_net_problem(result), host, "Can't find %s: %s", host, net_strerror(result));
            return;
        }
        char found[16];
        net_format_address(address, found);
        prtf("%s is %s.\n", host, found);
        scpy(name, host, sizeof(name));
    }

    // the password both ends will seal their messages with, asked for twice
    char password[NET_PASSWORD_MAX];
    char again[NET_PASSWORD_MAX];
    ask_secret("Password both ends will use: ", password, sizeof(password));
    ask_secret("Again:                       ", again, sizeof(again));
    bool same = scmp(password, again) == 0;
    bool long_enough = slen(password) >= 8;
    if (same && long_enough) {
        // the rule keeps a key made from the password, never the password itself. Making it
        // is slow on purpose, so guessing passwords is slow too
        uint8_t key[NETSEAL_KEY_SIZE];
        ui_busy_begin("Making the key");
        netseal_key(password, key);
        ui_busy_end();
        // fw_allow keeps its own copy of the key, so this one can be wiped below
        if (fw_allow(address, (uint16_t)port, name, key)) {
            char allowed[16];
            net_format_address(address, allowed);
            prtf("Allowed %s port %d. Only messages sealed with that password get through.\n", allowed, port);
        } else {
            prtf("The firewall already has %d rules, the most it can have.\n", FW_RULES_MAX);
        }
        wipe(key, sizeof(key));
    } else if (!same) {
        prtf("The passwords don't match, nothing was allowed.\n");
    } else {
        prtf("The password needs 8 characters or more, nothing was allowed.\n");
    }
    wipe(password, sizeof(password));
    wipe(again, sizeof(again));
}

// FW [ALLOW <ADDRESS OR NAME> <PORT> | DEL <N>]: the firewall's rules (sys/firewall.h)
void cli_firewall (char* arguments) {
    char* rest;
    char* word = split_first_word(arguments, &rest);
    if (word[0] == '\0') {
        list_rules();
    } else if (strcasecmp(word, "allow") == 0) {
        allow_computer(rest);
    } else if (strcasecmp(word, "del") == 0 && atoi(rest) > 0) {
        if (fw_remove(atoi(rest))) {
            prtf("Removed rule %d: that computer is blocked again.\n", atoi(rest));
        } else {
            prtf("There is no rule %s (%s lists them).\n", rest, command_name(cli_firewall));
        }
    } else {
        show_usage("FW, FW ALLOW 10.0.2.2 5000 or FW DEL 1");
    }
}

// MSG <ADDRESS OR NAME> <PORT> <TEXT>: send a sealed message to a computer the firewall allows
void cli_message (char* arguments) {
    // two splits: "10.0.2.2 5000 hello there" -> "10.0.2.2", then "5000" and "hello there"
    char* rest;
    char* host = split_first_word(arguments, &rest);
    char* text;
    char* port_text = split_first_word(rest, &text);
    int port = atoi(port_text);
    if (host[0] == '\0' || port < 1 || port > 65535 || text[0] == '\0') {
        show_usage("e.g. MSG 10.0.2.2 5000 hello");
        return;
    }
    // the firewall decides where it goes: an address it has a rule for, or a name a rule was
    // made with (names are never looked up here, see FW ALLOW)
    uint32_t address;
    if (!fw_resolve(host, (uint16_t)port, &address)) {
        explain_say(PROBLEM_BLOCKED, host, "Can't send to %s: the firewall blocks port %d", host, port);
        return;
    }
    ui_busy_begin("Sending");
    int result = net_send(address, (uint16_t)port, text, slen(text));
    ui_busy_end();
    if (result == NET_OK) {
        prtf("Sent to %s port %d, sealed.\n", host, port);
    } else {
        explain_say(explain_net_problem(result), host, "Can't send to %s: %s", host, net_strerror(result));
    }
}

// LSN <PORT>: wait for sealed messages arriving on a port, from the computers the firewall
// allows there, and print each one, until Esc
void cli_listen (char* arguments) {
    int port = atoi(arguments);
    if (port < 1 || port > 65535) {
        show_usage("e.g. LSN 5000");
        return;
    }
    if (!fw_port_open((uint16_t)port)) {
        explain_say(PROBLEM_BLOCKED, NULL, "Nothing is allowed on port %d, so nothing can arrive (%s ALLOW)",
                    port, command_name(cli_firewall));
        return;
    }
    prtf("Waiting for messages on port %d (Esc stops)...\n", port);
    for (;;) {
        // wait a tenth of a second at most for a message (-1 if none came), so Esc is checked
        // ten times a second however quiet the network is
        char message[NET_MESSAGE_MAX + 1];
        uint32_t from;
        if (net_receive((uint16_t)port, 100, message, sizeof(message), &from) >= 0) {
            char sender[16];
            net_format_address(from, sender);
            colour(LIGHT_CYAN, BLACK);
            prtf("[%s] ", sender);
            colour(BRIGHT, BLACK);
            prtf("%s\n", message);
            wipe(message, sizeof(message)); // it was sealed for a reason, so don't leave it in memory
        }
        // only read a key if one is waiting, or getk() would stop and wait for one
        if (kb_haschar() && getk() == KEY_ESCAPE) {
            break;
        }
    }
}
