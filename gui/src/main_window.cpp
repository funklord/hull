/*
 * main_window.cpp -- see main_window.h.
 *
 * What it asks the core it asks in the protocol, through core_link: what
 * runs, stop, kill, pause, stats and logs. A machine is started by `hull
 * run`, the CLI, as the window's child, because planning is the
 * frontend's (section 4.7): the spec read, the capability matrix applied,
 * the secrets resolved as the user. A second planner in the GUI would be a
 * second reading of hull.conf, which is exactly what one codebase is for
 * not having. So the run is that child's, and the machine lives as long
 * as it does, unless its spec says `(on_disconnect keep)`.
 */

#include "main_window.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef HULL_QTTY
#include <qtty/qtty.h>
#endif

/* Under qtty there is no window system: a file dialog of icons and a
 * mouse is the wrong question to put on a terminal, so a path is asked. */
static bool on_terminal()
{
	return QGuiApplication::platformName() == QStringLiteral("offscreen");
}

main_window::main_window(const QString &hull_program, QWidget *parent)
    : QMainWindow(parent), hull(hull_program)
{
	setWindowTitle(QStringLiteral("hull"));

	tree = new QTreeWidget;
	tree->setColumnCount(3);
	tree->setHeaderLabels({ QStringLiteral("Machine"), QStringLiteral("State"),
	                        QStringLiteral("Backend") });
	tree->setRootIsDecorated(false);
	tree->setUniformRowHeights(true);
	tree->header()->setStretchLastSection(true);
	connect(tree, &QTreeWidget::currentItemChanged, this, &main_window::selected_changed);

	details = new QPlainTextEdit;
	details->setReadOnly(true);
	output = new QPlainTextEdit;
	output->setReadOnly(true);
	output->setMaximumBlockCount(5000);
	usage = new QPlainTextEdit;
	usage->setReadOnly(true);
	tabs = new QTabWidget;
	tabs->addTab(details, QStringLiteral("&Details"));
	tabs->addTab(output, QStringLiteral("&Output"));
	tabs->addTab(usage, QStringLiteral("&Usage"));

	QSplitter *split = new QSplitter;
	split->addWidget(tree);
	split->addWidget(tabs);
	split->setStretchFactor(0, 1);
	split->setStretchFactor(1, 2);
	setCentralWidget(split);

	core_state = new QLabel;
	statusBar()->addPermanentWidget(core_state);

	build_actions();

	for (const added_machine &machine : load_added()) {
		row r;

		r.added = machine;
		r.id = machine_id_of(machine, QString());
		rows.append(r);
	}
	for (int i = 0; i < rows.size(); i++)
		plan_of(i);
	rebuild();

	poll = new QTimer(this);
	connect(poll, &QTimer::timeout, this, [this]() {
		refresh();
		ask_stats();
	});
	poll->start(2000);
	refresh();
}

main_window::~main_window()
{
	for (row &r : rows)
		if (r.run != nullptr) {
			r.run->disconnect(this);
			r.run->kill();
			r.run->waitForFinished(2000);
		}
}

/*
 * The shortcuts keep off the keys qtty gives text editing on a terminal,
 * Ctrl+A, E, K, U, W and D, and Ctrl+L, its redraw: bound here, they would
 * answer differently by where the focus is, or never. Hence F7 and F8.
 */
