#!/usr/bin/env python3
"""RFC 6455 conformance and abuse battery for Mercury's built-in web server.

Mercury speaks HTTP and WebSocket with its own code (gui_interface/websocket/),
having dropped a vendored library that could not be combined with GPLv3.  The
frame codec and handshake digest are unit-tested in
tests/gui_interface/test_ws_protocol.c; this covers what needs a live server:
close codes, HTTP status codes, the connection cap, the handshake timeout, and
behaviour under partial reads, abrupt disconnects and concurrency.

Opt-in, like tests/fuzz -- it needs a running mercury and is not a CI gate.

    make                                   # or make SANITIZE_ASAN_UBSAN=1
    ./mercury -G -U 10141 -p 8700 -b 8800 -x null -C /tmp/m.ini &
    python3 tests/ws/ws_conformance.py 10141

Run it against an ASan build when changing the server: every check here drives
the same buffers the real UI does, so a read overrun shows up as a sanitizer
report rather than as a wrong answer.

It has already earned its keep twice: it caught an oversized header block
being served instead of refused (the limit depended on how the request was
split across reads), and, in its config-side companion, a certificate path
being emptied by a self-overlapping copy.

Exit status is 0 when every check passes.

Copyright (C) 2026 Rhizomatica
Author: Rafael Diniz <rafael@riseup.net>
SPDX-License-Identifier: GPL-3.0-or-later
"""
import socket, ssl, base64, os, struct, time, json, sys, threading

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
results = []
def ok(name, cond, detail=""):
    results.append((name, bool(cond), detail))
    print(("  PASS  " if cond else "  FAIL  ") + name + (("   " + detail) if detail else ""))

def conn(timeout=8):
    return socket.create_connection((HOST, PORT), timeout=timeout)

def handshake(s, path="/websocket", extra=""):
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET %s HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n%s\r\n"
               % (path, key, extra)).encode())
    buf=b""
    while b"\r\n\r\n" not in buf:
        d=s.recv(4096)
        if not d: return None, b""
        buf+=d
    head,rest = buf.split(b"\r\n\r\n",1)
    return head.decode(errors="replace"), rest

def mask_frame(op, payload, fin=True, mask=True):
    b0=(0x80 if fin else 0)|op; n=len(payload)
    if n<126: h=struct.pack("!BB",b0,(0x80 if mask else 0)|n)
    elif n<=0xFFFF: h=struct.pack("!BBH",b0,(0x80 if mask else 0)|126,n)
    else: h=struct.pack("!BBQ",b0,(0x80 if mask else 0)|127,n)
    if not mask: return h+payload
    m=os.urandom(4)
    return h+m+bytes(payload[i]^m[i&3] for i in range(n))

def read_close(s, budget=5):
    """Read frames until a CLOSE arrives; return its status code."""
    buf=b""; t0=time.time()
    while time.time()-t0 < budget:
        try: d=s.recv(65536)
        except (socket.timeout, ConnectionResetError, OSError): return None
        if not d: return None
        buf+=d
        while len(buf)>=2:
            b0,b1=buf[0],buf[1]; n=b1&0x7F; off=2
            if n==126:
                if len(buf)<4: break
                n=struct.unpack("!H",buf[2:4])[0]; off=4
            elif n==127:
                if len(buf)<10: break
                n=struct.unpack("!Q",buf[2:10])[0]; off=10
            if len(buf)<off+n: break
            pl=buf[off:off+n]; op=b0&0xF; buf=buf[off+n:]
            if op==0x8:
                return struct.unpack("!H",pl[:2])[0] if len(pl)>=2 else 1000
    return None

print("== protocol violations must be refused with the right close code ==")
s=conn(); handshake(s); s.sendall(mask_frame(1,b"hi",mask=False))
ok("unmasked client frame -> close 1002", read_close(s)==1002); s.close()

s=conn(); handshake(s)
f=bytearray(mask_frame(1,b"hi")); f[0]|=0x40   # RSV1
s.sendall(bytes(f))
ok("reserved bit set -> close 1002", read_close(s)==1002); s.close()

s=conn(); handshake(s); s.sendall(mask_frame(0x3,b"hi"))
ok("unknown opcode -> close 1002", read_close(s)==1002); s.close()

s=conn(); handshake(s); s.sendall(mask_frame(0,b"orphan"))
ok("continuation without start -> close 1002", read_close(s)==1002); s.close()

s=conn(); handshake(s)
s.sendall(mask_frame(1,b"a",fin=False)); s.sendall(mask_frame(1,b"b"))
ok("interleaved new message -> close 1002", read_close(s)==1002); s.close()

s=conn(); handshake(s); s.sendall(mask_frame(0x9,b"x"*126))
ok("oversized control frame -> close 1002", read_close(s)==1002); s.close()

s=conn(12); handshake(s); s.sendall(mask_frame(1,b"z"*9000))
ok("message over the 8192 cap -> close 1009", read_close(s,8)==1009); s.close()

# RFC 6455 7.4.1: 1005/1006/1015 describe a local condition and must never
# appear in a frame, so they must not be echoed back either.
for bad in (1005, 1006, 1015):
    s=conn(); handshake(s)
    s.sendall(mask_frame(0x8, struct.pack("!H", bad)))
    got = read_close(s)
    ok("close %d is not echoed on the wire" % bad, got == 1000, "got %s" % got)
    s.close()

s=conn(); handshake(s); s.sendall(mask_frame(0x8, struct.pack("!H", 1001)))
got = read_close(s)
ok("a normal close code is echoed", got == 1001, "got %s" % got); s.close()

