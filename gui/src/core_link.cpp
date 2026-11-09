/*
 * core_link.cpp -- see core_link.h.
 */

#include "core_link.h"

#include <QSocketNotifier>
#include <QTimer>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

extern "C" {
#include "sexpr.h"
#include "wire.h"
}

QString core_request::socket_path()
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");

	if (runtime == nullptr || runtime[0] != '/')
		return QString();
	return QString::fromLocal8Bit(runtime) + QStringLiteral("/hull/core.sock");
}

/* The local core, as the CLI's dial_local() reaches it: through a
 * descriptor on its directory where the whole path does not fit in a
 * socket address. A descriptor, or -1 where none listens. */
static int dial_core()
{
	const char        *runtime = getenv("XDG_RUNTIME_DIR");
	struct sockaddr_un address;
	char               dir[4096];
	int                at = -1;
	int                fd;

	if (runtime == nullptr || runtime[0] != '/' ||
	    (size_t)snprintf(dir, sizeof dir, "%s/hull/", runtime) >= sizeof dir)
		return -1;
	memset(&address, 0, sizeof address);
	address.sun_family = AF_UNIX;
	if ((size_t)snprintf(address.sun_path, sizeof address.sun_path, "%score.sock", dir) >=
	    sizeof address.sun_path) {
		at = open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC);
		if (at < 0)
			return -1;
		snprintf(address.sun_path, sizeof address.sun_path, "/proc/self/fd/%d/core.sock", at);
	}
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd >= 0 && ::connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
		close(fd);
		fd = -1;
	}
	if (at >= 0)
		close(at);
	return fd;
}

core_request::core_request(QObject *parent) : QObject(parent)
{
}

core_request::~core_request()
{
	delete notifier;
	if (wire != nullptr) {
		hull_wire_close(wire);
		delete wire;
	}
}

core_request *core_request::send(QObject *parent, const char *type, const QString &machine,
                                 const QByteArray &rest, int timeout_ms)
{
	core_request *request = new core_request(parent);
	hull_msg      m = {};
	int           fd = dial_core();

	/* Answered from the event loop, so that a caller has connected its
	 * slots before anything is said. */
	if (fd < 0) {
		QTimer::singleShot(0, request, [request]() {
			request->end(no_core, QStringLiteral("no core is running, so no machine is"));
		});
		return request;
	}
	request->wire = new hull_wire;
	if (hull_wire_open(request->wire, fd, HULL_WIRE_FROM_CORE) != 0) {
		close(fd);
		delete request->wire;
		request->wire = nullptr;
		QTimer::singleShot(0, request,
		                   [request]() { request->end(lost, QStringLiteral("out of memory")); });
		return request;
	}
	/* Hello without a build: a GUI is never the frontend a core gives way
	 * to, so an idle core of another build stays for the CLI that wants it. */
	hull_msg_raw(&m, "(5:hello(2:ch1:0)(5:proto1:1))");
	hull_wire_send(fd, &m);
	hull_msg_free(&m);
	hull_msg_raw(&m, type);
	hull_msg_channel(&m, 1u);
	if (rest.startsWith("(6:follow"))
		hull_msg_raw(&m, rest.constData());
	if (!machine.isEmpty()) {
		const QByteArray id = machine.toUtf8();

		hull_msg_raw(&m, "(7:machine");
		hull_msg_atom(&m, (const uint8_t *)id.constData(), (size_t)id.size());
		hull_msg_raw(&m, ")");
	}
	if (!rest.isEmpty() && !rest.startsWith("(6:follow"))
		hull_msg_raw(&m, rest.constData());
	hull_msg_raw(&m, ")");
	request->wire->fd = fd;
	if (hull_wire_send(fd, &m) != 0) {
		hull_msg_free(&m);
		QTimer::singleShot(0, request, [request]() {
			request->end(lost, QStringLiteral("the core did not take the request"));
		});
		return request;
	}
	hull_msg_free(&m);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	request->notifier = new QSocketNotifier(fd, QSocketNotifier::Read, request);
	connect(request->notifier, &QSocketNotifier::activated, request, &core_request::readable);
	if (timeout_ms > 0) {
		request->deadline = new QTimer(request);
		request->deadline->setSingleShot(true);
		connect(request->deadline, &QTimer::timeout, request, [request]() {
			request->end(lost, QStringLiteral("the core did not answer in time"));
		});
		request->deadline->start(timeout_ms);
	}
	return request;
}

void core_request::end(outcome how, const QString &reason)
{
	if (ended)
		return;
	ended = true;
	if (notifier != nullptr)
		notifier->setEnabled(false);
	if (deadline != nullptr)
		deadline->stop();
	emit finished(how, reason);
	deleteLater();
}

