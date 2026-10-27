#!/usr/bin/env python3
"""
Update the ring's firmware over BLE (firmware v0.9.0+, MCUboot + SMP).

    python tools/history_sync.py                       # FIRST: the update wipes it
    python tools/ota_update.py build/ring-fw/zephyr/zephyr.signed.bin --yes
    python tools/ota_update.py release/ring-fw-v0.9.0-prod-update.bin --yes
    python tools/ota_update.py --info                  # what is on the ring

Uploads a signed, compressed MCUboot image into slot 1 over MCUmgr's SMP
service, marks it for install and resets the ring; MCUboot then checks the
signature, decompresses it over the running image and boots it (~10-20 s
of no radio). Reconnects afterwards and prints the new version.

Slot 1 IS the history partition, so the upload erases the on-ring history.
Sync it first; --yes says you have. Records after the update carry on from
the same seq, so the app's sync position stays valid (OTA.md).

This is the reference for the phone app's update, which should use Nordic's
nRF Connect Device Manager library instead (iOSMcuManagerLibrary,
Android-nRF-Connect-Device-Manager, or the mcumgr_flutter plugin) with the
same .bin -- or dfu_application.zip from the build directory. No extra
Python packages: SMP and the bit of CBOR it needs are below.

Pairs like history_sync.py does: the SMP characteristic needs an encrypted
link (CONFIG_MCUMGR_TRANSPORT_BT_PERM_RW_ENCRYPT).

Errors the ring can refuse an upload with (OTA.md):
    rc 10 (EBUSY)      battery below 3.6 V -- charge it first
    rc 6  (EBADSTATE)  the ring took slot 1 back after 10 min of silence;
                       this tool then starts again from byte 0
"""

import argparse
import asyncio
import hashlib
import struct
import sys
import time

from bleak import BleakClient, BleakScanner

SMP_CHAR = "da2e7828-fbce-4e01-ae9e-261174997c48"
FWREV = "00002a26-0000-1000-8000-00805f9b34fb"

OP_READ, OP_WRITE = 0, 2
GROUP_OS, GROUP_IMAGE = 0, 1
OS_RESET, OS_PARAMS = 5, 6
IMG_STATE, IMG_UPLOAD = 0, 1
SMP_VERSION = 1             # "SMP version 2" on the wire is 1
HDR = struct.Struct(">BBHHBB")

IMAGE_MAGIC = 0x96F3B83D
IMG_HDR = struct.Struct("<IIHHIIBBHII")
F_LZMA2 = 0x400

RC_NAMES = {0: "ok", 1: "unknown", 2: "no memory", 3: "invalid", 4: "timeout",
            5: "not found", 6: "EBADSTATE: upload must restart",
            7: "message too big", 8: "not supported", 9: "corrupt",
            10: "EBUSY: battery too low to start an update",
            11: "access denied"}


# --- CBOR: just what SMP uses ------------------------------------------------

def _head(major, n):
    if n < 24:
        return bytes([major << 5 | n])
    for ai, fmt in ((24, ">B"), (25, ">H"), (26, ">I"), (27, ">Q")):
        if n < 1 << (8 * struct.calcsize(fmt)):
            return bytes([major << 5 | ai]) + struct.pack(fmt, n)
    raise ValueError(n)


def cbor_encode(v):
    if isinstance(v, bool):
        return b"\xf5" if v else b"\xf4"
    if isinstance(v, int):
        return _head(0, v) if v >= 0 else _head(1, -1 - v)
    if isinstance(v, (bytes, bytearray)):
        return _head(2, len(v)) + bytes(v)
    if isinstance(v, str):
        b = v.encode()
        return _head(3, len(b)) + b
    if isinstance(v, (list, tuple)):
        return _head(4, len(v)) + b"".join(cbor_encode(x) for x in v)
    if isinstance(v, dict):
        return _head(5, len(v)) + b"".join(cbor_encode(k) + cbor_encode(x)
                                           for k, x in v.items())
    raise TypeError(type(v))


