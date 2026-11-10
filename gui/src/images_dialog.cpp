/*
 * images_dialog.cpp -- see images_dialog.h.
 */

#include "images_dialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QTreeWidget>
#include <QVBoxLayout>

images_dialog::images_dialog(const QString &hull_program, QWidget *parent)
    : QDialog(parent), hull(hull_program)
{
	setWindowTitle(QStringLiteral("Images"));
	list = new QTreeWidget;
	list->setColumnCount(2);
	list->setHeaderLabels({ QStringLiteral("Ref"), QStringLiteral("Manifest") });
	list->setRootIsDecorated(false);
	list->header()->setStretchLastSection(true);
	connect(list, &QTreeWidget::currentItemChanged, this, &images_dialog::selected_changed);
	log = new QPlainTextEdit;
	log->setReadOnly(true);
	log->setMaximumBlockCount(2000);

	pull_button = new QPushButton(QStringLiteral("&Pull..."));
	pull_button->setToolTip(QStringLiteral("hull pull IMAGE [REF]: an image from a registry"));
	push_button = new QPushButton(QStringLiteral("P&ush..."));
	push_button->setToolTip(QStringLiteral("hull push REF IMAGE: the ref to a registry, under "
	                                       "the name given"));
	remove_button = new QPushButton(QStringLiteral("&Remove"));
	remove_button->setToolTip(QStringLiteral("hull tag -d REF: the ref; its objects stay "
	                                         "until nothing names them"));
	new_button = new QPushButton(QStringLiteral("&New Machine"));
	new_button->setToolTip(QStringLiteral("A machine whose source is this image"));
	connect(pull_button, &QPushButton::clicked, this, [this]() {
		bool          given = false;
		const QString image = QInputDialog::getText(
		    this, QStringLiteral("Pull"),
		    QStringLiteral("An image, as a registry names it (debian:13, "
		                   "ghcr.io/owner/name:tag):"),
		    QLineEdit::Normal, QString(), &given).trimmed();

		if (given && !image.isEmpty())
			run({ QStringLiteral("pull"), image });
	});
	connect(push_button, &QPushButton::clicked, this, [this]() {
		QTreeWidgetItem *item = list->currentItem();
		bool             given = false;

		if (item == nullptr)
			return;
		const QString ref = item->text(0);
		const QString image = QInputDialog::getText(
		    this, QStringLiteral("Push"),
		    QStringLiteral("%1 to a registry, as (ghcr.io/owner/name:tag, "
		                   "localhost:5000/name:tag):")
		        .arg(ref),
		    QLineEdit::Normal, QString(), &given).trimmed();

		if (given && !image.isEmpty())
			run({ QStringLiteral("push"), ref, image });
	});
	connect(remove_button, &QPushButton::clicked, this, [this]() {
		QTreeWidgetItem *item = list->currentItem();

		if (item == nullptr)
			return;
		const QString ref = item->text(0);

		if (QMessageBox::question(this, QStringLiteral("Remove"),
		                          QStringLiteral("Remove the ref %1? A machine whose source "
		                                         "names it then has none.")
		                              .arg(ref)) == QMessageBox::Yes)
			run({ QStringLiteral("tag"), QStringLiteral("-d"), ref });
	});
	connect(new_button, &QPushButton::clicked, this, [this]() {
		QTreeWidgetItem *item = list->currentItem();

		if (item != nullptr)
			emit new_machine_from(item->text(0));
	});

	QHBoxLayout *buttons = new QHBoxLayout;
	buttons->addWidget(pull_button);
	buttons->addWidget(push_button);
	buttons->addWidget(remove_button);
	buttons->addWidget(new_button);
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

QStringList images_dialog::refs() const
{
	QStringList refs;

	for (int i = 0; i < list->topLevelItemCount(); i++)
		refs << list->topLevelItem(i)->text(0);
	return refs;
}

/* `hull images`: "REF MANIFEST" under a header line. */
void images_dialog::reload()
{
	QProcess images;
	QString  keep = list->currentItem() != nullptr ? list->currentItem()->text(0) : QString();

	images.setStandardInputFile(QProcess::nullDevice());
	images.start(hull, { QStringLiteral("images") });
	list->clear();
	if (!images.waitForFinished(10000) || images.exitCode() != 0) {
		log->appendPlainText(QStringLiteral("hull images: %1")
		                         .arg(QString::fromUtf8(images.readAllStandardError()).trimmed()));
		selected_changed();
		return;
	}
	const QStringList lines =
	    QString::fromUtf8(images.readAllStandardOutput()).split(QLatin1Char('\n')).mid(1);

	for (const QString &line : lines) {
		const QStringList fields = line.simplified().split(QLatin1Char(' '));

		if (fields.size() < 2)
			continue;
		QTreeWidgetItem *item = new QTreeWidgetItem({ fields[0], fields[1] });

		list->addTopLevelItem(item);
		if (fields[0] == keep)
			list->setCurrentItem(item);
	}
	list->resizeColumnToContents(0);
	if (list->currentItem() == nullptr && list->topLevelItemCount() > 0)
		list->setCurrentItem(list->topLevelItem(0));
	selected_changed();
}

void images_dialog::selected_changed()
{
	const bool any = list->currentItem() != nullptr;

	remove_button->setEnabled(any && busy == nullptr);
	push_button->setEnabled(any && busy == nullptr);
	new_button->setEnabled(any);
	pull_button->setEnabled(busy == nullptr);
}

/* One of the store's commands, its output in the log as it comes, the
 * list read again at its end. One at a time: the store takes its lock. */
void images_dialog::run(const QStringList &arguments)
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
		log->appendPlainText(QStringLiteral("[exited %1]").arg(code));
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
