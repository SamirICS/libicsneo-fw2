#include "TransmitPanel.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

// CAN FD's DLC table tops out at 64 bytes; classic CAN at 8. Enforced on send
// rather than by restricting the field, so the error names the actual problem.
constexpr int CLASSIC_MAX_BYTES = 8;
constexpr int FD_MAX_BYTES = 64;

} // namespace

TransmitPanel::TransmitPanel(QWidget* parent) : QWidget(parent) {
	auto* layout = new QVBoxLayout(this);

	auto* frameGroup = new QGroupBox(QStringLiteral("Frame"), this);
	auto* form = new QFormLayout(frameGroup);

	idEdit = new QLineEdit(QStringLiteral("123"), this);
	idEdit->setToolTip(QStringLiteral("Arbitration ID in hex, without a 0x prefix."));
	form->addRow(QStringLiteral("ID (hex)"), idEdit);

	dataEdit = new QLineEdit(QStringLiteral("DE AD BE EF 00 00 00 00"), this);
	dataEdit->setToolTip(QStringLiteral(
		"Payload bytes in hex. Spaces optional. Leave empty for a zero-length frame."));
	form->addRow(QStringLiteral("Data (hex)"), dataEdit);

	auto* flags = new QHBoxLayout;
	extendedBox = new QCheckBox(QStringLiteral("Extended (29-bit)"), this);
	fdBox = new QCheckBox(QStringLiteral("CAN FD"), this);
	flags->addWidget(extendedBox);
	flags->addWidget(fdBox);
	flags->addStretch();
	form->addRow(QStringLiteral("Format"), flags);

	layout->addWidget(frameGroup);

	auto* sendGroup = new QGroupBox(QStringLiteral("Send"), this);
	auto* sendLayout = new QHBoxLayout(sendGroup);

	sendButton = new QPushButton(QStringLiteral("Send once"), this);
	connect(sendButton, &QPushButton::clicked, this, &TransmitPanel::sendOnce);

	periodSpin = new QSpinBox(this);
	periodSpin->setRange(1, 60000);
	periodSpin->setValue(100);
	periodSpin->setSuffix(QStringLiteral(" ms"));

	periodicButton = new QPushButton(QStringLiteral("Start periodic"), this);
	periodicButton->setCheckable(true);
	connect(periodicButton, &QPushButton::clicked, this, &TransmitPanel::togglePeriodic);

	sendLayout->addWidget(sendButton);
	sendLayout->addSpacing(16);
	sendLayout->addWidget(new QLabel(QStringLiteral("Period:"), this));
	sendLayout->addWidget(periodSpin);
	sendLayout->addWidget(periodicButton);
	sendLayout->addStretch();
	layout->addWidget(sendGroup);

	counters = new QLabel(this);
	lastError = new QLabel(this);
	lastError->setWordWrap(true);
	auto* resetButton = new QPushButton(QStringLiteral("Reset counters"), this);
	connect(resetButton, &QPushButton::clicked, this, &TransmitPanel::resetCounters);

	auto* statusRow = new QHBoxLayout;
	statusRow->addWidget(counters);
	statusRow->addStretch();
	statusRow->addWidget(resetButton);
	layout->addLayout(statusRow);
	layout->addWidget(lastError);

	auto* caveat = new QLabel(QStringLiteral(
		"\"Accepted\" means the firmware took the command, not that the frame reached the wire.\n"
		"On current FW2 firmware there is no transmit echo, so confirm with a bus partner."), this);
	caveat->setWordWrap(true);
	caveat->setStyleSheet(QStringLiteral("color: #8a6d00;"));
	layout->addWidget(caveat);

	layout->addStretch();
	setOnline(false);
	updateCounters();
}

void TransmitPanel::setOnline(bool isOnline) {
	online = isOnline;
	sendButton->setEnabled(online);
	periodicButton->setEnabled(online);

	// Going offline with a periodic running would otherwise leave the button
	// latched on while nothing is being sent.
	if(!online && periodicActive) {
		periodicActive = false;
		periodicButton->setChecked(false);
		periodicButton->setText(QStringLiteral("Start periodic"));
		emit periodicRequested(false, 0, 0, QByteArray(), false, false);
	}
}

bool TransmitPanel::readFrame(quint32& arbid, QByteArray& data, QString& problem) const {
	bool ok = false;
	arbid = idEdit->text().trimmed().toUInt(&ok, 16);
	if(!ok) {
		problem = QStringLiteral("ID is not valid hex");
		return false;
	}

	const bool extended = extendedBox->isChecked();
	const quint32 limit = extended ? 0x1FFFFFFFu : 0x7FFu;
	if(arbid > limit) {
		problem = QStringLiteral("ID 0x%1 does not fit in %2 bits")
			.arg(arbid, 0, 16).arg(extended ? 29 : 11);
		return false;
	}

	QString text = dataEdit->text();
	text.remove(QRegularExpression(QStringLiteral("[\\s,]")));
	if(text.size() % 2 != 0) {
		problem = QStringLiteral("data has an odd number of hex digits");
		return false;
	}

	data.clear();
	for(int i = 0; i < text.size(); i += 2) {
		const quint8 byte = static_cast<quint8>(text.mid(i, 2).toUInt(&ok, 16));
		if(!ok) {
			problem = QStringLiteral("data is not valid hex at digit %1").arg(i + 1);
			return false;
		}
		data.append(static_cast<char>(byte));
	}

	const int maximum = fdBox->isChecked() ? FD_MAX_BYTES : CLASSIC_MAX_BYTES;
	if(data.size() > maximum) {
		problem = QStringLiteral("%1 bytes exceeds the %2-byte limit for %3")
			.arg(data.size()).arg(maximum)
			.arg(fdBox->isChecked() ? QStringLiteral("CAN FD") : QStringLiteral("classic CAN"));
		return false;
	}
	return true;
}

void TransmitPanel::sendOnce() {
	quint32 arbid = 0;
	QByteArray data;
	QString problem;
	if(!readFrame(arbid, data, problem)) {
		lastError->setText(QStringLiteral("<b>Not sent:</b> ") + problem);
		return;
	}
	lastError->clear();
	emit transmitRequested(arbid, data, extendedBox->isChecked(), fdBox->isChecked());
}

void TransmitPanel::togglePeriodic() {
	if(periodicActive) {
		periodicActive = false;
		periodicButton->setChecked(false);
		periodicButton->setText(QStringLiteral("Start periodic"));
		emit periodicRequested(false, 0, 0, QByteArray(), false, false);
		return;
	}

	quint32 arbid = 0;
	QByteArray data;
	QString problem;
	if(!readFrame(arbid, data, problem)) {
		periodicButton->setChecked(false);
		lastError->setText(QStringLiteral("<b>Not started:</b> ") + problem);
		return;
	}

	lastError->clear();
	periodicActive = true;
	periodicButton->setChecked(true);
	periodicButton->setText(QStringLiteral("Stop periodic"));
	emit periodicRequested(true, periodSpin->value(), arbid, data,
		extendedBox->isChecked(), fdBox->isChecked());
}

void TransmitPanel::noteResult(bool ok, const QString& detail) {
	if(ok) {
		++accepted;
	} else {
		++rejected;
		if(!detail.isEmpty())
			lastError->setText(QStringLiteral("<b>Last failure:</b> ") + detail);
	}
	updateCounters();
}

void TransmitPanel::resetCounters() {
	accepted = rejected = 0;
	lastError->clear();
	updateCounters();
}

void TransmitPanel::updateCounters() {
	counters->setText(QStringLiteral("accepted %1    rejected %2").arg(accepted).arg(rejected));
}
