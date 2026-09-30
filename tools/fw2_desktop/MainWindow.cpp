#include "MainWindow.h"

#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QStatusBar>
#include <QTabWidget>
#include <QThread>
#include <QToolBar>

#include "ConsolePanel.h"
#include "MonitorPanel.h"
#include "PowerPanel.h"
#include "SettingsPanel.h"
#include "TransmitPanel.h"

namespace {

/** Mirrors icsneo::CANErrorCode, which the GUI deliberately does not include. */
QString errorCodeName(int code) {
	switch(code) {
		case 0: return QStringLiteral("none");
		case 1: return QStringLiteral("stuff");
		case 2: return QStringLiteral("form");
		case 3: return QStringLiteral("ACK");
		case 4: return QStringLiteral("bit1");
		case 5: return QStringLiteral("bit0");
		case 6: return QStringLiteral("CRC");
		case 7: return QStringLiteral("no change");
		default: return QStringLiteral("?");
	}
}

} // namespace

MainWindow::MainWindow() {
	setWindowTitle(QStringLiteral("FreeWili 2 Console"));
	resize(1100, 720);

	DeviceWorker::registerMetaTypes();

	auto* tabs = new QTabWidget(this);
	monitor = new MonitorPanel(this);
	transmit = new TransmitPanel(this);
	settings = new SettingsPanel(this);
	power = new PowerPanel(this);
	console = new ConsolePanel(this);
	tabs->addTab(monitor, QStringLiteral("Monitor"));
	tabs->addTab(transmit, QStringLiteral("Transmit"));
	tabs->addTab(settings, QStringLiteral("Settings"));
	tabs->addTab(power, QStringLiteral("Power"));
	tabs->addTab(console, QStringLiteral("Console"));
	setCentralWidget(tabs);

	buildToolbar();

	connectionStatus = new QLabel(QStringLiteral("disconnected"), this);
	busStatus = new QLabel(QStringLiteral("bus: unknown"), this);
	busStatus->setToolTip(QStringLiteral(
		"Controller error state. \"unknown\" means the device has reported nothing --\n"
		"which is a different claim from a healthy bus, and is what you see on\n"
		"firmware where the controller registers cannot be read."));
	statusBar()->addWidget(connectionStatus, 1);
	statusBar()->addPermanentWidget(busStatus);

	// --- worker thread ---------------------------------------------------
	workerThread = new QThread(this);
	worker = new DeviceWorker;
	worker->moveToThread(workerThread);

	connect(workerThread, &QThread::started, worker, &DeviceWorker::initialize);
	connect(workerThread, &QThread::finished, worker, &QObject::deleteLater);

	connect(this, &MainWindow::requestRefresh, worker, &DeviceWorker::refreshDevices);
	connect(this, &MainWindow::requestConnect, worker, &DeviceWorker::connectDevice);
	connect(this, &MainWindow::requestDisconnect, worker, &DeviceWorker::disconnectDevice);
	connect(this, &MainWindow::requestOnline, worker, &DeviceWorker::setOnline);

	connect(worker, &DeviceWorker::devicesFound, this, &MainWindow::onDevicesFound);
	connect(worker, &DeviceWorker::connectionChanged, this, &MainWindow::onConnectionChanged);
	connect(worker, &DeviceWorker::onlineChanged, this, &MainWindow::onOnlineChanged);
	connect(worker, &DeviceWorker::busStateChanged, this, &MainWindow::onBusStateChanged);
	connect(worker, &DeviceWorker::log, this, &MainWindow::onLog);

	connect(worker, &DeviceWorker::framesReceived, monitor, &MonitorPanel::addFrames);
	connect(worker, &DeviceWorker::framesDropped, monitor, &MonitorPanel::noteDropped);

	connect(transmit, &TransmitPanel::transmitRequested, worker, &DeviceWorker::transmitFrame);
	connect(transmit, &TransmitPanel::periodicRequested, worker, &DeviceWorker::setPeriodicTransmit);
	connect(worker, &DeviceWorker::transmitResult, transmit, &TransmitPanel::noteResult);

	connect(settings, &SettingsPanel::applyRequested, worker, &DeviceWorker::applyCANSettings);
	connect(power, &PowerPanel::refreshRequested, worker, &DeviceWorker::refreshPowerZones);
	connect(power, &PowerPanel::zoneChangeRequested, worker, &DeviceWorker::setPowerZone);
	connect(worker, &DeviceWorker::powerZonesRefreshed, power, &PowerPanel::setZones);

	connect(console, &ConsolePanel::commandRequested, worker, &DeviceWorker::sendOneWili);
	connect(worker, &DeviceWorker::oneWiliReply, console, &ConsolePanel::noteReply);
	connect(worker, &DeviceWorker::oneWiliReply, this,
		[this](const QString& command, bool ok, const QString& response, const QString& tag) {
			if(tag == QStringLiteral("settings"))
				settings->noteReply(command, ok, response);
		});

	workerThread->start();
	updateConnectionUI();
}