def cbor_decode(b, i=0):
    """(value, next index). Indefinite lengths included: zcbor sends them."""
    ib = b[i]
    major, ai = ib >> 5, ib & 0x1F
    i += 1
    if major == 7:
        simple = {20: False, 21: True, 22: None, 23: None}
        if ai in simple:
            return simple[ai], i
        raise ValueError("float/simple %d" % ai)
    if ai == 31:
        if major in (4, 5):
            out = [] if major == 4 else {}
            while b[i] != 0xFF:
                k, i = cbor_decode(b, i)
                if major == 4:
                    out.append(k)
                else:
                    out[k], i = cbor_decode(b, i)
            return out, i + 1
        if major in (2, 3):
            parts = []
            while b[i] != 0xFF:
                p, i = cbor_decode(b, i)
                parts.append(p)
            return (b"" if major == 2 else "").join(parts), i + 1
        raise ValueError("indefinite major %d" % major)
    if ai < 24:
        n = ai
    else:
        size = {24: 1, 25: 2, 26: 4, 27: 8}[ai]
        n = int.from_bytes(b[i:i + size], "big")
        i += size
    if major == 0:
        return n, i
    if major == 1:
        return -1 - n, i
    if major == 2:
        return bytes(b[i:i + n]), i + n
    if major == 3:
        return b[i:i + n].decode(), i + n
    if major == 4:
        out = []
        for _ in range(n):
            x, i = cbor_decode(b, i)
            out.append(x)
        return out, i
    if major == 5:
        out = {}
        for _ in range(n):
            k, i = cbor_decode(b, i)
            out[k], i = cbor_decode(b, i)
        return out, i
    if major == 6:                  # tag: skip it, keep the value
        return cbor_decode(b, i)
    raise ValueError("major %d" % major)


def _selftest():
    for v in (0, 23, 24, 255, 65536, 2**32, -1, -500, b"", b"\x00" * 300,
              "off", True, False, [1, [2, 3]], {"off": 0, "data": b"ab"}):
        e = cbor_encode(v)
        d, n = cbor_decode(e)
        assert d == v and n == len(e), (v, e, d)
    # zcbor's indefinite map: {_ "rc": 0, "off": 512}
    d, _ = cbor_decode(bytes.fromhex("bf" "627263" "00" "636f6666" "190200" "ff"))
    assert d == {"rc": 0, "off": 512}, d


_selftest()


# --- the image ----------------------------------------------------------------

def image_info(data):
    if len(data) < IMG_HDR.size:
        sys.exit("file too short to be an MCUboot image")
    (magic, load, hdr_size, _ptlv, img_size, flags, major, minor, rev,
     build, _pad) = IMG_HDR.unpack(data[:IMG_HDR.size])
    if magic != IMAGE_MAGIC:
        sys.exit("not a signed MCUboot image (no 0x96f3b83d header) -- use "
                 "zephyr.signed.bin, not zephyr.bin or zephyr.hex")
    return {"version": "%d.%d.%d+%d" % (major, minor, rev, build),
            "compressed": bool(flags & F_LZMA2), "size": len(data),
            "load": load, "img_size": img_size, "hdr_size": hdr_size}


# --- SMP over BLE ---------------------------------------------------------------

