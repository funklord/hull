/*
 * machine_list.cpp -- see machine_list.h.
 */

#include "machine_list.h"

#include <QDir>

#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>

static QSettings &settings()
{
	static QSettings stored(QStringLiteral("hull"), QStringLiteral("hull-gui"));

	return stored;
}

QString machines_home()
{
	return QDir::homePath() + QStringLiteral("/hull");
}

QList<added_machine> load_added()
{
	QList<added_machine> list;
	QSettings           &s = settings();
	const int            count = s.beginReadArray(QStringLiteral("machines"));

	for (int i = 0; i < count; i++) {
		added_machine machine;

		s.setArrayIndex(i);
		machine.file = s.value(QStringLiteral("file")).toString();
		machine.name = s.value(QStringLiteral("name")).toString();
		if (!machine.file.isEmpty() && !list.contains(machine))
			list.append(machine);
	}
	s.endArray();
	return list;
}

void save_added(const QList<added_machine> &list)
{
	QSettings &s = settings();

	s.remove(QStringLiteral("machines"));
	s.beginWriteArray(QStringLiteral("machines"), (int)list.size());
	for (int i = 0; i < list.size(); i++) {
		s.setArrayIndex(i);
		s.setValue(QStringLiteral("file"), list[i].file);
		if (!list[i].name.isEmpty())
			s.setValue(QStringLiteral("name"), list[i].name);
	}
	s.endArray();
	s.sync();
}

/* --- Settings' editor ------------------------------------------------ */

namespace {

/* The first position at or after `i` that is not space or a comment. */
int skip_space(const QString &text, int i)
{
	while (i < text.size()) {
		if (text[i].isSpace()) {
			i++;
		} else if (text[i] == QLatin1Char(';')) {
			while (i < text.size() && text[i] != QLatin1Char('\n'))
				i++;
		} else {
			break;
		}
	}
	return i;
}

/* Past the atom or string at `i`. */
int skip_atom(const QString &text, int i)
{
	if (i < text.size() && text[i] == QLatin1Char('"')) {
		for (i++; i < text.size() && text[i] != QLatin1Char('"'); i++)
			if (text[i] == QLatin1Char('\\'))
				i++;
		return i < text.size() ? i + 1 : -1;
	}
	while (i < text.size() && !text[i].isSpace() && text[i] != QLatin1Char('(') &&
	       text[i] != QLatin1Char(')') && text[i] != QLatin1Char(';'))
		i++;
	return i;
}

/* Past the form opening at `open`, its ')' the one before; -1 unclosed. */
int skip_form(const QString &text, int open)
{
	int i = open + 1;

	for (;;) {
		i = skip_space(text, i);
		if (i >= text.size())
			return -1;
		if (text[i] == QLatin1Char(')'))
			return i + 1;
		i = text[i] == QLatin1Char('(') ? skip_form(text, i) : skip_atom(text, i);
		if (i < 0)
			return -1;
	}
}

/* The head of the form opening at `open`. */
QString head_at(const QString &text, int open)
{
	const int start = skip_space(text, open + 1);
	const int end = skip_atom(text, start);

	return end > start ? text.mid(start, end - start) : QString();
}

/* The child of the form at `open` whose head is `head`: its open, or -1. */
int child_of(const QString &text, int open, const QString &head)
{
	int i = skip_atom(text, skip_space(text, open + 1));

	while (i >= 0) {
		i = skip_space(text, i);
		if (i >= text.size() || text[i] == QLatin1Char(')'))
			return -1;
		if (text[i] == QLatin1Char('(')) {
			if (head_at(text, i) == head)
				return i;
			i = skip_form(text, i);
		} else {
			i = skip_atom(text, i);
		}
	}
	return -1;
}

/* The (machine ...) form's open, or -1 with `why`. */
int machine_form(const QString &text, QString *why)
{
	const int i = skip_space(text, 0);

	if (i < text.size() && text[i] == QLatin1Char('(') &&
	    head_at(text, i) == QStringLiteral("machine") && skip_form(text, i) > 0)
		return i;
	if (why != nullptr)
		*why = i < text.size() && head_at(text, i) == QStringLiteral("machines") ?
		       QStringLiteral("the file holds several machines; Settings edits one") :
		       QStringLiteral("the file is not one (machine ...) form");
	return -1;
}

} // namespace

