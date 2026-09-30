"""Send and receive CAN/CAN-FD frames with a FreeWili 2.

The script uses the generated OneWili Python API from
https://github.com/freewili/onewili.
"""

from __future__ import annotations

import argparse
import queue
import re
import time
from typing import Iterable

import onewili


DLC_TO_LEN = {
    0: 0,
    1: 1,
    2: 2,
    3: 3,
    4: 4,
    5: 5,
    6: 6,
    7: 7,
    8: 8,
    9: 12,
    10: 16,
    11: 20,
    12: 24,
    13: 32,
    14: 48,
    15: 64,
}

DEMO_MESSAGES = (
    (0x123, False, False, bytes.fromhex("DE AD BE EF")),
    (0x124, False, False, bytes.fromhex("01 02 03 04 05 06 07 08")),
    (0x200, True, False, bytes.fromhex("00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF")),
)


def parse_hex_bytes(tokens: Iterable[str]) -> bytes:
    parts: list[str] = []
    for token in tokens:
        parts.extend(piece for piece in re.split(r"[\s,:-]+", token.strip()) if piece)

    if len(parts) == 1 and len(parts[0]) > 2 and len(parts[0]) % 2 == 0:
        value = parts[0].removeprefix("0x").removeprefix("0X")
        parts = [value[index : index + 2] for index in range(0, len(value), 2)]

    out = bytearray()
    for part in parts:
        value = int(part, 16)
        if value < 0 or value > 0xFF:
            raise argparse.ArgumentTypeError(f"byte out of range: {part}")
        out.append(value)
    return bytes(out)


def require_ok(result, action: str) -> None:
    is_err = getattr(result, "is_err", None)
    if callable(is_err) and is_err():
        err_value = getattr(result, "err_value", result)
        raise RuntimeError(f"{action} failed: {err_value!r}")


def can_payload_from_words(data_words: tuple[int, ...], length: int) -> bytes:
    raw = bytearray()
    for word in data_words:
        raw.extend(int(word).to_bytes(4, "little", signed=False))
    return bytes(raw[:length])


def format_can_rx_report(event) -> str:
    header_bits = int(event.r1_filter_header_bits)
    dlc = header_bits & 0x0F
    length = DLC_TO_LEN.get(dlc, 0)
    data = can_payload_from_words(event.data_words, length)
    data_hex = " ".join(f"{byte:02X}" for byte in data)
    flags = []
    if header_bits & (1 << 4):
        flags.append("extended")
    if header_bits & (1 << 5):
        flags.append("remote")
    if header_bits & (1 << 6):
        flags.append("brs")
    if header_bits & (1 << 7):
        flags.append("fd")
    if getattr(event, "error", False):
        flags.append("error")

    flag_text = ",".join(flags) if flags else "classic"
    return (
        f"rx t={event.time_stamp_ns}ns raw_id=0x{event.r0_canid:08X} "
        f"header=0x{header_bits:08X} dlc={dlc} len={length} "
        f"flags={flag_text} data={data_hex}"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Send a CAN/CAN-FD frame with FreeWili 2 and print received frames."
    )
    parser.add_argument("--list-ports", action="store_true", help="print available serial ports and exit")
    parser.add_argument("--serial", help="FreeWili USB serial number when more than one board is connected")
    parser.add_argument("--text-port", help="main OneWili command port, for example COM5")
    parser.add_argument("--binary-port", help="FTDI binary event port, for example COM9")
    parser.add_argument("--channel", type=int, default=0, help="CAN channel, FreeWili 2 uses 0")
    parser.add_argument("--id", type=lambda value: int(value, 0), default=0x123, help="CAN arbitration ID")
    parser.add_argument("--fd", action="store_true", help="send as CAN-FD instead of classic CAN")
    parser.add_argument("--extended", action="store_true", help="use a 29-bit extended ID")
    parser.add_argument("--data", nargs="*", default=["DE", "AD", "BE", "EF"], help="payload bytes, for example: DE AD BE EF")
    parser.add_argument("--seconds", type=float, default=10.0, help="how long to listen after enabling RX streaming")
    parser.add_argument("--forever", action="store_true", help="keep receiving until Ctrl+C")
    parser.add_argument("--send-interval", type=float, default=0.0, help="repeat transmit interval in seconds, 0 sends once")
    parser.add_argument("--fast-send", action="store_true", help="send from a device periodic slot as fast as the bus permits")
    parser.add_argument("--fast-slot", type=int, default=0, help="periodic transmit slot used by --fast-send")
    parser.add_argument("--demo", action="store_true", help="cycle through a few sample classic CAN and CAN-FD messages")
    parser.add_argument("--demo-interval", type=float, default=1.0, help="seconds between demo messages")
    parser.add_argument("--no-send", action="store_true", help="only listen for CAN frames")
    return parser.parse_args()