print("== HTTP edge cases ==")
s=conn(); s.sendall(b"POST /websocket HTTP/1.1\r\nHost: x\r\n\r\n")
d=s.recv(4096); ok("POST -> 405", b"405" in d, d.split(b"\r\n")[0].decode()); s.close()

s=conn(); s.sendall(b"GET /websocket HTTP/1.1\r\nHost: x\r\n\r\n")
d=s.recv(4096); ok("GET on ws path without Upgrade -> 426", b"426" in d, d.split(b"\r\n")[0].decode()); s.close()

s=conn(); s.sendall(b"GET /websocket HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
                    b"Connection: Upgrade\r\nSec-WebSocket-Version: 8\r\n"
                    b"Sec-WebSocket-Key: abc\r\n\r\n")
d=s.recv(4096); ok("wrong websocket version -> 426", b"426" in d, d.split(b"\r\n")[0].decode()); s.close()

s=conn(); s.sendall(b"GET /websocket HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
                    b"Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\r\n")
d=s.recv(4096); ok("upgrade without a key -> 400", b"400" in d, d.split(b"\r\n")[0].decode()); s.close()

# RFC 6455 4.2.1: the Connection header must carry the Upgrade token.
s=conn(); s.sendall(b"GET /websocket HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
                    b"Connection: keep-alive\r\nSec-WebSocket-Version: 13\r\n"
                    b"Sec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==\r\n\r\n")
d=s.recv(4096); ok("Connection without the upgrade token -> 400", b"400" in d,
                   d.split(b"\r\n")[0].decode()); s.close()

# ...and a comma-separated list that contains it is still valid.
s=conn(); s.sendall(b"GET /websocket HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
                    b"Connection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                    b"Sec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==\r\n\r\n")
d=s.recv(4096); ok("Connection: keep-alive, Upgrade accepted", b"101" in d,
                   d.split(b"\r\n")[0].decode()); s.close()

s=conn(); s.sendall(b"GET /../../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n")
d=s.recv(4096); ok("path traversal refused", b"400" in d or b"404" in d, d.split(b"\r\n")[0].decode()); s.close()

s=conn(); s.sendall(b"NOT-HTTP-AT-ALL\r\n\r\n")
try: d=s.recv(4096)
except Exception: d=b""
ok("garbage request does not crash the server", True); s.close()

s=conn(12); s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n" + b"X-Pad: " + b"a"*9000 + b"\r\n\r\n")
try: d=s.recv(4096)
except Exception: d=b""
ok("oversized headers -> 431", b"431" in d, d.split(b"\r\n")[0].decode()); s.close()

print("== byte-at-a-time handshake (partial reads) ==")
s=conn(12)
key=base64.b64encode(os.urandom(16)).decode()
req=("GET /websocket HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"%key).encode()
for b in req:
    s.sendall(bytes([b])); time.sleep(0.001)
buf=b""
t0=time.time()
while b"\r\n\r\n" not in buf and time.time()-t0<5: buf+=s.recv(4096)
ok("handshake split across 1-byte writes", b"101" in buf); s.close()

print("== dribbled frame (header and payload in separate packets) ==")
s=conn(12); handshake(s)
msg=json.dumps({"command":"set_waterfall","value":"1"}).encode()
f=mask_frame(1,msg)
for i in range(0,len(f),3):
    s.sendall(f[i:i+3]); time.sleep(0.005)
got=b""; t0=time.time()
while time.time()-t0<4:
    d=s.recv(65536)
    if b'"status"' in d: got=d; break
ok("frame delivered in 3-byte chunks is reassembled", b'"status"' in got); s.close()

print("== abrupt disconnects ==")
for i in range(40):
    s=conn()
    if i%3==0: s.sendall(b"GET /websocket HTTP/1.1\r\nHost: x\r\nUpg")   # mid-header
    elif i%3==1:
        handshake(s); s.sendall(mask_frame(1,b"x"*50)[:8])                # mid-frame
    s.close()
ok("40 abrupt disconnects at varied stages", True)

print("== concurrency ==")
errs=[]
def worker(n):
    try:
        s=conn(15); h,_=handshake(s)
        if not h or "101" not in h: errs.append("no upgrade %d"%n); return
        s.sendall(mask_frame(1, json.dumps({"command":"set_tx_gain","value":"3"}).encode()))
        t0=time.time()
        while time.time()-t0<6:
            d=s.recv(65536)
            if b'"status"' in d: break
        s.close()
    except Exception as e:
        errs.append("%d: %r"%(n,e))
ts=[threading.Thread(target=worker,args=(i,)) for i in range(16)]
[t.start() for t in ts]; [t.join() for t in ts]
ok("16 concurrent clients all upgrade and get a reply", not errs, str(errs[:2]))

print("== connection cap (server allows 32) ==")
held=[]
for i in range(40):
    try:
        s=conn(5); handshake(s); held.append(s)
    except Exception: pass
time.sleep(1)
alive=0
for s in held:
    try:
        s.settimeout(0.4); d=s.recv(1)
        if d: alive+=1
    except socket.timeout: alive+=1
    except Exception: pass
ok("cap enforced, server survives 40 attempts", alive<=32, "alive=%d"%alive)
for s in held: s.close()
time.sleep(1)

print("== still healthy after all of that ==")
s=conn(12); h,_=handshake(s)
ok("fresh client still upgrades", h is not None and "101" in h); s.close()

bad=[n for n,c,_ in results if not c]
print("\n%d/%d passed" % (len(results)-len(bad), len(results)))
if bad: print("FAILED:", bad); sys.exit(1)
