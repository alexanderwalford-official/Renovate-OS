/*
    net.c - the network stack (see net.h for what it does, and what it doesn't)

    SENDING A MESSAGE, from the top:

        net_send(10.0.2.2, 5000, "hello")
          -> the firewall's rule for 10.0.2.2 port 5000, or NET_ERR_BLOCKED
          -> sealed with the rule's key (sys/netseal.c)
          -> send_udp: an IPv4 header and a UDP header in front of it
          -> the next hop: the address itself if it is on our network, otherwise the gateway
          -> that card's MAC address, from the ARP cache or by asking (ARP)
          -> an Ethernet header in front, and out through the card (drivers/net/e1000.c)

    RECEIVING: poll() takes every frame the card has collected, and handle_frame() sorts them:

        ARP            answer a question about our address, remember the answer to one of ours
        IPv4 + UDP     the DNS answer being waited for, or else a message: it needs a firewall
                       rule for where it came from and the port it came to, and a seal that
                       opens with that rule's key. Then it goes in the inbox for net_receive
        anything else  thrown away

    Numbers in packet headers are big endian ("network order"), so they are read and written a
    byte at a time with get16 / put16 / get32 / put32.

    https://wiki.osdev.org/Network_Stack
*/

#include "net.h"
#include "netseal.h"
#include "firewall.h"
#include "f_sys.h"
#include "sys_clck.h"
#include "../drivers/net/e1000.h"
#include "../lib/crypto.h"
#include "../lib/primary_definitions.h"

// where things are in a frame
#define ETHERNET_HEADER 14
#define IP_HEADER       20
#define UDP_HEADER      8
#define TYPE_IPV4       0x0800
#define TYPE_ARP        0x0806
#define PROTOCOL_UDP    17
#define DNS_PORT        53

#define CONFIG_FOLDER "/acc/"
#define CONFIG_FILE   "network"

static const uint8_t BROADCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static bool card_found = false;   // there is a card (net_init found one)
static bool card_ready = false;   // ...and it is running (net_rest stops it, ready() restarts it)

// AerOS's own address and the ones it needs, QEMU's usual ones until NET IP changes them
static struct {
    bool loaded;
    uint32_t address, netmask, gateway, dns;
} config = { false, 0x0A00020F, 0xFFFFFF00, 0x0A000202, 0x0A000203 };

static net_status_t counts; // only the counters are used: sent, received, blocked...

// ---- bytes in network order ---------------------------------------------------------------------

