/*
 * settings_dialog.cpp -- see settings_dialog.h.
 */

#include "settings_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QFormLayout>
#include <QLabel>
#include <QRegularExpression>
#include <QSaveFile>
#include <QVBoxLayout>

#include "machine_list.h"

static const QString default_choice = QStringLiteral("Default");

settings_dialog::settings_dialog(const QString &file_path, bool pc, QWidget *parent)
    : QDialog(parent), file(file_path)
{
	QFile in(file);

	setWindowTitle(QStringLiteral("Settings"));
	if (!in.open(QIODevice::ReadOnly)) {
		why = QStringLiteral("%1 cannot be read").arg(file);
		return;
	}
	text = QString::fromUtf8(in.readAll());
	/* One machine's file, or nothing to edit: a probe of the editor. */
	if (spec_with(text, { QStringLiteral("memory") }, QString(), &why).isNull())
		return;
	ok = true;

	auto *layout = new QVBoxLayout(this);
	auto *form = new QFormLayout;

	layout->addLayout(form);
	setLayout(layout);
	/* The form is filled by add(), which needs it as the layout's first. */
	add(QStringLiteral("&Memory:"), { QStringLiteral("memory") },
	    { QStringLiteral("256M"), QStringLiteral("512M"), QStringLiteral("1G"), QStringLiteral("2G"),
	      QStringLiteral("4G"), QStringLiteral("8G"), QStringLiteral("16G") },
	    true, true);
	add(QStringLiteral("&Processors:"), { QStringLiteral("cpus") },
	    { QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("4"), QStringLiteral("6"),
	      QStringLiteral("8") },
	    true, false);
	add(QStringLiteral("&Network:"), { QStringLiteral("network") },
	    { QStringLiteral("nat"), QStringLiteral("none") }, false, false);
	add(QStringLiteral("&Screen:"), { QStringLiteral("display") },
	    { QStringLiteral("window"), QStringLiteral("vnc"), QStringLiteral("local") }, false, false);
	if (pc) {
		add(QStringLiteral("Network &card:"),
		    { QStringLiteral("tuning"), QStringLiteral("pc"), QStringLiteral("nic") },
		    { QStringLiteral("e1000"), QStringLiteral("e1000e"), QStringLiteral("rtl8139"),
		      QStringLiteral("pcnet"), QStringLiteral("ne2k_pci"), QStringLiteral("ne2k_isa"),
		      QStringLiteral("virtio") },
		    false, false);
		add(QStringLiteral("S&ound:"),
		    { QStringLiteral("tuning"), QStringLiteral("pc"), QStringLiteral("sound") },
		    { QStringLiteral("ac97"), QStringLiteral("hda"), QStringLiteral("sb16"),
		      QStringLiteral("es1370"), QStringLiteral("adlib"), QStringLiteral("gus"),
		      QStringLiteral("none") },
		    false, false);
		add(QStringLiteral("&Pointer:"),
		    { QStringLiteral("tuning"), QStringLiteral("pc"), QStringLiteral("pointer") },
		    { QStringLiteral("tablet"), QStringLiteral("ps2") }, false, false);

		/* Its disks and discs, as VirtualBox's Storage page: each record
		 * shown by its type and source, a new one added by its image, one
		 * removed; a record of another shape kept as it was written. */
		auto *row = new QHBoxLayout;
		auto *add_disk = new QPushButton(QStringLiteral("Add &Disk..."));
		auto *add_disc = new QPushButton(QStringLiteral("Add D&isc..."));
		auto *remove = new QPushButton(QStringLiteral("&Remove"));

		media = new QListWidget;
		media_was = spec_forms(text, { QStringLiteral("media") });
		for (const QString &record : media_was) {
			const QString wrapped = QStringLiteral("(machine ") + record + QLatin1Char(')');
			const QString type = spec_value(wrapped, { QStringLiteral("medium"),
			                                           QStringLiteral("type") });
			const QString source = spec_value(wrapped, { QStringLiteral("medium"),
			                                             QStringLiteral("source") });
			auto *item = new QListWidgetItem(type.isEmpty() || source.isEmpty() ?
			                                 record.simplified() :
			                                 QStringLiteral("%1  %2").arg(type, source));

			item->setData(Qt::UserRole, record);
			media->addItem(item);
		}
		media->setAccessibleName(QStringLiteral("Disks and discs"));
		media->setCurrentRow(0);
		connect(add_disk, &QPushButton::clicked, this, [this]() { add_medium(QStringLiteral("disk")); });
		connect(add_disc, &QPushButton::clicked, this, [this]() { add_medium(QStringLiteral("cdrom")); });
		connect(remove, &QPushButton::clicked, this, [this]() { delete media->currentItem(); });
		row->addWidget(add_disk);
		row->addWidget(add_disc);
		row->addWidget(remove);
		form->addRow(QStringLiteral("Disks and discs:"), media);
		form->addRow(QString(), row);
	}
	error = new QLabel;
	error->setWordWrap(true);
	layout->addWidget(error);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

	connect(buttons, &QDialogButtonBox::accepted, this, &settings_dialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &settings_dialog::reject);
	layout->addWidget(buttons);
}

