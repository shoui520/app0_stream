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
it from the PC) into OUTPUT/app/PCSE00001 or OUTPUT/patch/PCSE00001, with the
marker file that tells the plugin to stream it.  Copy OUTPUT's contents to
ux0: and refresh the LiveArea (VitaShell: "Refresh LiveArea"); the game is
then launched from its bubble.

The module is also what app0stream_gui.py runs the server with.

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

MARKER = "app0stream.txt"   # src/a0s.h A0S_MARKER

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


class StubError(Exception):
    """A stub or param.sfo that cannot be made or read."""


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
        self.conn = self.server.stats.connect(self.peer, self.request)
        log(f"{self.peer} connected")

    def finish(self):
        self.server.library.close_files()
        self.server.stats.disconnect(self.peer)
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

            status, payload, rel = 0, b"", ""
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

            self.server.stats.request(self.conn, op, rel if op in (OP_STAT, OP_READ) else None,
                                      len(payload) if op == OP_READ else 0)

            try:
                sock.sendall(RSP.pack(RSP_MAGIC, seq, status, len(payload)) + payload)
            except OSError:
                return


class Stats:
    """What the server is doing, for the GUI: connections and totals."""

    def __init__(self):
        self.lock = threading.Lock()
        self.started = time.time()
        self.conns = {}         # peer -> dict
        self.requests = 0
        self.bytes = 0

    def connect(self, peer, sock):
        conn = {"peer": peer, "sock": sock, "since": time.time(),
                "requests": 0, "bytes": 0, "last_path": "", "last_time": time.time()}
        with self.lock:
            self.conns[peer] = conn
        return conn

    def disconnect(self, peer):
        with self.lock:
            self.conns.pop(peer, None)

    def request(self, conn, op, path, nbytes):
        now = time.time()
        with self.lock:
            self.requests += 1
            self.bytes += nbytes
            conn["requests"] += 1
            conn["bytes"] += nbytes
            conn["last_time"] = now
            if path:
                conn["last_path"] = path

    def snapshot(self):
        with self.lock:
            conns = [{k: v for k, v in c.items() if k != "sock"} for c in self.conns.values()]
            return {"requests": self.requests, "bytes": self.bytes,
                    "started": self.started, "connections": conns}

    def close_all(self):
        with self.lock:
            socks = [c["sock"] for c in self.conns.values()]
        for s in socks:
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def stop(self):
        """Stops accepting, drops every connection and frees the port."""
        self.shutdown()
        self.stats.close_all()
        self.server_close()


_log_sinks = []


def add_log_sink(fn):
    """fn(line) is called with every log line, from any thread."""
    _log_sinks.append(fn)


def log(msg):
    line = time.strftime("%H:%M:%S ") + msg
    if sys.stdout:
        print(line, flush=True)
    for fn in _log_sinks:
        fn(line)


def local_addresses():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.0.2.1", 9))     # nothing is sent
        addr = s.getsockname()[0]
        s.close()
        return [addr]
    except OSError:
        return []


def start_server(library, port=DEFAULT_PORT, bind="0.0.0.0", verbose=0):
    """A Server for library, bound and ready; run serve_forever() on it.
    Raises OSError when the port is taken."""
    lib = Library(library)
    if not os.path.isdir(lib.root):
        raise OSError(f"{library}: not a directory")

    for kind in ("app", "patch"):
        d = os.path.join(lib.root, kind)
        if os.path.isdir(d):
            titles = sorted(os.listdir(d))
            log(f"{kind}: {', '.join(titles) if titles else '(none)'}")

    server = Server((bind, port), Handler)
    server.library = lib
    server.verbose = verbose
    server.stats = Stats()

    for addr in local_addresses():
        log(f"listening on {addr}:{port} (server = {addr} in config.txt)")
    if not local_addresses():
        log(f"listening on port {port}")

    return server


def serve(args):
    try:
        server = start_server(args.library, args.port, args.bind, args.verbose)
    except OSError as e:
        sys.exit(str(e))

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


# ---- stubs ------------------------------------------------------------------

