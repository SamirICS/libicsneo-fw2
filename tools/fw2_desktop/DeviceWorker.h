// DeviceWorker -- owns the icsneo::Device and does every blocking thing off the GUI thread.
//
// Everything here runs on a dedicated QThread. That is not defensive style: open()
// performs USB enumeration and a real `h\t` round trip, goOnline() raises a power
// zone with a settle loop that can take seconds, and every OneWili command is a
// synchronous request/response with a timeout. Any one of those on the GUI thread
// would freeze the window, and the power-zone path would freeze it for five
// seconds at a stretch.
//
// Two directions of traffic cross the thread boundary, and they cross differently:
//
//   GUI -> worker   queued slot invocations. The GUI never touches the Device.
//   worker -> GUI   signals, auto-connected, so they arrive queued.
//
// Received frames take a third path, because they do not originate in either
// thread: libicsneo delivers them on its own internal read thread. Those land in a
// mutex-guarded deque and a timer on the worker thread drains it into one batched
// signal. Emitting per frame would post a GUI event per CAN message, which a busy
// 500 kbps bus turns into thousands per second.

#ifndef FW2_DESKTOP_DEVICEWORKER_H
#define FW2_DESKTOP_DEVICEWORKER_H

#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

class QTimer;

namespace icsneo {
class Device;
class FW2Communication;
}

/** One CAN frame, flattened out of icsneo::CANMessage so it can cross threads. */
struct FrameRecord {
	quint64 timestamp = 0;
	quint32 arbid = 0;
	QByteArray data;
	bool extended = false;
	bool canFD = false;
	bool remote = false;
	bool transmitted = false;
	bool error = false;
};

/** Controller error state, as reported by FW2Communication's bus-state poller. */
struct BusStateRecord {
	quint8 tec = 0;
	quint8 rec = 0;
	bool busOff = false;
	bool errorPassive = false;
	bool errorWarn = false;
	int errorCode = 0;
	int dataErrorCode = 0;
};

/** A device found by FindAllDevices(), identified by serial. */
struct DeviceEntry {
	QString serial;
	QString description;
	bool isFW2 = false;
};

/** One power rail as reported by `h\p\g`. */
struct PowerZone {
	int index = 0;
	QString name;
	QString state;     // verbatim: "on", "off", "held in reset", "high (read only)"
	bool on = false;
	bool settable = true;
};

Q_DECLARE_METATYPE(FrameRecord)
Q_DECLARE_METATYPE(BusStateRecord)
Q_DECLARE_METATYPE(DeviceEntry)
Q_DECLARE_METATYPE(PowerZone)

class DeviceWorker : public QObject {
	Q_OBJECT

public:
	explicit DeviceWorker(QObject* parent = nullptr);
	~DeviceWorker() override;

	/** Registers the metatypes above. Call once, from the GUI thread, before use. */
	static void registerMetaTypes();

public slots:
	/** Creates the worker-thread timers. Connected to QThread::started. */
	void initialize();

	/** Stops everything and releases the device. Must run before the thread quits. */
	void shutdown();

	void refreshDevices();
	void connectDevice(const QString& serial);
	void disconnectDevice();
	void setOnline(bool online);

	void transmitFrame(quint32 arbid, const QByteArray& data, bool extended, bool canFD);

	/**
	 * Periodic transmit, driven by a timer on this thread.
	 *
	 * Deliberately not OneWili's own `i\c\p`: sending through Device::transmit()
	 * on a timer exercises the same path an application would use, so what the
	 * bus partner sees is evidence about the library and not only about firmware.
	 */
	void setPeriodicTransmit(bool enable, int periodMs, quint32 arbid,
		const QByteArray& data, bool extended, bool canFD);

	/** Raw OneWili command. `tag` is echoed back so callers can route the reply. */
	void sendOneWili(const QString& command, const QString& tag = QString());

	/** Applies a CAN configuration as an ordered batch of OneWili commands. */
	void applyCANSettings(int rateBps, int fdDataRateBps, bool listenOnly,
		bool termination, bool txRetry);

	void refreshPowerZones();
	void setPowerZone(int zone, bool on);

signals:
	void devicesFound(const QVector<DeviceEntry>& devices);
	void connectionChanged(bool connected, const QString& detail);
	void onlineChanged(bool online, const QString& detail);
	void framesReceived(const QVector<FrameRecord>& frames);
	void busStateChanged(const BusStateRecord& state);
	void transmitResult(bool ok, const QString& detail);
	void oneWiliReply(const QString& command, bool ok, const QString& response, const QString& tag);
	void powerZonesRefreshed(const QVector<PowerZone>& zones);
	void log(const QString& text);

	/** Frames dropped because the GUI could not keep up. Reported, never hidden. */
	void framesDropped(quint64 total);

private slots:
	void drainFrames();
	void onPeriodicTick();
	void pollConnection();

private:
	std::shared_ptr<icsneo::FW2Communication> fw2Communication() const;
	bool requireDevice(const char* what);
	/** Runs one OneWili command, logging both sides. Returns false if it failed. */
	bool oneWili(const QString& command, QString& response, int timeoutMs = 1000);
	void drainAPIEvents(const QString& context);
	void detachCallbacks();

	std::shared_ptr<icsneo::Device> device;
	int frameCallbackId = -1;
	int errorCallbackId = -1;

	// Written from libicsneo's read thread, drained on this one.
	std::mutex frameLock;
	std::deque<FrameRecord> pendingFrames;
	quint64 droppedFrames = 0;
	// Sized for roughly a second of a saturated 500 kbps bus. Past this, the GUI
	// is not keeping up and showing stale frames is worse than saying so.
	static constexpr size_t MAX_PENDING_FRAMES = 20000;

	QTimer* drainTimer = nullptr;
	QTimer* periodicTimer = nullptr;
	QTimer* connectionTimer = nullptr;

	// Periodic transmit state, owned by this thread alone.
	quint32 periodicArbid = 0;
	QByteArray periodicData;
	bool periodicExtended = false;
	bool periodicCANFD = false;
	quint64 periodicOk = 0;
	quint64 periodicFail = 0;

	bool lastKnownOpen = false;
};

#endif
