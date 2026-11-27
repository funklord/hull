/*
 * machine_list.h -- the machines the GUI shows: those the user added, a
 * spec file each and the name of one of its machines where it holds
 * several, and those the core holds now, whoever started them.
 *
 * The added ones are saved, `QSettings` under hull/hull-gui, and they
 * are the user's choices. The one place a file is found rather than
 * added is the machines' home, ~/hull, as VirtualBox's manager shows the
 * machines of its own folder: a NAME/hull.conf there is listed, and
 * added, unless the user removed it from the list, which is remembered
 * as that choice. A machine the core holds is shown for as long as it
 * does, saved by nobody.
 */

#ifndef HULL_GUI_MACHINE_LIST_H
#define HULL_GUI_MACHINE_LIST_H

#include <QList>
#include <QString>
#include <QStringList>

/* One added machine: `file`, its hull.conf, and `name` where the file's
 * (machines ...) holds more than one, as `hull run -f FILE NAME` takes. */
struct added_machine {
	QString file;
	QString name;

	bool operator==(const added_machine &other) const
	{
		return file == other.file && name == other.name;
	}
};

/* Where machines are kept, one folder each: ~/hull, as VirtualBox keeps
 * ~/VirtualBox VMs. */
QString machines_home();

QList<added_machine> load_added();
void                 save_added(const QList<added_machine> &list);

/* The machines' home's files the user took off the list, by path. */
QStringList load_removed();
void        save_removed(const QStringList &files);

/* Each HOME/NAME/hull.conf, in name order, neither in `added` nor in
 * `removed`: the machines found in the home. */
QList<added_machine> found_in(const QString &home, const QList<added_machine> &added,
                              const QStringList &removed);

/*
 * The id a run of `machine` gets, by the CLI's rule (cli/hull.c,
 * machine_id()): its name where given, else its hostname, else its file's
 * directory's name, lowercased into a DNS label. `hostname` is the spec's,
 * "" where it has none. "" where nothing is left of any.
 */
QString machine_id_of(const added_machine &machine, const QString &hostname);

/* A clone's hull.conf: `text` with its one `(hostname "...")` saying
 * `hostname`, so the copy's id is not the original's; `text` as it is
 * where it names none, its id then its folder's. "" where it names more
 * than one, a file of several machines, which Clone does not copy. */
QString with_hostname(const QString &text, const QString &hostname);

/*
 * Settings' editor, a file of one (machine ...) changed in one place and
 * nowhere else, its comments and layout kept: `path` the forms' heads
 * from the machine down, {"memory"} or {"tuning", "pc", "nic"}.
 *
 * spec_value: what follows the head of the form at `path`, as written --
 * `"2G"`, `e1000` -- or "" where the file says nothing there, the
 * default holding.
 *
 * spec_with: `text` with that form saying `value`, made with the forms
 * above it where they are missing; with `value` "" the form taken out,
 * so the default holds again. "" with `why` said where the file is not
 * one machine's.
 */
QString spec_value(const QString &text, const QStringList &path);

/* The forms inside the form at `path`, each its text as written -- a
 * (media ...)'s (medium ...) records -- in order; none where it is absent. */
QStringList spec_forms(const QString &text, const QStringList &path);
QString spec_with(const QString &text, const QStringList &path, const QString &value,
                  QString *why);

/* The hostname a `hull plan --resolved` line says, "" where none. */
QString hostname_in(const QString &resolved);

/* The segments a `hull plan --resolved` line names, `(segments "a" (b
 * ...))`, each once, in order; none where it names none. */
QStringList segments_in(const QString &resolved);

/* A machine as New writes it: each field left empty, 0 or false is left
 * out of the file, so the spec's default holds and says so in the plan. */
struct new_machine {
	QString     hostname;
	QString     source;
	QString     backend;   /* a class or an engine, "" for the default */
	QStringList command;
	QString     memory;
	int         cpus = 0;
	bool        overlay = false;
	bool        ssh = false;
	QString     display;   /* "", window, vnc or local: (display ...) */
	QString     gpu;       /* "", virgl or venus: (tuning (qemu (gpu ...))) */
	bool        sound = false;  /* a pc's (sound hda), else qemu's (sound virtio) */
	/* A pc as VirtualBox's New makes one: an installer disc it boots
	 * from, and a new disk of this many GiB, ./disk.qcow2 beside its
	 * file, which the caller makes. Either replaces `source`. */
	QString     installer;
	int         disk_gigabytes = 0;
};

/*
 * `machine` as a hull.conf's text, in the readable syntax of section 4.1:
 * strings with the reader's four escapes, `\\`, `\"`, `\n` and `\t`. ""
 * where a field could not be written as the spec's type -- a backend or
 * memory that is not a word, a control character in a string -- and
 * `why` then says which.
 */
QString spec_text(const new_machine &machine, QString *why);

#endif
