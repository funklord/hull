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
#include "images_dialog.h"
#include "pools_dialog.h"
#include "new_machine_dialog.h"
#include "settings_dialog.h"
#include "screen_window.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
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
/* Whether `file` is a machine's hull.conf in a folder of its own in the
 * machines' home, as New, Import and Clone make. */
static bool in_own_folder(const QString &file)
{
	return !file.isEmpty() && QFileInfo(file).fileName() == QStringLiteral("hull.conf") &&
	       QFileInfo(QFileInfo(file).absolutePath()).absolutePath() == machines_home();
}

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
	/* Its three columns whole beside details that do not wrap, which
	 * would otherwise take the width: about 36 characters, cells under
	 * qtty. */
	tree->setMinimumWidth(tree->fontMetrics().horizontalAdvance(QString(36, QLatin1Char('x'))));
	connect(tree, &QTreeWidget::currentItemChanged, this, &main_window::selected_changed);

	details = new QPlainTextEdit;
	details->setReadOnly(true);
	/* Its keys are a column, which a proportional font does not keep. */
	details->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	details->setLineWrapMode(QPlainTextEdit::NoWrap);
	output = new QPlainTextEdit;
	output->setReadOnly(true);
	output->setMaximumBlockCount(5000);
	usage = new QPlainTextEdit;
	usage->setReadOnly(true);
	tabs = new QTabWidget;
	tabs->addTab(details, QStringLiteral("&Details"));
	tabs->addTab(output, QStringLiteral("&Output"));
	tabs->addTab(usage, QStringLiteral("&Usage"));
	tabs->addTab(build_network(), QStringLiteral("&Network"));
	/* The screen beside the list, as the holder asked of VirtualBox's
	 * layout (project.md section 4.14): shown while this tab is, and only
	 * then connected, so a screen nobody looks at costs qemu nothing;
	 * Detach gives it a window of its own, which F8 opens too. */
	screen_tab = new QWidget;
	{
		auto *layout = new QVBoxLayout(screen_tab);
		auto *row = new QHBoxLayout;
		auto *detach = new QPushButton(QStringLiteral("&Detach"));

		screen_note = new QLabel;
		screen_note->setWordWrap(true);
		row->addWidget(screen_note, 1);
		row->addWidget(detach);
		layout->addLayout(row);
		layout->setContentsMargins(0, 0, 0, 0);
		connect(detach, &QPushButton::clicked, this, [this]() {
			delete embedded;
			embedded_id.clear();
			tabs->setCurrentIndex(0);
			open_screen();
		});
	}
	if (!on_terminal())
		tabs->addTab(screen_tab, QStringLiteral("Scr&een"));
	connect(tabs, &QTabWidget::currentChanged, this, [this]() {
		ask_ports();
		update_screen_tab();
	});

	QSplitter *split = new QSplitter;
	split->addWidget(tree);
	split->addWidget(tabs);
	split->setStretchFactor(0, 1);
	split->setStretchFactor(1, 2);
	setCentralWidget(split);

	core_state = new QLabel;
	statusBar()->addPermanentWidget(core_state);

	build_actions();

	/* The machines' home's folders join the list, as VirtualBox's own
	 * folder's machines are in its manager. */
	{
		QList<added_machine>       added = load_added();
		const QList<added_machine> found = found_in(machines_home(), added, load_removed());

		if (!found.isEmpty()) {
			added.append(found);
			save_added(added);
		}
	}
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
		ask_ports();
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
 * The actions carry no mnemonic. Under qtty a menu action's Alt+letter
 * answers with its menu closed and beats a tab's or a button's, so
 * Alt+N opened New where the Network tab was meant; the actions have
 * shortcuts, and the menus' own titles keep theirs.
 *
 * The shortcuts keep off the keys qtty gives text editing on a terminal,
 * Ctrl+A, E, K, U, W and D, and Ctrl+L, its redraw: bound here, they would
 * answer differently by where the focus is, or never. Hence F6 to F9.
 */
