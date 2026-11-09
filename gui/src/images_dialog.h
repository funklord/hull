/*
 * images_dialog.h -- the store's images (project.md section 3), as
 * VirtualBox's media manager lists disks: each ref and its manifest, with
 * Pull, Remove and New Machine. The store has no protocol of its own; the
 * window runs the CLI's commands, `hull images`, `hull pull` and `hull tag
 * -d`, as the user would.
 */

#ifndef HULL_GUI_IMAGES_DIALOG_H
#define HULL_GUI_IMAGES_DIALOG_H

#include <QDialog>

class QPlainTextEdit;
class QProcess;
class QPushButton;
class QTreeWidget;

class images_dialog : public QDialog {
	Q_OBJECT

public:
	images_dialog(const QString &hull_program, QWidget *parent = nullptr);

	/* The refs `hull images` listed last. */
	QStringList refs() const;

signals:
	/* New Machine on the selected ref. */
	void new_machine_from(const QString &ref);

private:
	void reload();
	void run(const QStringList &arguments);
	void selected_changed();

	QString         hull;
	QTreeWidget    *list;
	QPlainTextEdit *log;
	QPushButton    *pull_button;
	QPushButton    *remove_button;
	QPushButton    *new_button;
	QProcess       *busy = nullptr;
};

#endif