class Smp:
    def __init__(self, client):
        self.c = client
        self.seq = 0
        self.rx = bytearray()
        self.q = asyncio.Queue()
        self.frag = max(20, client.mtu_size - 3)

    def _on_notify(self, _s, d):
        self.rx += bytes(d)
        while len(self.rx) >= HDR.size:
            _, _, length, _, _, _ = HDR.unpack(self.rx[:HDR.size])
            if len(self.rx) < HDR.size + length:
                return
            pkt = bytes(self.rx[:HDR.size + length])
            del self.rx[:HDR.size + length]
            self.q.put_nowait(pkt)

    async def start(self):
        await self.c.start_notify(SMP_CHAR, self._on_notify)

    async def call(self, op, group, cmd, body, timeout=10.0):
        payload = cbor_encode(body)
        self.seq = (self.seq + 1) & 0xFF
        pkt = HDR.pack(op | SMP_VERSION << 3, 0, len(payload), group,
                       self.seq, cmd) + payload
        # A packet longer than one ATT write goes in pieces; the ring
        # reassembles by the length in the header
        # (CONFIG_MCUMGR_TRANSPORT_BT_REASSEMBLY).
        for i in range(0, len(pkt), self.frag):
            await self.c.write_gatt_char(SMP_CHAR, pkt[i:i + self.frag],
                                         response=False)
        while True:
            rsp = await asyncio.wait_for(self.q.get(), timeout)
            _, _, _, rgroup, rseq, rid = HDR.unpack(rsp[:HDR.size])
            if rseq == self.seq and rgroup == group and rid == cmd:
                body, _ = cbor_decode(rsp, HDR.size)
                return body

    @staticmethod
    def rc(body):
        if "err" in body:                       # SMP v2 group error
            return body["err"].get("rc", 1)
        return body.get("rc", 0)


async def find(address):
    if address:
        return address
    dev = await BleakScanner.find_device_by_name("Ring PPG", timeout=15)
    if not dev:
        sys.exit("no 'Ring PPG' advertising nearby")
    return dev.address


async def connect(address):
    c = BleakClient(address, timeout=20)
    await c.connect()
    try:
        await c.pair()
    except Exception as e:          # already bonded, or the OS does it
        print(f"pair: {e!r} (continuing)")
    return c


def show_state(body):
    for im in body.get("images", []):
        flags = [f for f in ("active", "confirmed", "pending", "permanent")
                 if im.get(f)]
        h = im.get("hash", b"")
        print("  slot %d  %-10s %s  %s" % (im.get("slot", -1),
                                           im.get("version", "?"),
                                           h[:8].hex() if h else "-",
                                           " ".join(flags)))


async def upload(smp, data, chunk_max):
    sha = hashlib.sha256(data).digest()
    off = 0
    restarted = False
    t0 = time.monotonic()
    last_print = 0.0

    while off < len(data):
        chunk = data[off:off + chunk_max]
        body = {"off": off, "data": chunk}
        if off == 0:
            body.update({"image": 0, "len": len(data), "sha": sha})
        rsp = await smp.call(OP_WRITE, GROUP_IMAGE, IMG_UPLOAD, body,
                             timeout=20.0)
        rc = Smp.rc(rsp)
        if rc == 6 and not restarted:
            print("\nring refused mid-image (it took slot 1 back): "
                  "starting again from 0")
            off, restarted = 0, True
            continue
        if rc:
            sys.exit("\nupload refused: rc %d (%s)" % (rc, RC_NAMES.get(rc, "?")))
        off = rsp.get("off", off + len(chunk))

        now = time.monotonic()
        if now - last_print > 0.5 or off >= len(data):
            last_print = now
            rate = off / max(now - t0, 1e-3) / 1024
            print("\r  %6.1f%%  %7d / %d bytes  %5.1f kB/s"
                  % (100.0 * off / len(data), off, len(data), rate),
                  end="", flush=True)
    print()
    return time.monotonic() - t0