def read_sfo(path):
    """param.sfo's entries as a dict; dumps keep it in the clear."""
    with open(path, "rb") as f:
        data = f.read()

    magic, version, keys, values, count = struct.unpack_from("<4sIIII", data)
    if magic != b"\0PSF":
        raise StubError(f"{path}: not a param.sfo")

    out = {}
    for i in range(count):
        key_off, fmt, length, _, off = struct.unpack_from("<HHIII", data, 20 + i * 16)
        key = data[keys + key_off:data.index(b"\0", keys + key_off)].decode("utf-8", "replace")
        raw = data[values + off:values + off + length]
        if fmt == 0x0404:
            out[key] = struct.unpack("<I", raw[:4])[0]
        else:
            out[key] = raw.split(b"\0", 1)[0].decode("utf-8", "replace")
    return out


def find_folder(parent, name):
    """parent/name, matched without regard to case, or None."""
    if not os.path.isdir(parent):
        return None
    for n in os.listdir(parent):
        if n.upper() == name.upper():
            return os.path.join(parent, n)
    return None


def folder_size(path):
    return sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(path) for f in fs)


def titles(library):
    """Every title in library: id, name, version, sizes and whether it has a patch."""
    out = {}
    for kind in ("app", "patch"):
        base = os.path.join(library, kind)
        if not os.path.isdir(base):
            continue
        for name in sorted(os.listdir(base)):
            src = os.path.join(base, name)
            if not os.path.isdir(src):
                continue

            tid = name.upper()
            t = out.setdefault(tid, {"id": tid, "name": "", "version": "", "app": None, "patch": None})

            sfo = find_folder(src, "sce_sys")
            sfo = sfo and find_folder(sfo, "param.sfo")
            info = {}
            if sfo:
                try:
                    info = read_sfo(sfo)
                except (OSError, StubError, struct.error, ValueError):
                    pass

            t[kind] = {"path": src, "size": folder_size(src),
                       "version": info.get("APP_VER", ""), "valid": sfo is not None}
            if info.get("TITLE") and (kind == "app" or not t["name"]):
                t["name"] = info["TITLE"].replace("\n", " ")
            if info.get("APP_VER"):
                t["version"] = info["APP_VER"]

    return sorted(out.values(), key=lambda t: (t["name"] or t["id"]).lower())


def same_or_inside(a, b):
    """Whether folders a and b are the same, or one holds the other."""
    a = os.path.normcase(os.path.realpath(a))
    b = os.path.normcase(os.path.realpath(b))
    return a == b or a.startswith(b.rstrip(os.sep) + os.sep) or b.startswith(a.rstrip(os.sep) + os.sep)


def make_stub(library, title, output, patch=False, eboot=True):
    """Writes the memory card stub for title into output/app|patch/TITLE.
    Returns (path, size); raises StubError.

    It only ever creates files: an existing stub is left alone, and an
    output folder that is, holds or is inside the library is refused, so
    a game dump can never be touched."""
    kind = "patch" if patch else "app"
    title = title.upper()

    if same_or_inside(library, output):
        raise StubError("the stub folder must be outside the game library")

    src = find_folder(os.path.join(library, kind), title)
    if src is None:
        raise StubError(f"{title} is not in {os.path.join(library, kind)}")

    names = {n.lower(): n for n in os.listdir(src)}
    if "sce_sys" not in names:
        raise StubError(f"{src} has no sce_sys/: not a Vita game folder")

    dst = os.path.join(output, kind, title)
    if os.path.exists(dst):
        raise StubError(f"{kind}/{title} is there already; left as it is")

    for key in ("sce_sys", "sce_pfs"):
        if key in names:
            shutil.copytree(os.path.join(src, names[key]), os.path.join(dst, names[key]))

    if "eboot.bin" in names and eboot:
        shutil.copy2(os.path.join(src, names["eboot.bin"]), os.path.join(dst, names["eboot.bin"]))

    with open(os.path.join(dst, MARKER), "x") as f:
        f.write("streamed by app0stream\n")

    return dst, folder_size(dst)


def stub(args):
    try:
        dst, size = make_stub(args.library, args.titleid, args.output,
                              args.patch, not args.no_eboot)
    except (StubError, OSError) as e:
        sys.exit(str(e))

    kind = "patch" if args.patch else "app"
    if not os.path.isdir(os.path.join(dst, "sce_pfs")):
        print("note: no sce_pfs/, so this is a decrypted (non-PFS) dump")

    print(f"stub written to {dst} ({size // 1024} KiB)")
    print(f"copy it to ux0:{kind}/{args.titleid.upper()} and launch the game from its bubble")


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
