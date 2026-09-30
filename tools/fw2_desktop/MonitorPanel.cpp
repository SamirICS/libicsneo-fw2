#include "MonitorPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>

namespace {

QString formatData(const QByteArray& data) {
	QString out;
	out.reserve(data.size() * 3);
	for(unsigned char byte : data) {
		if(!out.isEmpty())
			out += QLatin1Char(' ');
		out += QStringLiteral("%1").arg(byte, 2, 16, QLatin1Char('0')).toUpper();
	}
	return out;
}

QString formatId(const FrameRecord& frame) {
	// Padded to the width the format implies -- 3 nibbles standard, 8 extended --
	// so a column of IDs lines up and an extended frame is obvious at a glance.
	const int digits = frame.extended ? 8 : 3;
	return QStringLiteral("0x")
		+ QString::number(frame.arbid, 16).toUpper().rightJustified(digits, QLatin1Char('0'));
}

QString formatType(const FrameRecord& frame) {
	QStringList bits;
	bits << (frame.canFD ? QStringLiteral("FD") : QStringLiteral("CAN"));
	if(frame.extended)
		bits << QStringLiteral("ext");
	if(frame.remote)
		bits << QStringLiteral("rtr");
	if(frame.error)
		bits << QStringLiteral("ERR");
	return bits.join(QLatin1Char(' '));
}

quint64 groupKey(const FrameRecord& frame) {
	return (static_cast<quint64>(frame.arbid) << 8)
		| (frame.transmitted ? 0x1u : 0u)
		| (frame.extended ? 0x2u : 0u)
		| (frame.canFD ? 0x4u : 0u);
}

} // namespace

MonitorPanel::MonitorPanel(QWidget* parent) : QWidget(parent) {
	auto* layout = new QVBoxLayout(this);

	auto* controls = new QHBoxLayout;
	modeBox = new QComboBox(this);
	modeBox->addItem(QStringLiteral("Scroll"));
	modeBox->addItem(QStringLiteral("Group by ID"));
	connect(modeBox, &QComboBox::currentIndexChanged, this, &MonitorPanel::changeMode);

	pauseBox = new QCheckBox(QStringLiteral("Pause display"), this);
	pauseBox->setToolTip(QStringLiteral(
		"Stops updating the table. Frames are still counted, so the totals keep\n"
		"moving -- this pauses the view, not the capture."));

	auto* clearButton = new QPushButton(QStringLiteral("Clear"), this);
	connect(clearButton, &QPushButton::clicked, this, &MonitorPanel::clear);

	auto* exportButton = new QPushButton(QStringLiteral("Export CSV..."), this);
	connect(exportButton, &QPushButton::clicked, this, &MonitorPanel::exportCSV);

	controls->addWidget(new QLabel(QStringLiteral("View:"), this));
	controls->addWidget(modeBox);
	controls->addWidget(pauseBox);
	controls->addStretch();
	controls->addWidget(clearButton);
	controls->addWidget(exportButton);
	layout->addLayout(controls);

	table = new QTableWidget(this);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setAlternatingRowColors(true);
	table->verticalHeader()->setVisible(false);
	table->verticalHeader()->setDefaultSectionSize(20);
	layout->addWidget(table, 1);

	stats = new QLabel(this);
	layout->addWidget(stats);

	applyMode();
	updateStats();
}

void MonitorPanel::applyMode() {
	table->clearContents();
	table->setRowCount(0);
	groups.clear();

	if(mode == Mode::Scroll) {
		table->setColumnCount(6);
		table->setHorizontalHeaderLabels({ QStringLiteral("Time (s)"), QStringLiteral("Dir"),
			QStringLiteral("ID"), QStringLiteral("Type"), QStringLiteral("DLC"),
			QStringLiteral("Data") });
	} else {
		table->setColumnCount(7);
		table->setHorizontalHeaderLabels({ QStringLiteral("ID"), QStringLiteral("Dir"),
			QStringLiteral("Type"), QStringLiteral("DLC"), QStringLiteral("Data"),
			QStringLiteral("Count"), QStringLiteral("Period (ms)") });
	}
	table->horizontalHeader()->setStretchLastSection(true);
	table->resizeColumnsToContents();
}

void MonitorPanel::changeMode(int index) {
	mode = index == 0 ? Mode::Scroll : Mode::Group;
	applyMode();
}

QString MonitorPanel::formatTimestamp(quint64 nanoseconds) const {
	if(nanoseconds == 0)
		return QStringLiteral("-");
	// Relative to the first frame of the session. The device epoch is not the
	// host's, so an absolute rendering would be a date with no meaning.
	const double seconds = static_cast<double>(nanoseconds - firstTimestamp) / 1e9;
	return QString::number(seconds, 'f', 6);
}