static void put16 (uint8_t* p, uint16_t value) {
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static uint16_t get16 (const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void put32 (uint8_t* p, uint32_t value) {
    put16(p, (uint16_t)(value >> 16));
    put16(p + 2, (uint16_t)value);
}

static uint32_t get32 (const uint8_t* p) {
    return ((uint32_t)get16(p) << 16) | get16(p + 2);
}


// =================================================================================================
//  Addresses
// =================================================================================================

bool net_parse_address (const char* text, uint32_t* address) {
    uint32_t value = 0;
    for (int part = 0; part < 4; part++) {
        if (!isdigit((unsigned char)*text)) {
            return false;
        }
        uint32_t number = 0;
        int digits = 0;
        while (isdigit((unsigned char)*text) && digits < 4) {
            number = number * 10 + (uint32_t)(*text++ - '0');
            digits++;
        }
        bool last = part == 3;
        if (number > 255 || (last ? *text != '\0' : *text++ != '.')) {
            return false;
        }
        value = (value << 8) | number;
    }
    *address = value;
    return true;
}

void net_format_address (uint32_t address, char out[16]) {
    sfmt(out, 16, "%u.%u.%u.%u", (address >> 24) & 0xFF, (address >> 16) & 0xFF,
         (address >> 8) & 0xFF, address & 0xFF);
}

// read /acc/network ("address 10.0.2.15 255.255.255.0 10.0.2.2 10.0.2.3"), if it is there
static void load_config (void) {
    if (config.loaded) {
        return;
    }
    config.loaded = true;
    char* text = fs_read_f(CONFIG_FOLDER, CONFIG_FILE);
    if (text == NULL) {
        return;
    }
    char* save = NULL;
    for (char* line = strtok_r(text, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        char* words = NULL;
        const char* word = strtok_r(line, " ", &words);
        if (word == NULL || !seq(word, "address")) {
            continue;
        }
        uint32_t values[4];
        bool good = true;
        for (int i = 0; i < 4 && good; i++) {
            const char* part = strtok_r(NULL, " ", &words);
            good = part != NULL && net_parse_address(part, &values[i]);
        }
        if (good) {
            config.address = values[0];
            config.netmask = values[1];
            config.gateway = values[2];
            config.dns = values[3];
        }
    }
    free(text);
}

// the card is running and the settings are read: everything below starts with this. A card
// that was resting (net_rest) is started again
static bool ready (void) {
    load_config();
    if (card_found && !card_ready) {
        card_ready = e1000_init();
    }
    return card_ready;
}

// is this address on our own network (so it can be sent to directly, not through the gateway)?
static bool on_our_network (uint32_t address) {
    return (address & config.netmask) == (config.address & config.netmask);
}


// =================================================================================================
//  Ethernet and ARP
// =================================================================================================

// send `length` bytes of `type` (TYPE_IPV4, TYPE_ARP) to the card with this MAC address
static bool send_frame (const uint8_t destination[6], uint16_t type, const uint8_t* data, size_t length) {
    static uint8_t frame[ETHERNET_FRAME_MAX];
    if (ETHERNET_HEADER + length > sizeof(frame)) {
        return false;
    }
    uint8_t mac[6];
    e1000_mac(mac);
    memcpy(frame, destination, 6);
    memcpy(frame + 6, mac, 6);
    put16(frame + 12, type);
    memcpy(frame + ETHERNET_HEADER, data, length);
    return e1000_send(frame, ETHERNET_HEADER + length);
}

// the cards whose addresses we know: ARP asks once, then they are remembered
#define ARP_CACHE_SIZE 8
static struct {
    bool used;
    uint32_t address;
    uint8_t mac[6];
} arp_cache[ARP_CACHE_SIZE];
static int arp_next = 0;

static bool arp_known (uint32_t address, uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].used && arp_cache[i].address == address) {
            memcpy(mac, arp_cache[i].mac, 6);
            return true;
        }
    }
    return false;
}

static void arp_remember (uint32_t address, const uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].used && arp_cache[i].address == address) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    arp_cache[arp_next].used = true;
    arp_cache[arp_next].address = address;
    memcpy(arp_cache[arp_next].mac, mac, 6);
    arp_next = (arp_next + 1) % ARP_CACHE_SIZE;
}

// An ARP packet: "who has <target address>? tell <us>" (operation 1, a request, sent to every
// card) or "<target address> is at <our MAC>" (operation 2, a reply, sent to the one who asked)
//
//     hardware type 1 (Ethernet), protocol 0x0800 (IPv4), sizes 6 and 4, operation,
//     sender MAC, sender address, target MAC, target address
static void send_arp (uint16_t operation, const uint8_t target_mac[6], uint32_t target_address) {
    uint8_t packet[28];
    uint8_t mac[6];
    e1000_mac(mac);
    put16(packet, 1);
    put16(packet + 2, TYPE_IPV4);
    packet[4] = 6;
    packet[5] = 4;
    put16(packet + 6, operation);
    memcpy(packet + 8, mac, 6);
    put32(packet + 14, config.address);
    memcpy(packet + 18, target_mac, 6);
    put32(packet + 24, target_address);
    send_frame(operation == 1 ? BROADCAST : target_mac, TYPE_ARP, packet, sizeof(packet));
}

static void handle_arp (const uint8_t* packet, size_t length) {
    if (length < 28 || get16(packet + 2) != TYPE_IPV4) {
        return;
    }
    uint16_t operation = get16(packet + 6);
    const uint8_t* sender_mac = packet + 8;
    uint32_t sender = get32(packet + 14);
    uint32_t target = get32(packet + 24);

    if (operation == 2 || target == config.address) {
        arp_remember(sender, sender_mac); // an answer, or someone about to talk to us
    }
    if (operation == 1 && target == config.address) {
        send_arp(2, sender_mac, sender);  // "that's us"
    }
}

