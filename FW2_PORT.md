# libicsneo-fw2 — the FreeWili 2 port

**Status: Phase 1 complete (skeleton builds and links). Nothing in this fork has
yet been run against real FreeWili 2 hardware.**

---

## 1. What this fork is

`libicsneo-fw2` is a fork of [intrepidcs/libicsneo](https://github.com/intrepidcs/libicsneo)
that makes **FreeWili 2 (FW2) usable as an ordinary `icsneo::Device`**, so that any
libicsneo application — Linux or Windows — can drive CAN and CAN FD through an FW2
with no application-side changes.

The full upstream ICS device tree is kept. FW2 is an *added* device: a FIRE3 and an
FW2 enumerate side by side from one `icsneo::FindAllDevices()` call.

### Why, rather than an application-level abstraction

`firmio_uds_flasher` already worked this problem from the other end: it introduced
an `ICanLink` seam *inside the flasher* so the flasher could drive either a FIRE3 or
an FW2. Only the flasher benefited.

This inverts that. Teaching *libicsneo* about FW2 means every libicsneo application
gets FW2 support for free, and the flasher's `ICanLink` work becomes unnecessary —
`IcsneoCanLink` alone covers both.

### Decisions taken up front

| | |
|---|---|
| **Seam** | Communication / Device layer — **not** a byte-level ICS protocol emulator at the Driver layer |
| **Scope** | Fork keeps the full ICS device tree; FW2 is an *added* device |
| **Firmware** | Stock OneWili firmware first; custom MAIN firmware permitted only if measurement demands it |
| **API surface** | `icsneocpp` (C++) and `icsneoc` (C). Not `icsnVC40` legacy, not the Python bindings |
| **Channels** | MVP is **one** CAN channel (`DWCAN_01`), but the channel index stays parametric throughout |

---

## 2. The mismatch, stated precisely

libicsneo's layering:

```
Device  (device/device.cpp, include/icsneo/device/device.h)
  └─ Communication          (communication/communication.cpp)
       ├─ Packetizer        ICS framing: 0xAA header, length, ICSChecksum
       ├─ Encoder/Decoder   ICS packet bytes  <->  CANMessage etc.
       └─ Driver            raw byte stream (CDCACM / D2XX / PCAP / firmio)
```

`Driver` is a byte pipe feeding a 512 KB `RingBuffer` that `Packetizer` parses into
ICS packets. FW2 speaks nothing resembling this, so the **Driver seam is the wrong
level** — using it would mean hand-writing ICS byte formats for serial number, reset
status, the settings EEPROM blob and CAN packets. We are not doing that.

### What FW2 actually offers

Two independent USB ports, asymmetric:

| | Text port | Binary port |
|---|---|---|
| Identity | CDC ACM, product string `"FW2 v01"` | FTDI, **VID 0x0403** |
| Direction | command → response (bidirectional) | **read-only** |
| Carries | `i\c\w` transmit, all settings | `canRxReport` events |
| Speed class | USB Full-Speed, 64 B packets | USB Hi-Speed, 512 B packets |

- **TX** — the text command `i\c\w <ch> <arbid:8hex> <fd> <xtd> <hex bytes>`, roughly
  33 bytes for an 8-byte frame.
- **RX** — 84-byte `canRxReport` frames in MCP2518FD register layout, carrying a
  `uint64_t time_stamp_ns` hardware timestamp.
- **Settings** — individual OneWili menu items (`h\s\p\a` CAN1 Mode, `h\s\p\b` CAN1
  Rate, `h\s\p\c` FD data rate, `h\s\p\y` listen-only, `h\s\p\e` TX retry, `h\s\p\1`
  termination, plus raw `C1NBTCFG` / `C1DBTCFG` register strings).

There is therefore **no adapter to write at the byte level; there is a translation to
write at the message level.** That is exactly why the seam sits at `Communication` —
we replace `Packetizer` + `Encoder` + `Decoder` wholesale rather than impersonating
them.

### The enabler

`Device::initialize()` builds the entire stack through virtuals —
`makeCommunication`, `makeEncoder`, `makeDecoder`, `makePacketizer`,
`makeSettings<Settings>`, `setupSupportedRX/TXNetworks`, `setupExtensions`. A device
subclass can therefore supply a `Communication` that ignores the packet pipeline
entirely, without `Device::open()` being touched at all.

`Device::open()` is left non-virtual. Instead we override the *steps inside it*:

| `Device::open()` step | FW2 handling |
|---|---|
| `com->open()` | delegates to the `OneWili` driver, which opens both serial ports |
| `attemptToBeginCommunication()` | **override** — real `i\g\u` liveness probe instead of the ICS serial/version handshake |
| `refreshComponentVersions()` | already gated on `supportsComponentVersions()`; overridden to `false` |
| `settings->refresh()` | `FW2Settings::refresh()` — Phase 3 |
| heartbeat → `Command::RequestStatusUpdate` every 110 ms | **`FW2Communication::sendCommand()` override** answers with a synthetic `ResetStatusMessage` backed by a real `i\g\u` round trip |

That last row is the good part: libicsneo's disconnect detection stays live and
becomes a *genuine* FW2 liveness check rather than a stub.

---

## 3. The core diff

Deliberately small, so rebasing onto upstream stays cheap. 149 insertions across 10
files, and roughly half of that is build wiring rather than semantics.

| File | Change |
|---|---|
| `include/icsneo/device/device.h` | `virtual` on `attemptToBeginCommunication()` and `enableNetworkCommunication()`; `ProductID::FreeWili2`; `forEachExtension()` moved private → protected |
| `include/icsneo/communication/communication.h` + `.cpp` | new `virtual bool sendMessage(const std::shared_ptr<Frame>&)`, defaulting to `encode` + `sendPacket` |
| `device/device.cpp` | `transmit()` tail calls `com->sendMessage(frame)` |
| `include/icsneo/device/devicetype.h` | `FreeWili2 = 0x0F000001` enum, `GetGenericProductName()` case, `ICSNEO_DEVICETYPE_FREEWILI2` |
| `device/devicefinder.cpp` | `#ifdef ICSNEO_ENABLE_ONEWILI` → `OneWili::Find()`, `makeIfSerialMatches<FreeWili2>`, `FreeWili2::DEVICE_TYPE` |
| `include/icsneo/platform/{posix,windows}/devices.h` | include `freewili2.h` under the guard |
| `platform/posix/linux/cdcacmlinux.cpp` | product-string filter so `CDCACM::Find()` does not double-enumerate an FW2 |
| `CMakeLists.txt` | `LIBICSNEO_ENABLE_ONEWILI` option, sources, vendored OneWili target |

Three notes on why these specific changes:

- **A derived class may override a *private* virtual in C++** (the NVI idiom), so
  making `attemptToBeginCommunication()` and `enableNetworkCommunication()` virtual
  required no access-level change. `forEachExtension` is different — it is *called*,
  not overridden, so it had to move to `protected`. Since `addExtension()` is public,
  silently dropping the extension hooks from the FW2 `goOnline`/`goOffline` would be
  a latent bug, not a harmless omission.
- **`Communication::sendMessage()` is a genuine, upstreamable refactor**, not a
  fork-only hack. Making `Device::transmit()` itself virtual would have lost its
  `isOpen` / `isOnline` / `isSupportedTXNetwork` checks and the extension
  `transmitHook` loop.
- **VID collision.** `INTREPID_USB_VENDOR_ID` is `0x093c` — *the same VID FreeWili
  uses*. `CDCACM::Find()` filters on VID alone and would happily claim an FW2. Both
  finders now discriminate by product string, in both directions.

---

## 4. New code

```
third-party/onewili/                      vendored OneWili C client (10.4k lines)
  CMakeLists.txt                          static lib, linked PRIVATE to icsneocpp
  README.md                               provenance + the license question

include/icsneo/platform/onewili.h         OneWili : Driver — 145 lines
platform/posix/onewili.cpp                open/close, read + write tasks — 264 lines
platform/posix/linux/onewililinux.cpp     sysfs enumeration, port pairing — 269 lines
platform/windows/onewiliwindows.cpp       honest stub, not yet written — 69 lines

include/icsneo/communication/fw2communication.h    147 lines
communication/fw2communication.cpp                 466 lines

include/icsneo/device/tree/freewili2/freewili2.h          174 lines
include/icsneo/device/tree/freewili2/freewili2settings.h  113 lines
```

### `OneWili : Driver`

Owns FW2's two USB ports behind libicsneo's single-`Driver` abstraction. This works
because `Communication::open()` / `close()` / `isOpen()` / `isDisconnected()` all
delegate straight to `driver` — a custom `Driver` subclass gets correct lifecycle
for free with zero core diff.

- **Enumeration** pairs the CDC ACM text port with the FTDI binary port by walking
  `/sys/bus/usb` and matching parent hubs. The binary port is matched by FTDI VID
  **only** — never by an `"FW2"` product substring, because that is the *text*
  port's product string.
- **Identity.** libicsneo's serial field is 6 characters and FW2's real serial is
  longer, so `Find()` derives `"FW"` + 4 base36 characters via FNV-1a. That derived
  value is what appears in `FindAllDevices()`.
- The binary port feeds the standard `RingBuffer`; the text port is line-split into
  a separate response queue (256-line / 64 KB caps) that `FW2Communication` drains.

### `FW2Communication : Communication`

The seam. Nothing in this class produces or consumes an ICS packet.

- **RX** — `rxTask()` parses WILI binary frames with the vendored `ow_bin_parser`,
  filters `header_type == 1` / 84-byte payloads, decodes `canRxReport`, and
  dispatches `CANMessage`s.
- **TX** — `sendMessage()` formats OneWili's exact `i\c\w` command and writes it
  **without waiting for the response**. This is the one place we deliberately do not
  reuse the vendored `ow__call`: that blocks for a full round trip per frame, which
  caps TX far below a saturated bus. Acks are reconciled asynchronously, with a
  pipelining depth of `MAX_IN_FLIGHT = 16`.
- **Synchronous path** — `callOneWili()` waits for the pipeline to drain before
  registering, so a settings response can never be mistaken for a TX ack. (OneWili
  response frames carry no request ID to correlate on.)

### `FreeWili2 : Device` and `FW2Settings : IDeviceSettings`

`FreeWili2` overrides only what it must: `getProductID`, `goOnline`/`goOffline`,
`attemptToBeginCommunication`, `enableNetworkCommunication`,
`setupSupportedRX/TXNetworks`, `requiresVehiclePower() = false`,
`supportsComponentVersions() = false`. Everything else — CoreMini, VSA, WiVI,
LiveData, FlexRay, MACsec, Ethernet PHY, logical disk — inherits libicsneo's existing
"not supported" defaults. That is not an oversight: libicsneo is already designed for
sparse devices, and `VividCAN` (58 lines) is the precedent.

`goOnline()` deliberately does *not* call `Device::goOnline()`, which drives an LED
state command, a `KeepAlive` Periodic and the network-mutex client-ID handshake —
three ICS packet-protocol commands with no OneWili equivalent. Reusing it would only
"work" by having `sendCommand()` silently swallow them, which is precisely the
failure mode this port avoids.

`FW2Settings` turned out nearly free: `IDeviceSettings::getBaudrateFor()` and
`setBaudrateFor()` operate purely on the in-memory staging buffer through the
`getCANSettingsFor()` / `getCANFDSettingsFor()` accessors. Only `refresh()` and
`apply()` ever touch the device. So `fw2_settings_t` plus two accessors is the entire
Phase 3 surface, and the flasher's existing `setBaudrateFor(DWCAN_01, 500000)` call
sites need no FW2-specific code once `apply()` lands.

### A design rule this port follows throughout: fail honestly

- `FW2Settings::refresh()` does **not** synthesize plausible defaults. Reporting
  "500 kbit" for a board actually running at 125 kbit is worse than reporting
  "unknown". It reports `SettingsNotAvailable` as a warning and returns false;
  `Device::open()` already tolerates a failed refresh.
- `FW2Communication::sendCommand()` reports `NotSupported` for unmapped ICS commands
  rather than swallowing them. A caller that thinks it configured something and did
  not is worse than an explicit failure.
- `answerStatusRequest()` leaves `busVoltage` and `deviceTemperature` as `nullopt`
  rather than zeroing them — zero would read as a real measurement.
- `attemptToBeginCommunication()` returns `NoDeviceResponse`, not
  `getCommunicationNotEstablishedError()` — all of that function's return values are
  `NoSerialNumber*` variants, which would be actively misleading when the serial *is*
  known from the USB descriptor.

---

## 5. Milestones

| Phase | Scope | Status |
|---|---|---|
| **0** | `tools/fw2_probe.c` throughput harness against vendored OneWili | ⬜ deferred behind the skeleton |
| **1** | Fork, vendor OneWili, core diff, `OneWili::Find()`, build wiring | ✅ **complete (build-verified only)** |
| **2** | Classic CAN both directions; verify the MCP2518FD bit layout and timestamp epoch | ⬜ blocked on hardware |
| **3** | `FW2Settings` mapping onto `h\s\p\*` and `C1NBTCFG` / `C1DBTCFG` | ⬜ |
| **4** | CAN FD: BRS, ESI, the DLC table (0…8, 12, 16, 20, 24, 32, 48, 64) | ⬜ |
| **5** | `icsneoc` C API and `DeviceType` string path; FW2 in the examples | ⬜ |
| **6** | Custom MAIN firmware for bulk TX — **only** if Phase 0 numbers demand it | ⬜ conditional |

### Phase 0 — measure, before writing firmware

A standalone probe against the vendored library. CAN bring-up must follow the order
`freewili2-platform-evaluation.md` Part 1b specifies, because **the first three steps
fail silently**: enable `5V_S15`; steer the PCAL6416A muxes (`CAN_CS_VREG_SEL`,
`CAN_nINT_SEL`, `CAN_CS_TXSW_SEL`) or confirm `ow_io_canfd_*` already does; request
Config mode and **poll `OPMOD` until it reads Config**; bring `IOCON` GPIO0 out of
standby (`CAN_STBY`) and set GPIO1 for termination; request Normal; enable the stream.

To publish: sustained TX rate blocking vs pipelined · RX loss over a 10k-frame burst ·
inter-frame gap against `canTx0` hardware timestamps · whether channel 1 exists ·
achieved bitrate on an analyzer. **This decides whether Phase 6 happens.**

### Phase 1 exit criterion — not yet met

`icsneo::FindAllDevices()` lists an FW2 alongside a FIRE3, and `open()` / `close()`
succeed with the heartbeat holding for 5 minutes. **Requires hardware.**

---

## 6. What Phase 1 actually accomplished

Everything below was verified on a Linux build machine with **no FW2 attached**.

- ✅ Vendored OneWili into `third-party/onewili/` with provenance recorded
- ✅ Core diff landed — 149 insertions across 10 files
- ✅ `OneWili : Driver` with sysfs enumeration and text/binary port pairing
- ✅ `FW2Communication` — binary RX parse → `CANMessage`, pipelined text TX,
  heartbeat backed by a real liveness probe
- ✅ `FreeWili2` device and `FW2Settings` skeleton
- ✅ CMake and `DeviceFinder` wiring behind `LIBICSNEO_ENABLE_ONEWILI`

Verified:

| Check | Result |
|---|---|
| Build with `LIBICSNEO_ENABLE_ONEWILI=ON` | clean, no warnings from new code |
| Build with `LIBICSNEO_ENABLE_ONEWILI=OFF` | clean, zero OneWili/FW2 references in the build log |
| Upstream `test/` suite | **75/75 pass** |
| `icsneo::GetSupportedDevices()` | reports `FreeWili 2 (0x0F000001)` |
| `icsneo::FindAllDevices()` | returns 0 devices, no error (no FW2 present) |

### What is explicitly *not* verified

This matters more than the list above.

- **No FW2 hardware has ever been attached to this machine.** Every wire-level
  assumption below is inference from the datasheet and the generated client, not
  measurement.
- **MCP2518FD R0/R1 bit layout** is datasheet-derived. Marked `TODO(bench)` in
  `communication/fw2communication.cpp`. "The datasheet layout, byte-swapped by
  firmware" is exactly the kind of thing that only shows up on a scope.
- **The FW2 timestamp epoch.** `time_stamp_ns` is nanoseconds like libicsneo, but its
  epoch is almost certainly boot, where ICS counts from 2007-01-01. Deltas and jitter
  are correct today; absolute wall-clock time is not.
- **TX throughput.** A saturated 500 kbit bus needs ~3,800 frames/s. A ~33-byte
  command on a 64-byte Full-Speed CDC endpoint gives roughly **1,900 commands/s even
  perfectly pipelined** — about 2× short. RX is fine (84 B × 3,800 fps ≈ 320 KB/s on
  a Hi-Speed FTDI pipe). Expect this port to be comfortable for monitoring,
  request-response diagnostics and periodic TX, and short for saturated bulk
  transmit until Phase 0 says otherwise.
- **Windows is a stub.** `platform/windows/onewiliwindows.cpp` compiles nothing real
  and has not been built at all — there is no MSVC or mingw on this machine. Two
  pieces remain: SetupAPI enumeration with sibling-hub pairing (note that **COM
  numbers can exceed 255, so `PackHandle()` must be widened**), and CreateFile / DCB /
  overlapped I/O (note that **the FW2 text port ignores writes unless DTR is
  asserted**). Windows builds will be done on a Windows machine from a clone of this
  repo.
- **Golden-vector tests do not exist yet.** `TODO(test)` markers sit on the command
  formatter and the `canRxReport` decode.

---

## 7. Building

```sh
cmake -S . -B build -DLIBICSNEO_ENABLE_ONEWILI=ON
cmake --build build -j
```

Two upstream quirks will bite on a fresh clone. **Neither is caused by this fork.**

1. `CMakeLists.txt` unconditionally `FetchContent`s `intrepidcs/icspb`, which
   downloads and builds protobuf 33.0 from source (~10 minutes). If a sibling
   libicsneo build tree already has it, reuse it:

   ```sh
   -DFETCHCONTENT_SOURCE_DIR_ICSPB=../libicsneo/build/_deps/icspb-src
   -DICSPB_BOOTSTRAP_DIR=../libicsneo/build/_deps/icspb-build/_bootstrap
   ```

2. `protoc` then fails with `<build>/_deps/icspb-build/protos/: No such file or
   directory` — it does not create its own output subdirectories. Pre-create them
   mirroring `icspb-src/protos/` before building.

`LIBICSNEO_ENABLE_ONEWILI` is force-disabled on Darwin with a warning: the OneWili
driver needs per-platform USB enumeration, and only the Linux (sysfs) and Windows
(SetupAPI) ports exist. Unlike CDCACM there is no "use the Linux code and hope"
fallback — the Linux port reads `/sys/bus/usb` directly.

---

## 8. Verification plan

**Regression on real ICS hardware first.** The core diff touches
`Device::transmit()`, which *every* ICS device uses. Before FW2 work continues:
build for FIRE3, run the flasher's `uds_tester` and `test_server_flow` against
`uds_ecu_sim`, flash a real ECU, and **diff the CAN frame log against a pre-fork
capture — bit-identical, or the refactor is wrong.**

**Unit / hardware-free.** Golden-vector tests for `CANMessage` ↔ OneWili command
text, and for `canRxReport` bytes → `CANMessage` (all DLCs, standard and extended
IDs, error frames, FD flags). Following the `ics_canopen` rhythm: golden frames,
mutation tests, config matrix.

**Loopback.** Against a second CAN node: classic and FD, standard and extended IDs,
every FD DLC boundary, filters, a 10k-frame burst for loss, and a sustained burst
timed against `canTx0` for jitter.

**Cross-hardware equivalence — the strongest check available.** Run one unmodified
libicsneo application against a FIRE3 and an FW2 on the same bus and diff the message
streams. Then the real prize: point `firmio_uds_flasher` at an FW2 with **zero
flasher code changes** and flash the EMS (`7E0/7E8`) and TCU (`7E1/7E9`). Expect
≈3.6 min and ≈1.2 min of bus time at 500 kbit; a large overshoot means TX throughput
is the limit, which is Phase 0's gate resurfacing.

---

## 9. Open questions

**Settled:** one CAN channel for the MVP (`DWCAN_01`). A second — FW2's own `can1`,
or one added by the **San Diego Orca** expansion — is deferred, with the channel
index kept parametric so it stays cheap to add.

Deferred deliberately. None of these changes the architecture; they change constants,
a settings mapping, or one message type. Answer them as we hit them.

1. **Can the text port sustain pipelined writes without dropping or reordering?**
   The vendored `ow__call` flushes stale input on every call, so pipelining is
   entirely our code's responsibility. *Phase 0 — same measurement as the TX rate.*
2. **Is the text port really Full-Speed CDC?** The vendored client opens at a nominal
   "1,000,000 baud", but the actual endpoint size sets the TX ceiling. *Phase 0, or
   `lsusb -v` in five minutes.*
3. **Does `ow_io_canfd_setup_filter` cover what libicsneo apps expect,** or do we
   filter in software? *Phase 2. Software filtering is an acceptable fallback, so
   this cannot block us.*
4. **Error frames and bus state.** libicsneo has `Message::Type::CANError` and a
   populated `HardwareCANErrorPacket` (error code, REC, TEC, error-warn,
   error-passive, bus-off). `canRxReport` carries only a single `error` bit, so
   REC/TEC/bus-off may need an MCP2518FD register read — `C1TREC` has all of it.
   *Phase 2.*

**Worth asking the FreeWili team early**, since the answers are free and shape Phase
0's scope: does `ow_io_canfd_*` already sequence the PCAL6416A muxes and `IOCON`? Is
there any bulk/binary TX path, or is the text port the only way in?

---

## 10. Licensing — blocks any public release

The vendored OneWili client in `third-party/onewili/` carries **no LICENSE file
upstream**. This is recorded in `third-party/onewili/README.md` and must be resolved
with the FreeWili team before `libicsneo-fw2` is published anywhere.

libicsneo itself is MIT; this fork inherits that for everything outside
`third-party/`.

---

## Appendix — what "ICS framing" actually is

Recorded because it is the whole reason this port exists. All of it lives in
`communication/packetizer.cpp` and `communication/packet/canpacket.cpp`.

**Layer 1 — the envelope** (`Packetizer::packetWrap`). Every packet starts with
`0xAA`. Two forms:

- *Short*: `0xAA`, then one byte packing `length << 4 | netid` — 4 bits of length
  (max 15 payload bytes) and 4 bits of network ID — then payload, then a one-byte
  `ICSChecksum` (sum, invert, add one). Some devices 16-bit-align by appending `'A'`.
- *Long*: length nibble `0`, then a little-endian 16-bit length at bytes 3–4 and a
  little-endian 16-bit netid at bytes 5–6. No checksum. Length here covers the whole
  packet including the `0xAA`; the short form's length covers payload only.

`Packetizer::input()` is a five-state machine over the ring buffer —
`SearchForHeader` → `ParseHeader` → `{ParseLongStylePacketHeader | ParseDiskDataHeader}`
→ `GetData` — resynchronising on `0xAA` whenever a length is implausible.

**Layer 2 — the CAN payload** (`HardwareCANPacket`). A packed struct of 16-bit
bitfields laid out to match ICS firmware's own memory: `IDE/SRR/SID:11/EDL/BRS/ESI`,
then `EID:12/TXMSG/TXAborted/TXLostArb/TXError`, then
`DLC:4/RB0/IVRIF/HVEnable/ExtendedNetworkIndexBit/RB1/RTR/EID2:6`, 8 data bytes, a
`stats` word, a 60-bit timestamp in `timestampResolution` ticks (25 ns on most
devices), then `NetworkID` and `Length`. A 29-bit extended ID is reassembled from
three separate fields: `SID<<18 | EID<<6 | EID2`.

`HardwareCANErrorPacket` overlays the same bytes; the `ERROR_INDICATOR` bit selects
it, and it carries `error_code`, `brs_data_error_code`, `REC`, `TEC`, and flags for
error-warn / error-passive / bus-off.

**Why this is the crux.** This format is a contract between libicsneo and *ICS
firmware*. FIRE3 speaks it because ICS wrote both ends. FW2 has no idea it exists —
its wire format is ASCII commands (`i\c\w 0 000007E0 0 0 02 10 03`) outbound and
84-byte MCP2518FD-shaped `canRxReport` frames inbound, on a *different USB port*.
