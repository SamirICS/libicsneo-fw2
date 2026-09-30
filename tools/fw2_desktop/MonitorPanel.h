// MonitorPanel -- the receive trace.
//
// Two views over the same stream, because the two questions are different. Scroll
// mode answers "what happened, in order" and is what you want while something is
// going wrong. Group mode collapses by arbitration ID and shows a count and a
// measured period, which is what you want to confirm a bus is behaving -- a
// scrolling view of a 500 Hz message is unreadable by construction.

#ifndef FW2_DESKTOP_MONITORPANEL_H
#define FW2_DESKTOP_MONITORPANEL_H

#include <QHash>
#include <QVector>
#include <QWidget>

#include "DeviceWorker.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QTableWidget;

class MonitorPanel : public QWidget {
	Q_OBJECT

public:
	explicit MonitorPanel(QWidget* parent = nullptr);

public slots:
	void addFrames(const QVector<FrameRecord>& frames);
	void noteDropped(quint64 total);
	void clear();

private slots:
	void changeMode(int index);
	void exportCSV();

private:
	enum class Mode { Scroll, Group };

	void applyMode();
	void appendScrollRow(const FrameRecord& frame);
	void updateGroupRow(const FrameRecord& frame);
	void updateStats();
	QString formatTimestamp(quint64 nanoseconds) const;

	/** Per-ID accumulator backing group mode. */
	struct GroupEntry {
		int row = 0;
		quint64 count = 0;
		quint64 lastTimestamp = 0;
		double periodMs = 0.0;
	};

	QTableWidget* table = nullptr;
	QComboBox* modeBox = nullptr;
	QCheckBox* pauseBox = nullptr;
	QLabel* stats = nullptr;

	Mode mode = Mode::Scroll;
	// Keyed on ID plus direction plus frame format, so a transmitted echo and a
	// received frame with the same ID stay distinguishable.
	QHash<quint64, GroupEntry> groups;

	quint64 totalReceived = 0;
	quint64 totalTransmitted = 0;
	quint64 totalErrors = 0;
	quint64 totalDropped = 0;
	quint64 firstTimestamp = 0;

	// Scroll mode keeps a bounded window. Past this the table costs more to
	// maintain than the history is worth; export is there for keeping it all.
	static constexpr int MAX_SCROLL_ROWS = 5000;
};

#endif
