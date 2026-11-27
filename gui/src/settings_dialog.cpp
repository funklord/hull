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
#include <QInputDialog>
#include <QLineEdit>
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
		      QStringLiteral("virtio"), QStringLiteral("vmxnet3") },
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
		/* A PC's System and Display pages, as VirtualBox's: its chipset,
		 * firmware, TPM and graphics. A board's or a Mac's machine is its
		 * own word, which a chipset list would only offer to replace. */
		const QString board = spec_value(text, { QStringLiteral("tuning"), QStringLiteral("pc"),
		                                         QStringLiteral("machine") });

		if (board.isEmpty() || board == QStringLiteral("pc") || board == QStringLiteral("q35") ||
		    board == QStringLiteral("isapc")) {
			add(QStringLiteral("C&hipset:"),
			    { QStringLiteral("tuning"), QStringLiteral("pc"), QStringLiteral("machine") },
			    { QStringLiteral("pc"), QStringLiteral("q35"), QStringLiteral("isapc") }, false,
			    false);
			add(QStringLiteral("Firm&ware:"),
			    { QStringLiteral("tuning"), QStringLiteral("pc"), QStringLiteral("firmware") },
			    { QStringLiteral("bios"), QStringLiteral("uefi"), QStringLiteral("uefi-secure") },
			    false, false);
			add(QStringLiteral("&TPM:"),
			    { QStringLiteral("tuning"), QStringLiteral("pc"), QStringLiteral("tpm") },
			    { QStringLiteral("true"), QStringLiteral("false") }, false, false);
			add(QStringLiteral("&Graphics:"),
			    { QStringLiteral("tuning"), QStringLiteral("pc"), QStringLiteral("vga") },
			    { QStringLiteral("std"), QStringLiteral("cirrus"), QStringLiteral("vmware"),
			      QStringLiteral("virtio"), QStringLiteral("none") },
			    false, false);
		}
		/* VirtualBox's Boot Order: which of its media the PC starts from,
		 * the installer's disc or the disk it installed on. A source is
		 * required, so it has no Default to fall back on. */
		if (spec_value(text, { QStringLiteral("source") }).startsWith(QStringLiteral("\"media:"))) {
			add(QStringLiteral("&Boot from:"), { QStringLiteral("source") },
			    { QStringLiteral("media:disk"), QStringLiteral("media:cdrom"),
			      QStringLiteral("media:floppy") },
			    false, true);
			fields.last().box->removeItem(0);
		}

		/* Its disks and discs, as VirtualBox's Storage page: each record
		 * shown by its type and source, a new one added by its image. */
		auto *add_disk = new QPushButton(QStringLiteral("Add &Disk..."));
		auto *add_disc = new QPushButton(QStringLiteral("Add D&isc..."));

		media = record_list(QStringLiteral("Disks and discs:"), QStringLiteral("media"),
		                    [](const QString &record) {
			                    const QString w = QStringLiteral("(machine ") + record + QLatin1Char(')');
			                    const QString type = spec_value(w, { QStringLiteral("medium"),
			                                                         QStringLiteral("type") });
			                    const QString source = spec_value(w, { QStringLiteral("medium"),
			                                                           QStringLiteral("source") });

			                    return type.isEmpty() || source.isEmpty() ? QString() :
			                           QStringLiteral("%1  %2").arg(type, source);
		                    },
		                    { add_disk, add_disc });
		connect(add_disk, &QPushButton::clicked, this, [this]() { add_medium(QStringLiteral("disk")); });
		connect(add_disc, &QPushButton::clicked, this, [this]() { add_medium(QStringLiteral("cdrom")); });
	}
	/* Every class's: VirtualBox's Port Forwarding and Shared Folders. */
	{
		auto *add_port_button = new QPushButton(QStringLiteral("Add P&ort..."));
		auto *add_folder_button = new QPushButton(QStringLiteral("Add &Folder..."));

		ports = record_list(QStringLiteral("Forwarded ports:"), QStringLiteral("ports"),
		                    [](const QString &record) {
			                    const QString w = QStringLiteral("(machine ") + record + QLatin1Char(')');
			                    const QString host = spec_value(w, { QStringLiteral("port"),
			                                                         QStringLiteral("host") });
			                    const QString guest = spec_value(w, { QStringLiteral("port"),
			                                                          QStringLiteral("guest") });

			                    return host.isEmpty() || guest.isEmpty() ? QString() :
			                           QStringLiteral("%1 on the machine's address to %2 in it")
			                               .arg(host, guest);
		                    },
		                    { add_port_button });
		connect(add_port_button, &QPushButton::clicked, this, &settings_dialog::add_port);
		folders = record_list(QStringLiteral("Shared folders:"), QStringLiteral("volumes"),
		                      [](const QString &record) {
			                      const QString w = QStringLiteral("(machine ") + record +
			                                        QLatin1Char(')');
			                      const QString host = spec_value(w, { QStringLiteral("volume"),
			                                                           QStringLiteral("host") });
			                      const QString guest = spec_value(w, { QStringLiteral("volume"),
			                                                            QStringLiteral("guest") });
			                      const QString mode = spec_value(w, { QStringLiteral("volume"),
			                                                           QStringLiteral("mode") });

			                      return host.isEmpty() || guest.isEmpty() ? QString() :
			                             QStringLiteral("%1 at %2%3").arg(host, guest,
			                                 mode == QStringLiteral("ro") ? QStringLiteral(", read-only")
			                                                             : QString());
		                      },
		                      { add_folder_button });
		connect(add_folder_button, &QPushButton::clicked, this, &settings_dialog::add_folder);
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

QListWidget *settings_dialog::record_list(const QString &label, const QString &key,
                                          const std::function<QString(const QString &)> &describe,
                                          const QList<QPushButton *> &adds)
{
	auto *form = static_cast<QFormLayout *>(layout()->itemAt(0)->layout());
	auto *list = new QListWidget;
	auto *row = new QHBoxLayout;
	auto *remove = new QPushButton(QStringLiteral("&Remove"));
	const QStringList was = spec_forms(text, { key });

	for (const QString &record : was) {
		const QString shown = describe(record);

		/* A record of another shape kept as it was written, shown so. */
		add_record(list, record, shown.isEmpty() ? record.simplified() : shown);
	}
	list->setAccessibleName(QString(label).remove(QLatin1Char(':')));
	list->setCurrentRow(0);
	for (QPushButton *add : adds)
		row->addWidget(add);
	row->addWidget(remove);
	connect(remove, &QPushButton::clicked, this, [list]() { delete list->currentItem(); });
	form->addRow(label, list);
	form->addRow(QString(), row);
	lists.append({ key, list, was });
	return list;
}

void settings_dialog::add_record(QListWidget *list, const QString &record, const QString &shown)
{
	auto *item = new QListWidgetItem(shown);

	item->setData(Qt::UserRole, record);
	list->addItem(item);
}

/* A path chosen in a dialog, named from the machine's folder where it is
 * inside it, as hull.conf's own are; "" with the error said where it
 * cannot be written in a string here. */
QString settings_dialog::folder_path(const QString &chosen)
{
	const QString folder = QFileInfo(file).absolutePath();
	QString       path = QDir(folder).relativeFilePath(chosen);

	if (path.startsWith(QStringLiteral("../")) || QFileInfo(path).isAbsolute())
		path = QFileInfo(chosen).absoluteFilePath();
	else
		path = QStringLiteral("./") + path;
	if (path.contains(QLatin1Char('"')) || path.contains(QLatin1Char('\\'))) {
		error->setText(QStringLiteral("%1: a path with a quote or a backslash is not written here; "
		                              "Edit hull.conf takes it").arg(path));
		return QString();
	}
	return path;
}

/* A disk or disc image, by a file dialog. */
void settings_dialog::add_medium(const QString &type)
{
	const QString image = QFileDialog::getOpenFileName(
	    this, type == QStringLiteral("disk") ? QStringLiteral("Add Disk") : QStringLiteral("Add Disc"),
	    QFileInfo(file).absolutePath(), type == QStringLiteral("disk") ?
	            QStringLiteral("Disk images (*.qcow2 *.img *.raw *.vdi *.vmdk *.vhd *.vhdx);;All files (*)") :
	            QStringLiteral("Disc images (*.iso);;All files (*)"));
	const QString path = image.isEmpty() ? QString() : folder_path(image);

	if (path.isEmpty())
		return;
	add_record(media, QStringLiteral("(medium (type %1) (source \"image:%2\"))").arg(type, path),
	           QStringLiteral("%1  \"image:%2\"").arg(type, path));
}

/* A port of the machine's address forwarded to one in it, as
 * VirtualBox's Port Forwarding rule. */
void settings_dialog::add_port()
{
	bool      given = false;
	const int host_port = QInputDialog::getInt(this, QStringLiteral("Add Port"),
	                                           QStringLiteral("Port on the machine's address:"),
	                                           8080, 1, 65535, 1, &given);

	if (!given)
		return;
	const int guest_port = QInputDialog::getInt(this, QStringLiteral("Add Port"),
	                                            QStringLiteral("Port in the machine:"), host_port, 1,
	                                            65535, 1, &given);

	if (!given)
		return;
	add_record(ports, QStringLiteral("(port (host %1) (guest %2))").arg(host_port).arg(guest_port),
	           QStringLiteral("%1 on the machine's address to %2 in it").arg(host_port).arg(guest_port));
}

/* A folder of the host's at a path in the machine, as VirtualBox's Shared
 * Folders: read and write, or read-only. */
void settings_dialog::add_folder()
{
	const QString chosen = QFileDialog::getExistingDirectory(this, QStringLiteral("Add Folder"),
	                                                         QFileInfo(file).absolutePath());
	const QString host_path = chosen.isEmpty() ? QString() : folder_path(chosen);
	bool          given = false;

	if (host_path.isEmpty())
		return;
	const QString guest = QInputDialog::getText(this, QStringLiteral("Add Folder"),
	                                            QStringLiteral("Its path in the machine:"),
	                                            QLineEdit::Normal,
	                                            QLatin1Char('/') + QFileInfo(chosen).fileName(), &given)
	                          .trimmed();

	if (!given || !guest.startsWith(QLatin1Char('/')) || guest.contains(QLatin1Char('"')) ||
	    guest.contains(QLatin1Char('\\'))) {
		if (given)
			error->setText(QStringLiteral("%1: a path in the machine starts with / and holds no "
			                              "quote or backslash").arg(guest));
		return;
	}
	const QString mode = QInputDialog::getItem(this, QStringLiteral("Add Folder"),
	                                           QStringLiteral("The machine may:"),
	                                           { QStringLiteral("read and write"),
	                                             QStringLiteral("only read") },
	                                           0, false, &given);

	if (!given)
		return;
	const bool ro = mode == QStringLiteral("only read");

	add_record(folders,
	           QStringLiteral("(volume (host \"%1\") (guest \"%2\")%3)")
	               .arg(host_path, guest, ro ? QStringLiteral(" (mode ro)") : QString()),
	           QStringLiteral("\"%1\" at \"%2\"%3")
	               .arg(host_path, guest, ro ? QStringLiteral(", read-only") : QString()));
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
	/* Each list written only where it changed, whole, a record a line, so
	 * its other records keep their text. */
	for (const records &r : lists) {
		QStringList now;

		if (!problem.isEmpty() || changed.isNull())
			break;
		for (int i = 0; i < r.list->count(); i++)
			now << r.list->item(i)->data(Qt::UserRole).toString();
		if (now != r.was)
			changed = spec_with(changed, { r.key }, now.join(QStringLiteral("\n    ")), &problem);
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
