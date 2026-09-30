// fw2_ports -- diagnostic dump of every SetupAPI "Ports"-class USB device.
// Standalone (no icsneocpp) so it isolates whether OneWili::Find()'s
// enumeration approach even sees the FreeWili's COM ports on this machine,
// independent of any device-matching logic.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <usbiodef.h>
#include <devpkey.h>

#include <cstdio>
#include <string>

namespace {

std::wstring getWideProperty(HDEVINFO set, SP_DEVINFO_DATA* data, const DEVPROPKEY& key) {
	DWORD type = 0;
	DWORD bytes = 0;
	SetupDiGetDevicePropertyW(set, data, &key, &type, nullptr, 0, &bytes, 0);
	if(bytes == 0)
		return {};
	std::wstring value(bytes / sizeof(wchar_t), L'\0');
	if(!SetupDiGetDevicePropertyW(set, data, &key, &type,
			reinterpret_cast<PBYTE>(value.data()), bytes, &bytes, 0))
		return {};
	while(!value.empty() && value.back() == L'\0')
		value.pop_back();
	return value;
}

std::wstring getPortName(HDEVINFO set, SP_DEVINFO_DATA* data) {
	DWORD type = 0;
	DWORD bytes = 0;
	SetupDiGetCustomDevicePropertyW(set, data, L"PortName", 0, &type, nullptr, 0, &bytes);
	if(bytes == 0)
		return {};
	std::wstring value(bytes / sizeof(wchar_t), L'\0');
	if(!SetupDiGetCustomDevicePropertyW(set, data, L"PortName", 0, &type,
			reinterpret_cast<PBYTE>(value.data()), bytes, &bytes))
		return {};
	while(!value.empty() && value.back() == L'\0')
		value.pop_back();
	return value;
}

std::wstring getInstanceId(HDEVINFO set, SP_DEVINFO_DATA* data) {
	DWORD chars = 0;
	SetupDiGetDeviceInstanceIdW(set, data, nullptr, 0, &chars);
	if(chars == 0)
		return {};
	std::wstring value(chars, L'\0');
	if(!SetupDiGetDeviceInstanceIdW(set, data, value.data(), chars, &chars))
		return {};
	while(!value.empty() && value.back() == L'\0')
		value.pop_back();
	return value;
}

} // namespace

int main() {
	HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_DEVICE, nullptr, nullptr,
		DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if(set == INVALID_HANDLE_VALUE) {
		printf("SetupDiGetClassDevsW failed: %lu\n", GetLastError());
		return 1;
	}

	printf("Every device SetupAPI reports via GUID_DEVINTERFACE_USB_DEVICE:\n\n");

	SP_DEVINFO_DATA data = {};
	data.cbSize = sizeof(data);
	int total = 0, ports = 0;
	for(DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &data); ++i) {
		++total;
		const std::wstring cls = getWideProperty(set, &data, DEVPKEY_Device_Class);
		const std::wstring instanceId = getInstanceId(set, &data);
		const std::wstring port = getPortName(set, &data);

		wprintf(L"[%2d] class=%-8s instance=%-55s port=%s\n",
			i, cls.empty() ? L"(none)" : cls.c_str(),
			instanceId.empty() ? L"(none)" : instanceId.c_str(),
			port.empty() ? L"(n/a)" : port.c_str());

		if(cls == L"Ports")
			++ports;
	}

	printf("\n%d device(s) total, %d with class \"Ports\".\n", total, ports);
	SetupDiDestroyDeviceInfoList(set);
	return 0;
}
