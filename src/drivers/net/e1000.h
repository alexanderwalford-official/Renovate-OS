/*
    e1000.h - the Intel PRO/1000 (e1000) network card

    The card QEMU gives a PC by default, and one of the cards VirtualBox can pretend to be. It
    sends and receives Ethernet frames: whole packets with the 14 byte Ethernet header at the
    front (the card adds and checks the CRC at the end by itself). Everything above that, IP,
    UDP, the firewall, is in sys/net.c.

        e1000_init()                      find the card and get it going, false if there is none
        e1000_send(frame, length)         send one frame
        e1000_receive(buffer, size)       the next frame that has arrived, 0 if none has

    Nothing waits for an interrupt: e1000_receive() just looks whether the card has put a frame
    in memory, so the network code calls it whenever it is waiting for something ("polling").

    https://wiki.osdev.org/Intel_Ethernet_i217 and Intel's "PCI/PCI-X Family of Gigabit
    Ethernet Controllers Software Developer's Manual" (8254x)
*/
#ifndef E1000_H
#define E1000_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ETHERNET_FRAME_MAX 1514   // the biggest frame without its CRC: 14 byte header + 1500

// find an e1000 on the PCI bus, reset it and start it sending and receiving. False if there
// is no such card. Safe to call more than once
bool e1000_init (void);

// a short description for the boot log and NET, e.g. "Intel 82540EM (PCI 00:03.0)"
const char* e1000_name (void);

// the card's own hardware address (its MAC address)
void e1000_mac (uint8_t mac[6]);

// is the cable plugged in (in QEMU: is the link up)?
bool e1000_link_up (void);

// send a frame of `length` bytes (at most ETHERNET_FRAME_MAX). False if the card didn't take it
bool e1000_send (const void* frame, size_t length);

// copy the next frame that has arrived into buffer, and return its length. 0 if nothing has
// arrived (or it was bigger than size, and dropped)
size_t e1000_receive (void* buffer, size_t size);

#endif /* E1000_H */
