#ifndef __FW2COMMUNICATION_H_
#define __FW2COMMUNICATION_H_

/**
 * Communication for FreeWili 2, which speaks OneWili rather than the ICS packet
 * protocol.
 *
 * This is the seam of the whole port. Device::initialize() builds the stack
 * through virtuals, so substituting this class replaces Packetizer + Encoder +
 * Decoder wholesale -- we translate at the *message* level rather than trying to
 * impersonate ICS framing at the byte level. Nothing here produces or consumes
 * an ICS packet.
 *
 *   RX   OneWili's binary WILI frames arrive on the driver's ring buffer (fed by
 *        the FTDI port). rxTask() parses them with the vendored
 *        ow_bin_parser, decodes canRxReport, and dispatches CANMessages.
 *
 *   TX   sendMessage() formats OneWili's exact `i\c\w` command text and writes
 *        it WITHOUT waiting for the response. This is the one place we
 *        deliberately do not reuse upstream's `ow__call`: that blocks for a
 *        round trip per frame, which caps TX at far below a saturated bus.
 *        Acks are reconciled asynchronously in rxTask(). See the throughput
 *        note on MAX_IN_FLIGHT below.
 *
 *   Heartbeat  Device's heartbeat thread sends Command::RequestStatusUpdate
 *        every 110 ms and closes the device if nothing answers. sendCommand()
 *        answers it with a synthetic ResetStatusMessage backed by a real
 *        `i\g\u` (gpio_read_all) probe, so libicsneo's disconnect detection
 *        stays live and actually means something on FW2.
 */

#ifdef __cplusplus

#include "icsneo/communication/communication.h"
#include "icsneo/platform/onewili.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace icsneo {

class FW2Communication : public Communication {
public:
	// Both defined in the .cpp: BinaryParser is incomplete here (see the
	// binaryParser member below), and a unique_ptr to an incomplete type needs
	// its owner's constructor and destructor where the type IS complete.
	FW2Communication(
		device_eventhandler_t report,
		std::unique_ptr<Driver>&& driver,
		std::function<std::unique_ptr<Packetizer>()> makeConfiguredPacketizer,
		std::unique_ptr<Encoder>&& encoder,
		std::unique_ptr<Decoder>&& decoder,
		uint8_t channel);
	~FW2Communication() override;

	void spawnThreads() override;
	void joinThreads() override;

	bool sendMessage(const std::shared_ptr<Frame>& frame) override;
	bool sendCommand(Command cmd, std::vector<uint8_t> arguments = {}) override;
	bool sendPacket(std::vector<uint8_t>& bytes) override;

	/**
	 * Write one OneWili command and return without waiting for its response.
	 * The ack is reconciled by rxTask(). Use for anything on the hot path.
	 */
	bool sendOneWiliCommand(const std::string& command);

	/**
	 * Write one OneWili command and wait for its response frame. Use for
	 * settings and bring-up, never on the CAN TX path.
	 *
	 * Blocks the pipelined path for its duration so a response cannot be
	 * mistaken for a TX ack.
	 */
	bool callOneWili(const std::string& command, std::string& response,
		std::chrono::milliseconds timeout = std::chrono::milliseconds(500));

	/**
	 * The OneWili CAN channel this device drives.
	 *
	 * The MVP advertises exactly one channel and the fatherboard carries one
	 * MCP2518FD, but this is a parameter rather than a constant throughout so
	 * that a second controller -- FW2's own can1, or one added by the San Diego
	 * Orca expansion -- is a config change and not a refactor.
	 */
	uint8_t getChannel() const { return channel; }

private:
	const uint8_t channel;

	std::thread rxThread;
	void rxTask();

	/**
	 * How many CAN transmit commands may be outstanding (written, ack not yet
	 * seen) at once.
	 *
	 * This is the pipelining depth, and it is the entire TX throughput story.
	 * At 1 we degrade to upstream's blocking `ow__call`; higher values fill the
	 * USB pipe instead of waiting on round trips. It cannot exceed the driver's
	 * write queue usefully, and too high just moves the queue from our side to
	 * the kernel's, hurting latency without helping rate.
	 *
	 * TODO(bench): Phase 0 measures the real ceiling. A saturated 500 kbit bus
	 * needs ~3,800 frames/s; a ~33-byte command on a 64-byte Full-Speed CDC
	 * endpoint gives ~1,900/s even perfectly pipelined. Expect this port to be
	 * comfortable for monitoring, request-response diagnostics and periodic TX,
	 * and roughly 2x short for saturated bulk transmit.
	 */
	static constexpr size_t MAX_IN_FLIGHT = 16;
	std::atomic<size_t> inFlight{0};
	std::mutex inFlightMutex;
	std::condition_variable inFlightCv;

	// Synchronous-call handoff. rxTask() owns the text response queue, so a
	// blocking caller registers here and rxTask() hands the line over.
	std::mutex callMutex;        // Serializes synchronous callers against each other
	std::mutex syncMutex;        // Guards the fields below
	std::condition_variable syncCv;
	bool syncWaiting = false;
	std::string syncResponse;
	bool syncResponseReady = false;

	/** Drain the driver's text response queue: acks, or a pending sync reply. */
	void drainTextResponses(OneWili& driver);

	/** Parse whatever binary frames are buffered and dispatch them. */
	void drainBinaryFrames();

	/** Send Device's heartbeat a synthetic status backed by a real liveness probe. */
	bool answerStatusRequest();

	// Opaque so the vendored C parser does not leak into every translation unit
	// that includes this header.
	struct BinaryParser;
	std::unique_ptr<BinaryParser> binaryParser;
};

}

#endif // __cplusplus

#endif