void MonitorPanel::addFrames(const QVector<FrameRecord>& frames) {
	for(const auto& frame : frames) {
		if(firstTimestamp == 0 && frame.timestamp != 0)
			firstTimestamp = frame.timestamp;
		if(frame.transmitted)
			++totalTransmitted;
		else
			++totalReceived;
		if(frame.error)
			++totalErrors;
	}

	if(!pauseBox->isChecked()) {
		// setUpdatesEnabled around the batch: without it Qt repaints per inserted
		// row, which is what makes naive trace views collapse under bus load.
		table->setUpdatesEnabled(false);
		const bool atBottom = table->verticalScrollBar()->value()
			>= table->verticalScrollBar()->maximum() - 4;
		for(const auto& frame : frames) {
			if(mode == Mode::Scroll)
				appendScrollRow(frame);
			else
				updateGroupRow(frame);
		}
		// Follow the tail only if the user was already at the tail; scrolling back
		// to inspect something should not be yanked away by new traffic.
		if(mode == Mode::Scroll && atBottom)
			table->scrollToBottom();
		table->setUpdatesEnabled(true);
	}

	updateStats();
}

void MonitorPanel::appendScrollRow(const FrameRecord& frame) {
	if(table->rowCount() >= MAX_SCROLL_ROWS)
		table->removeRow(0);

	const int row = table->rowCount();
	table->insertRow(row);
	table->setItem(row, 0, new QTableWidgetItem(formatTimestamp(frame.timestamp)));
	table->setItem(row, 1, new QTableWidgetItem(frame.transmitted
		? QStringLiteral("Tx") : QStringLiteral("Rx")));
	table->setItem(row, 2, new QTableWidgetItem(formatId(frame)));
	table->setItem(row, 3, new QTableWidgetItem(formatType(frame)));
	table->setItem(row, 4, new QTableWidgetItem(QString::number(frame.data.size())));
	table->setItem(row, 5, new QTableWidgetItem(formatData(frame.data)));
}

void MonitorPanel::updateGroupRow(const FrameRecord& frame) {
	const quint64 key = groupKey(frame);
	auto it = groups.find(key);
	if(it == groups.end()) {
		GroupEntry entry;
		entry.row = table->rowCount();
		table->insertRow(entry.row);
		table->setItem(entry.row, 0, new QTableWidgetItem(formatId(frame)));
		table->setItem(entry.row, 1, new QTableWidgetItem(frame.transmitted
			? QStringLiteral("Tx") : QStringLiteral("Rx")));
		table->setItem(entry.row, 2, new QTableWidgetItem(formatType(frame)));
		for(int column = 3; column < 7; ++column)
			table->setItem(entry.row, column, new QTableWidgetItem);
		it = groups.insert(key, entry);
	}

	GroupEntry& entry = it.value();
	if(entry.lastTimestamp && frame.timestamp > entry.lastTimestamp) {
		const double deltaMs = static_cast<double>(frame.timestamp - entry.lastTimestamp) / 1e6;
		// Exponential average rather than the raw delta: a single late frame
		// otherwise makes the period column flicker and unreadable.
		entry.periodMs = entry.periodMs > 0.0 ? (entry.periodMs * 0.8 + deltaMs * 0.2) : deltaMs;
	}
	entry.lastTimestamp = frame.timestamp;
	++entry.count;

	table->item(entry.row, 3)->setText(QString::number(frame.data.size()));
	table->item(entry.row, 4)->setText(formatData(frame.data));
	table->item(entry.row, 5)->setText(QString::number(entry.count));
	table->item(entry.row, 6)->setText(entry.periodMs > 0.0
		? QString::number(entry.periodMs, 'f', 1) : QStringLiteral("-"));
}

void MonitorPanel::noteDropped(quint64 total) {
	totalDropped = total;
	updateStats();
}

void MonitorPanel::updateStats() {
	QString text = QStringLiteral("received %1    transmitted %2    errors %3")
		.arg(totalReceived).arg(totalTransmitted).arg(totalErrors);
	if(totalDropped)
		text += QStringLiteral("    DROPPED %1 (display could not keep up)").arg(totalDropped);
	stats->setText(text);
}

void MonitorPanel::clear() {
	totalReceived = totalTransmitted = totalErrors = totalDropped = 0;
	firstTimestamp = 0;
	applyMode();
	updateStats();
}

void MonitorPanel::exportCSV() {
	const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export trace"),
		QStringLiteral("fw2_trace.csv"), QStringLiteral("CSV files (*.csv)"));
	if(path.isEmpty())
		return;

	QFile file(path);
	if(!file.open(QIODevice::WriteOnly | QIODevice::Text))
		return;

	QTextStream out(&file);
	QStringList header;
	for(int column = 0; column < table->columnCount(); ++column)
		header << table->horizontalHeaderItem(column)->text();
	out << header.join(QLatin1Char(',')) << '\n';

	for(int row = 0; row < table->rowCount(); ++row) {
		QStringList fields;
		for(int column = 0; column < table->columnCount(); ++column) {
			const auto* item = table->item(row, column);
			QString value = item ? item->text() : QString();
			// Data bytes are space separated, so only a quote could break the
			// format; escape it the CSV way rather than mangling the value.
			if(value.contains(QLatin1Char('"')) || value.contains(QLatin1Char(',')))
				value = QLatin1Char('"') + value.replace(QLatin1Char('"'),
					QStringLiteral("\"\"")) + QLatin1Char('"');
			fields << value;
		}
		out << fields.join(QLatin1Char(',')) << '\n';
	}
}
