#!/usr/bin/env python3
"""aernet.py - talk to AerOS over the network, from another computer.

AerOS only sends and accepts sealed messages (see src/sys/netseal.h), so anything that talks
to it has to seal and open them the same way. This does, with the password given to FW ALLOW
in AerOS:

    python3 aernet.py listen 5000            print the messages that arrive on port 5000
    python3 aernet.py listen 5000 --echo     ...and send each one back, as "echo: <message>"
    python3 aernet.py send 127.0.0.1 5001 hello

The password is asked for, or taken from the AERNET_PASSWORD environment variable.

With QEMU's usual networking, AerOS reaches the computer QEMU runs on as 10.0.2.2:

    AerOS:  FW ALLOW 10.0.2.2 5000, then MSG 10.0.2.2 5000 hello
    here:   python3 aernet.py listen 5000          (prints "hello")

The other way round, QEMU has to pass a port on to AerOS. "make run" starts QEMU with
hostfwd=udp::5001-:5000, so a message sent here to port 5001 arrives at AerOS's port 5000:

    AerOS:  FW ALLOW 10.0.2.2 5000, then LSN 5000
    here:   python3 aernet.py send 127.0.0.1 5001 hello

Needs the "cryptography" package for AES (pip install cryptography).

The format (all of it is in src/sys/netseal.h):

    shared key = PBKDF2-HMAC-SHA-256(password, "AerOS network", 100000 rounds, 32 bytes)
    AES key    = HMAC-SHA-256(shared key, "AerOS net key")
    check key  = HMAC-SHA-256(shared key, "AerOS net mac")
    packet     = "AERN" + nonce (8 random bytes) + AES-256-CTR(message) + tag (32 bytes)
    tag        = HMAC-SHA-256(check key, everything before it)

AES-CTR's counter block is the nonce followed by an 8 byte counter starting at 0.
"""
import getpass
import hashlib
import hmac
import os
import socket
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

MAGIC = b"AERN"
NONCE_SIZE = 8
TAG_SIZE = 32


def keys(password):
    shared = hashlib.pbkdf2_hmac("sha256", password.encode(), b"AerOS network", 100000, 32)
    aes_key = hmac.new(shared, b"AerOS net key", "sha256").digest()
    check_key = hmac.new(shared, b"AerOS net mac", "sha256").digest()
    return aes_key, check_key


def ctr(aes_key, nonce, data):
    counter_block = nonce + bytes(8)
    return Cipher(algorithms.AES(aes_key), modes.CTR(counter_block)).encryptor().update(data)


def seal(aes_key, check_key, message):
    nonce = os.urandom(NONCE_SIZE)
    body = MAGIC + nonce + ctr(aes_key, nonce, message)
    return body + hmac.new(check_key, body, "sha256").digest()


def open_sealed(aes_key, check_key, packet):
    """The message, or None if the packet isn't sealed with this password (or was changed)."""
    if len(packet) < len(MAGIC) + NONCE_SIZE + TAG_SIZE or not packet.startswith(MAGIC):
        return None
    body, tag = packet[:-TAG_SIZE], packet[-TAG_SIZE:]
    if not hmac.compare_digest(hmac.new(check_key, body, "sha256").digest(), tag):
        return None
    nonce = body[len(MAGIC):len(MAGIC) + NONCE_SIZE]
    return ctr(aes_key, nonce, body[len(MAGIC) + NONCE_SIZE:])


def password():
    return os.environ.get("AERNET_PASSWORD") or getpass.getpass("Password both ends use: ")


def listen(port, echo):
    aes_key, check_key = keys(password())
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    print(f"listening on port {port} (Ctrl+C stops)", flush=True)
    while True:
        packet, sender = sock.recvfrom(2048)
        message = open_sealed(aes_key, check_key, packet)
        if message is None:
            print(f"{sender[0]}: (not sealed with this password, thrown away)", flush=True)
            continue
        text = message.decode(errors="replace")
        print(f"{sender[0]}: {text}", flush=True)
        if echo:
            sock.sendto(seal(aes_key, check_key, b"echo: " + message), sender)


def send(host, port, text):
    aes_key, check_key = keys(password())
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.sendto(seal(aes_key, check_key, text.encode()), (host, port))
    print(f"sent to {host} port {port}, sealed")


def main(args):
    if len(args) >= 2 and args[0] == "listen":
        listen(int(args[1]), "--echo" in args[2:])
    elif len(args) >= 4 and args[0] == "send":
        send(args[1], int(args[2]), " ".join(args[3:]))
    else:
        sys.exit(__doc__.split("\n\n")[1])


if __name__ == "__main__":
    main(sys.argv[1:])
