#include "icsneo/platform/onewili.h"
#include "icsneo/device/founddevice.h"

#include <setupapi.h>
#include <initguid.h>
#include <usbiodef.h>
#include <devpkey.h>
#include <devguid.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

using namespace icsneo;

namespace {

static constexpr size_t WSTRING_ELEMENT_SIZE = sizeof(std::wstring::value_type);
static constexpr uint16_t FREEWILI_FW2_PID = 0x205a;

struct WindowsPort {
	std::wstring path;
	std::wstring portName;
	std::string instanceId;
	std::string serial;
	std::string product;
	uint16_t vid = 0;
	uint16_t pid = 0;
};

struct PortPair {
	std::wstring textPath;
	std::wstring binaryPath;
};

std::mutex handleMapMutex;
std::map<neodevice_handle_t, PortPair> handleMap;
neodevice_handle_t nextHandle = 0x01100000;

std::string wideToUtf8(const std::wstring& value) {
	if(value.empty())
		return {};
	const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if(size <= 1)
		return {};
	std::string out(static_cast<size_t>(size - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), size, nullptr, nullptr);
	return out;
}

std::string upper(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::toupper(c));
	});
	return value;
}

bool parseHexAfter(const std::string& text, const std::string& marker, uint16_t& out) {
	const std::string haystack = upper(text);
	const auto pos = haystack.find(marker);
	if(pos == std::string::npos)
		return false;
	try {
		out = static_cast<uint16_t>(std::stoul(haystack.substr(pos + marker.size(), 4), nullptr, 16));
		return true;
	} catch(...) {
		return false;
	}
}

std::string parseSerial(const std::string& instanceId) {
	const std::string id = upper(instanceId);
	const auto vidPos = id.find("VID_");
	if(vidPos == std::string::npos)
		return {};

	const auto usbSlash = id.rfind('\\');
	if(id.rfind("USB\\", 0) == 0 && usbSlash != std::string::npos) {
		// Composite children ('...&MI_00\6&xxxx&0&0000') carry no serial; only a
		// plain 'USB\VID_x&PID_y\SERIAL' node does. Reject the locally-unique form.
		const std::string tail = id.substr(usbSlash + 1);
		if(id.find("&MI_", vidPos) == std::string::npos && tail.find('&') == std::string::npos)
			return tail;
		return {};
	}

	const auto ftdiPid = id.find("PID_", vidPos);
	if(ftdiPid != std::string::npos) {
		const auto serialStart = id.find('+', ftdiPid);
		if(serialStart != std::string::npos) {
			const auto serialEnd = id.find('\\', serialStart + 1);
			return id.substr(serialStart + 1, serialEnd - serialStart - 1);
		}
	}

	return {};
}

/**
 * A device node's own instance ID for a Ports-class child is often a
 * composite MI_xx interface with no VID/PID/serial of its own (just a
 * locally-unique suffix). DEVPKEY_Device_Parent gives the parent's instance
 * ID directly as a string -- no second device-info set needed -- and that
 * parent is exactly the 'USB\VID_x&PID_y\SERIAL' node identity carries.
 */
std::string getParentInstanceId(HDEVINFO set, SP_DEVINFO_DATA* data) {
	DWORD type = 0;
	DWORD bytes = 0;
	SetupDiGetDevicePropertyW(set, data, &DEVPKEY_Device_Parent, &type, nullptr, 0, &bytes, 0);
	if(bytes == 0)
		return {};
	std::wstring value(bytes / WSTRING_ELEMENT_SIZE, L'\0');
	if(!SetupDiGetDevicePropertyW(set, data, &DEVPKEY_Device_Parent, &type,
			reinterpret_cast<PBYTE>(value.data()), bytes, &bytes, 0))
		return {};
	while(!value.empty() && value.back() == L'\0')
		value.pop_back();
	return wideToUtf8(value);
}

std::string rootSerial(const std::string& serial) {
	std::string out = upper(serial);
	while(!out.empty() && std::isalpha(static_cast<unsigned char>(out.back())))
		out.pop_back();
	return out.empty() ? upper(serial) : out;
}

