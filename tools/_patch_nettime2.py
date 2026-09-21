#!/usr/bin/env python3
"""Count how many times the connect loop looked at the network while waiting.

The half second is inside tcp_connect. There are two ways that happens: the
reply really does arrive half a second late, or it arrives at once and nobody
looks. Counting the polls tells them apart -- millions of polls means the
packet was late, a handful means we were not watching.
"""
import sys

R = "/home/roman/xyuos-neo/"
p = R + "kernel/net/net.c"
s = open(p).read()

MARK = "// --- temporary: was anybody watching ---"

A = """    for (int tries = 0; tries < 5; tries++) {
        uint32_t isn = tcp.snd_nxt;
        tcp_out(TCP_SYN, NULL, 0);
        tcp.snd_nxt = isn + 1;          // SYN consumes one sequence number
        uint64_t deadline = now_ms() + 600;
        while (now_ms() < deadline) {
            net_poll();
            if (tcp.state == TCP_ESTABLISHED) return 1;
            if (tcp.state == TCP_CLOSED) return 0;
        }
        tcp.snd_nxt = isn;              // retransmit with the same ISN
    }
    return 0;"""

B = MARK + """
    unsigned long polls = 0;
    uint64_t t0 = now_ms();
    for (int tries = 0; tries < 5; tries++) {
        uint32_t isn = tcp.snd_nxt;
        tcp_out(TCP_SYN, NULL, 0);
        tcp.snd_nxt = isn + 1;          // SYN consumes one sequence number
        uint64_t deadline = now_ms() + 600;
        while (now_ms() < deadline) {
            net_poll();
            polls++;
            if (tcp.state == TCP_ESTABLISHED) {
                kprintf("net: connect took %ums, %lu polls, try %d\\n",
                        (unsigned)(now_ms() - t0), polls, tries);
                return 1;
            }
            if (tcp.state == TCP_CLOSED) return 0;
        }
        tcp.snd_nxt = isn;              // retransmit with the same ISN
    }
    return 0;"""

if len(sys.argv) > 1 and sys.argv[1] == "off":
    if MARK not in s:
        print("already off")
        sys.exit(0)
    open(p, "w").write(s.replace(B, A, 1))
    print("poll counting removed")
else:
    if MARK in s:
        print("already on")
        sys.exit(0)
    assert A in s, "tcp_connect is not the shape expected"
    open(p, "w").write(s.replace(A, B, 1))
    print("poll counting added")