QString spec_value(const QString &text, const QStringList &path)
{
	int at = machine_form(text, nullptr);

	for (const QString &head : path) {
		if (at < 0)
			return QString();
		at = child_of(text, at, head);
	}
	if (at < 0)
		return QString();
	const int start = skip_space(text, skip_atom(text, skip_space(text, at + 1)));
	const int end = skip_form(text, at) - 1;

	return text.mid(start, end - start).trimmed();
}

QStringList spec_forms(const QString &text, const QStringList &path)
{
	QStringList forms;
	int         at = machine_form(text, nullptr);

	for (const QString &head : path) {
		if (at < 0)
			return forms;
		at = child_of(text, at, head);
	}
	if (at < 0)
		return forms;
	for (int i = skip_atom(text, skip_space(text, at + 1)); i >= 0;) {
		i = skip_space(text, i);
		if (i >= text.size() || text[i] == QLatin1Char(')'))
			break;
		if (text[i] == QLatin1Char('(')) {
			const int end = skip_form(text, i);

			if (end < 0)
				break;
			forms << text.mid(i, end - i);
			i = end;
		} else {
			i = skip_atom(text, i);
		}
	}
	return forms;
}

QString spec_with(const QString &text, const QStringList &path, const QString &value,
                  QString *why)
{
	int     parent = machine_form(text, why);
	int     depth = 0;
	QString result = text;

	if (parent < 0 || path.isEmpty())
		return QString();
	/* Down as far as the forms are there. */
	for (; depth < path.size(); depth++) {
		const int child = child_of(text, parent, path[depth]);

		if (child < 0)
			break;
		parent = child;
	}
	if (depth == path.size()) {
		const int end = skip_form(text, parent);

		if (!value.isEmpty())
			return result.replace(parent, end - parent,
			                      QStringLiteral("(%1 %2)").arg(path.last(), value));
		/* Out with the spaces before it; where it began a line, the line
		 * too if nothing else is on it, or else the newline before it, so
		 * a closing paren after it rejoins the line above. */
		int from = parent;
		int to = end;

		while (from > 0 && (text[from - 1] == QLatin1Char(' ') || text[from - 1] == QLatin1Char('\t')))
			from--;
		if (from > 0 && text[from - 1] == QLatin1Char('\n')) {
			int after = end;

			while (after < text.size() &&
			       (text[after] == QLatin1Char(' ') || text[after] == QLatin1Char('\t')))
				after++;
			if (after < text.size() && text[after] == QLatin1Char('\n'))
				to = after + 1;
			else
				from--;
		}
		return result.remove(from, to - from);
	}
	if (value.isEmpty())
		return result;
	/* The missing forms made, innermost first, and put before the closing
	 * paren of the deepest one there: on a line of its own in the machine,
	 * after a space below it. */
	QString made = QStringLiteral("(%1 %2)").arg(path.last(), value);

	for (int i = path.size() - 2; i >= depth; i--)
		made = QStringLiteral("(%1 %2)").arg(path[i], made);
	const int close = skip_form(text, parent) - 1;

	return result.insert(close, depth == 0 ? QStringLiteral("\n  ") + made :
	                                          QLatin1Char(' ') + made);
}

QString with_hostname(const QString &text, const QString &hostname)
{
	static const QRegularExpression form(QStringLiteral("\\(hostname\\s+\"(?:[^\"\\\\]|\\\\.)*\"\\)"));
	QRegularExpressionMatchIterator it = form.globalMatch(text);
	QRegularExpressionMatch         first;
	int                             count = 0;
	QString                         escaped = hostname;

	while (it.hasNext()) {
		QRegularExpressionMatch match = it.next();

		if (count++ == 0)
			first = match;
	}
	if (count == 0)
		return text;
	if (count > 1)
		return QString();
	escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
	escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
	return QString(text).replace(first.capturedStart(), first.capturedLength(),
	                             QStringLiteral("(hostname \"%1\")").arg(escaped));
}

QStringList load_removed()
{
	return settings().value(QStringLiteral("removed")).toStringList();
}

void save_removed(const QStringList &files)
{
	QSettings &s = settings();

	if (files.isEmpty())
		s.remove(QStringLiteral("removed"));
	else
		s.setValue(QStringLiteral("removed"), files);
	s.sync();
}