async def run(args):
    data = None
    if not args.info:
        data = open(args.image, "rb").read()
        info = image_info(data)
        print("image %s, %d bytes%s" % (info["version"], info["size"],
                                        ", LZMA2" if info["compressed"] else
                                        "  ** NOT COMPRESSED -- will not fit "
                                        "slot 1 **"))
        if not info["compressed"]:
            sys.exit("refusing: the ring's slot 1 holds only a compressed image")
        if not args.yes:
            sys.exit("\nThis erases the ring's on-ring history (slot 1 is the "
                     "history partition).\nSync it first:  python "
                     "tools/history_sync.py\nthen run again with --yes.")

    address = await find(args.address)
    print(f"connecting to {address} ...")
    c = await connect(address)
    try:
        fw = (await c.read_gatt_char(FWREV)).decode(errors="replace")
        print(f"firmware {fw}, MTU {c.mtu_size}")
        if c.services.get_characteristic(SMP_CHAR) is None:
            sys.exit("no SMP service: this firmware predates OTA (v0.9.0); "
                     "flash it over SWD once (FLASHING.md)")

        smp = Smp(c)
        await smp.start()

        state = await smp.call(OP_READ, GROUP_IMAGE, IMG_STATE, {})
        print("images:")
        show_state(state)
        if args.info:
            return

        try:
            params = await smp.call(OP_READ, GROUP_OS, OS_PARAMS, {})
            buf = int(params.get("buf_size", 384))
        except Exception:
            buf = 384
        # Leave room for the 8-byte header and the CBOR around the data:
        # the first chunk also carries "len", "image" and the 32-byte sha,
        # ~70 bytes in all, and the whole packet must fit the ring's buffer.
        chunk_max = max(64, min(buf, args.max_packet) - 100)
        print(f"ring buffer {buf} B, {chunk_max} B per chunk, "
              f"{smp.frag} B per write")

        secs = await upload(smp, data, chunk_max)
        print("uploaded in %.0f s" % secs)

        state = await smp.call(OP_READ, GROUP_IMAGE, IMG_STATE, {})
        slot1 = [im for im in state.get("images", []) if im.get("slot") == 1]
        if not slot1 or not slot1[0].get("hash"):
            show_state(state)
            sys.exit("the ring does not list the upload in slot 1 -- signature "
                     "or hash problem; nothing will be installed")
        rsp = await smp.call(OP_WRITE, GROUP_IMAGE, IMG_STATE,
                             {"hash": slot1[0]["hash"], "confirm": False})
        if Smp.rc(rsp):
            sys.exit("marking for install failed: rc %d" % Smp.rc(rsp))
        print("marked for install:")
        show_state(rsp)

        if args.no_reset:
            print("not resetting (--no-reset); the ring installs it at its "
                  "next reboot, or by itself in ~1 minute")
            return
        try:
            await smp.call(OP_WRITE, GROUP_OS, OS_RESET, {}, timeout=3.0)
        except (asyncio.TimeoutError, Exception):
            pass                # the link often drops before the reply
        print("reset sent")
    finally:
        try:
            await c.disconnect()
        except Exception:
            pass

    print("waiting for MCUboot to install and the ring to come back ...")
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        await asyncio.sleep(5)
        try:
            dev = await BleakScanner.find_device_by_address(address, timeout=5)
            if not dev:
                continue
            c = await connect(address)
            try:
                fw = (await c.read_gatt_char(FWREV)).decode(errors="replace")
                print(f"back up: firmware {fw}")
                smp = Smp(c)
                await smp.start()
                show_state(await smp.call(OP_READ, GROUP_IMAGE, IMG_STATE, {}))
                return
            finally:
                await c.disconnect()
        except Exception as e:
            print(f"  not yet ({e.__class__.__name__})")
    sys.exit("the ring did not come back within 90 s -- check it on the jig")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", nargs="?", help="signed, compressed update (.bin)")
    ap.add_argument("--address", help="ring address (default: scan for 'Ring PPG')")
    ap.add_argument("--yes", action="store_true",
                    help="the history is synced; erasing it is fine")
    ap.add_argument("--info", action="store_true",
                    help="only list the ring's image slots")
    ap.add_argument("--no-reset", action="store_true",
                    help="upload and mark, but do not reset")
    ap.add_argument("--max-packet", type=int, default=1024,
                    help="cap on one SMP packet (default 1024)")
    args = ap.parse_args()
    if not args.info and not args.image:
        ap.error("give an image, or --info")
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
