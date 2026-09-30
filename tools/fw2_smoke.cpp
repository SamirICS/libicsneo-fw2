// fw2_smoke -- Phase 1 exit-criterion harness for the FreeWili 2 port.
//
// The criterion from FW2_PORT.md section 5:
//   "icsneo::FindAllDevices() lists an FW2 alongside a FIRE3, and open() /
//    close() succeed with the heartbeat holding for 5 minutes."
//
// Each stage prints PASS/FAIL on its own line and the harness keeps going where
// it usefully can, so one run tells you how far the port actually gets rather
// than only where it first stops.
//
// Usage: fw2_smoke [hold_seconds] [tx_period_ms]
//   hold_seconds   default 30; use 300 for the real gate
//   tx_period_ms   0 (default) stays receive-only; non-zero transmits a frame
//                  on that period through Device::transmit(), which is the
//                  whole libicsneo TX path -- not a raw OneWili command. Use it
//                  to prove the wire from a bus partner's point of view.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>

#include "icsneo/icsneocpp.h"
#include "icsneo/communication/message/canmessage.h"
#include "icsneo/communication/message/canerrormessage.h"

namespace {

std::atomic<bool> gStop{false};
void onSigint(int) { gStop = true; }

int gPass = 0, gFail = 0;

// Arbitrary, but fixed so the bus partner has one ID to filter on.
constexpr uint32_t TX_ARBID = 0x123;

const char* errorCodeName(icsneo::CANErrorCode code) {
	switch(code) {
		case icsneo::CANErrorCode::NoError: return "none";
		case icsneo::CANErrorCode::StuffError: return "stuff";
		case icsneo::CANErrorCode::FormError: return "form";
		case icsneo::CANErrorCode::AckError: return "ACK";
		case icsneo::CANErrorCode::Bit1Error: return "bit1";
		case icsneo::CANErrorCode::Bit0Error: return "bit0";
		case icsneo::CANErrorCode::CRCError: return "CRC";
		case icsneo::CANErrorCode::NoChange: return "nochange";
	}
	return "?";
}

/**
 * Bus state accumulated over the run.
 *
 * `everBusOff` and `everErrorPassive` are sticky on purpose: a controller that
 * recovers still had a fault, and a summary that only showed the final state
 * would report a clean run for a bus that dropped out halfway through.
 */
struct BusSummary {
	std::mutex lock;
	bool seen = false;
	uint8_t tec = 0, rec = 0;
	bool busOff = false, errorPassive = false, errorWarn = false;
	bool everBusOff = false, everErrorPassive = false;
	uint8_t peakTEC = 0, peakREC = 0;
	icsneo::CANErrorCode errorCode = icsneo::CANErrorCode::NoError;
	uint64_t changes = 0;
};

bool stage(const char* name, bool ok, const std::string& detail = {}) {
	std::cout << (ok ? "  PASS  " : "  FAIL  ") << std::left << std::setw(34) << name;
	if(!detail.empty())
		std::cout << detail;
	std::cout << std::endl;
	ok ? ++gPass : ++gFail;
	return ok;
}

void drainEvents(const char* context) {
	const auto events = icsneo::GetEvents();
	for(const auto& e : events)
		std::cout << "        [" << context << "] " << e.describe() << std::endl;
}

} // namespace

