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
#include "trace.h"

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
//
// The x86 keeps numbers little endian (lowest byte first) but packets want big endian (highest
// byte first), so a number can't just be copied into a header: 0x0800 must go in as 08 00

// write a 16 bit number into two bytes, highest byte first
static void put16 (uint8_t* p, uint16_t value) {
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

// read a 16 bit number from two bytes, highest byte first
static uint16_t get16 (const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

// write a 32 bit number (an IPv4 address, say) as four bytes, highest first: the top half,
// then the bottom half
static void put32 (uint8_t* p, uint32_t value) {
    put16(p, (uint16_t)(value >> 16));
    put16(p + 2, (uint16_t)value);
}

// read a 32 bit number from four bytes, highest first
static uint32_t get32 (const uint8_t* p) {
    return ((uint32_t)get16(p) << 16) | get16(p + 2);
}


// =================================================================================================
//  Addresses
// =================================================================================================

// "10.0.2.15" -> 0x0A00020F: four numbers 0-255 with dots between, and nothing after
bool net_parse_address (const char* text, uint32_t* address) {
    uint32_t value = 0;
    for (int part = 0; part < 4; part++) {
        // each part must start with a digit: "10..2.15" and "10.-1.2.3" are both wrong
        if (!isdigit((unsigned char)*text)) {
            return false;
        }
        // at most 4 digits are read, which is plenty to see that "1000" is too big, and stops
        // a very long run of digits from overflowing `number` into something that looks fine
        uint32_t number = 0;
        int digits = 0;
        while (isdigit((unsigned char)*text) && digits < 4) {
            number = number * 10 + (uint32_t)(*text++ - '0');
            digits++;
        }
        // after the first three parts comes a dot (stepped over), after the last the end
        bool last = part == 3;
        if (number > 255 || (last ? *text != '\0' : *text++ != '.')) {
            return false;
        }
        // the first part ends up in the top byte, as in "10" -> 0x0A......
        value = (value << 8) | number;
    }
    *address = value;
    return true;
}

// 0x0A00020F -> "10.0.2.15", one byte at a time from the top
void net_format_address (uint32_t address, char out[16]) {
    sfmt(out, 16, "%u.%u.%u.%u", (address >> 24) & 0xFF, (address >> 16) & 0xFF,
         (address >> 8) & 0xFF, address & 0xFF);
}

// read /acc/network ("address 10.0.2.15 255.255.255.0 10.0.2.2 10.0.2.3"), if it is there
static void load_config (void) {
    if (config.loaded) {
        return;
    }
    // marked as loaded straight away, so a missing or broken file is only looked at once
    config.loaded = true;
    char* text = fs_read_f(CONFIG_FOLDER, CONFIG_FILE);
    if (text == NULL) {
        return; // no file: QEMU's usual addresses above stay
    }
    // two strtok_r loops, one inside the other: `save` keeps the place among the lines, and
    // `words` the place among the words of one line (plain strtok could only keep one)
    char* save = NULL;
    for (char* line = strtok_r(text, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        char* words = NULL;
        const char* word = strtok_r(line, " ", &words);
        if (word == NULL || !seq(word, "address")) {
            continue; // a "#" comment, a blank line, or something else
        }
        // address, netmask, gateway, DNS server: all four must be there and make sense,
        // otherwise the line is ignored rather than half used
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

// Is this address on our own network (so it can be sent to directly, not through the gateway)?
// The netmask's 1 bits mark the network part of an address: with 255.255.255.0, 10.0.2.2 and
// 10.0.2.15 both have 10.0.2 there, so they are neighbours, while 93.184.215.14 isn't
static bool on_our_network (uint32_t address) {
    return (address & config.netmask) == (config.address & config.netmask);
}


// =================================================================================================
//  Ethernet and ARP
// =================================================================================================

// Send `length` bytes of `type` (TYPE_IPV4, TYPE_ARP) to the card with this MAC address.
// The Ethernet header is 14 bytes:
//
//     0   destination MAC (6 bytes)
//     6   source MAC, ours (6 bytes)
//     12  type: what is inside, 0x0800 IPv4 or 0x0806 ARP (2 bytes, big endian)
//
// The card adds the CRC at the end itself (CMD_IFCS in e1000.c)
static bool send_frame (const uint8_t destination[6], uint16_t type, const uint8_t* data, size_t length) {
    // static: a frame is up to 1514 bytes, too much to put on the kernel stack every time
    static uint8_t frame[ETHERNET_FRAME_MAX];
    if (ETHERNET_HEADER + length > sizeof(frame)) {
        return false;
    }
    uint8_t mac[6];
    e1000_mac(mac);
    // the header, then the packet copied in after it
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
static int arp_next = 0;   // the entry a new address goes in, oldest first once all are used

// do we already know the MAC address of the card with this address? Copies it into mac if so
static bool arp_known (uint32_t address, uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].used && arp_cache[i].address == address) {
            memcpy(mac, arp_cache[i].mac, 6);
            return true;
        }
    }
    return false;
}

// Remember which MAC address has this address. An address already in the cache gets its MAC
// updated (the computer may have a new card); a new one takes the next entry round the ring,
// which once all 8 are used means forgetting the oldest. It will simply be asked for again
static void arp_remember (uint32_t address, const uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].used && arp_cache[i].address == address) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    // not known yet: a new entry, and arp_next moves on (wrapping round from 7 to 0)
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
//
// 28 bytes in all, laid out as:
//
//     0   hardware type (2)      6   operation (2)          18  target MAC (6)
//     2   protocol type (2)      8   sender MAC (6)         24  target address (4)
//     4   MAC size, 6 (1)        14  sender address (4)
//     5   address size, 4 (1)
static void send_arp (uint16_t operation, const uint8_t target_mac[6], uint32_t target_address) {
    uint8_t packet[28];
    uint8_t mac[6];
    e1000_mac(mac);
    // what kind of addresses this is about: Ethernet MACs (6 bytes) for IPv4 addresses (4 bytes)
    put16(packet, 1);
    put16(packet + 2, TYPE_IPV4);
    packet[4] = 6;
    packet[5] = 4;
    put16(packet + 6, operation);
    // who we are, then who it is for. In a request the target MAC is what we want to know, so
    // it goes out as zeros
    memcpy(packet + 8, mac, 6);
    put32(packet + 14, config.address);
    memcpy(packet + 18, target_mac, 6);
    put32(packet + 24, target_address);
    // a question goes to every card on the network (FF:FF:FF:FF:FF:FF), since we don't know
    // which one has the address yet; an answer goes straight back to the one who asked
    send_frame(operation == 1 ? BROADCAST : target_mac, TYPE_ARP, packet, sizeof(packet));
}

// An ARP packet has arrived (the layout is above send_arp): learn from it, and answer it if
// someone is asking for our address
static void handle_arp (const uint8_t* packet, size_t length) {
    // too short to hold the fields read below, or about some other kind of address than IPv4
    if (length < 28 || get16(packet + 2) != TYPE_IPV4) {
        return;
    }
    uint16_t operation = get16(packet + 6);
    const uint8_t* sender_mac = packet + 8;
    uint32_t sender = get32(packet + 14);
    uint32_t target = get32(packet + 24);

    // Someone asking for us is about to send to us, and we will probably want to answer, so
    // keeping their MAC saves asking for it back. Questions about other computers are none
    // of our business, and aren't remembered (they would only push useful entries out)
    if (operation == 2 || target == config.address) {
        arp_remember(sender, sender_mac); // an answer, or someone about to talk to us
    }
    if (operation == 1 && target == config.address) {
        send_arp(2, sender_mac, sender);  // "that's us"
    }
}

// (poll is further down, with the rest of receiving, but find_mac needs it to collect the answer)
static void poll (void);

// The MAC address of the card with this address: remembered, or asked for (four times, half a
// second each). Two seconds in all, because just after the card has been started again (after
// app mode put it away) QEMU's e1000 holds arriving frames back for a second before handing
// them over, so the answer can come that late
static bool find_mac (uint32_t address, uint8_t mac[6]) {
    if (arp_known(address, mac)) {
        return true;
    }
    // the target MAC in a question: all zeros, it is what we are asking for
    static const uint8_t unknown[6] = { 0 };
    for (int tries = 0; tries < 4; tries++) {
        send_arp(1, unknown, address);
        // nothing arrives in the background, so keep collecting frames while waiting: the
        // answer goes through handle_arp, which puts it in the cache for arp_known to find.
        // (Subtracting the start time works even if the millisecond count wraps round)
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
    // added up in 32 bits, so the carries out of the bottom 16 pile up in the top half
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < length; i += 2) {
        sum += get16(header + i);
    }
    // "one's complement" addition: whatever carried out of the 16 bits is added back in at
    // the bottom. Doing it once can carry again (0xFFFF + 1), hence the loop
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

// send `length` bytes in a UDP packet, from our port `from_port` to `address`:`to_port`.
// Nothing checks the firewall here: everything that calls this has
static int send_udp (uint32_t address, uint16_t from_port, uint16_t to_port, const uint8_t* data, size_t length) {
    // big enough for the largest sealed message and both headers (static: too big for the stack)
    static uint8_t packet[IP_HEADER + UDP_HEADER + NET_MESSAGE_MAX + NETSEAL_OVERHEAD];
    // every packet gets its own id (it only matters to a receiver putting fragments back
    // together, which never happens here, but each packet should still have a different one)
    static uint16_t next_id = 1;
    if (IP_HEADER + UDP_HEADER + length > sizeof(packet)) {
        return NET_ERR_TOO_BIG;
    }
    // Which card the frame goes to: a neighbour's own, or for anywhere further away the
    // gateway's, which passes it on. The IP header still names the real destination
    uint8_t mac[6];
    uint32_t next_hop = on_our_network(address) ? address : config.gateway;
    if (!find_mac(next_hop, mac)) {
        return NET_ERR_NO_ANSWER;
    }

    // IPv4: version 4 with a 5 word header, its total length, an id, "don't fragment",
    // 64 hops to live, UDP inside, the checksum, and the two addresses
    //
    //     0   version (top 4 bits) and header length in 32 bit words (bottom 4)
    //     1   type of service, 0          8   time to live (hops)
    //     2   total length (2)            9   protocol, 17 for UDP
    //     4   id (2)                      10  header checksum (2)
    //     6   flags and fragment (2)      12  source address (4)
    //                                     16  destination address (4)
    uint16_t total = (uint16_t)(IP_HEADER + UDP_HEADER + length);
    memset(packet, 0, IP_HEADER + UDP_HEADER);
    packet[0] = 0x45;                    // 4 = IPv4, 5 words = 20 bytes, no options
    put16(packet + 2, total);
    put16(packet + 4, next_id++);
    put16(packet + 6, 0x4000);           // bit 14, "don't fragment"; fragment offset 0
    packet[8] = 64;                      // each router takes one off, and drops it at 0
    packet[9] = PROTOCOL_UDP;
    put32(packet + 12, config.address);
    put32(packet + 16, address);
    // the checksum last, over the finished header, with its own place still 0 (the memset)
    put16(packet + 10, ip_checksum(packet, IP_HEADER));

    // UDP: the ports and the length. The checksum is left 0 ("none"), which IPv4 allows:
    // the seal checks the message far more thoroughly
    //
    //     0   source port (2)     4   length, header included (2)
    //     2   destination port    6   checksum (2)
    uint8_t* udp = packet + IP_HEADER;
    put16(udp, from_port);
    put16(udp + 2, to_port);
    put16(udp + 4, (uint16_t)(UDP_HEADER + length));
    memcpy(udp + UDP_HEADER, data, length);

    // and an Ethernet header round the lot
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

// Keep a message that has arrived until net_receive asks for it. When the inbox is full the
// new one is dropped rather than an old one, so messages are always taken in the order sent.
// (length is at most NET_MESSAGE_MAX: netseal_open only gives back what fitted in the packet)
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
            continue; // waiting for a different port: it stays for whoever reads that one
        }
        // as much as fits, leaving room for the '\0' (a longer message is cut short)
        size_t length = inbox[i].length < size - 1 ? inbox[i].length : size - 1;
        memcpy(out, inbox[i].text, length);
        out[length] = '\0';
        if (from != NULL) {
            *from = inbox[i].from;
        }
        // close the gap: everything after it moves down one, so inbox[0] is still the oldest
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

// A UDP packet has arrived from `from` (the layout is in send_udp): the DNS answer being waited
// for, or a sealed message that the firewall lets in, or else something to block
static void handle_udp (uint32_t from, const uint8_t* udp, size_t length) {
    if (length < UDP_HEADER) {
        return;
    }
    uint16_t from_port = get16(udp);
    uint16_t to_port = get16(udp + 2);
    size_t udp_length = get16(udp + 4);
    // The length the packet claims must hold at least the header, and mustn't run past what
    // actually arrived: a made-up length would otherwise read beyond the end of the frame
    if (udp_length < UDP_HEADER || udp_length > length) {
        return;
    }
    const uint8_t* data = udp + UDP_HEADER;
    size_t data_length = udp_length - UDP_HEADER;

    // the answer to our DNS question: from the DNS server, to the port we asked from. All four
    // must match, and the port was picked at random, so a stranger can't easily slip in a
    // made-up answer. A longer answer than 512 bytes is cut short (one with an IPv4 address in
    // it never needs that much)
    if (dns.waiting && from == config.dns && from_port == DNS_PORT && to_port == dns.port) {
        dns.length = data_length < sizeof(dns.answer) ? data_length : sizeof(dns.answer);
        memcpy(dns.answer, data, dns.length);
        dns.arrived = true;
        return;
    }

    // everything else needs a rule, and a seal that opens with the rule's key
    const fw_rule_t* rule = fw_find(from, to_port);
    if (rule == NULL) {
        char text[16];
        net_format_address(from, text);
        TRACE("blocked a packet from %s to port %u: no firewall rule allows it", text, to_port);
        block(from, to_port);
        return;
    }
    // Opened into a buffer of its own, so nothing of it reaches the inbox unless the tag
    // matched. Too long, or sealed with a different password (or changed on the way): rejected
    static uint8_t message[NET_MESSAGE_MAX + NETSEAL_OVERHEAD];
    size_t message_length;
    if (data_length > sizeof(message) || !netseal_open(rule->key, data, data_length, message, &message_length)) {
        TRACE("threw away a packet on port %u: its seal didn't open with the rule's password", to_port);
        counts.rejected++;
        return;
    }
    TRACE("let in a sealed message on port %u: a rule allows it and its seal opened", to_port);
    counts.received++;
    inbox_add(to_port, from, message, message_length);
    // the inbox has its own copy now: don't leave the message lying about in this buffer
    wipe(message, message_length);
}

// An IPv4 packet has arrived (the header layout is in send_udp). Only whole UDP packets
// addressed to us go any further
static void handle_ipv4 (const uint8_t* packet, size_t length) {
    // the version is the top 4 bits of the first byte
    if (length < IP_HEADER || (packet[0] >> 4) != 4) {
        return;
    }
    // the bottom 4 bits are the header's length in 32 bit words: 5 (20 bytes) unless the
    // sender added options, which are stepped over rather than read
    size_t header_length = (size_t)(packet[0] & 0x0F) * 4;
    size_t total = get16(packet + 2);
    uint32_t from = get32(packet + 12);
    uint32_t to = get32(packet + 16);
    if (to != config.address) {
        return; // not for us (a broadcast, say)
    }
    // The flags and fragment word: bit 13 is "more fragments", and the bottom 13 bits are
    // where this piece goes in the whole packet. 0x3FFF keeps both but not bit 14 ("don't
    // fragment"), which is fine to see. Any of them set means this is only part of a packet
    bool fragment = (get16(packet + 6) & 0x3FFF) != 0; // "more fragments", or not the first
    // the lengths must agree with each other and with what arrived. total can be less than
    // length: a short packet arrives padded out to Ethernet's 60 byte minimum frame
    bool sound = header_length >= IP_HEADER && total >= header_length && total <= length;
    if (!sound || fragment || packet[9] != PROTOCOL_UDP) {
        block(from, 0); // TCP, ICMP (ping), pieces of a bigger packet: never let in
        return;
    }
    // the UDP packet starts after the IP header (options and all), and ends where total says
    handle_udp(from, packet + header_length, total - header_length);
}

// A frame from the card: look at the Ethernet header's type (bytes 12 and 13, see send_frame)
// and pass what is inside on, without the header. Anything that isn't ARP or IPv4 (IPv6, say)
// is quietly ignored
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

// Take every frame the card has collected, and deal with each one. There are no interrupts:
// this is called whenever AerOS is waiting for something from the network, and the card keeps
// what arrives in between (up to its 32 receive buffers)
static void poll (void) {
    // the size of one of the card's receive buffers, so any frame it took will fit
    static uint8_t frame[2048];
    size_t length;
    while ((length = e1000_receive(frame, sizeof(frame))) > 0) {
        handle_frame(frame, length);
    }
}


// =================================================================================================
//  The functions net.h offers
// =================================================================================================

// what a NET_ result means, in words for a message
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

// find the card and start it, at boot. The settings wait until the network is first used
// (ready), because the file system isn't there yet
bool net_init (void) {
    card_found = e1000_init();
    card_ready = card_found;
    return card_found;
}

// the card's name for the boot log, or "none"
const char* net_card_name (void) {
    return card_found ? e1000_name() : "none";
}

// stop the card and free its buffers, for app mode. ready() starts it again when needed
void net_rest (void) {
    if (card_ready) {
        e1000_stop();
        card_ready = false;
    }
}

// everything NET shows: the card, our addresses and the counters
void net_status (net_status_t* out) {
    // (ready() reads /acc/network, so the addresses shown are the ones that will be used)
    ready();
    *out = counts;
    out->card = card_found;
    out->card_name = card_found ? e1000_name() : "";
    out->link = e1000_link_up();
    // the MAC address is only asked for when the card is running; otherwise it stays as
    // whatever was in `counts` (zeros)
    if (card_ready) {
        e1000_mac(out->mac);
    }
    out->address = config.address;
    out->netmask = config.netmask;
    out->gateway = config.gateway;
    out->dns = config.dns;
}

// set our address, netmask, gateway and DNS server (NET IP), and keep them in /acc/network
void net_set_address (uint32_t address, uint32_t netmask, uint32_t gateway, uint32_t dns_server) {
    // read the file first, so it can't later be read over the top of the new settings
    load_config();
    config.address = address;
    config.netmask = netmask;
    config.gateway = gateway;
    config.dns = dns_server;
    memset(arp_cache, 0, sizeof(arp_cache)); // the neighbours may be different ones now

    // written back in the same form load_config reads: one "address" line
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

// seal a message with the firewall rule's key and send it, if a rule allows it at all
int net_send (uint32_t address, uint16_t port, const void* message, size_t length) {
    if (!ready()) {
        return NET_ERR_NO_CARD;
    }
    if (length > NET_MESSAGE_MAX) {
        return NET_ERR_TOO_BIG;
    }
    // the firewall is asked before anything at all goes out, even the ARP question: with no
    // rule, the network never hears from AerOS
    const fw_rule_t* rule = fw_find(address, port);
    char text[16];
    net_format_address(address, text);
    if (rule == NULL) {
        TRACE("not sending to %s port %u: the firewall has no rule for it, so nothing goes out", text, port);
        return NET_ERR_BLOCKED;
    }
    TRACE("the firewall allows %s port %u: sealing %u bytes with the rule's key", text, port,
          (unsigned int)length);
    // the sealed message is 44 bytes longer: "AERN", the nonce and the tag (sys/netseal.h)
    static uint8_t sealed[NET_MESSAGE_MAX + NETSEAL_OVERHEAD];
    size_t sealed_length = netseal_seal(rule->key, message, length, sealed);
    // (sent from the same port it is going to, so the answer comes back to it)
    int result = send_udp(address, port, port, sealed, sealed_length);
    if (result == NET_OK) {
        counts.sent++;
    }
    return result;
}

// wait up to `ms` milliseconds for a sealed message on `port`, and copy it into out
int net_receive (uint16_t port, uint32_t ms, char* out, size_t size, uint32_t* from) {
    // (size 0 would leave no room even for the '\0', and size - 1 in inbox_take would wrap)
    if (!ready() || size == 0) {
        return -1;
    }
    // collect what has arrived and look in the inbox, again and again until a message for
    // this port is there or the time is up. With ms 0 it looks just once
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
//
// The 12 byte header, every field 2 bytes, big endian:
//
//     0   id: the answer carries the same one, so it can be matched to the question
//     2   flags: bit 15 "this is an answer", bit 8 "recursion desired", bottom 4 bits the
//         result code (0 fine, 3 "no such name")
//     4   how many questions     8   how many "authority" records (not used here)
//     6   how many answers       10  how many "additional" records (not used here)

// write the question for `name` into out, returns its length (0 if the name can't be asked)
static size_t dns_question (const char* name, uint16_t id, uint8_t* out, size_t size) {
    // the header: zeros, apart from the id, one flag, and a count of 1
    memset(out, 0, 12);
    put16(out, id);
    put16(out + 2, 0x0100);   // recursion desired: the server finds the answer for us
    put16(out + 4, 1);        // one question
    // the name, one label at a time: its length in a byte, then its letters (no dots)
    size_t at = 12;
    const char* label = name;
    while (*label != '\0') {
        size_t length = strcspn(label, ".");
        // An empty label ("a..b", or a name starting with a dot) can't be asked, and 63 is
        // the most a label can be: the top 2 bits of the length byte mean something else (a
        // pointer, see skip_name). The 6 is room for this length byte, the final 0, and the
        // type and class
        if (length == 0 || length > 63 || at + length + 6 > size) {
            return 0;
        }
        out[at++] = (uint8_t)length;
        memcpy(out + at, label, length);
        at += length;
        label += length;
        if (*label == '.') {
            label++; // (so "example.com." with a dot on the end is fine too)
        }
    }
    out[at++] = 0;            // a label of length 0: the end of the name
    put16(out + at, 1);       // type A
    put16(out + at + 2, 1);   // class IN
    return at + 4;
}

// step over a name in a DNS message: labels, ending in 0 or in a 2 byte pointer to a name
// further back (0xC0 in the top bits). Returns where the name ends, or 0 if it runs off the end
static size_t skip_name (const uint8_t* message, size_t length, size_t at) {
    while (at < length) {
        uint8_t label = message[at];
        // the 0 at the end of a name
        if (label == 0) {
            return at + 1;
        }
        // A pointer: answers repeat names a lot, so a name can end by pointing back at one
        // already in the message (the other 14 bits say where). The name stops here either
        // way, so there is no need to follow it, which also means a pointer that loops
        // round can't trap us
        if ((label & 0xC0) == 0xC0) {
            return at + 2;
        }
        // an ordinary label: its length byte and its letters
        at += (size_t)label + 1;
    }
    return 0;
}

// Read the answer that arrived for question `id`: the first IPv4 address in it goes in
// *address. The layout is described above dns_question
static int dns_answer (uint16_t id, uint32_t* address) {
    const uint8_t* message = dns.answer;
    size_t length = dns.length;
    // a whole header, our id, and bit 15 of the flags set: it is an answer, not a question
    if (length < 12 || get16(message) != id || !(get16(message + 2) & 0x8000)) {
        return NET_ERR_NO_ANSWER;
    }
    // the result code, in the bottom 4 bits of the flags
    int code = get16(message + 2) & 0x000F;
    if (code == 3) {
        return NET_ERR_NO_NAME; // "no such name"
    }
    if (code != 0) {
        return NET_ERR_NO_ANSWER;
    }
    // Step over the questions the answer repeats (the count is at byte 4). `at` becomes 0 as
    // soon as anything runs off the end, which stops both loops
    size_t at = 12;
    for (int i = get16(message + 4); i > 0 && at != 0; i--) {
        at = skip_name(message, length, at);
        at = at != 0 ? at + 4 : 0;     // the question's type and class
    }
    // Then the answers (the count is at byte 6). After each one's name come 10 bytes:
    //
    //     0   type (2)      4   time to live in seconds (4)
    //     2   class (2)     8   data length (2), then the data itself
    for (int i = get16(message + 6); i > 0 && at != 0 && at < length; i--) {
        at = skip_name(message, length, at);
        if (at == 0 || at + 10 > length) {
            break;
        }
        uint16_t type = get16(message + at);
        uint16_t data_length = get16(message + at + 8);
        // type A with 4 bytes of data is an IPv4 address: the one we want (checking that all
        // 4 bytes really arrived)
        if (type == 1 && data_length == 4 && at + 14 <= length) {
            *address = get32(message + at + 10);
            return NET_OK;
        }
        at += 10 + data_length;        // something else (a CNAME): skip it
    }
    return NET_ERR_NO_NAME;
}

// ask the DNS server for a name's IPv4 address (only FW ALLOW does, when a rule is made)
int net_lookup (const char* name, uint32_t* address) {
    if (!ready()) {
        return NET_ERR_NO_CARD;
    }
    // room for the header, a name of up to 253 letters, and the type and class
    uint8_t question[300];
    // a random id and a random port make a made-up answer from someone else hard to pass off
    // as the real one: they would have to guess both
    uint16_t id;
    crypto_random(&id, sizeof(id));
    size_t length = dns_question(name, id, question, sizeof(question));
    if (length == 0) {
        return NET_ERR_NO_NAME;
    }
    crypto_random(&dns.port, sizeof(dns.port));
    dns.port = (uint16_t)(49152 + dns.port % 16384); // a port from the range kept for this
    // from now on handle_udp watches for the answer, and copies it into dns.answer
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
    // stop watching, so a late answer arriving after this is thrown away like anything else
    dns.waiting = false;
    // no answer: say why (sent but unanswered, or it couldn't be sent at all)
    if (!dns.arrived) {
        return result == NET_OK ? NET_ERR_NO_ANSWER : result;
    }
    return dns_answer(id, address);
}
