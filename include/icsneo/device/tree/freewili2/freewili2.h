#ifndef __FREEWILI2_H_
#define __FREEWILI2_H_

#ifdef __cplusplus

#include "icsneo/device/device.h"
#include "icsneo/device/devicetype.h"
#include "icsneo/device/extensions/deviceextension.h"
#include "icsneo/platform/onewili.h"
#include "icsneo/communication/fw2communication.h"
#include "icsneo/device/tree/freewili2/freewili2settings.h"

namespace icsneo {

/**
 * FreeWili 2 as an ordinary icsneo::Device.
 *
 * Almost all of the work is in FW2Communication; this class exists to swap that
 * Communication in and to stub out the four places Device assumes ICS firmware:
 * the bring-up handshake, network enable, and goOnline/goOffline.
 *
 * Everything else -- CoreMini, VSA, WiVI, LiveData, FlexRay, MACsec, Ethernet
 * PHY, logical disk -- inherits Device's existing "not supported" defaults. That
 * is not an oversight; libicsneo is already designed for sparse devices, and
 * VividCAN (58 lines) is the precedent.
 */
class FreeWili2 : public Device {
public:
	/**
	 * Serial numbers start with FW.
	 *
	 * The real FW2 serial is longer than libicsneo's 6-character identity, so
	 * OneWili::Find() derives "FW" + 4 base36 characters from a hash of it. That
	 * derived value is what appears here and in FindAllDevices().
	 *
	 * TODO(Phase 2): surface the full FW2 serial through getProductName().
	 * FoundDevice has no field to carry it from Find() to the constructor today,
	 * so rather than fabricate one this is left as a known gap.
	 */
	ICSNEO_FINDABLE_DEVICE(FreeWili2, DeviceType::FreeWili2, "FW");

	/**
	 * The OneWili CAN channel this device drives.
	 *
	 * The MVP advertises one channel and the fatherboard carries one MCP2518FD.
	 * OneWili does expose can1/canTx1 and a full CAN2 settings block, and the San
	 * Diego Orca expansion may populate a second controller, so this stays a
	 * named constant threaded through FW2Communication rather than a literal 0 --
	 * adding DWCAN_02 should be a config change, not a refactor.
	 */
	static constexpr uint8_t CAN_CHANNEL = 0;

	ProductID getProductID() const override { return ProductID::FreeWili2; }

	bool goOnline() override {
		if(isOnline())
			return true;

		if(!enableNetworkCommunication(true))
			return false;

		// Deliberately not Device::goOnline(): that path drives an LED state
		// command, a KeepAlive Periodic and the network-mutex client-ID
		// handshake, all three of which are ICS packet-protocol commands with no
		// OneWili equivalent. Calling it would work only by having
		// FW2Communication::sendCommand() silently swallow them, which is exactly
		// the kind of quiet lie this port is trying to avoid.
		online = true;
		forEachExtension([](const std::shared_ptr<DeviceExtension>& ext) { ext->onGoOnline(); return true; });
		return true;
	}

	bool goOffline() override {
		if(!isOnline())
			return true;

		forEachExtension([](const std::shared_ptr<DeviceExtension>& ext) { ext->onGoOffline(); return true; });

		// Stop the stream even if the board is gone, but do not fail on it --
		// a disconnected device is still legitimately offline afterwards.
		if(!isDisconnected())
			enableNetworkCommunication(false);

		online = false;
		return true;
	}

protected:
	FreeWili2(neodevice_t neodevice, const driver_factory_t& makeDriver) : Device(neodevice) {
		initialize<FW2Settings>(makeDriver);
	}

	std::shared_ptr<Communication> makeCommunication(
		std::unique_ptr<Driver> t,
		std::function<std::unique_ptr<Packetizer>()> makeConfiguredPacketizer,
		std::unique_ptr<Encoder> e,
		std::unique_ptr<Decoder> d) override {
		return std::make_shared<FW2Communication>(report, std::move(t), makeConfiguredPacketizer,
			std::move(e), std::move(d), CAN_CHANNEL);
	}

	void setupSupportedRXNetworks(std::vector<Network>& rx) override {
		rx.emplace_back(Network::NetID::DWCAN_01);
	}

	void setupSupportedTXNetworks(std::vector<Network>& tx) override {
		tx.emplace_back(Network::NetID::DWCAN_01);
	}

	// FW2 is USB powered and carries no vehicle power sense.
	bool requiresVehiclePower() const override { return false; }

	// There is no ICS component-version table to read.
	bool supportsComponentVersions() const override { return false; }

	/**
	 * Replaces the ICS bring-up handshake.
	 *
	 * Device::attemptToBeginCommunication() asks the device for its serial and
	 * version table over the ICS packet protocol. FW2 answers none of that: the
	 * serial already came from the USB descriptor during Find(), and there is no
	 * version table.
	 *
	 * What it does still do is prove the board is actually talking, which matters
	 * -- an FW2 whose ports enumerate but whose firmware is wedged should fail
	 * open() rather than appear healthy. So this issues a real `i\g\u`
	 * (gpio_read_all) round trip and fails if it does not come back.
	 */
	APIEvent::Type attemptToBeginCommunication() override {
		auto fw2 = std::dynamic_pointer_cast<FW2Communication>(com);
		if(!fw2)
			return APIEvent::Type::NoDeviceResponse;

		std::string response;
		if(!fw2->callOneWili("i\\g\\u", response, std::chrono::milliseconds(1000))) {
			// Not getCommunicationNotEstablishedError(): that resolves to a
			// NoSerialNumber* variant, which would be actively misleading here.
			// We have the serial -- from the USB descriptor -- and the board is
			// simply not answering.
			return APIEvent::Type::NoDeviceResponse;
		}

		return APIEvent::Type::NoErrorFound;
	}

	/**
	 * Start or stop the CAN RX stream (`i\c\o <channel> <enabled>`).
	 *
	 * Device's version sends Command::EnableNetworkCommunication with a watchdog
	 * timeout. FW2 has no such watchdog, so the timeout argument is genuinely
	 * meaningless here rather than merely unimplemented -- the stream runs until
	 * it is told to stop or the USB link drops.
	 */
	bool enableNetworkCommunication(bool enable, uint32_t timeout = 0) override {
		(void)timeout;

		auto fw2 = std::dynamic_pointer_cast<FW2Communication>(com);
		if(!fw2)
			return false;

		std::string command = "i\\c\\o ";
		command += std::to_string(static_cast<int>(CAN_CHANNEL));
		command += enable ? " 1" : " 0";

		std::string response;
		return fw2->callOneWili(command, response, std::chrono::milliseconds(1000));
	}
};

}

#endif // __cplusplus

#endif
