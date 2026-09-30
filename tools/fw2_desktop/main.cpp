// fw2_desktop -- a desktop console for the FreeWili 2.
//
// Freeware for FW2 customers, so there is deliberately no license check here.
// The only licensing constraint on this binary is Qt6 LGPLv3, which is satisfied
// by linking Qt6::Widgets dynamically -- the default for a shared Qt build, and
// what the CMakeLists in this directory does. libicsneo itself is MIT.

#include <QApplication>
#include <QColor>
#include <QPalette>

#include "MainWindow.h"

int main(int argc, char** argv) {
	QApplication app(argc, argv);

	app.setApplicationName(QStringLiteral("FreeWili 2 Console"));
	app.setOrganizationName(QStringLiteral("Intrepid"));

	app.setStyle(QStringLiteral("Fusion"));

	// A fixed light palette rather than the system theme. This is a bench tool
	// whose output gets screenshotted into bug reports, and the status colours --
	// green for error-active, red for bus-off -- need to stay legible regardless
	// of whatever desktop theme the machine happens to be running.
	QPalette palette;
	palette.setColor(QPalette::Window, QColor(240, 240, 240));
	palette.setColor(QPalette::WindowText, Qt::black);
	palette.setColor(QPalette::Base, Qt::white);
	palette.setColor(QPalette::AlternateBase, QColor(245, 245, 245));
	palette.setColor(QPalette::Text, Qt::black);
	palette.setColor(QPalette::Button, QColor(240, 240, 240));
	palette.setColor(QPalette::ButtonText, Qt::black);
	palette.setColor(QPalette::BrightText, Qt::red);
	palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
	palette.setColor(QPalette::HighlightedText, Qt::white);
	app.setPalette(palette);

	MainWindow window;
	window.show();

	return app.exec();
}
