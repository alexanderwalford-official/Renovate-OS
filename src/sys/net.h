/*
    net.h - the network: sealed messages to and from the computers you have allowed

    A very small network stack, built for one job: sending short messages that can't be read
    or changed on the way, to and from computers you have allowed, and nothing else.

        +----------------------------------------------+
        | your message                                 |   net_send / net_receive
        +----------------------------------------------+
        | sealed: AES-256 + HMAC tag (sys/netseal.h)   |   the only thing ever sent
        +----------------------------------------------+
        | UDP, on the port the firewall allows         |   sys/firewall.h
        +----------------------------------------------+
        | IPv4, ARP (finding a neighbour's card)       |   this file
        +----------------------------------------------+
        | Ethernet: the e1000 card                     |   drivers/net/e1000.c
        +----------------------------------------------+

    The firewall is always on and starts with everything blocked: until you make a rule with
    FW ALLOW, AerOS sends nothing at all and throws away everything that arrives.

    ADDRESSES

    AerOS doesn't ask the network for an address (that would mean sending something before
    anything is allowed), so the address is set by hand with NET IP, and kept in /acc/network.
    Until then it is QEMU's usual one:

        address 10.0.2.15, netmask 255.255.255.0, gateway (the way out) 10.0.2.2, DNS 10.0.2.3

    In QEMU, 10.0.2.2 is also the computer QEMU runs on, so a message to 10.0.2.2 reaches a
    program there (tools/aernet.py) listening on 127.0.0.1 and the same port.

    THE FEW THINGS THAT AREN'T SEALED

    - ARP: to send to an address on the local network, AerOS has to ask (by broadcast) which
      card has it, and it answers when another computer asks for AerOS's address. ARP carries
      addresses only, no data, and happens only for a message the firewall allows.
    - DNS: FW ALLOW with a name (example.com) asks the DNS server for its address, once, when
      the rule is made, because DNS has no encryption. Nothing else ever uses DNS.

    WHAT IT DOESN'T DO

    No TCP (so no web pages), no IPv6, no DHCP, no fragments: a message must fit in one packet
    (NET_MESSAGE_MAX bytes). Nothing arrives in the background: packets are collected whenever
    AerOS is waiting for one (the card keeps the last 32 meanwhile).
*/
#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// the longest message: what fits in one Ethernet frame (1500 bytes of IP packet) once the IP
// and UDP headers (28 bytes) and the seal (sys/netseal.h, 44 bytes) are added
#define NET_MESSAGE_MAX 1428

// results (NET_OK, or one of the errors; net_strerror says what they mean)
#define NET_OK             0
#define NET_ERR_NO_CARD   -1   // there is no network card AerOS can use
#define NET_ERR_BLOCKED   -2   // the firewall has no rule for that address and port
#define NET_ERR_NO_ANSWER -3   // nobody answered (ARP or DNS)
#define NET_ERR_TOO_BIG   -4   // longer than NET_MESSAGE_MAX
#define NET_ERR_NO_NAME   -5   // the DNS server says there is no such name
#define NET_ERR_SEND      -6   // the card wouldn't send it

const char* net_strerror (int error);

// find the network card and start it (at boot, before the file system: the settings in
// /acc/network are read the first time the network is used). False if there isn't one
bool net_init (void);

// the card, e.g. "Intel e1000 100e (PCI 00:03.0)", for the boot log
const char* net_card_name (void);

// everything NET shows
typedef struct {
    bool card;                    // there is a network card
    const char* card_name;
    bool link;                    // ...and it is plugged in
    uint8_t mac[6];
    uint32_t address, netmask, gateway, dns;
    uint32_t sent, received;      // sealed messages sent, and accepted
    uint32_t blocked;             // packets thrown away because no rule allows them
    uint32_t rejected;            // ...because their seal didn't match the rule's password
    uint32_t last_blocked;        // where the last blocked packet came from (0 for none yet)
    uint16_t last_blocked_port;   // and the port it was for (0: it wasn't UDP)
} net_status_t;

void net_status (net_status_t* out);

// set AerOS's own address, netmask, gateway and DNS server, and keep them in /acc/network
void net_set_address (uint32_t address, uint32_t netmask, uint32_t gateway, uint32_t dns);

// seal a message with the firewall rule's key and send it to that address and port.
// NET_ERR_BLOCKED if there is no rule for them
int net_send (uint32_t address, uint16_t port, const void* message, size_t length);

// wait up to `ms` milliseconds for a sealed message that arrives on `port` from a computer a
// rule allows (and is sealed with that rule's password). Copies it into out (room for `size`
// bytes, a '\0' is added after it) and returns its length, or -1 if none came. *from gets the
// address it came from (may be NULL)
int net_receive (uint16_t port, uint32_t ms, char* out, size_t size, uint32_t* from);

// ask the DNS server for a name's IPv4 address. Not sealed (DNS can't be), so only FW ALLOW
// uses it, when a rule is made for a name
int net_lookup (const char* name, uint32_t* address);

// "10.0.2.15" <-> 0x0A00020F. parse is false if the text isn't four numbers 0-255 with dots
bool net_parse_address (const char* text, uint32_t* address);
void net_format_address (uint32_t address, char out[16]);

#endif /* NET_H */
