#ifndef __FREEWILI2SETTINGS_H_
#define __FREEWILI2SETTINGS_H_

#include <stdint.h>
#include "icsneo/device/idevicesettings.h"

#ifdef __cplusplus

namespace icsneo {

#endif

/**
 * FreeWili 2 settings, in libicsneo's shape rather than the device's.
 *
 * ICS devices store a single packed GLOBAL_SETTINGS blob in EEPROM and libicsneo
 * reads and writes it whole. FW2 has no such blob -- its settings are individual
 * OneWili menu items (`h\s\p\a` CAN1 Mode, `h\s\p\b` CAN1 Rate, `h\s\p\c` CAN1 FD
 * Data Rate, `h\s\p\y` Listen Only, `h\s\p\e` TX Retry, `h\s\p\1` Termination,
 * and the raw `h\s\p\f` / `h\s\p\g` C1NBTCFG / C1DBTCFG register strings).
 *
 * So this struct is not a wire format; it is the *staging buffer* the base class
 * edits. That is what makes the mapping cheap: IDeviceSettings::getBaudrateFor()
 * and setBaudrateFor() work purely on this in-memory struct through the
 * getCANSettingsFor() / getCANFDSettingsFor() accessors below. Only refresh()
 * and apply() ever touch the device, and only those two need OneWili.
 *
 * Consequently `icsneo::Device::settings->setBaudrateFor(DWCAN_01, 500000)` --
 * the call firmio_uds_flasher already makes -- needs no FW2-specific code at all
 * once apply() lands.
 */
#pragma pack(push, 2)
typedef struct {
	CAN_SETTINGS can1;       // 12 bytes
	CANFD_SETTINGS canfd1;   // 10 bytes
	uint16_t termination_enables;
} fw2_settings_t;
#pragma pack(pop)

#ifdef __cplusplus

static_assert(sizeof(fw2_settings_t) == 24, "FreeWili 2 settings size mismatch");

/**
 * NOT YET IMPLEMENTED -- this is the Phase 3 skeleton.
 *
 * The accessors below are complete and correct; refresh() and apply() are not,
 * and they say so rather than pretending. refresh() deliberately does NOT
 * synthesize plausible defaults: reporting "500 kbit" for a board actually
 * running at 125 kbit would be worse than reporting "unknown", and callers
 * already handle a failed refresh (Device::open() downgrades and ignores it).
 */
class FW2Settings : public IDeviceSettings {
public:
	FW2Settings(Device* device) : IDeviceSettings(device, sizeof(fw2_settings_t)) {}

	const CAN_SETTINGS* getCANSettingsFor(Network net) const override {
		auto cfg = getStructurePointer<fw2_settings_t>();
		if(cfg == nullptr)
			return nullptr;
		switch(net.getNetID()) {
			case Network::NetID::DWCAN_01:
				return &(cfg->can1);
			default:
				return nullptr;
		}
	}

	const CANFD_SETTINGS* getCANFDSettingsFor(Network net) const override {
		auto cfg = getStructurePointer<fw2_settings_t>();
		if(cfg == nullptr)
			return nullptr;
		switch(net.getNetID()) {
			case Network::NetID::DWCAN_01:
				return &(cfg->canfd1);
			default:
				return nullptr;
		}
	}

	std::vector<TerminationGroup> getTerminationGroups() const override {
		// One group, one network: FW2's termination is a per-channel software
		// switch, so there is no "only one of these may be terminated" tradeoff
		// to express here the way there is on multi-channel ICS hardware.
		return {
			{ Network(Network::NetID::DWCAN_01) }
		};
	}

	bool refresh() override {
		// TODO(Phase 3): read the `h\s\p\*` items back over OneWili and populate
		// settings / settingsInDeviceRAM, then set settingsLoaded = true. Until
		// then we report that we do not know, rather than guessing.
		report(APIEvent::Type::SettingsNotAvailable, APIEvent::Severity::EventWarning);
		return false;
	}

	bool apply(bool temporary = false) override {
		(void)temporary;
		// TODO(Phase 3): translate the staged struct into OneWili menu writes.
		// Note `temporary` has no FW2 analogue -- OneWili settings are applied
		// immediately and persist -- so the volatile/EEPROM distinction will have
		// to be either emulated or reported as unsupported.
		report(APIEvent::Type::NotSupported, APIEvent::Severity::Error);
		return false;
	}
};

}

#endif // __cplusplus

#endif
