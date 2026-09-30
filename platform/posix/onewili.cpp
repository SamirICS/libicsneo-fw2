#include "icsneo/platform/onewili.h"
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <sys/select.h>
#include <algorithm>

using namespace icsneo;

namespace {

/**
 * Put a TTY into 8N1 raw mode.
 *
 * Both FW2 ports are USB serial, so the baud rate is nominal -- the endpoint
 * decides the real throughput. Upstream opens at 1,000,000 for the same reason.
 */
bool configureRaw(int fd) {
	struct termios tty = {};
	if(tcgetattr(fd, &tty) != 0)
		return false;

	cfmakeraw(&tty);
	tty.c_cflag |= (CLOCAL | CREAD);
	tty.c_cflag &= ~CSIZE;
	tty.c_cflag |= CS8;
	tty.c_cflag &= ~PARENB;
	tty.c_cflag &= ~CSTOPB;
	tty.c_cflag &= ~CRTSCTS;

	tty.c_cc[VMIN] = 0;
	tty.c_cc[VTIME] = 1;

	cfsetispeed(&tty, B1000000);
	cfsetospeed(&tty, B1000000);

	return tcsetattr(fd, TCSAFLUSH, &tty) == 0;
}

/** Wait up to `micros` for `fd` to become readable. */
void waitReadable(int fd, long micros) {
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	struct timeval tv = {};
	tv.tv_usec = micros;
	::select(fd + 1, &rfds, nullptr, nullptr, &tv);
}

} // namespace

OneWili::~OneWili() {
	if(isOpen())
		close();
}