void settings_dialog::add(const QString &label, const QStringList &path, const QStringList &choices,
                          bool editable, bool quoted)
{
	auto   *form = static_cast<QFormLayout *>(layout()->itemAt(0)->layout());
	auto   *box = new QComboBox;
	field   f{ path, box, spec_value(text, path), quoted };
	QString shown = f.was;

	if (quoted && shown.size() >= 2 && shown.startsWith(QLatin1Char('"')) &&
	    shown.endsWith(QLatin1Char('"')))
		shown = shown.mid(1, shown.size() - 2);
	box->addItem(default_choice);
	box->addItems(choices);
	box->setEditable(editable);
	/* What the file says, among the choices or beside them. */
	if (!shown.isEmpty()) {
		if (box->findText(shown) < 0)
			box->addItem(shown);
		box->setCurrentIndex(box->findText(shown));
	}
	form->addRow(label, box);
	fields.append(f);
}

/* A disk or disc image chosen, named from the machine's folder where it
 * is inside it, as hull.conf's own media are. */
void settings_dialog::add_medium(const QString &type)
{
	const QString folder = QFileInfo(file).absolutePath();
	const QString image = QFileDialog::getOpenFileName(
	    this, type == QStringLiteral("disk") ? QStringLiteral("Add Disk") : QStringLiteral("Add Disc"),
	    folder, type == QStringLiteral("disk") ?
	            QStringLiteral("Disk images (*.qcow2 *.img *.raw *.vdi *.vmdk *.vhd *.vhdx);;All files (*)") :
	            QStringLiteral("Disc images (*.iso);;All files (*)"));

	if (image.isEmpty())
		return;
	QString path = QDir(folder).relativeFilePath(image);

	if (path.startsWith(QStringLiteral("../")) || QFileInfo(path).isAbsolute())
		path = QFileInfo(image).absoluteFilePath();
	else
		path = QStringLiteral("./") + path;
	if (path.contains(QLatin1Char('"')) || path.contains(QLatin1Char('\\'))) {
		error->setText(QStringLiteral("%1: a path with a quote or a backslash is not written here; "
		                              "Edit hull.conf takes it").arg(path));
		return;
	}
	const QString record = QStringLiteral("(medium (type %1) (source \"image:%2\"))").arg(type, path);
	auto         *item = new QListWidgetItem(QStringLiteral("%1  \"image:%2\"").arg(type, path));

	item->setData(Qt::UserRole, record);
	media->addItem(item);
}

/* The (media ...) form's inside as the list now has it, a record a line;
 * "" for none. */
QString settings_dialog::media_value() const
{
	QStringList records;

	for (int i = 0; i < media->count(); i++)
		records << media->item(i)->data(Qt::UserRole).toString();
	return records.join(QStringLiteral("\n    "));
}

void settings_dialog::accept()
{
	QString changed = text;
	QString problem;

	for (const field &f : fields) {
		QString chosen = f.box->currentText().trimmed();
		QString value;

		if (chosen.isEmpty() || chosen == default_choice)
			value = QString();
		else if (f.quoted)
			value = QLatin1Char('"') + QString(chosen).replace(QLatin1Char('\\'), QStringLiteral("\\\\"))
			                               .replace(QLatin1Char('"'), QStringLiteral("\\\"")) +
			        QLatin1Char('"');
		else if (chosen.contains(QRegularExpression(QStringLiteral("[\\s()\";]"))))
			problem = QStringLiteral("%1 is not one word").arg(chosen);
		else
			value = chosen;
		/* Untouched, the file keeps what it said, spelling and all. */
		if (value == f.was || (value.isEmpty() && f.was.isEmpty()))
			continue;
		changed = spec_with(changed, f.path, value, &problem);
		if (changed.isNull())
			break;
	}
	/* The media written only where the list changed, whole, so its other
	 * records keep their text. */
	if (media != nullptr && problem.isEmpty() && !changed.isNull()) {
		QStringList now;

		for (int i = 0; i < media->count(); i++)
			now << media->item(i)->data(Qt::UserRole).toString();
		if (now != media_was)
			changed = spec_with(changed, { QStringLiteral("media") },
			                    now.isEmpty() ? QString() : media_value(), &problem);
	}
	if (!problem.isEmpty() || changed.isNull()) {
		error->setText(problem);
		return;
	}
	if (changed != text) {
		QSaveFile out(file);

		if (!out.open(QIODevice::WriteOnly) || out.write(changed.toUtf8()) < 0 || !out.commit()) {
			error->setText(QStringLiteral("%1 could not be written").arg(file));
			return;
		}
	}
	QDialog::accept();
}