void core_request::readable()
{
	hull_wire_status status = hull_wire_fill(wire);

	if (status == HULL_WIRE_WAIT)
		return;
	if (status != HULL_WIRE_OK) {
		end(lost, status == HULL_WIRE_CLOSED ? QStringLiteral("the core went away")
		                                     : QStringLiteral("the core said what this cannot "
		                                                      "read"));
		return;
	}
	for (;;) {
		hull_sexpr_doc body;

		status = hull_wire_next(wire, &body);
		if (status == HULL_WIRE_WAIT)
			return;
		if (status != HULL_WIRE_OK) {
			end(lost, QStringLiteral("the core said what this cannot read"));
			return;
		}
		if (!take(&body))
			return;
	}
}

/* The value of `(key value)` in `item`, a list, as text; "" where absent. */
static QString value_of(const hull_sexpr_doc *body, const hull_sexpr_form *item, const char *key)
{
	hull_sexpr_form entry;
	hull_sexpr_form name;
	hull_sexpr_form value;
	char            text[512];

	if (!hull_sexpr_child(body, item, &entry))
		return QString();
	for (int more = hull_sexpr_next(body, &entry, &entry); more;
	     more = hull_sexpr_next(body, &entry, &entry))
		if (hull_sexpr_child(body, &entry, &name) && hull_sexpr_is(body, &name, key) &&
		    hull_sexpr_next(body, &name, &value) &&
		    hull_msg_string(body, &value, text, sizeof text))
			return QString::fromUtf8(text);
	return QString();
}

/* One message: false once the request has ended. */
bool core_request::take(const void *opened)
{
	const hull_sexpr_doc *body = static_cast<const hull_sexpr_doc *>(opened);
	hull_sexpr_form       value;
	long long             channel = -1;

	if (!greeted) {
		greeted = true;
		if (!hull_msg_is(body, "hello")) {
			end(lost, QStringLiteral("the core did not answer hello"));
			return false;
		}
		return true;
	}
	if (!hull_msg_find(body, "ch", &value) || !hull_msg_long(body, &value, &channel) ||
	    channel != 1)
		return true;
	if (hull_msg_is(body, "machines")) {
		QList<machine_row> rows;
		hull_sexpr_form    message;
		hull_sexpr_form    item;

		hull_sexpr_first(body, &message);
		hull_sexpr_child(body, &message, &item);
		for (int more = hull_sexpr_next(body, &item, &item); more;
		     more = hull_sexpr_next(body, &item, &item)) {
			hull_sexpr_form head;
			machine_row     row;

			if (!hull_sexpr_child(body, &item, &head) || !hull_sexpr_is(body, &head, "machine"))
				continue;
			row.id = value_of(body, &item, "id");
			row.address = value_of(body, &item, "address");
			row.engine = value_of(body, &item, "engine");
			row.machine_class = value_of(body, &item, "class");
			row.state = value_of(body, &item, "state");
			row.arch = value_of(body, &item, "arch");
			row.root = value_of(body, &item, "root") == QStringLiteral("true");
			rows.append(row);
		}
		emit machines(rows);
		end(done, QString());
		return false;
	}
	if (hull_msg_is(body, "stats")) {
		long long figures[3] = { 0, 0, 0 };
		const char *const keys[] = { "cpu", "io", "memory" };

		for (unsigned i = 0u; i < 3u; i++)
			if (!hull_msg_find(body, keys[i], &value) ||
			    !hull_msg_long(body, &value, &figures[i])) {
				end(lost, QStringLiteral("the core's answer to stats is not stats"));
				return false;
			}
		emit stats(figures[0], figures[1], figures[2]);
		end(done, QString());
		return false;
	}
	if (hull_msg_is(body, "data")) {
		hull_sexpr_form buf;
		hull_sexpr_form fd;

		if (hull_msg_find(body, "buf", &buf) && hull_msg_find(body, "fd", &fd))
			emit output(hull_sexpr_is(body, &fd, "2") ? 2 : 1,
			            QByteArray((const char *)body->data + buf.content,
			                       (qsizetype)buf.content_length));
		return true;
	}
	if (hull_msg_is(body, "lost")) {
		long long bytes = 0;

		if (hull_msg_find(body, "bytes", &value))
			hull_msg_long(body, &value, &bytes);
		emit output(2, QStringLiteral("[%1 bytes of output lost: this reader fell behind]\n")
		                   .arg(bytes)
		                   .toUtf8());
		return true;
	}
	if (hull_msg_is(body, "error")) {
		char reason[512] = "an error";

		if (hull_msg_find(body, "reason", &value))
			hull_msg_string(body, &value, reason, sizeof reason);
		end(refused, QString::fromUtf8(reason));
		return false;
	}
	if (hull_msg_is(body, "exited")) {
		long long code = 0;
		long long sig = 0;

		if (hull_msg_find(body, "code", &value))
			hull_msg_long(body, &value, &code);
		if (hull_msg_find(body, "signal", &value))
			hull_msg_long(body, &value, &sig);
		end(done, sig != 0 ? QStringLiteral("ended by signal %1").arg(sig)
		                   : QStringLiteral("exited %1").arg(code));
		return false;
	}
	if (hull_msg_is(body, "eof") || hull_msg_is(body, "done")) {
		end(done, QString());
		return false;
	}
	return true;
}
