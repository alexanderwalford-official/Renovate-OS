/*
    bi_network.c
    AerLang's built-in functions for the network: sending a message to another computer (nsend)
    and waiting for one (nrecv). Messages are sealed (sys/netseal.c), and only go to and come
    from computers the firewall allows (sys/firewall.h, the FW command).

    How a built-in works, and how to add one, is at the top of builtins.h.
*/

#include "builtins.h"
#include "../drivers/input/IO.h"
#include "../sys/net.h"
#include "../sys/firewall.h"

// nsend(host, port, text): send text, sealed, to a computer the firewall allows (FW ALLOW).
// host is an address ("10.0.2.2") or the name a rule was made for. True if it was sent
value_t bi_net_send (int argc, value_t* argv, int line) {
    (void)argc;
    const char* host = str_arg(argv, 0, line, "nsend")->data.chars;
    int32_t port = int_arg(argv, 1, line, "nsend");
    // any value can be sent, it goes as the text print() would show (a new string, released below)
    value_t text = aer_to_string(argv[2]);
    uint32_t address;
    // each step only runs if the one before it passed: a real port number (checked before it is
    // cut down to 16 bits), then the firewall finding the host and allowing it, then the send
    bool sent = port > 0 && port <= 65535 && fw_resolve(host, (uint16_t)port, &address)
                && net_send(address, (uint16_t)port, text.as.obj->data.chars, text.as.obj->length) == NET_OK;
    aer_release(text);
    return aer_bool(sent);
}

// nrecv(port, ms): wait up to ms milliseconds for a sealed message on port, from a computer the
// firewall allows there. The message, or nil if none came. Ctrl+C stops the program
value_t bi_net_receive (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t port = int_arg(argv, 0, line, "nrecv");
    int32_t ms = int_arg(argv, 1, line, "nrecv");
    if (port < 1 || port > 65535) {
        aer_error(line, "nrecv() needs a port from 1 to 65535");
    }
    // the biggest message there can be, + 1 for the '\0' after it. aer_alloc rather than the
    // stack, as it may be large, and if Ctrl+C abandons the program it is freed with the rest
    char* message = aer_alloc(NET_MESSAGE_MAX + 1);
    // a tenth of a second at a time, so Ctrl+C is noticed
    for (int32_t waited = 0; ; waited += 100) {
        // 100ms, or only what is left of ms if that is less. A ms of 0 (or below) still has
        // one look, without waiting, for a message that has already arrived
        int32_t wait = ms - waited < 100 ? ms - waited : 100;
        int length = net_receive((uint16_t)port, wait > 0 ? (uint32_t)wait : 0, message, NET_MESSAGE_MAX + 1, NULL);
        // a message came: hand it back as a string (a copy, so the buffer can go)
        if (length >= 0) {
            value_t text = aer_string(message, (uint32_t)length);
            aer_free(message);
            return text;
        }
        if (kb_take_break()) {
            aer_stop(line);
        }
        // the whole of ms has now been waited, and nothing came
        if (waited + 100 >= ms) {
            aer_free(message);
            return aer_nil();
        }
    }
}
