/*
 * core_link_test.cpp -- the GUI's requests to the core, each against a
 * real core and a running machine: test/gui_test.sh starts both and gives
 * the machine's id. Each step prints a line and the test exits 1 at the
 * first that does not hold, so a failure says which request it was.
 *
 *   list     the machine is there, running, with an engine and an address
 *   stats    figures, the memory one above zero
 *   logs     what the machine wrote so far, ending in eof
 *   pause    done, and listed paused; unpause, done, listed running
 *   kill     a signal the core refuses, refused with its reason
 *   stop     done, and the machine listed exited, or no longer
 *   no core  with XDG_RUNTIME_DIR somewhere none listens: no_core
 */

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <functional>
#include <stdio.h>
#include <stdlib.h>

#include "core_link.h"

static int failures = 0;

/* Send, and run the loop until finished(); the outcome and reason. */
static core_request::outcome ask(const char *type, const QString &id, const QByteArray &rest,
                                 QString *reason, std::function<void(core_request *)> hook = {})
{
	QEventLoop            loop;
	core_request::outcome how = core_request::lost;
	core_request         *request = core_request::send(nullptr, type, id, rest, 20000);

	if (hook)
		hook(request);
	QObject::connect(request, &core_request::finished, &loop,
	                 [&](core_request::outcome o, const QString &r) {
		                 how = o;
		                 if (reason != nullptr)
			                 *reason = r;
		                 loop.quit();
	                 });
	loop.exec();
	return how;
}

static QList<machine_row> listed()
{
	QList<machine_row> rows;

	ask("(4:list", QString(), QByteArray(), nullptr, [&rows](core_request *request) {
		QObject::connect(request, &core_request::machines,
		                 [&rows](const QList<machine_row> &got) { rows = got; });
	});
	return rows;
}

static QString state_of(const QString &id)
{
	for (const machine_row &row : listed())
		if (row.id == id)
			return row.state;
	return QStringLiteral("(not listed)");
}

static void check(bool holds, const char *what, const QString &detail = QString())
{
	printf("%s: %s%s%s\n", holds ? "ok" : "FAIL", what, detail.isEmpty() ? "" : " -- ",
	       detail.toUtf8().constData());
	failures += !holds;
}

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	QString          reason;

	if (argc != 3) {
		fprintf(stderr, "usage: core_link_test ID EXPECTED-OUTPUT\n");
		return 2;
	}
	const QString    id = QString::fromLocal8Bit(argv[1]);
	const QByteArray expected = argv[2];

	{
		bool found = false;

		for (const machine_row &row : listed())
			if (row.id == id) {
				found = row.state == QStringLiteral("running") && !row.engine.isEmpty() &&
				        !row.address.isEmpty();
				reason = row.state + QLatin1Char(' ') + row.engine + QLatin1Char(' ') + row.address;
			}
		check(found, "list", reason);
	}
	{
		qint64 memory = -1;
		const core_request::outcome how =
		    ask("(5:stats", id, QByteArray(), &reason, [&memory](core_request *request) {
			    QObject::connect(request, &core_request::stats,
			                     [&memory](qint64, qint64, qint64 m) { memory = m; });
		    });
		check(how == core_request::done && memory > 0, "stats",
		      QStringLiteral("memory %1 %2").arg(memory).arg(reason));
	}
	{
		QByteArray said;
		const core_request::outcome how =
		    ask("(4:logs", id, QByteArray("(6:follow5:false)"), &reason,
		        [&said](core_request *request) {
			        QObject::connect(request, &core_request::output,
			                         [&said](int, const QByteArray &bytes) { said += bytes; });
		        });
		check(how == core_request::done && said.contains(expected), "logs",
		      QString::fromUtf8(said.left(200)));
	}
	check(ask("(5:pause", id, QByteArray(), &reason) == core_request::done, "pause", reason);
	check(state_of(id) == QStringLiteral("paused"), "listed paused", state_of(id));
	check(ask("(7:unpause", id, QByteArray(), &reason) == core_request::done, "unpause", reason);
	check(state_of(id) == QStringLiteral("running"), "listed running", state_of(id));
	check(ask("(4:kill", id, QByteArray("(6:signal5:NOPE!)"), &reason) == core_request::refused &&
	          !reason.isEmpty(),
	      "kill with a signal the core refuses", reason);
	check(ask("(4:stop", id, QByteArray("(10:timeout_ms4:2000)"), &reason) == core_request::done,
	      "stop", reason);
	/* Listed as exited until its run's frontend has gone, or not at all. */
	check(state_of(id) == QStringLiteral("exited") || state_of(id) == QStringLiteral("(not listed)"),
	      "ended after stop", state_of(id));

	qputenv("XDG_RUNTIME_DIR", "/nonexistent-hull-gui-test");
	check(ask("(4:list", QString(), QByteArray(), &reason) == core_request::no_core, "no core",
	      reason);
	return failures != 0;
}