void main_window::build_actions()
{
	add_action = new QAction(QStringLiteral("&Add..."), this);
	add_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+O")));
	add_action->setToolTip(QStringLiteral("Add a machine's hull.conf to the list"));
	connect(add_action, &QAction::triggered, this, &main_window::ask_add);
	remove_action = new QAction(QStringLiteral("&Remove from List"), this);
	connect(remove_action, &QAction::triggered, this, &main_window::remove_machine);
	start_action = new QAction(QStringLiteral("&Start"), this);
	start_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+S")));
	connect(start_action, &QAction::triggered, this, &main_window::start);
	stop_action = new QAction(QStringLiteral("S&top"), this);
	stop_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
	stop_action->setToolTip(QStringLiteral("Stop: TERM, then KILL after the machine's grace"));
	connect(stop_action, &QAction::triggered, this, &main_window::stop);
	kill_action = new QAction(QStringLiteral("&Power Off"), this);
	kill_action->setToolTip(QStringLiteral("KILL the workload at once"));
	connect(kill_action, &QAction::triggered, this, &main_window::kill_machine);
	pause_action = new QAction(QStringLiteral("&Pause"), this);
	pause_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+P")));
	connect(pause_action, &QAction::triggered, this, [this]() { pause_machine(true); });
	resume_action = new QAction(QStringLiteral("Res&ume"), this);
	resume_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
	connect(resume_action, &QAction::triggered, this, [this]() { pause_machine(false); });
	terminal_action = new QAction(QStringLiteral("&Terminal"), this);
	terminal_action->setShortcut(QKeySequence(QStringLiteral("F7")));
	terminal_action->setToolTip(QStringLiteral("A shell in the machine: hull exec -t ID /bin/sh"));
	connect(terminal_action, &QAction::triggered, this, &main_window::open_terminal);
	screen_action = new QAction(QStringLiteral("Scr&een"), this);
	screen_action->setShortcut(QKeySequence(QStringLiteral("F8")));
	screen_action->setToolTip(QStringLiteral("The machine's screen, where (display vnc) gives "
	                                         "one, in a VNC client"));
	connect(screen_action, &QAction::triggered, this, &main_window::open_screen);
	refresh_action = new QAction(QStringLiteral("Re&fresh"), this);
	refresh_action->setShortcut(QKeySequence(QStringLiteral("F5")));
	connect(refresh_action, &QAction::triggered, this, [this]() {
		for (int i = 0; i < rows.size(); i++)
			if (!rows[i].added.file.isEmpty())
				plan_of(i);
		refresh();
	});
	QAction *quit = new QAction(QStringLiteral("&Quit"), this);
	quit->setShortcut(QKeySequence(QStringLiteral("Ctrl+Q")));
	connect(quit, &QAction::triggered, this, &QWidget::close);

	QMenu *machine = menuBar()->addMenu(QStringLiteral("&Machine"));
	machine->addAction(add_action);
	machine->addAction(remove_action);
	machine->addSeparator();
	machine->addAction(start_action);
	machine->addAction(stop_action);
	machine->addAction(kill_action);
	machine->addAction(pause_action);
	machine->addAction(resume_action);
	machine->addAction(terminal_action);
	machine->addAction(screen_action);
	machine->addSeparator();
	machine->addAction(refresh_action);
	machine->addSeparator();
	machine->addAction(quit);

	QToolBar *bar = addToolBar(QStringLiteral("Machine"));
	bar->setObjectName(QStringLiteral("machine"));
	bar->setToolButtonStyle(Qt::ToolButtonTextOnly);
	bar->setMovable(false);
	bar->addAction(add_action);
	bar->addAction(start_action);
	bar->addAction(stop_action);
	bar->addAction(pause_action);
	bar->addAction(resume_action);
	bar->addAction(terminal_action);
	bar->addAction(screen_action);
	update_actions();
}

void main_window::say(const QString &text)
{
	statusBar()->showMessage(text, 8000);
}

int main_window::current() const
{
	QTreeWidgetItem *item = tree->currentItem();

	return item != nullptr ? item->data(0, Qt::UserRole).toInt() : -1;
}

/* The tree again from `rows`, the selection kept by what it names. */
void main_window::rebuild()
{
	const int  was = current();
	QString    keep_file;
	QString    keep_name;
	QString    keep_id;
	QTreeWidgetItem *select = nullptr;

	if (was >= 0 && was < rows.size()) {
		keep_file = rows[was].added.file;
		keep_name = rows[was].added.name;
		keep_id = rows[was].id;
	}
	tree->blockSignals(true);
	tree->clear();
	for (int i = 0; i < rows.size(); i++) {
		const row &r = rows[i];
		QString    state = r.is_held ? r.held.state : QStringLiteral("stopped");
		QString    backend;

		if (r.is_held)
			backend = r.held.engine + QStringLiteral(" (") + r.held.machine_class +
			          QStringLiteral(")");
		else if (r.run != nullptr)
			state = QStringLiteral("starting");
		if (r.is_held && r.held.root)
			state += QStringLiteral(" ROOT");
		if (backend.isEmpty()) {
			static const QRegularExpression on(QStringLiteral(" on (\\S+) \\((\\S+)\\): (ok|rejected, .*)$"),
			                                   QRegularExpression::MultilineOption);
			const QRegularExpressionMatch match = on.match(r.explain);

			if (match.hasMatch())
				backend = match.captured(1) + QStringLiteral(" (") + match.captured(2) +
				          QStringLiteral(")");
		}
		QTreeWidgetItem *item = new QTreeWidgetItem(
		    { r.id.isEmpty() ? QFileInfo(r.added.file).fileName() : r.id, state, backend });
		item->setData(0, Qt::UserRole, i);
		item->setToolTip(0, r.added.file.isEmpty() ? QStringLiteral("held by the core")
		                                          : r.added.file);
		tree->addTopLevelItem(item);
		if ((!keep_file.isEmpty() && r.added.file == keep_file && r.added.name == keep_name) ||
		    (keep_file.isEmpty() && !keep_id.isEmpty() && r.added.file.isEmpty() &&
		     r.id == keep_id))
			select = item;
	}
	for (int c = 0; c < 2; c++)
		tree->resizeColumnToContents(c);
	if (select == nullptr && tree->topLevelItemCount() > 0)
		select = tree->topLevelItem(0);
	tree->setCurrentItem(select);
	tree->blockSignals(false);
	if (current() != was || was < 0)
		selected_changed();
	else {
		update_actions();
		show_details();
	}
}

