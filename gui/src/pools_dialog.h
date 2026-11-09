/*
 * pools_dialog.h -- the core's pools and leases (project.md section 4.12):
 * each pool's capacity, what is held and what waits, each lease and when
 * it ends, with Reserve and Release, as a CI job's `hull reserve` and
 * `hull release` do them. Read through `hull pool ls --json` and `hull
 * lease ls --json`, the CLI's interface for scripts.
 */

#ifndef HULL_GUI_POOLS_DIALOG_H
#define HULL_GUI_POOLS_DIALOG_H

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;
class QTreeWidget;

class pools_dialog : public QDialog {
	Q_OBJECT

public:
	pools_dialog(const QString &hull_program, QWidget *parent = nullptr);

private:
	void reload();
	void act(const QStringList &arguments);
	void update_buttons();

	QString      hull;
	QTreeWidget *pools;
	QTreeWidget *leases;
	QSpinBox    *count;
	QLineEdit   *span;
	QPushButton *reserve_button;
	QPushButton *release_button;
	QLabel      *said;
	QTimer      *poll;
	bool         busy = false;
};

#endif
