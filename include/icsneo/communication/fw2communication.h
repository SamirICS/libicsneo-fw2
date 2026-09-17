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
 *        hands that to statusTask(), which answers with a synthetic
 *        ResetStatusMessage backed by a real `h\t` probe, so libicsneo's
 *        disconnect detection stays live and actually means something on FW2.
 *        The handoff is not an optimisation -- see statusTask() for why
 *        answering inline deadlocks.
 *
 *   Bus state  FW2 emits no error event of any kind, so busStateTask() polls
 *        the controller's own C1TREC / C1BDIAG1 registers and dispatches
 *        CANErrorMessages on change. See busStateTask() for why this is polled
 *        rather than pushed.
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
	 * Read `count` consecutive 32-bit CAN controller registers starting at
	 * `address`, through OneWili's `i\c\r`.
	 *
	 * Addresses are MCP2518FD SFR offsets -- C1CON at 0x000, C1TREC at 0x01C --
	 * and are formatted as the 8 hex digits the vendored client uses
	 * (third-party/onewili/src/onewili.c, ow_io_canfd_read_can_registers).
	 *
	 * Public deliberately. These registers are the only window this port has onto
	 * what the controller actually thinks is happening, so bring-up and bench
	 * tools need them directly rather than through a message.
	 *
	 * Returns false if the command fails OR if it succeeds without returning
	 * register data -- see busStateTask() for why that distinction matters.
	 */
	bool readCANRegisters(uint32_t address, size_t count, std::vector<uint32_t>& out,
		std::chrono::milliseconds timeout = std::chrono::milliseconds(300));

	/**
	 * Start or stop polling CAN bus state.
	 *
	 * While enabled, TEC/REC and the error flags are read periodically and any
	 * change is dispatched as a CANErrorMessage, which is what makes bus-off,
	 * error-passive and a rising ACK-error count visible to an ordinary libicsneo
	 * application. Driven by FreeWili2::enableNetworkCommunication(), so it
	 * follows goOnline()/goOffline().
	 */
	void setBusStatePolling(bool enable);

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

	/**
	 * Answers Command::RequestStatusUpdate, off the requesting thread.
	 *
	 * This thread exists for correctness, not for speed. Device's heartbeat
	 * calls sendCommand() while holding a mutex that its own message callback
	 * takes (device.cpp: `recvLk` on `receivedMessageMutex`, taken before
	 * sendCommand and released only inside the wait_for that follows). On an
	 * ICS device that is safe because sendCommand() merely writes bytes and the
	 * status arrives later on the read thread. If FW2 answers inline instead,
	 * dispatchMessage() runs that callback on the heartbeat thread itself and
	 * it self-deadlocks on a non-recursive mutex -- while holding
	 * Communication::messageCallbacksLock, which then wedges every other
	 * thread that touches the callback list. Observed on hardware: the run
	 * survives the heartbeat but hangs forever in removeMessageCallback().
	 *
	 * So sendCommand() posts here and returns, matching the threading contract
	 * upstream's own devices keep.
	 */
	std::thread statusThread;
	void statusTask();
	std::mutex statusMutex;
	std::condition_variable statusCv;
	// Deliberately a flag, not a count: requests that pile up while a probe is
	// in flight coalesce into one. The heartbeat wants a recent answer, not one
	// answer per ask.
	bool statusRequested = false;

	/** Send Device's heartbeat a synthetic status backed by a real liveness probe. */
	bool answerStatusRequest();

	/**
	 * Polls CAN bus state and dispatches CANErrorMessages.
	 *
	 * FW2 has no spontaneous error event. `i\c\o` is documented as streaming
	 * "received CAN frames and errors", but the only error signal that actually
	 * reaches us is a single bit in the WILI frame header of a canRxReport --
	 * which says one received frame was bad and says nothing about TEC, REC or
	 * bus-off. OneWili exposes no `canError` text event either; the full event
	 * list in third-party/onewili/include/onewili.h has none.
	 *
	 * So bus state has to be pulled out of the controller's own registers. That
	 * is a polled round trip rather than a push, which is why this is its own
	 * thread: it must not stall the RX path, the TX pipeline, or the heartbeat.
	 *
	 * Without this, a board sitting in bus-off is indistinguishable from an idle
	 * bus -- both produce exactly nothing. That is not a hypothetical; it is what
	 * made the "no frames received" bring-up problem hard to localise.
	 */
	std::thread busStateThread;
	void busStateTask();
	std::mutex busStateMutex;
	std::condition_variable busStateCv;
	bool busStatePolling = false;
	// Set when something already suspects trouble -- an errored canRxReport --
	// so the next poll happens immediately rather than up to a period later.
	bool busStateDue = false;
	/**
	 * Cleared the first time `i\c\r` answers without register data.
	 *
	 * Measured on hardware: `i\c\r` returns a success frame carrying no body.
	 * That may be firmware that does not implement the read, so rather than
	 * burning a round trip every period forever, the poller reports once and
	 * stops. Reporting "no bus state available" beats silently pretending the
	 * bus is healthy.
	 */
	bool busStateSupported = true;
	bool haveLastBusState = false;
	uint32_t lastTREC = 0;
	uint32_t lastBDIAG1 = 0;

	// Opaque so the vendored C parser does not leak into every translation unit
	// that includes this header.
	struct BinaryParser;
	std::unique_ptr<BinaryParser> binaryParser;
};

}

#endif // __cplusplus

#endif
