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
	folder = new QLineEdit(QDir::currentPath());
	folder->setToolTip(QStringLiteral("Where hull.conf is written; a dir: source is relative "
	                                  "to it"));
	source = new QComboBox;
	source->setEditable(true);
	source->addItem(QStringLiteral("dir:./rootfs"));
	for (const QString &image : images)
		source->addItem(image);
	if (!chosen.isEmpty())
		source->setCurrentText(chosen);
	backend = new QComboBox;
	backend->addItem(QStringLiteral("default (ns)"), QString());
	for (const char *word : { "ns", "vm", "emu", "pc", "dos", "chroot", "docker", "qemu" })
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
	gpu->setToolTip(QStringLiteral("A vm's 3D, the guest needing Mesa's driver; Venus needs "
	                               "/dev/udmabuf open to the user"));
	problem = new QLabel;
	problem->setWordWrap(true);

	QFormLayout *form = new QFormLayout;
	form->addRow(QStringLiteral("&Name:"), name);
	form->addRow(QStringLiteral("&Folder:"), folder);
	form->addRow(QStringLiteral("S&ource:"), source);
	form->addRow(QStringLiteral("&Backend:"), backend);
	form->addRow(QStringLiteral("&Command:"), command);
	form->addRow(QStringLiteral("&Memory:"), memory);
	form->addRow(QStringLiteral("C&PUs:"), cpus);
	form->addRow(QStringLiteral("Sc&reen:"), screen);
	form->addRow(QStringLiteral("&3D:"), gpu);
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
	QFile out(path);

	if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
	    out.write(text.toUtf8()) != text.toUtf8().size() || !out.flush()) {
		problem->setText(QStringLiteral("%1: %2").arg(path, out.errorString()));
		out.remove();
		return;
	}
	out.close();
	file = path;
	QDialog::accept();
}
