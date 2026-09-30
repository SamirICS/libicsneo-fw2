#!/usr/bin/env python3
"""Bring CAN up step by step and dump everything both FW2 ports emit.

Exists to split one question that the libicsneo path cannot split on its own:
when nothing is received, is the controller silent, or is the firmware
reporting on a channel we do not listen to? OneWili can report CAN reception
two different ways -- binary canRxReport frames on the FTDI port, and can0 /
canTx0 *text* events on the ACM port -- and FW2Communication only decodes the
binary one.

So this listens to both at once and prints whatever arrives, tagged.

Usage: fw2_listen.py [seconds] [--tx] [--filter] [--no-setup]
"""

import argparse
import sys
import time

import serial

TEXT_PORT = '/dev/ttyACM1'
BIN_PORT = '/dev/ttyUSB0'
WILI = b'WILI'


def send(ser, command, settle=0.6):
    ser.write(b'\x02' + command.encode() + b'\n')
    ser.flush()
    buf = b''
    deadline = time.time() + settle
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
        else:
            time.sleep(0.01)
    text = buf.decode(errors='replace').strip()
    print(f'>>> {command}')
    for line in text.splitlines():
        if line.strip():
            print(f'    {line.strip()}')
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('seconds', nargs='?', type=float, default=10.0)
    ap.add_argument('--tx', action='store_true', help='transmit 0x123 every 500 ms while listening')
    ap.add_argument('--filter', action='store_true', help='install a catch-all RX filter first')
    ap.add_argument('--no-setup', action='store_true', help='skip power/rate/stream setup')
    args = ap.parse_args()

    text = serial.Serial(TEXT_PORT, 1000000, timeout=0)
    time.sleep(0.2)

    if not args.no_setup:
        # Zone 15 (5V_S15) feeds the MCP2518FD and the TCAN1472 and is off on a
        # fresh boot. The board serialises zone changes, hence the long settle.
        send(text, 'h\\p\\s 15 1', settle=2.0)
        send(text, 'h\\s\\p\\b 500000')   # CAN1 rate, raw bps
        send(text, 'h\\s\\p\\1 1')        # CAN1 termination
        if args.filter:
            # mask 0 = every ID matches. The MCP2518FD stores a frame only when
            # an enabled filter hits, and nothing in this port ever sets one.
            send(text, 'i\\c\\f 0 0 1 0 00000000 00000000')
        send(text, 'i\\c\\o 0 1')         # start the RX stream

    binary = serial.Serial(BIN_PORT, 1000000, timeout=0)

    print(f'\n--- listening {args.seconds}s '
          f'(text={TEXT_PORT}, binary={BIN_PORT}){", transmitting" if args.tx else ""} ---')

    text_buf = b''
    bin_buf = b''
    wili_frames = 0
    text_events = 0
    next_tx = time.time()
    end = time.time() + args.seconds

    while time.time() < end:
        now = time.time()
        if args.tx and now >= next_tx:
            next_tx = now + 0.5
            text.write(b'\x02i\\c\\w 0 00000123 0 0 DEADBEEF\n')
            text.flush()

        chunk = text.read(4096)
        if chunk:
            text_buf += chunk
            while b'\n' in text_buf:
                line, text_buf = text_buf.split(b'\n', 1)
                line = line.decode(errors='replace').strip()
                if line:
                    text_events += 1
                    print(f'  [text] {line}')

        chunk = binary.read(4096)
        if chunk:
            bin_buf += chunk
            # Only count frames, and only show the header plus a little payload:
            # a live stream would otherwise scroll the interesting lines away.
            while True:
                at = bin_buf.find(WILI)
                if at < 0 or len(bin_buf) - at < 12:
                    break
                hdr = bin_buf[at:at + 12]
                htype = int.from_bytes(hdr[6:8], 'little')
                length = int.from_bytes(hdr[8:12], 'little') & 0x7FFFFFFF
                err = bool(int.from_bytes(hdr[8:12], 'little') & 0x80000000)
                if len(bin_buf) - at - 12 < length:
                    break
                payload = bin_buf[at + 12:at + 12 + length]
                bin_buf = bin_buf[at + 12 + length:]
                wili_frames += 1
                kind = {0: 'gpioReport', 1: 'canRxReport', 2: 'logicAnalyzer'}.get(htype, f'type{htype}')
                print(f'  [bin ] {kind} len={length} err={err} {payload[:24].hex()}')

        time.sleep(0.005)

    print(f'\n--- done: {text_events} text line(s), {wili_frames} WILI frame(s) ---')
    text.close()
    binary.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