QList<added_machine> found_in(const QString &home, const QList<added_machine> &added,
                              const QStringList &removed)
{
	QList<added_machine> found;
	const QStringList    folders = QDir(home).entryList(QDir::Dirs | QDir::NoDotAndDotDot,
	                                                    QDir::Name);

	for (const QString &folder : folders) {
		added_machine machine;
		bool          listed = false;

		machine.file = QDir(home).absoluteFilePath(folder + QStringLiteral("/hull.conf"));
		if (!QFileInfo(machine.file).isFile() || removed.contains(machine.file))
			continue;
		/* Listed already under any of its machines' names. */
		for (const added_machine &a : added)
			listed = listed || a.file == machine.file;
		if (!listed)
			found.append(machine);
	}
	return found;
}

QString machine_id_of(const added_machine &machine, const QString &hostname)
{
	QString from = machine.name;
	QString id;

	if (from.isEmpty())
		from = hostname;
	if (from.isEmpty())
		from = QFileInfo(QFileInfo(machine.file).absolutePath()).fileName();
	for (QChar c : from) {
		char ascii = c.toLatin1();

		if (ascii >= 'A' && ascii <= 'Z')
			ascii = (char)(ascii - 'A' + 'a');
		if (!((ascii >= 'a' && ascii <= 'z') || (ascii >= '0' && ascii <= '9')))
			ascii = '-';
		if (ascii == '-' && (id.isEmpty() || id.endsWith(QLatin1Char('-'))))
			continue;
		if (id.size() == 63)
			break;
		id.append(QLatin1Char(ascii));
	}
	while (id.endsWith(QLatin1Char('-')))
		id.chop(1);
	return id;
}

/* `text` as a readable string; `*ok` cleared where it holds a control
 * character the reader has no escape for. */
static QString quoted(const QString &text, bool *ok)
{
	QString out = QStringLiteral("\"");

	for (QChar c : text) {
		if (c == QLatin1Char('\\') || c == QLatin1Char('"'))
			out += QLatin1Char('\\') + QString(c);
		else if (c == QLatin1Char('\n'))
			out += QStringLiteral("\\n");
		else if (c == QLatin1Char('\t'))
			out += QStringLiteral("\\t");
		else if (c.unicode() < 0x20 || c.unicode() == 0x7f)
			*ok = false;
		else
			out += c;
	}
	return out + QLatin1Char('"');
}

