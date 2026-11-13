/*
 * screen_window_test.cpp -- hull-gui's screen window on a running vm's
 * D-Bus display: its first frame, by qemu's shared memory rather than a
 * copy; a key typed into it, which the guest reads from its own keyboard;
 * and a new size, which the guest takes as its preferred mode
 * (test/screen_test.sh looks at both).
 *
 * Usage: screen_window_test MONITOR SECONDS. Prints `frame WxH shared` or
 * `frame WxH copied`, then `typed` and `resized`, and exits 0; exits 1
 * saying why.
 * Every wait ends at SECONDS. Run with QT_QPA_PLATFORM=offscreen.
 */

#include "screen_window.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>

#include <stdio.h>

int main(int argc, char **argv)
{
	QApplication  application(argc, argv);
	QElapsedTimer clock;
	QString       why;

	if (argc != 3) {
		fprintf(stderr, "usage: screen_window_test MONITOR SECONDS\n");
		return 2;
	}
	const qint64 limit = QString::fromLocal8Bit(argv[2]).toLongLong() * 1000;
	screen_window window(QStringLiteral("test"), QString::fromLocal8Bit(argv[1]));

	window.setAttribute(Qt::WA_DeleteOnClose, false);
	if (!window.open(&why)) {
		fprintf(stderr, "screen_window_test: %s\n", why.toLocal8Bit().constData());
		return 1;
	}
	window.show();
	clock.start();
	while (window.frame_size().isEmpty() && clock.elapsed() < limit)
		application.processEvents(QEventLoop::WaitForMoreEvents, 100);
	if (window.frame_size().isEmpty()) {
		fprintf(stderr, "screen_window_test: no frame\n");
		return 1;
	}
	printf("frame %dx%d %s\n", window.frame_size().width(), window.frame_size().height(),
	       window.shared() ? "shared" : "copied");
	fflush(stdout);

	/* `a`, evdev's 30, as xcb gives it: plus 8. */
	QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, 38u, 0u, 0u, QStringLiteral("a"));
	QKeyEvent release(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier, 38u, 0u, 0u, QStringLiteral("a"));
	QApplication::sendEvent(&window, &press);
	QApplication::sendEvent(&window, &release);
	clock.restart();
	while (clock.elapsed() < 1000)
		application.processEvents(QEventLoop::WaitForMoreEvents, 100);
	printf("typed\n");

	/* A new size, told to the guest once it settles. */
	window.resize(1000, 700);
	clock.restart();
	while (clock.elapsed() < 1500)
		application.processEvents(QEventLoop::WaitForMoreEvents, 100);
	printf("resized\n");
	return 0;
}