bool OneWili::open() {
	if(isOpen()) {
		report(APIEvent::Type::DeviceCurrentlyOpen, APIEvent::Severity::Error);
		return false;
	}

	const std::string textPath = HandleToTextPort(device.handle);
	if(textPath.empty()) {
		report(APIEvent::Type::DriverFailedToOpen, APIEvent::Severity::Error);
		return false;
	}

	textFd = ::open(textPath.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
	if(textFd < 0 || !configureRaw(textFd)) {
		if(textFd >= 0) {
			::close(textFd);
			textFd = -1;
		}
		report(APIEvent::Type::DriverFailedToOpen, APIEvent::Severity::Error);
		return false;
	}

	// The binary port is optional: without it we simply receive no CAN frames.
	// That is a much more useful failure than refusing to open at all, since the
	// command port still works and the user can see why.
	const std::string binaryPath = HandleToBinaryPort(device.handle);
	if(!binaryPath.empty()) {
		binaryFd = ::open(binaryPath.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK);
		if(binaryFd >= 0 && !configureRaw(binaryFd)) {
			::close(binaryFd);
			binaryFd = -1;
		}
	}
	if(binaryFd < 0) {
		// Command port only: settings and TX will work, but no frames will arrive.
		report(APIEvent::Type::NotSupported, APIEvent::Severity::EventWarning);
	}

	// Reset the firmware's menu navigation to the root and enter quiet mode, so
	// the first command we send is interpreted from a known state. Same 0x02
	// prefix upstream's transport writes on open.
	const uint8_t reset[] = { 0x02, '\n' };
	if(::write(textFd, reset, sizeof(reset)) < 0) {
		close();
		report(APIEvent::Type::DriverFailedToOpen, APIEvent::Severity::Error);
		return false;
	}

	setIsClosing(false);
	setIsDisconnected(false);

	textReadThread = std::thread(&OneWili::textReadTask, this);
	writeThread = std::thread(&OneWili::writeTask, this);
	if(binaryFd >= 0)
		binaryReadThread = std::thread(&OneWili::binaryReadTask, this);

	return true;
}

bool OneWili::isOpen() {
	return textFd >= 0; // The binary port is optional; the command port is not
}

bool OneWili::close() {
	if(!isOpen() && !isDisconnected()) {
		report(APIEvent::Type::DeviceCurrentlyClosed, APIEvent::Severity::Error);
		return false;
	}

	setIsClosing(true);

	if(binaryReadThread.joinable())
		binaryReadThread.join();
	if(textReadThread.joinable())
		textReadThread.join();
	if(writeThread.joinable())
		writeThread.join();

	setIsClosing(false);
	setIsDisconnected(false);

	bool ok = true;
	if(binaryFd >= 0) {
		ok = ::close(binaryFd) == 0 && ok;
		binaryFd = -1;
	}
	if(textFd >= 0) {
		ok = ::close(textFd) == 0 && ok;
		textFd = -1;
	}

	clearBuffers();
	{
		std::scoped_lock<std::mutex> lk(textResponseMutex);
		textResponses.clear();
		textAccumulator.clear();
	}

	if(!ok)
		report(APIEvent::Type::DriverFailedToClose, APIEvent::Severity::Error);
	return ok;
}

bool OneWili::popTextResponse(std::string& line) {
	std::scoped_lock<std::mutex> lk(textResponseMutex);
	if(textResponses.empty())
		return false;
	line = std::move(textResponses.front());
	textResponses.pop_front();
	return true;
}

void OneWili::binaryReadTask() {
	constexpr size_t READ_BUFFER_SIZE = 4096;
	uint8_t buf[READ_BUFFER_SIZE];
	EventManager::GetInstance().downgradeErrorsOnCurrentThread();

	while(!isClosing() && !isDisconnected()) {
		waitReadable(binaryFd, 50000);
		const ssize_t bytesRead = ::read(binaryFd, buf, READ_BUFFER_SIZE);
		if(bytesRead > 0) {
			// Straight into the Driver ring buffer. FW2Communication's RX thread
			// parses OneWili binary framing out of it -- no Packetizer involved.
			pushRx(buf, static_cast<size_t>(bytesRead));
		} else if(bytesRead < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
			if(!isClosing() && !isDisconnected()) {
				setIsDisconnected(true);
				report(APIEvent::Type::DeviceDisconnected, APIEvent::Severity::Error);
			}
		}
	}
}

void OneWili::textReadTask() {
	constexpr size_t READ_BUFFER_SIZE = 4096;
	char buf[READ_BUFFER_SIZE];
	EventManager::GetInstance().downgradeErrorsOnCurrentThread();

	while(!isClosing() && !isDisconnected()) {
		waitReadable(textFd, 50000);
		const ssize_t bytesRead = ::read(textFd, buf, READ_BUFFER_SIZE);
		if(bytesRead > 0) {
			std::scoped_lock<std::mutex> lk(textResponseMutex);
			textAccumulator.append(buf, static_cast<size_t>(bytesRead));
			size_t newline;
			while((newline = textAccumulator.find('\n')) != std::string::npos) {
				std::string line = textAccumulator.substr(0, newline);
				textAccumulator.erase(0, newline + 1);
				while(!line.empty() && (line.back() == '\r' || line.back() == '\n'))
					line.pop_back();
				if(line.empty())
					continue;
				// Drop the oldest rather than the newest: a consumer that has
				// fallen behind wants the most recent acks, and the queue only
				// grows if nobody is reconciling at all.
				if(textResponses.size() >= MAX_TEXT_RESPONSES)
					textResponses.pop_front();
				textResponses.push_back(std::move(line));
			}
			// A device that never emits a newline must not grow the accumulator
			// without bound either.
			if(textAccumulator.size() > 64 * 1024)
				textAccumulator.clear();
		} else if(bytesRead < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
			if(!isClosing() && !isDisconnected()) {
				setIsDisconnected(true);
				report(APIEvent::Type::DeviceDisconnected, APIEvent::Severity::Error);
			}
		}
	}
}

void OneWili::writeTask() {
	WriteOperation writeOp;
	EventManager::GetInstance().downgradeErrorsOnCurrentThread();

	while(!isClosing() && !isDisconnected()) {
		if(!writeQueue.wait_dequeue_timed(writeOp, std::chrono::milliseconds(100)))
			continue;

		const ssize_t total = static_cast<ssize_t>(writeOp.bytes.size());
		ssize_t written = 0;
		while(written < total && !isClosing()) {
			const ssize_t actual = ::write(textFd, writeOp.bytes.data() + written, total - written);
			if(actual > 0) {
				written += actual;
				continue;
			}
			if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
				// TX FIFO full -- wait for room rather than spinning.
				fd_set wfds;
				FD_ZERO(&wfds);
				FD_SET(textFd, &wfds);
				struct timeval tv = {};
				tv.tv_usec = 50000;
				::select(textFd + 1, nullptr, &wfds, nullptr, &tv);
				continue;
			}
			if(!isDisconnected()) {
				setIsDisconnected(true);
				report(APIEvent::Type::DeviceDisconnected, APIEvent::Severity::Error);
			}
			break;
		}
	}
}
