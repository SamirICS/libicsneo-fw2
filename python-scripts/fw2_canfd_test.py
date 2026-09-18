"""CAN-FD test harness for FreeWili 2 via icsneopy.

Sweeps every valid CAN-FD DLC payload length (0-8, 12, 16, 20, 24, 32, 48, 64
bytes) and reports which ones the controller accepts. Optionally enables BRS
(bit rate switch) and holds afterward to print anything received.

Requires icsneopy installed into this interpreter first:
    python scripts/install_icsneopy.py
"""

from __future__ import annotations

import argparse
import time

import icsneopy

# Every payload length a CAN-FD frame can carry; anything else has no DLC.
CANFD_LENGTHS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64]


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


def make_payload(length: int) -> list[int]:
    # Incrementing pattern rather than all-zero, so a byte-order or truncation
    # bug on the wire is visible instead of silently matching zeros.
    return [(i * 0x11) & 0xFF for i in range(length)]


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
    parser = argparse.ArgumentParser(description="CAN-FD DLC sweep test for FreeWili 2.")
    parser.add_argument("--serial", help="disambiguate when more than one FreeWili 2 is connected")
    parser.add_argument("--id", type=parse_arbid, default=0x1FD, help="CAN arbitration ID")
    parser.add_argument("--extended", action="store_true", help="use a 29-bit extended ID")
    parser.add_argument("--brs", action="store_true", help="set bit rate switch on every frame")
    parser.add_argument("--period", type=float, default=0.05, help="seconds between frames in the sweep")
    parser.add_argument("--hold-seconds", type=float, default=2.0, help="seconds to keep listening after the sweep")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    extended = args.extended or args.id > 0x7FF

    fw2 = find_freewili(args.serial)
    print(f"using {fw2.describe()}")
    print(f"sweeping {len(CANFD_LENGTHS)} CAN-FD DLC lengths: {CANFD_LENGTHS}\n")

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

    accepted, rejected = 0, 0
    try:
        for length in CANFD_LENGTHS:
            payload = make_payload(length)
            msg = icsneopy.CANMessage()
            msg.network = icsneopy.Network(icsneopy.Network.NetID.DWCAN_01)
            msg.arbid = args.id
            msg.isExtended = extended
            msg.isCANFD = True
            msg.baudrateSwitch = args.brs
            msg.data = payload

            ok = fw2.transmit(msg)
            accepted += ok
            rejected += not ok
            status = "OK  " if ok else "FAIL"
            print(f"[{status}] len={length:2d} data={' '.join(f'{b:02X}' for b in payload)}")
            time.sleep(args.period)

            messages, _ = fw2.get_messages()
            for msg in messages:
                if isinstance(msg, icsneopy.CANMessage):
                    received += 1
                    print(format_can_message(msg))

        print(f"\nsweep done: {accepted} accepted, {rejected} rejected")

        if args.hold_seconds > 0:
            print(f"listening for {args.hold_seconds}s...")
            deadline = time.monotonic() + args.hold_seconds
            while time.monotonic() < deadline:
                messages, _ = fw2.get_messages()
                for msg in messages:
                    if isinstance(msg, icsneopy.CANMessage):
                        received += 1
                        print(format_can_message(msg))
                time.sleep(0.05)
    finally:
        fw2.go_offline()
        fw2.close()

    print(f"\n{received} frame(s) received total")
    return 0 if rejected == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