void main_window::refresh(std::function<void()> after)
{
	if (listing) {
		if (after)
			QTimer::singleShot(200, this, [this, after]() { refresh(after); });
		return;
	}
	listing = true;
	core_request *request = core_request::send(this, "(4:list", QString(), QByteArray(), 5000);

	connect(request, &core_request::machines, this, [this](const QList<machine_row> &held) {
		QList<row> next;

		for (row r : rows) {
			if (r.added.file.isEmpty())
				continue;  /* the core's own rows come back from its answer */
			r.is_held = false;
			for (const machine_row &m : held)
				if (m.id == r.id) {
					r.held = m;
					r.is_held = true;
				}
			next.append(r);
		}
		for (const machine_row &m : held) {
			bool known = false;

			for (const row &r : next)
				known = known || (r.is_held && r.held.id == m.id);
			if (!known) {
				row r;

				r.id = m.id;
				r.held = m;
				r.is_held = true;
				next.append(r);
			}
		}
		rows = next;
		core_state->setText(QStringLiteral("core: %1 machine%2")
		                        .arg(held.size())
		                        .arg(held.size() == 1 ? QString() : QStringLiteral("s")));
		rebuild();
	});
	connect(request, &core_request::finished, this,
	        [this, after](core_request::outcome how, const QString &reason) {
		        listing = false;
		        if (how == core_request::no_core) {
			        bool changed = false;

			        for (int i = rows.size() - 1; i >= 0; i--) {
				        changed = changed || rows[i].is_held;
				        rows[i].is_held = false;
				        if (rows[i].added.file.isEmpty())
					        rows.removeAt(i);
			        }
			        core_state->setText(QStringLiteral("core: not running"));
			        if (changed)
				        rebuild();
		        } else if (how != core_request::done) {
			        core_state->setText(QStringLiteral("core: ") + reason);
		        }
		        if (after)
			        after();
	        });
}

/*
 * The tokens a refused plan says would honour its refusals, "(--accept
 * TOKEN honours it)", each once. Root mode's are left out: the window
 * never makes root a click away (section 7), and its grant needs the host
 * owner's listing anyway, which the CLI's refusal explains.
 */
static QStringList tokens_wanted(const QString &explain)
{
	static const QRegularExpression honours(QStringLiteral("\\(--accept (\\S+) honours it\\)"));
	QStringList                     tokens;
	QRegularExpressionMatchIterator it = honours.globalMatch(explain);

	while (it.hasNext()) {
		const QString token = it.next().captured(1);

		if (!token.startsWith(QStringLiteral("root:")) && !tokens.contains(token))
			tokens << token;
	}
	return tokens;
}

/* `--accept TOKEN` for each of `accepted`. */
static QStringList accept_arguments(const QStringList &accepted)
{
	QStringList arguments;

	for (const QString &token : accepted)
		arguments << QStringLiteral("--accept") << token;
	return arguments;
}

/* `hull plan --explain --resolved` for an added machine: its backend, its
 * hostname and so its id, and what the matrix says of each field. */