QString spec_text(const new_machine &machine, QString *why)
{
	static const QRegularExpression word(QStringLiteral("^[a-z][a-z0-9-]*$"));
	static const QRegularExpression size(QStringLiteral("^[0-9]+[KMGT]?$"));
	bool    ok = true;
	QString text = QStringLiteral("(machine\n");

	const bool own_media = machine.backend == QStringLiteral("pc") &&
	                       (!machine.installer.isEmpty() || machine.disk_gigabytes > 0);

	if ((!machine.installer.isEmpty() || machine.disk_gigabytes > 0) && !own_media) {
		*why = QStringLiteral("an installer disc and a new disk are a pc's: choose the pc backend");
		return QString();
	}
	if (machine.source.isEmpty() && !own_media) {
		*why = QStringLiteral("a machine needs a source");
		return QString();
	}
	if (!machine.backend.isEmpty() && !word.match(machine.backend).hasMatch()) {
		*why = QStringLiteral("a backend is a word: ns, vm, emu, pc, dos, or an engine's name");
		return QString();
	}
	if (!machine.display.isEmpty() && !word.match(machine.display).hasMatch()) {
		*why = QStringLiteral("a screen is a word: window, vnc or local");
		return QString();
	}
	if (!machine.gpu.isEmpty() && (!word.match(machine.gpu).hasMatch() || machine.display.isEmpty())) {
		*why = QStringLiteral("3D is a screen's: choose one, in hull-gui (local) for 3D shown here");
		return QString();
	}
	if (machine.sound && machine.display.isEmpty()) {
		*why = QStringLiteral("sound is heard where a screen is shown: choose a screen");
		return QString();
	}
	if (!machine.memory.isEmpty() && !size.match(machine.memory).hasMatch()) {
		*why = QStringLiteral("memory is a size such as 512M or 2G");
		return QString();
	}
	if (!machine.hostname.isEmpty())
		text += QStringLiteral("  (hostname %1)\n").arg(quoted(machine.hostname, &ok));
	if (!machine.backend.isEmpty())
		text += QStringLiteral("  (backend %1)\n").arg(machine.backend);
	/* A PC of its own media boots its installer while there is one, and
	 * keeps what is installed, as VirtualBox's machine does: Settings'
	 * Boot from moves it to the disk after. */
	if (own_media) {
		text += QStringLiteral("  (source \"media:%1\")\n")
		            .arg(machine.installer.isEmpty() ? QStringLiteral("disk") : QStringLiteral("cdrom"));
		text += QStringLiteral("  (mutable direct)\n");
	} else {
		text += QStringLiteral("  (source %1)\n").arg(quoted(machine.source, &ok));
	}
	if (machine.overlay)
		text += QStringLiteral("  (mutable overlay)\n");
	if (!machine.command.isEmpty()) {
		QStringList words;

		for (const QString &arg : machine.command)
			words << quoted(arg, &ok);
		text += QStringLiteral("  (command %1)\n").arg(words.join(QLatin1Char(' ')));
	}
	if (!machine.memory.isEmpty())
		text += QStringLiteral("  (memory \"%1\")\n").arg(machine.memory);
	if (machine.cpus > 0)
		text += QStringLiteral("  (cpus %1)\n").arg(machine.cpus);
	if (machine.ssh)
		text += QStringLiteral("  (ssh true)\n");
	if (!machine.display.isEmpty())
		text += QStringLiteral("  (display %1)\n").arg(machine.display);
	if (own_media) {
		QString media;

		if (machine.disk_gigabytes > 0)
			media += QStringLiteral("\n    (medium (type disk) (source \"image:./disk.qcow2\"))");
		if (!machine.installer.isEmpty())
			media += QStringLiteral("\n    (medium (type cdrom) (source %1))")
			             .arg(quoted(QStringLiteral("image:") + machine.installer, &ok));
		text += QStringLiteral("  (network nat)\n  (media%1)\n").arg(media);
	}
	/* One (tuning ...): a pc's keys are its own, a vm's qemu's. */
	if (machine.backend == QStringLiteral("pc")) {
		QString pc;

		if (machine.sound)
			pc += QStringLiteral(" (sound hda)");
		if (!machine.gpu.isEmpty())
			pc += QStringLiteral(" (gpu %1)").arg(machine.gpu);
		if (!pc.isEmpty())
			text += QStringLiteral("  (tuning (pc%1))\n").arg(pc);
	} else if (!machine.gpu.isEmpty() || machine.sound) {
		QString qemu;

		if (!machine.gpu.isEmpty())
			qemu += QStringLiteral(" (gpu %1)").arg(machine.gpu);
		if (machine.sound)
			qemu += QStringLiteral(" (sound virtio)");
		text += QStringLiteral("  (tuning (qemu%1))\n").arg(qemu);
	}
	if (!ok) {
		*why = QStringLiteral("a field holds a control character the spec cannot carry");
		return QString();
	}
	text.chop(1);
	return text + QStringLiteral(")\n");
}

QStringList segments_in(const QString &resolved)
{
	const int   start = resolved.indexOf(QStringLiteral("(segments "));
	QStringList names;
	int         depth = 0;

	if (start < 0)
		return names;
	/* Inside the form: a string at its own depth is a bare segment, a list
	 * there one with settings, named by its head. */
	for (int i = start + 1; i < resolved.size(); i++) {
		const QChar c = resolved[i];

		if (c == QLatin1Char('"')) {
			int end = i + 1;

			while (end < resolved.size() && resolved[end] != QLatin1Char('"'))
				end += resolved[end] == QLatin1Char('\\') ? 2 : 1;
			if (depth == 0 && !names.contains(resolved.mid(i + 1, end - i - 1)))
				names << resolved.mid(i + 1, end - i - 1);
			i = end;
		} else if (c == QLatin1Char('(')) {
			if (depth == 0) {
				const int     end = resolved.indexOf(QRegularExpression(QStringLiteral("[ ()]")),
				                                     i + 1);
				const QString head = resolved.mid(i + 1, end - i - 1);

				if (!head.isEmpty() && !names.contains(head))
					names << head;
			}
			depth++;
		} else if (c == QLatin1Char(')')) {
			if (depth-- == 0)
				break;
		}
	}
	return names;
}

QString hostname_in(const QString &resolved)
{
	static const QRegularExpression hostname(QStringLiteral("\\(hostname \"([^\"\\\\]*)\"\\)"));
	const QRegularExpressionMatch   match = hostname.match(resolved);

	return match.hasMatch() ? match.captured(1) : QString();
}
