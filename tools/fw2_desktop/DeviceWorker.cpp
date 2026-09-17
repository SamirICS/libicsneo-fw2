#include "DeviceWorker.h"

#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

#include "icsneo/icsneocpp.h"
#include "icsneo/communication/fw2communication.h"
#include "icsneo/communication/message/canerrormessage.h"
#include "icsneo/communication/message/canmessage.h"
#include "icsneo/device/tree/freewili2/freewili2.h"

namespace {

// How often the RX deque is drained into the GUI. 50 ms is below the threshold
// where a human reads the trace as laggy, and it bounds the GUI to 20 batched
// updates a second regardless of bus load.
constexpr int DRAIN_INTERVAL_MS = 50;

// Liveness polling. FW2Communication runs its own 110 ms heartbeat; this only
// notices the result, so it can be lazy.
constexpr int CONNECTION_POLL_MS = 500;

QString describeEvent(const icsneo::APIEvent& event) {
	return QString::fromStdString(event.describe());
}

} // namespace

DeviceWorker::DeviceWorker(QObject* parent) : QObject(parent) {}

DeviceWorker::~DeviceWorker() {
	// shutdown() is the ordered path and is normally called first; this only
	// covers destruction without it, where leaving callbacks registered on a
	// live device would let libicsneo call into a destroyed object.
	detachCallbacks();
}

void DeviceWorker::registerMetaTypes() {
	qRegisterMetaType<FrameRecord>("FrameRecord");
	qRegisterMetaType<QVector<FrameRecord>>("QVector<FrameRecord>");
	qRegisterMetaType<BusStateRecord>("BusStateRecord");
	qRegisterMetaType<DeviceEntry>("DeviceEntry");
	qRegisterMetaType<QVector<DeviceEntry>>("QVector<DeviceEntry>");
	qRegisterMetaType<PowerZone>("PowerZone");
	qRegisterMetaType<QVector<PowerZone>>("QVector<PowerZone>");
}

void DeviceWorker::initialize() {
	// Created here rather than in the constructor because the constructor runs on
	// the GUI thread: a QTimer belongs to the thread that made it, and one made
	// there would never fire on this one.
	drainTimer = new QTimer(this);
	drainTimer->setInterval(DRAIN_INTERVAL_MS);
	connect(drainTimer, &QTimer::timeout, this, &DeviceWorker::drainFrames);
	drainTimer->start();

	periodicTimer = new QTimer(this);
	connect(periodicTimer, &QTimer::timeout, this, &DeviceWorker::onPeriodicTick);

	connectionTimer = new QTimer(this);
	connectionTimer->setInterval(CONNECTION_POLL_MS);
	connect(connectionTimer, &QTimer::timeout, this, &DeviceWorker::pollConnection);
	connectionTimer->start();

	// neoversion_t only renders through its ostream operator, so go via a stream
	// rather than reimplementing the format here and letting it drift.
	std::ostringstream version;
	version << icsneo::GetVersion();
	emit log(QStringLiteral("libicsneo %1").arg(QString::fromStdString(version.str())));
	refreshDevices();
}

void DeviceWorker::shutdown() {
	if(periodicTimer)
		periodicTimer->stop();
	if(drainTimer)
		drainTimer->stop();
	if(connectionTimer)
		connectionTimer->stop();
	disconnectDevice();
}

std::shared_ptr<icsneo::FW2Communication> DeviceWorker::fw2Communication() const {
	if(!device)
		return nullptr;
	return std::dynamic_pointer_cast<icsneo::FW2Communication>(device->com);
}

bool DeviceWorker::requireDevice(const char* what) {
	if(device)
		return true;
	emit log(QStringLiteral("%1: no device connected").arg(QString::fromLatin1(what)));
	return false;
}

void DeviceWorker::drainAPIEvents(const QString& context) {
	for(const auto& event : icsneo::GetEvents())
		emit log(QStringLiteral("[%1] %2").arg(context, describeEvent(event)));
}

void DeviceWorker::refreshDevices() {
	QVector<DeviceEntry> found;
	for(const auto& dev : icsneo::FindAllDevices()) {
		DeviceEntry entry;
		entry.serial = QString::fromStdString(dev->getSerial());
		entry.description = QString::fromStdString(dev->describe());
		entry.isFW2 = dev->getType().getDeviceType() == icsneo::DeviceType::FreeWili2;
		found.push_back(entry);
	}
	drainAPIEvents(QStringLiteral("find"));
	emit devicesFound(found);
}

