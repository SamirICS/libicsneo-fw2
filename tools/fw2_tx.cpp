// fw2_tx -- interactive CAN transmit console for the FreeWili 2.
//
// Opens the FW2 through the ordinary libicsneo path (open -> goOnline ->
// Device::transmit), then hands the keyboard the start/stop switch so a bus
// partner can be watched while transmission is turned on and off deliberately.
// That on/off control is the whole point: a partner that shows frames appearing
// exactly when TX is enabled and stopping when it is disabled has proven the
// link in a way a fixed-duration batch run cannot.
//
// Keys:
//   space / s   start or stop transmitting
//   + / -       halve or double the period (10 ms .. 5 s)
//   r           reset the counters
//   q           quit (also Ctrl-C)
//
// Usage: fw2_tx [period_ms] [arbid_hex]      (defaults: 100, 123)

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include "icsneo/icsneocpp.h"
#include "icsneo/communication/message/canmessage.h"
#include "icsneo/communication/message/canerrormessage.h"

namespace {

std::atomic<bool> gQuit{false};
void onSignal(int) { gQuit = true; }

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
 * Latest CAN bus state, as reported by FW2Communication's poller.
 *
 * Written from the poller thread and read by the draw loop, so it is guarded
 * rather than atomic -- the fields are only meaningful together. A transition
 * is parked in `pending` for the main loop to print instead of being written
 * from the callback, which would interleave with the status line being redrawn.
 */
struct BusState {
	std::mutex lock;
	bool seen = false;
	uint8_t tec = 0, rec = 0;
	bool busOff = false, errorPassive = false, errorWarn = false;
	icsneo::CANErrorCode errorCode = icsneo::CANErrorCode::NoError;
	icsneo::CANErrorCode dataErrorCode = icsneo::CANErrorCode::NoError;
	uint64_t changes = 0;
	std::string pending;
};

/**
 * Puts the terminal in raw-ish mode so single keypresses arrive without Enter,
 * and -- more importantly -- puts it back on the way out.
 *
 * A destructor rather than a cleanup call at the end of main: this program can
 * leave through Ctrl-C or an early return, and a terminal left in non-canonical
 * no-echo mode is a genuinely hostile thing to hand back to a user.
 */
class RawTerminal {
public:
	RawTerminal() {
		// O_NONBLOCK first, and unconditionally. termios VMIN/VTIME only make
		// read() non-blocking on a tty, so without this a piped stdin blocks
		// the whole loop and transmission stalls between keystrokes -- which is
		// exactly what happened the first time this was driven from a script.
		flags = fcntl(STDIN_FILENO, F_GETFL);
		if(flags != -1)
			fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

		if(tcgetattr(STDIN_FILENO, &saved) != 0)
			return; // Not a tty; the O_NONBLOCK above is all we need
		valid = true;
		termios raw = saved;
		raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
		raw.c_cc[VMIN] = 0;
		raw.c_cc[VTIME] = 0;
		tcsetattr(STDIN_FILENO, TCSANOW, &raw);
	}
	~RawTerminal() {
		if(valid)
			tcsetattr(STDIN_FILENO, TCSANOW, &saved);
		if(flags != -1)
			fcntl(STDIN_FILENO, F_SETFL, flags);
	}
	RawTerminal(const RawTerminal&) = delete;
	RawTerminal& operator=(const RawTerminal&) = delete;

private:
	termios saved{};
	bool valid = false;
	int flags = -1;
};

/** Next pending keypress, or 0 if none is waiting. */
char pollKey() {
	char c = 0;
	return read(STDIN_FILENO, &c, 1) == 1 ? c : 0;
}

// The status line is rewritten in place, so anything else printed has to clear
// it first or the two overlap. \r returns to column 0, \033[K erases the rest.
void printAbove(const std::string& text) {
	std::cout << "\r\033[K" << text << std::endl;
}

} // namespace