MainWindow::~MainWindow() {
	if(workerThread && workerThread->isRunning()) {
		// Blocking so the device is released before the thread's event loop dies;
		// a queued shutdown() would never be processed after quit().
		QMetaObject::invokeMethod(worker, "shutdown", Qt::BlockingQueuedConnection);
		workerThread->quit();
		workerThread->wait(5000);
	}
}

void MainWindow::buildToolbar() {
	auto* bar = addToolBar(QStringLiteral("Device"));
	bar->setMovable(false);

	bar->addWidget(new QLabel(QStringLiteral("  Device: "), this));
	deviceBox = new QComboBox(this);
	deviceBox->setMinimumWidth(320);
	bar->addWidget(deviceBox);

	refreshAction = bar->addAction(QStringLiteral("Refresh"), this, [this]() {
		emit requestRefresh();
	});

	connectButton = new QPushButton(QStringLiteral("Connect"), this);
	connect(connectButton, &QPushButton::clicked, this, &MainWindow::toggleConnection);
	bar->addWidget(connectButton);

	onlineButton = new QPushButton(QStringLiteral("Go online"), this);
	connect(onlineButton, &QPushButton::clicked, this, &MainWindow::toggleOnline);
	bar->addWidget(onlineButton);
}

void MainWindow::onDevicesFound(const QVector<DeviceEntry>& devices) {
	const QString previous = deviceBox->currentData().toString();
	deviceBox->clear();
	for(const auto& device : devices) {
		// Non-FW2 devices are listed rather than filtered out: seeing a FIRE3 in
		// the list is how you tell "no FW2 attached" from "libicsneo found nothing
		// at all", and those have very different causes.
		const QString label = device.isFW2 ? device.description
			: device.description + QStringLiteral("  [not a FreeWili 2]");
		deviceBox->addItem(label, device.serial);
		if(!device.isFW2)
			deviceBox->setItemData(deviceBox->count() - 1, QColor(0x88, 0x88, 0x88),
				Qt::ForegroundRole);
	}

	const int restored = deviceBox->findData(previous);
	if(restored >= 0)
		deviceBox->setCurrentIndex(restored);

	if(devices.isEmpty())
		connectionStatus->setText(QStringLiteral("no devices found"));
	updateConnectionUI();
}

void MainWindow::toggleConnection() {
	if(connected) {
		emit requestDisconnect();
		return;
	}
	const QString serial = deviceBox->currentData().toString();
	if(serial.isEmpty())
		return;
	connectionStatus->setText(QStringLiteral("connecting..."));
	connectButton->setEnabled(false);
	emit requestConnect(serial);
}

void MainWindow::toggleOnline() {
	onlineButton->setEnabled(false);
	emit requestOnline(!online);
}

void MainWindow::onConnectionChanged(bool isConnected, const QString& detail) {
	connected = isConnected;
	if(!connected) {
		online = false;
		busStatus->setText(QStringLiteral("bus: unknown"));
	}
	connectionStatus->setText(connected ? QStringLiteral("connected -- %1").arg(detail)
		: QStringLiteral("disconnected (%1)").arg(detail));
	updateConnectionUI();
}

void MainWindow::onOnlineChanged(bool isOnline, const QString& detail) {
	online = isOnline;
	connectionStatus->setText(online ? QStringLiteral("online -- %1").arg(detail)
		: QStringLiteral("offline (%1)").arg(detail));
	updateConnectionUI();
}

void MainWindow::onBusStateChanged(const BusStateRecord& state) {
	QString text = QStringLiteral("bus: TEC %1  REC %2").arg(state.tec).arg(state.rec);
	QString colour = QStringLiteral("#1a7f37");
	if(state.busOff) {
		text += QStringLiteral("  BUS-OFF");
		colour = QStringLiteral("#b3261e");
	} else if(state.errorPassive) {
		text += QStringLiteral("  error-passive");
		colour = QStringLiteral("#b3261e");
	} else if(state.errorWarn) {
		text += QStringLiteral("  error-warn");
		colour = QStringLiteral("#8a6d00");
	}
	if(state.errorCode != 0)
		text += QStringLiteral("  last %1").arg(errorCodeName(state.errorCode));

	busStatus->setText(text);
	busStatus->setStyleSheet(QStringLiteral("color: %1;").arg(colour));
}

void MainWindow::onLog(const QString& text) {
	// Everything the worker says goes to the console tab, which makes that tab a
	// running record of the session rather than only an echo of typed commands.
	console->appendLog(text);
}

void MainWindow::updateConnectionUI() {
	const bool haveSelection = deviceBox->count() > 0;
	connectButton->setEnabled(haveSelection || connected);
	connectButton->setText(connected ? QStringLiteral("Disconnect") : QStringLiteral("Connect"));
	deviceBox->setEnabled(!connected);
	refreshAction->setEnabled(!connected);

	onlineButton->setEnabled(connected);
	onlineButton->setText(online ? QStringLiteral("Go offline") : QStringLiteral("Go online"));

	transmit->setOnline(connected && online);
	settings->setConnected(connected);
	console->setConnected(connected);
	power->setConnected(connected);
}

void MainWindow::closeEvent(QCloseEvent* event) {
	if(connected)
		QMetaObject::invokeMethod(worker, "shutdown", Qt::BlockingQueuedConnection);
	event->accept();
}
