#ifndef __ONEWILI_H_
#define __ONEWILI_H_

/**
 * Driver for FreeWili 2 (FW2) hardware, which speaks the OneWili protocol
 * rather than the ICS packet protocol.
 *
 * FW2 presents *two* USB serial ports, and they are not two interfaces of one
 * USB device -- they are two separate USB devices behind the board's internal
 * hub:
 *
 *   text port    CDC ACM, VID 0x093C, product string like "FW2 v01".
 *                Bidirectional. Carries every OneWili command and its response,
 *                including CAN transmit.
 *
 *   binary port  FTDI (VID 0x0403). **Read-only** -- upstream's own
 *                `ow_binary_open` only requires a read callback. Carries the
 *                84-byte `canRxReport` event stream.
 *
 * So this driver owns two file handles. The mapping onto the Driver base:
 *
 *   write()          -> text port. This is the OneWili command channel.
 *   pushRx()/ring    -> binary port. This is the asynchronous message stream,
 *                       which is what FW2Communication's RX thread consumes.
 *   textResponses    -> text port replies, kept in a separate queue so the
 *                       pipelined TX path can reconcile acks without them
 *                       polluting the message stream.
 *
 * Note the ring buffer is NOT fed to a Packetizer: FW2Communication overrides
 * spawnThreads() and never runs Communication::readTask(). Nothing in this
 * driver produces or consumes ICS framing.
 *
 * ==> WARNING: FreeWili uses VID 0x093C, which is Intrepid's own
 *     INTREPID_USB_VENDOR_ID. CDCACM::Find() filters on that VID alone, so it
 *     will happily claim an FW2's text port. Both finders therefore
 *     discriminate on the USB product string; see IsFreeWiliProduct().
 */

#ifdef __cplusplus

#include "icsneo/communication/driver.h"
#include "icsneo/device/neodevice.h"
#include "icsneo/api/eventmanager.h"
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

// Shared with Intrepid -- see the warning above.
#define FREEWILI_USB_VENDOR_ID (0x093c)
// The FW2 binary event port enumerates as an FTDI device.
#define FTDI_USB_VENDOR_ID (0x0403)

namespace icsneo {

class OneWili : public Driver {
public:
	OneWili(const device_eventhandler_t& err, neodevice_t& forDevice) : Driver(err), device(forDevice) {}
	~OneWili() override;

	static void Find(std::vector<FoundDevice>& found);

	/**
	 * True if a USB product string belongs to a FreeWili board.
	 *
	 * Used in two directions: OneWili::Find() requires it, and CDCACM::Find()
	 * rejects it, so a single FW2 is claimed by exactly one driver. Matching on
	 * the product string rather than the PID is deliberate -- FW2 PIDs vary per
	 * variant and we do not have the full list.
	 *
	 * TODO(bench): get the authoritative VID/PID list from the FreeWili team and
	 * switch to a PID check, which is cheaper and cannot be defeated by a
	 * firmware that changes its product string.
	 */
	static bool IsFreeWiliProduct(const std::string& product);

	bool open() override;
	bool isOpen() override;
	bool close() override;
	driver_finder_t getFinder() override { return OneWili::Find; }

	/**
	 * Pop one complete response line from the text port, or return false if none
	 * is buffered. Never blocks. FW2Communication uses this to reconcile acks for
	 * commands it wrote without waiting -- the whole point of not routing TX
	 * through OneWili's blocking `ow__call`.
	 */
	bool popTextResponse(std::string& line);

private:
	neodevice_t& device;

	// The two ports. Handle type differs by platform; everything else does not,
	// which is why this driver has one header rather than the posix/windows pair
	// the ICS drivers use.
#ifdef _WIN32
	HANDLE textHandle = INVALID_HANDLE_VALUE;
	HANDLE binaryHandle = INVALID_HANDLE_VALUE;
#else
	int textFd = -1;
	int binaryFd = -1;
#endif

	std::thread binaryReadThread, textReadThread, writeThread;
	void binaryReadTask(); // FTDI port -> pushRx() -> ring buffer
	void textReadTask();   // CDC port -> textResponses
	void writeTask();      // writeQueue -> CDC port

	std::mutex textResponseMutex;
	std::deque<std::string> textResponses;
	std::string textAccumulator;
	// A runaway device must not be able to grow this without bound.
	static constexpr size_t MAX_TEXT_RESPONSES = 256;

	/**
	 * FoundDevice carries a single integer handle, so the two TTY indices are
	 * packed into it: text index in bits 0-7, binary index in bits 8-15, and a
	 * "binary port present" flag in bit 16. An FW2 whose FTDI port is bound to
	 * some other driver still opens, it just receives nothing -- which is a far
	 * better failure than refusing to enumerate.
	 *
	 * Like CDCACM's handle, this goes stale if the device is replugged; re-run
	 * Find() in that case.
	 */
	static constexpr neodevice_handle_t HANDLE_OFFSET = 0x01000000;
	static constexpr neodevice_handle_t BINARY_PRESENT_BIT = 0x00010000;
	static neodevice_handle_t PackHandle(uint8_t textIndex, uint8_t binaryIndex, bool hasBinary) {
		return HANDLE_OFFSET | textIndex | (neodevice_handle_t(binaryIndex) << 8) |
			(hasBinary ? BINARY_PRESENT_BIT : 0);
	}
	static std::string HandleToTextPort(neodevice_handle_t handle);
	static std::string HandleToBinaryPort(neodevice_handle_t handle); // empty if absent
};

}

#endif // __cplusplus

#endif
