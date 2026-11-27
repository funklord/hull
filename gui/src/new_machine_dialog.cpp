/*
 * new_machine_dialog.cpp -- see new_machine_dialog.h.
 */

#include "new_machine_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QSpinBox>
#include <QVBoxLayout>

#include "machine_list.h"

new_machine_dialog::new_machine_dialog(const QStringList &images, const QString &chosen,
                                       QWidget *parent)
    : QDialog(parent)
{
	setWindowTitle(QStringLiteral("New Machine"));

	name = new QLineEdit;
	name->setPlaceholderText(QStringLiteral("web"));
	/* A machine's folder of its own under ~/hull, named for it, as
	 * VirtualBox keeps ~/VirtualBox VMs/NAME: it follows the name until
	 * the folder is typed in. */
	folder = new QLineEdit(machines_home());
	folder->setToolTip(QStringLiteral("Where hull.conf is written, ~/hull/NAME unless given; a "
	                                  "dir: source is relative to it"));
	connect(name, &QLineEdit::textEdited, this, [this](const QString &text) {
		if (!folder->isModified())
			folder->setText(machines_home() + (text.trimmed().isEmpty() ? QString()
			                                   : QLatin1Char('/') + text.trimmed()));
	});
	source = new QComboBox;
	source->setEditable(true);
	source->addItem(QStringLiteral("dir:./rootfs"));
	for (const QString &image : images)
		source->addItem(image);
	if (!chosen.isEmpty())
		source->setCurrentText(chosen);
	backend = new QComboBox;
	backend->addItem(QStringLiteral("default (ns)"), QString());
	for (const char *word : { "ns", "vm", "emu", "pc", "dos", "chroot", "docker", "qemu",
	                          "virtualbox" })
		backend->addItem(QString::fromLatin1(word), QString::fromLatin1(word));
	command = new QLineEdit;
	command->setPlaceholderText(QStringLiteral("/bin/sh -c \"...\"  (no shell: words, quoted)"));
	memory = new QLineEdit;
	memory->setPlaceholderText(QStringLiteral("the engine's default, or 512M, 2G"));
	cpus = new QSpinBox;
	cpus->setRange(0, 256);
	cpus->setSpecialValueText(QStringLiteral("the engine's default"));
	overlay = new QCheckBox(QStringLiteral("&Keep the source as it is (overlay)"));
	ssh = new QCheckBox(QStringLiteral("&SSH into it (hull ssh)"));
	/* A screen, as VirtualBox's machines have: hull-gui's own window
	 * first, which is near native and the only one with 3D shown here. */
	screen = new QComboBox;
	screen->addItem(QStringLiteral("none"), QString());
	screen->addItem(QStringLiteral("in hull-gui (local)"), QStringLiteral("local"));
	screen->addItem(QStringLiteral("by VNC, at its address"), QStringLiteral("vnc"));
	screen->addItem(QStringLiteral("qemu's own window"), QStringLiteral("window"));
	gpu = new QComboBox;
	gpu->addItem(QStringLiteral("none"), QString());
	gpu->addItem(QStringLiteral("OpenGL on this host's GPU (virgl)"), QStringLiteral("virgl"));
	gpu->addItem(QStringLiteral("Vulkan on this host's GPU (Venus)"), QStringLiteral("venus"));
	sound = new QCheckBox(QStringLiteral("So&und, heard where the screen is shown here"));
	gpu->setToolTip(QStringLiteral("The screen's 3D on a vm or a pc, the guest needing Mesa's driver; Venus needs "
	                               "/dev/udmabuf open to the user"));
	/* A pc as VirtualBox's New makes one: the disc to install from, and
	 * a new disk to install on, both in place of a source. */
	installer = new QLineEdit;
	installer->setPlaceholderText(QStringLiteral("an installer's .iso, to boot from"));
	disk = new QSpinBox;
	disk->setRange(0, 4096);
	disk->setSuffix(QStringLiteral(" GiB"));
	disk->setSpecialValueText(QStringLiteral("none"));
	disk->setToolTip(QStringLiteral("A new disk in the folder, a qcow2 or for VirtualBox a "
	                                "VDI, taking room only as it is written"));
	problem = new QLabel;
	problem->setWordWrap(true);

	QFormLayout *form = new QFormLayout;
	form->addRow(QStringLiteral("&Name:"), name);
	form->addRow(QStringLiteral("&Folder:"), folder);
	form->addRow(QStringLiteral("S&ource:"), source);
	form->addRow(QStringLiteral("&Backend:"), backend);
	form->addRow(QStringLiteral("&Installer disc:"), installer);
	form->addRow(QStringLiteral("New &disk:"), disk);
	/* A pc's two rows, shown for a pc alone; its source then unused. */
	auto pc_rows = [this, form]() {
		const bool pc = is_pc_backend(backend->currentData().toString());

		form->setRowVisible(installer, pc);
		form->setRowVisible(disk, pc);
		if (pc && disk->value() == 0 && installer->text().isEmpty())
			disk->setValue(32);
		/* An installer is watched: a PC is shown, here, unless told. */
		if (pc && screen->currentData().toString().isEmpty())
			screen->setCurrentIndex(screen->findData(QStringLiteral("local")));
	};
	connect(backend, &QComboBox::currentIndexChanged, this, pc_rows);
	pc_rows();
	form->addRow(QStringLiteral("&Command:"), command);
	form->addRow(QStringLiteral("&Memory:"), memory);
	form->addRow(QStringLiteral("C&PUs:"), cpus);
	form->addRow(QStringLiteral("Sc&reen:"), screen);
	form->addRow(QStringLiteral("&3D:"), gpu);
	form->addRow(QString(), sound);
	form->addRow(QString(), overlay);
	form->addRow(QString(), ssh);

	QDialogButtonBox *buttons =
	    new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, this, &new_machine_dialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(problem);
	layout->addWidget(buttons);
}