std::wstring getWideProperty(HDEVINFO set, SP_DEVINFO_DATA* data, const DEVPROPKEY& key) {
	DWORD type = 0;
	DWORD bytes = 0;
	SetupDiGetDevicePropertyW(set, data, &key, &type, nullptr, 0, &bytes, 0);
	if(bytes == 0)
		return {};

	std::wstring value(bytes / WSTRING_ELEMENT_SIZE, L'\0');
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

	std::wstring value(bytes / WSTRING_ELEMENT_SIZE, L'\0');
	if(!SetupDiGetCustomDevicePropertyW(set, data, L"PortName", 0, &type,
			reinterpret_cast<PBYTE>(value.data()), bytes, &bytes))
		return {};
	while(!value.empty() && value.back() == L'\0')
		value.pop_back();
	return value;
}

std::string getDeviceInstanceId(HDEVINFO set, SP_DEVINFO_DATA* data) {
	DWORD chars = 0;
	SetupDiGetDeviceInstanceIdW(set, data, nullptr, 0, &chars);
	if(chars == 0)
		return {};

	std::wstring value(chars, L'\0');
	if(!SetupDiGetDeviceInstanceIdW(set, data, value.data(), chars, &chars))
		return {};
	while(!value.empty() && value.back() == L'\0')
		value.pop_back();
	return wideToUtf8(value);
}

std::vector<WindowsPort> enumeratePorts() {
	std::vector<WindowsPort> ports;
	// GUID_DEVCLASS_PORTS is the *setup class* for every COM port on the system,
	// USB or not. GUID_DEVINTERFACE_USB_DEVICE (a device *interface* class) only
	// covers the top-level USB node, which for a composite device is a different,
	// separate PnP node from the one Windows actually assigns "Ports" to and
	// hands a COM number -- so it silently misses FreeWili's two ports entirely
	// while happening to still find single-node devices like the ValueCAN4.
	HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
	if(set == INVALID_HANDLE_VALUE) {
		EventManager::GetInstance().add(APIEvent::Type::SyscallError, APIEvent::Severity::Error);
		return ports;
	}

	SP_DEVINFO_DATA data = {};
	data.cbSize = sizeof(data);
	for(DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &data); ++i) {
		WindowsPort port;
		port.portName = getPortName(set, &data);
		if(port.portName.empty() || port.portName.rfind(L"COM", 0) != 0)
			continue;
		port.path = L"\\\\.\\" + port.portName;

		port.instanceId = getDeviceInstanceId(set, &data);
		if(!parseHexAfter(port.instanceId, "VID_", port.vid)) {
			// Composite MI_xx child with no VID of its own -- try the parent node,
			// which is the 'USB\VID_x&PID_y\SERIAL' identity we actually want.
			const std::string parentId = getParentInstanceId(set, &data);
			if(!parentId.empty() && parseHexAfter(parentId, "VID_", port.vid))
				port.instanceId = parentId;
		}
		if(port.vid == 0)
			continue;
		parseHexAfter(port.instanceId, "PID_", port.pid);
		port.serial = parseSerial(port.instanceId);

		std::ostringstream product;
		product << wideToUtf8(getWideProperty(set, &data, DEVPKEY_Device_BusReportedDeviceDesc)) << ' '
			<< wideToUtf8(getWideProperty(set, &data, DEVPKEY_Device_FriendlyName)) << ' '
			<< wideToUtf8(getWideProperty(set, &data, DEVPKEY_Device_Manufacturer));
		port.product = product.str();

		ports.push_back(std::move(port));
	}

	SetupDiDestroyDeviceInfoList(set);
	return ports;
}

std::string deriveIcsneoSerial(const std::string& fw2Serial) {
	uint32_t hash = 2166136261u;
	for(const char c : fw2Serial) {
		hash ^= static_cast<uint8_t>(c);
		hash *= 16777619u;
	}
	static constexpr char DIGITS[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	std::string out = "FW";
	for(int i = 0; i < 4; i++) {
		out.push_back(DIGITS[hash % 36]);
		hash /= 36;
	}
	return out;
}

bool configureSerial(HANDLE handle, bool textPort) {
	COMMTIMEOUTS timeouts = {};
	timeouts.ReadIntervalTimeout = MAXDWORD;
	timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
	timeouts.ReadTotalTimeoutConstant = MAXDWORD - 1;
	timeouts.WriteTotalTimeoutMultiplier = 0;
	timeouts.WriteTotalTimeoutConstant = 0;
	if(!SetCommTimeouts(handle, &timeouts))
		return false;

	DCB state = {};
	state.DCBlength = sizeof(state);
	if(!GetCommState(handle, &state))
		return false;
	state.BaudRate = 1000000;
	state.ByteSize = 8;
	state.Parity = NOPARITY;
	state.StopBits = ONESTOPBIT;
	state.fBinary = TRUE;
	state.fDtrControl = textPort ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
	state.fRtsControl = RTS_CONTROL_DISABLE;
	state.fOutxCtsFlow = FALSE;
	state.fOutxDsrFlow = FALSE;
	state.fOutX = FALSE;
	state.fInX = FALSE;
	if(!SetCommState(handle, &state))
		return false;

	PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
	return true;
}

HANDLE openSerial(const std::wstring& path, bool textPort) {
	HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
		OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
	if(handle == INVALID_HANDLE_VALUE)
		return INVALID_HANDLE_VALUE;
	if(!configureSerial(handle, textPort)) {
		CloseHandle(handle);
		return INVALID_HANDLE_VALUE;
	}
	return handle;
}

std::wstring lookupPath(neodevice_handle_t handle, bool binary) {
	std::scoped_lock<std::mutex> lock(handleMapMutex);
	const auto it = handleMap.find(handle);
	if(it == handleMap.end())
		return {};
	return binary ? it->second.binaryPath : it->second.textPath;
}

} // namespace

