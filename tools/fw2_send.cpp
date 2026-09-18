// fw2_send -- transmit CAN/CAN-FD frames from a FreeWili 2 through the normal
// libicsneo Device::transmit() path (no raw OneWili commands). Windows and
// POSIX both work: unlike fw2_tx.cpp, this has no termios dependency.
//
// Usage:
//   fw2_send [arbid_hex] [count] [period_ms] [byte...]
//
//   arbid_hex   default 0x123
//   count       default 10; 0 = run forever (Ctrl-C to stop)
//   period_ms   default 100; time between frames
//   byte...     up to 64 hex bytes, space separated; default DE AD BE EF
//
// Examples:
//   fw2_send                          11 3 20 default payload every 100 ms
//   fw2_send 1ABCDEF 0 250             extended-looking id, forever, 250 ms
//   fw2_send 200 5 10 00 11 22 33 44   5 frames of a 5-byte payload

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "icsneo/icsneocpp.h"
#include "icsneo/communication/message/canmessage.h"

namespace {

std::atomic<bool> gStop{false};
void onSigint(int) { gStop = true; }

void printUsageAndExit(const char* argv0) {
	std::cout << "Usage: " << argv0 << " [arbid_hex] [count] [period_ms] [byte...]\n";
	std::exit(2);
}

uint32_t parseHex(const std::string& text) {
	return static_cast<uint32_t>(std::stoul(text, nullptr, 16));
}

} // namespace

int main(int argc, char** argv) {
	std::signal(SIGINT, onSigint);

	uint32_t arbid = 0x123;
	int count = 10;
	int periodMs = 100;
	std::vector<uint8_t> payload = { 0xDE, 0xAD, 0xBE, 0xEF };

	int argi = 1;
	try {
		if(argi < argc) arbid = parseHex(argv[argi++]);
		if(argi < argc) count = std::atoi(argv[argi++]);
		if(argi < argc) periodMs = std::atoi(argv[argi++]);
		if(argi < argc) {
			payload.clear();
			for(; argi < argc; ++argi)
				payload.push_back(static_cast<uint8_t>(parseHex(argv[argi])));
		}
	} catch(...) {
		printUsageAndExit(argv[0]);
	}

	const bool extended = arbid > 0x7FF;
	const bool canFd = payload.size() > 8;

	std::cout << "libicsneo " << icsneo::GetVersion() << " -- FW2 send\n";
	std::cout << "arbid=0x" << std::hex << arbid << std::dec
		<< " extended=" << extended << " fd=" << canFd
		<< " bytes=" << payload.size()
		<< " count=" << (count == 0 ? "infinite" : std::to_string(count))
		<< " period=" << periodMs << "ms\n" << std::endl;

	auto devices = icsneo::FindAllDevices();
	std::shared_ptr<icsneo::Device> fw2;
	for(const auto& d : devices) {
		if(d->getType().getDeviceType() == icsneo::DeviceType::FreeWili2)
			fw2 = d;
	}
	if(!fw2) {
		std::cout << "no FreeWili 2 found among " << devices.size() << " device(s)" << std::endl;
		for(const auto& d : devices)
			std::cout << "  " << d->describe() << std::endl;
		return 1;
	}
	std::cout << "using " << fw2->describe() << std::endl;

	if(!fw2->open()) {
		std::cout << "open() failed: " << icsneo::GetLastError().describe() << std::endl;
		return 1;
	}
	if(!fw2->goOnline()) {
		std::cout << "goOnline() failed: " << icsneo::GetLastError().describe() << std::endl;
		fw2->close();
		return 1;
	}

	uint64_t sent = 0, failed = 0;
	auto nextTx = std::chrono::steady_clock::now();
	while(!gStop && (count == 0 || static_cast<int>(sent + failed) < count)) {
		auto msg = std::make_shared<icsneo::CANMessage>();
		msg->network = icsneo::Network::NetID::DWCAN_01;
		msg->arbid = arbid;
		msg->isExtended = extended;
		msg->isCANFD = canFd;
		msg->data = payload;

		if(fw2->transmit(msg)) {
			++sent;
			std::cout << "tx #" << sent << " id=0x" << std::hex << arbid << std::dec << " data=";
			for(auto b : payload)
				std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b) << ' ';
			std::cout << std::dec << std::endl;
		} else {
			++failed;
			std::cout << "tx failed: " << icsneo::GetLastError().describe() << std::endl;
		}

		nextTx += std::chrono::milliseconds(periodMs);
		std::this_thread::sleep_until(nextTx);
	}

	std::cout << "\n" << sent << " sent, " << failed << " failed" << std::endl;

	fw2->goOffline();
	fw2->close();
	return failed > 0 && sent == 0 ? 1 : 0;
}
