/*
 * new_machine_dialog.h -- New: a machine's few common keys asked for, and
 * its hull.conf written into a directory, as VirtualBox's New writes a
 * machine's settings. What it does not ask, the spec's defaults decide,
 * and the file is plain text for anything more.
 */

#ifndef HULL_GUI_NEW_MACHINE_DIALOG_H
#define HULL_GUI_NEW_MACHINE_DIALOG_H

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

class new_machine_dialog : public QDialog {
	Q_OBJECT

public:
	/* `images`, the store's refs, offered as sources beside a directory;
	 * `source`, where given, chosen. */
	new_machine_dialog(const QStringList &images, const QString &source = QString(),
	                   QWidget *parent = nullptr);

	/* The hull.conf written, once accepted. */
	QString written() const { return file; }

	void accept() override;

private:
	QLineEdit *name;
	QLineEdit *folder;
	QComboBox *source;
	QComboBox *backend;
	QLineEdit *command;
	QLineEdit *memory;
	QSpinBox  *cpus;
	QCheckBox *overlay;
	QCheckBox *ssh;
	QComboBox *screen;
	QComboBox *gpu;
	QCheckBox *sound;
	QLabel    *problem;
	QString    file;
};

#endif