void OneWili::Find(std::vector<FoundDevice>& found) {
	std::vector<WindowsPort> textPorts;
	std::vector<WindowsPort> binaryPorts;
	for(auto& port : enumeratePorts()) {
		if(port.vid == FREEWILI_USB_VENDOR_ID &&
				(IsFreeWiliProduct(port.product) || port.pid == FREEWILI_FW2_PID))
			textPorts.push_back(std::move(port));
		else if(port.vid == FTDI_USB_VENDOR_ID)
			binaryPorts.push_back(std::move(port));
	}

	std::set<std::string> claimedBinary;
	std::set<std::string> usedSerials;
	for(const auto& text : textPorts) {
		const WindowsPort* binary = nullptr;
		const std::string textRoot = rootSerial(text.serial);
		for(const auto& candidate : binaryPorts) {
			if(claimedBinary.count(wideToUtf8(candidate.portName)))
				continue;
			if(rootSerial(candidate.serial) == textRoot) {
				binary = &candidate;
				break;
			}
		}
		if(!binary && textPorts.size() == 1 && binaryPorts.size() == 1)
			binary = &binaryPorts[0];

		std::string serial = deriveIcsneoSerial(text.serial.empty() ? wideToUtf8(text.portName) : text.serial);
		if(!usedSerials.insert(serial).second)
			continue;

		neodevice_handle_t handle;
		{
			std::scoped_lock<std::mutex> lock(handleMapMutex);
			handle = nextHandle++;
			handleMap[handle] = { text.path, binary ? binary->path : std::wstring{} };
		}
		if(binary)
			claimedBinary.insert(wideToUtf8(binary->portName));

		FoundDevice foundDevice = {};
		foundDevice.handle = handle;
		foundDevice.productId = text.pid;
		foundDevice.serial[serial.copy(foundDevice.serial, sizeof(foundDevice.serial) - 1)] = '\0';
		foundDevice.makeDriver = [](const device_eventhandler_t& err, neodevice_t& device) {
			return std::unique_ptr<Driver>(new OneWili(err, device));
		};

		found.push_back(std::move(foundDevice));
	}
}

bool OneWili::IsFreeWiliProduct(const std::string& product) {
	const std::string name = upper(product);
	return name.rfind("FW", 0) == 0 || name.find("FREEWILI") != std::string::npos ||
		name.find("FREE-WILI") != std::string::npos;
}

OneWili::~OneWili() {
	if(isOpen())
		close();
}