void main_window::plan_of(int index)
{
	QProcess     *plan = new QProcess(this);
	const QString file = rows[index].added.file;
	const QString name = rows[index].added.name;
	QStringList   arguments = { QStringLiteral("plan"), QStringLiteral("-f"), file,
		                        QStringLiteral("--explain"), QStringLiteral("--resolved") };

	arguments << accept_arguments(rows[index].accepted);
	if (!name.isEmpty())
		arguments << name;
	plan->setProcessChannelMode(QProcess::MergedChannels);
	plan->setStandardInputFile(QProcess::nullDevice());
	plan->setWorkingDirectory(QFileInfo(file).absolutePath());
	connect(plan, &QProcess::finished, this, [this, plan, file, name](int code) {
		const QString said = QString::fromUtf8(plan->readAll());

		plan->deleteLater();
		for (row &r : rows)
			if (r.added.file == file && r.added.name == name) {
				QString resolved;

				for (const QString &line : said.split(QLatin1Char('\n')))
					if (line.startsWith(QStringLiteral("(machine ")))
						resolved = line;
				r.resolved = resolved;
				r.explain = code == 0 ? said : said + QStringLiteral("\n(refused: hull plan "
				                                                      "exited %1)\n")
				                                          .arg(code);
				r.id = machine_id_of(r.added, hostname_in(resolved));
			}
		rebuild();
	});
	connect(plan, &QProcess::errorOccurred, this, [this, plan, file](QProcess::ProcessError) {
		for (row &r : rows)
			if (r.added.file == file)
				r.explain = QStringLiteral("could not run %1: %2").arg(hull, plan->errorString());
		plan->deleteLater();
		rebuild();
	});
	plan->start(hull, arguments);
}

void main_window::selected_changed()
{
	last_cpu = -1;
	update_actions();
	show_details();
	show_output();
	usage->clear();
	ask_stats();
}

void main_window::update_actions()
{
	const int  i = current();
	const bool any = i >= 0 && i < rows.size();
	const QString state = any && rows[i].is_held ? rows[i].held.state : QString();
	/* Held and not yet ended: an exited machine stays listed until its
	 * run's frontend has gone, and there is nothing left to stop. */
	const bool held = state == QStringLiteral("running") || state == QStringLiteral("paused");

	remove_action->setEnabled(any && !rows[i].added.file.isEmpty());
	start_action->setEnabled(any && !rows[i].is_held && rows[i].run == nullptr &&
	                         !rows[i].added.file.isEmpty());
	stop_action->setEnabled(held);
	kill_action->setEnabled(held);
	pause_action->setEnabled(held && state != QStringLiteral("paused"));
	resume_action->setEnabled(held && state == QStringLiteral("paused"));
	terminal_action->setEnabled(state == QStringLiteral("running"));
	screen_action->setEnabled(state == QStringLiteral("running") && !rows[i].held.address.isEmpty());
}

void main_window::show_details()
{
	const int i = current();
	QString   text;

	if (i < 0 || i >= rows.size()) {
		details->setPlainText(QStringLiteral("No machine. Add a hull.conf with Machine > Add, "
		                                     "or start one with `hull run`: the core's are "
		                                     "listed here whoever started them."));
		return;
	}
	const row &r = rows[i];
	auto line = [&text](const char *key, const QString &value) {
		if (!value.isEmpty())
			text += QStringLiteral("%1 %2\n").arg(QString::fromLatin1(key), -10).arg(value);
	};
	line("Id", r.id);
	line("File", r.added.file);
	line("Name", r.added.name);
	line("State", r.is_held ? r.held.state : QStringLiteral("stopped"));
	if (r.is_held) {
		line("Engine", r.held.engine);
		line("Class", r.held.machine_class);
		line("Address", r.held.address);
		line("Arch", r.held.arch);
		if (r.held.root)
			line("Root", QStringLiteral("yes: root mode granted this machine (section 7)"));
	}
	if (!r.accepted.isEmpty())
		line("Accepted", r.accepted.join(QLatin1Char(' ')));
	if (!r.resolved.isEmpty())
		text += QStringLiteral("\nAs planned:\n%1\n").arg(r.resolved);
	if (!r.explain.isEmpty()) {
		QString explain;

		for (const QString &l : r.explain.split(QLatin1Char('\n')))
			if (!l.startsWith(QStringLiteral("(machine ")))
				explain += l + QLatin1Char('\n');
		text += QStringLiteral("\nPlan:\n") + explain.trimmed() + QLatin1Char('\n');
	}
	if (r.added.file.isEmpty())
		text += QStringLiteral("\nStarted outside this window; add its hull.conf to start it "
		                       "again from here.\n");
	if (details->toPlainText() != text)
		details->setPlainText(text);
}

/* The Output tab: a run started here, as it said it; else, for a machine
 * the core holds, its log from the core, followed. */
