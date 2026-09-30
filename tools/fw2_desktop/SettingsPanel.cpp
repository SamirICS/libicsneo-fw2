#include "SettingsPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

// The rates a bench actually uses, plus an escape hatch. OneWili takes raw bits
// per second, so these are the literal values sent.
const QVector<QPair<QString, int>> STANDARD_RATES = {
	{ QStringLiteral("125 kbps"), 125000 },
	{ QStringLiteral("250 kbps"), 250000 },
	{ QStringLiteral("500 kbps"), 500000 },
	{ QStringLiteral("1 Mbps"), 1000000 },
};

const QVector<QPair<QString, int>> FD_RATES = {
	{ QStringLiteral("1 Mbps"), 1000000 },
	{ QStringLiteral("2 Mbps"), 2000000 },
	{ QStringLiteral("4 Mbps"), 4000000 },
	{ QStringLiteral("5 Mbps"), 5000000 },
};

const QString CUSTOM = QStringLiteral("Custom...");

} // namespace

SettingsPanel::SettingsPanel(QWidget* parent) : QWidget(parent) {
	auto* layout = new QVBoxLayout(this);

	auto* group = new QGroupBox(QStringLiteral("CAN 1"), this);
	auto* form = new QFormLayout(group);

	rateBox = new QComboBox(this);
	for(const auto& rate : STANDARD_RATES)
		rateBox->addItem(rate.first, rate.second);
	rateBox->addItem(CUSTOM);
	rateBox->setCurrentIndex(2); // 500 kbps, the bench default

	customRate = new QSpinBox(this);
	customRate->setRange(10000, 1000000);
	customRate->setSingleStep(1000);
	customRate->setValue(500000);
	customRate->setSuffix(QStringLiteral(" bps"));
	customRate->setEnabled(false);
	connect(rateBox, &QComboBox::currentTextChanged, this, [this](const QString& text) {
		customRate->setEnabled(text == CUSTOM);
	});

	auto* rateRow = new QHBoxLayout;
	rateRow->addWidget(rateBox);
	rateRow->addWidget(customRate);
	form->addRow(QStringLiteral("Arbitration rate"), rateRow);

	fdRateBox = new QComboBox(this);
	for(const auto& rate : FD_RATES)
		fdRateBox->addItem(rate.first, rate.second);
	fdRateBox->addItem(CUSTOM);
	fdRateBox->setCurrentIndex(1); // 2 Mbps

	customFDRate = new QSpinBox(this);
	customFDRate->setRange(100000, 8000000);
	customFDRate->setSingleStep(100000);
	customFDRate->setValue(2000000);
	customFDRate->setSuffix(QStringLiteral(" bps"));
	customFDRate->setEnabled(false);
	connect(fdRateBox, &QComboBox::currentTextChanged, this, [this](const QString& text) {
		customFDRate->setEnabled(text == CUSTOM);
	});

	auto* fdRow = new QHBoxLayout;
	fdRow->addWidget(fdRateBox);
	fdRow->addWidget(customFDRate);
	form->addRow(QStringLiteral("FD data rate"), fdRow);

	terminationBox = new QCheckBox(QStringLiteral("Enable 120 Ω termination"), this);
	terminationBox->setChecked(true);
	terminationBox->setToolTip(QStringLiteral(
		"A CAN bus needs exactly two terminators, one at each end. If the partner\n"
		"device is already terminated and so is this one, that is correct. Three or\n"
		"one will both cause trouble."));
	form->addRow(QString(), terminationBox);

	listenOnlyBox = new QCheckBox(QStringLiteral("Listen only (do not acknowledge)"), this);
	listenOnlyBox->setToolTip(QStringLiteral(
		"Silent monitoring. Note that a listen-only node does not ACK, so a bus with\n"
		"only this node and one transmitter will show the transmitter erroring."));
	form->addRow(QString(), listenOnlyBox);

	txRetryBox = new QCheckBox(QStringLiteral("Retry failed transmits"), this);
	txRetryBox->setChecked(true);
	form->addRow(QString(), txRetryBox);

	layout->addWidget(group);

	applyButton = new QPushButton(QStringLiteral("Apply to device"), this);
	connect(applyButton, &QPushButton::clicked, this, &SettingsPanel::apply);
	layout->addWidget(applyButton);

	results = new QLabel(this);
	results->setWordWrap(true);
	results->setTextFormat(Qt::RichText);
	layout->addWidget(results);

	auto* caveats = new QLabel(QStringLiteral(
		"<b>Two things worth knowing.</b><br>"
		"These settings do <i>not</i> survive a board reboot, and goOnline() does not "
		"re-apply them &mdash; after a power cycle, apply them again.<br>"
		"The firmware does not report its current configuration, so this panel can show "
		"what was sent but cannot read back what is in effect."), this);
	caveats->setWordWrap(true);
	caveats->setStyleSheet(QStringLiteral("color: #8a6d00;"));
	layout->addWidget(caveats);

	layout->addStretch();
	setConnected(false);
}

void SettingsPanel::setConnected(bool connected) {
	applyButton->setEnabled(connected);
}

int SettingsPanel::selectedRate(const QComboBox* box, const QSpinBox* custom) const {
	const QVariant value = box->currentData();
	return value.isValid() ? value.toInt() : custom->value();
}

void SettingsPanel::apply() {
	results->clear();
	emit applyRequested(selectedRate(rateBox, customRate),
		selectedRate(fdRateBox, customFDRate),
		listenOnlyBox->isChecked(),
		terminationBox->isChecked(),
		txRetryBox->isChecked());
}

void SettingsPanel::noteReply(const QString& command, bool ok, const QString& response) {
	const QString colour = ok ? QStringLiteral("#1a7f37") : QStringLiteral("#b3261e");
	const QString line = QStringLiteral("<span style='color:%1'>%2</span> &mdash; <code>%3</code> %4")
		.arg(colour, ok ? QStringLiteral("ok") : QStringLiteral("failed"),
			command.toHtmlEscaped(), response.toHtmlEscaped());
	results->setText(results->text().isEmpty() ? line
		: results->text() + QStringLiteral("<br>") + line);
}
