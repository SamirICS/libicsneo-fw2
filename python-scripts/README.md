# FreeWili 2 Python test scripts

Companion scripts used to validate the `libicsneo-fw2` port against real hardware.
Two independent code paths are included:

## 1. Raw OneWili (no libicsneo)

`freewili_can.py` talks to FreeWili 2 directly using the upstream
[`freewili/onewili`](https://github.com/freewili/onewili) Python package — it does
not use libicsneo at all. Useful as a baseline / sanity check.

```powershell
python -m venv venv
.\venv\Scripts\Activate.ps1
python -m pip install "onewili @ git+https://github.com/freewili/onewili.git#subdirectory=python"

python freewili_can.py --id 0x123 --data DE AD BE EF --seconds 10
python freewili_can.py --list-ports
python freewili_can.py --text-port COM12 --binary-port COM15 --demo --forever
```

## 2. Via libicsneo's `icsneopy` bindings (this port)

These exercise the actual `FreeWili2` device through libicsneo, exactly as any other
`icsneopy` application would.

First build and install the bindings (see the top-level [FW2_PORT.md](../FW2_PORT.md)
for the C++ build step):

```powershell
python install_icsneopy.py
```

Then:

| Script | Purpose |
|---|---|
| `fw2_can.py` | Send/receive classic CAN frames through `icsneopy`. |
| `fw2_canfd_test.py` | Send/receive CAN FD frames across all DLC lengths. |
| `fw2_icsneopy_can.py` | General interactive CAN send/receive example. |
| `fw2_loopback_test.py` | Self-contained two-device test: drives a ValueCAN 4 and a FreeWili 2 in one process to confirm bidirectional CAN without any external tool. |

```powershell
python fw2_can.py --id 0x123 --data DE AD BE EF --seconds 10
python fw2_canfd_test.py
python fw2_loopback_test.py
```

## Notes

- FreeWili 2 enumerates two USB serial ports: a CDC text/command port (e.g. `COM12`)
  and an FTDI binary event port (e.g. `COM15`). Pass them with `--text-port`/
  `--binary-port` if auto-discovery doesn't find them.
- `Device.get_messages()` (zero-arg overload) returns a `(list, bool)` tuple in the
  Python bindings — unpack it (`messages, _ = fw2.get_messages()`), don't iterate it
  directly.
- CAN RX arrives from the firmware as text events (`can0`/`can1`), which
  `FW2Communication` parses into normal `CANMessage` objects — no special handling is
  needed on the Python side.
