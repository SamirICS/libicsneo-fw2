"""Self-contained CAN loopback test: ValueCAN4 <-> FreeWili2, one process.

Removes all cross-tool timing/coordination guesswork (Vehicle Spy, separate
terminals, etc.) by driving both devices from the same script: ValueCAN4
transmits while FreeWili2 listens, and vice versa, at the same time.

Requires icsneopy installed first:
    python scripts/install_icsneopy.py

Wire ValueCAN4's DWCAN_01 to FreeWili2's CAN bus (CAN-H/CAN-L), with proper
120-ohm termination at both ends, same bitrate on both.
"""

from __future__ import annotations

import argparse
import time

import icsneopy

VC4_TO_FW2_ARBID = 0x555
FW2_TO_VC4_ARBID = 0x556


def find_device(kind: str) -> "icsneopy.Device":
    devices = icsneopy.find_all_devices()
    if kind == "freewili":
        candidates = [d for d in devices if d.get_type().get_device_type() == icsneopy.DeviceType.Enum.FreeWili2]
    else:
        candidates = [d for d in devices if d.get_type().get_device_type() != icsneopy.DeviceType.Enum.FreeWili2]

    if not candidates:
        listed = "\n".join(f"  - {d.describe()}" for d in devices) or "  (none)"
        raise SystemExit(f"no {kind} device found. Devices seen:\n{listed}")
    return candidates[0]


def open_online_poll(device: "icsneopy.Device") -> None:
    if not device.open():
        raise SystemExit(f"{device.describe()}: open() failed: {icsneopy.get_last_error().describe()}")
    if not device.go_online():
        device.close()
        raise SystemExit(f"{device.describe()}: go_online() failed: {icsneopy.get_last_error().describe()}")
    if not device.enable_message_polling():
        device.go_offline()
        device.close()
        raise SystemExit(f"{device.describe()}: enable_message_polling() failed: {icsneopy.get_last_error().describe()}")


def make_message(arbid: int, counter: int) -> "icsneopy.CANMessage":
    msg = icsneopy.CANMessage()
    msg.network = icsneopy.Network(icsneopy.Network.NetID.DWCAN_01)
    msg.arbid = arbid
    msg.isExtended = False
    msg.isCANFD = False
    msg.data = [0xDE, 0xAD, 0xBE, 0xEF, (counter >> 8) & 0xFF, counter & 0xFF]
    return msg


def drain(device: "icsneopy.Device", expected_arbid: int, counts: dict) -> None:
    messages, _ = device.get_messages()
    for msg in messages:
        if not isinstance(msg, icsneopy.CANMessage):
            continue
        counts["total"] += 1
        if msg.arbid == expected_arbid:
            counts["matched"] += 1
        else:
            print(f"  unexpected arbid=0x{msg.arbid:X} on {device.describe()}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Two-device loopback test: ValueCAN4 <-> FreeWili2.")
    parser.add_argument("--seconds", type=float, default=10.0, help="test duration")
    parser.add_argument("--period", type=float, default=0.05, help="seconds between transmits, each direction")
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    vc4 = find_device("other")
    fw2 = find_device("freewili")
    print(f"vc4: {vc4.describe()}")
    print(f"fw2: {fw2.describe()}")

    open_online_poll(vc4)
    open_online_poll(fw2)

    vc4_to_fw2 = {"total": 0, "matched": 0}  # counted on fw2
    fw2_to_vc4 = {"total": 0, "matched": 0}  # counted on vc4
    vc4_sent = fw2_sent = 0

    try:
        deadline = time.monotonic() + args.seconds
        next_send = time.monotonic()
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_send:
                vc4.transmit(make_message(VC4_TO_FW2_ARBID, vc4_sent))
                fw2.transmit(make_message(FW2_TO_VC4_ARBID, fw2_sent))
                vc4_sent += 1
                fw2_sent += 1
                next_send = now + args.period

            drain(fw2, VC4_TO_FW2_ARBID, vc4_to_fw2)
            drain(vc4, FW2_TO_VC4_ARBID, fw2_to_vc4)
            time.sleep(0.01)
    finally:
        vc4.go_offline()
        fw2.go_offline()
        vc4.close()
        fw2.close()

    print(f"\nVC4 -> FW2: sent {vc4_sent}, FreeWili received {vc4_to_fw2['matched']} matching "
        f"({vc4_to_fw2['total']} total frames seen)")
    print(f"FW2 -> VC4: sent {fw2_sent}, ValueCAN received {fw2_to_vc4['matched']} matching "
        f"({fw2_to_vc4['total']} total frames seen)")

    return 0 if vc4_to_fw2["matched"] > 0 and fw2_to_vc4["matched"] > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
