# libicsneo-fw2 — the FreeWili 2 port

A fork of [intrepidcs/libicsneo](https://github.com/intrepidcs/libicsneo) that makes
**FreeWili 2 (FW2) usable as an ordinary `icsneo::Device`**, so any libicsneo
application can drive CAN and CAN FD through an FW2 with no application-side changes.

The full upstream ICS device tree is kept. FW2 is an *added* device: a FIRE3 and an
FW2 enumerate side by side from one `icsneo::FindAllDevices()` call.

## Status

| | |
|---|---|
| Enumeration, `open()`, `close()`, heartbeat | ✅ working on hardware |
| `goOnline()` / `goOffline()`, power-zone bring-up | ✅ working on hardware |
| CAN transmit and receive | ✅ confirmed on the wire (validated against a ValueCAN 4-2) |
| CAN FD | ✅ confirmed, all 16 DLC lengths |
| Python bindings (`icsneopy`) | ✅ `DeviceType.FreeWili2` exposed and working end-to-end |
| Qt6 desktop GUI (`fw2_desktop`) | ✅ builds and runs |
| Windows | ✅ full driver: `Ports`-class device discovery + overlapped serial I/O |
| Settings (`FW2Settings::refresh()` / `apply()`) | ⬜ not implemented |

CAN RX arrives on this firmware as spontaneous **text events** (`can0`/`can1`) on the
main OneWili text port, not as the documented binary `canRxReport` WILI frames on the
FTDI port — `FW2Communication` parses those text events directly. An accept-all CAN
filter and event host streaming (`h\a\e 1`) must both be enabled for RX to reach the
host; `FreeWili2::enableNetworkCommunication()` does this automatically.

Everything else in libicsneo is untouched and the upstream test suite passes.

## Building

```sh
cmake -S . -B build -DLIBICSNEO_ENABLE_ONEWILI=ON
cmake --build build -j
```

`LIBICSNEO_ENABLE_ONEWILI` defaults off, so a build without it is byte-for-byte
upstream. It is force-disabled on macOS: the OneWili driver needs per-platform USB
enumeration, and only the Linux (sysfs) port exists.

On Linux, install the udev rules first or the FTDI port will not be openable without
root:

```sh
sudo tools/udev/install-fw2-udev.sh
```

Two upstream quirks bite on a fresh clone and are **not** caused by this fork: the
build unconditionally fetches and compiles protobuf via `intrepidcs/icspb` (~10
minutes), and `protoc` then fails unless its output directories are pre-created. Both
are avoidable by pointing `FETCHCONTENT_SOURCE_DIR_ICSPB` at an existing build tree.

### Windows quick start

Prerequisites:

- **Visual Studio Build Tools** (MSVC + `cl`/`msbuild` on `PATH`) — install the
  "Desktop development with C++" workload if `cmake --build` can't find a compiler.
- **CMake** 3.20+.
- **Python 3.10+** for the `icsneopy` bindings.
- **Qt 6.7+** (optional, only for the `fw2_desktop` GUI) — easiest via
  `pip install aqtinstall` then
  `aqt install-qt windows desktop 6.7.2 win64_msvc2019_64 -O C:\Qt`.

Use a **short build directory** outside the repo (e.g. `C:\fw2b`), not a nested path
like `libicsneo-fw2\build`. The protobuf dependency this project fetches has deeply
nested source paths that exceed Windows' `MAX_PATH` if the build dir itself is long:

```powershell
cmake -S . -B C:\fw2b -DLIBICSNEO_ENABLE_ONEWILI=ON -DLIBICSNEO_ENABLE_BINDINGS_PYTHON=ON
cmake --build C:\fw2b --config Release --target icsneocpp icsneopy fw2_smoke fw2_send fw2_desktop
```

(`fw2_desktop` is skipped automatically if Qt6 isn't found; drop it from `--target`
if you didn't install Qt.)

To use the Python bindings from any venv, run the helper in `python-scripts/`:

```powershell
python -m venv venv
.\venv\Scripts\Activate.ps1
python python-scripts\install_icsneopy.py
```

This copies the built `icsneopy` extension module into the venv's `site-packages` so
`import icsneopy` works from anywhere, without `sys.path` hacks. Re-run it after every
rebuild of the `icsneopy` target. It looks for the build under `C:\fw2b` by default —
pass a different `-B` path to `cmake` and edit `BUILD_DIRS` in the script if you used
one.

## How it works

libicsneo normally layers `Device` → `Communication` → `Packetizer` / `Encoder` /
`Decoder` / `Driver`, where the `Driver` is a byte pipe carrying ICS's own framed
packet protocol. FW2 speaks nothing resembling that protocol, so this port replaces
`Communication` wholesale rather than impersonating ICS framing at the byte level.

- **`OneWili : Driver`** owns FW2's two USB ports — a CDC ACM text port for commands
  and an FTDI port for binary event frames — behind libicsneo's single-`Driver`
  abstraction.
- **`FW2Communication : Communication`** translates at the message level: outbound
  `CANMessage` becomes a OneWili `i\c\w` text command, inbound `canRxReport` binary
  frames become `CANMessage`s. No ICS packet is ever produced or consumed.
- **`FreeWili2 : Device`** and **`FW2Settings`** override only what they must.
  Everything FW2 does not have — CoreMini, VSA, FlexRay, Ethernet PHY and so on —
  inherits libicsneo's existing "not supported" defaults.

The diff against upstream is deliberately small, so rebasing stays cheap: about 150
insertions across 10 files, roughly half of it build wiring. The one substantive core
change is a new `Communication::sendMessage()` virtual that `Device::transmit()` calls,
which is an upstreamable refactor rather than a fork-only hack.

**A rule this port follows throughout: fail honestly.** Unsupported commands report
`NotSupported` rather than being silently swallowed; unavailable settings report
"unknown" rather than synthesizing plausible defaults; unmeasured values stay
`nullopt` rather than being zeroed, because zero reads as a real measurement.

## Tools

Built alongside the library when `LIBICSNEO_ENABLE_ONEWILI=ON`:

| | |
|---|---|
| `tools/fw2_desktop/` | Qt6 desktop console — monitor, transmit, settings, power zones, raw OneWili. Skipped automatically if Qt6 is absent. |
| `build/tools/fw2_smoke` | Enumerate, open, go online, count frames for N seconds. The reference test. |
| `build/tools/fw2_tx` | Interactive transmit console (POSIX). |
| `tools/fw2_cmd.py` | Raw OneWili over the text port, bypassing libicsneo entirely. |
| `tools/fw2_listen.py` | Listens to both USB ports at once, text and binary. |

## Licensing

libicsneo is MIT and this fork inherits that for everything outside `third-party/`.

The vendored OneWili client in `third-party/onewili/` carries **no LICENSE file
upstream**. This is recorded in `third-party/onewili/README.md` and must be resolved
with the FreeWili team before `libicsneo-fw2` is distributed.
