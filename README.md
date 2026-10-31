# app0stream

**app0stream** is a PS Vita kernel plugin for retail 3.65 consoles (running HENkaku Ensō) that streams game data directly from a PC over Wi-Fi on demand.

Games remain in their original dumped format—PFS-encrypted directories are supported directly without prior decryption. Instead of installing the full game on the console, only a lightweight stub (`sce_sys/`, `sce_pfs/`, and `eboot.bin`, typically a few megabytes) is placed on the memory card.

## How It Works

When a game launches, the operating system mounts its application directory (`ux0:app/TITLEID`) via `scePfsMount`. `app0stream` hooks this system call and redirects configured titles to `net0:/app/TITLEID`—a read-only virtual file system backed by the PC server. Because PFS mounts on top of `net0:` just as it would on the memory card, decryption, licenses, game updates, and save data all function normally.

File reads pass through a kernel RAM cache with sequential readahead over up to four parallel TCP connections. Dropped connections automatically retry for up to `timeout_s` seconds (with the retry timer resetting whenever the console wakes from sleep).

---

## Setup

### 1. PC Server

**Prerequisites:** Python 3.8 or newer (no third-party packages required).

Organize your game dumps in a directory structure mirroring the PS Vita memory card:

```text
Library/
  app/PCSE00001/        # Base game (as dumped)
  patch/PCSE00001/      # Game update (optional)
```

Generate the memory card stub for your title (and its patch, if applicable):

```bash
python app0stream_server.py stub Library PCSE00001 Stubs
python app0stream_server.py stub Library PCSE00001 Stubs --patch
```

Start the streaming server:

```bash
python app0stream_server.py serve Library
```

Upon startup, the server prints the IPv4 address to use in your console configuration.

#### Windows Network & Firewall Configuration

* **Firewall Rule:** Open TCP port `7350` to your local network by running the following command once in an **Administrator PowerShell**:

  ```powershell
  New-NetFirewallRule -DisplayName "app0stream server" -Direction Inbound `
      -Protocol TCP -LocalPort 7350 -RemoteAddress LocalSubnet -Action Allow -Profile Any
  ```

  *Note:* Creating a port-based rule ensures connectivity regardless of which `python.exe` binary runs the server (Windows treats the `py` launcher and the underlying Python interpreter as separate executables). If you previously declined a Windows Firewall prompt for Python, an explicit **Block** rule for `python.exe` may exist and override this Allow rule. To fix this, disable any Python block rules under **Windows Defender Firewall > Advanced Settings > Inbound Rules**.

* **WSL Limitation:** Do not run the server inside WSL. WSL runs behind NAT and cannot be reached directly by the console; use native Windows Python instead.

---

### 2. Console Installation

1. Copy `app0stream.skprx` to `ur0:tai/` and register it under `*KERNEL` in `ur0:tai/config.txt`:

   ```ini
   *KERNEL
   ur0:tai/app0stream.skprx
   ```

2. Copy the contents of your generated `Stubs/` folder to `ux0:` (producing paths such as `ux0:app/PCSE00001/sce_sys/...`).
   * **NoNpDrm dumps** include their fake license at `sce_sys/package/work.bin`, which will be installed when refreshing the LiveArea.
   * **Retail/owned games** continue to use their existing license in `ux0:license/app/...`.

3. Create the configuration file at `ux0:data/app0stream/config.txt`:

   ```ini
   server = 192.168.1.20
   title = PCSE00001
   patch = PCSE00001
   ```

4. Reboot the console, open **VitaShell**, and run **Refresh LiveArea** to populate the game bubble on the home screen.

---

## Configuration (`config.txt`)

`ux0:data/app0stream/config.txt` is parsed automatically every time a game launches—no reboot is required after making changes.

| Key | Default | Description |
| :--- | :--- | :--- |
| `server` | *(Required)* | IPv4 address of the host PC running the server. |
| `port` | `7350` | TCP port used by the server. |
| `title` | *(None)* | Streams `ux0:app/TITLEID` (specify one `title` line per game). |
| `patch` | *(None)* | Also streams `ux0:patch/TITLEID` (specify one `patch` line per patched game). |
| `connections` | `2` | Number of parallel TCP connections (`1` to `4`). |
| `cache_kb` | `256` | Kernel RAM cache size in KiB (`256` to `16384`). |
| `readahead_kb` | `256` | Prefetch window size in KiB for sequential reads. |
| `timeout_s` | `120` | Duration (in seconds) to retry a failed request before aborting. |
| `log` | `1` | Enables logging to `ux0:data/app0stream/log.txt` (`1` = enabled, `0` = disabled). |
| `trace` | `0` | Mirrors every file operation to the PC server console (`1` = enabled, `0` = disabled). |

### Behavioral Notes

* **Launch Fallback:** If the server cannot be reached within 10 seconds of launching a game, `app0stream` falls back to mounting the local memory card directory. Because only the stub is present locally, the game will fail to start cleanly rather than hanging the system.
* **Kernel Memory (`cache_kb`):** The read cache is allocated from the kernel heap, which is shared with the OS and other plugins. On consoles running multiple kernel plugins, setting a large cache (e.g., 2 MiB) can exhaust kernel memory and cause the console to power off after the initial loading screen. Increase `cache_kb` incrementally and revert to a lower value if the console shuts down during gameplay.
* **Request Tracing (`trace = 1`):** Sends every `open`, `read`, and `close` event to the server in real time (logged as `vita ...` entries) so the exact request preceding a crash is preserved on the PC. Because each read waits for an additional network round trip, enable this option only for debugging.

---

## Limitations

* **Add-on Content (DLC):** DLC is mounted read-write by the OS and cannot be streamed. Install DLC directly to the memory card.
* **Network Bandwidth:** Throughput is constrained by the PS Vita's Wi-Fi hardware (typically 1.5 to 3 MB/s). Expect longer loading screens and potential stuttering in titles that continuously stream large assets.
* **Case Sensitivity:** File names are matched case-insensitively (matching exFAT behavior) for ASCII, Latin, Greek, Cyrillic, and fullwidth characters. Other scripts require exact case matching (though game assets rarely use them).
* **Lifecycle:** As a kernel plugin, `app0stream` cannot be unloaded dynamically while the system is running.

---

## Building & Testing

Building the plugin requires [VitaSDK](https://vitasdk.org):

```bash
mkdir build && cd build
cmake .. && make
```

To run the host test suite (covering the portable client, cache, configuration parser, and filename decoding logic) against a live server instance:

```bash
cd tests/host && make
./test SERVER_IP 7350 Library PCSE00001
```

## Protocol

For wire protocol specifications and packet structures, refer to `src/proto.h`.