int main(int argc, char** argv) {
	const int holdSeconds = (argc > 1) ? std::atoi(argv[1]) : 30;
	const int txPeriodMs = (argc > 2) ? std::atoi(argv[2]) : 0;
	std::signal(SIGINT, onSigint);

	std::cout << "libicsneo " << icsneo::GetVersion() << " -- FW2 smoke test\n" << std::endl;

	// ---- Stage 1: the device type is compiled in -------------------------
	bool typeKnown = false;
	for(const auto& t : icsneo::GetSupportedDevices()) {
		if(t.getDeviceType() == icsneo::DeviceType::FreeWili2)
			typeKnown = true;
	}
	stage("FreeWili2 in supported devices", typeKnown,
		typeKnown ? "" : "built without LIBICSNEO_ENABLE_ONEWILI?");

	// ---- Stage 2: enumeration --------------------------------------------
	auto devices = icsneo::FindAllDevices();
	std::cout << "\n  FindAllDevices() -> " << devices.size() << " device(s)" << std::endl;
	for(const auto& d : devices)
		std::cout << "        " << d->describe() << std::endl;
	drainEvents("find");

	std::shared_ptr<icsneo::Device> fw2;
	for(const auto& d : devices) {
		if(d->getType().getDeviceType() == icsneo::DeviceType::FreeWili2)
			fw2 = d;
	}
	std::cout << std::endl;
	if(!stage("FW2 enumerated", static_cast<bool>(fw2), fw2 ? fw2->describe() : "no FW2 present")) {
		std::cout << "\n" << gPass << " passed, " << gFail << " failed -- stopping, no device."
			<< std::endl;
		return 1;
	}

	// ---- Stage 3: open ----------------------------------------------------
	const bool opened = fw2->open();
	stage("open()", opened, opened ? "" : icsneo::GetLastError().describe());
	drainEvents("open");
	if(!opened) {
		std::cout << "\n" << gPass << " passed, " << gFail << " failed." << std::endl;
		return 1;
	}

	// ---- Stage 4: RX plumbing + goOnline ---------------------------------
	std::atomic<uint64_t> rxCount{0};
	std::atomic<uint64_t> firstTimestamp{0};
	const int cbId = fw2->addMessageCallback(std::make_shared<icsneo::MessageCallback>(
		[&](std::shared_ptr<icsneo::Message> msg) {
			const auto can = std::static_pointer_cast<icsneo::CANMessage>(msg);
			const auto n = ++rxCount;
			if(n == 1) {
				firstTimestamp = can->timestamp;
				std::cout << "        first RX: arbid=0x" << std::hex << can->arbid << std::dec
					<< " dlc=" << can->data.size()
					<< " fd=" << can->isCANFD
					<< " ext=" << can->isExtended
					<< " ts=" << can->timestamp << std::endl;
			}
		},
		icsneo::MessageFilter(icsneo::Message::Type::Frame)));

	// Bus state, which FW2 reports by polling the controller's TEC/REC and error
	// latches. Registered before goOnline() because that is what starts the
	// polling, and a board already in bus-off should be caught by the first read.
	BusSummary bus;
	const int errCbId = fw2->addMessageCallback(std::make_shared<icsneo::MessageCallback>(
		[&](std::shared_ptr<icsneo::Message> msg) {
			const auto err = std::static_pointer_cast<icsneo::CANErrorMessage>(msg);
			std::scoped_lock<std::mutex> lk(bus.lock);
			bus.seen = true;
			bus.tec = err->transmitErrorCount;
			bus.rec = err->receiveErrorCount;
			bus.busOff = err->busOff;
			bus.errorPassive = err->errorPassive;
			bus.errorWarn = err->errorWarn;
			bus.everBusOff = bus.everBusOff || err->busOff;
			bus.everErrorPassive = bus.everErrorPassive || err->errorPassive;
			bus.peakTEC = std::max(bus.peakTEC, err->transmitErrorCount);
			bus.peakREC = std::max(bus.peakREC, err->receiveErrorCount);
			if(err->errorCode != icsneo::CANErrorCode::NoError)
				bus.errorCode = err->errorCode;
			++bus.changes;
			std::cout << "        bus state: TEC " << static_cast<int>(err->transmitErrorCount)
				<< " REC " << static_cast<int>(err->receiveErrorCount)
				<< (err->busOff ? " BUS-OFF" : (err->errorPassive ? " error-passive"
					: (err->errorWarn ? " error-warn" : "")))
				<< " last=" << errorCodeName(err->errorCode) << std::endl;
		},
		icsneo::MessageFilter(icsneo::Message::Type::CANError)));

	const bool online = fw2->goOnline();
	stage("goOnline()", online, online ? "" : icsneo::GetLastError().describe());
	drainEvents("online");

	// ---- Stage 5: the heartbeat hold (and optional transmit) --------------
	// Device::open() starts a 110 ms RequestStatusUpdate heartbeat, which for FW2
	// is a real h\t liveness probe. If the board stops answering, isOpen() goes
	// false -- that is precisely what this loop is watching for.
	std::cout << "\n  Holding for " << holdSeconds << "s (Ctrl-C to stop early)..." << std::endl;
	if(txPeriodMs > 0) {
		std::cout << "  Transmitting arbid 0x" << std::hex << TX_ARBID << std::dec
			<< " every " << txPeriodMs << " ms via Device::transmit()" << std::endl;
	}

	const auto start = std::chrono::steady_clock::now();
	auto nextTx = start;
	uint64_t txOk = 0, txFail = 0;
	bool stayedOpen = true;
	int lastReported = -1;
	while(!gStop) {
		const auto now = std::chrono::steady_clock::now();
		const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
		if(elapsed >= holdSeconds)
			break;
		if(!fw2->isOpen()) {
			stayedOpen = false;
			std::cout << "        disconnected after " << elapsed << "s" << std::endl;
			break;
		}

		if(txPeriodMs > 0 && now >= nextTx) {
			// Counter in the payload so the partner can see frames advancing
			// rather than a single message repeated, and spot any gaps.
			auto msg = std::make_shared<icsneo::CANMessage>();
			msg->network = icsneo::Network::NetID::DWCAN_01;
			msg->arbid = TX_ARBID;
			msg->isExtended = false;
			msg->isCANFD = false;
			const uint64_t n = txOk + txFail;
			msg->data = { 0xDE, 0xAD, 0xBE, 0xEF,
				static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16),
				static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n) };

			if(fw2->transmit(msg)) {
				++txOk;
			} else if(++txFail == 1) {
				// Only the first failure, or a wedged bus would bury the log.
				std::cout << "        first TX failure: "
					<< icsneo::GetLastError().describe() << std::endl;
			}
			// Fixed cadence from the schedule, not from now(), so transmit()
			// latency does not quietly stretch the period.
			nextTx += std::chrono::milliseconds(txPeriodMs);
			if(nextTx < now) // Fell behind; resynchronise rather than burst
				nextTx = now + std::chrono::milliseconds(txPeriodMs);
		}

		if(elapsed % 10 == 0 && elapsed != lastReported) {
			lastReported = static_cast<int>(elapsed);
			std::cout << "        t=" << elapsed << "s  rx=" << rxCount.load()
				<< "  online=" << fw2->isOnline();
			if(txPeriodMs > 0)
				std::cout << "  tx=" << txOk << " ok / " << txFail << " fail";
			std::cout << std::endl;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(txPeriodMs > 0 ? 2 : 200));
	}
	stage("heartbeat held", stayedOpen,
		stayedOpen ? (std::to_string(holdSeconds) + "s, " + std::to_string(rxCount.load()) + " frames RX")
		           : "device dropped");
	if(txPeriodMs > 0) {
		stage("transmit()", txFail == 0 && txOk > 0,
			std::to_string(txOk) + " accepted, " + std::to_string(txFail) + " rejected");
	}

	// Bus health. Not folded into the transmit stage: transmit() reports whether
	// the firmware accepted the command, and this reports whether the wire
	// agreed -- a run where every transmit is "accepted" while TEC climbs to
	// bus-off is exactly the case worth separating.
	{
		std::scoped_lock<std::mutex> lk(bus.lock);
		if(!bus.seen) {
			std::cout << "  ----  " << std::left << std::setw(34) << "bus state"
				<< "not reported -- controller registers unreadable" << std::endl;
		} else {
			const std::string detail = "peak TEC " + std::to_string(static_cast<int>(bus.peakTEC))
				+ ", peak REC " + std::to_string(static_cast<int>(bus.peakREC))
				+ ", last error " + errorCodeName(bus.errorCode);
			stage("bus stayed error-active", !bus.everBusOff && !bus.everErrorPassive, detail);
		}
	}
	drainEvents("hold");

	// ---- Stage 6: teardown ------------------------------------------------
	// Each step is announced *before* it runs: teardown is where a hang would
	// otherwise be invisible, since stage() only prints once a call returns.
	auto step = [](const char* what) {
		std::cout << "        -> " << what << std::flush;
	};
	const auto stepStart = std::chrono::steady_clock::now();
	auto stepDone = [&stepStart]() {
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - stepStart).count();
		std::cout << " (" << ms << " ms cumulative)" << std::endl;
	};

	step("removeMessageCallback");
	fw2->removeMessageCallback(cbId);
	fw2->removeMessageCallback(errCbId);
	stepDone();

	if(online) {
		step("goOffline");
		const bool wentOffline = fw2->goOffline();
		stepDone();
		stage("goOffline()", wentOffline);
	}

	step("close");
	const bool closed = fw2->close();
	stepDone();
	stage("close()", closed, closed ? "" : icsneo::GetLastError().describe());
	drainEvents("close");

	std::cout << "\n  RX total: " << rxCount.load() << " frame(s)" << std::endl;
	if(rxCount.load() == 0)
		std::cout << "  note: 0 frames is expected if nothing else is transmitting on the bus."
			<< std::endl;

	std::cout << "\n" << gPass << " passed, " << gFail << " failed." << std::endl;
	return gFail == 0 ? 0 : 1;
}
