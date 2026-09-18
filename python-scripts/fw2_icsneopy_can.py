"""Send and receive CAN/CAN-FD frames from a FreeWili 2 through icsneopy.

Unlike scripts/freewili_can.py (which talks raw OneWili text commands),
this goes through the ported libicsneo Windows driver in
libicsneo-fw2/platform/windows/onewiliwindows.cpp: FreeWili2 shows up as an
ordinary icsneo::Device, found by icsneopy.find_all_devices().

Requires the icsneopy extension module built from libicsneo-fw2 with
-DLIBICSNEO_ENABLE_BINDINGS_PYTHON=ON (see the icsneopy target under
C:\\fw2b\\bindings\\python\\Release). This script locates it automatically
without a manual copy/PYTHONPATH step.
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path


def _add_icsneopy_to_path() -> None:
    """Make the built icsneopy .pyd importable without a manual copy step."""
    try:
        import icsneopy  # noqa: F401
        return
    except ImportError:
        pass

    candidates = [
        Path(r"C:\fw2b\bindings\python\Release"),
        Path(r"C:\fw2b\bindings\python"),
        Path(__file__).resolve().parents[1] / "libicsneo-fw2" / "build-fw2" / "bindings" / "python" / "Release",
    ]
    for candidate in candidates:
        if candidate.is_dir() and any(candidate.glob("icsneopy*.pyd")):
            sys.path.insert(0, str(candidate))
            return

    raise SystemExit(
        "icsneopy not found. Build it first:\n"
        r"  cmake -S libicsneo-fw2 -B C:\fw2b -DLIBICSNEO_ENABLE_BINDINGS_PYTHON=ON" "\n"
        r"  cmake --build C:\fw2b --config Release --target icsneopy"
    )


_add_icsneopy_to_path()
import icsneopy  # noqa: E402


DLC_TO_LEN = {9: 12, 10: 16, 11: 20, 12: 24, 13: 32, 14: 48, 15: 64}


def parse_hex_bytes(tokens: list[str]) -> list[int]:
    parts: list[str] = []
    for token in tokens:
        parts.extend(piece for piece in re.split(r"[\s,:-]+", token.strip()) if piece)
    return [int(part, 16) for part in parts]


def find_freewili(serial: str | None = None) -> "icsneopy.Device":
    devices = icsneopy.find_all_devices()
    candidates = [d for d in devices if d.get_type().get_device_type() == icsneopy.DeviceType.Enum.FreeWili2]
    if serial is not None:
        candidates = [d for d in candidates if serial in d.describe()]

    if not candidates:
        listed = "\n".join(f"  - {d.describe()}" for d in devices) or "  (none)"
        raise SystemExit(f"no FreeWili 2 found. Devices seen:\n{listed}")
    if len(candidates) > 1:
        raise SystemExit(f"{len(candidates)} FreeWili 2 devices found; pass --serial to disambiguate")
    return candidates[0]


def format_can_message(msg: "icsneopy.CANMessage") -> str:
    data_hex = " ".join(f"{b:02X}" for b in msg.data)
    flags = []
    if msg.isExtended:
        flags.append("extended")
    if msg.isCANFD:
        flags.append("fd")
    if msg.baudrateSwitch:
        flags.append("brs")
    flag_text = ",".join(flags) if flags else "classic"
    return f"rx id=0x{msg.arbid:X} len={len(msg.data)} flags={flag_text} data={data_hex} ts={msg.timestamp}"


def parse_arbid(text: str) -> int:
    # int(text, 0) needs a "0x" prefix for hex; accept bare hex like "1ABCDEF" too.
    try:
        return int(text, 0)
    except ValueError:
        return int(text, 16)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Send/receive CAN via FreeWili 2 through icsneopy.")
    parser.add_argument("--serial", help="disambiguate when more than one FreeWili 2 is connected")
    parser.add_argument("--id", type=parse_arbid, default=0x123, help="CAN arbitration ID")
    parser.add_argument("--data", nargs="*", default=["DE", "AD", "BE", "EF"], help="payload bytes, e.g. DE AD BE EF")
    parser.add_argument("--count", type=int, default=10, help="frames to send, 0 = run forever")
    parser.add_argument("--period", type=float, default=0.1, help="seconds between sent frames")
    parser.add_argument("--no-send", action="store_true", help="only listen, do not transmit")
    parser.add_argument("--seconds", type=float, default=None, help="total run time; defaults to enough for --count")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    payload = parse_hex_bytes(args.data)
    extended = args.id > 0x7FF
    can_fd = len(payload) > 8

    fw2 = find_freewili(args.serial)
    print(f"using {fw2.describe()}")

    received = 0

    if not fw2.open():
        raise SystemExit(f"open() failed: {icsneopy.get_last_error().describe()}")
    if not fw2.go_online():
        fw2.close()
        raise SystemExit(f"go_online() failed: {icsneopy.get_last_error().describe()}")

    # Polling, not add_message_callback: the callback path does not reliably
    # fire through this pybind11 build, while enable_message_polling() +
    # get_messages() is confirmed working against real hardware.
    if not fw2.enable_message_polling():
        fw2.close()
        raise SystemExit(f"enable_message_polling() failed: {icsneopy.get_last_error().describe()}")

    try:
        deadline = time.monotonic() + (args.seconds if args.seconds is not None else args.count * args.period + 1.0)
        sent = 0
        next_send = time.monotonic()
        while time.monotonic() < deadline:
            now = time.monotonic()
            if not args.no_send and (args.count == 0 or sent < args.count) and now >= next_send:
                msg = icsneopy.CANMessage()
                msg.network = icsneopy.Network(icsneopy.Network.NetID.DWCAN_01)
                msg.arbid = args.id
                msg.isExtended = extended
                msg.isCANFD = can_fd
                msg.data = payload
                ok = fw2.transmit(msg)
                sent += 1
                print(f"tx #{sent} id=0x{args.id:X} data={' '.join(f'{b:02X}' for b in payload)} ok={ok}")
                next_send = now + args.period
            elif args.no_send or (args.count != 0 and sent >= args.count):
                if args.seconds is None:
                    break
            time.sleep(0.01)

            # get_messages() returns (list_of_messages, more_available_bool).
            messages, _ = fw2.get_messages()
            for msg in messages:
                if isinstance(msg, icsneopy.CANMessage):
                    received += 1
                    print(format_can_message(msg))
    finally:
        fw2.go_offline()
        fw2.close()

    print(f"\n{sent if not args.no_send else 0} sent, {received} received")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
