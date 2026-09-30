// SettingsPanel -- CAN configuration, written straight through to OneWili.
//
// This panel is write-only on purpose, and that deserves an explanation rather
// than looking like laziness. On the firmware this was developed against, the
// settings read commands (`h\s\p\a`, `h\s\p\b`, `h\s\p\c`, `h\s\p\e`, ...) return
// no response frame at all when sent without an argument, while the same commands
// with an argument return `Ok`. There is therefore nothing to read back and no way
// to show the board's current configuration honestly. Showing the last values the
// user typed and implying they came from the device would be worse than showing
// nothing, so the panel says what it knows and no more.

#ifndef FW2_DESKTOP_SETTINGSPANEL_H
#define FW2_DESKTOP_SETTINGSPANEL_H

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

class SettingsPanel : public QWidget {
	Q_OBJECT

public:
	explicit SettingsPanel(QWidget* parent = nullptr);

	void setConnected(bool connected);

public slots:
	/** Appends the result of one settings command to the applied-so-far log. */
	void noteReply(const QString& command, bool ok, const QString& response);

signals:
	void applyRequested(int rateBps, int fdDataRateBps, bool listenOnly,
		bool termination, bool txRetry);

private slots:
	void apply();

private:
	int selectedRate(const QComboBox* box, const QSpinBox* custom) const;

	QComboBox* rateBox = nullptr;
	QSpinBox* customRate = nullptr;
	QComboBox* fdRateBox = nullptr;
	QSpinBox* customFDRate = nullptr;
	QCheckBox* listenOnlyBox = nullptr;
	QCheckBox* terminationBox = nullptr;
	QCheckBox* txRetryBox = nullptr;
	QPushButton* applyButton = nullptr;
	QLabel* results = nullptr;
};

#endif
