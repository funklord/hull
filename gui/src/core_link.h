/*
 * core_link.h -- the GUI's side of the frontend protocol (project.md
 * section 4.10): one request to the per-user core on a connection of its
 * own, answered asynchronously through the event loop.
 *
 * The bytes are the CLI's: hull's own C reads and writes them (core/wire.c,
 * held to core/frontend.situ by situ's validator), so the GUI is a peer
 * frontend and not a second reading of the protocol. It never starts a
 * core and never makes one give way to it: it says hello without a build,
 * which a core takes as a frontend that cannot replace it, so a GUI left
 * open does not end a core another build's CLI started.
 */

#ifndef HULL_GUI_CORE_LINK_H
#define HULL_GUI_CORE_LINK_H

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

class QSocketNotifier;
class QTimer;
struct hull_wire;

/* One machine of the core's `machines` answer. */
struct machine_row {
	QString id;
	QString address;
	QString engine;
	QString machine_class;
	QString state;
	QString arch;
	bool    root = false;
};

class core_request : public QObject {
	Q_OBJECT

public:
	enum outcome {
		done,     /* answered: done, eof, an exit, or the answer asked for */
		refused,  /* the core said error, `reason` its words */
		no_core,  /* nothing listens on the socket: no machine runs */
		lost,     /* the core went, or said what this cannot read */
	};
	Q_ENUM(outcome)

	/*
	 * Send `(TYPE (ch 1) [rest-before] (machine ID) [rest])` as the CLI's
	 * send_request() does: `type` is the opening, "(4:list" say, and
	 * `rest` canonical entries, sorted around `machine` as the protocol
	 * wants -- `follow` sorts before it, the rest after. The request
	 * deletes itself after finished(); `timeout_ms` 0 waits for ever, as a
	 * followed log does.
	 */
	static core_request *send(QObject *parent, const char *type, const QString &machine,
	                          const QByteArray &rest, int timeout_ms = 10000);

	~core_request() override;

	/* The runtime directory's hull/core.sock, as the core names it. */
	static QString socket_path();

signals:
	void machines(const QList<machine_row> &rows);
	void stats(qint64 cpu_us, qint64 io_bytes, qint64 memory_bytes);
	void output(int fd, const QByteArray &bytes);
	void finished(core_request::outcome how, const QString &reason);

private:
	explicit core_request(QObject *parent);
	void readable();
	void end(outcome how, const QString &reason);
	bool take(const void *body);

	hull_wire       *wire = nullptr;
	QSocketNotifier *notifier = nullptr;
	QTimer          *deadline = nullptr;
	bool             greeted = false;
	bool             ended = false;
};

#endif
