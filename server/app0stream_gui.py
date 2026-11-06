#!/usr/bin/env python3
"""
app0stream desktop app: the library, the server and the stub generator in
one window.

    pip install pywebview pystray pillow
    python app0stream_gui.py

It runs the same server as "app0stream_server.py serve", in this process.
On Windows, minimizing the window sends it to the tray (pystray), where the
server keeps running; the tray menu shows it again or quits.

Settings are kept in %APPDATA%\\app0stream\\settings.json (or
~/.config/app0stream/settings.json, or $APP0STREAM_SETTINGS).
"""

import collections
import json
import os
import sys
import threading

import webview

import app0stream_server as srv

HERE = os.path.dirname(os.path.abspath(__file__))

DEFAULTS = {
    "library": "",
    "output": "",
    "port": srv.DEFAULT_PORT,
    "autostart": False,
    "eboot": True,
    "write_config": True,
    "address": "",
}


def settings_path():
    if os.environ.get("APP0STREAM_SETTINGS"):
        return os.environ["APP0STREAM_SETTINGS"]
    base = os.environ.get("APPDATA") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "app0stream", "settings.json")


def load_settings():
    s = dict(DEFAULTS)
    try:
        with open(settings_path(), encoding="utf-8") as f:
            s.update({k: v for k, v in json.load(f).items() if k in DEFAULTS})
    except (OSError, ValueError):
        pass
    return s


def save_settings(s):
    path = settings_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(s, f, indent=2)
    os.replace(tmp, path)


def tray_image():
    """The tray icon: a play mark on a rounded square."""
    from PIL import Image, ImageDraw

    img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((2, 2, 61, 61), radius=14, fill=(37, 99, 235, 255))
    d.polygon([(24, 17), (24, 47), (48, 32)], fill=(255, 255, 255, 255))
    return img


