/*
 * main.cpp -- hull-gui: hull's machine manager, as a window or, built
 * with qtty, the same window on a terminal (`--tui`, or `hull-tui`).
 *
 * One QWidget codebase for both, which is qtty's premise and the reason
 * hull's TUI is not a second program: the list, the details and the
 * actions are main_window's whichever frontend draws them.
 */

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "main_window.h"

#ifdef HULL_QTTY
#include <qtty/qtty.h>
#endif

#ifndef HULL_VERSION
#define HULL_VERSION "unknown"
#endif

/* Whether the terminal frontend was asked for: decided before
 * QApplication exists, since qtty chooses the platform it reads. */
static bool asked_for_tui(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--tui") == 0)
			return true;
		if (strcmp(argv[i], "--gui") == 0)
			return false;
	}
	if (QFileInfo(QString::fromLocal8Bit(argv[0])).fileName().endsWith(QStringLiteral("-tui")))
		return true;
#ifdef HULL_QTTY
	/* No display, on a terminal: the terminal is what there is. */
	return !qEnvironmentVariableIsSet("WAYLAND_DISPLAY") &&
	       !qEnvironmentVariableIsSet("DISPLAY") && isatty(1);
#else
	return false;
#endif
}

/* The hull this window runs: --hull, else the one beside it, as `make`
 * and `make install` put them, else hull on PATH. */
static QString hull_program(const QString &given)
{
	if (!given.isEmpty())
		return given;
	const QString beside = QCoreApplication::applicationDirPath() + QStringLiteral("/hull");

	if (QFileInfo(beside).isExecutable())
		return beside;
	const QString found = QStandardPaths::findExecutable(QStringLiteral("hull"));

	return found.isEmpty() ? QStringLiteral("hull") : found;
}

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--version") == 0) {
		printf("hull-gui %s\nCopyright (C) 2026 Nabeel Sowan <nabeel@vibes.se>\n",
		       HULL_VERSION);
		return 0;
	}
	const bool tui = asked_for_tui(argc, argv);
#ifdef HULL_QTTY
	if (tui)
		Qtty::prepare_environment();
#else
	if (tui) {
		fprintf(stderr, "hull-tui: this build has no terminal frontend: it is compiled in where "
		        "qtty is (vendor/qtty, `make gui` builds it); the window is hull-gui\n");
		return 2;
	}
#endif
	QApplication application(argc, argv);
#ifdef HULL_QTTY
	/* And a terminal's habits: Enter on the focused control, Up and Down
	 * between controls, Ctrl+PgUp/PgDn between tabs, Alt+letter to jump.
	 * qtty leaves them off for an application that binds those keys
	 * itself; this one binds none of them. */
	if (tui) {
		Qtty::setup(application);
		Qtty::set_keyboard_conventions(true);
	}
#endif
	QApplication::setApplicationName(QStringLiteral("hull-gui"));
	QApplication::setApplicationVersion(QStringLiteral(HULL_VERSION));

	QCommandLineParser parser;
	parser.setApplicationDescription(
	    QStringLiteral("hull's machine manager: the machines you added and those the core runs, "
	                   "started, stopped, paused and watched from one window."));
	parser.addHelpOption();
	QCommandLineOption hull_option(QStringLiteral("hull"),
	                               QStringLiteral("The hull to run machines with."),
	                               QStringLiteral("path"));
	QCommandLineOption tui_option(QStringLiteral("tui"),
	                              QStringLiteral("Draw on the terminal instead of the desktop."));
	QCommandLineOption gui_option(QStringLiteral("gui"),
	                              QStringLiteral("Draw on the desktop."));
	QCommandLineOption snapshot_option(
	    QStringLiteral("snapshot"),
	    QStringLiteral("With --tui: draw the window once the core and the plans have answered, "
	                   "COLSxROWS cells, as text on stdout, and exit. For tests."),
	    QStringLiteral("size"));
	parser.addOption(hull_option);
	parser.addOption(tui_option);
	parser.addOption(gui_option);
	QCommandLineOption picture_option(
	    QStringLiteral("picture"),
	    QStringLiteral("Draw the window once the core and the plans have answered, as a PNG "
	                   "in FILE, and exit. For tests and pictures of it."),
	    QStringLiteral("file"));
	parser.addOption(snapshot_option);
	parser.addOption(picture_option);
	parser.addPositionalArgument(QStringLiteral("file"),
	                             QStringLiteral("A hull.conf to add to the list."),
	                             QStringLiteral("[file...]"));
	parser.process(application);

	main_window window(hull_program(parser.value(hull_option)));
	for (const QString &file : parser.positionalArguments())
		window.add_machine(file, QString());

	if (parser.isSet(picture_option)) {
		const QString file = parser.value(picture_option);
		QTimer       *wait = new QTimer(&window);
		int           waited = 0;

		/* As --snapshot waits: the plans run, and a list asked after. */
		QObject::connect(wait, &QTimer::timeout, &window, [&window, wait, &waited, file]() {
			bool busy = false;

			for (QProcess *p : window.findChildren<QProcess *>())
				busy = busy || p->state() != QProcess::NotRunning;
			if (busy && ++waited < 100)
				return;
			wait->stop();
			window.refresh([&window, file]() {
				QCoreApplication::exit(window.grab().save(file, "PNG") ? 0 : 1);
			});
		});
		window.resize(900, 560);
		window.show();
		wait->start(100);
		return application.exec();
	}
	if (parser.isSet(snapshot_option)) {
#ifdef HULL_QTTY
		const QStringList size = parser.value(snapshot_option).split(QLatin1Char('x'));
		const int         cols = size.value(0).toInt();
		const int         rows = size.value(1).toInt();

		if (!tui || cols < 20 || rows < 5) {
			fprintf(stderr, "hull-gui: --snapshot needs --tui and a size such as 100x30\n");
			return 2;
		}
		/* Once nothing this window started is still running -- its plans --
		 * and the core has answered a list asked after them. */
		QTimer *wait = new QTimer(&window);
		int     waited = 0;

		QObject::connect(wait, &QTimer::timeout, &window, [&window, wait, &waited, cols, rows]() {
			bool busy = false;

			for (QProcess *p : window.findChildren<QProcess *>())
				busy = busy || p->state() != QProcess::NotRunning;
			if (busy && ++waited < 100)
				return;
			wait->stop();
			window.refresh([&window, cols, rows]() {
				window.resize(Qtty::GridMetrics::cells(cols, rows));
				fputs(Qtty::test::snapshot_of(window, cols, rows).toUtf8().constData(), stdout);
				fflush(stdout);
				QCoreApplication::exit(0);
			});
		});
		wait->start(100);
		window.show();
		return application.exec();
#else
		fprintf(stderr, "hull-gui: --snapshot needs a build with qtty\n");
		return 2;
#endif
	}
#ifdef HULL_QTTY
	if (tui)
		return Qtty::exec(application, window);
#endif
	window.resize(900, 560);
	window.show();
	return application.exec();
}
