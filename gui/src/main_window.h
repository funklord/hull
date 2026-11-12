/*
 * main_window.h -- hull's manager window: the machines on the left, what
 * the selected one is on the right, and what can be done to it above, in
 * the shape VirtualBox's manager taught people to expect.
 *
 * The same QWidgets are the TUI under qtty (main.cpp): nothing here may
 * depend on pixels, icons or a mouse.
 */

#ifndef HULL_GUI_MAIN_WINDOW_H
#define HULL_GUI_MAIN_WINDOW_H

#include <QHash>
#include <QMainWindow>

#include "core_link.h"
#include "machine_list.h"

class QAction;
class QComboBox;
class QLineEdit;
class QLabel;
class QPlainTextEdit;
class QProcess;
class QTabWidget;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

class main_window : public QMainWindow {
	Q_OBJECT

public:
	explicit main_window(const QString &hull_program, QWidget *parent = nullptr);
	~main_window() override;

	/* Add `file` (and `name` of its machines) to the list, saved. */
	void add_machine(const QString &file, const QString &name);

	/* Ask the core what it holds now; `after` runs once it has answered. */
	void refresh(std::function<void()> after = nullptr);

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	/* One row: an added machine, a machine the core holds, or both. */
	struct row {
		added_machine added;       /* file empty where only the core has it */
		QString       id;          /* the run's id, by the CLI's rule */
		QString       resolved;    /* `hull plan --resolved`'s line */
		QString       explain;     /* `hull plan --explain`'s, or why not */
		machine_row   held;        /* as the core lists it */
		bool          is_held = false;
		QProcess     *run = nullptr;  /* `hull run` started here */
		QByteArray    output;      /* what that run said */
		QStringList   accepted;    /* --accept tokens agreed to here, this session */
	};

	void build_actions();
	void rebuild();
	void selected_changed();
	void update_actions();
	void show_details();
	void show_output();
	void plan_of(int index);
	int  current() const;
	void start();
	void stop();
	void kill_machine();
	void pause_machine(bool paused);
	void open_terminal();
	void open_screen();
	void commit_machine();
	void change_medium();
	void insert_ctrl_alt_del();
	void take_screenshot();
	void run_for(const QStringList &arguments, const QString &done);
	void edit_spec();
	void remove_machine();
	void ask_add();
	void ask_new(const QString &source = QString());
	void show_images();
	void say(const QString &text);
	void follow_logs();
	void ask_stats();
	QWidget *build_network();
	void     ask_ports();
	void     segment_act(const QString &verb, const QStringList &words);

	QString         hull;
	QList<row>      rows;
	QTreeWidget    *tree;
	QTabWidget     *tabs;
	QPlainTextEdit *details;
	QPlainTextEdit *output;
	QPlainTextEdit *usage;
	QWidget        *network;
	QTreeWidget    *ports;
	QLabel         *network_note;
	QComboBox      *segment_verb;
	QComboBox      *segment_target;
	QComboBox      *segment_way;
	QLineEdit      *segment_words;
	int             ports_pending = 0;
	QLabel         *core_state;
	QTimer         *poll;
	QAction        *new_action;
	QAction        *add_action;
	QAction        *remove_action;
	QAction        *start_action;
	QAction        *stop_action;
	QAction        *kill_action;
	QAction        *pause_action;
	QAction        *resume_action;
	QAction        *refresh_action;
	QAction        *terminal_action;
	QAction        *commit_action;
	QAction        *medium_action;
	QAction        *keys_action;
	QAction        *screenshot_action;
	QAction        *screen_action;
	QAction        *edit_action;
	core_request   *logs = nullptr;    /* following the selected machine's */
	QString         logs_of;
	bool            listing = false;
	qint64          last_cpu = -1;
	qint64          last_at = 0;
};

#endif