void main_window::show_output()
{
	const int i = current();

	if (logs != nullptr) {
		logs->disconnect(this);
		logs->deleteLater();
		logs = nullptr;
		logs_of.clear();
	}
	output->clear();
	if (i < 0 || i >= rows.size())
		return;
	if (rows[i].run != nullptr || !rows[i].output.isEmpty()) {
		output->setPlainText(QString::fromUtf8(rows[i].output));
		output->verticalScrollBar()->setValue(output->verticalScrollBar()->maximum());
		return;
	}
	if (rows[i].is_held)
		follow_logs();
}

void main_window::follow_logs()
{
	const int i = current();

	if (i < 0 || i >= rows.size() || !rows[i].is_held)
		return;
	logs_of = rows[i].id;
	logs = core_request::send(this, "(4:logs", logs_of, QByteArray("(6:follow4:true)"), 0);
	connect(logs, &core_request::output, this, [this](int, const QByteArray &bytes) {
		output->moveCursor(QTextCursor::End);
		output->insertPlainText(QString::fromUtf8(bytes));
		output->verticalScrollBar()->setValue(output->verticalScrollBar()->maximum());
	});
	connect(logs, &core_request::finished, this,
	        [this](core_request::outcome how, const QString &reason) {
		        logs = nullptr;
		        if (how != core_request::done || !reason.isEmpty())
			        output->appendPlainText(QStringLiteral("[%1]").arg(
			            reason.isEmpty() ? QStringLiteral("ended") : reason));
	        });
}

void main_window::ask_stats()
{
	const int i = current();

	if (i < 0 || i >= rows.size() || !rows[i].is_held ||
	    rows[i].held.state != QStringLiteral("running"))
		return;
	const QString id = rows[i].id;
	core_request *request = core_request::send(this, "(5:stats", id, QByteArray(), 5000);

	connect(request, &core_request::stats, this, [this, id](qint64 cpu, qint64 io, qint64 memory) {
		const int       now_i = current();
		struct timespec now;
		QString         rate;

		if (now_i < 0 || now_i >= rows.size() || rows[now_i].id != id)
			return;
		clock_gettime(CLOCK_MONOTONIC, &now);
		const qint64 at = (qint64)now.tv_sec * 1000000 + now.tv_nsec / 1000;
		if (last_cpu >= 0 && at > last_at)
			rate = QStringLiteral(" (%1 % of one CPU)")
			           .arg(100.0 * (double)(cpu - last_cpu) / (double)(at - last_at), 0, 'f', 1);
		last_cpu = cpu;
		last_at = at;
		usage->setPlainText(QStringLiteral("CPU     %1 s%2\nIO      %3 bytes\nMemory  %4 "
		                                   "bytes resident (Pss)\n")
		                        .arg((double)cpu / 1e6, 0, 'f', 2)
		                        .arg(rate)
		                        .arg(io)
		                        .arg(memory));
	});
	connect(request, &core_request::finished, this,
	        [this](core_request::outcome how, const QString &reason) {
		        if (how == core_request::refused)
			        usage->setPlainText(reason);
	        });
}

void main_window::add_machine(const QString &given, const QString &name)
{
	const QString file = QFileInfo(given).absoluteFilePath();
	QList<added_machine> added = load_added();
	QStringList names;

	if (name.isEmpty()) {
		/* A file of (machines ...) is one row each, as `hull plan` names
		 * them: "FILE: NAME on ENGINE (CLASS): ok". */
		QProcess plan;
		static const QRegularExpression line(QStringLiteral("^[^\\n]*: (\\S+) on \\S+ \\(\\S+\\)"),
		                                     QRegularExpression::MultilineOption);

		plan.setStandardInputFile(QProcess::nullDevice());
		plan.setWorkingDirectory(QFileInfo(file).absolutePath());
		plan.start(hull, { QStringLiteral("plan"), QStringLiteral("-f"), file });
		if (plan.waitForFinished(10000)) {
			QRegularExpressionMatchIterator it =
			    line.globalMatch(QString::fromUtf8(plan.readAllStandardOutput()));

			/* Each machine once: a refused one is named again on the
			 * line of each field refused. */
			while (it.hasNext()) {
				const QString found = it.next().captured(1);

				if (!names.contains(found))
					names << found;
			}
		}
		if (names.size() < 2)
			names = { QString() };
	} else {
		names = { name };
	}
	for (const QString &n : names) {
		added_machine machine{ file, n };
		row           r;

		if (added.contains(machine))
			continue;
		added.append(machine);
		r.added = machine;
		r.id = machine_id_of(machine, QString());
		rows.append(r);
		plan_of((int)rows.size() - 1);
	}
	save_added(added);
	rebuild();
	refresh();
}

