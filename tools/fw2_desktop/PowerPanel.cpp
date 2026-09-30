#include "PowerPanel.h"

#include <QColor>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

PowerPanel::PowerPanel(QWidget* parent) : QWidget(parent) {
	auto* layout = new QVBoxLayout(this);

	auto* controls = new QHBoxLayout;
	refreshButton = new QPushButton(QStringLiteral("Refresh"), this);
	connect(refreshButton, &QPushButton::clicked, this, &PowerPanel::refreshRequested);
	controls->addWidget(refreshButton);
	controls->addStretch();
	layout->addLayout(controls);

	table = new QTableWidget(this);
	table->setColumnCount(4);
	table->setHorizontalHeaderLabels({ QStringLiteral("Zone"), QStringLiteral("Name"),
		QStringLiteral("State"), QStringLiteral("") });
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->verticalHeader()->setVisible(false);
	table->horizontalHeader()->setStretchLastSection(true);
	layout->addWidget(table, 1);

	note = new QLabel(QStringLiteral(
		"Zone 15 (CAN) powers the controller and transceiver and is <b>off on a fresh boot</b>. "
		"Going online raises it automatically. Rail changes are serialised by the board and "
		"take a moment to settle."), this);
	note->setWordWrap(true);
	layout->addWidget(note);

	// Not setConnected(false): that is a transition and this is the initial state.
	refreshButton->setEnabled(false);
}

void PowerPanel::setConnected(bool isConnected) {
	// Called on every connection *and* online transition, so it has to be idempotent:
	// re-emitting on an unchanged state would put a OneWili round trip on the wire
	// every time the user toggled the stream.
	if(isConnected == connected)
		return;
	connected = isConnected;
	refreshButton->setEnabled(connected);
	if(!connected) {
		table->setRowCount(0);
		return;
	}
	emit refreshRequested();
}

void PowerPanel::setZones(const QVector<PowerZone>& zones) {
	table->setRowCount(zones.size());
	for(int row = 0; row < zones.size(); ++row) {
		const PowerZone& zone = zones[row];
		table->setItem(row, 0, new QTableWidgetItem(QString::number(zone.index)));
		table->setItem(row, 1, new QTableWidgetItem(zone.name));

		auto* state = new QTableWidgetItem(zone.state);
		if(zone.settable)
			state->setForeground(zone.on ? QColor(0x1a, 0x7f, 0x37) : QColor(0x66, 0x66, 0x66));
		table->setItem(row, 2, state);

		// Read-only lines get a label instead of a button: offering a switch that
		// cannot work is worse than showing the rail is not ours to change.
		if(!zone.settable) {
			table->setCellWidget(row, 3, nullptr);
			table->setItem(row, 3, new QTableWidgetItem(QStringLiteral("read only")));
			continue;
		}

		auto* toggle = new QPushButton(zone.on ? QStringLiteral("Turn off")
			: QStringLiteral("Turn on"), table);
		const int index = zone.index;
		const bool target = !zone.on;
		connect(toggle, &QPushButton::clicked, this, [this, index, target]() {
			emit zoneChangeRequested(index, target);
		});
		table->setCellWidget(row, 3, toggle);
	}
	table->resizeColumnsToContents();
	table->horizontalHeader()->setStretchLastSection(true);
}
