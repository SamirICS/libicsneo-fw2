#include "icsneo/platform/onewili.h"
#include "icsneo/device/founddevice.h"
#include <dirent.h>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

using namespace icsneo;

namespace {

/** Read the first line of a sysfs attribute. Returns empty on any failure. */
std::string readSysfsLine(const std::string& path) {
	std::ifstream reader(path);
	std::string line;
	if(!std::getline(reader, line))
		return {};
	return line;
}

/** List a directory, skipping "." and "..". Empty if the directory is absent. */
std::vector<std::string> listDirectory(const std::string& path) {
	std::vector<std::string> results;
	DIR* dir = opendir(path.c_str());
	if(!dir)
		return results;
	struct dirent* entry;
	while((entry = readdir(dir)) != nullptr) {
		const std::string name = entry->d_name;
		if(name != "." && name != "..")
			results.push_back(name);
	}
	closedir(dir);
	return results;
}

/**
 * One USB serial port found in sysfs.
 *
 * `usbPath` is the USB topology id with the interface suffix stripped: sysfs
 * names a cdc_acm binding "3-2.1:1.0", of which "3-2.1" identifies the USB
 * device itself and "3-2" its parent hub.
 */
struct SysfsPort {
	std::string usbPath;  // e.g. "3-2.1"
	std::string ttyName;  // e.g. "ttyACM0"
	unsigned ttyIndex = 0;
	uint16_t vid = 0;
	uint16_t pid = 0;
	std::string product;
	std::string serial;
};

/** "3-2.1:1.0" -> "3-2.1". Returns the input unchanged if there is no colon. */
std::string stripInterface(const std::string& binding) {
	const auto colon = binding.find(':');
	return colon == std::string::npos ? binding : binding.substr(0, colon);
}

/**
 * "3-2.1" -> "3-2": the hub the device hangs off. FW2's text port and FTDI port
 * are siblings on the board's internal hub, so a shared parent is what pairs
 * them. Returns empty for a root-port device ("3-2"), which has no ".N" to drop
 * and therefore cannot be paired topologically.
 */
std::string parentHub(const std::string& usbPath) {
	const auto dot = usbPath.rfind('.');
	return dot == std::string::npos ? std::string{} : usbPath.substr(0, dot);
}

/** Trailing digits of "ttyACM12" -> 12. False if there are none. */
bool ttyIndexOf(const std::string& ttyName, unsigned& out) {
	size_t i = 0;
	while(i < ttyName.size() && !isdigit(static_cast<unsigned char>(ttyName[i])))
		i++;
	if(i == ttyName.size())
		return false;
	try {
		out = static_cast<unsigned>(std::stoul(ttyName.substr(i)));
	} catch(...) {
		return false;
	}
	return out <= 0xFF; // Must survive PackHandle()
}

/**
 * Every TTY bound to a given USB serial driver, with its VID/PID/product/serial.
 *
 * The VID/PID come from the TTY's uevent PRODUCT= line ("PRODUCT=93c/1101/100");
 * the product and serial strings come from the USB device node, since the
 * interface node does not carry them.
 */
std::vector<SysfsPort> enumerateDriver(const std::string& driverName) {
	std::vector<SysfsPort> ports;
	const std::string driverDir = "/sys/bus/usb/drivers/" + driverName;

	for(const auto& binding : listDirectory(driverDir)) {
		// Bindings are named "<usbpath>:<config>.<interface>". Other entries in
		// here (bind, unbind, uevent, module, ...) do not start with a digit.
		if(binding.empty() || !isdigit(static_cast<unsigned char>(binding[0])))
			continue;

		const auto ttys = listDirectory(driverDir + "/" + binding + "/tty");
		if(ttys.size() != 1) // No TTY, or ambiguous -- either way not usable
			continue;

		SysfsPort port;
		port.ttyName = ttys[0];
		if(!ttyIndexOf(port.ttyName, port.ttyIndex))
			continue;
		port.usbPath = stripInterface(binding);

		// uevent is multi-line; find the PRODUCT= entry rather than assuming order.
		std::ifstream uevent("/sys/class/tty/" + port.ttyName + "/device/uevent");
		std::string line, productField;
		while(std::getline(uevent, line)) {
			if(line.rfind("PRODUCT=", 0) == 0) {
				productField = line.substr(strlen("PRODUCT="));
				break;
			}
		}
		if(productField.empty())
			continue;

		const auto slash = productField.find('/');
		if(slash == std::string::npos)
			continue;
		try {
			port.vid = static_cast<uint16_t>(std::stoul(productField.substr(0, slash), nullptr, 16));
			const auto pidStart = slash + 1;
			port.pid = static_cast<uint16_t>(
				std::stoul(productField.substr(pidStart, productField.find('/', pidStart) - pidStart),
					nullptr, 16));
		} catch(...) {
			continue;
		}

		const std::string deviceDir = "/sys/bus/usb/devices/" + port.usbPath;
		port.product = readSysfsLine(deviceDir + "/product");
		port.serial = readSysfsLine(deviceDir + "/serial");

		ports.push_back(std::move(port));
	}

	return ports;
}

/**
 * Derive libicsneo's 6-character identity from FW2's arbitrary-length USB
 * serial: "FW" plus four base36 digits of an FNV-1a hash.
 *
 * A hash rather than a truncation because FW2 serials are not guaranteed to
 * differ in their first four characters, and a truncation that collides would
 * make two boards indistinguishable to DeviceFinder (which dedupes on serial).
 * Four base36 digits give ~1.7M values, so a collision across the handful of
 * boards on one host is negligible -- and Find() detects it anyway. The full
 * serial stays available through FreeWili2::getProductName().
 */
std::string deriveIcsneoSerial(const std::string& fw2Serial) {
	uint32_t hash = 2166136261u;
	for(const char c : fw2Serial) {
		hash ^= static_cast<uint8_t>(c);
		hash *= 16777619u;
	}
	static constexpr char DIGITS[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	std::string out = "FW";
	for(int i = 0; i < 4; i++) {
		out.push_back(DIGITS[hash % 36]);
		hash /= 36;
	}
	return out;
}

} // namespace

bool OneWili::IsFreeWiliProduct(const std::string& product) {
	// Upstream's own Rust finder matches the text port's product string, which is
	// "FW2 v01" on current firmware. Keep it to the "FW" stem so a version bump
	// does not de-enumerate the board.
	return product.rfind("FW", 0) == 0 || product.rfind("FreeWili", 0) == 0;
}

void OneWili::Find(std::vector<FoundDevice>& found) {
	// FW2's command port. VID alone is not enough -- 0x093C is Intrepid's VID and
	// genuine ICS devices share it -- so the product string decides.
	std::vector<SysfsPort> textPorts;
	for(auto& port : enumerateDriver("cdc_acm")) {
		if(port.vid != FREEWILI_USB_VENDOR_ID || !IsFreeWiliProduct(port.product))
			continue;
		if(port.serial.empty())
			continue; // Without a serial there is no stable identity
		textPorts.push_back(std::move(port));
	}
	if(textPorts.empty())
		return;

	// FW2's event port. Matched by FTDI VID only: the FTDI descriptor does NOT
	// carry an "FW2" product string, and matching one would false-positive on the
	// text port. Upstream's binary_transport documents the same rule.
	std::vector<SysfsPort> binaryPorts;
	for(auto& port : enumerateDriver("ftdi_sio")) {
		if(port.vid != FTDI_USB_VENDOR_ID)
			continue;
		binaryPorts.push_back(std::move(port));
	}

	std::set<unsigned> claimedBinary;
	std::set<std::string> usedSerials;

	for(const auto& text : textPorts) {
		// Pair by topology: the two ports are siblings on the board's internal hub.
		const std::string hub = parentHub(text.usbPath);
		const SysfsPort* binary = nullptr;
		if(!hub.empty()) {
			for(const auto& candidate : binaryPorts) {
				if(claimedBinary.count(candidate.ttyIndex))
					continue;
				if(parentHub(candidate.usbPath) == hub) {
					binary = &candidate;
					break;
				}
			}
		}
		// Fallback for boards wired without an internal hub, or a topology we did
		// not anticipate: with exactly one of each there is no ambiguity to
		// resolve. This mirrors what upstream's find_binary_port() does. With more
		// than one board attached and no topology match we deliberately leave the
		// binary port unpaired rather than guess -- guessing would silently cross
		// the wires between two boards.
		if(!binary && textPorts.size() == 1 && binaryPorts.size() == 1 &&
				!claimedBinary.count(binaryPorts[0].ttyIndex)) {
			binary = &binaryPorts[0];
		}
		if(binary)
			claimedBinary.insert(binary->ttyIndex);

		std::string serial = deriveIcsneoSerial(text.serial);
		if(!usedSerials.insert(serial).second)
			continue; // Hash collision between two attached boards; skip the second

		FoundDevice foundDevice;
		foundDevice.handle = PackHandle(static_cast<uint8_t>(text.ttyIndex),
			binary ? static_cast<uint8_t>(binary->ttyIndex) : 0, binary != nullptr);
		foundDevice.productId = text.pid;
		foundDevice.serial[serial.copy(foundDevice.serial, sizeof(foundDevice.serial) - 1)] = '\0';
		foundDevice.makeDriver = [](const device_eventhandler_t& report, neodevice_t& device) {
			return std::unique_ptr<Driver>(new OneWili(report, device));
		};

		found.push_back(std::move(foundDevice));
	}
}

std::string OneWili::HandleToTextPort(neodevice_handle_t handle) {
	if((handle & HANDLE_OFFSET) == 0)
		return {};
	return "/dev/ttyACM" + std::to_string(handle & 0xFF);
}

std::string OneWili::HandleToBinaryPort(neodevice_handle_t handle) {
	if((handle & HANDLE_OFFSET) == 0 || (handle & BINARY_PRESENT_BIT) == 0)
		return {};
	return "/dev/ttyUSB" + std::to_string((handle >> 8) & 0xFF);
}
