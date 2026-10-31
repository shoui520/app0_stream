#!/usr/bin/env python3
"""
app0stream server: serves PS Vita game folders to the app0stream plugin.

    app0stream_server.py serve LIBRARY [--port 7350] [--bind 0.0.0.0]
    app0stream_server.py stub  LIBRARY TITLEID OUTPUT [--patch]

LIBRARY holds the games the way the memory card holds them:

    LIBRARY/app/PCSE00001/...       the game, as dumped (PFS encrypted or not)
    LIBRARY/patch/PCSE00001/...     its update, if any

"stub" copies what has to stay on the memory card (sce_sys/, sce_pfs/ and
eboot.bin, which the system's promoter may look for; the game still loads
it from the PC)
into OUTPUT/app/PCSE00001 or OUTPUT/patch/PCSE00001; copy OUTPUT's contents
to ux0: and refresh the LiveArea (VitaShell: "Refresh LiveArea").

Protocol: see src/proto.h.  Requests are answered in order on each
connection; the plugin opens one connection per worker thread.
"""

import argparse
import hashlib
import os
import shutil
import socket
import socketserver
import struct
import sys
import threading
import time
from collections import OrderedDict

VERSION = 1

REQ_MAGIC = 0x51533041  # "A0SQ"
RSP_MAGIC = 0x52533041  # "A0SR"

OP_HELLO = 1
OP_STAT = 2
OP_READ = 3
OP_READDIR = 4
OP_LOG = 5

TYPE_FILE = 1
TYPE_DIR = 2

MAX_PATH = 255
MAX_READ = 1024 * 1024
MAX_DIRENTS = 64

DEFAULT_PORT = 7350

# SCE error codes (SCE_ERROR_ERRNO_*), as the signed 32-bit status
ENOENT = 0x80010002 - (1 << 32)
EIO = 0x80010005 - (1 << 32)
EACCES = 0x8001000D - (1 << 32)
ENOTDIR = 0x80010014 - (1 << 32)
EISDIR = 0x80010015 - (1 << 32)
EINVAL = 0x80010016 - (1 << 32)

REQ = struct.Struct("<IIHHIQ")      # 24 bytes
RSP = struct.Struct("<IIiI")        # 16 bytes
REC = struct.Struct("<IIQQHHHHHHIH")  # 42 bytes

assert REQ.size == 24 and RSP.size == 16 and REC.size == 42


class Error(Exception):
    def __init__(self, code):
        super().__init__(code)
        self.code = code


def fold(name):
    """The key names are compared by: case-insensitive, like exFAT."""
    return name.upper()