int main(int argc, char** argv) {
	int periodMs = (argc > 1) ? std::atoi(argv[1]) : 100;
	if(periodMs < 10)
		periodMs = 10;
	const uint32_t arbid = (argc > 2) ? std::strtoul(argv[2], nullptr, 16) : 0x123;

	std::signal(SIGINT, onSignal);
	std::signal(SIGTERM, onSignal);

	std::cout << "libicsneo " << icsneo::GetVersion() << " -- FW2 transmit console\n" << std::endl;

	std::shared_ptr<icsneo::Device> fw2;
	for(const auto& d : icsneo::FindAllDevices()) {
		if(d->getType().getDeviceType() == icsneo::DeviceType::FreeWili2)
			fw2 = d;
	}
	if(!fw2) {
		std::cout << "  no FreeWili 2 found -- is it plugged in?" << std::endl;
		return 1;
	}
	std::cout << "  device:  " << fw2->describe() << std::endl;

	if(!fw2->open()) {
		std::cout << "  open() failed: " << icsneo::GetLastError().describe() << std::endl;
		return 1;
	}

	std::atomic<uint64_t> rxCount{0};
	std::atomic<uint32_t> rxLastArbid{0};
	const int cbId = fw2->addMessageCallback(std::make_shared<icsneo::MessageCallback>(
		[&](std::shared_ptr<icsneo::Message> msg) {
			const auto can = std::static_pointer_cast<icsneo::CANMessage>(msg);
			rxLastArbid = static_cast<uint32_t>(can->arbid);
			++rxCount;
		},
		icsneo::MessageFilter(icsneo::Message::Type::Frame)));

	// Bus state. This is the reason a silent bus and a broken one no longer look
	// alike: FW2 polls TEC/REC and the controller's error latches, and a cable
	// fault shows up here as a climbing TEC and an ACK error long before
	// anything else in this program changes.
	BusState bus;
	const int errCbId = fw2->addMessageCallback(std::make_shared<icsneo::MessageCallback>(
		[&](std::shared_ptr<icsneo::Message> msg) {
			const auto err = std::static_pointer_cast<icsneo::CANErrorMessage>(msg);
			std::ostringstream line;
			line << "  !! bus state: TEC " << static_cast<int>(err->transmitErrorCount)
				<< "  REC " << static_cast<int>(err->receiveErrorCount);
			if(err->busOff)
				line << "  BUS-OFF";
			else if(err->errorPassive)
				line << "  error-passive";
			else if(err->errorWarn)
				line << "  error-warn";
			if(err->errorCode != icsneo::CANErrorCode::NoError)
				line << "  last " << errorCodeName(err->errorCode);
			if(err->dataErrorCode != icsneo::CANErrorCode::NoError)
				line << "  data " << errorCodeName(err->dataErrorCode);

			std::scoped_lock<std::mutex> lk(bus.lock);
			bus.seen = true;
			bus.tec = err->transmitErrorCount;
			bus.rec = err->receiveErrorCount;
			bus.busOff = err->busOff;
			bus.errorPassive = err->errorPassive;
			bus.errorWarn = err->errorWarn;
			bus.errorCode = err->errorCode;
			bus.dataErrorCode = err->dataErrorCode;
			++bus.changes;
			bus.pending = line.str();
		},
		icsneo::MessageFilter(icsneo::Message::Type::CANError)));

	if(!fw2->goOnline()) {
		std::cout << "  goOnline() failed: " << icsneo::GetLastError().describe() << std::endl;
		fw2->close();
		return 1;
	}

	std::cout << "  online,  arbid 0x" << std::hex << arbid << std::dec
		<< ",  period " << periodMs << " ms\n" << std::endl;
	std::cout << "  [space/s] start-stop TX   [+/-] period   [r] reset counters   [q] quit\n"
		<< std::endl;

	RawTerminal rawMode;

	bool transmitting = false;
	uint64_t txOk = 0, txFail = 0;
	auto nextTx = std::chrono::steady_clock::now();
	auto nextDraw = std::chrono::steady_clock::now();
	std::string lastError;

	while(!gQuit) {
		const auto now = std::chrono::steady_clock::now();

		switch(pollKey()) {
			case ' ':
			case 's':
				transmitting = !transmitting;
				// Re-anchor rather than resume the old schedule: otherwise a
				// long pause leaves nextTx far behind and the first thing the
				// partner sees on restart is a catch-up burst.
				nextTx = now;
				printAbove(transmitting ? "  >> TX STARTED" : "  || TX STOPPED");
				break;
			case '+':
			case '=':
				periodMs = periodMs > 10 ? periodMs / 2 : 10;
				printAbove("  period -> " + std::to_string(periodMs) + " ms");
				break;
			case '-':
			case '_':
				periodMs = periodMs < 5000 ? periodMs * 2 : 5000;
				printAbove("  period -> " + std::to_string(periodMs) + " ms");
				break;
			case 'r':
				txOk = txFail = 0;
				rxCount = 0;
				rxLastArbid = 0;
				lastError.clear();
				printAbove("  counters reset");
				break;
			case 'q':
				gQuit = true;
				break;
			default:
				break;
		}

		if(!fw2->isOpen()) {
			printAbove("  !! device disconnected");
			break;
		}

		if(transmitting && now >= nextTx) {
			auto msg = std::make_shared<icsneo::CANMessage>();
			msg->network = icsneo::Network::NetID::DWCAN_01;
			msg->arbid = arbid;
			msg->isExtended = false;
			msg->isCANFD = false;
			// Rolling counter in the payload so the partner can tell live
			// traffic from one frame repeated, and can see gaps.
			const uint64_t n = txOk + txFail;
			msg->data = { 0xDE, 0xAD, 0xBE, 0xEF,
				static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16),
				static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n) };

			if(fw2->transmit(msg)) {
				++txOk;
			} else {
				++txFail;
				lastError = icsneo::GetLastError().describe();
			}

			nextTx += std::chrono::milliseconds(periodMs);
			if(nextTx < now) // Fell behind; resynchronise instead of bursting
				nextTx = now + std::chrono::milliseconds(periodMs);
		}

		// Bus-state transitions are printed from here, not from the callback, so
		// they cannot interleave with the status line being redrawn.
		{
			std::string transition;
			{
				std::scoped_lock<std::mutex> lk(bus.lock);
				transition.swap(bus.pending);
			}
			if(!transition.empty())
				printAbove(transition);
		}

		if(now >= nextDraw) {
			nextDraw = now + std::chrono::milliseconds(100);
			std::ostringstream line;
			line << "\r\033[K  [" << (transmitting ? "TX ON " : "TX OFF")
				<< "]  period " << std::setw(4) << periodMs << " ms"
				<< "   tx " << txOk << " ok";
			if(txFail)
				line << " / " << txFail << " FAIL";
			line << "   rx " << rxCount.load();
			if(rxCount.load())
				line << " (last 0x" << std::hex << rxLastArbid.load() << std::dec << ")";

			{
				std::scoped_lock<std::mutex> lk(bus.lock);
				if(!bus.seen) {
					// Distinguished from "0/0" deliberately: no reading at all is
					// a different claim from a healthy controller, and on
					// firmware without `i\c\r` this is what you get.
					line << "   bus ?";
				} else {
					line << "   TEC " << static_cast<int>(bus.tec)
						<< " REC " << static_cast<int>(bus.rec);
					if(bus.busOff)
						line << " BUS-OFF";
					else if(bus.errorPassive)
						line << " PASSIVE";
					else if(bus.errorWarn)
						line << " WARN";
					if(bus.errorCode != icsneo::CANErrorCode::NoError)
						line << " " << errorCodeName(bus.errorCode);
				}
			}
			std::cout << line.str() << std::flush;
		}

		// Short enough that a keypress feels immediate and a 10 ms period is
		// still achievable, long enough not to spin a core.
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}

	std::cout << "\r\033[K" << std::flush;
	printAbove("  closing...");

	fw2->removeMessageCallback(cbId);
	fw2->removeMessageCallback(errCbId);
	fw2->goOffline();
	const bool closed = fw2->close();

	std::cout << "\n  transmitted : " << txOk << " accepted, " << txFail << " rejected" << std::endl;
	if(!lastError.empty())
		std::cout << "  last TX error: " << lastError << std::endl;
	std::cout << "  received    : " << rxCount.load() << " frame(s)" << std::endl;
	{
		std::scoped_lock<std::mutex> lk(bus.lock);
		if(!bus.seen) {
			std::cout << "  bus state   : never reported -- the controller's registers were"
				" not readable" << std::endl;
		} else {
			std::cout << "  bus state   : TEC " << static_cast<int>(bus.tec)
				<< ", REC " << static_cast<int>(bus.rec)
				<< (bus.busOff ? ", BUS-OFF" : (bus.errorPassive ? ", error-passive"
					: (bus.errorWarn ? ", error-warn" : ", no error flags")))
				<< ", last error " << errorCodeName(bus.errorCode)
				<< " (" << bus.changes << " change(s))" << std::endl;
		}
	}
	std::cout << "  close()     : " << (closed ? "ok" : icsneo::GetLastError().describe()) << std::endl;

	for(const auto& e : icsneo::GetEvents())
		std::cout << "  [event] " << e.describe() << std::endl;

	return 0;
}