static void poll (void);

// The MAC address of the card with this address: remembered, or asked for (four times, half a
// second each). Two seconds in all, because just after the card has been started again (after
// app mode put it away) QEMU's e1000 holds arriving frames back for a second before handing
// them over, so the answer can come that late
static bool find_mac (uint32_t address, uint8_t mac[6]) {
    if (arp_known(address, mac)) {
        return true;
    }
    static const uint8_t unknown[6] = { 0 };
    for (int tries = 0; tries < 4; tries++) {
        send_arp(1, unknown, address);
        uint32_t start = clkuptms();
        while (clkuptms() - start < 500) {
            poll();
            if (arp_known(address, mac)) {
                return true;
            }
            sleep_ms(1);
        }
    }
    return false;
}


// =================================================================================================
//  IPv4 and UDP
// =================================================================================================

// The IPv4 header checksum: add up the header as 16 bit numbers, fold the carries back in, and
// flip every bit. A receiver adding up the whole header (checksum included) then gets 0xFFFF
static uint16_t ip_checksum (const uint8_t* header, size_t length) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < length; i += 2) {
        sum += get16(header + i);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

// send `length` bytes in a UDP packet, from our port `from_port` to `address`:`to_port`.
// Nothing checks the firewall here: everything that calls this has
static int send_udp (uint32_t address, uint16_t from_port, uint16_t to_port, const uint8_t* data, size_t length) {
    static uint8_t packet[IP_HEADER + UDP_HEADER + NET_MESSAGE_MAX + NETSEAL_OVERHEAD];
    static uint16_t next_id = 1;
    if (IP_HEADER + UDP_HEADER + length > sizeof(packet)) {
        return NET_ERR_TOO_BIG;
    }
    uint8_t mac[6];
    uint32_t next_hop = on_our_network(address) ? address : config.gateway;
    if (!find_mac(next_hop, mac)) {
        return NET_ERR_NO_ANSWER;
    }

    // IPv4: version 4 with a 5 word header, its total length, an id, "don't fragment",
    // 64 hops to live, UDP inside, the checksum, and the two addresses
    uint16_t total = (uint16_t)(IP_HEADER + UDP_HEADER + length);
    memset(packet, 0, IP_HEADER + UDP_HEADER);
    packet[0] = 0x45;
    put16(packet + 2, total);
    put16(packet + 4, next_id++);
    put16(packet + 6, 0x4000);
    packet[8] = 64;
    packet[9] = PROTOCOL_UDP;
    put32(packet + 12, config.address);
    put32(packet + 16, address);
    put16(packet + 10, ip_checksum(packet, IP_HEADER));

    // UDP: the ports and the length. The checksum is left 0 ("none"), which IPv4 allows:
    // the seal checks the message far more thoroughly
    uint8_t* udp = packet + IP_HEADER;
    put16(udp, from_port);
    put16(udp + 2, to_port);
    put16(udp + 4, (uint16_t)(UDP_HEADER + length));
    memcpy(udp + UDP_HEADER, data, length);

    return send_frame(mac, TYPE_IPV4, packet, total) ? NET_OK : NET_ERR_SEND;
}


// =================================================================================================
//  The inbox: messages that have arrived, until net_receive takes them
// =================================================================================================

#define INBOX_SIZE 8

static struct {
    uint16_t port;      // the port it came to
    uint32_t from;      // the address it came from
    uint16_t length;
    char text[NET_MESSAGE_MAX];
} inbox[INBOX_SIZE];
static int inbox_count = 0;   // inbox[0] is the oldest

static void inbox_add (uint16_t port, uint32_t from, const uint8_t* message, size_t length) {
    if (inbox_count == INBOX_SIZE) {
        return; // full: nobody is reading them
    }
    inbox[inbox_count].port = port;
    inbox[inbox_count].from = from;
    inbox[inbox_count].length = (uint16_t)length;
    memcpy(inbox[inbox_count].text, message, length);
    inbox_count++;
}

// take the oldest message for `port` out of the inbox, -1 if there is none
static int inbox_take (uint16_t port, char* out, size_t size, uint32_t* from) {
    for (int i = 0; i < inbox_count; i++) {
        if (inbox[i].port != port) {
            continue;
        }
        size_t length = inbox[i].length < size - 1 ? inbox[i].length : size - 1;
        memcpy(out, inbox[i].text, length);
        out[length] = '\0';
        if (from != NULL) {
            *from = inbox[i].from;
        }
        for (int j = i; j < inbox_count - 1; j++) {
            inbox[j] = inbox[j + 1];
        }
        inbox_count--;
        return (int)length;
    }
    return -1;
}


// =================================================================================================
//  Receiving
// =================================================================================================

// the DNS question net_lookup is waiting for an answer to
static struct {
    bool waiting;
    uint16_t port;          // the port we asked from
    uint8_t answer[512];    // the answer, once it is here
    size_t length;
    bool arrived;
} dns;

// a packet for AerOS that isn't let in: count it, and say where it came from (NET shows it,
// to help you see what to allow)
static void block (uint32_t from, uint16_t port) {
    counts.blocked++;
    counts.last_blocked = from;
    counts.last_blocked_port = port;
}

static void handle_udp (uint32_t from, const uint8_t* udp, size_t length) {
    if (length < UDP_HEADER) {
        return;
    }
    uint16_t from_port = get16(udp);
    uint16_t to_port = get16(udp + 2);
    size_t udp_length = get16(udp + 4);
    if (udp_length < UDP_HEADER || udp_length > length) {
        return;
    }
    const uint8_t* data = udp + UDP_HEADER;
    size_t data_length = udp_length - UDP_HEADER;

    // the answer to our DNS question: from the DNS server, to the port we asked from
    if (dns.waiting && from == config.dns && from_port == DNS_PORT && to_port == dns.port) {
        dns.length = data_length < sizeof(dns.answer) ? data_length : sizeof(dns.answer);
        memcpy(dns.answer, data, dns.length);
        dns.arrived = true;
        return;
    }

    // everything else needs a rule, and a seal that opens with the rule's key
    const fw_rule_t* rule = fw_find(from, to_port);
    if (rule == NULL) {
        block(from, to_port);
        return;
    }
    static uint8_t message[NET_MESSAGE_MAX + NETSEAL_OVERHEAD];
    size_t message_length;
    if (data_length > sizeof(message) || !netseal_open(rule->key, data, data_length, message, &message_length)) {
        counts.rejected++;
        return;
    }
    counts.received++;
    inbox_add(to_port, from, message, message_length);
    wipe(message, message_length);
}

static void handle_ipv4 (const uint8_t* packet, size_t length) {
    if (length < IP_HEADER || (packet[0] >> 4) != 4) {
        return;
    }
    size_t header_length = (size_t)(packet[0] & 0x0F) * 4;
    size_t total = get16(packet + 2);
    uint32_t from = get32(packet + 12);
    uint32_t to = get32(packet + 16);
    if (to != config.address) {
        return; // not for us (a broadcast, say)
    }
    bool fragment = (get16(packet + 6) & 0x3FFF) != 0; // "more fragments", or not the first
    bool sound = header_length >= IP_HEADER && total >= header_length && total <= length;
    if (!sound || fragment || packet[9] != PROTOCOL_UDP) {
        block(from, 0); // TCP, ICMP (ping), pieces of a bigger packet: never let in
        return;
    }
    handle_udp(from, packet + header_length, total - header_length);
}

static void handle_frame (const uint8_t* frame, size_t length) {
    if (length < ETHERNET_HEADER) {
        return;
    }
    uint16_t type = get16(frame + 12);
    if (type == TYPE_ARP) {
        handle_arp(frame + ETHERNET_HEADER, length - ETHERNET_HEADER);
    } else if (type == TYPE_IPV4) {
        handle_ipv4(frame + ETHERNET_HEADER, length - ETHERNET_HEADER);
    }
}

// take every frame the card has collected, and deal with each one
static void poll (void) {
    static uint8_t frame[2048];
    size_t length;
    while ((length = e1000_receive(frame, sizeof(frame))) > 0) {
        handle_frame(frame, length);
    }
}


// =================================================================================================
//  The functions net.h offers
// =================================================================================================

const char* net_strerror (int error) {
    switch (error) {
        case NET_OK: return "sent";
        case NET_ERR_NO_CARD: return "there is no network card";
        case NET_ERR_BLOCKED: return "the firewall blocks it";
        case NET_ERR_NO_ANSWER: return "nothing answered";
        case NET_ERR_TOO_BIG: return "too long for one message";
        case NET_ERR_NO_NAME: return "the DNS server knows no such name";
        case NET_ERR_SEND: return "the network card wouldn't send it";
        default: return "unknown error";
    }
}

bool net_init (void) {
    card_found = e1000_init();
    card_ready = card_found;
    return card_found;
}

const char* net_card_name (void) {
    return card_found ? e1000_name() : "none";
}

void net_rest (void) {
    if (card_ready) {
        e1000_stop();
        card_ready = false;
    }
}

void net_status (net_status_t* out) {
    ready();
    *out = counts;
    out->card = card_found;
    out->card_name = card_found ? e1000_name() : "";
    out->link = e1000_link_up();
    if (card_ready) {
        e1000_mac(out->mac);
    }
    out->address = config.address;
    out->netmask = config.netmask;
    out->gateway = config.gateway;
    out->dns = config.dns;
}

void net_set_address (uint32_t address, uint32_t netmask, uint32_t gateway, uint32_t dns_server) {
    load_config();
    config.address = address;
    config.netmask = netmask;
    config.gateway = gateway;
    config.dns = dns_server;
    memset(arp_cache, 0, sizeof(arp_cache)); // the neighbours may be different ones now

    char a[16], m[16], g[16], d[16];
    net_format_address(address, a);
    net_format_address(netmask, m);
    net_format_address(gateway, g);
    net_format_address(dns_server, d);
    char text[128];
    sfmt(text, sizeof(text), "# AerOS's network address (see NET)\naddress %s %s %s %s\n", a, m, g, d);
    if (fs_file_exists(CONFIG_FOLDER, CONFIG_FILE)) {
        fs_write_f(CONFIG_FOLDER, CONFIG_FILE, text, false);
    } else {
        fs_create_f(CONFIG_FOLDER, CONFIG_FILE, text);
    }
}

int net_send (uint32_t address, uint16_t port, const void* message, size_t length) {
    if (!ready()) {
        return NET_ERR_NO_CARD;
    }
    if (length > NET_MESSAGE_MAX) {
        return NET_ERR_TOO_BIG;
    }
    const fw_rule_t* rule = fw_find(address, port);
    if (rule == NULL) {
        return NET_ERR_BLOCKED;
    }
    static uint8_t sealed[NET_MESSAGE_MAX + NETSEAL_OVERHEAD];
    size_t sealed_length = netseal_seal(rule->key, message, length, sealed);
    // (sent from the same port it is going to, so the answer comes back to it)
    int result = send_udp(address, port, port, sealed, sealed_length);
    if (result == NET_OK) {
        counts.sent++;
    }
    return result;
}

int net_receive (uint16_t port, uint32_t ms, char* out, size_t size, uint32_t* from) {
    if (!ready() || size == 0) {
        return -1;
    }
    uint32_t start = clkuptms();
    for (;;) {
        poll();
        int length = inbox_take(port, out, size, from);
        if (length >= 0 || clkuptms() - start >= ms) {
            return length;
        }
        sleep_ms(1);
    }
}

// ---- DNS -------------------------------------------------------------------------------------------
//
// A question: a 12 byte header (an id, "please look it up for me", one question), then the
// name as labels ("example.com" is 7 "example" 3 "com" 0), then "type A (an IPv4 address),
// class IN (the internet)". The answer repeats the header and the question, then gives
// answers: a name, type, class, time to live, length, and the data (4 bytes for type A).
// https://datatracker.ietf.org/doc/html/rfc1035

// write the question for `name` into out, returns its length (0 if the name can't be asked)
static size_t dns_question (const char* name, uint16_t id, uint8_t* out, size_t size) {
    memset(out, 0, 12);
    put16(out, id);
    put16(out + 2, 0x0100);   // recursion desired: the server finds the answer for us
    put16(out + 4, 1);        // one question
    size_t at = 12;
    const char* label = name;
    while (*label != '\0') {
        size_t length = strcspn(label, ".");
        if (length == 0 || length > 63 || at + length + 6 > size) {
            return 0;
        }
        out[at++] = (uint8_t)length;
        memcpy(out + at, label, length);
        at += length;
        label += length;
        if (*label == '.') {
            label++;
        }
    }
    out[at++] = 0;
    put16(out + at, 1);       // type A
    put16(out + at + 2, 1);   // class IN
    return at + 4;
}

// step over a name in a DNS message: labels, ending in 0 or in a 2 byte pointer to a name
// further back (0xC0 in the top bits). Returns where the name ends, or 0 if it runs off the end
static size_t skip_name (const uint8_t* message, size_t length, size_t at) {
    while (at < length) {
        uint8_t label = message[at];
        if (label == 0) {
            return at + 1;
        }
        if ((label & 0xC0) == 0xC0) {
            return at + 2;
        }
        at += (size_t)label + 1;
    }
    return 0;
}

static int dns_answer (uint16_t id, uint32_t* address) {
    const uint8_t* message = dns.answer;
    size_t length = dns.length;
    if (length < 12 || get16(message) != id || !(get16(message + 2) & 0x8000)) {
        return NET_ERR_NO_ANSWER;
    }
    int code = get16(message + 2) & 0x000F;
    if (code == 3) {
        return NET_ERR_NO_NAME; // "no such name"
    }
    if (code != 0) {
        return NET_ERR_NO_ANSWER;
    }
    size_t at = 12;
    for (int i = get16(message + 4); i > 0 && at != 0; i--) {
        at = skip_name(message, length, at);
        at = at != 0 ? at + 4 : 0;     // the question's type and class
    }
    for (int i = get16(message + 6); i > 0 && at != 0 && at < length; i--) {
        at = skip_name(message, length, at);
        if (at == 0 || at + 10 > length) {
            break;
        }
        uint16_t type = get16(message + at);
        uint16_t data_length = get16(message + at + 8);
        if (type == 1 && data_length == 4 && at + 14 <= length) {
            *address = get32(message + at + 10);
            return NET_OK;
        }
        at += 10 + data_length;        // something else (a CNAME): skip it
    }
    return NET_ERR_NO_NAME;
}

int net_lookup (const char* name, uint32_t* address) {
    if (!ready()) {
        return NET_ERR_NO_CARD;
    }
    uint8_t question[300];
    uint16_t id;
    crypto_random(&id, sizeof(id));
    size_t length = dns_question(name, id, question, sizeof(question));
    if (length == 0) {
        return NET_ERR_NO_NAME;
    }
    crypto_random(&dns.port, sizeof(dns.port));
    dns.port = (uint16_t)(49152 + dns.port % 16384); // a port from the range kept for this
    dns.waiting = true;
    dns.arrived = false;

    // ask, and wait a second for the answer, three times
    int result = NET_ERR_NO_ANSWER;
    for (int tries = 0; tries < 3 && !dns.arrived; tries++) {
        result = send_udp(config.dns, dns.port, DNS_PORT, question, length);
        if (result != NET_OK) {
            break;
        }
        uint32_t start = clkuptms();
        while (!dns.arrived && clkuptms() - start < 1000) {
            poll();
            sleep_ms(1);
        }
    }
    dns.waiting = false;
    if (!dns.arrived) {
        return result == NET_OK ? NET_ERR_NO_ANSWER : result;
    }
    return dns_answer(id, address);
}
