# Tiny MQTT 3.1.1 broker (QoS 0, exact-topic subscriptions) over TCP and WebSocket, for local tests only.
import asyncio, base64, hashlib, struct, sys
subs = {}  # topic -> set(writer-like)
class Conn:
    def __init__(s, send): s.send = send; s.topics = set()
def enc_len(n):
    o = bytearray()
    while True:
        d = n % 128; n //= 128
        if n: d |= 128
        o.append(d)
        if not n: return bytes(o)
async def handle_packets(conn, read):
    while True:
        h = (await read(1))[0]
        n = 0; mul = 1
        while True:
            b = (await read(1))[0]; n += (b & 127) * mul; mul *= 128
            if not b & 128: break
        body = await read(n) if n else b''
        t = h & 0xF0
        if t == 0x10: await conn.send(b'\x20\x02\x00\x00')
        elif t == 0x80:
            pid = body[:2]; tl = struct.unpack('>H', body[2:4])[0]; topic = body[4:4+tl].decode()
            subs.setdefault(topic, set()).add(conn); conn.topics.add(topic)
            await conn.send(b'\x90\x03' + pid + b'\x00')
        elif t == 0x30:
            tl = struct.unpack('>H', body[:2])[0]; topic = body[2:2+tl].decode()
            pkt = bytes([0x30]) + enc_len(len(body)) + body
            print('pub', topic, len(body), flush=True)
            for c in list(subs.get(topic, ())):
                try: await c.send(pkt)
                except Exception: pass
        elif t == 0xC0: await conn.send(b'\xD0\x00')
        elif t == 0xE0: return
async def tcp(reader, writer):
    async def send(b): writer.write(b); await writer.drain()
    c = Conn(send)
    try: await handle_packets(c, reader.readexactly)
    except Exception: pass
    for t in c.topics: subs.get(t, set()).discard(c)
    writer.close()
async def ws(reader, writer):
    req = b''
    while b'\r\n\r\n' not in req: req += await reader.read(1024)
    key = [l.split(b':',1)[1].strip() for l in req.split(b'\r\n') if l.lower().startswith(b'sec-websocket-key')][0]
    acc = base64.b64encode(hashlib.sha1(key + b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
    writer.write(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + acc + b'\r\nSec-WebSocket-Protocol: mqtt\r\n\r\n')
    await writer.drain()
    buf = bytearray()
    async def send(b):
        hdr = bytes([0x82]) + (bytes([len(b)]) if len(b) < 126 else bytes([126]) + struct.pack('>H', len(b)) if len(b) < 65536 else bytes([127]) + struct.pack('>Q', len(b)))
        writer.write(hdr + b); await writer.drain()
    async def read(n):
        while len(buf) < n:
            h = await reader.readexactly(2); ln = h[1] & 127
            if ln == 126: ln = struct.unpack('>H', await reader.readexactly(2))[0]
            elif ln == 127: ln = struct.unpack('>Q', await reader.readexactly(8))[0]
            mask = await reader.readexactly(4) if h[1] & 128 else b'\0\0\0\0'
            data = bytearray(await reader.readexactly(ln))
            for i in range(ln): data[i] ^= mask[i % 4]
            if h[0] & 15 == 8: raise EOFError
            if h[0] & 15 in (1, 2, 0): buf.extend(data)
        out = bytes(buf[:n]); del buf[:n]; return out
    c = Conn(send)
    try: await handle_packets(c, read)
    except Exception: pass
    for t in c.topics: subs.get(t, set()).discard(c)
    writer.close()
async def main():
    await asyncio.start_server(tcp, '127.0.0.1', 18830)
    await asyncio.start_server(ws, '127.0.0.1', 9001)
    print('broker up', flush=True)
    await asyncio.Event().wait()
asyncio.run(main())