def print_serial_ports() -> None:
    from serial.tools import list_ports

    for port in list_ports.comports():
        print(f"{port.device}: {port.description} hwid={port.hwid}")


def open_device(args: argparse.Namespace):
    if args.text_port or args.binary_port:
        if not args.text_port or not args.binary_port:
            raise SystemExit("pass both --text-port and --binary-port, or neither")
        return onewili.OneWili(args.text_port, binary_port=args.binary_port).open().open_binary()

    try:
        return onewili.connect(serial=args.serial, binary=True)
    except RuntimeError as exc:
        raise SystemExit(
            f"{exc}\n\n"
            "Auto-discovery found the board but could not map it to COM ports. "
            "Run with --list-ports, then pass both ports manually, for example:\n"
            "  python .\\scripts\\freewili_can.py --text-port COM5 --binary-port COM9 "
            "--id 0x123 --data DE AD BE EF"
        ) from exc


def send_frame(dev, channel: int, arb_id: int, can_fd: bool, extended: bool, payload: bytes) -> None:
    require_ok(
        dev.io.canfd.write_canfd(channel, arb_id, 1 if can_fd else 0, 1 if extended else 0, payload),
        "send CAN frame",
    )
    print(
        f"tx id=0x{arb_id:X} fd={int(can_fd)} extended={int(extended)} "
        f"data={' '.join(f'{byte:02X}' for byte in payload)}"
    )


def start_fast_send(
    dev,
    slot: int,
    channel: int,
    arb_id: int,
    can_fd: bool,
    extended: bool,
    payload: bytes,
) -> None:
    require_ok(
        dev.io.canfd.write_canfd_periodic(
            slot,
            1,
            0,
            channel,
            arb_id,
            1 if can_fd else 0,
            1 if extended else 0,
            payload,
        ),
        "start fast CAN transmit",
    )
    print(
        f"fast tx slot={slot} id=0x{arb_id:X} fd={int(can_fd)} extended={int(extended)} "
        f"data={' '.join(f'{byte:02X}' for byte in payload)}"
    )


def stop_fast_send(dev, slot: int, channel: int) -> None:
    require_ok(
        dev.io.canfd.write_canfd_periodic(slot, 0, 0, channel, 0, 0, 0, b""),
        "stop fast CAN transmit",
    )


def main() -> int:
    args = parse_args()
    if args.list_ports:
        print_serial_ports()
        return 0

    payload = parse_hex_bytes(args.data)
    can_fd = 1 if args.fd else 0
    extended = 1 if args.extended else 0

    if not args.fd and len(payload) > 8:
        raise SystemExit("classic CAN payloads must be 0 to 8 bytes; use --fd for longer frames")

    dev = open_device(args)
    sent_once = False
    next_send = time.monotonic()
    demo_index = 0
    received = 0
    fast_send_started = False

    try:
        require_ok(dev.io.canfd.enable_canfd_stream(args.channel, 1), "enable CAN stream")
        print(f"streaming CAN channel {args.channel}")

        if args.fast_send and not args.no_send:
            if args.demo:
                raise SystemExit("--fast-send sends one frame; remove --demo and pass --id/--data")
            start_fast_send(dev, args.fast_slot, args.channel, args.id, bool(can_fd), bool(extended), payload)
            fast_send_started = True

        deadline = None if args.forever else time.monotonic() + args.seconds
        while deadline is None or time.monotonic() < deadline:
            now = time.monotonic()
            if args.fast_send:
                pass
            elif args.demo and not args.no_send and now >= next_send:
                demo_id, demo_fd, demo_extended, demo_payload = DEMO_MESSAGES[demo_index]
                send_frame(dev, args.channel, demo_id, demo_fd, demo_extended, demo_payload)
                demo_index = (demo_index + 1) % len(DEMO_MESSAGES)
                sent_once = True
                next_send = now + args.demo_interval
            elif not args.no_send and not args.demo:
                should_send = not sent_once or (args.send_interval > 0 and now >= next_send)
                if should_send:
                    send_frame(dev, args.channel, args.id, bool(can_fd), bool(extended), payload)
                    sent_once = True
                    next_send = now + args.send_interval

            try:
                event = dev.binary_events.get(timeout=0.1)
            except queue.Empty:
                continue

            if event.__class__.__name__ == "CanRxReportEvent":
                received += 1
                print(format_can_rx_report(event))

        print(f"received {received} CAN binary events")
        return 0
    except KeyboardInterrupt:
        print(f"stopped; received {received} CAN binary events")
        return 130
    finally:
        try:
            if fast_send_started:
                stop_fast_send(dev, args.fast_slot, args.channel)
            require_ok(dev.io.canfd.enable_canfd_stream(args.channel, 0), "disable CAN stream")
        finally:
            dev.close()


if __name__ == "__main__":
    raise SystemExit(main())