class Library:
    """Case-insensitive, read-only view of a directory tree."""

    def __init__(self, root):
        self.root = os.path.realpath(root)
        self.lock = threading.Lock()
        self.listings = OrderedDict()   # real dir path -> (mtime, {fold: name}, sorted names)
        self.max_listings = 512
        self.local = threading.local()  # each connection keeps its own open files
        self.max_files = 32

    # ---- names ----------------------------------------------------------

    def listing(self, real):
        """(fold -> name map, sorted names) of a real directory, cached."""
        mtime = os.stat(real).st_mtime_ns
        with self.lock:
            hit = self.listings.get(real)
            if hit and hit[0] == mtime:
                self.listings.move_to_end(real)
                return hit[1], hit[2]

        names = sorted(n for n in os.listdir(real) if n not in (".", ".."))
        by_fold = {}
        for n in names:
            by_fold.setdefault(fold(n), n)

        with self.lock:
            self.listings[real] = (mtime, by_fold, names)
            if len(self.listings) > self.max_listings:
                self.listings.popitem(last=False)

        return by_fold, names

    def resolve(self, rel):
        """The real path and canonical relative path of rel."""
        if len(rel) > MAX_PATH or "\\" in rel or "\0" in rel:
            raise Error(EINVAL)

        real, canon = self.root, []

        for p in rel.split("/") if rel else []:
            if p in ("", ".", ".."):
                raise Error(EINVAL)
            if not os.path.isdir(real):
                raise Error(ENOTDIR)

            try:
                by_fold, _ = self.listing(real)
            except OSError:
                raise Error(EACCES)

            name = by_fold.get(fold(p))
            if name is None:
                raise Error(ENOENT)

            # names come from listing the parent, so "..", "/" and the like
            # never get here; symbolic links in the library are followed
            real = os.path.join(real, name)
            canon.append(name)

        return real, "/".join(canon)

    # ---- records --------------------------------------------------------

    @staticmethod
    def file_id(canon):
        digest = hashlib.blake2b(canon.lower().encode("utf-8"), digest_size=8).digest()
        return struct.unpack("<Q", digest)[0] or 1

    def record(self, real, canon, name):
        try:
            st = os.stat(real)
        except OSError:
            raise Error(ENOENT)

        is_dir = os.path.isdir(real)
        t = time.gmtime(st.st_mtime)
        usec = int((st.st_mtime % 1) * 1_000_000)
        raw = name.encode("utf-8")[:255]

        return REC.pack(
            TYPE_DIR if is_dir else TYPE_FILE,
            0,
            0 if is_dir else st.st_size,
            self.file_id(canon),
            t.tm_year, t.tm_mon, t.tm_mday,
            t.tm_hour, t.tm_min, min(t.tm_sec, 59),
            usec,
            len(raw),
        ) + raw

    # ---- operations -----------------------------------------------------

    def stat(self, rel):
        real, canon = self.resolve(rel)
        return self.record(real, canon, canon.rsplit("/", 1)[-1] if canon else "")

    def readdir(self, rel, index, count):
        real, canon = self.resolve(rel)
        if not os.path.isdir(real):
            raise Error(ENOTDIR)

        _, names = self.listing(real)
        out = []
        for name in names[index:index + min(count, MAX_DIRENTS)]:
            child = canon + "/" + name if canon else name
            try:
                out.append(self.record(os.path.join(real, name), child, name))
            except Error:
                pass    # vanished meanwhile

        return len(out), b"".join(out)

    def open_file(self, real):
        """A file object for real, from this thread's small LRU of open files."""
        files = getattr(self.local, "files", None)
        if files is None:
            files = self.local.files = OrderedDict()

        f = files.get(real)
        if f is not None:
            files.move_to_end(real)
            return f

        f = open(real, "rb", buffering=0)
        files[real] = f
        if len(files) > self.max_files:
            files.popitem(last=False)[1].close()

        return f

    def close_files(self):
        for f in getattr(self.local, "files", {}).values():
            f.close()
        self.local.files = None

    def read(self, rel, length, offset):
        if length > MAX_READ:
            raise Error(EINVAL)

        real, _ = self.resolve(rel)
        if os.path.isdir(real):
            raise Error(EISDIR)

        try:
            f = self.open_file(real)
            f.seek(offset)
            out = bytearray()
            while len(out) < length:
                chunk = f.read(length - len(out))
                if not chunk:
                    break
                out += chunk
            return bytes(out)
        except OSError:
            raise Error(EIO)


# ---- network --------------------------------------------------------------

def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError(f"closed by the client after {len(buf)} of {n} bytes")
        buf += chunk
    return bytes(buf)


class Handler(socketserver.BaseRequestHandler):
    def setup(self):
        self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.request.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024 * 1024)
        self.peer = "%s:%d" % self.client_address[:2]
        log(f"{self.peer} connected")

    def finish(self):
        self.server.library.close_files()
        log(f"{self.peer} disconnected")

    def handle(self):
        lib = self.server.library
        sock = self.request

        while True:
            try:
                magic, seq, op, path_len, arg0, arg1 = REQ.unpack(recv_exact(sock, REQ.size))
                path = recv_exact(sock, path_len) if path_len else b""
            except (ConnectionError, OSError) as e:
                if self.server.verbose:
                    log(f"{self.peer}: {e}")
                return

            if magic != REQ_MAGIC:
                log(f"{self.peer}: bad request magic {magic:08X}")
                return

            status, payload = 0, b""
            try:
                rel = path.decode("utf-8")
                if op == OP_HELLO:
                    status = VERSION
                elif op == OP_STAT:
                    payload = lib.stat(rel)
                elif op == OP_READ:
                    payload = lib.read(rel, arg0, arg1)
                    status = len(payload)
                elif op == OP_READDIR:
                    status, payload = lib.readdir(rel, arg0, arg1)
                elif op == OP_LOG:
                    log(f"{self.peer}: vita {rel}")
                else:
                    status = EINVAL
            except UnicodeDecodeError:
                status = EINVAL
            except Error as e:
                status = e.code
                if self.server.verbose:
                    log(f"{self.peer}: op {op} {path!r}: {status & 0xFFFFFFFF:08X}")

            if op == OP_LOG:
                pass
            elif self.server.verbose and status >= 0 and op != OP_READ:
                log(f"{self.peer}: op {op} {path!r} -> {status}")
            elif self.server.verbose >= 2 and status >= 0:
                log(f"{self.peer}: read {path!r} @{arg1} +{arg0} -> {status}")

            try:
                sock.sendall(RSP.pack(RSP_MAGIC, seq, status, len(payload)) + payload)
            except OSError:
                return


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def log(msg):
    print(time.strftime("%H:%M:%S ") + msg, flush=True)