bool OneWili::open() {
	if(isOpen()) {
		report(APIEvent::Type::DeviceCurrentlyOpen, APIEvent::Severity::Error);
		return false;
	}

	const std::string textPathString = HandleToTextPort(device.handle);
	if(textPathString.empty()) {
		report(APIEvent::Type::DriverFailedToOpen, APIEvent::Severity::Error);
		return false;
	}

	const std::wstring textPath = lookupPath(device.handle, false);
	textHandle = openSerial(textPath, true);
	if(textHandle == INVALID_HANDLE_VALUE) {
		report(APIEvent::Type::DriverFailedToOpen, APIEvent::Severity::Error);
		return false;
	}

	const std::wstring binaryPath = lookupPath(device.handle, true);
	if(!binaryPath.empty())
		binaryHandle = openSerial(binaryPath, false);
	if(binaryHandle == INVALID_HANDLE_VALUE)
		report(APIEvent::Type::NotSupported, APIEvent::Severity::EventWarning);

	readOverlapped = {};
	writeOverlapped = {};
	readOverlapped.hEvent = CreateEventA(nullptr, true, false, nullptr);
	writeOverlapped.hEvent = CreateEventA(nullptr, true, false, nullptr);
	if(!readOverlapped.hEvent || !writeOverlapped.hEvent) {
		close();
		report(APIEvent::Type::DriverFailedToOpen, APIEvent::Severity::Error);
		return false;
	}

	// The board's UART needs a moment after DTR asserts (openSerial() above) to
	// actually be ready; writing the reset byte immediately can lose it, which
	// then silently fails the h\t handshake in attemptToBeginCommunication().
	// Intermittent without this -- some opens land after the board is already
	// ready, some do not.
	std::this_thread::sleep_for(std::chrono::milliseconds(150));

	const uint8_t reset[] = { 0x02, '\n' };
	DWORD written = 0;
	if(!WriteFile(textHandle, reset, sizeof(reset), &written, &writeOverlapped) &&
			GetLastError() != ERROR_IO_PENDING) {
		close();
		report(APIEvent::Type::DriverFailedToOpen, APIEvent::Severity::Error);
		return false;
	}
	if(!GetOverlappedResult(textHandle, &writeOverlapped, &written, true) || written != sizeof(reset)) {
		close();
		report(APIEvent::Type::DriverFailedToOpen, APIEvent::Severity::Error);
		return false;
	}
	ResetEvent(writeOverlapped.hEvent);

	setIsClosing(false);
	setIsDisconnected(false);

	textReadThread = std::thread(&OneWili::textReadTask, this);
	writeThread = std::thread(&OneWili::writeTask, this);
	if(binaryHandle != INVALID_HANDLE_VALUE)
		binaryReadThread = std::thread(&OneWili::binaryReadTask, this);

	return true;
}

bool OneWili::isOpen() {
	return textHandle != INVALID_HANDLE_VALUE;
}

bool OneWili::close() {
	if(!isOpen() && !isDisconnected()) {
		report(APIEvent::Type::DeviceCurrentlyClosed, APIEvent::Severity::Error);
		return false;
	}

	setIsClosing(true);
	if(textHandle != INVALID_HANDLE_VALUE)
		CancelIoEx(textHandle, nullptr);
	if(binaryHandle != INVALID_HANDLE_VALUE)
		CancelIoEx(binaryHandle, nullptr);
	writeQueue.enqueue(WriteOperation{});

	if(binaryReadThread.joinable())
		binaryReadThread.join();
	if(textReadThread.joinable())
		textReadThread.join();
	if(writeThread.joinable())
		writeThread.join();

	if(readOverlapped.hEvent) {
		CloseHandle(readOverlapped.hEvent);
		readOverlapped.hEvent = nullptr;
	}
	if(writeOverlapped.hEvent) {
		CloseHandle(writeOverlapped.hEvent);
		writeOverlapped.hEvent = nullptr;
	}
	if(binaryHandle != INVALID_HANDLE_VALUE) {
		CloseHandle(binaryHandle);
		binaryHandle = INVALID_HANDLE_VALUE;
	}
	if(textHandle != INVALID_HANDLE_VALUE) {
		CloseHandle(textHandle);
		textHandle = INVALID_HANDLE_VALUE;
	}

	setIsClosing(false);
	setIsDisconnected(false);
	clearBuffers();
	{
		std::scoped_lock<std::mutex> lock(textResponseMutex);
		textResponses.clear();
		textAccumulator.clear();
	}
	return true;
}

bool OneWili::popTextResponse(std::string& line) {
	std::scoped_lock<std::mutex> lock(textResponseMutex);
	if(textResponses.empty())
		return false;
	line = std::move(textResponses.front());
	textResponses.pop_front();
	return true;
}

