// ConsolePanel -- a raw OneWili command line.
//
// The escape hatch. The other panels cover what the port models; this covers
// everything else the board can do, which during bring-up is most of what you
// need. Commands go through FW2Communication::callOneWili, so they share the
// device's transport and its request/response matching rather than fighting the
// library for the serial port.
//
// The quick buttons are the bring-up sequence that is easy to get wrong by hand,
// in the order that matters.

#ifndef FW2_DESKTOP_CONSOLEPANEL_H
#define FW2_DESKTOP_CONSOLEPANEL_H

#include <QStringList>
#include <QWidget>

class QLineEdit;
class QPlainTextEdit;
class QPushButton;

class ConsolePanel : public QWidget {
	Q_OBJECT

public:
	explicit ConsolePanel(QWidget* parent = nullptr);

	void setConnected(bool connected);

public slots:
	void noteReply(const QString& command, bool ok, const QString& response, const QString& tag);
	void appendLog(const QString& text);

signals:
	void commandRequested(const QString& command, const QString& tag);

private slots:
	void submit();

private:
	bool eventFilter(QObject* watched, QEvent* event) override;
	void append(const QString& html);

	QLineEdit* entry = nullptr;
	QPlainTextEdit* output = nullptr;
	QPushButton* sendButton = nullptr;

	// Shell-style history on the up and down arrows.
	QStringList history;
	int historyIndex = 0;
};

#endif