def local_addresses():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.0.2.1", 9))     # nothing is sent
        addr = s.getsockname()[0]
        s.close()
        return [addr]
    except OSError:
        return []


def serve(args):
    lib = Library(args.library)
    if not os.path.isdir(lib.root):
        sys.exit(f"{args.library}: not a directory")

    for kind in ("app", "patch"):
        d = os.path.join(lib.root, kind)
        if os.path.isdir(d):
            titles = sorted(os.listdir(d))
            log(f"{kind}: {', '.join(titles) if titles else '(none)'}")

    server = Server((args.bind, args.port), Handler)
    server.library = lib
    server.verbose = args.verbose

    for addr in local_addresses():
        log(f"listening on {addr}:{args.port} (server = {addr} in config.txt)")
    if not local_addresses():
        log(f"listening on port {args.port}")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


# ---- stubs ------------------------------------------------------------------

def stub(args):
    kind = "patch" if args.patch else "app"
    title = args.titleid.upper()

    src = None
    base = os.path.join(args.library, kind)
    if os.path.isdir(base):
        for name in os.listdir(base):
            if name.upper() == title:
                src = os.path.join(base, name)

    if src is None:
        sys.exit(f"{title} is not in {base}")

    names = {n.lower(): n for n in os.listdir(src)}
    if "sce_sys" not in names:
        sys.exit(f"{src} has no sce_sys/: not a Vita game folder")
    if "sce_pfs" not in names:
        print("note: no sce_pfs/, so this is a decrypted (non-PFS) dump")

    dst = os.path.join(args.output, kind, title)
    if os.path.exists(dst):
        sys.exit(f"{dst} exists already")

    for key in ("sce_sys", "sce_pfs"):
        if key in names:
            shutil.copytree(os.path.join(src, names[key]), os.path.join(dst, names[key]))

    if "eboot.bin" in names and not args.no_eboot:
        shutil.copy2(os.path.join(src, names["eboot.bin"]), os.path.join(dst, names["eboot.bin"]))

    size = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(dst) for f in fs)

    print(f"stub written to {dst} ({size // 1024} KiB)")
    print()
    print(f"copy it to ux0:{kind}/{title} and add to ux0:data/app0stream/config.txt:")
    print()
    print(f"    {kind if kind == 'patch' else 'title'} = {title}")


def main():
    p = argparse.ArgumentParser(description="Serves PS Vita games to the app0stream plugin.")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("serve", help="serve a game library")
    s.add_argument("library", help="folder holding app/ and patch/")
    s.add_argument("--port", type=int, default=DEFAULT_PORT)
    s.add_argument("--bind", default="0.0.0.0")
    s.add_argument("-v", "--verbose", action="count", default=0,
                   help="log every request (-vv: every read too)")
    s.set_defaults(fn=serve)

    t = sub.add_parser("stub", help="make the memory card stub of a game")
    t.add_argument("library", help="folder holding app/ and patch/")
    t.add_argument("titleid")
    t.add_argument("output", help="folder to write app/TITLEID (or patch/TITLEID) to")
    t.add_argument("--patch", action="store_true", help="the update rather than the game")
    t.add_argument("--no-eboot", action="store_true", help="leave eboot.bin out of the stub")
    t.set_defaults(fn=stub)

    args = p.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
