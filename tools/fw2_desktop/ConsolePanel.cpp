#include "ConsolePanel.h"

#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

struct QuickCommand {
	const char* label;
	const char* command;
	const char* tooltip;
};

// The bring-up sequence, in order. Each of these is something that has actually
// been needed on the bench, not a tour of the command set.
const QuickCommand QUICK_COMMANDS[] = {
	{ "Board time (h\\t)", "h\\t",
		"Liveness probe. This is what open() uses to decide the board is talking." },
	{ "Power zones (h\\p\\g)", "h\\p\\g",
		"Lists every rail. Check zone 15 (CAN) here first when CAN does nothing." },
	{ "CAN power on", "h\\p\\s 15 1",
		"Raises zone 15, which feeds the MCP2518FD and the transceiver." },
	{ "Rate 500k", "h\\s\\p\\b 500000",
		"Sets the CAN 1 arbitration rate in raw bits per second." },
	{ "Termination on", "h\\s\\p\\1 1",
		"Switches in the on-board 120 ohm terminator." },
	{ "Accept-all filter", "i\\c\\f 0 0 1 0 00000000 00000000",
		"Enables filter 0 with a zero mask, which matches every ID. The controller\n"
		"stores a received frame only when an enabled filter matches it." },
	{ "Stream on", "i\\c\\o 0 1",
		"Starts the CAN receive stream on channel 0." },
	{ "Stream off", "i\\c\\o 0 0", "Stops the receive stream." },
	{ "Read C1TREC", "i\\c\\r 0 0000001C 3",
		"Reads TEC/REC and the bus diagnostic latches. Returns no data on firmware\n"
		"where register reads are unimplemented -- an empty Ok is the tell." },
	{ "Send 0x123", "i\\c\\w 0 00000123 0 0 DEADBEEF",
		"Transmits one frame via OneWili directly, bypassing libicsneo." },
};

} // namespace

ConsolePanel::ConsolePanel(QWidget* parent) : QWidget(parent) {
	auto* layout = new QVBoxLayout(this);

	auto* quickGroup = new QGroupBox(QStringLiteral("Common commands"), this);
	auto* grid = new QGridLayout(quickGroup);
	int column = 0, row = 0;
	for(const auto& quick : QUICK_COMMANDS) {
		auto* button = new QPushButton(QString::fromLatin1(quick.label), this);
		button->setToolTip(QString::fromLatin1(quick.tooltip)
			+ QStringLiteral("\n\nSends: ") + QString::fromLatin1(quick.command));
		const QString command = QString::fromLatin1(quick.command);
		connect(button, &QPushButton::clicked, this, [this, command]() {
			entry->setText(command);
			submit();
		});
		grid->addWidget(button, row, column);
		if(++column == 5) {
			column = 0;
			++row;
		}
	}
	layout->addWidget(quickGroup);

	output = new QPlainTextEdit(this);
	output->setReadOnly(true);
	output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	// Bounded so a long session cannot grow the process without limit.
	output->setMaximumBlockCount(5000);
	layout->addWidget(output, 1);

	auto* entryRow = new QHBoxLayout;
	entry = new QLineEdit(this);
	entry->setPlaceholderText(QStringLiteral("OneWili command, e.g.  h\\p\\g"));
	entry->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	entry->installEventFilter(this);
	connect(entry, &QLineEdit::returnPressed, this, &ConsolePanel::submit);

	sendButton = new QPushButton(QStringLiteral("Send"), this);
	connect(sendButton, &QPushButton::clicked, this, &ConsolePanel::submit);

	entryRow->addWidget(entry, 1);
	entryRow->addWidget(sendButton);
	layout->addLayout(entryRow);

	auto* hint = new QLabel(QStringLiteral(
		"Backslashes are literal &mdash; type <code>h\\p\\g</code>, not <code>h/p/g</code>. "
		"Up and down arrows walk the history."), this);
	hint->setWordWrap(true);
	layout->addWidget(hint);

	setConnected(false);
}

void ConsolePanel::setConnected(bool connected) {
	entry->setEnabled(connected);
	sendButton->setEnabled(connected);
	for(auto* button : findChildren<QPushButton*>())
		button->setEnabled(connected);
}

bool ConsolePanel::eventFilter(QObject* watched, QEvent* event) {
	if(watched != entry || event->type() != QEvent::KeyPress)
		return QWidget::eventFilter(watched, event);

	auto* key = static_cast<QKeyEvent*>(event);
	if(key->key() == Qt::Key_Up && !history.isEmpty()) {
		historyIndex = qMax(0, historyIndex - 1);
		entry->setText(history.value(historyIndex));
		return true;
	}
	if(key->key() == Qt::Key_Down && !history.isEmpty()) {
		historyIndex = qMin(history.size(), historyIndex + 1);
		// Walking past the newest entry returns to an empty line, which is what a
		// shell does and what the muscle memory expects.
		entry->setText(historyIndex < history.size() ? history.value(historyIndex) : QString());
		return true;
	}
	return QWidget::eventFilter(watched, event);
}

void ConsolePanel::submit() {
	const QString command = entry->text().trimmed();
	if(command.isEmpty())
		return;

	if(history.isEmpty() || history.last() != command)
		history.append(command);
	historyIndex = history.size();
	entry->clear();

	append(QStringLiteral("<span style='color:#0b5394'>&gt;&gt;&gt; %1</span>")
		.arg(command.toHtmlEscaped()));
	emit commandRequested(command, QStringLiteral("console"));
}

void ConsolePanel::noteReply(const QString& command, bool ok, const QString& response,
	const QString& tag) {
	// Settings and power commands are issued by their own panels and reported
	// there; echoing them here too would bury what the user typed.
	if(tag != QStringLiteral("console"))
		return;

	Q_UNUSED(command);
	if(ok) {
		append(response.toHtmlEscaped());
		return;
	}
	append(QStringLiteral("<span style='color:#b3261e'>failed%1</span>")
		.arg(response.isEmpty() ? QString() : QStringLiteral(": ") + response.toHtmlEscaped()));
}

void ConsolePanel::appendLog(const QString& text) {
	append(QStringLiteral("<span style='color:#666666'>%1</span>").arg(text.toHtmlEscaped()));
}

void ConsolePanel::append(const QString& html) {
	output->appendHtml(html);
}
