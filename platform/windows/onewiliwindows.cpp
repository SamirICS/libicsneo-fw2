#include "icsneo/platform/onewili.h"
#include "icsneo/device/founddevice.h"

using namespace icsneo;

/*
 * Windows support for FreeWili 2 is NOT IMPLEMENTED YET.
 *
 * Everything above this file -- FW2Communication, FreeWili2, FW2Settings -- is
 * platform independent, so bringing Windows up means implementing only this
 * file. Two pieces:
 *
 *   1. Find(). SetupAPI enumeration of both ports. The text port is a
 *      USB\VID_093C&PID_* CDC device whose product string passes
 *      IsFreeWiliProduct(); the binary port is a VID_0403 FTDI COM port. Pair
 *      them by shared USB parent (SPDRP_ENUMERATOR_NAME / the device instance
 *      path's parent), the same sibling-on-the-internal-hub rule the Linux
 *      finder uses.
 *
 *      The one place Windows differs materially: COM port numbers are not the
 *      small dense integers PackHandle() assumes. A COM number can exceed 255,
 *      so the handle packing needs widening (or a Find()-populated lookup table)
 *      before this is correct.
 *
 *   2. Port I/O. CreateFile + DCB + overlapped reads, mirroring
 *      platform/posix/onewili.cpp's three tasks. Note the FW2 text port needs
 *      DTR asserted or it ignores writes entirely -- upstream's Rust transport
 *      hit exactly this (os error 121) and calls write_data_terminal_ready(true)
 *      on open.
 *
 * Until then, LIBICSNEO_ENABLE_ONEWILI builds cleanly on Windows and simply
 * finds no FW2 devices. It does not break the ICS device tree.
 */

void OneWili::Find(std::vector<FoundDevice>&) {}

bool OneWili::IsFreeWiliProduct(const std::string& product) {
	return product.rfind("FW", 0) == 0 || product.rfind("FreeWili", 0) == 0;
}

OneWili::~OneWili() {
	if(isOpen())
		close();
}

bool OneWili::open() {
	report(APIEvent::Type::NotSupported, APIEvent::Severity::Error);
	return false;
}

bool OneWili::isOpen() {
	return textHandle != INVALID_HANDLE_VALUE;
}

bool OneWili::close() {
	report(APIEvent::Type::NotSupported, APIEvent::Severity::Error);
	return false;
}

bool OneWili::popTextResponse(std::string&) {
	return false;
}

void OneWili::binaryReadTask() {}
void OneWili::textReadTask() {}
void OneWili::writeTask() {}

std::string OneWili::HandleToTextPort(neodevice_handle_t) { return {}; }
std::string OneWili::HandleToBinaryPort(neodevice_handle_t) { return {}; }
