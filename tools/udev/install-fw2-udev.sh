#!/bin/sh
# Install FW2 udev rules and repair two Intrepid rules that over-match.
#
# Run with: sudo tools/udev/install-fw2-udev.sh
#
# Idempotent -- safe to run more than once. Backs up anything it edits to
# <file>.bak-fw2-<timestamp> the first time it touches it.
#
# ---------------------------------------------------------------------------
# Why the Intrepid rules need repairing
#
# The FW2's FTDI binary port (0403:6014) sits behind an internal hub whose
# vendor ID is 093c -- Intrepid's. udev's ATTRS{} matches walk *up the parent
# chain*, so two rules written for genuine ICS hardware match the FW2's FTDI
# port through that hub and unbind ftdi_sio from it. No ftdi_sio means no
# /dev/ttyUSB*, which means libicsneo's OneWili driver -- which enumerates via
# enumerateDriver("ftdi_sio") -- never finds the binary port, so CAN RX is dead.
#
# Confirmed with:
#   udevadm test /sys/bus/usb/devices/3-1.3
#   -> run: 'sh -c 'echo -n 3-1.3:1.0 > /sys/bus/usb/drivers/ftdi_sio/unbind''
#
# The fix narrows each rule to the device's *own* vendor rather than any
# ancestor's. Genuine ICS FTDI devices are reprogrammed to VID 093c, so they
# still match and are still unbound -- the rules' original intent is preserved.
# ---------------------------------------------------------------------------

set -eu

RULES_DIR=/etc/udev/rules.d
SRC_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
STAMP=$(date +%Y%m%d-%H%M%S)

if [ "$(id -u)" -ne 0 ]; then
	echo "error: must run as root (use sudo)" >&2
	exit 1
fi

backup_once() {
	# Only back up if we have not already made an fw2 backup of this file.
	if ! ls "$1".bak-fw2-* >/dev/null 2>&1; then
		cp -a "$1" "$1.bak-fw2-$STAMP"
		echo "  backed up $1 -> $1.bak-fw2-$STAMP"
	fi
}

echo "==> Repairing over-matching Intrepid rules"

# 1) 99-intrepid.rules: ATTRS{idVendor} (any ancestor) -> ATTR{idVendor} (this
#    device) on the ftdi_sio unbind rule only. The permission rule above it is
#    left alone.
F="$RULES_DIR/99-intrepid.rules"
if [ ! -f "$F" ]; then
	echo "  $F not present, skipping"
elif grep -q 'ATTRS{idVendor}=="093c", ATTR{bDeviceClass}=="00"' "$F"; then
	backup_once "$F"
	sed -i 's/ATTRS{idVendor}=="093c", ATTR{bDeviceClass}=="00"/ATTR{idVendor}=="093c", ATTR{bDeviceClass}=="00"/' "$F"
	echo "  patched $F (ftdi_sio unbind now matches own VID only)"
else
	echo "  $F already patched"
fi

# 2) 99-intrepidcs.rules: exclude anything with an FTDI ancestor from the
#    ttyUSB ftdi_sio unbind. A real ICS device has no 0403 in its parent chain.
F="$RULES_DIR/99-intrepidcs.rules"
if [ ! -f "$F" ]; then
	echo "  $F not present, skipping"
elif grep -q 'ATTRS{idVendor}!="0403"' "$F"; then
	echo "  $F already patched"
elif grep -q 'ATTRS{idVendor}=="093c", KERNEL=="ttyUSB\*"' "$F"; then
	backup_once "$F"
	sed -i 's/ATTRS{idVendor}=="093c", KERNEL=="ttyUSB\*"/ATTRS{idVendor}=="093c", ATTRS{idVendor}!="0403", KERNEL=="ttyUSB*"/' "$F"
	echo "  patched $F (ttyUSB unbind now skips FTDI-backed ports)"
else
	echo "  $F not in expected form, leaving alone"
fi

echo "==> Installing $RULES_DIR/99-zz-freewili2.rules"
install -m 0644 "$SRC_DIR/99-zz-freewili2.rules" "$RULES_DIR/99-zz-freewili2.rules"

echo "==> Reloading udev"
udevadm control --reload-rules
udevadm trigger --subsystem-match=tty --subsystem-match=usb
udevadm settle

echo "==> Binding ftdi_sio to any unbound FW2 binary port"
modprobe ftdi_sio 2>/dev/null || true
found=0
for dev in /sys/bus/usb/devices/*; do
	[ -f "$dev/idVendor" ] || continue
	[ "$(cat "$dev/idVendor")" = "0403" ] || continue
	[ "$(cat "$dev/idProduct")" = "6014" ] || continue
	found=1
	for intf in "$dev":*; do
		[ -d "$intf" ] || continue
		name=$(basename "$intf")
		if [ -e "$intf/driver" ]; then
			echo "  $name already bound to $(basename "$(readlink -f "$intf/driver")")"
		elif printf '%s' "$name" > /sys/bus/usb/drivers/ftdi_sio/bind 2>/dev/null; then
			echo "  bound $name to ftdi_sio"
		else
			echo "  could not bind $name -- unplug and replug the FW2"
		fi
	done
done
[ "$found" -eq 1 ] || echo "  no 0403:6014 device present -- is the FW2 plugged in?"

echo
echo "==> Result"
ls -l /dev/ttyACM* /dev/ttyUSB* 2>&1 || true
echo
echo "If /dev/ttyUSB* is still missing, unplug and replug the FW2 -- the rules"
echo "are correct from the next enumeration onward."
