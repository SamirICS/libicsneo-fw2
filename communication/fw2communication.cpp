#include "icsneo/communication/fw2communication.h"
#include "icsneo/communication/message/canmessage.h"
#include "icsneo/communication/message/resetstatusmessage.h"
#include "icsneo/communication/command.h"
#include "icsneo/communication/network.h"
#include "icsneo/api/eventmanager.h"

#include "onewili_binary_framing.h"
#include "onewili_events.h"

#include <array>
#include <cstdio>
#include <optional>
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
		if(!isResponseFrame(line))
			continue; // Spontaneous text event (can0, canTx0, ...); not an ack

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
	// A frame ends in "ok" when the command succeeded; the firmware says so
	// explicitly rather than by omission.
	return response.find(" ok]") != std::string::npos;
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

	if(cmd == Command::RequestStatusUpdate)
		return answerStatusRequest();

	// Every other Command in libicsneo's vocabulary is an ICS packet-protocol
	// command with no OneWili equivalent. Report rather than silently succeed:
	// a caller that thinks it configured something and did not is worse than an
	// explicit failure.
	report(APIEvent::Type::NotSupported, APIEvent::Severity::Error);
	return false;
}

bool FW2Communication::answerStatusRequest() {
	// Device's heartbeat closes the device if nothing answers within 3.5 s, so
	// this has to be both prompt and truthful. `i\g\u` (gpio_read_all) is a
	// cheap round trip that fails exactly when the USB bridge is gone -- which
	// makes libicsneo's disconnect detection mean something real on FW2.
	std::string response;
	if(!callOneWili("i\\g\\u", response, std::chrono::milliseconds(250)))
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