void DeviceWorker::connectDevice(const QString& serial) {
	if(device)
		disconnectDevice();

	// FindAllDevices() again rather than caching handles from refreshDevices():
	// a stale shared_ptr from a previous scan can outlive a replug, and opening
	// one produces a confusing failure rather than an obvious one.
	std::shared_ptr<icsneo::Device> target;
	for(const auto& dev : icsneo::FindAllDevices()) {
		if(QString::fromStdString(dev->getSerial()) == serial)
			target = dev;
	}
	if(!target) {
		emit connectionChanged(false, QStringLiteral("device %1 is no longer present").arg(serial));
		return;
	}

	if(!target->open()) {
		const QString why = QString::fromStdString(icsneo::GetLastError().describe());
		drainAPIEvents(QStringLiteral("open"));
		emit connectionChanged(false, why);
		return;
	}

	device = target;
	lastKnownOpen = true;

	// Registered immediately after open() and before any goOnline(), so a board
	// that is already in trouble reports it on the first poll rather than only
	// after the user starts a stream.
	frameCallbackId = device->addMessageCallback(std::make_shared<icsneo::MessageCallback>(
		[this](std::shared_ptr<icsneo::Message> message) {
			const auto can = std::static_pointer_cast<icsneo::CANMessage>(message);
			FrameRecord record;
			record.timestamp = can->timestamp;
			record.arbid = can->arbid;
			record.data = QByteArray(reinterpret_cast<const char*>(can->data.data()),
				static_cast<qsizetype>(can->data.size()));
			record.extended = can->isExtended;
			record.canFD = can->isCANFD;
			record.remote = can->isRemote;
			record.transmitted = can->transmitted;
			record.error = can->error;

			std::scoped_lock<std::mutex> lock(frameLock);
			if(pendingFrames.size() >= MAX_PENDING_FRAMES) {
				// Drop the oldest: on an overrun the newest frames are the ones
				// still worth looking at, and the count is surfaced either way.
				pendingFrames.pop_front();
				++droppedFrames;
			}
			pendingFrames.push_back(std::move(record));
		},
		icsneo::MessageFilter(icsneo::Message::Type::Frame)));

	errorCallbackId = device->addMessageCallback(std::make_shared<icsneo::MessageCallback>(
		[this](std::shared_ptr<icsneo::Message> message) {
			const auto error = std::static_pointer_cast<icsneo::CANErrorMessage>(message);
			BusStateRecord state;
			state.tec = error->transmitErrorCount;
			state.rec = error->receiveErrorCount;
			state.busOff = error->busOff;
			state.errorPassive = error->errorPassive;
			state.errorWarn = error->errorWarn;
			state.errorCode = static_cast<int>(error->errorCode);
			state.dataErrorCode = static_cast<int>(error->dataErrorCode);
			// Safe to emit straight from libicsneo's thread: the connection to the
			// GUI is queued, so this only posts an event.
			emit busStateChanged(state);
		},
		icsneo::MessageFilter(icsneo::Message::Type::CANError)));

	drainAPIEvents(QStringLiteral("open"));
	emit connectionChanged(true, QString::fromStdString(device->describe()));
	refreshPowerZones();
}

void DeviceWorker::detachCallbacks() {
	if(!device)
		return;
	if(frameCallbackId >= 0)
		device->removeMessageCallback(frameCallbackId);
	if(errorCallbackId >= 0)
		device->removeMessageCallback(errorCallbackId);
	frameCallbackId = errorCallbackId = -1;
}

void DeviceWorker::disconnectDevice() {
	if(!device)
		return;

	if(periodicTimer)
		periodicTimer->stop();

	// Order matters. Callbacks come off first so nothing can be delivered into a
	// half-torn-down state, then the stream stops, then the device closes.
	detachCallbacks();
	if(device->isOnline())
		device->goOffline();
	const bool closed = device->close();

	device.reset();
	lastKnownOpen = false;
	{
		std::scoped_lock<std::mutex> lock(frameLock);
		pendingFrames.clear();
	}
	drainAPIEvents(QStringLiteral("close"));
	emit connectionChanged(false, closed ? QStringLiteral("closed")
		: QString::fromStdString(icsneo::GetLastError().describe()));
	emit onlineChanged(false, QStringLiteral("disconnected"));
}

