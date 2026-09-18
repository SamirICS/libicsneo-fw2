"""Send and receive CAN/CAN-FD frames from a FreeWili 2 via icsneopy.

Requires icsneopy installed into this interpreter's site-packages first:
    python scripts/install_icsneopy.py
"""

from __future__ import annotations

import argparse
import re
import time

import icsneopy


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

    received = 0
    sent = 0
    try:
        deadline = time.monotonic() + (args.seconds if args.seconds is not None else args.count * args.period + 1.0)
        next_send = time.monotonic()
        while time.monotonic() < deadline:
            now = time.monotonic()
            can_still_send = not args.no_send and (args.count == 0 or sent < args.count)
            if can_still_send and now >= next_send:
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
            elif not can_still_send and args.seconds is None:
                break
            time.sleep(0.01)

            # get_messages() returns (list_of_messages, more_available_bool),
            # not the list directly -- unpack it, don't iterate the pair itself.
            messages, _ = fw2.get_messages()
            for msg in messages:
                if isinstance(msg, icsneopy.CANMessage):
                    received += 1
                    print(format_can_message(msg))
    finally:
        fw2.go_offline()
        fw2.close()

    print(f"\n{sent} sent, {received} received")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
