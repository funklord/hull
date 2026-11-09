/*
 * machine_list.h -- the machines the GUI shows: those the user added, a
 * spec file each and the name of one of its machines where it holds
 * several, and those the core holds now, whoever started them.
 *
 * The added ones are the only thing saved, `QSettings` under
 * hull/hull-gui, and they are the user's choices: a file is listed
 * because somebody added it, never because it was found. A machine the
 * core holds is shown for as long as it does, saved by nobody.
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

QList<added_machine> load_added();
void                 save_added(const QList<added_machine> &list);

/*
 * The id a run of `machine` gets, by the CLI's rule (cli/hull.c,
 * machine_id()): its name where given, else its hostname, else its file's
 * directory's name, lowercased into a DNS label. `hostname` is the spec's,
 * "" where it has none. "" where nothing is left of any.
 */
QString machine_id_of(const added_machine &machine, const QString &hostname);

/* The hostname a `hull plan --resolved` line says, "" where none. */
QString hostname_in(const QString &resolved);

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