void main_window::build_actions()
{
	new_action = new QAction(QStringLiteral("New..."), this);
	new_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+N")));
	new_action->setToolTip(QStringLiteral("Write a new machine's hull.conf and add it"));
	connect(new_action, &QAction::triggered, this, [this]() { ask_new(); });
	add_action = new QAction(QStringLiteral("Add..."), this);
	add_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+O")));
	add_action->setToolTip(QStringLiteral("Add a machine's hull.conf to the list"));
	connect(add_action, &QAction::triggered, this, &main_window::ask_add);
	import_action = new QAction(QStringLiteral("Import..."), this);
	import_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+I")));
	import_action->setToolTip(QStringLiteral("Copy a VirtualBox or VMware machine or an "
	                                         "appliance into a folder of its own in ~/hull"));
	connect(import_action, &QAction::triggered, this, &main_window::ask_import);
	export_action = new QAction(QStringLiteral("Export Appliance..."), this);
	export_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+X")));
	export_action->setToolTip(QStringLiteral("The machine as an .ova, which VirtualBox and "
	                                         "VMware import"));
	connect(export_action, &QAction::triggered, this, &main_window::export_appliance);
	edit_action = new QAction(QStringLiteral("Edit hull.conf"), this);
	edit_action->setShortcut(QKeySequence(QStringLiteral("F4")));
	edit_action->setToolTip(QStringLiteral("The machine's file in an editor, planned again after"));
	connect(edit_action, &QAction::triggered, this, &main_window::edit_spec);
	settings_action = new QAction(QStringLiteral("Settings..."), this);
	settings_action->setShortcut(QKeySequence(QStringLiteral("F3")));
	settings_action->setToolTip(QStringLiteral("The machine's hardware in a form, written back "
	                                           "into its hull.conf"));
	connect(settings_action, &QAction::triggered, this, &main_window::edit_settings);
	clone_action = new QAction(QStringLiteral("Clone..."), this);
	clone_action->setToolTip(QStringLiteral("Copy a machine in ~/hull, its disks with it, "
	                                        "into a folder of its own"));
	connect(clone_action, &QAction::triggered, this, &main_window::clone_machine);
	remove_action = new QAction(QStringLiteral("Remove..."), this);
	connect(remove_action, &QAction::triggered, this, &main_window::remove_machine);
	start_action = new QAction(QStringLiteral("Start"), this);
	start_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+S")));
	connect(start_action, &QAction::triggered, this, &main_window::start);
	stop_action = new QAction(QStringLiteral("Stop"), this);
	stop_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
	stop_action->setToolTip(QStringLiteral("Stop: TERM, then KILL after the machine's grace"));
	connect(stop_action, &QAction::triggered, this, &main_window::stop);
	kill_action = new QAction(QStringLiteral("Power Off"), this);
	kill_action->setToolTip(QStringLiteral("KILL the workload at once"));
	connect(kill_action, &QAction::triggered, this, &main_window::kill_machine);
	pause_action = new QAction(QStringLiteral("Pause"), this);
	pause_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+P")));
	connect(pause_action, &QAction::triggered, this, [this]() { pause_machine(true); });
	resume_action = new QAction(QStringLiteral("Resume"), this);
	resume_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
	connect(resume_action, &QAction::triggered, this, [this]() { pause_machine(false); });
	terminal_action = new QAction(QStringLiteral("Terminal"), this);
	terminal_action->setShortcut(QKeySequence(QStringLiteral("F7")));
	terminal_action->setToolTip(QStringLiteral("A shell in the machine: hull exec -t ID /bin/sh"));
	connect(terminal_action, &QAction::triggered, this, &main_window::open_terminal);
	screen_action = new QAction(QStringLiteral("Screen"), this);
	screen_action->setShortcut(QKeySequence(QStringLiteral("F8")));
	screen_action->setToolTip(QStringLiteral("The machine's screen, where (display vnc) gives "
	                                         "one, in a VNC client"));
	connect(screen_action, &QAction::triggered, this, &main_window::open_screen);
	commit_action = new QAction(QStringLiteral("Commit to Image..."), this);
	commit_action->setShortcut(QKeySequence(QStringLiteral("F6")));
	commit_action->setToolTip(QStringLiteral("The running machine's tree as it is now, into the "
	                                         "store: hull commit ID REF"));
	connect(commit_action, &QAction::triggered, this, &main_window::commit_machine);
	medium_action = new QAction(QStringLiteral("Change Medium..."), this);
	medium_action->setShortcut(QKeySequence(QStringLiteral("F9")));
	medium_action->setToolTip(QStringLiteral("A running pc machine's floppy or CD swapped, or "
	                                         "taken out: hull media change ID DRIVE [IMAGE]"));
	connect(medium_action, &QAction::triggered, this, &main_window::change_medium);
	/* VirtualBox's Input > Keyboard > Insert Ctrl-Alt-Del and View > Take
	 * Screenshot: the chord is the desktop's own before any window sees
	 * it, and a screen to keep is wanted where no VNC client is open. */
	keys_action = new QAction(QStringLiteral("Insert Ctrl-Alt-Del"), this);
	keys_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+End")));
	keys_action->setToolTip(QStringLiteral("Ctrl-Alt-Del pressed on a running pc machine's "
	                                       "keyboard: hull key ID ctrl-alt-delete"));
	connect(keys_action, &QAction::triggered, this, &main_window::insert_ctrl_alt_del);
	screenshot_action = new QAction(QStringLiteral("Take Screenshot..."), this);
	screenshot_action->setShortcut(QKeySequence(QStringLiteral("F12")));
	screenshot_action->setToolTip(QStringLiteral("A running pc machine's screen, a PNG: hull "
	                                             "screenshot ID FILE"));
	connect(screenshot_action, &QAction::triggered, this, &main_window::take_screenshot);
	refresh_action = new QAction(QStringLiteral("Refresh"), this);
	refresh_action->setShortcut(QKeySequence(QStringLiteral("F5")));
	connect(refresh_action, &QAction::triggered, this, [this]() {
		for (int i = 0; i < rows.size(); i++)
			if (!rows[i].added.file.isEmpty())
				plan_of(i);
		refresh();
	});
	QAction *quit = new QAction(QStringLiteral("Quit"), this);
	quit->setShortcut(QKeySequence(QStringLiteral("Ctrl+Q")));
	connect(quit, &QAction::triggered, this, &QWidget::close);

	QMenu *machine = menuBar()->addMenu(QStringLiteral("&Machine"));
	machine->addAction(new_action);
	machine->addAction(add_action);
	machine->addAction(import_action);
	machine->addAction(export_action);
	machine->addAction(settings_action);
	machine->addAction(edit_action);
	machine->addAction(clone_action);
	machine->addAction(remove_action);
	machine->addSeparator();
	machine->addAction(start_action);
	machine->addAction(stop_action);
	machine->addAction(kill_action);
	machine->addAction(pause_action);
	machine->addAction(resume_action);
	machine->addAction(terminal_action);
	machine->addAction(screen_action);
	machine->addAction(commit_action);
	machine->addAction(medium_action);
	machine->addAction(keys_action);
	machine->addAction(screenshot_action);
	machine->addSeparator();
	machine->addAction(refresh_action);
	machine->addSeparator();
	machine->addAction(quit);

	QMenu   *store = menuBar()->addMenu(QStringLiteral("&Store"));
	QAction *images = store->addAction(QStringLiteral("Images..."));

	images->setToolTip(QStringLiteral("The store's images: pull, remove, a machine from one"));
	connect(images, &QAction::triggered, this, &main_window::show_images);

	QMenu   *pool_menu = menuBar()->addMenu(QStringLiteral("&Pools"));
	QAction *pools = pool_menu->addAction(QStringLiteral("Pools and Leases..."));

	pools->setToolTip(QStringLiteral("The core's pools and leases: reserve, release"));
	connect(pools, &QAction::triggered, this, [this]() {
		pools_dialog dialog(hull, this);

		dialog.resize(760, 480);
		dialog.exec();
	});

	QMenu   *help = menuBar()->addMenu(QStringLiteral("&Help"));
	QAction *about = help->addAction(QStringLiteral("About hull-gui..."));

	connect(about, &QAction::triggered, this, [this]() {
		/* Rich text: the address escaped, or it is read as a tag and
		 * dropped (project-attribution). */
		QMessageBox::about(this, QStringLiteral("About hull-gui"),
		                   QStringLiteral("<p><b>hull-gui %1</b></p><p>hull's machine manager: "
		                                  "containers, virtual machines and emulated systems "
		                                  "from one spec, started and watched from one window."
		                                  "</p><p>%2</p>")
		                       .arg(QApplication::applicationVersion(),
		                            QStringLiteral("Copyright (C) 2026 Nabeel Sowan "
		                                           "<nabeel@vibes.se>").toHtmlEscaped()));
	});

	QToolBar *bar = addToolBar(QStringLiteral("Machine"));
	bar->setObjectName(QStringLiteral("machine"));
	bar->setToolButtonStyle(Qt::ToolButtonTextOnly);
	bar->setMovable(false);
	bar->addAction(new_action);
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
		if (!wanted.isEmpty() && r.id == wanted) {
			select = item;
			wanted.clear();
		}
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
		update_screen_tab();
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
 * hostname and so its id, and what the matrix says of each field. plan
 * takes no machine's name, so a file of (machines ...) is planned whole
 * and a named row keeps its own lines: "...: NAME on ..." of the explain,
 * and the resolved form whose hostname is the name, which the plan writes
 * in for each machine of such a file. */
void main_window::plan_of(int index)
{
	QProcess     *plan = new QProcess(this);
	const QString file = rows[index].added.file;
	const QString name = rows[index].added.name;
	QStringList   arguments = { QStringLiteral("plan"), QStringLiteral("-f"), file,
		                        QStringLiteral("--explain"), QStringLiteral("--resolved") };

	arguments << accept_arguments(rows[index].accepted);
	plan->setProcessChannelMode(QProcess::MergedChannels);
	plan->setStandardInputFile(QProcess::nullDevice());
	plan->setWorkingDirectory(QFileInfo(file).absolutePath());
	connect(plan, &QProcess::finished, this, [this, plan, file, name](int code) {
		const QString said = QString::fromUtf8(plan->readAll());

		plan->deleteLater();
		for (row &r : rows)
			if (r.added.file == file && r.added.name == name) {
				QString resolved;
				QString own;

				for (const QString &line : said.split(QLatin1Char('\n'))) {
					if (line.startsWith(QStringLiteral("(machine "))) {
						if (resolved.isEmpty() &&
						    (name.isEmpty() || hostname_in(line) == name))
							resolved = line;
					} else if (name.isEmpty() ||
					           line.contains(QStringLiteral(": ") + name + QStringLiteral(" on "))) {
						own += line + QLatin1Char('\n');
					}
				}
				r.resolved = resolved;
				r.explain = code == 0 ? own : own + QStringLiteral("\n(refused: hull plan "
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
	ports->clear();
	ask_ports();
	update_screen_tab();
}

bool main_window::show_tab(const QString &name)
{
	for (int t = 0; t < tabs->count(); t++)
		if (tabs->tabText(t).remove(QLatin1Char('&')).compare(name, Qt::CaseInsensitive) == 0) {
			tabs->setCurrentIndex(t);
			return true;
		}
	return false;
}

/* The Screen tab follows the selection while it is the tab shown; any
 * other time it holds no screen, its listener dropped with it. */
void main_window::update_screen_tab()
{
	const int     i = current();
	const bool    showing = tabs->currentWidget() == screen_tab;
	const bool    running = i >= 0 && i < rows.size() && rows[i].is_held &&
	                        !rows[i].held.address.isEmpty();
	const QString id = running ? rows[i].id : QString();

	if (embedded != nullptr && (!showing || id != embedded_id)) {
		delete embedded;
		embedded_id.clear();
	}
	if (!showing || embedded != nullptr)
		return;
	if (!running) {
		screen_note->setText(QStringLiteral("The machine is not running."));
		return;
	}
	const QString runtime = QString::fromLocal8Bit(qgetenv("XDG_RUNTIME_DIR"));
	const QString local = runtime + QStringLiteral("/hull/display/") + rows[i].held.address;

	if (runtime.isEmpty() || !QFileInfo::exists(local)) {
		screen_note->setText(QStringLiteral("%1 has no screen shown here: (display local) or "
		                                    "(display window) gives one; (display vnc) is a VNC "
		                                    "client's, F8").arg(id));
		return;
	}
	auto   *screen = new screen_window(id, local, screen_tab);
	QString why;

	if (!screen->open(&why)) {
		delete screen;
		screen_note->setText(QStringLiteral("%1's screen cannot be shown: %2").arg(id, why));
		return;
	}
	embedded = screen;
	embedded_id = id;
	screen_note->setText(QStringLiteral("%1 -- a click captures the keyboard, Right Ctrl "
	                                    "gives it back").arg(id));
	static_cast<QVBoxLayout *>(screen_tab->layout())->addWidget(screen, 1);
	screen->show();
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
	clone_action->setEnabled(any && in_own_folder(rows[i].added.file) &&
	                         rows[i].added.name.isEmpty() && !held && rows[i].run == nullptr);
	edit_action->setEnabled(any && !rows[i].added.file.isEmpty());
	/* A stopped machine's disks: a running one's are being written. */
	export_action->setEnabled(any && !rows[i].added.file.isEmpty() && !held &&
	                          rows[i].run == nullptr);
	/* A running machine keeps the hardware it started with: changed
	 * after it stops, as VirtualBox greys its own out. */
	settings_action->setEnabled(any && !rows[i].added.file.isEmpty() && rows[i].added.name.isEmpty() &&
	                            !held && rows[i].run == nullptr);
	/* An exited machine the core still lists is started again: the core
	 * takes a new run of the id beside it, as `hull run` does. */
	start_action->setEnabled(any && (!rows[i].is_held || rows[i].held.state == QStringLiteral("exited")) &&
	                         rows[i].run == nullptr && !rows[i].added.file.isEmpty());
	stop_action->setEnabled(held);
	kill_action->setEnabled(held);
	pause_action->setEnabled(held && state != QStringLiteral("paused"));
	resume_action->setEnabled(held && state == QStringLiteral("paused"));
	terminal_action->setEnabled(state == QStringLiteral("running"));
	commit_action->setEnabled(state == QStringLiteral("running"));
	medium_action->setEnabled(state == QStringLiteral("running") &&
	                          rows[i].held.engine == QStringLiteral("qemu-pc"));
	screen_action->setEnabled(state == QStringLiteral("running") && !rows[i].held.address.isEmpty());
	keys_action->setEnabled(medium_action->isEnabled());
	screenshot_action->setEnabled(medium_action->isEnabled());
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
	/* Under the home, as ~/..., the way a person names it. */
	{
		const QString home = QDir::homePath() + QLatin1Char('/');

		line("File", r.added.file.startsWith(home) ? QStringLiteral("~/") + r.added.file.mid(home.size())
		                                           : r.added.file);
	}
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
		/* "FILE:LINE:COL: NAME on ENGINE (CLASS): FIELD: VERDICT" as
		 * "LINE:COL  FIELD: VERDICT": the file and the machine are above. */
		static const QRegularExpression field(
		    QStringLiteral("^.*:(\\d+:\\d+): \\S+ on \\S+ \\(\\S+\\): (.*)$"));
		static const QRegularExpression summary(
		    QStringLiteral("^.*: \\S+ on \\S+ \\(\\S+\\): (.*)$"));
		QString explain;

		for (const QString &l : r.explain.split(QLatin1Char('\n'))) {
			QRegularExpressionMatch m;

			if (l.startsWith(QStringLiteral("(machine ")))
				continue;
			if ((m = field.match(l)).hasMatch())
				explain += QStringLiteral("%1 %2\n").arg(m.captured(1), -7).arg(m.captured(2));
			else if ((m = summary.match(l)).hasMatch())
				explain += QStringLiteral("=> %1\n").arg(m.captured(1));
			else
				explain += l + QLatin1Char('\n');
		}
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

	/* Added again after being taken off: no longer a removal to keep. */
	{
		QStringList removed = load_removed();

		if (removed.removeAll(file) > 0)
			save_removed(removed);
	}
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
		                             QStringLiteral("A machine's hull.conf, a compose file or "
		                                            "a Vagrantfile:"),
		                             QLineEdit::Normal,
		                             QDir::currentPath() + QStringLiteral("/hull.conf"), &given);
		if (!given)
			return;
	} else {
		/* From the machines' folder where there is one; a VirtualBox or
		 * VMware machine's own file or appliance is a machine too, which
		 * the CLI runs as the pc class (project.md section 3.13). */
		file = QFileDialog::getOpenFileName(this, QStringLiteral("Add Machine"),
		                                    QDir(machines_home()).exists() ? machines_home()
		                                                                   : QDir::currentPath(),
		                                    QStringLiteral("Machines (hull.conf *.conf compose.yaml "
		                                                   "compose.yml docker-compose.yaml "
		                                                   "docker-compose.yml *.yaml *.yml "
		                                                   "Vagrantfile *.vbox *.vmx *.ovf *.ova);;"
		                                                   "All files (*)"));
	}
	if (file.isEmpty())
		return;
	if (!QFileInfo(file).isFile()) {
		say(QStringLiteral("%1: no such file").arg(file));
		return;
	}
	add_machine(file, QString());
}

/* Import, as VirtualBox's Import Appliance: another hypervisor's machine
 * copied into ~/hull/NAME by `hull import --machine`, its disks converted
 * to qcow2 there, then added. Add runs such a file where it stands;
 * Import gives hull its own copy. The conversion can take minutes, so
 * the CLI runs beside the window and what it says is the status line. */
void main_window::ask_import()
{
	QString file;
	bool    given = false;

	if (on_terminal()) {
		file = QInputDialog::getText(this, QStringLiteral("Import Machine"),
		                             QStringLiteral("A .vbox, .vmx, .ovf or .ova:"),
		                             QLineEdit::Normal, QString(), &given);
		if (!given)
			return;
	} else {
		file = QFileDialog::getOpenFileName(this, QStringLiteral("Import Machine"),
		                                    QDir::homePath(),
		                                    QStringLiteral("Virtual machines (*.vbox *.vmx *.ovf "
		                                                   "*.ova);;All files (*)"));
	}
	if (file.isEmpty())
		return;
	if (!QFileInfo(file).isFile()) {
		say(QStringLiteral("%1: no such file").arg(file));
		return;
	}
	/* The folder named as the machine's file is, which for VirtualBox and
	 * VMware is the machine's own name. */
	const QString name = QInputDialog::getText(this, QStringLiteral("Import Machine"),
	                                           QStringLiteral("Its folder in %1:")
	                                               .arg(machines_home()),
	                                           QLineEdit::Normal,
	                                           QFileInfo(file).completeBaseName(), &given)
	                         .trimmed();

	if (!given || name.isEmpty())
		return;
	if (name.contains(QLatin1Char('/')) || name == QStringLiteral(".") ||
	    name == QStringLiteral("..")) {
		say(QStringLiteral("%1: a folder's name, not a path").arg(name));
		return;
	}
	const QString dir = machines_home() + QLatin1Char('/') + name;

	if (QFileInfo::exists(dir + QStringLiteral("/hull.conf"))) {
		say(QStringLiteral("%1 holds a machine already").arg(dir));
		return;
	}
	QDir().mkpath(machines_home());

	QProcess *import = new QProcess(this);

	import->setStandardInputFile(QProcess::nullDevice());
	import->setProcessChannelMode(QProcess::MergedChannels);
	connect(import, &QProcess::readyRead, this, [this, import]() {
		const QList<QByteArray> lines = import->readAll().trimmed().split('\n');

		if (!lines.isEmpty())
			say(QString::fromUtf8(lines.last()));
		import->setProperty("said", QString::fromUtf8(lines.last()));
	});
	connect(import, &QProcess::finished, this, [this, import, dir, name](int code,
	                                                                     QProcess::ExitStatus how) {
		import->deleteLater();
		if (how != QProcess::NormalExit || code != 0) {
			QMessageBox::warning(this, QStringLiteral("Import Machine"),
			                     QStringLiteral("%1 was not imported: %2")
			                         .arg(name, import->property("said").toString()));
			return;
		}
		add_machine(dir + QStringLiteral("/hull.conf"), QString());
		say(QStringLiteral("imported into %1").arg(dir));
	});
	say(QStringLiteral("importing %1 into %2").arg(file, dir));
	import->start(hull, { QStringLiteral("import"), QStringLiteral("--machine"), file, dir });
}

/* Export Appliance, as VirtualBox's: the machine as an .ova by `hull
 * export --bootable -f FILE [NAME] X.ova` -- a PC's own disks, or a
 * directory's tree made bootable -- for VirtualBox, VMware or Import to
 * read. The CLI says why where a machine cannot be one, a board's say;
 * the copy takes a while for a large disk, so it runs beside the window
 * as Import does. */
void main_window::export_appliance()
{
	const int i = current();

	if (i < 0 || i >= rows.size() || rows[i].added.file.isEmpty())
		return;
	const added_machine machine = rows[i].added;
	QString             base = machine.name;

	if (base.isEmpty())
		base = hostname_in(rows[i].resolved);
	if (base.isEmpty())
		base = QFileInfo(machine.file).absoluteDir().dirName();
	const QString suggested = QDir::homePath() + QLatin1Char('/') + base + QStringLiteral(".ova");
	QString       ova;
	bool          given = false;

	if (on_terminal()) {
		ova = QInputDialog::getText(this, QStringLiteral("Export Appliance"),
		                            QStringLiteral("The .ova to write:"), QLineEdit::Normal,
		                            suggested, &given)
		          .trimmed();
		if (!given)
			return;
	} else {
		ova = QFileDialog::getSaveFileName(this, QStringLiteral("Export Appliance"), suggested,
		                                   QStringLiteral("Appliances (*.ova)"));
	}
	if (ova.isEmpty())
		return;
	if (!ova.endsWith(QStringLiteral(".ova")))
		ova += QStringLiteral(".ova");

	QProcess   *exporting = new QProcess(this);
	QStringList arguments = { QStringLiteral("export"), QStringLiteral("--bootable"),
	                          QStringLiteral("-f"), machine.file };

	if (!machine.name.isEmpty())
		arguments << machine.name;
	arguments << ova;
	exporting->setStandardInputFile(QProcess::nullDevice());
	exporting->setProcessChannelMode(QProcess::MergedChannels);
	connect(exporting, &QProcess::readyRead, this, [this, exporting]() {
		const QList<QByteArray> lines = exporting->readAll().trimmed().split('\n');

		if (!lines.isEmpty() && !lines.last().isEmpty()) {
			say(QString::fromUtf8(lines.last()));
			exporting->setProperty("said", QString::fromUtf8(lines.last()));
		}
	});
	connect(exporting, &QProcess::finished, this, [this, exporting, ova](int code,
	                                                                    QProcess::ExitStatus how) {
		exporting->deleteLater();
		if (how != QProcess::NormalExit || code != 0) {
			QMessageBox::warning(this, QStringLiteral("Export Appliance"),
			                     QStringLiteral("%1 was not written: %2")
			                         .arg(ova, exporting->property("said").toString()));
			return;
		}
		say(QStringLiteral("exported to %1").arg(ova));
	});
	say(QStringLiteral("exporting to %1").arg(ova));
	exporting->start(hull, arguments);
}

/* New: the store's refs offered as sources, `hull images`' first column,
 * then the file the dialog wrote added as any other. */
void main_window::show_images()
{
	images_dialog dialog(hull, this);

	connect(&dialog, &images_dialog::new_machine_from, this, [this, &dialog](const QString &ref) {
		dialog.accept();
		QTimer::singleShot(0, this, [this, ref]() { ask_new(ref); });
	});
	dialog.resize(760, 480);
	dialog.exec();
}

void main_window::ask_new(const QString &source)
{
	QStringList images;
	QProcess    list;

	list.setStandardInputFile(QProcess::nullDevice());
	list.start(hull, { QStringLiteral("images") });
	if (list.waitForFinished(10000) && list.exitCode() == 0)
		for (const QString &line :
		     QString::fromUtf8(list.readAllStandardOutput()).split(QLatin1Char('\n')).mid(1))
			if (!line.trimmed().isEmpty())
				images << line.section(QLatin1Char(' '), 0, 0);

	new_machine_dialog dialog(images, source, this);

	if (dialog.exec() == QDialog::Accepted) {
		say(QStringLiteral("wrote %1").arg(dialog.written()));
		add_machine(dialog.written(), QString());
	}
}

/* Clone, VirtualBox's full clone: the machine's folder in ~/hull copied
 * whole, its disks with it -- reflinked where the filesystem can, so a
 * clone costs nothing until it writes -- its hostname the clone's name,
 * so the two have different ids and both can run, then added. A copy
 * runs beside the window, a disk taking minutes. */
void main_window::clone_machine()
{
	const int i = current();

	if (i < 0 || i >= rows.size() || !in_own_folder(rows[i].added.file))
		return;
	const QString from = QFileInfo(rows[i].added.file).absolutePath();
	bool          given = false;
	const QString name = QInputDialog::getText(this, QStringLiteral("Clone Machine"),
	                                           QStringLiteral("The clone's name, its folder "
	                                                          "in %1:").arg(machines_home()),
	                                           QLineEdit::Normal,
	                                           QFileInfo(from).fileName() +
	                                               QStringLiteral("-clone"), &given)
	                         .trimmed();

	if (!given || name.isEmpty())
		return;
	if (name.contains(QLatin1Char('/')) || name == QStringLiteral(".") ||
	    name == QStringLiteral("..")) {
		say(QStringLiteral("%1: a folder's name, not a path").arg(name));
		return;
	}
	const QString to = machines_home() + QLatin1Char('/') + name;
	QFile         original(rows[i].added.file);
	QString       text;

	if (QFileInfo::exists(to)) {
		say(QStringLiteral("%1 is there already").arg(to));
		return;
	}
	if (!original.open(QIODevice::ReadOnly)) {
		say(QStringLiteral("%1 cannot be read").arg(original.fileName()));
		return;
	}
	text = with_hostname(QString::fromUtf8(original.readAll()), name);
	if (text.isEmpty()) {
		say(QStringLiteral("%1 names several hostnames, so its clone's cannot be chosen; "
		                   "copy it by hand").arg(original.fileName()));
		return;
	}

	QProcess *copy = new QProcess(this);

	copy->setStandardInputFile(QProcess::nullDevice());
	copy->setProcessChannelMode(QProcess::MergedChannels);
	connect(copy, &QProcess::finished, this, [this, copy, to, text](int code,
	                                                               QProcess::ExitStatus how) {
		const QString said = QString::fromUtf8(copy->readAll()).trimmed();
		QFile         conf(to + QStringLiteral("/hull.conf"));

		copy->deleteLater();
		if (how != QProcess::NormalExit || code != 0) {
			QMessageBox::warning(this, QStringLiteral("Clone Machine"),
			                     QStringLiteral("The copy into %1 failed: %2").arg(to, said));
			return;
		}
		if (!conf.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
		    conf.write(text.toUtf8()) < 0 || !conf.flush()) {
			say(QStringLiteral("%1 could not be written").arg(conf.fileName()));
			return;
		}
		conf.close();
		add_machine(conf.fileName(), QString());
		say(QStringLiteral("cloned into %1").arg(to));
	});
	say(QStringLiteral("cloning %1 into %2").arg(from, to));
	/* --no-target-directory: `to` is the copy, never a folder inside it. */
	copy->start(QStringLiteral("cp"), { QStringLiteral("-a"), QStringLiteral("--reflink=auto"),
	                                    QStringLiteral("--no-target-directory"), from, to });
}

/* Settings: the hardware in a form, written into the file in place, and
 * the machine planned again, as after Edit. */
void main_window::edit_settings()
{
	const int i = current();

	if (i < 0 || i >= rows.size() || rows[i].added.file.isEmpty())
		return;
	const bool      pc = rows[i].resolved.contains(QStringLiteral("(backend pc)")) ||
	                     rows[i].resolved.contains(QStringLiteral("(backend qemu-pc)"));
	settings_dialog dialog(rows[i].added.file, pc, this);

	if (!dialog.usable()) {
		say(dialog.problem());
		return;
	}
	if (dialog.exec() == QDialog::Accepted) {
		plan_of(i);
		refresh();
	}
}

void main_window::remove_machine()
{
	const int i = current();

	if (i < 0 || i >= rows.size() || rows[i].added.file.isEmpty())
		return;
	const QString file = rows[i].added.file;
	const QString folder = QFileInfo(file).absolutePath();
	/* A machine in a folder of its own in the home, as New and Import
	 * make: VirtualBox's question, its files kept or deleted. Deleted is
	 * to the desktop's trash, which gives them back. */
	const bool own_folder = in_own_folder(file);
	bool       deleted = false;

	if (own_folder) {
		QMessageBox ask(QMessageBox::Question, QStringLiteral("Remove Machine"),
		                QStringLiteral("Remove %1 from the list, or delete its folder, %2, "
		                               "with its disks?")
		                    .arg(rows[i].id, folder),
		                QMessageBox::Cancel, this);
		QPushButton *remove_only = ask.addButton(QStringLiteral("Remove Only"),
		                                         QMessageBox::AcceptRole);
		QPushButton *delete_all = ask.addButton(QStringLiteral("Delete All Files"),
		                                        QMessageBox::DestructiveRole);

		ask.setDefaultButton(remove_only);
		ask.exec();
		if (ask.clickedButton() == delete_all) {
			if (rows[i].run != nullptr || rows[i].is_held) {
				say(QStringLiteral("%1 is running: stop it before deleting its files")
				        .arg(rows[i].id));
				return;
			}
			if (!QFile::moveToTrash(folder)) {
				say(QStringLiteral("%1 could not be moved to the trash").arg(folder));
				return;
			}
			deleted = true;
		} else if (ask.clickedButton() != remove_only) {
			return;
		}
	}
	QList<added_machine> added = load_added();

	added.removeAll(rows[i].added);
	save_added(added);
	/* Kept, it would be found again at the next start: the choice is
	 * remembered, as the file's own removal does by itself. */
	if (own_folder && !deleted) {
		QStringList removed = load_removed();

		if (!removed.contains(file))
			save_removed(removed << file);
	}
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
		if (at < 0 || (rows[at].is_held && rows[at].held.state != QStringLiteral("exited")) ||
		    rows[at].run != nullptr)
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

/* How long a stop may wait on its guest before the window says so. */
static const int stop_patience_ms = 3000;

void main_window::stop()
{
	const int i = current();

	if (i < 0 || i >= rows.size() || !rows[i].is_held)
		return;
	const QString id = rows[i].id;
	/* -1: the machine's own grace, as `hull stop` without --timeout. */
	QPointer<core_request> request =
	    core_request::send(this, "(4:stop", id, QByteArray("(10:timeout_ms2:-1)"), 0);
	/* Stop is the guest's to act on -- a PC's power button, a workload's
	 * TERM -- and its grace can be minutes, so a stop still waiting after
	 * a moment says so, and offers Power Off, which does not wait; the
	 * question goes when the machine does. */
	say(QStringLiteral("stopping %1").arg(id));
	QTimer::singleShot(stop_patience_ms, this, [this, id, request]() {
		if (request.isNull())
			return;
		QMessageBox *box = new QMessageBox(QMessageBox::Question, QStringLiteral("Stop"),
		                                   QStringLiteral("%1 is still stopping: its guest has "
		                                                  "not acted on it yet, and is given "
		                                                  "until its grace runs out. Power it "
		                                                  "off now?").arg(id),
		                                   QMessageBox::NoButton, this);
		QPushButton *off = box->addButton(QStringLiteral("Power Off"), QMessageBox::DestructiveRole);

		box->addButton(QStringLiteral("Keep Waiting"), QMessageBox::RejectRole);
		box->setAttribute(Qt::WA_DeleteOnClose);
		box->setDefaultButton(off);
		say(QStringLiteral("%1: waiting for its guest to stop").arg(id));
		connect(box, &QMessageBox::buttonClicked, this, [this, id, off](QAbstractButton *button) {
			if (button == off)
				power_off(id);
		});
		connect(request.data(), &QObject::destroyed, box, &QWidget::close);
		box->open();
	});
	connect(request.data(), &core_request::finished, this,
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
	power_off(rows[i].id);
}

/* KILL, the plug pulled: the workload, or a PC's emulator, at once. */
void main_window::power_off(const QString &id)
{
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

#ifdef HULL_QTTY
/* `argv` run on the terminal qtty draws on, handed over to it until it
 * exits, as a shell's job in the foreground has it. */
static void on_this_terminal(const QStringList &command)
{
	Qtty::shell_out([&command]() {
		QByteArrayList bytes;
		QList<char *>  argv;
		pid_t          child;

		for (const QString &arg : command)
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
}
#endif

/*
 * The selected machine's tree, as it is while it runs, into the store as
 * REF: `hull commit ID REF`, VirtualBox's Take Snapshot in hull's terms.
 * The engine writes it from inside the machine, so a write in an
 * overlay's memory or a vm's guest is in it and a volume is not
 * (section 3). The ref offered is the machine's name and the time, so
 * a second commit does not move the first's ref unless asked to.
 */
void main_window::commit_machine()
{
	const int i = current();

	if (i < 0 || i >= rows.size() || !rows[i].is_held)
		return;
	const QString id = rows[i].id;
	bool          ok = false;
	const QString ref = QInputDialog::getText(
	    this, QStringLiteral("Commit to Image"),
	    QStringLiteral("%1's tree, as it is now, stored as:").arg(id), QLineEdit::Normal,
	    id + QStringLiteral("/") +
	        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")),
	    &ok)
	                        .trimmed();

	if (!ok || ref.isEmpty())
		return;
	QProcess *process = new QProcess(this);

	process->setProcessChannelMode(QProcess::MergedChannels);
	process->setStandardInputFile(QProcess::nullDevice());
	say(QStringLiteral("committing %1 as %2...").arg(id, ref));
	connect(process, &QProcess::finished, this, [this, process, id, ref](int code) {
		const QString said = QString::fromUtf8(process->readAll()).trimmed();

		process->deleteLater();
		say(code == 0 ? QStringLiteral("%1 committed as %2").arg(id, ref)
		              : said.isEmpty() ? QStringLiteral("hull commit exited %1").arg(code) : said);
	});
	connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError e) {
		if (e != QProcess::FailedToStart)
			return;
		say(process->errorString());
		process->deleteLater();
	});
	process->start(hull, { QStringLiteral("commit"), id, ref });
}

/*
 * A running pc machine's floppy or CD swapped, as VirtualBox's Devices
 * menu does it: `hull media change ID DRIVE [IMAGE]`, an empty image
 * taking the medium out. The drive is one its (media ...) gave it,
 * floppy or cdrom and floppy2 or cdrom2 for the next; the CLI says
 * which it lacks. A path typed rather than a file dialog's, on the
 * desktop as on a terminal, since an empty answer is how a medium is
 * taken out and a file dialog has no such answer; a relative one is the
 * window's directory's.
 */
void main_window::change_medium()
{
	const int i = current();

	if (i < 0 || i >= rows.size() || !rows[i].is_held)
		return;
	const QString id = rows[i].id;
	bool          ok = false;
	const QString drive = QInputDialog::getItem(
	    this, QStringLiteral("Change Medium"), QStringLiteral("%1's drive:").arg(id),
	    { QStringLiteral("floppy"), QStringLiteral("cdrom"), QStringLiteral("floppy2"),
	      QStringLiteral("cdrom2") },
	    0, true, &ok);

	if (!ok || drive.trimmed().isEmpty())
		return;
	const QString image = QInputDialog::getText(
	    this, QStringLiteral("Change Medium"),
	    QStringLiteral("The image for %1, or nothing to take it out:").arg(drive.trimmed()),
	    QLineEdit::Normal, QString(), &ok)
	                          .trimmed();

	if (!ok)
		return;
	QStringList arguments = { QStringLiteral("media"), QStringLiteral("change"), id,
		                      drive.trimmed() };
	/* A relative path is the window's own directory's, made whole. */
	const QString path = image.isEmpty() ? QString() : QFileInfo(image).absoluteFilePath();
	const bool    out = path.isEmpty();

	if (!out)
		arguments << path;
	QProcess *process = new QProcess(this);

	process->setProcessChannelMode(QProcess::MergedChannels);
	process->setStandardInputFile(QProcess::nullDevice());
	connect(process, &QProcess::finished, this,
	        [this, process, id, drive, path, out](int code) {
		        const QString said = QString::fromUtf8(process->readAll()).trimmed();

		        process->deleteLater();
		        say(code != 0 ? (said.isEmpty() ? QStringLiteral("hull media change exited %1")
		                                                  .arg(code)
		                                        : said)
		            : out ? QStringLiteral("%1's %2 taken out").arg(id, drive.trimmed())
		                  : QStringLiteral("%1's %2 holds %3").arg(id, drive.trimmed(), path));
	        });
	connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError e) {
		if (e != QProcess::FailedToStart)
			return;
		say(process->errorString());
		process->deleteLater();
	});
	process->start(hull, arguments);
}

void main_window::insert_ctrl_alt_del()
{
	const int i = current();

	if (i < 0 || i >= rows.size() || !rows[i].is_held)
		return;
	run_for({ QStringLiteral("key"), rows[i].id, QStringLiteral("ctrl-alt-delete") },
	        QStringLiteral("Ctrl-Alt-Del pressed on %1").arg(rows[i].id));
}

void main_window::take_screenshot()
{
	const int i = current();

	if (i < 0 || i >= rows.size() || !rows[i].is_held)
		return;
	const QString id = rows[i].id;
	bool          ok = false;
	const QString file = QInputDialog::getText(
	    this, QStringLiteral("Take Screenshot"), QStringLiteral("%1's screen, as a PNG, to:").arg(id),
	    QLineEdit::Normal, QStringLiteral("%1-screen.png").arg(id), &ok)
	                         .trimmed();

	if (!ok || file.isEmpty())
		return;
	/* A relative path is the window's own directory's, as for a medium. */
	const QString path = QFileInfo(file).absoluteFilePath();

	run_for({ QStringLiteral("screenshot"), id, path },
	        QStringLiteral("%1's screen is in %2").arg(id, path));
}

/* `hull ARGUMENTS...`, its word or `done` on the status line. */
void main_window::run_for(const QStringList &arguments, const QString &done)
{
	QProcess *process = new QProcess(this);

	process->setProcessChannelMode(QProcess::MergedChannels);
	process->setStandardInputFile(QProcess::nullDevice());
	connect(process, &QProcess::finished, this, [this, process, arguments, done](int code) {
		const QString said = QString::fromUtf8(process->readAll()).trimmed();

		process->deleteLater();
		say(code != 0 ? (said.isEmpty() ? QStringLiteral("hull %1 exited %2")
		                                      .arg(arguments.first())
		                                      .arg(code)
		                                : said)
		              : done);
	});
	connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError e) {
		if (e != QProcess::FailedToStart)
			return;
		say(process->errorString());
		process->deleteLater();
	});
	process->start(hull, arguments);
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
		on_this_terminal(shell);
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
 * desktop, a vm's is shown here first, by qemu's D-Bus display where the
 * machine is this host's and has one (section 5.2), its link at
 * hull/display/ADDRESS; otherwise the first VNC client found is started
 * on it. Under qtty there is no screen to show one on, so the address is
 * said. A machine with no screen says so rather than a client that fails
 * to connect.
 */
void main_window::open_screen()
{
	const int i = current();

	if (i < 0 || i >= rows.size() || !rows[i].is_held || rows[i].held.address.isEmpty())
		return;
	const QString id = rows[i].id;
	const QString address = rows[i].held.address;
	const QString at = address + QStringLiteral(":5900");

	/* Here first: (display local) has no VNC at all. */
	const QString runtime = QString::fromLocal8Bit(qgetenv("XDG_RUNTIME_DIR"));
	const QString local = runtime + QStringLiteral("/hull/display/") + address;
	if (!on_terminal() && !runtime.isEmpty() && QFileInfo::exists(local)) {
		auto   *window = new screen_window(id, local);
		QString why;

		if (window->open(&why)) {
			window->show();
			say(QStringLiteral("%1's screen, here").arg(id));
			return;
		}
		delete window;
		say(QStringLiteral("%1's screen is not shown here (%2); by VNC instead").arg(id, why));
	}
	if (!answers(address, 5900)) {
		say(QStringLiteral("%1 shows no screen at %2: a machine gives one with (display vnc), "
		                   "or with (display local) here only")
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

/*
 * The Network tab: hull's own wires (section 8, Appendix D) as the
 * selected machine's plan names them, each port's counters as `hull
 * segment NAME` reads them, and what `hull segment NAME` does to a link
 * or the wire: impair a way, tamper with the frames crossing it, inject
 * frames of the switch's own, or capture a way to a pcap file. The CLI
 * is asked, as the counters, netem's names and the faults are its words,
 * and parsing them a second time here would drift.
 */

/* The counters shown, as `hull segment NAME` names them: what a link
 * carries, then what an impairment, a fault and an injector did. */
static const char *const counted[] = { "frames", "bytes", "passed", "lost",
	                                   "matched", "tampered", "replayed", "injected" };

/* What each verb's words are, shown in the field until it is typed in. */
static QString words_hint(const QString &verb)
{
	if (verb == QLatin1String("tamper"))
		return QStringLiteral("match=0:0800 drop=1 | rewrite=1 set=4:ff | flip=0.1 | "
		                      "replay=1 copies=3 every=100");
	if (verb == QLatin1String("inject"))
		return QStringLiteral("frame=HEX | size=60 [count=N] [every=MS] [seed=N]");
	if (verb == QLatin1String("capture"))
		return QStringLiteral("FILE.pcap [frames=N]");
	return QStringLiteral("loss=10% delay=50ms rate=1mbit");
}

QWidget *main_window::build_network()
{
	network = new QWidget;
	ports = new QTreeWidget;
	QStringList headers = { QStringLiteral("Segment"), QStringLiteral("Machine"),
		                    QStringLiteral("Way") };

	for (const char *count : counted) {
		QString header = QString::fromLatin1(count);

		header[0] = header[0].toUpper();
		headers << header;
	}
	ports->setHeaderLabels(headers);
	ports->setRootIsDecorated(false);
	network_note = new QLabel;
	network_note->setWordWrap(true);
	segment_verb = new QComboBox;
	segment_verb->addItems({ QStringLiteral("impair"), QStringLiteral("tamper"),
	                         QStringLiteral("inject"), QStringLiteral("capture") });
	segment_verb->setToolTip(QStringLiteral("impair: netem's faults on a way; tamper: frames "
	                                        "matched and dropped, rewritten, flipped or "
	                                        "replayed; inject: the switch's own frames; "
	                                        "capture: a way's frames to a pcap file"));
	segment_target = new QComboBox;
	segment_target->addItem(QStringLiteral("this machine"), QString());
	segment_target->addItem(QStringLiteral("all ports"), QStringLiteral("all"));
	segment_way = new QComboBox;
	segment_way->addItems({ QStringLiteral("both"), QStringLiteral("in"), QStringLiteral("out") });
	segment_words = new QLineEdit;
	segment_words->setPlaceholderText(words_hint(QStringLiteral("impair")));
	connect(segment_verb, &QComboBox::currentTextChanged, this, [this](const QString &verb) {
		/* One verb's words are never another's. */
		segment_words->clear();
		segment_words->setPlaceholderText(words_hint(verb));
		/* An injector has no way, and a capture is one way. */
		segment_way->setEnabled(verb != QLatin1String("inject"));
		if (verb == QLatin1String("capture") && segment_way->currentText() == QLatin1String("both"))
			segment_way->setCurrentText(QStringLiteral("out"));
	});
	QPushButton *apply = new QPushButton(QStringLiteral("&Apply"));
	QPushButton *clear = new QPushButton(QStringLiteral("C&lear"));
	QPushButton *down = new QPushButton(QStringLiteral("Link Do&wn"));

	apply->setToolTip(QStringLiteral("hull segment NAME VERB TARGET [WAY] WORDS"));
	clear->setToolTip(QStringLiteral("The verb undone: an impairment or fault cleared, the "
	                                 "target's injectors removed, the capture stopped"));
	connect(apply, &QPushButton::clicked, this, [this]() {
		segment_act(segment_verb->currentText(), QProcess::splitCommand(segment_words->text()));
	});
	connect(clear, &QPushButton::clicked, this, [this]() {
		const QString verb = segment_verb->currentText();

		segment_act(verb, verb == QLatin1String("inject")    ? QStringList{ QStringLiteral("count=0") }
		                  : verb == QLatin1String("capture") ? QStringList{ QStringLiteral("stop") }
		                                                     : QStringList());
	});
	connect(down, &QPushButton::clicked, this,
	        [this]() { segment_act(QStringLiteral("impair"), { QStringLiteral("down") }); });

	/* Labels with buddies, so each control has an Alt+letter on a
	 * terminal as well, none of them the menus', the tabs' or a
	 * button's. Two rows, so the tab is no wider than an 80-column
	 * terminal's window allows. */
	QHBoxLayout *choices = new QHBoxLayout;
	QHBoxLayout *controls = new QHBoxLayout;
	const struct {
		const char  *label;
		QWidget     *buddy;
		QHBoxLayout *row;
	} fields[] = {
		{ "Ac&tion:", segment_verb, choices },
		{ "&For:", segment_target, choices },
		{ "Wa&y:", segment_way, choices },
		{ "Wo&rds:", segment_words, controls },
	};

	for (const auto &field : fields) {
		QLabel *label = new QLabel(QString::fromLatin1(field.label));

		label->setBuddy(field.buddy);
		field.row->addWidget(label);
		field.row->addWidget(field.buddy, field.buddy == segment_words ? 1 : 0);
	}
	choices->addStretch(1);
	controls->addWidget(apply);
	controls->addWidget(clear);
	controls->addWidget(down);
	QVBoxLayout *layout = new QVBoxLayout(network);
	layout->addWidget(ports, 1);
	layout->addWidget(network_note);
	layout->addLayout(choices);
	layout->addLayout(controls);
	return network;
}

/* Each segment of the selected machine, its ports read again, while the
 * tab is the one shown. A machine started elsewhere has no plan here, so
 * its segments are not known. */
void main_window::ask_ports()
{
	const int i = current();

	if (tabs->currentWidget() != network || ports_pending > 0)
		return;
	if (i < 0 || i >= rows.size()) {
		ports->clear();
		network_note->setText(QString());
		return;
	}
	const QStringList segments = segments_in(rows[i].resolved);

	if (segments.isEmpty()) {
		ports->clear();
		network_note->setText(rows[i].resolved.isEmpty()
		                          ? QStringLiteral("Its plan is not known here: add its hull.conf "
		                                           "to see its segments.")
		                          : QStringLiteral("On no segment of hull's: (segments ...) puts "
		                                           "a machine on a wire of its own."));
		return;
	}
	network_note->setText(rows[i].is_held ? QString()
	                                      : QStringLiteral("Not running: the counters are "
	                                                       "a running machine's."));
	if (!rows[i].is_held) {
		ports->clear();
		return;
	}
	ports->clear();
	for (const QString &segment : segments) {
		QProcess *read = new QProcess(this);

		ports_pending++;
		read->setStandardInputFile(QProcess::nullDevice());
		connect(read, &QProcess::finished, this, [this, read, segment]() {
			static const QRegularExpression line(QStringLiteral("^(\\S+)\\s+(in|out)\\s+(.*)$"));

			ports_pending--;
			read->deleteLater();
			for (const QString &text : QString::fromUtf8(read->readAllStandardOutput())
			                               .split(QLatin1Char('\n'))) {
				const QRegularExpressionMatch m = line.match(text);
				QHash<QString, QString>       counts;

				if (!m.hasMatch())
					continue;
				for (const QString &pair : m.captured(3).split(QLatin1Char(' '), Qt::SkipEmptyParts))
					counts.insert(pair.section(QLatin1Char('='), 0, 0),
					              pair.section(QLatin1Char('='), 1));
				QStringList cells = { segment, m.captured(1), m.captured(2) };

				for (const char *count : counted)
					cells << counts.value(QString::fromLatin1(count));
				ports->addTopLevelItem(new QTreeWidgetItem(cells));
			}
			for (int c = 0; c < ports->columnCount(); c++)
				ports->resizeColumnToContents(c);
		});
		connect(read, &QProcess::errorOccurred, this, [this, read](QProcess::ProcessError e) {
			if (e != QProcess::FailedToStart)
				return;
			ports_pending--;
			read->deleteLater();
		});
		read->start(hull, { QStringLiteral("segment"), segment });
	}
}

/* `hull segment NAME VERB ...` on the segment of the selected port, else
 * the selected machine's first, aimed at that machine or at every port:
 * impair and tamper with a way (both being each in turn, as the CLI does
 * it), inject with none, capture with one, and `capture stop` with no
 * target. What the CLI says is the status line's. */
void main_window::segment_act(const QString &verb, const QStringList &words)
{
	const int i = current();

	if (i < 0 || i >= rows.size() || !rows[i].is_held)
		return;
	const QStringList segments = segments_in(rows[i].resolved);
	QString           segment = segments.value(0);

	if (ports->currentItem() != nullptr)
		segment = ports->currentItem()->text(0);
	if (segment.isEmpty()) {
		say(QStringLiteral("%1 is on no segment known here").arg(rows[i].id));
		return;
	}
	const QString id = rows[i].id;
	const QString target =
	    segment_target->currentData().toString().isEmpty() ? id : QStringLiteral("all");
	const bool    stopping = verb == QLatin1String("capture") && words == QStringList{ QStringLiteral("stop") };
	QStringList   arguments = { QStringLiteral("segment"), segment, verb };

	if (verb == QLatin1String("capture") && !stopping) {
		if (segment_way->currentText() == QLatin1String("both")) {
			say(QStringLiteral("a capture is one way: in or out"));
			return;
		}
		if (words.isEmpty()) {
			say(QStringLiteral("a capture names its file: FILE.pcap [frames=N]"));
			return;
		}
	}
	if (verb == QLatin1String("inject") && words.isEmpty()) {
		say(QStringLiteral("an injection is frame=HEX or size=N"));
		return;
	}
	if (!stopping)
		arguments << target;
	if (verb != QLatin1String("inject") && !stopping)
		arguments << segment_way->currentText();
	arguments << words;

	QProcess *process = new QProcess(this);

	process->setProcessChannelMode(QProcess::MergedChannels);
	process->setStandardInputFile(QProcess::nullDevice());
	connect(process, &QProcess::finished, this, [this, process, verb, target, segment](int code) {
		const QString said = QString::fromUtf8(process->readAll()).trimmed();

		process->deleteLater();
		say(code == 0 ? QStringLiteral("%1 on %2: %3 done").arg(target, segment, verb)
		              : said.isEmpty() ? QStringLiteral("hull segment exited %1").arg(code) : said);
		ask_ports();
	});
	process->start(hull, arguments);
}

/*
 * The selected machine's hull.conf in an editor: under qtty $VISUAL, else
 * $EDITOR, else vi, on the window's own terminal until it exits; on the
 * desktop whatever the desktop opens a text file with. The file is
 * planned again after, at once under qtty, and on the desktop at the
 * next Refresh, there being no end of an editor's window to wait for.
 */
void main_window::edit_spec()
{
	const int i = current();

	if (i < 0 || i >= rows.size() || rows[i].added.file.isEmpty())
		return;
	const QString file = rows[i].added.file;

	if (on_terminal()) {
#ifdef HULL_QTTY
		QString editor = qEnvironmentVariable("VISUAL");

		if (editor.isEmpty())
			editor = qEnvironmentVariable("EDITOR");
		if (editor.isEmpty())
			editor = QStringLiteral("vi");
		/* As a shell reads $EDITOR: it may carry arguments. */
		const QStringList shell = { QStringLiteral("/bin/sh"), QStringLiteral("-c"),
			                        editor + QStringLiteral(" \"$0\""), file };

		on_this_terminal(shell);
		for (int k = 0; k < rows.size(); k++)
			if (rows[k].added.file == file)
				plan_of(k);
#endif
		return;
	}
	if (!QDesktopServices::openUrl(QUrl::fromLocalFile(file)))
		say(QStringLiteral("nothing opens %1 here: edit it, then Refresh").arg(file));
	else
		say(QStringLiteral("%1 opened: Refresh plans it again").arg(file));
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
