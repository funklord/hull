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

QString hostname_in(const QString &resolved)
{
	static const QRegularExpression hostname(QStringLiteral("\\(hostname \"([^\"\\\\]*)\"\\)"));
	const QRegularExpressionMatch   match = hostname.match(resolved);

	return match.hasMatch() ? match.captured(1) : QString();
}