void DeviceWorker::setOnline(bool online) {
	if(!requireDevice("goOnline"))
		return;

	const bool ok = online ? device->goOnline() : device->goOffline();
	const QString detail = ok ? (online ? QStringLiteral("streaming") : QStringLiteral("stopped"))
		: QString::fromStdString(icsneo::GetLastError().describe());
	drainAPIEvents(online ? QStringLiteral("online") : QStringLiteral("offline"));
	emit onlineChanged(ok && online, detail);

	if(online && ok) {
		// The rail comes up as part of goOnline(); show the result rather than
		// leaving the power tab showing what was true before.
		refreshPowerZones();
	}
}

void DeviceWorker::transmitFrame(quint32 arbid, const QByteArray& data, bool extended, bool canFD) {
	if(!requireDevice("transmit")) {
		emit transmitResult(false, QStringLiteral("no device"));
		return;
	}

	auto message = std::make_shared<icsneo::CANMessage>();
	message->network = icsneo::Network::NetID::DWCAN_01;
	message->arbid = arbid;
	message->isExtended = extended;
	message->isCANFD = canFD;
	message->data.assign(data.begin(), data.end());

	if(device->transmit(message)) {
		emit transmitResult(true, QString());
		return;
	}
	emit transmitResult(false, QString::fromStdString(icsneo::GetLastError().describe()));
}

void DeviceWorker::setPeriodicTransmit(bool enable, int periodMs, quint32 arbid,
	const QByteArray& data, bool extended, bool canFD) {
	if(!periodicTimer)
		return;

	if(!enable) {
		periodicTimer->stop();
		emit log(QStringLiteral("periodic transmit stopped after %1 sent, %2 rejected")
			.arg(periodicOk).arg(periodicFail));
		return;
	}

	periodicArbid = arbid;
	periodicData = data;
	periodicExtended = extended;
	periodicCANFD = canFD;
	periodicOk = periodicFail = 0;
	periodicTimer->start(std::max(1, periodMs));
	emit log(QStringLiteral("periodic transmit started: 0x%1 every %2 ms")
		.arg(arbid, 0, 16).arg(periodMs));
}

void DeviceWorker::onPeriodicTick() {
	if(!device) {
		periodicTimer->stop();
		return;
	}

	auto message = std::make_shared<icsneo::CANMessage>();
	message->network = icsneo::Network::NetID::DWCAN_01;
	message->arbid = periodicArbid;
	message->isExtended = periodicExtended;
	message->isCANFD = periodicCANFD;
	message->data.assign(periodicData.begin(), periodicData.end());

	if(device->transmit(message)) {
		++periodicOk;
		emit transmitResult(true, QString());
		return;
	}

	++periodicFail;
	const QString why = QString::fromStdString(icsneo::GetLastError().describe());
	emit transmitResult(false, why);
	// Report the first failure only. A wedged bus fails every tick, and a log
	// growing at the transmit rate buries whatever caused it.
	if(periodicFail == 1)
		emit log(QStringLiteral("periodic transmit failed: %1").arg(why));
}

bool DeviceWorker::oneWili(const QString& command, QString& response, int timeoutMs) {
	auto communication = fw2Communication();
	if(!communication) {
		response = QStringLiteral("not a FreeWili 2, or not connected");
		return false;
	}

	std::string reply;
	const bool ok = communication->callOneWili(command.toStdString(), reply,
		std::chrono::milliseconds(timeoutMs));
	response = QString::fromStdString(reply).trimmed();
	return ok;
}

void DeviceWorker::sendOneWili(const QString& command, const QString& tag) {
	const QString trimmed = command.trimmed();
	if(trimmed.isEmpty())
		return;

	QString response;
	const bool ok = oneWili(trimmed, response);
	emit oneWiliReply(trimmed, ok, response, tag);
}

void DeviceWorker::applyCANSettings(int rateBps, int fdDataRateBps, bool listenOnly,
	bool termination, bool txRetry) {
	if(!requireDevice("settings"))
		return;

	// Ordered, and each step reported separately, because this firmware returns a
	// bare `Ok` for commands that demonstrably do nothing -- register reads
	// included. A per-command result is the only honest thing to show.
	struct Step { QString command; QString what; };
	const QVector<Step> steps = {
		{ QStringLiteral("h\\s\\p\\b %1").arg(rateBps), QStringLiteral("arbitration rate") },
		{ QStringLiteral("h\\s\\p\\c %1").arg(fdDataRateBps), QStringLiteral("FD data rate") },
		{ QStringLiteral("h\\s\\p\\y %1").arg(listenOnly ? 1 : 0), QStringLiteral("listen only") },
		{ QStringLiteral("h\\s\\p\\e %1").arg(txRetry ? 1 : 0), QStringLiteral("TX retry") },
		{ QStringLiteral("h\\s\\p\\1 %1").arg(termination ? 1 : 0), QStringLiteral("termination") },
	};

	for(const auto& step : steps) {
		QString response;
		const bool ok = oneWili(step.command, response);
		emit oneWiliReply(step.command, ok, response, QStringLiteral("settings"));
		emit log(QStringLiteral("%1 %2: %3")
			.arg(ok ? QStringLiteral("set") : QStringLiteral("FAILED to set"), step.what, response));
	}
}