void main_window::ask_add()
{
	QString file;

	if (on_terminal()) {
		bool given = false;

		file = QInputDialog::getText(this, QStringLiteral("Add Machine"),
		                             QStringLiteral("A machine's hull.conf:"), QLineEdit::Normal,
		                             QDir::currentPath() + QStringLiteral("/hull.conf"), &given);
		if (!given)
			return;
	} else {
		file = QFileDialog::getOpenFileName(this, QStringLiteral("Add Machine"),
		                                    QDir::currentPath(),
		                                    QStringLiteral("hull specs (hull.conf *.conf);;All "
		                                                   "files (*)"));
	}
	if (file.isEmpty())
		return;
	if (!QFileInfo(file).isFile()) {
		say(QStringLiteral("%1: no such file").arg(file));
		return;
	}
	add_machine(file, QString());
}

void main_window::remove_machine()
{
	const int i = current();

	if (i < 0 || i >= rows.size() || rows[i].added.file.isEmpty())
		return;
	QList<added_machine> added = load_added();

	added.removeAll(rows[i].added);
	save_added(added);
	if (rows[i].run != nullptr || rows[i].is_held) {
		/* Still running: the core's row it stays, until it ends. */
		rows[i].added = added_machine();
	} else {
		rows.removeAt(i);
	}
	rebuild();
}

void main_window::start()
{
	const int i = current();

	if (i < 0 || i >= rows.size() || rows[i].added.file.isEmpty() || rows[i].run != nullptr)
		return;
	const added_machine which = rows[i].added;
	const QStringList   wanted = tokens_wanted(rows[i].explain);
	const QString       explain = rows[i].explain;
	QStringList         asked;
	int                 at = i;

	/* A plan refused where a token would honour it: asked, with the lines
	 * that say why, once a session; the run then carries --accept. */
	for (const QString &token : wanted)
		if (!rows[i].accepted.contains(token))
			asked << token;
	if (!asked.isEmpty()) {
		QStringList lines;

		for (const QString &line : explain.split(QLatin1Char('\n')))
			for (const QString &token : asked)
				if (line.contains(QStringLiteral("--accept ") + token + QLatin1Char(' ')))
					lines << line.section(QStringLiteral(": "), 1);
		if (QMessageBox::question(
		        this, QStringLiteral("Accept"),
		        QStringLiteral("This machine's plan refuses what the backend can only "
		                       "approximate:\n\n%1\n\nRun it with %2?")
		            .arg(lines.join(QLatin1Char('\n')),
		                 accept_arguments(asked).join(QLatin1Char(' ')))) != QMessageBox::Yes)
			return;
		/* The dialog ran the event loop, and a refresh may have built the
		 * rows again: found anew, by what it names. */
		at = -1;
		for (int k = 0; k < rows.size(); k++)
			if (rows[k].added == which)
				at = k;
		if (at < 0 || rows[at].is_held || rows[at].run != nullptr)
			return;
		rows[at].accepted << asked;
		plan_of(at);
	}
	row        &r = rows[at];
	QProcess   *run = new QProcess(this);
	QStringList arguments = { QStringLiteral("run"), QStringLiteral("-f"), r.added.file };

	arguments << accept_arguments(r.accepted);
	if (!r.added.name.isEmpty())
		arguments << r.added.name;
	run->setProcessChannelMode(QProcess::MergedChannels);
	run->setStandardInputFile(QProcess::nullDevice());
	run->setWorkingDirectory(QFileInfo(r.added.file).absolutePath());
	r.run = run;
	r.output = QByteArray("$ hull ") + arguments.join(QLatin1Char(' ')).toUtf8() + "\n";
	auto row_of = [this, which]() -> row * {
		for (row &each : rows)
			if (each.added == which)
				return &each;
		return nullptr;
	};
	connect(run, &QProcess::readyRead, this, [this, run, row_of]() {
		const QByteArray said = run->readAll();
		row             *r = row_of();

		if (r == nullptr)
			return;
		r->output += said;
		if (r->output.size() > 1 << 20)
			r->output = r->output.right(1 << 20);
		if (current() >= 0 && current() < rows.size() && &rows[current()] == r) {
			output->moveCursor(QTextCursor::End);
			output->insertPlainText(QString::fromUtf8(said));
			output->verticalScrollBar()->setValue(output->verticalScrollBar()->maximum());
		}
	});
	connect(run, &QProcess::finished, this, [this, run, row_of](int code, QProcess::ExitStatus) {
		row *r = row_of();
		const QByteArray said = QStringLiteral("[hull run exited %1]\n").arg(code).toUtf8();

		run->deleteLater();
		if (r != nullptr) {
			r->run = nullptr;
			r->output += said;
			if (current() >= 0 && current() < rows.size() && &rows[current()] == r)
				output->appendPlainText(QString::fromUtf8(said).trimmed());
		}
		refresh();
	});
	connect(run, &QProcess::errorOccurred, this, [this, run, row_of](QProcess::ProcessError e) {
		row *r = row_of();

		if (e != QProcess::FailedToStart)
			return;
		say(QStringLiteral("could not run %1: %2").arg(hull, run->errorString()));
		if (r != nullptr)
			r->run = nullptr;
		run->deleteLater();
		rebuild();
	});
	run->start(hull, arguments);
	show_output();
	tabs->setCurrentWidget(output);
	rebuild();
	/* The core lists it once the run has reached it. */
	QTimer::singleShot(500, this, [this]() { refresh(); });
}