void new_machine_dialog::accept()
{
	new_machine machine;
	QString     why;
	const QString dir = QDir(folder->text().trimmed()).absolutePath();
	const QString path = dir + QStringLiteral("/hull.conf");

	machine.hostname = name->text().trimmed();
	machine.source = source->currentText().trimmed();
	machine.backend = backend->currentData().toString();
	machine.command = QProcess::splitCommand(command->text());
	machine.memory = memory->text().trimmed();
	machine.cpus = cpus->value();
	machine.overlay = overlay->isChecked();
	machine.ssh = ssh->isChecked();
	machine.display = screen->currentData().toString();
	machine.gpu = gpu->currentData().toString();
	machine.sound = sound->isChecked();
	if (is_pc_backend(machine.backend)) {
		machine.installer = installer->text().trimmed();
		machine.disk_gigabytes = disk->value();
		if (!machine.installer.isEmpty())
			machine.installer = QFileInfo(machine.installer).absoluteFilePath();
		if (!machine.installer.isEmpty() && !QFileInfo(machine.installer).isFile()) {
			problem->setText(QStringLiteral("%1: no such file").arg(machine.installer));
			return;
		}
	}

	const QString text = spec_text(machine, &why);

	if (text.isEmpty()) {
		problem->setText(why);
		return;
	}
	/* Never over a file of the user's: a second machine is a second
	 * directory, or (machines ...) written by hand. */
	if (QFileInfo::exists(path)) {
		problem->setText(QStringLiteral("%1 is there already").arg(path));
		return;
	}
	if (!QDir().mkpath(dir)) {
		problem->setText(QStringLiteral("%1 could not be made").arg(dir));
		return;
	}
	/* The new disk first, so a file is written only naming one there;
	 * never over a disk that is. */
	const QString disk_path = dir + QLatin1Char('/') + new_disk_name(machine.backend);

	if (machine.disk_gigabytes > 0) {
		QProcess create;

		if (QFileInfo::exists(disk_path)) {
			problem->setText(QStringLiteral("%1 is there already").arg(disk_path));
			return;
		}
		create.setProcessChannelMode(QProcess::MergedChannels);
		create.setStandardInputFile(QProcess::nullDevice());
		create.start(QStringLiteral("qemu-img"),
		             { QStringLiteral("create"), QStringLiteral("-q"), QStringLiteral("-f"),
		               new_disk_name(machine.backend).section(QLatin1Char('.'), 1), disk_path,
		               QStringLiteral("%1G").arg(machine.disk_gigabytes) });
		if (!create.waitForFinished(60000) || create.exitStatus() != QProcess::NormalExit ||
		    create.exitCode() != 0) {
			problem->setText(QStringLiteral("qemu-img could not make %1: %2")
			                     .arg(disk_path, create.errorString() + QLatin1Char(' ') +
			                                         QString::fromUtf8(create.readAll()).trimmed()));
			QFile::remove(disk_path);
			return;
		}
	}
	QFile out(path);

	if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
	    out.write(text.toUtf8()) != text.toUtf8().size() || !out.flush()) {
		problem->setText(QStringLiteral("%1: %2").arg(path, out.errorString()));
		out.remove();
		if (machine.disk_gigabytes > 0)
			QFile::remove(disk_path);
		return;
	}
	out.close();
	file = path;
	QDialog::accept();
}