void OneWili::binaryReadTask() {
	constexpr DWORD READ_BUFFER_SIZE = 4096;
	std::vector<uint8_t> buffer(READ_BUFFER_SIZE);
	OVERLAPPED overlapped = {};
	overlapped.hEvent = CreateEventA(nullptr, true, false, nullptr);
	EventManager::GetInstance().downgradeErrorsOnCurrentThread();

	while(!isClosing() && !isDisconnected()) {
		ResetEvent(overlapped.hEvent);
		DWORD bytesRead = 0;
		if(!ReadFile(binaryHandle, buffer.data(), READ_BUFFER_SIZE, nullptr, &overlapped)) {
			const DWORD error = GetLastError();
			if(error != ERROR_IO_PENDING && error != ERROR_OPERATION_ABORTED) {
				setIsDisconnected(true);
				report(APIEvent::Type::DeviceDisconnected, APIEvent::Severity::Error);
				break;
			}
		}
		if(!GetOverlappedResult(binaryHandle, &overlapped, &bytesRead, true)) {
			if(GetLastError() == ERROR_OPERATION_ABORTED && isClosing())
				break;
			setIsDisconnected(true);
			report(APIEvent::Type::DeviceDisconnected, APIEvent::Severity::Error);
			break;
		}
		if(bytesRead > 0)
			pushRx(buffer.data(), bytesRead);
	}

	CloseHandle(overlapped.hEvent);
}

void OneWili::textReadTask() {
	constexpr DWORD READ_BUFFER_SIZE = 4096;
	std::vector<char> buffer(READ_BUFFER_SIZE);
	EventManager::GetInstance().downgradeErrorsOnCurrentThread();

	while(!isClosing() && !isDisconnected()) {
		ResetEvent(readOverlapped.hEvent);
		DWORD bytesRead = 0;
		if(!ReadFile(textHandle, buffer.data(), READ_BUFFER_SIZE, nullptr, &readOverlapped)) {
			const DWORD error = GetLastError();
			if(error != ERROR_IO_PENDING && error != ERROR_OPERATION_ABORTED) {
				setIsDisconnected(true);
				report(APIEvent::Type::DeviceDisconnected, APIEvent::Severity::Error);
				break;
			}
		}
		if(!GetOverlappedResult(textHandle, &readOverlapped, &bytesRead, true)) {
			if(GetLastError() == ERROR_OPERATION_ABORTED && isClosing())
				break;
			setIsDisconnected(true);
			report(APIEvent::Type::DeviceDisconnected, APIEvent::Severity::Error);
			break;
		}
		if(bytesRead == 0)
			continue;

		std::scoped_lock<std::mutex> lock(textResponseMutex);
		textAccumulator.append(buffer.data(), bytesRead);
		size_t newline;
		while((newline = textAccumulator.find('\n')) != std::string::npos) {
			std::string line = textAccumulator.substr(0, newline);
			textAccumulator.erase(0, newline + 1);
			while(!line.empty() && (line.back() == '\r' || line.back() == '\n'))
				line.pop_back();
			if(line.empty())
				continue;
			if(textResponses.size() >= MAX_TEXT_RESPONSES)
				textResponses.pop_front();
			textResponses.push_back(std::move(line));
		}
		if(textAccumulator.size() > 64 * 1024)
			textAccumulator.clear();
	}
}

void OneWili::writeTask() {
	WriteOperation writeOp;
	EventManager::GetInstance().downgradeErrorsOnCurrentThread();

	while(!isClosing() && !isDisconnected()) {
		if(!writeQueue.wait_dequeue_timed(writeOp, std::chrono::milliseconds(100)))
			continue;
		if(isClosing() || isDisconnected())
			return;

		DWORD written = 0;
		ResetEvent(writeOverlapped.hEvent);
		if(!WriteFile(textHandle, writeOp.bytes.data(), static_cast<DWORD>(writeOp.bytes.size()), nullptr,
				&writeOverlapped)) {
			const DWORD error = GetLastError();
			if(error != ERROR_IO_PENDING && error != ERROR_OPERATION_ABORTED) {
				setIsDisconnected(true);
				report(APIEvent::Type::DeviceDisconnected, APIEvent::Severity::Error);
				return;
			}
		}
		if(!GetOverlappedResult(textHandle, &writeOverlapped, &written, true)) {
			if(GetLastError() == ERROR_OPERATION_ABORTED && isClosing())
				return;
			setIsDisconnected(true);
			report(APIEvent::Type::DeviceDisconnected, APIEvent::Severity::Error);
			return;
		}
		if(written != writeOp.bytes.size()) {
			setIsDisconnected(true);
			report(APIEvent::Type::FailedToWrite, APIEvent::Severity::Error);
			return;
		}
	}
}

std::string OneWili::HandleToTextPort(neodevice_handle_t handle) { return wideToUtf8(lookupPath(handle, false)); }
std::string OneWili::HandleToBinaryPort(neodevice_handle_t handle) { return wideToUtf8(lookupPath(handle, true)); }