void main_window::stop()
{
	const int i = current();

	if (i < 0 || i >= rows.size() || !rows[i].is_held)
		return;
	const QString id = rows[i].id;
	/* -1: the machine's own grace, as `hull stop` without --timeout. */
	core_request *request =
	    core_request::send(this, "(4:stop", id, QByteArray("(10:timeout_ms2:-1)"), 0);

	say(QStringLiteral("stopping %1").arg(id));
	connect(request, &core_request::finished, this,
	        [this, id](core_request::outcome how, const QString &reason) {
		        say(how == core_request::done ? QStringLiteral("%1 stopped").arg(id)
		                                      : QStringLiteral("%1: %2").arg(id, reason));
		        refresh();
	        });
}

void main_window::kill_machine()
{
	const int i = current();

	if (i < 0 || i >= rows.size() || !rows[i].is_held)
		return;
	const QString id = rows[i].id;
	core_request *request =
	    core_request::send(this, "(4:kill", id, QByteArray("(6:signal4:KILL)"), 10000);

	connect(request, &core_request::finished, this,
	        [this, id](core_request::outcome how, const QString &reason) {
		        say(how == core_request::done ? QStringLiteral("%1 powered off").arg(id)
		                                      : QStringLiteral("%1: %2").arg(id, reason));
		        refresh();
	        });
}

void main_window::pause_machine(bool paused)
{
	const int i = current();

	if (i < 0 || i >= rows.size() || !rows[i].is_held)
		return;
	const QString id = rows[i].id;
	core_request *request =
	    core_request::send(this, paused ? "(5:pause" : "(7:unpause", id, QByteArray(), 10000);

	connect(request, &core_request::finished, this,
	        [this, id, paused](core_request::outcome how, const QString &reason) {
		        say(how == core_request::done
		                ? QStringLiteral("%1 %2").arg(id, paused ? QStringLiteral("paused")
		                                                         : QStringLiteral("resumed"))
		                : QStringLiteral("%1: %2").arg(id, reason));
		        refresh();
	        });
}

/*
 * A shell in the selected machine, `hull exec -t ID /bin/sh`, the CLI's,
 * on a terminal of its own: on the desktop a terminal emulator's window,
 * $TERMINAL's, else x-terminal-emulator's, else xterm's; under qtty this
 * terminal, handed over for as long as the shell runs. A shell that cannot
 * start leaves its reason on the screen until Enter, rather than a window
 * that closes before it can be read. A machine with no agent -- a pc --
 * has no exec, and says so there.
 */
