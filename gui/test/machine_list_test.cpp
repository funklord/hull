/*
 * machine_list_test.cpp -- gui/src/machine_list.cpp held to what it
 * claims: the CLI's id rule, a written hull.conf that the real `hull plan`
 * reads back as it was meant, the fields the writer refuses, and the
 * saved list kept as it was given. Run by test/gui_test.sh, which gives
 * the hull binary and a scratch directory, and points XDG_CONFIG_HOME
 * there so the account's own list is never touched.
 */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>

#include <stdio.h>

#include "machine_list.h"

static int failures = 0;

static void check(bool holds, const char *what, const QString &detail = QString())
{
	printf("%s: %s%s%s\n", holds ? "ok" : "FAIL", what, holds || detail.isEmpty() ? "" : " -- ",
	       holds ? "" : detail.toUtf8().constData());
	failures += !holds;
}

/* `hull plan -f FILE --resolved`: its output, and its exit in `code`. */
static QString plan(const QString &hull, const QString &file, int *code)
{
	QProcess p;

	p.setProcessChannelMode(QProcess::MergedChannels);
	p.setWorkingDirectory(QFileInfo(file).absolutePath());
	p.start(hull, { QStringLiteral("plan"), QStringLiteral("-f"), file,
	                QStringLiteral("--resolved") });
	p.waitForFinished(20000);
	*code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
	return QString::fromUtf8(p.readAll());
}

static QString written(const QString &dir, const new_machine &m, int *code, const QString &hull)
{
	QString why;
	QString text = spec_text(m, &why);
	QFile   out(dir + QStringLiteral("/hull.conf"));

	if (text.isEmpty() || !out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		*code = -2;
		return why;
	}
	out.write(text.toUtf8());
	out.close();
	return plan(hull, out.fileName(), code);
}

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);

	if (argc != 3) {
		fprintf(stderr, "usage: machine_list_test HULL SCRATCH\n");
		return 2;
	}
	const QString hull = QString::fromLocal8Bit(argv[1]);
	const QString scratch = QString::fromLocal8Bit(argv[2]);

	/* The id: the name, else the hostname, else the directory, as a label. */
	check(machine_id_of({ QStringLiteral("/x/y/hull.conf"), QStringLiteral("Web_1") },
	                    QStringLiteral("other")) == QStringLiteral("web-1"),
	      "id from the name");
	check(machine_id_of({ QStringLiteral("/x/y/hull.conf"), QString() },
	                    QStringLiteral("Foo.Bar")) == QStringLiteral("foo-bar"),
	      "id from the hostname");
	check(machine_id_of({ QStringLiteral("/x/My  Dir!/hull.conf"), QString() }, QString()) ==
	          QStringLiteral("my-dir"),
	      "id from the directory",
	      machine_id_of({ QStringLiteral("/x/My  Dir!/hull.conf"), QString() }, QString()));
	check(machine_id_of({ QStringLiteral("/x/y/hull.conf"), QString(70, QLatin1Char('a')) },
	                    QString())
	              .size() == 63,
	      "id at most 63");
	check(hostname_in(QStringLiteral("(machine (source \"dir:.\") (hostname \"gui-a\") (x y))")) ==
	          QStringLiteral("gui-a"),
	      "hostname read from a resolved line");

	/* Written, then read back by the real planner. */
	QDir().mkpath(scratch + QStringLiteral("/plain/rootfs"));
	QDir().mkpath(scratch + QStringLiteral("/vm/rootfs"));
	QDir().mkpath(scratch + QStringLiteral("/screen/rootfs"));
	{
		new_machine m;
		int         code = 0;

		m.hostname = QStringLiteral("gui-new");
		m.source = QStringLiteral("dir:./rootfs");
		m.command = { QStringLiteral("/probe"), QStringLiteral("say"),
		              QStringLiteral("a \"quoted\" \\ word\tand\nlines") };
		const QString said = written(scratch + QStringLiteral("/plain"), m, &code, hull);

		check(code == 0, "a written chroot machine plans", said);
		check(said.contains(QStringLiteral(
		          "(command \"/probe\" \"say\" \"a \\\"quoted\\\" \\\\ word\\tand\\nlines\")")),
		      "its command read back as given", said);
		check(said.contains(QStringLiteral("(hostname \"gui-new\")")), "its hostname read back");
	}
	{
		new_machine m;
		int         code = 0;

		m.source = QStringLiteral("dir:./rootfs");
		m.backend = QStringLiteral("vm");
		m.memory = QStringLiteral("512M");
		m.cpus = 2;
		m.overlay = true;
		m.ssh = true;
		const QString said = written(scratch + QStringLiteral("/vm"), m, &code, hull);

		check(code == 0, "a written vm machine plans", said);
		for (const char *entry : { "(memory \"512M\")", "(cpus 2)", "(mutable overlay)",
		                           "(ssh true)" })
			check(said.contains(QString::fromLatin1(entry)), entry, said);
	}
	/* A screen for hull-gui, with 3D: what New's Screen and 3D write. */
	{
		new_machine m;
		int         code = 0;

		m.source = QStringLiteral("dir:./rootfs");
		m.backend = QStringLiteral("vm");
		m.display = QStringLiteral("local");
		m.gpu = QStringLiteral("virgl");
		m.sound = true;
		const QString said = written(scratch + QStringLiteral("/screen"), m, &code, hull);

		check(code == 0, "a written vm with a local 3D screen plans", said);
		for (const char *entry : { "(display local)", "(gpu virgl)", "(sound virtio)" })
			check(said.contains(QString::fromLatin1(entry)), entry, said);
	}

	/* What the writer refuses, each with a reason. */
	{
		new_machine m;
		QString     why;

		check(spec_text(m, &why).isEmpty() && !why.isEmpty(), "no source refused", why);
		m.source = QStringLiteral("dir:.");
		m.memory = QStringLiteral("lots");
		why.clear();
		check(spec_text(m, &why).isEmpty() && !why.isEmpty(), "memory not a size refused", why);
		m.memory.clear();
		m.backend = QStringLiteral("vm) (network host");
		why.clear();
		check(spec_text(m, &why).isEmpty() && !why.isEmpty(), "a backend not a word refused",
		      why);
		m.backend.clear();
		m.gpu = QStringLiteral("virgl");
		why.clear();
		check(spec_text(m, &why).isEmpty() && !why.isEmpty(), "3D with no screen refused", why);
		m.gpu.clear();
		m.sound = true;
		why.clear();
		check(spec_text(m, &why).isEmpty() && !why.isEmpty(), "sound with no screen refused", why);
		m.sound = false;
		m.command = { QStringLiteral("a\x01") };
		why.clear();
		check(spec_text(m, &why).isEmpty() && !why.isEmpty(),
		      "a control character refused", why);
	}

	/* The list, saved and loaded as it was given, a name kept apart. */
	{
		const QList<added_machine> list = {
			{ QStringLiteral("/a/hull.conf"), QString() },
			{ QStringLiteral("/b/hull.conf"), QStringLiteral("db") },
		};

		save_added(list);
		check(load_added() == list, "the list saved and loaded");
		save_added({});
		check(load_added().isEmpty(), "the list emptied");
	}
	return failures != 0;
}
