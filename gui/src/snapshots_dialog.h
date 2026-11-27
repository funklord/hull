/*
 * snapshots_dialog.h -- a pc machine's snapshots, as VirtualBox's
 * Snapshots pane keeps a machine's: each name and when it was taken, with
 * Take, Restore and Delete. The disks are the machine's files, so the
 * window runs the CLI's `hull snapshot` on its hull.conf, as the user
 * would, one command at a time.
 */

#ifndef HULL_GUI_SNAPSHOTS_DIALOG_H
#define HULL_GUI_SNAPSHOTS_DIALOG_H

#include <QDialog>

class QPlainTextEdit;
class QProcess;
class QPushButton;
class QTreeWidget;

class snapshots_dialog : public QDialog {
	Q_OBJECT

public:
	snapshots_dialog(const QString &hull_program, const QString &file, const QString &title,
	                 QWidget *parent = nullptr);

private:
	void reload();
	void run(const QStringList &arguments);
	void selected_changed();

	QString         hull;
	QString         file;
	QTreeWidget    *list;
	QPlainTextEdit *log;
	QPushButton    *take_button;
	QPushButton    *restore_button;
	QPushButton    *delete_button;
	QProcess       *busy = nullptr;
};

#endif