void main_window::open_terminal()
{
	const int i = current();

	if (i < 0 || i >= rows.size() || !rows[i].is_held)
		return;
	const QString id = rows[i].id;
	const QString script = QStringLiteral(
	    "\"$0\" exec -t \"$1\" /bin/sh || { echo; echo 'press Enter to close'; read x; }");
	const QStringList shell = { QStringLiteral("/bin/sh"), QStringLiteral("-c"), script, hull, id };

	if (on_terminal()) {
#ifdef HULL_QTTY
		Qtty::shell_out([&shell]() {
			QByteArrayList bytes;
			QList<char *>  argv;
			pid_t          child;

			for (const QString &arg : shell)
				bytes.append(arg.toLocal8Bit());
			for (QByteArray &arg : bytes)
				argv.append(arg.data());
			argv.append(nullptr);
			child = fork();
			if (child == 0) {
				execv(argv[0], argv.data());
				_exit(127);
			}
			if (child > 0)
				while (waitpid(child, nullptr, 0) < 0 && errno == EINTR)
					;
		});
#endif
		return;
	}
	QStringList candidates;

	if (!qEnvironmentVariableIsEmpty("TERMINAL"))
		candidates << qEnvironmentVariable("TERMINAL");
	candidates << QStringLiteral("x-terminal-emulator") << QStringLiteral("xterm");
	for (const QString &candidate : candidates) {
		const QString found = QStandardPaths::findExecutable(candidate);

		if (!found.isEmpty() && QProcess::startDetached(found, QStringList{ QStringLiteral("-e") } + shell)) {
			say(QStringLiteral("a terminal on %1").arg(id));
			return;
		}
	}
	say(QStringLiteral("no terminal emulator found: set $TERMINAL, or run `hull exec -t %1 "
	                   "/bin/sh`").arg(id));
}

/* Whether something answers TCP at `address`:`port` within a second. */
static bool answers(const QString &address, quint16 port)
{
	struct sockaddr_in to;
	struct pollfd      ready;
	int                error = 0;
	socklen_t          length = sizeof error;
	int                fd;
	bool               open = false;

	memset(&to, 0, sizeof to);
	to.sin_family = AF_INET;
	to.sin_port = htons(port);
	if (inet_pton(AF_INET, address.toLatin1().constData(), &to.sin_addr) != 1)
		return false;
	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return false;
	if (::connect(fd, (struct sockaddr *)&to, sizeof to) == 0) {
		open = true;
	} else if (errno == EINPROGRESS) {
		ready.fd = fd;
		ready.events = POLLOUT;
		open = poll(&ready, 1, 1000) == 1 &&
		       getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
	}
	close(fd);
	return open;
}

/*
 * The selected machine's screen: `(display vnc)` serves it on port 5900
 * of the machine's address (section 5.0's pc), for any VNC client. On the
 * desktop the first client found is started on it; under qtty there is no
 * screen to show one on, so the address is said. A machine with no screen
 * says so rather than a client that fails to connect.
 */
void main_window::open_screen()
{
	const int i = current();

	if (i < 0 || i >= rows.size() || !rows[i].is_held || rows[i].held.address.isEmpty())
		return;
	const QString id = rows[i].id;
	const QString address = rows[i].held.address;
	const QString at = address + QStringLiteral(":5900");

	if (!answers(address, 5900)) {
		say(QStringLiteral("%1 shows no screen at %2: a machine gives one with (display vnc)")
		        .arg(id, at));
		return;
	}
	if (on_terminal()) {
		say(QStringLiteral("%1's screen is at %2, for a VNC client").arg(id, at));
		return;
	}
	const QList<QStringList> clients = {
		{ QStringLiteral("vncviewer"), address + QStringLiteral("::5900") },
		{ QStringLiteral("xtigervncviewer"), address + QStringLiteral("::5900") },
		{ QStringLiteral("remmina"), QStringLiteral("-c"), QStringLiteral("vnc://") + at },
		{ QStringLiteral("krdc"), QStringLiteral("vnc://") + at },
		{ QStringLiteral("xdg-open"), QStringLiteral("vnc://") + at },
	};
	for (const QStringList &client : clients) {
		const QString found = QStandardPaths::findExecutable(client.first());

		if (!found.isEmpty() && QProcess::startDetached(found, client.mid(1))) {
			say(QStringLiteral("%1's screen, %2, in %3").arg(id, at, client.first()));
			return;
		}
	}
	say(QStringLiteral("no VNC client found: %1's screen is at %2").arg(id, at));
}

void main_window::closeEvent(QCloseEvent *event)
{
	int running = 0;

	for (const row &r : rows)
		running += r.run != nullptr;
	if (running > 0 &&
	    QMessageBox::question(
	        this, QStringLiteral("Quit"),
	        QStringLiteral("%1 machine%2 started from this window %3 still running. A run ends "
	                       "with the window, and its machine then does what its "
	                       "(on_disconnect ...) says: it stops, unless it says keep. Quit?")
	            .arg(running)
	            .arg(running == 1 ? QString() : QStringLiteral("s"))
	            .arg(running == 1 ? QStringLiteral("is") : QStringLiteral("are"))) !=
	        QMessageBox::Yes) {
		event->ignore();
		return;
	}
	event->accept();
}
