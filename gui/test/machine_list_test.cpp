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
	/* Machines are kept under ~/hull, one folder each. */
	check(machines_home() == QDir::homePath() + QStringLiteral("/hull"),
	      "machines are kept in ~/hull", machines_home());
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

	/* A pc's 3D and sound in its own (tuning (pc ...)). */
	{
		new_machine m;
		QString     why;

		m.source = QStringLiteral("image:./disk.img");
		m.backend = QStringLiteral("pc");
		m.display = QStringLiteral("vnc");
		m.gpu = QStringLiteral("virgl");
		m.sound = true;
		const QString text = spec_text(m, &why);

		check(text.contains(QStringLiteral("(tuning (pc (sound hda) (gpu virgl)))")),
		      "a pc's 3D written as its own tuning", text + why);
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

	/* Settings' editor: one form read, set, made or taken out, the rest of
	 * the file -- its comment, its layout -- byte for byte as it was. */
	{
		const QString xp = QStringLiteral("; XP, kept here\n"
		                                  "(machine (backend pc) (hostname \"winxp\")\n"
		                                  "  (memory \"1024M\") (cpus 6)\n"
		                                  "  (command \"a)b\")\n"
		                                  "  (tuning (pc (machine pc) (nic e1000)))\n"
		                                  "  (mutable direct))\n");
		QString why;

		check(spec_value(xp, { QStringLiteral("memory") }) == QStringLiteral("\"1024M\""),
		      "a value read", spec_value(xp, { QStringLiteral("memory") }));
		check(spec_value(xp, { QStringLiteral("tuning"), QStringLiteral("pc"), QStringLiteral("nic") }) ==
		          QStringLiteral("e1000"), "a nested value read");
		check(spec_value(xp, { QStringLiteral("tuning"), QStringLiteral("pc"), QStringLiteral("sound") })
		          .isEmpty(), "an absent one, the default");
		check(spec_value(QStringLiteral("(machine ; (memory \"9G\")\n (backend pc))"),
		                 { QStringLiteral("memory") }).isEmpty(), "a form in a comment is no form");
		check(spec_with(xp, { QStringLiteral("memory") }, QStringLiteral("\"2G\""), &why) ==
		          QString(xp).replace(QStringLiteral("\"1024M\""), QStringLiteral("\"2G\"")),
		      "a value replaced, nothing else", spec_with(xp, { QStringLiteral("memory") },
		                                                  QStringLiteral("\"2G\""), &why));
		check(spec_with(xp, { QStringLiteral("tuning"), QStringLiteral("pc"), QStringLiteral("sound") },
		                QStringLiteral("ac97"), &why) ==
		          QString(xp).replace(QStringLiteral("(nic e1000)"),
		                              QStringLiteral("(nic e1000) (sound ac97)")),
		      "a form made in its parent");
		check(spec_with(xp, { QStringLiteral("network") }, QStringLiteral("nat"), &why) ==
		          QString(xp).replace(QStringLiteral("(mutable direct))"),
		                              QStringLiteral("(mutable direct)\n  (network nat))")),
		      "a form made in the machine, on a line of its own",
		      spec_with(xp, { QStringLiteral("network") }, QStringLiteral("nat"), &why));
		check(spec_with(xp, { QStringLiteral("mutable") }, QString(), &why) ==
		          QString(xp).replace(QStringLiteral("\n  (mutable direct))"), QStringLiteral(")")),
		      "a last form taken out, the paren rejoining its line",
		      spec_with(xp, { QStringLiteral("mutable") }, QString(), &why));
		check(spec_with(xp, { QStringLiteral("cpus") }, QString(), &why) ==
		          QString(xp).replace(QStringLiteral(" (cpus 6)"), QString()),
		      "a form beside another taken out", spec_with(xp, { QStringLiteral("cpus") },
		                                                     QString(), &why));
		check(spec_with(xp, { QStringLiteral("command") }, QString(), &why) ==
		          QString(xp).replace(QStringLiteral("  (command \"a)b\")\n"), QString()),
		      "a form alone on its line taken out with the line, a paren in its string");
		check(spec_with(xp, { QStringLiteral("sound") }, QString(), &why) == xp,
		      "an absent form taken out: nothing changes");
		check(spec_with(QStringLiteral("(machine (backend pc))\n"),
		                { QStringLiteral("tuning"), QStringLiteral("pc"), QStringLiteral("sound") },
		                QStringLiteral("hda"), &why) ==
		          QStringLiteral("(machine (backend pc)\n  (tuning (pc (sound hda))))\n"),
		      "the forms above it made too");
		why.clear();
		check(spec_with(QStringLiteral("(machines (a (backend pc)))"), { QStringLiteral("memory") },
		                QStringLiteral("\"1G\""), &why).isEmpty() && why.contains(QStringLiteral("several")),
		      "a file of several machines refused", why);
	}

	/* Settings' disks and discs: the (media ...) records listed, and the
	 * form rewritten whole from a list, a record a line. */
	{
		const QString pc = QStringLiteral("(machine (backend pc)\n"
		                                  "  (media\n"
		                                  "    (medium (type disk) (source \"image:./a.qcow2\"))\n"
		                                  "    ; the installer\n"
		                                  "    (medium (type cdrom) (source \"image:./b (1).iso\"))))\n");
		const QStringList forms = spec_forms(pc, { QStringLiteral("media") });
		QString why;

		check(forms == QStringList{ QStringLiteral("(medium (type disk) (source \"image:./a.qcow2\"))"),
		                            QStringLiteral("(medium (type cdrom) (source \"image:./b (1).iso\"))") },
		      "the media's records listed, past a comment", forms.join(QLatin1Char('|')));
		check(spec_forms(pc, { QStringLiteral("volumes") }).isEmpty(), "no form, no records");
		check(spec_with(pc, { QStringLiteral("media") }, forms[0], &why) ==
		          QStringLiteral("(machine (backend pc)\n"
		                         "  (media (medium (type disk) (source \"image:./a.qcow2\"))))\n"),
		      "the disc removed from the media", spec_with(pc, { QStringLiteral("media") }, forms[0], &why));
	}

	/* A clone's hostname: the one form replaced, its quotes and
	 * backslashes escaped; none, the text as it was; two, refused. */
	{
		const QString one = QStringLiteral("; a (comment)\n(machine (backend pc) (hostname \"winxp\")\n"
		                                   "  (memory \"1G\"))\n");

		check(with_hostname(one, QStringLiteral("winxp-clone")) ==
		          QStringLiteral("; a (comment)\n(machine (backend pc) (hostname \"winxp-clone\")\n"
		                         "  (memory \"1G\"))\n"),
		      "a clone's hostname", with_hostname(one, QStringLiteral("winxp-clone")));
		check(with_hostname(QStringLiteral("(machine (hostname \"a\\\"b\"))"), QStringLiteral("q\"r")) ==
		          QStringLiteral("(machine (hostname \"q\\\"r\"))"),
		      "an escaped hostname read and written",
		      with_hostname(QStringLiteral("(machine (hostname \"a\\\"b\"))"), QStringLiteral("q\"r")));
		check(with_hostname(QStringLiteral("(machine (backend pc))"), QStringLiteral("x")) ==
		          QStringLiteral("(machine (backend pc))"),
		      "no hostname, the folder's name serves");
		check(with_hostname(QStringLiteral("(machines (a (hostname \"a\")) (b (hostname \"b\")))"),
		                    QStringLiteral("x")).isEmpty(),
		      "two hostnames refused");
	}

	/* The machines' home: each NAME/hull.conf found, in name order; one
	 * added already, one removed, a folder with no hull.conf and a file
	 * beside the folders passed over. */
	{
		const QString home = scratch + QStringLiteral("/home-hull");

		for (const char *folder : { "b", "a", "added", "gone", "empty" })
			QDir().mkpath(home + QLatin1Char('/') + QLatin1String(folder));
		for (const char *folder : { "b", "a", "added", "gone" }) {
			QFile conf(home + QLatin1Char('/') + QLatin1String(folder) +
			           QStringLiteral("/hull.conf"));

			conf.open(QIODevice::WriteOnly);
		}
		QFile stray(home + QStringLiteral("/hull.conf"));

		stray.open(QIODevice::WriteOnly);
		const QList<added_machine> found =
		    found_in(home, { { home + QStringLiteral("/added/hull.conf"), QStringLiteral("db") } },
		             { home + QStringLiteral("/gone/hull.conf") });

		check(found.size() == 2 && found[0].file == home + QStringLiteral("/a/hull.conf") &&
		          found[1].file == home + QStringLiteral("/b/hull.conf") && found[0].name.isEmpty(),
		      "the home's machines found, the added and removed passed over",
		      found.isEmpty() ? QString() : found[0].file);
		check(found_in(scratch + QStringLiteral("/no-such-home"), {}, {}).isEmpty(),
		      "no home, nothing found");
		save_removed({ QStringLiteral("/h/x/hull.conf") });
		check(load_removed() == QStringList{ QStringLiteral("/h/x/hull.conf") },
		      "a removal remembered");
		save_removed({});
		check(load_removed().isEmpty(), "and forgotten");
	}
	return failures != 0;
}