void DeviceWorker::refreshPowerZones() {
	if(!device)
		return;

	QString response;
	if(!oneWili(QStringLiteral("h\\p\\g"), response)) {
		emit log(QStringLiteral("power zone query failed: %1").arg(response));
		emit powerZonesRefreshed({});
		return;
	}

	// The reply is one long line of "<index>:<Name>   <state>" runs. Names have no
	// spaces but states do ("held in reset", "high (read only)"), so the split is
	// driven by where the next index starts, not by whitespace.
	static const QRegularExpression entry(QStringLiteral("(\\d+):(\\S+)"));
	QVector<PowerZone> zones;
	auto matches = entry.globalMatch(response);
	QList<QRegularExpressionMatch> all;
	while(matches.hasNext())
		all.append(matches.next());

	for(int i = 0; i < all.size(); ++i) {
		const auto& match = all[i];
		const int stateStart = match.capturedEnd();
		const int stateEnd = (i + 1 < all.size()) ? all[i + 1].capturedStart() : response.lastIndexOf(QLatin1Char(']'));
		if(stateEnd <= stateStart)
			continue;

		PowerZone zone;
		zone.index = match.captured(1).toInt();
		zone.name = match.captured(2);
		zone.state = response.mid(stateStart, stateEnd - stateStart).trimmed();
		// The trailing "Ok 1" of the frame rides on the last entry's state text.
		zone.state.remove(QRegularExpression(QStringLiteral("\\s*Ok\\s+\\d\\s*$")));
		zone.on = zone.state.startsWith(QStringLiteral("on"));
		// Read-only rails and the reset-held lines are reported but not offered as
		// switches, since writing them either fails or does something unintended.
		zone.settable = !zone.state.contains(QStringLiteral("read only"))
			&& !zone.state.contains(QStringLiteral("reset"));
		if(zone.index > 0)
			zones.push_back(zone);
	}

	emit powerZonesRefreshed(zones);
}

void DeviceWorker::setPowerZone(int zone, bool on) {
	if(!requireDevice("power zone"))
		return;

	// The board serialises rail changes and refuses while one is settling, so a
	// single "no" is not final. Same bounded retry as FreeWili2::raiseCANPowerZone.
	const QString command = QStringLiteral("h\\p\\s %1 %2").arg(zone).arg(on ? 1 : 0);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	QString response;
	bool ok = false;
	while(!(ok = oneWili(command, response)) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(250));

	emit log(QStringLiteral("zone %1 -> %2: %3").arg(zone)
		.arg(on ? QStringLiteral("on") : QStringLiteral("off"),
			ok ? response : QStringLiteral("failed (%1)").arg(response)));
	refreshPowerZones();
}

void DeviceWorker::drainFrames() {
	QVector<FrameRecord> batch;
	quint64 dropped = 0;
	{
		std::scoped_lock<std::mutex> lock(frameLock);
		if(pendingFrames.empty() && droppedFrames == 0)
			return;
		batch.reserve(static_cast<qsizetype>(pendingFrames.size()));
		for(auto& frame : pendingFrames)
			batch.push_back(std::move(frame));
		pendingFrames.clear();
		dropped = droppedFrames;
	}

	if(!batch.isEmpty())
		emit framesReceived(batch);
	if(dropped)
		emit framesDropped(dropped);
}

void DeviceWorker::pollConnection() {
	if(!device)
		return;

	const bool open = device->isOpen();
	if(open == lastKnownOpen)
		return;
	lastKnownOpen = open;

	if(!open) {
		// The heartbeat stopped answering. Say so plainly and tear down, rather
		// than leaving a window that looks connected to a board that is gone.
		emit log(QStringLiteral("device stopped responding -- disconnecting"));
		disconnectDevice();
	}
}
