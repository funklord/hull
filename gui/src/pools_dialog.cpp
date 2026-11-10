/*
 * pools_dialog.cpp -- see pools_dialog.h.
 */

#include "pools_dialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

/* A JSON value as a cell: a string as it is, a number as one, null as "-". */
static QString cell(const QJsonValue &value)
{
	if (value.isString())
		return value.toString();
	if (value.isDouble())
		return QString::number(value.toDouble(), 'f', 0);
	return QStringLiteral("-");
}

pools_dialog::pools_dialog(const QString &hull_program, QWidget *parent)
    : QDialog(parent), hull(hull_program)
{
	setWindowTitle(QStringLiteral("Pools and Leases"));
	pools = new QTreeWidget;
	pools->setHeaderLabels({ QStringLiteral("Pool"), QStringLiteral("Capacity"),
	                         QStringLiteral("Held"), QStringLiteral("Waiting") });
	pools->setRootIsDecorated(false);
	leases = new QTreeWidget;
	leases->setHeaderLabels({ QStringLiteral("Lease"), QStringLiteral("Pool"),
	                          QStringLiteral("Count"), QStringLiteral("Machines"),
	                          QStringLiteral("Ends"), QStringLiteral("Holder") });
	leases->setRootIsDecorated(false);
	boards = new QTreeWidget;
	boards->setHeaderLabels({ QStringLiteral("Board"), QStringLiteral("Address"),
	                          QStringLiteral("State") });
	boards->setRootIsDecorated(false);
	connect(pools, &QTreeWidget::currentItemChanged, this, &pools_dialog::reload_boards);
	connect(leases, &QTreeWidget::currentItemChanged, this, &pools_dialog::update_buttons);
	connect(boards, &QTreeWidget::currentItemChanged, this, &pools_dialog::update_buttons);

	count = new QSpinBox;
	count->setRange(1, 1000);
	count->setPrefix(QStringLiteral("count "));
	span = new QLineEdit;
	span->setPlaceholderText(QStringLiteral("for: 30m, 2h; empty, no end"));
	reserve_button = new QPushButton(QStringLiteral("&Reserve"));
	reserve_button->setToolTip(QStringLiteral("hull reserve POOL --count N [--for D] --no-wait: "
	                                          "refused at once where the pool has no room"));
	release_button = new QPushButton(QStringLiteral("Re&lease"));
	release_button->setToolTip(QStringLiteral("hull release ID: its machines stopped first"));
	connect(reserve_button, &QPushButton::clicked, this, [this]() {
		QStringList arguments = { QStringLiteral("reserve"), pools->currentItem()->text(0),
			                      QStringLiteral("--count"), QString::number(count->value()) };

		if (!span->text().trimmed().isEmpty())
			arguments << QStringLiteral("--for") << span->text().trimmed();
		act(arguments << QStringLiteral("--no-wait"));
	});
	connect(release_button, &QPushButton::clicked, this, [this]() {
		act({ QStringLiteral("release"), leases->currentItem()->text(0) });
	});
	fix_button = new QPushButton(QStringLiteral("&Fix"));
	fix_button->setToolTip(QStringLiteral("hull board fix NAME: a board marked broken here "
	                                      "offered again; a broker's marks are its own"));
	connect(fix_button, &QPushButton::clicked, this, [this]() {
		act({ QStringLiteral("board"), QStringLiteral("fix"), boards->currentItem()->text(0) });
	});
	said = new QLabel;
	said->setWordWrap(true);

	QHBoxLayout *controls = new QHBoxLayout;
	controls->addWidget(count);
	controls->addWidget(span, 1);
	controls->addWidget(reserve_button);
	controls->addWidget(release_button);
	controls->addWidget(fix_button);
	QDialogButtonBox *close = new QDialogButtonBox(QDialogButtonBox::Close);
	connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(new QLabel(QStringLiteral("Pools, from pools.conf:")));
	layout->addWidget(pools, 1);
	layout->addWidget(new QLabel(QStringLiteral("Leases the core holds:")));
	layout->addWidget(leases, 1);
	layout->addWidget(new QLabel(QStringLiteral("Boards of the pool chosen:")));
	layout->addWidget(boards, 1);
	layout->addLayout(controls);
	layout->addWidget(said);
	layout->addWidget(close);

	poll = new QTimer(this);
	connect(poll, &QTimer::timeout, this, &pools_dialog::reload);
	poll->start(2000);
	reload();
}

