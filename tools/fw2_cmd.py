#!/usr/bin/env python3
"""Send OneWili text commands to the FW2 and print the replies.

The wire format is the one FW2Communication::frameCommand() produces:
0x02, the command bytes, then '\\n'. Responses are '[...]' lines; lines that do
not start with '[' are free-text firmware chatter, and '[*...' is a spontaneous
event rather than a reply to us.

Usage:
  fw2_cmd.py 'h\\p\\g'                 one command
  fw2_cmd.py 'h\\p\\s 6 1' 'i\\g\\u'   several, in order
  fw2_cmd.py --port /dev/ttyACM0 ...
"""

import argparse
import sys
import time

import serial


def send(ser, command, settle=0.4):
	ser.reset_input_buffer()
	ser.write(b'\x02' + command.encode() + b'\n')
	ser.flush()

	buf = b''
	deadline = time.time() + settle
	while time.time() < deadline:
		chunk = ser.read(4096)
		if chunk:
			buf += chunk
			# Keep reading a little after the first reply -- some commands emit
			# chatter before the bracketed line.
			deadline = max(deadline, time.time() + 0.15)
		else:
			time.sleep(0.01)
	return buf.decode(errors='replace')


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument('commands', nargs='+')
	ap.add_argument('--port', default='/dev/ttyACM0')
	ap.add_argument('--baud', type=int, default=1000000)
	ap.add_argument('--settle', type=float, default=0.4)
	args = ap.parse_args()

	with serial.Serial(args.port, args.baud, timeout=0) as ser:
		time.sleep(0.2)
		for command in args.commands:
			print(f'>>> {command}')
			text = send(ser, command, args.settle)
			for line in text.splitlines():
				if line.strip():
					print(f'    {line}')
			print()
	return 0


if __name__ == '__main__':
	sys.exit(main())
