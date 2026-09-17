// PowerPanel -- the board's power rails, listed and switchable.
//
// This exists because of a specific bring-up trap: zone 15 (CAN) is off on a
// freshly booted FW2, and every CAN operation fails quietly until it is raised.
// goOnline() raises it automatically, but being able to see the rails is the
// difference between diagnosing that in a minute and diagnosing it in a day.

#ifndef FW2_DESKTOP_POWERPANEL_H
#define FW2_DESKTOP_POWERPANEL_H

#include <QVector>
#include <QWidget>

#include "DeviceWorker.h"

class QLabel;
class QPushButton;
class QTableWidget;

class PowerPanel : public QWidget {
	Q_OBJECT

public:
	explicit PowerPanel(QWidget* parent = nullptr);

	void setConnected(bool connected);

public slots:
	void setZones(const QVector<PowerZone>& zones);

signals:
	void refreshRequested();
	void zoneChangeRequested(int zone, bool on);

private:
	QTableWidget* table = nullptr;
	QPushButton* refreshButton = nullptr;
	QLabel* note = nullptr;
	bool connected = false;
};

#endif
