/*
 * settings_dialog.h -- Settings, as VirtualBox's: a machine's hardware in
 * a form -- memory, processors, network, screen, and a PC's card, sound
 * and pointer -- written back into its hull.conf in place. Each field
 * offers Default, which follows the spec's default and is written as the
 * key's absence; only what was changed is written, so a choice the file
 * did not make stays unmade (working-practice.md, a config file records
 * choices). Comments and layout are kept (spec_with, machine_list.h).
 */

#ifndef HULL_GUI_SETTINGS_DIALOG_H
#define HULL_GUI_SETTINGS_DIALOG_H

#include <QDialog>
#include <QList>
#include <QStringList>

#include <functional>

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;

class settings_dialog : public QDialog {
	Q_OBJECT

public:
	/* `file`'s machine; `pc` whether it is the pc class, which adds its
	 * card, sound and pointer. */
	settings_dialog(const QString &file, bool pc, QWidget *parent = nullptr);

	/* Whether the file was opened as one machine; else `problem()` says why. */
	bool usable() const { return ok; }
	QString problem() const { return why; }

	void accept() override;

private:
	struct field {
		QStringList path;
		QComboBox  *box;
		QString     was;      /* the value as the file wrote it, "" for none */
		bool        quoted;   /* a string in the file, "2G" */
	};

	void add(const QString &label, const QStringList &path, const QStringList &choices,
	         bool editable, bool quoted);
	/* A form of records, `key`'s -- (media ...), (ports ...), (volumes
	 * ...) -- listed one a line as `describe` says, with `adds` and a
	 * Remove beside it; written back whole where the list changed. */
	QListWidget *record_list(const QString &label, const QString &key,
	                         const std::function<QString(const QString &)> &describe,
	                         const QList<QPushButton *> &adds);
	void         add_record(QListWidget *list, const QString &record, const QString &shown);
	QString      folder_path(const QString &chosen);
	void add_medium(const QString &type);
	void add_port();
	void add_folder();

	QString      file;
	QString      text;
	QList<field> fields;
	struct records {
		QString      key;
		QListWidget *list;
		QStringList  was;
	};
	QList<records> lists;
	QListWidget   *media = nullptr;    /* a pc's disks and discs */
	QListWidget   *ports = nullptr;    /* its forwarded ports */
	QListWidget   *folders = nullptr;  /* its shared folders, (volumes ...) */
	QLabel      *error;
	bool         ok = false;
	QString      why;
};

#endif
