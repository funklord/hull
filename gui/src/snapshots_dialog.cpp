/*
 * snapshots_dialog.cpp -- see snapshots_dialog.h.
 */

#include "snapshots_dialog.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QTreeWidget>
#include <QVBoxLayout>

snapshots_dialog::snapshots_dialog(const QString &hull_program, const QString &machine_file,
                                   const QString &title, const QString &running, QWidget *parent)
    : QDialog(parent), hull(hull_program), file(machine_file),
      target(running.isEmpty() ? machine_file : running)
{
	setWindowTitle(running.isEmpty() ? QStringLiteral("Snapshots of %1").arg(title)
	                                 : QStringLiteral("Snapshots of %1, running").arg(title));
	list = new QTreeWidget;
	list->setColumnCount(2);
	list->setHeaderLabels({ QStringLiteral("Name"), QStringLiteral("Taken") });
	list->setRootIsDecorated(false);
	list->header()->setStretchLastSection(true);
	connect(list, &QTreeWidget::currentItemChanged, this, &snapshots_dialog::selected_changed);
	log = new QPlainTextEdit;
	log->setReadOnly(true);
	log->setMaximumBlockCount(2000);

	take_button = new QPushButton(QStringLiteral("&Take..."));
	take_button->setToolTip(QStringLiteral("hull snapshot take: every disk as it is now, "
	                                       "under the name given"));
	restore_button = new QPushButton(QStringLiteral("&Restore"));
	restore_button->setToolTip(QStringLiteral("hull snapshot restore: every disk back to the "
	                                          "snapshot, what was written since lost"));
	delete_button = new QPushButton(QStringLiteral("&Delete"));
	delete_button->setToolTip(QStringLiteral("hull snapshot delete: the snapshot, the disks "
	                                         "as they are kept"));
	connect(take_button, &QPushButton::clicked, this, [this]() {
		bool          given = false;
		/* A name offered as VirtualBox offers "Snapshot N": the time,
		 * which sorts and says when. */
		const QString offered = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd-HHmm"));
		const QString name = QInputDialog::getText(this, QStringLiteral("Take Snapshot"),
		                                           QStringLiteral("Its name (letters, digits, "
		                                                          "'.', '_' and '-'):"),
		                                           QLineEdit::Normal, offered, &given)
		                         .trimmed();

		if (given && !name.isEmpty())
			run({ QStringLiteral("snapshot"), QStringLiteral("take"), target, name });
	});
	connect(restore_button, &QPushButton::clicked, this, [this]() {
		QTreeWidgetItem *item = list->currentItem();

		if (item == nullptr)
			return;
		const QString name = item->text(0);

		const QString asked = target == file ?
		    QStringLiteral("Put every disk back as %1 had it? What was written since is lost, "
		                   "unless a snapshot keeps it.").arg(name) :
		    QStringLiteral("Put the running machine back where it was at %1? What it did "
		                   "since is lost, unless a snapshot keeps it.").arg(name);

		if (QMessageBox::question(this, QStringLiteral("Restore Snapshot"), asked) ==
		    QMessageBox::Yes)
			run({ QStringLiteral("snapshot"), QStringLiteral("restore"), target, name });
	});
	connect(delete_button, &QPushButton::clicked, this, [this]() {
		QTreeWidgetItem *item = list->currentItem();

		if (item == nullptr)
			return;
		const QString name = item->text(0);

		if (QMessageBox::question(this, QStringLiteral("Delete Snapshot"),
		                          QStringLiteral("Delete the snapshot %1? The disks stay as "
		                                         "they are.")
		                              .arg(name)) == QMessageBox::Yes)
			run({ QStringLiteral("snapshot"), QStringLiteral("delete"), target, name });
	});

	QHBoxLayout *buttons = new QHBoxLayout;
	buttons->addWidget(take_button);
	buttons->addWidget(restore_button);
	buttons->addWidget(delete_button);
	buttons->addStretch(1);
	QDialogButtonBox *close = new QDialogButtonBox(QDialogButtonBox::Close);
	connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
	buttons->addWidget(close);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(list, 3);
	layout->addLayout(buttons);
	layout->addWidget(log, 1);
	reload();
}

/* `hull snapshot ls FILE`: "NAME<tab>TAKEN" a line, the newest last. */
void snapshots_dialog::reload()
{
	QProcess listing;
	QString  keep = list->currentItem() != nullptr ? list->currentItem()->text(0) : QString();

	listing.setStandardInputFile(QProcess::nullDevice());
	listing.start(hull, { QStringLiteral("snapshot"), QStringLiteral("ls"), file });
	list->clear();
	if (!listing.waitForFinished(30000) || listing.exitCode() != 0) {
		log->appendPlainText(QStringLiteral("hull snapshot ls: %1")
		                         .arg(QString::fromUtf8(listing.readAllStandardError()).trimmed()));
		selected_changed();
		return;
	}
	const QStringList lines =
	    QString::fromUtf8(listing.readAllStandardOutput()).split(QLatin1Char('\n'));

	for (const QString &line : lines) {
		const int tab = line.indexOf(QLatin1Char('\t'));

		if (tab <= 0)
			continue;
		QTreeWidgetItem *item = new QTreeWidgetItem({ line.left(tab), line.mid(tab + 1) });

		list->addTopLevelItem(item);
		if (item->text(0) == keep)
			list->setCurrentItem(item);
	}
	list->resizeColumnToContents(0);
	if (list->currentItem() == nullptr && list->topLevelItemCount() > 0)
		list->setCurrentItem(list->topLevelItem(list->topLevelItemCount() - 1));
	selected_changed();
}

void snapshots_dialog::selected_changed()
{
	const bool any = list->currentItem() != nullptr;

	take_button->setEnabled(busy == nullptr);
	restore_button->setEnabled(any && busy == nullptr);
	delete_button->setEnabled(any && busy == nullptr);
}

/* One `hull snapshot` command, its output in the log, the list read
 * again at its end. One at a time: each writes every disk. */
void snapshots_dialog::run(const QStringList &arguments)
{
	if (busy != nullptr)
		return;
	busy = new QProcess(this);
	busy->setProcessChannelMode(QProcess::MergedChannels);
	busy->setStandardInputFile(QProcess::nullDevice());
	log->appendPlainText(QStringLiteral("$ hull ") + arguments.join(QLatin1Char(' ')));
	connect(busy, &QProcess::readyRead, this, [this]() {
		log->moveCursor(QTextCursor::End);
		log->insertPlainText(QString::fromUtf8(busy->readAll()));
		log->verticalScrollBar()->setValue(log->verticalScrollBar()->maximum());
	});
	connect(busy, &QProcess::finished, this, [this](int code) {
		log->appendPlainText(code == 0 ? QStringLiteral("[done]")
		                               : QStringLiteral("[exited %1]").arg(code));
		busy->deleteLater();
		busy = nullptr;
		reload();
	});
	connect(busy, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
		if (error != QProcess::FailedToStart)
			return;
		log->appendPlainText(busy->errorString());
		busy->deleteLater();
		busy = nullptr;
		selected_changed();
	});
	busy->start(hull, arguments);
	selected_changed();
}
