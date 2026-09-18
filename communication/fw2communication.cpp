#include "icsneo/communication/fw2communication.h"
#include "icsneo/communication/message/canmessage.h"
#include "icsneo/communication/message/canerrormessage.h"
#include "icsneo/communication/message/resetstatusmessage.h"
#include "icsneo/communication/command.h"
#include "icsneo/communication/network.h"
#include "icsneo/api/eventmanager.h"

#include "onewili_binary_framing.h"
#include "onewili_events.h"

#include <array>
#include <cstdio>
#include <optional>
#include <sstream>
#include <vector>

using namespace icsneo;

namespace {

/**
 * header_type of a canRxReport WILI frame.
 *
 * From the vendored decoder table in onewili_events.c:
 *   { 1, 84, "canRxReport", ow__ev_can_rx_report }
 */
constexpr uint16_t CAN_RX_REPORT_HEADER_TYPE = 1;
constexpr uint32_t CAN_RX_REPORT_PAYLOAD_SIZE = 84;

/*
 * MCP2518FD receive object header. FW2 forwards these two registers verbatim,
 * which is why the event struct calls them r0/r1 rather than naming the fields.
 *
 * TODO(bench): Phase 2 must confirm this layout against real captures before
 * these decodes are trusted. It is the datasheet layout, but "the datasheet
 * layout, byte-swapped by firmware" is exactly the kind of thing that only
 * shows up on a scope.
 */
constexpr uint32_t R0_SID_MASK = 0x000007FFu; // bits 0-10
constexpr uint32_t R0_EID_SHIFT = 11;
constexpr uint32_t R0_EID_MASK = 0x0003FFFFu; // bits 11-28, 18 bits

constexpr uint32_t R1_DLC_MASK = 0x0000000Fu; // bits 0-3
constexpr uint32_t R1_IDE = 1u << 4;
constexpr uint32_t R1_RTR = 1u << 5;
constexpr uint32_t R1_BRS = 1u << 6;
constexpr uint32_t R1_FDF = 1u << 7;
constexpr uint32_t R1_ESI = 1u << 8;

/** CAN FD DLC -> payload length. Classic CAN saturates at 8. */
uint8_t dlcToLength(uint8_t dlc, bool fd) {
	if(dlc <= 8)
		return dlc;
	if(!fd)
		return 8;
	static constexpr std::array<uint8_t, 7> FD_LENGTHS = { 12, 16, 20, 24, 32, 48, 64 };
	return FD_LENGTHS[dlc - 9];
}

/** Payload length -> CAN FD DLC. Nullopt for a length CAN FD cannot express. */
std::optional<uint8_t> lengthToDLC(size_t length, bool fd) {
	if(length <= 8)
		return static_cast<uint8_t>(length);
	if(!fd)
		return std::nullopt;
	switch(length) {
		case 12: return 9;
		case 16: return 10;
		case 20: return 11;
		case 24: return 12;
		case 32: return 13;
		case 48: return 14;
		case 64: return 15;
		default: return std::nullopt; // CAN FD has no DLC for e.g. 9 bytes
	}
}

/**
 * Format a OneWili command exactly as the generated client would, then frame it
 * for the wire: 0x02 (reset to root menu + quiet mode), the command, newline.
 *
 * Reproduced rather than called because ow__call() would block for a round trip
 * per frame. The formatting itself must stay byte-identical to
 * ow_io_canfd_write_canfd(), so any change to the vendored library's
 * ow__cat_hex/ow__cat_bytes has to be mirrored here.
 *
 * TODO(test): golden-vector tests pinning this against the vendored formatter.
 */
std::vector<uint8_t> frameCommand(const std::string& command) {
	std::vector<uint8_t> out;
	out.reserve(command.size() + 2);
	out.push_back(0x02);
	out.insert(out.end(), command.begin(), command.end());
	out.push_back('\n');
	return out;
}

/** True if a text line is a OneWili response frame rather than an event. */
bool isResponseFrame(const std::string& line) {
	// "[<path> ...]" is a response; "[*<id> ...]" is a spontaneous event.
	if(line.size() < 2 || line[0] != '[')
		return false;
	return line[1] != '*';
}

/**
 * True if a OneWili response frame reports success.
 *
 * A frame is "[<path> <id> <seq> <body...> <ok>]" and the status is the last
 * space-separated token before the closing bracket: "1" succeeded, "0" failed.
 * This mirrors ow__parse_frame() in the vendored client
 * (third-party/onewili/src/onewili.c:78), which is the only definition of the
 * protocol we have.
 *
 * Deliberately not a search for the word "ok". Measured against real firmware,
 * that is wrong in both directions: "Ok" appears inside the *payload* of some
 * successful frames (h\p\g prints it as text) and is absent from others that
 * also succeeded (h\t returns bare numbers, "[h\t ... 09 46 46 1]").
 */
bool frameReportsSuccess(const std::string& line) {
	if(line.size() < 5 || line.front() != '[' || line.back() != ']')
		return false;
	const auto lastSpace = line.rfind(' ', line.size() - 2);
	if(lastSpace == std::string::npos)
		return false;
	return line[lastSpace + 1] == '1';
}

/**
 * Parse a spontaneous "[*can0 ...]" / "[*can1 ...]" text event into a
 * CANMessage. This is the RX path that actually carries traffic on hardware
 * measured so far -- the binary canRxReport stream this port originally
 * relied on has not been observed to fire at all, while these text events
 * stream reliably the moment a real frame hits the bus.
 *
 * Observed wire shape: "[*can0 <ts_hex16> <seq_dec> <arbid_hex>[x] <data hex
 * bytes...> <ok>]" -- the same generic <ts><seq>...<ok> envelope every OneWili
 * line uses (frameReportsSuccess() above reads the same trailing <ok>).
 * arbid's trailing 'x'/'X' marks an extended (29-bit) id, per the event's
 * documented "hex arb id, 'x' suffix = extended" description.
 */
bool tryParseCanTextEvent(const std::string& line, std::shared_ptr<CANMessage>& out) {
	if(line.size() < 4 || line.front() != '[' || line.back() != ']')
		return false;

	std::istringstream tokens(line.substr(1, line.size() - 2));
	std::string idToken;
	tokens >> idToken;
	Network::NetID netId;
	if(idToken == "*can0")
		netId = Network::NetID::DWCAN_01;
	else if(idToken == "*can1")
		netId = Network::NetID::DWCAN_02;
	else
		return false;

	std::vector<std::string> rest;
	for(std::string tok; tokens >> tok;)
		rest.push_back(tok);
	// <ts> <seq> <arbid> <ok>, at minimum, for a zero-length data frame.
	if(rest.size() < 4)
		return false;
	rest.pop_back(); // trailing <ok>

	std::string arbToken = rest[2];
	bool extended = false;
	if(!arbToken.empty() && (arbToken.back() == 'x' || arbToken.back() == 'X')) {
		extended = true;
		arbToken.pop_back();
	}

	uint32_t arbid = 0;
	try {
		arbid = static_cast<uint32_t>(std::stoul(arbToken, nullptr, 16));
	} catch(...) {
		return false;
	}

	std::vector<uint8_t> data;
	for(size_t i = 3; i < rest.size(); ++i) {
		if(rest[i].size() != 2)
			continue;
		try {
			data.push_back(static_cast<uint8_t>(std::stoul(rest[i], nullptr, 16)));
		} catch(...) {
			return false;
		}
	}

	auto msg = std::make_shared<CANMessage>();
	msg->network = netId;
	msg->arbid = arbid;
	msg->isExtended = extended;
	msg->isCANFD = false; // can0/can1 text events are the classic-CAN path only
	msg->data = std::move(data);
	// No timestamp field on this path; 0 matches the convention the binary
	// path uses for fields it cannot populate (see drainBinaryFrames()).
	msg->timestamp = 0;
	out = msg;
	return true;
}

/*
 * MCP2518FD SFR offsets used for bus state. From the datasheet memory map; the
 * CAN SFR block starts at 0x000 in the controller's address space.
 */
constexpr uint32_t REG_C1TREC = 0x01C;   // TEC / REC and the error-state flags
constexpr uint32_t REG_BUS_STATE_WORDS = 3; // C1TREC, C1BDIAG0, C1BDIAG1

// C1TREC
constexpr uint32_t TREC_REC_MASK = 0x000000FFu; // bits 0-7
constexpr uint32_t TREC_TEC_SHIFT = 8;
constexpr uint32_t TREC_TEC_MASK = 0x000000FFu; // bits 8-15
constexpr uint32_t TREC_EWARN = 1u << 16;
constexpr uint32_t TREC_RXBP = 1u << 19; // Receiver bus passive
constexpr uint32_t TREC_TXBP = 1u << 20; // Transmitter bus passive
constexpr uint32_t TREC_TXBO = 1u << 21; // Transmitter bus off

// C1BDIAG1 -- nominal-phase error latches
constexpr uint32_t BDIAG1_NBIT0ERR = 1u << 16;
constexpr uint32_t BDIAG1_NBIT1ERR = 1u << 17;
constexpr uint32_t BDIAG1_NACKERR = 1u << 18;
constexpr uint32_t BDIAG1_NFORMERR = 1u << 19;
constexpr uint32_t BDIAG1_NSTUFERR = 1u << 20;
constexpr uint32_t BDIAG1_NCRCERR = 1u << 21;
// C1BDIAG1 -- data-phase error latches
constexpr uint32_t BDIAG1_DBIT0ERR = 1u << 24;
constexpr uint32_t BDIAG1_DBIT1ERR = 1u << 25;
constexpr uint32_t BDIAG1_DFORMERR = 1u << 27;
constexpr uint32_t BDIAG1_DSTUFERR = 1u << 28;
constexpr uint32_t BDIAG1_DCRCERR = 1u << 29;

/** Every C1BDIAG1 bit we translate; used to ignore EFMSGCNT churn. */
constexpr uint32_t BDIAG1_ERROR_BITS =
	BDIAG1_NBIT0ERR | BDIAG1_NBIT1ERR | BDIAG1_NACKERR | BDIAG1_NFORMERR |
	BDIAG1_NSTUFERR | BDIAG1_NCRCERR | BDIAG1_DBIT0ERR | BDIAG1_DBIT1ERR |
	BDIAG1_DFORMERR | BDIAG1_DSTUFERR | BDIAG1_DCRCERR;

/**
 * Collapse a set of C1BDIAG1 error latches into libicsneo's single CANErrorCode.
 *
 * The register latches every error type independently and libicsneo carries one
 * code per phase, so this has to choose. The order below is diagnostic value,
 * not datasheet order: on a miswired or unterminated bus an ACK error is the
 * thing that is actually wrong, and reporting a stuff error instead -- true, but
 * a downstream consequence -- sends the reader after the wrong fault.
 */
CANErrorCode worstErrorCode(uint32_t bdiag1, bool dataPhase) {
	if(dataPhase) {
		if(bdiag1 & BDIAG1_DCRCERR) return CANErrorCode::CRCError;
		if(bdiag1 & BDIAG1_DFORMERR) return CANErrorCode::FormError;
		if(bdiag1 & BDIAG1_DSTUFERR) return CANErrorCode::StuffError;
		if(bdiag1 & BDIAG1_DBIT1ERR) return CANErrorCode::Bit1Error;
		if(bdiag1 & BDIAG1_DBIT0ERR) return CANErrorCode::Bit0Error;
		return CANErrorCode::NoError;
	}
	if(bdiag1 & BDIAG1_NACKERR) return CANErrorCode::AckError;
	if(bdiag1 & BDIAG1_NCRCERR) return CANErrorCode::CRCError;
	if(bdiag1 & BDIAG1_NFORMERR) return CANErrorCode::FormError;
	if(bdiag1 & BDIAG1_NSTUFERR) return CANErrorCode::StuffError;
	if(bdiag1 & BDIAG1_NBIT1ERR) return CANErrorCode::Bit1Error;
	if(bdiag1 & BDIAG1_NBIT0ERR) return CANErrorCode::Bit0Error;
	return CANErrorCode::NoError;
}

/**
 * Pull `count` register words out of an `i\c\r` response frame.
 *
 * The frame is "[<path> <id> <seq> <body...> <ok>]" and the body is where the
 * register words live, but the exact number of leading metadata tokens is not
 * something this port can pin down from the vendored client -- it calls
 * ow__rest_str() and hands the whole remainder back to its caller.
 *
 * So rather than counting tokens, this takes every token that is exactly eight
 * hex digits and keeps the LAST `count` of them. Eight digits is the width
 * ow__cat_hex() uses for register data, and taking from the end skips any
 * id/seq that happens to look the same. Fewer than `count` matches means the
 * firmware did not return register data, which the caller must treat as a
 * failure rather than as zeroes.
 */
bool parseRegisterWords(const std::string& frame, size_t count, std::vector<uint32_t>& out) {
	if(frame.size() < 2 || frame.front() != '[' || frame.back() != ']')
		return false;

	std::vector<uint32_t> words;
	std::istringstream tokens(frame.substr(1, frame.size() - 2));
	std::string token;
	while(tokens >> token) {
		if(token.size() != 8)
			continue;
		if(token.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
			continue;
		words.push_back(static_cast<uint32_t>(std::stoul(token, nullptr, 16)));
	}

	if(words.size() < count)
		return false;
	out.assign(words.end() - static_cast<std::ptrdiff_t>(count), words.end());
	return true;
}

} // namespace

/** Wraps the vendored C parser so its 4 KB buffer stays out of the header. */
struct FW2Communication::BinaryParser {
	ow_bin_parser parser;
	BinaryParser() { ow_bin_parser_init(&parser); }
};

FW2Communication::FW2Communication(
	device_eventhandler_t report,
	std::unique_ptr<Driver>&& driver,
	std::function<std::unique_ptr<Packetizer>()> makeConfiguredPacketizer,
	std::unique_ptr<Encoder>&& encoder,
	std::unique_ptr<Decoder>&& decoder,
	uint8_t channel)
	: Communication(report, std::move(driver), makeConfiguredPacketizer,
		std::move(encoder), std::move(decoder)), channel(channel) {}

FW2Communication::~FW2Communication() {
	if(isOpen())
		close();
}

void FW2Communication::spawnThreads() {
	closing = false;
	binaryParser = std::make_unique<BinaryParser>();
	// Deliberately NOT Communication::spawnThreads(): readTask() would feed the
	// stream to a Packetizer expecting ICS framing, which FW2 never produces.
	rxThread = std::thread(&FW2Communication::rxTask, this);
	statusThread = std::thread(&FW2Communication::statusTask, this);
	busStateThread = std::thread(&FW2Communication::busStateTask, this);
}

void FW2Communication::joinThreads() {
	// Same order upstream uses: a paused read task would otherwise sit in its
	// wait until the timeout expires rather than noticing `closing`.
	if(pauseReadTask)
		resumeReads();

	closing = true;
	if(rxThread.joinable())
		rxThread.join();
	// Release anything blocked on TX backpressure, or close() will hang.
	{
		std::scoped_lock<std::mutex> lk(inFlightMutex);
		inFlight = 0;
	}
	inFlightCv.notify_all();
	{
		std::scoped_lock<std::mutex> lk(syncMutex);
		syncResponseReady = true;
		syncResponse.clear();
	}
	syncCv.notify_all();

	// Joined last, and only after the two releases above: a probe that was in
	// flight when close() was called is waiting on syncCv for a response that
	// rxTask() -- already joined -- can no longer deliver. Both of these threads
	// make synchronous calls, so both are in that position.
	statusCv.notify_all();
	if(statusThread.joinable())
		statusThread.join();

	busStateCv.notify_all();
	if(busStateThread.joinable())
		busStateThread.join();
}

void FW2Communication::statusTask() {
	EventManager::GetInstance().downgradeErrorsOnCurrentThread();

	while(!closing) {
		{
			std::unique_lock<std::mutex> lk(statusMutex);
			// The timeout is only a liveness check on `closing`; nothing is due
			// when it expires.
			statusCv.wait_for(lk, std::chrono::milliseconds(50),
				[this]() { return statusRequested || closing; });
			if(closing)
				return;
			if(!statusRequested)
				continue;
			statusRequested = false;
		}

		// Failure is not reported here. answerStatusRequest() simply does not
		// dispatch, and Device's heartbeat draws the disconnect conclusion from
		// the silence -- which is the mechanism it already uses for every other
		// device, and the only one that stays honest if the board is wedged.
		answerStatusRequest();
	}
}

void FW2Communication::setBusStatePolling(bool enable) {
	{
		std::scoped_lock<std::mutex> lk(busStateMutex);
		busStatePolling = enable;
		if(enable) {
			// Read immediately on the way up rather than after a full period, so
			// a board that is already in bus-off says so at goOnline() instead of
			// looking merely idle for the first half second.
			busStateDue = true;
			// A fresh online is a fresh question: re-report the state even if it
			// is bit-identical to whatever we last saw before going offline.
			haveLastBusState = false;
		}
	}
	busStateCv.notify_one();
}

bool FW2Communication::readCANRegisters(uint32_t address, size_t count,
		std::vector<uint32_t>& out, std::chrono::milliseconds timeout) {
	if(count == 0)
		return false;

	// Formatted exactly as ow_io_canfd_read_can_registers() does it: channel as
	// a decimal int, address as 8 hex digits, word count as a decimal int. The
	// 8-digit width is not cosmetic -- a bare "0" for the address was measured
	// to produce a response with no register data at all.
	char command[32];
	std::snprintf(command, sizeof(command), "i\\c\\r %d %08X %d",
		static_cast<int>(channel), address, static_cast<int>(count));

	std::string response;
	if(!callOneWili(command, response, timeout))
		return false;

	return parseRegisterWords(response, count, out);
}

void FW2Communication::busStateTask() {
	EventManager::GetInstance().downgradeErrorsOnCurrentThread();

	// Slow on purpose. Each poll is a synchronous round trip that serialises
	// against the TX pipeline and the heartbeat, and bus state is a condition
	// rather than an event -- a board in bus-off stays in bus-off. Half a second
	// is well inside human reaction time for someone wiggling a connector, which
	// is the use this was written for.
	constexpr auto POLL_PERIOD = std::chrono::milliseconds(500);

	while(!closing) {
		{
			std::unique_lock<std::mutex> lk(busStateMutex);
			busStateCv.wait_for(lk, POLL_PERIOD,
				[this]() { return closing || busStateDue || !busStatePolling; });
			if(closing)
				return;
			if(!busStatePolling || !busStateSupported)
				continue;
			busStateDue = false;
		}

		std::vector<uint32_t> regs;
		if(!readCANRegisters(REG_C1TREC, REG_BUS_STATE_WORDS, regs)) {
			// Two different failures land here and only one is worth giving up
			// on. A timeout is transient -- the board may simply be busy -- so
			// keep polling. A success frame carrying no register data means this
			// firmware does not implement `i\c\r`, and retrying that forever
			// costs a round trip every period for information that will never
			// arrive.
			std::string probe;
			if(callOneWili("h\\t", probe, std::chrono::milliseconds(250))) {
				// The board is answering, so the register read itself is what
				// came back empty.
				{
					std::scoped_lock<std::mutex> lk(busStateMutex);
					busStateSupported = false;
				}
				report(APIEvent::Type::NotSupported, APIEvent::Severity::EventWarning);
			}
			continue;
		}

		const uint32_t trec = regs[0];
		const uint32_t bdiag1 = regs[2];

		// EFMSGCNT in the low half of C1BDIAG1 counts *good* frames and moves
		// constantly on a live bus, so compare only the error latches. Without
		// this the change detector fires on every poll and buries the real
		// transitions.
		//
		// Those latches are sticky -- the controller sets them and only a write
		// clears them -- so they answer "has this ever happened" rather than "is
		// this happening now". That is deliberate: clearing them would be a
		// write performed by what is otherwise a read, and it would destroy
		// evidence for anything else looking at the same controller. TEC and REC
		// are the live signal, and they are in the same message.
		const uint32_t errorBits = bdiag1 & BDIAG1_ERROR_BITS;
		{
			std::scoped_lock<std::mutex> lk(busStateMutex);
			if(haveLastBusState && trec == lastTREC && errorBits == (lastBDIAG1 & BDIAG1_ERROR_BITS))
				continue;
			haveLastBusState = true;
			lastTREC = trec;
			lastBDIAG1 = bdiag1;
		}

		auto msg = std::make_shared<CANErrorMessage>();
		msg->network = Network::NetID::DWCAN_01;
		msg->receiveErrorCount = static_cast<uint8_t>(trec & TREC_REC_MASK);
		msg->transmitErrorCount = static_cast<uint8_t>((trec >> TREC_TEC_SHIFT) & TREC_TEC_MASK);
		msg->errorWarn = (trec & TREC_EWARN) != 0;
		// libicsneo carries one error-passive flag; the MCP2518FD tracks the
		// receiver and transmitter separately. Either one is error-passive.
		msg->errorPassive = (trec & (TREC_RXBP | TREC_TXBP)) != 0;
		msg->busOff = (trec & TREC_TXBO) != 0;
		msg->errorCode = worstErrorCode(bdiag1, false);
		msg->dataErrorCode = worstErrorCode(bdiag1, true);
		// Deliberately left at zero. FW2's timestamps come from canRxReport, and
		// this message is the product of a host-initiated poll -- there is no
		// device timestamp for it, and inventing one from the host clock would
		// put a different time base in the same field.
		msg->timestamp = 0;

		dispatchMessage(msg);
	}
}

void FW2Communication::rxTask() {
	EventManager::GetInstance().downgradeErrorsOnCurrentThread();

	auto* oneWili = dynamic_cast<OneWili*>(driver.get());

	RingBuffer& buffer = driver->getReadBuffer();

	while(!closing) {
		// Wake as soon as binary bytes land. The timeout still matters: text
		// acks arrive on the *other* port and never touch this ring buffer, so
		// on a silent CAN bus this is what services them.
		driver->waitForRx([this, &buffer]() { return buffer.size() > 0 || closing; },
			std::chrono::milliseconds(50));

		if(readsArePaused()) {
			std::unique_lock<std::mutex> lk(pauseReadTaskMutex);
			pauseReadTaskCv.wait_for(lk, std::chrono::milliseconds(50),
				[this]() { return !pauseReadTask || closing; });
			continue;
		}

		drainBinaryFrames();
		if(oneWili)
			drainTextResponses(*oneWili);
	}
}

void FW2Communication::drainBinaryFrames() {
	RingBuffer& buffer = driver->getReadBuffer();

	// Copy out in chunks: the vendored parser wants a flat buffer, and the ring
	// buffer's storage wraps.
	constexpr size_t CHUNK = 4096;
	uint8_t chunk[CHUNK];

	while(true) {
		const size_t available = buffer.size();
		if(available == 0)
			return;
		const size_t toRead = available < CHUNK ? available : CHUNK;
		if(!buffer.read(chunk, 0, toRead))
			return;
		buffer.pop(toRead);

		size_t offset = 0;
		while(offset < toRead) {
			ow_bin_frame frame = {};
			int ready = 0;
			const size_t consumed = ow_bin_parser_feed(&binaryParser->parser,
				chunk + offset, toRead - offset, &frame, &ready);
			if(consumed == 0 && !ready)
				break; // Parser made no progress; avoid spinning
			offset += consumed;
			if(!ready)
				continue;
			if(frame.header_type != CAN_RX_REPORT_HEADER_TYPE ||
					frame.payload_len != CAN_RX_REPORT_PAYLOAD_SIZE) {
				// gpioReport, logicAnalyzerReport and anything newer: not ours.
				continue;
			}

			ow_evt_can_rx_report report_ = {};
			if(ow_decode_can_rx_report(frame.payload, frame.payload_len, frame.error,
					&report_) != OW_OK) {
				continue;
			}

			auto msg = std::make_shared<CANMessage>();
			msg->network = Network::NetID::DWCAN_01;

			const uint32_t r0 = report_.r0_canid;
			const uint32_t r1 = report_.r1_filter_header_bits;

			msg->isExtended = (r1 & R1_IDE) != 0;
			msg->isCANFD = (r1 & R1_FDF) != 0;
			msg->baudrateSwitch = (r1 & R1_BRS) != 0;
			msg->errorStateIndicator = (r1 & R1_ESI) != 0;
			// CAN FD has no RTR; the bit is RRS there and must not be surfaced.
			msg->isRemote = !msg->isCANFD && (r1 & R1_RTR) != 0;
			msg->error = report_.error;
			if(report_.error) {
				// The header error bit says this one frame was bad and nothing
				// about bus state, so ask the poller to look now rather than up
				// to a period later. The counters it reads are what turn "a bad
				// frame arrived" into "the bus is degrading".
				{
					std::scoped_lock<std::mutex> lk(busStateMutex);
					busStateDue = true;
				}
				busStateCv.notify_one();
			}

			const uint32_t sid = r0 & R0_SID_MASK;
			const uint32_t eid = (r0 >> R0_EID_SHIFT) & R0_EID_MASK;
			msg->arbid = msg->isExtended ? ((sid << 18) | eid) : sid;

			msg->dlcOnWire = static_cast<uint8_t>(r1 & R1_DLC_MASK);
			const uint8_t length = dlcToLength(msg->dlcOnWire, msg->isCANFD);
			msg->data.resize(length);
			for(uint8_t i = 0; i < length; i++)
				msg->data[i] = static_cast<uint8_t>(report_.data_words[i / 4] >> (8 * (i % 4)));

			// libicsneo carries nanoseconds; so does FW2. The EPOCH differs
			// though -- ICS devices count from 2007-01-01 while FW2's
			// time_stamp_ns is almost certainly since boot. Deltas and jitter
			// measurements are correct today; absolute wall-clock time is not.
			// TODO(bench): Phase 2 -- establish the FW2 epoch and offset here.
			msg->timestamp = report_.time_stamp_ns;

			dispatchMessage(msg);
		}
	}
}

void FW2Communication::drainTextResponses(OneWili& oneWili) {
	std::string line;
	while(oneWili.popTextResponse(line)) {
		if(!isResponseFrame(line)) {
			// Spontaneous text event (can0, can1, canTx0, ...). can0/can1 are
			// real CAN RX frames -- see tryParseCanTextEvent() for why this is
			// the RX path that actually works. Everything else (canTx echoes,
			// etc.) is not something this port consumes yet.
			std::shared_ptr<CANMessage> msg;
			if(tryParseCanTextEvent(line, msg))
				dispatchMessage(msg);
			continue;
		}

		{
			std::scoped_lock<std::mutex> lk(syncMutex);
			if(syncWaiting && !syncResponseReady) {
				syncResponse = line;
				syncResponseReady = true;
				syncCv.notify_all();
				continue;
			}
		}

		// Otherwise it acknowledges one of our pipelined transmits.
		{
			// The mutex is held for the decrement even though inFlight is
			// atomic: without it, a decrement landing between a waiter's
			// predicate check and its wait is a lost wakeup.
			std::scoped_lock<std::mutex> lk(inFlightMutex);
			if(inFlight > 0)
				inFlight--;
		}
		// notify_all, not notify_one: a pipelined sender waiting for room and a
		// synchronous caller waiting for the pipe to drain both sleep here, and
		// waking only the first would strand the second.
		inFlightCv.notify_all();
	}
}

bool FW2Communication::sendOneWiliCommand(const std::string& command) {
	// Backpressure: without this a fast producer outruns the CDC endpoint and
	// the driver's write queue silently drops (or blocks unboundedly).
	{
		std::unique_lock<std::mutex> lk(inFlightMutex);
		if(!inFlightCv.wait_for(lk, std::chrono::milliseconds(500),
				[this]() { return inFlight.load() < MAX_IN_FLIGHT || closing; })) {
			report(APIEvent::Type::Timeout, APIEvent::Severity::Error);
			return false;
		}
		if(closing)
			return false;
	}

	std::scoped_lock<std::mutex> callLk(callMutex);

	inFlight++;
	// Held only across the write, so a synchronous caller waiting on callMutex
	// blocks for one command rather than for the whole backpressure window.
	// Taken BEFORE the increment: callOneWili() holds this while it waits for
	// inFlight to reach zero, and an increment on the far side of the lock
	// would be a count it could never see drain.
	if(!rawWrite(frameCommand(command))) {
		inFlight--;
		return false;
	}
	return true;
}

bool FW2Communication::callOneWili(const std::string& command, std::string& response,
		std::chrono::milliseconds timeout) {
	// One synchronous caller at a time: two in flight could not tell their
	// responses apart, since a OneWili response frame carries no request id we
	// can correlate on from here.
	std::scoped_lock<std::mutex> callLk(callMutex);

	// For the same reason, let the pipelined transmits already on the wire be
	// acknowledged before we register. Otherwise the next ack -- which belongs
	// to a CAN frame, not to us -- gets handed over as our response.
	{
		std::unique_lock<std::mutex> lk(inFlightMutex);
		if(!inFlightCv.wait_for(lk, timeout, [this]() { return inFlight.load() == 0 || closing; })) {
			report(APIEvent::Type::Timeout, APIEvent::Severity::Error);
			return false;
		}
		if(closing)
			return false;
	}

	{
		std::scoped_lock<std::mutex> lk(syncMutex);
		syncWaiting = true;
		syncResponseReady = false;
		syncResponse.clear();
	}

	if(!rawWrite(frameCommand(command))) {
		std::scoped_lock<std::mutex> lk(syncMutex);
		syncWaiting = false;
		return false;
	}

	std::unique_lock<std::mutex> lk(syncMutex);
	const bool got = syncCv.wait_for(lk, timeout, [this]() { return syncResponseReady; });
	syncWaiting = false;
	if(!got) {
		report(APIEvent::Type::Timeout, APIEvent::Severity::Error);
		return false;
	}
	response = syncResponse;
	return frameReportsSuccess(response);
}

bool FW2Communication::sendMessage(const std::shared_ptr<Frame>& frame) {
	const auto canmsg = std::dynamic_pointer_cast<CANMessage>(frame);
	if(!canmsg) {
		// FW2 is CAN only; Device::transmit()'s isSupportedTXNetwork check should
		// already have rejected anything else, so reaching here is a real bug.
		report(APIEvent::Type::UnsupportedTXNetwork, APIEvent::Severity::Error);
		return false;
	}

	if(canmsg->isRemote) {
		// OneWili's write command has no RTR parameter.
		report(APIEvent::Type::RTRNotSupported, APIEvent::Severity::Error);
		return false;
	}

	if(!lengthToDLC(canmsg->data.size(), canmsg->isCANFD)) {
		report(APIEvent::Type::MessageMaxLengthExceeded, APIEvent::Severity::Error);
		return false;
	}

	// Byte-for-byte what ow_io_canfd_write_canfd() would produce:
	//   i\c\w <channel> <arbid:8 hex> <fd> <xtd> <data bytes: 2 hex each>
	std::string command = "i\\c\\w";
	char scratch[32];

	std::snprintf(scratch, sizeof(scratch), " %d", static_cast<int>(channel));
	command += scratch;

	std::snprintf(scratch, sizeof(scratch), " %08lX",
		static_cast<unsigned long>(canmsg->arbid));
	command += scratch;

	command += canmsg->isCANFD ? " 1" : " 0";
	command += canmsg->isExtended ? " 1" : " 0";

	for(const uint8_t byte : canmsg->data) {
		std::snprintf(scratch, sizeof(scratch), " %02X", static_cast<unsigned>(byte));
		command += scratch;
	}

	return sendOneWiliCommand(command);
}

bool FW2Communication::sendCommand(Command cmd, std::vector<uint8_t> arguments) {
	(void)arguments;

	if(cmd == Command::RequestStatusUpdate) {
		// Hand off rather than answer here. The caller is Device's heartbeat
		// thread holding a mutex our dispatch would re-enter; see statusTask().
		{
			std::scoped_lock<std::mutex> lk(statusMutex);
			if(closing)
				return false;
			statusRequested = true;
		}
		statusCv.notify_one();
		// True means "the request was accepted", exactly as it does for an ICS
		// device whose write succeeded. Whether the board is alive is answered
		// by the status message arriving, not by this return value.
		return true;
	}

	// Every other Command in libicsneo's vocabulary is an ICS packet-protocol
	// command with no OneWili equivalent. Report rather than silently succeed:
	// a caller that thinks it configured something and did not is worse than an
	// explicit failure.
	report(APIEvent::Type::NotSupported, APIEvent::Severity::Error);
	return false;
}

bool FW2Communication::answerStatusRequest() {
	// Device's heartbeat closes the device if nothing answers within 3.5 s, so
	// this has to be both prompt and truthful.
	//
	// `h\t` (read the board RTC) rather than `i\g\u` (gpio_read_all): measured
	// on real hardware, `i\g\u` fails with EPOWERZONE unless power zone 6
	// (FPGA, 3V3_S6) is up, and that rail is off by default and has nothing to
	// do with CAN. A liveness probe that depends on an unrelated optional rail
	// reports a healthy board as dead. `h\t` is served by the board manager, is
	// always available, and at ~47 bytes is cheaper on the Full-Speed text
	// endpoint than the ~700-byte `h\p\g` alternative.
	std::string response;
	if(!callOneWili("h\\t", response, std::chrono::milliseconds(250)))
		return false;

	auto status = std::make_shared<ResetStatusMessage>();
	status->mainLoopTime = 0;
	status->maxMainLoopTime = 0;
	status->justReset = false;
	status->comEnabled = true;
	status->cmRunning = false;
	status->cmChecksumFailed = false;
	status->cmLicenseFailed = false;
	status->cmVersionMismatch = false;
	status->cmBootOff = false;
	status->hardwareFailure = false;
	status->usbComEnabled = true;
	status->linuxComEnabled = false;
	status->cmTooBig = false;
	status->hidUsbState = false;
	status->fpgaUsbState = false;
	// Left unset rather than zeroed: FW2 does not report vehicle bus voltage or
	// die temperature, and zero would read as a real measurement.
	status->busVoltage.reset();
	status->deviceTemperature.reset();

	dispatchMessage(status);
	return true;
}

bool FW2Communication::sendPacket(std::vector<uint8_t>& bytes) {
	(void)bytes;
	// Reaching here means something built an ICS packet for an FW2. Nothing in
	// this class does, and nothing should.
	report(APIEvent::Type::NotSupported, APIEvent::Severity::Error);
	return false;
}
