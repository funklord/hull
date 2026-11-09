/*
 * machine_list.cpp -- see machine_list.h.
 */

#include "machine_list.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>

static QSettings &settings()
{
	static QSettings stored(QStringLiteral("hull"), QStringLiteral("hull-gui"));

	return stored;
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

	if (machine.source.isEmpty()) {
		*why = QStringLiteral("a machine needs a source");
		return QString();
	}
	if (!machine.backend.isEmpty() && !word.match(machine.backend).hasMatch()) {
		*why = QStringLiteral("a backend is a word: ns, vm, emu, pc, dos, or an engine's name");
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
	text += QStringLiteral("  (source %1)\n").arg(quoted(machine.source, &ok));
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
