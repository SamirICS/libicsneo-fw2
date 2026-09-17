// MainWindow -- device selection, connection state, and the panels.
//
// Owns the worker thread and is the only place that knows how the panels are
// wired to it. The panels themselves know nothing about libicsneo: they emit
// intent and render what they are given, which keeps the threading confined to
// this file and DeviceWorker.

#ifndef FW2_DESKTOP_MAINWINDOW_H
#define FW2_DESKTOP_MAINWINDOW_H

#include <QMainWindow>
#include <QVector>

#include "DeviceWorker.h"

class ConsolePanel;
class MonitorPanel;
class PowerPanel;
class SettingsPanel;
class TransmitPanel;

class QAction;
class QComboBox;
class QLabel;
class QPushButton;
class QThread;

class MainWindow : public QMainWindow {
	Q_OBJECT

public:
	MainWindow();
	~MainWindow() override;

protected:
	void closeEvent(QCloseEvent* event) override;

private slots:
	void onDevicesFound(const QVector<DeviceEntry>& devices);
	void onConnectionChanged(bool connected, const QString& detail);
	void onOnlineChanged(bool online, const QString& detail);
	void onBusStateChanged(const BusStateRecord& state);
	void onLog(const QString& text);
	void toggleConnection();
	void toggleOnline();

signals:
	// Mirrors of the worker slots. Emitting a signal is how a queued call is made
	// without QMetaObject::invokeMethod string lookups at every call site.
	//
	// shutdown() is deliberately absent: it has to run *before* the worker's event
	// loop stops, so it is invoked with a blocking connection rather than queued
	// like these. See the destructor.
	void requestRefresh();
	void requestConnect(const QString& serial);
	void requestDisconnect();
	void requestOnline(bool online);

private:
	void buildToolbar();
	void updateConnectionUI();

	QThread* workerThread = nullptr;
	DeviceWorker* worker = nullptr;

	QComboBox* deviceBox = nullptr;
	QPushButton* connectButton = nullptr;
	QPushButton* onlineButton = nullptr;
	QAction* refreshAction = nullptr;

	MonitorPanel* monitor = nullptr;
	TransmitPanel* transmit = nullptr;
	SettingsPanel* settings = nullptr;
	PowerPanel* power = nullptr;
	ConsolePanel* console = nullptr;

	QLabel* connectionStatus = nullptr;
	QLabel* busStatus = nullptr;

	bool connected = false;
	bool online = false;
};

#endif
