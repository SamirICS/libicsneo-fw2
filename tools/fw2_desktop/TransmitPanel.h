// TransmitPanel -- compose a frame, send it once or on a period.
//
// Everything here goes through Device::transmit(), not OneWili's own `i\c\w` or
// `i\c\p`. That is the point: the raw console is one tab over for anyone who wants
// to bypass the library, so this panel is worth having only if it proves the path
// an actual libicsneo application would take.

#ifndef FW2_DESKTOP_TRANSMITPANEL_H
#define FW2_DESKTOP_TRANSMITPANEL_H

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

class TransmitPanel : public QWidget {
	Q_OBJECT

public:
	explicit TransmitPanel(QWidget* parent = nullptr);

	/** Enables the controls. Transmitting while offline is refused by the device. */
	void setOnline(bool online);

public slots:
	void noteResult(bool ok, const QString& detail);

signals:
	void transmitRequested(quint32 arbid, const QByteArray& data, bool extended, bool canFD);
	void periodicRequested(bool enable, int periodMs, quint32 arbid,
		const QByteArray& data, bool extended, bool canFD);

private slots:
	void sendOnce();
	void togglePeriodic();
	void resetCounters();

private:
	/** Parses the ID and data fields. Returns false and explains if they are bad. */
	bool readFrame(quint32& arbid, QByteArray& data, QString& problem) const;
	void updateCounters();

	QLineEdit* idEdit = nullptr;
	QLineEdit* dataEdit = nullptr;
	QCheckBox* extendedBox = nullptr;
	QCheckBox* fdBox = nullptr;
	QSpinBox* periodSpin = nullptr;
	QPushButton* sendButton = nullptr;
	QPushButton* periodicButton = nullptr;
	QLabel* counters = nullptr;
	QLabel* lastError = nullptr;

	bool periodicActive = false;
	bool online = false;
	quint64 accepted = 0;
	quint64 rejected = 0;
};

#endif