/* Both lists again, each kept on the row it was on by its first cell. */
void pools_dialog::reload()
{
	struct list {
		QTreeWidget *tree;
		const char  *noun;
		QStringList  keys;
	};
	const QStringList pool_keys =
	    QStringLiteral("pool capacity held waiting").split(QLatin1Char(' '));
	const QStringList lease_keys =
	    QStringLiteral("id pool count machines ends holder").split(QLatin1Char(' '));
	const list lists[] = {
		{ pools, "pool", pool_keys },
		{ leases, "lease", lease_keys },
	};

	for (const list &l : lists) {
		QProcess      ls;
		const QString keep = l.tree->currentItem() != nullptr ? l.tree->currentItem()->text(0)
		                                                      : QString();

		ls.setStandardInputFile(QProcess::nullDevice());
		ls.start(hull, { QString::fromLatin1(l.noun), QStringLiteral("ls"),
		                 QStringLiteral("--json") });
		if (!ls.waitForFinished(10000) || ls.exitCode() != 0) {
			said->setText(QStringLiteral("hull %1 ls: %2")
			                  .arg(QString::fromLatin1(l.noun),
			                       QString::fromUtf8(ls.readAllStandardError()).trimmed()));
			continue;
		}
		const QJsonArray rows = QJsonDocument::fromJson(ls.readAllStandardOutput()).array();
		/* The boards are read once below, not for each row this puts
		 * in and takes out. */
		const QSignalBlocker quiet(l.tree);

		l.tree->clear();
		for (const QJsonValue &row : rows) {
			QStringList cells;

			for (const QString &key : l.keys)
				cells << cell(row.toObject().value(key));
			QTreeWidgetItem *item = new QTreeWidgetItem(cells);

			l.tree->addTopLevelItem(item);
			if (cells.first() == keep)
				l.tree->setCurrentItem(item);
		}
		if (l.tree->currentItem() == nullptr && l.tree->topLevelItemCount() > 0)
			l.tree->setCurrentItem(l.tree->topLevelItem(0));
		for (int c = 0; c < l.tree->columnCount(); c++)
			l.tree->resizeColumnToContents(c);
	}
	reload_boards();
}

/* The chosen pool's boards, as `hull board ls POOL` prints them, kept on
 * the row they were on. A pool of slots has none, and says so on stderr,
 * which is not shown: an empty list says it. */
void pools_dialog::reload_boards()
{
	const QString keep =
	    boards->currentItem() != nullptr ? boards->currentItem()->text(0) : QString();

	boards->clear();
	if (pools->currentItem() != nullptr) {
		QProcess ls;

		ls.setStandardInputFile(QProcess::nullDevice());
		ls.start(hull, { QStringLiteral("board"), QStringLiteral("ls"),
		                 pools->currentItem()->text(0) });
		if (ls.waitForFinished(10000) && ls.exitCode() == 0) {
			for (const QString &line : QString::fromUtf8(ls.readAllStandardOutput())
			                               .split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
				const QStringList cells = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);

				if (cells.size() != 3)
					continue;
				QTreeWidgetItem *item = new QTreeWidgetItem(cells);

				boards->addTopLevelItem(item);
				if (cells.first() == keep)
					boards->setCurrentItem(item);
			}
		}
	}
	if (boards->currentItem() == nullptr && boards->topLevelItemCount() > 0)
		boards->setCurrentItem(boards->topLevelItem(0));
	for (int c = 0; c < boards->columnCount(); c++)
		boards->resizeColumnToContents(c);
	update_buttons();
}

void pools_dialog::update_buttons()
{
	reserve_button->setEnabled(!busy && pools->currentItem() != nullptr);
	release_button->setEnabled(!busy && leases->currentItem() != nullptr);
	fix_button->setEnabled(!busy && boards->currentItem() != nullptr &&
	                       boards->currentItem()->text(2) == QLatin1String("broken"));
}

/* reserve or release, what it said shown, the lists read again. */
void pools_dialog::act(const QStringList &arguments)
{
	QProcess *process = new QProcess(this);

	busy = true;
	update_buttons();
	process->setProcessChannelMode(QProcess::MergedChannels);
	process->setStandardInputFile(QProcess::nullDevice());
	connect(process, &QProcess::finished, this, [this, process, arguments](int code) {
		const QString output = QString::fromUtf8(process->readAll()).trimmed();

		process->deleteLater();
		busy = false;
		said->setText(QStringLiteral("hull %1: %2")
		                  .arg(arguments.first(),
		                       output.isEmpty() ? QStringLiteral("exited %1").arg(code) : output));
		reload();
	});
	connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError e) {
		if (e != QProcess::FailedToStart)
			return;
		said->setText(process->errorString());
		process->deleteLater();
		busy = false;
		update_buttons();
	});
	process->start(hull, arguments);
}