class Api:
    """What the page calls (window.pywebview.api).  Attributes starting
    with an underscore are not exposed to it."""

    def __init__(self):
        self._settings = load_settings()
        self._lock = threading.Lock()
        self._server = None
        self._thread = None
        self._error = ""
        self._log = collections.deque(maxlen=1000)
        self._log_seq = 0
        self._titles = []
        self._window = None
        self._tray = None
        self._external = None    # (port, is app0stream) of a server not ours

        srv.add_log_sink(self._on_log)

    # log

    def _on_log(self, line):
        with self._lock:
            self._log_seq += 1
            self._log.append((self._log_seq, line))

    def get_log(self, since=0):
        with self._lock:
            return {"seq": self._log_seq,
                    "lines": [l for n, l in self._log if n > since]}

    # state

    def get_state(self):
        with self._lock:
            server = self._server
            error = self._error

        external = None
        if server is None:
            external = self._find_external(int(self._settings["port"]))

        state = {
            "settings": self._settings,
            "addresses": srv.local_addresses(),
            "running": server is not None,
            "external": external,
            "error": error,
            "stats": server.stats.snapshot() if server else None,
            "settings_path": settings_path(),
        }
        return state

    def _find_external(self, port):
        """What holds port when this app's server does not: "app0stream",
        "other" or None.  A HELLO is sent only when that changes."""
        if not srv.port_in_use(port):
            self._external = None
            return None

        if self._external is None or self._external[0] != port:
            known = srv.probe_server(port) is not None
            self._external = (port, known)

        return "app0stream" if self._external[1] else "other"

    def set_settings(self, changes):
        for k, v in changes.items():
            if k in DEFAULTS:
                self._settings[k] = v
        try:
            save_settings(self._settings)
        except OSError as e:
            return {"ok": False, "error": str(e)}
        return {"ok": True}

    def choose_folder(self, key):
        """Returns {"path": ...}, {"error": ...} or None when cancelled."""
        start = self._settings.get(key) or os.path.expanduser("~")
        kind = getattr(getattr(webview, "FileDialog", None), "FOLDER", None)
        if kind is None:
            kind = webview.FOLDER_DIALOG
        picked = self._window.create_file_dialog(kind, directory=start)
        if not picked:
            return None
        path = picked[0] if isinstance(picked, (list, tuple)) else picked

        other = self._settings["output" if key == "library" else "library"]
        if other and srv.same_or_inside(path, other):
            return {"error": "The stub folder and the game library must be separate folders."}

        self.set_settings({key: path})
        return {"path": path}

    # library

    def get_titles(self):
        lib = self._settings["library"]
        if not lib or not os.path.isdir(lib):
            self._titles = []
            return {"ok": False, "error": "Choose the folder that holds app/ and patch/."}
        try:
            self._titles = srv.titles(lib)
        except OSError as e:
            return {"ok": False, "error": str(e)}
        return {"ok": True, "titles": self._titles}

    # server

    def start_server(self):
        with self._lock:
            if self._server is not None:
                return {"ok": True}

        port = int(self._settings["port"])
        if srv.port_in_use(port):
            what = self._find_external(port)
            msg = (f"another app0stream server is already serving on port {port}; stop it first"
                   if what == "app0stream" else f"port {port} is used by another program")
            return {"ok": False, "error": msg}

        try:
            server = srv.start_server(self._settings["library"], int(self._settings["port"]))
        except (OSError, ValueError) as e:
            with self._lock:
                self._error = str(e)
            srv.log(f"cannot start: {e}")
            return {"ok": False, "error": str(e)}

        thread = threading.Thread(target=server.serve_forever, name="app0stream", daemon=True)
        with self._lock:
            self._server = server
            self._thread = thread
            self._error = ""
        thread.start()
        self._update_tray()
        return {"ok": True}

    def stop_server(self):
        with self._lock:
            server, thread = self._server, self._thread
            self._server = self._thread = None

        if server is None:
            return {"ok": True}

        server.stop()
        thread.join()
        srv.log("server stopped")
        self._update_tray()
        return {"ok": True}

    # stubs

    def make_stubs(self, picks):
        """picks: [{"id": ..., "patch": bool}].  Writes each stub, and
        data/app0stream/config.txt when asked to.  Nothing that exists is
        ever replaced or deleted."""
        s = self._settings
        lib, out = s["library"], s["output"]
        if not out:
            return {"ok": False, "error": "Choose a folder for the stubs first.", "results": []}
        if lib and srv.same_or_inside(lib, out):
            return {"ok": False, "results": [],
                    "error": "The stub folder must be outside the game library. Pick an empty folder elsewhere."}

        results = []
        for p in picks:
            kinds = [False] + ([True] if p.get("patch") else [])
            for patch in kinds:
                where = f"{'patch' if patch else 'app'}/{p['id']}"
                try:
                    path, size = srv.make_stub(lib, p["id"], out, patch, s["eboot"])
                    results.append({"what": where, "ok": True, "size": size, "path": path})
                    srv.log(f"stub {where} written ({size // 1024} KiB)")
                except (srv.StubError, OSError) as e:
                    results.append({"what": where, "ok": False, "error": str(e)})

        if s["write_config"]:
            results.append(self._write_config(out))

        return {"ok": all(r["ok"] for r in results), "results": results}

    def _write_config(self, out):
        addr = self._settings["address"] or next(iter(srv.local_addresses()), "")
        where = "data/app0stream/config.txt"
        if not addr:
            return {"what": where, "ok": False, "error": "no network address to put in it"}

        lines = [f"server = {addr}"]
        port = int(self._settings["port"])
        if port != srv.DEFAULT_PORT:
            lines.append(f"port = {port}")

        path = os.path.join(out, "data", "app0stream", "config.txt")
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "x", newline="\n") as f:
                f.write("\n".join(lines) + "\n")
        except FileExistsError:
            return {"what": where, "ok": True, "kept": True, "path": path, "size": os.path.getsize(path)}
        except OSError as e:
            return {"what": where, "ok": False, "error": str(e)}

        return {"what": where, "ok": True, "path": path, "size": os.path.getsize(path)}

    def open_folder(self, key):
        path = self._settings.get(key)
        if not path or not os.path.isdir(path):
            return False
        if sys.platform == "win32":
            os.startfile(path)
        else:
            import subprocess
            subprocess.Popen(["xdg-open", path])
        return True

    # window and tray

    def _update_tray(self):
        if self._tray is not None:
            self._tray.title = self._tray_title()
            self._tray.update_menu()

    def _tray_title(self):
        if self._server:
            return "app0stream: serving"
        if self._external and self._external[1]:
            return "app0stream: serving from another process"
        return "app0stream: stopped"

    def _show(self, *_):
        self._window.show()
        self._window.restore()

    def _toggle(self, *_):
        if self._server:
            self.stop_server()
        else:
            self.start_server()

    def _quit(self, *_):
        self.stop_server()
        if self._tray is not None:
            self._tray.stop()
        self._window.destroy()

    def _start_tray(self):
        import pystray

        menu = pystray.Menu(
            pystray.MenuItem("Show", self._show, default=True),
            pystray.MenuItem(lambda _: "Stop server" if self._server else "Start server", self._toggle),
            pystray.Menu.SEPARATOR,
            pystray.MenuItem("Quit", self._quit),
        )
        self._tray = pystray.Icon("app0stream", tray_image(), self._tray_title(), menu)
        self._tray.run_detached()

    def _on_minimized(self):
        if self._tray is not None:
            self._window.hide()


def main():
    api = Api()

    window = webview.create_window(
        "app0stream",
        url=os.path.join(HERE, "app0stream_gui.html"),
        js_api=api,
        width=1040,
        height=720,
        min_size=(760, 520),
    )
    api._window = window

    # the tray is what minimizing hides the window to; without it, it is a
    # plain minimize
    if sys.platform == "win32":
        try:
            api._start_tray()
            window.events.minimized += api._on_minimized
        except ImportError:
            srv.log("pystray is not installed: minimizing will not go to the tray")

    window.events.closed += api.stop_server

    if api._settings["autostart"] and api._settings["library"]:
        api.start_server()

    webview.start()

    if api._tray is not None:
        api._tray.stop()


if __name__ == "__main__":
    main()
