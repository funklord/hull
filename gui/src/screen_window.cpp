/*
 * screen_window.cpp -- a machine's screen by qemu's D-Bus display, no bus:
 * one end of a socket pair handed to qemu by its monitor (getfd, then
 * add_client), D-Bus spoken on the other by GIO, and a listener of the
 * first console registered over a second pair, on which qemu calls back
 * with frames. Its interface is qemu's ui/dbus-display1.xml.
 *
 * GIO's callbacks run on GLib's default context, which Qt's event loop
 * iterates where it is GLib's own dispatcher, as on a Linux desktop;
 * elsewhere a timer iterates it.
 */

#include "screen_window.h"
#include "dmabuf_view.h"

#include <QAbstractEventDispatcher>
#include <QPixmap>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* GLib names a struct member `signals`, which Qt defines away. */
#pragma push_macro("signals")
#undef signals
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#pragma pop_macro("signals")

static const char listener_xml[] =
    "<node>"
    "<interface name='org.qemu.Display1.Listener'>"
    "<method name='Scanout'><arg type='u' direction='in'/><arg type='u' direction='in'/>"
    "<arg type='u' direction='in'/><arg type='u' direction='in'/><arg type='ay' direction='in'/></method>"
    "<method name='Update'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='u' direction='in'/>"
    "<arg type='u' direction='in'/><arg type='ay' direction='in'/></method>"
    "<method name='ScanoutDMABUF'><arg type='h' direction='in'/><arg type='u' direction='in'/>"
    "<arg type='u' direction='in'/><arg type='u' direction='in'/><arg type='u' direction='in'/>"
    "<arg type='t' direction='in'/><arg type='b' direction='in'/></method>"
    "<method name='UpdateDMABUF'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='Disable'/>"
    "<method name='MouseSet'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/></method>"
    "<method name='CursorDefine'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='ay' direction='in'/></method>"
    "<property name='Interfaces' type='as' access='read'/>"
    "</interface>"
    "<interface name='org.qemu.Display1.Listener.Unix.Map'>"
    "<method name='ScanoutMap'><arg type='h' direction='in'/><arg type='u' direction='in'/>"
    "<arg type='u' direction='in'/><arg type='u' direction='in'/><arg type='u' direction='in'/>"
    "<arg type='u' direction='in'/></method>"
    "<method name='UpdateMap'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "</interface>"
    "</node>";

/* pixman's x8r8g8b8 and a8r8g8b8, which is what qemu's consoles are. */
enum { PIXMAN_X8R8G8B8 = 0x20020888, PIXMAN_A8R8G8B8 = 0x20028888 };

struct screen_link {
	screen_window   *window = nullptr;
	GCancellable    *cancel = nullptr;
	GDBusConnection *bus = nullptr;       /* to qemu's display */
	GDBusConnection *listener = nullptr;  /* qemu's calls to us */
	GDBusProxy      *console = nullptr;
	GDBusProxy      *keyboard = nullptr;
	GDBusProxy      *mouse = nullptr;
	GDBusNodeInfo   *node = nullptr;
	guint            registered[2] = { 0u, 0u };
	QTimer          *iterate = nullptr;   /* where Qt's loop is not GLib's */
};

/* One answer of QMP, events passing by: true for a return. */
static bool qmp_answer(int fd, QString *why)
{
	char   line[4096];
	size_t have = 0u;

	for (;;) {
		struct pollfd p = { fd, POLLIN, 0 };
		ssize_t       n;
		char         *end;

		if (poll(&p, 1u, 5000) <= 0) {
			*why = QStringLiteral("the machine's monitor did not answer");
			return false;
		}
		n = read(fd, line + have, sizeof line - 1u - have);
		if (n <= 0) {
			*why = QStringLiteral("the machine's monitor closed");
			return false;
		}
		have += (size_t)n;
		line[have] = '\0';
		while ((end = strchr(line, '\n')) != nullptr) {
			*end = '\0';
			if (strncmp(line, "{\"return\"", 9u) == 0)
				return true;
			if (strncmp(line, "{\"error\"", 8u) == 0) {
				*why = QStringLiteral("qemu: %1").arg(QString::fromUtf8(line));
				return false;
			}
			have -= (size_t)(end + 1 - line);
			memmove(line, end + 1, have + 1u);
		}
		if (have == sizeof line - 1u)
			have = 0u;
	}
}

/* A command said, with `fd` passed where it is not -1. */
static bool qmp_say(int fd, const char *command, int pass, QString *why)
{
	struct iovec  part = { const_cast<char *>(command), strlen(command) };
	struct msghdr message = {};
	char          control[CMSG_SPACE(sizeof(int))] = {};

	message.msg_iov = &part;
	message.msg_iovlen = 1u;
	if (pass >= 0) {
		struct cmsghdr *header;

		message.msg_control = control;
		message.msg_controllen = sizeof control;
		header = CMSG_FIRSTHDR(&message);
		header->cmsg_level = SOL_SOCKET;
		header->cmsg_type = SCM_RIGHTS;
		header->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(header), &pass, sizeof(int));
	}
	if (sendmsg(fd, &message, MSG_NOSIGNAL) != (ssize_t)part.iov_len) {
		*why = QStringLiteral("the machine's monitor: %1").arg(QString::fromUtf8(strerror(errno)));
		return false;
	}
	return qmp_answer(fd, why);
}

/* A D-Bus connection over our end of a pair, qemu authenticating. */
static GDBusConnection *peer(int fd, bool delay, GError **error)
{
	GSocket          *socket = g_socket_new_from_fd(fd, error);
	GSocketConnection *stream;
	GDBusConnection  *connection;

	if (socket == nullptr) {
		close(fd);
		return nullptr;
	}
	stream = g_socket_connection_factory_create_connection(socket);
	g_object_unref(socket);
	connection = g_dbus_connection_new_sync(
	    G_IO_STREAM(stream), nullptr,
	    /* qemu runs in a user namespace, where our uid is not 1000: an empty
	     * EXTERNAL identity lets it take the socket's credentials, as its
	     * namespace sees them, which are its own. */
	    GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
	                         G_DBUS_CONNECTION_FLAGS_CROSS_NAMESPACE |
	                         (delay ? G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING : 0)),
	    nullptr, nullptr, error);
	g_object_unref(stream);
	return connection;
}

static void called(GDBusConnection *, const gchar *, const gchar *, const gchar *interface,
                   const gchar *method, GVariant *parameters, GDBusMethodInvocation *invocation,
                   gpointer data)
{
	screen_link   *link = static_cast<screen_link *>(data);
	screen_window *window = link->window;

	if (strcmp(method, "ScanoutMap") == 0) {
		GUnixFDList *fds = g_dbus_message_get_unix_fd_list(g_dbus_method_invocation_get_message(invocation));
		gint32       handle;
		guint32      offset, width, height, stride, format;
		int          fd;
		void        *mapped;
		size_t       length;

		g_variant_get(parameters, "(huuuuu)", &handle, &offset, &width, &height, &stride, &format);
		fd = fds != nullptr ? g_unix_fd_list_get(fds, handle, nullptr) : -1;
		length = (size_t)stride * height;
		mapped = fd < 0 || length == 0u ? MAP_FAILED :
		         mmap(nullptr, length + offset, PROT_READ, MAP_SHARED, fd, 0);
		if (fd >= 0)
			close(fd);
		/* A row narrower than its pixels would have the image read past
		 * the mapping's end on the last one. */
		if (mapped == MAP_FAILED || (guint64)stride < (guint64)width * 4u ||
		    (format != PIXMAN_X8R8G8B8 && format != PIXMAN_A8R8G8B8)) {
			if (mapped != MAP_FAILED)
				munmap(mapped, length + offset);
			g_dbus_method_invocation_return_dbus_error(invocation, "org.qemu.Display1.Error",
			                                           "the framebuffer cannot be shown");
			return;
		}
		window->scanout(QImage(static_cast<const uchar *>(mapped) + offset, (int)width, (int)height,
		                       (qsizetype)stride, QImage::Format_RGB32),
		                mapped, length + offset);
	} else if (strcmp(method, "Scanout") == 0) {
		guint32   width, height, stride, format;
		GVariant *data;
		gsize     size = 0u;
		const void *bytes;

		g_variant_get(parameters, "(uuuu@ay)", &width, &height, &stride, &format, &data);
		bytes = g_variant_get_fixed_array(data, &size, 1u);
		if ((format == PIXMAN_X8R8G8B8 || format == PIXMAN_A8R8G8B8) &&
		    (guint64)stride >= (guint64)width * 4u && size >= (gsize)stride * height)
			window->scanout(QImage(static_cast<const uchar *>(bytes), (int)width, (int)height,
			                       (qsizetype)stride, QImage::Format_RGB32).copy(),
			                nullptr, 0u);
		g_variant_unref(data);
	} else if (strcmp(method, "Update") == 0) {
		gint32    x, y, w, h;
		guint32   stride, format;
		GVariant *data;

		g_variant_get(parameters, "(iiiiuu@ay)", &x, &y, &w, &h, &stride, &format, &data);
		g_variant_unref(data);
		(void)stride;
		(void)format;
		window->update_rect(QRect(x, y, w, h));
	} else if (strcmp(method, "UpdateMap") == 0) {
		gint32 x, y, w, h;

		g_variant_get(parameters, "(iiii)", &x, &y, &w, &h);
		window->update_rect(QRect(x, y, w, h));
	} else if (strcmp(method, "ScanoutDMABUF") == 0) {
		GUnixFDList *fds = g_dbus_message_get_unix_fd_list(g_dbus_method_invocation_get_message(invocation));
		gint32       handle;
		guint32      width, height, stride, fourcc;
		guint64      modifier;
		gboolean     y0_top;
		int          fd;

		g_variant_get(parameters, "(huuuutb)", &handle, &width, &height, &stride, &fourcc,
		              &modifier, &y0_top);
		fd = fds != nullptr ? g_unix_fd_list_get(fds, handle, nullptr) : -1;
		if (fd >= 0)
			window->scanout_dmabuf(fd, (int)width, (int)height, (int)stride, fourcc, modifier,
			                       y0_top != FALSE);
	} else if (strcmp(method, "UpdateDMABUF") == 0) {
		/* qemu waits for this answer before rendering into the buffer
		 * again, so it is given once the frame has been drawn. */
		g_object_ref(invocation);
		window->update_dmabuf([invocation] {
			g_dbus_method_invocation_return_value(invocation, nullptr);
			g_object_unref(invocation);
		});
		return;
	} else if (strcmp(method, "Disable") == 0) {
		window->disable();
	} else if (strcmp(method, "CursorDefine") == 0) {
		gint32    w, h, hot_x, hot_y;
		GVariant *data;
		gsize     size = 0u;
		const void *bytes;

		g_variant_get(parameters, "(iiii@ay)", &w, &h, &hot_x, &hot_y, &data);
		bytes = g_variant_get_fixed_array(data, &size, 1u);
		if (w > 0 && h > 0 && w <= 512 && h <= 512 && size >= (gsize)w * (gsize)h * 4u)
			window->cursor(QImage(static_cast<const uchar *>(bytes), w, h, w * 4,
			                      QImage::Format_ARGB32).copy(),
			               QPoint(hot_x, hot_y));
		g_variant_unref(data);
	} else if (strcmp(method, "MouseSet") == 0) {
		gint32 x, y, on;

		g_variant_get(parameters, "(iii)", &x, &y, &on);
		window->cursor_shown(on != 0);
	}
	(void)interface;
	g_dbus_method_invocation_return_value(invocation, nullptr);
}

static GVariant *property(GDBusConnection *, const gchar *, const gchar *, const gchar *,
                          const gchar *, GError **, gpointer)
{
	const gchar *const names[] = { "org.qemu.Display1.Listener.Unix.Map", nullptr };

	return g_variant_new_strv(names, -1);
}

static const GDBusInterfaceVTable vtable = { called, property, nullptr, {} };

screen_window::screen_window(const QString &id_, const QString &monitor_, QWidget *parent)
    : QWidget(parent), id(id_), monitor(monitor_)
{
	setWindowTitle(QStringLiteral("%1 -- hull").arg(id));
	setAttribute(Qt::WA_DeleteOnClose);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setFocusPolicy(Qt::StrongFocus);
	setMouseTracking(true);
	resize(1024, 768);
}

screen_window::~screen_window()
{
	if (link != nullptr) {
		g_cancellable_cancel(link->cancel);
		for (guint id_ : link->registered)
			if (id_ != 0u)
				g_dbus_connection_unregister_object(link->listener, id_);
		if (link->listener != nullptr) {
			g_dbus_connection_close_sync(link->listener, nullptr, nullptr);
			g_object_unref(link->listener);
		}
		if (link->bus != nullptr) {
			g_dbus_connection_close_sync(link->bus, nullptr, nullptr);
			g_object_unref(link->bus);
		}
		g_clear_object(&link->console);
		g_clear_object(&link->keyboard);
		g_clear_object(&link->mouse);
		if (link->node != nullptr)
			g_dbus_node_info_unref(link->node);
		g_object_unref(link->cancel);
		delete link;
	}
	if (map != nullptr)
		munmap(map, map_length);
}

bool screen_window::open(QString *why)
{
	const QByteArray path = monitor.toLocal8Bit();
	struct sockaddr_un address = {};
	int    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	int    pair[2] = { -1, -1 };
	int    listen_pair[2] = { -1, -1 };
	GError *error = nullptr;
	GUnixFDList *fds;
	bool   said;

	address.sun_family = AF_UNIX;
	if (fd < 0 || (size_t)path.size() >= sizeof address.sun_path) {
		*why = QStringLiteral("the machine's monitor cannot be reached");
		if (fd >= 0)
			::close(fd);
		return false;
	}
	memcpy(address.sun_path, path.constData(), (size_t)path.size());
	if (::connect(fd, reinterpret_cast<struct sockaddr *>(&address), sizeof address) != 0) {
		*why = QStringLiteral("no local screen: %1").arg(QString::fromUtf8(strerror(errno)));
		::close(fd);
		return false;
	}
	said = qmp_say(fd, "{\"execute\":\"qmp_capabilities\"}\n", -1, why) &&
	       socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0 &&
	       qmp_say(fd, "{\"execute\":\"getfd\",\"arguments\":{\"fdname\":\"hullgui\"}}\n", pair[1], why) &&
	       qmp_say(fd, "{\"execute\":\"add_client\",\"arguments\":"
	               "{\"protocol\":\"@dbus-display\",\"fdname\":\"hullgui\"}}\n", -1, why);
	::close(fd);
	if (pair[1] >= 0)
		::close(pair[1]);
	if (!said) {
		if (pair[0] >= 0)
			::close(pair[0]);
		if (why->isEmpty())
			*why = QStringLiteral("a socket pair: %1").arg(QString::fromUtf8(strerror(errno)));
		return false;
	}

	link = new screen_link;
	link->window = this;
	link->cancel = g_cancellable_new();
	link->bus = peer(pair[0], false, &error);
	if (link->bus != nullptr)
		link->console = g_dbus_proxy_new_sync(link->bus, G_DBUS_PROXY_FLAGS_NONE, nullptr, nullptr,
		                                      "/org/qemu/Display1/Console_0",
		                                      "org.qemu.Display1.Console", nullptr, &error);
	if (link->console != nullptr)
		link->keyboard = g_dbus_proxy_new_sync(link->bus, G_DBUS_PROXY_FLAGS_NONE, nullptr, nullptr,
		                                       "/org/qemu/Display1/Console_0",
		                                       "org.qemu.Display1.Keyboard", nullptr, &error);
	if (link->keyboard != nullptr)
		link->mouse = g_dbus_proxy_new_sync(link->bus, G_DBUS_PROXY_FLAGS_NONE, nullptr, nullptr,
		                                    "/org/qemu/Display1/Console_0",
		                                    "org.qemu.Display1.Mouse", nullptr, &error);
	if (link->mouse == nullptr ||
	    socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, listen_pair) != 0) {
		*why = QStringLiteral("qemu's display: %1")
		           .arg(error != nullptr ? QString::fromUtf8(error->message) : QString::fromUtf8(strerror(errno)));
		g_clear_error(&error);
		return false;
	}
	{
		GVariant *width = g_dbus_proxy_get_cached_property(link->console, "Width");
		GVariant *height = g_dbus_proxy_get_cached_property(link->console, "Height");

		if (width != nullptr && height != nullptr)
			resize((int)g_variant_get_uint32(width), (int)g_variant_get_uint32(height));
		if (width != nullptr)
			g_variant_unref(width);
		if (height != nullptr)
			g_variant_unref(height);
	}

	/* qemu authenticates the listener inside RegisterListener, so the call
	 * goes out before our side of it is made. */
	fds = g_unix_fd_list_new_from_array(&listen_pair[1], 1);
	g_dbus_proxy_call_with_unix_fd_list(link->console, "RegisterListener", g_variant_new("(h)", 0),
	                                    G_DBUS_CALL_FLAGS_NONE, 10000, fds, link->cancel,
	                                    nullptr, nullptr);
	g_object_unref(fds);
	link->listener = peer(listen_pair[0], true, &error);
	link->node = g_dbus_node_info_new_for_xml(listener_xml, nullptr);
	if (link->listener == nullptr || link->node == nullptr) {
		*why = QStringLiteral("qemu's display listener: %1")
		           .arg(error != nullptr ? QString::fromUtf8(error->message) : QStringLiteral("?"));
		g_clear_error(&error);
		return false;
	}
	for (int i = 0; i < 2; i++)
		link->registered[i] = g_dbus_connection_register_object(
		    link->listener, "/org/qemu/Display1/Listener", link->node->interfaces[i], &vtable,
		    link, nullptr, nullptr);
	g_dbus_connection_start_message_processing(link->listener);

	const QAbstractEventDispatcher *dispatcher = QAbstractEventDispatcher::instance();
	if (dispatcher == nullptr || !QString::fromLatin1(dispatcher->metaObject()->className()).contains(QStringLiteral("Glib"))) {
		link->iterate = new QTimer(this);
		connect(link->iterate, &QTimer::timeout, this, [] {
			while (g_main_context_iteration(nullptr, FALSE))
				;
		});
		link->iterate->start(5);
	}
	return true;
}

void screen_window::scanout(QImage image, void *mapped, size_t length)
{
	if (map != nullptr)
		munmap(map, map_length);
	map = mapped;
	map_length = length;
	frame = image;
	if (view != nullptr)
		view->hide();
	update();
}

void screen_window::update_rect(const QRect &rect)
{
	const QRect at = shown();

	if (frame.isNull() || frame.width() == 0 || frame.height() == 0)
		return;
	update(QRect(at.x() + rect.x() * at.width() / frame.width() - 1,
	             at.y() + rect.y() * at.height() / frame.height() - 1,
	             rect.width() * at.width() / frame.width() + 3,
	             rect.height() * at.height() / frame.height() + 3));
}

void screen_window::cursor(const QImage &image, const QPoint &hot)
{
	guest_cursor = QCursor(QPixmap::fromImage(image), hot.x(), hot.y());
	if (cursor_on)
		setCursor(guest_cursor);
}

/* The guest draws its own pointer where it says so: ours is hidden then,
 * or two would be seen. */
void screen_window::cursor_shown(bool on)
{
	cursor_on = on;
	if (on)
		setCursor(guest_cursor.pixmap().isNull() ? QCursor(Qt::ArrowCursor) : guest_cursor);
	else
		setCursor(Qt::BlankCursor);
}

/* The window's size told to the guest, as a monitor's preferred mode, so a
 * desktop in it follows the window as VirtualBox's does: once the size
 * has settled, not at every step of a drag. */
void screen_window::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	if (view != nullptr)
		view->setGeometry(rect());
	if (link == nullptr)
		return;
	if (resized == nullptr) {
		resized = new QTimer(this);
		resized->setSingleShot(true);
		connect(resized, &QTimer::timeout, this, &screen_window::tell_size);
	}
	resized->start(300);
}

void screen_window::tell_size()
{
	const qreal  scale = devicePixelRatioF();
	const guint  w = (guint)(width() * scale), h = (guint)(height() * scale);
	const qreal  dpi = logicalDpiX() > 0 ? logicalDpiX() : 96.0;

	if (link == nullptr || w == 0u || h == 0u)
		return;
	g_dbus_proxy_call(link->console, "SetUIInfo",
	                  g_variant_new("(qqiiuu)", (guint16)(width() * 25.4 / dpi),
	                                (guint16)(height() * 25.4 / dpi), 0, 0, w, h),
	                  G_DBUS_CALL_FLAGS_NONE, -1, link->cancel, nullptr, nullptr);
}

/* A 3D machine's frames, by GL over the window; a 2D frame hides it. */
void screen_window::scanout_dmabuf(int fd, int width, int height, int stride, unsigned fourcc,
                                   quint64 modifier, bool y0_top)
{
	if (view == nullptr) {
		view = new dmabuf_view(this);
		view->setGeometry(rect());
	}
	view->scanout(fd, width, height, stride, fourcc, modifier, y0_top);
	view->show();
}

void screen_window::update_dmabuf(std::function<void()> done)
{
	if (view == nullptr || view->isHidden()) {
		done();
		return;
	}
	view->changed(std::move(done));
}

/* What the guest's pixels are: the GL frame's where it shows, else the
 * shared or copied image's. */
QSize screen_window::source_size() const
{
	return view != nullptr && !view->isHidden() ? view->frame_size() : frame.size();
}

void screen_window::disable()
{
	frame = QImage();
	update();
}

void screen_window::lost(const QString &why)
{
	setWindowTitle(QStringLiteral("%1 -- %2").arg(id, why));
}

/* Where the frame is painted: the window, its aspect kept. */
QRect screen_window::shown() const
{
	const QSize source = source_size();

	if (source.isEmpty())
		return rect();
	QSize size = source.scaled(this->size(), Qt::KeepAspectRatio);

	return QRect((width() - size.width()) / 2, (height() - size.height()) / 2, size.width(), size.height());
}

void screen_window::paintEvent(QPaintEvent *event)
{
	QPainter painter(this);
	const QRect at = shown();

	painter.fillRect(event->rect(), Qt::black);
	if (!frame.isNull()) {
		painter.setRenderHint(QPainter::SmoothPixmapTransform, at.size() != frame.size());
		painter.drawImage(at, frame);
	}
}

/* Linux's evdev code to qemu's: the same below 89, and the extended keys
 * 0x80 with the scancode after their 0xe0 prefix. */
static unsigned qemu_key(unsigned evdev)
{
	static const struct { unsigned short evdev, qemu; } extended[] = {
		{ 96, 0x9c },  { 97, 0x9d },  { 98, 0xb5 },  { 99, 0xb7 },  { 100, 0xb8 },
		{ 102, 0xc7 }, { 103, 0xc8 }, { 104, 0xc9 }, { 105, 0xcb }, { 106, 0xcd },
		{ 107, 0xcf }, { 108, 0xd0 }, { 109, 0xd1 }, { 110, 0xd2 }, { 111, 0xd3 },
		{ 119, 0xc6 }, { 125, 0xdb }, { 126, 0xdc }, { 127, 0xdd },
	};

	if (evdev > 0u && evdev < 89u)
		return evdev;
	for (const auto &e : extended)
		if (e.evdev == evdev)
			return e.qemu;
	return 0u;
}

void screen_window::key(QKeyEvent *event, bool down)
{
	unsigned code;

	/* Ctrl+Alt+Enter: fullscreen, VMware's chord, not the guest's. */
	if (down && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
	    (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) ==
	        (Qt::ControlModifier | Qt::AltModifier)) {
		setWindowState(windowState() ^ Qt::WindowFullScreen);
		return;
	}
	/* xcb's and Wayland's codes are evdev's plus 8. */
	code = event->nativeScanCode() > 8u ? qemu_key(event->nativeScanCode() - 8u) : 0u;
	if (link == nullptr || code == 0u)
		return;
	g_dbus_proxy_call(link->keyboard, down ? "Press" : "Release", g_variant_new("(u)", code),
	                  G_DBUS_CALL_FLAGS_NONE, -1, link->cancel, nullptr, nullptr);
}

void screen_window::keyPressEvent(QKeyEvent *event)
{
	key(event, true);
}

void screen_window::keyReleaseEvent(QKeyEvent *event)
{
	if (!event->isAutoRepeat())
		key(event, false);
}

/* Tab is the guest's, not focus's. */
bool screen_window::focusNextPrevChild(bool)
{
	return false;
}

void screen_window::mouseMoveEvent(QMouseEvent *event)
{
	const QRect at = shown();
	GVariant   *absolute;
	QPoint      p = event->position().toPoint() - at.topLeft();

	const QSize source = source_size();

	if (link == nullptr || source.isEmpty() || at.width() == 0 || at.height() == 0)
		return;
	absolute = g_dbus_proxy_get_cached_property(link->mouse, "IsAbsolute");
	if (absolute != nullptr && g_variant_get_boolean(absolute)) {
		int x = qBound(0, p.x() * source.width() / at.width(), source.width() - 1);
		int y = qBound(0, p.y() * source.height() / at.height(), source.height() - 1);

		g_dbus_proxy_call(link->mouse, "SetAbsPosition", g_variant_new("(uu)", (guint32)x, (guint32)y),
		                  G_DBUS_CALL_FLAGS_NONE, -1, link->cancel, nullptr, nullptr);
	}
	if (absolute != nullptr)
		g_variant_unref(absolute);
}

void screen_window::button(QMouseEvent *event, bool down)
{
	guint32 which;

	switch (event->button()) {
	case Qt::LeftButton:    which = 0u; break;
	case Qt::MiddleButton:  which = 1u; break;
	case Qt::RightButton:   which = 2u; break;
	case Qt::BackButton:    which = 5u; break;
	case Qt::ForwardButton: which = 6u; break;
	default:                return;
	}
	if (link != nullptr)
		g_dbus_proxy_call(link->mouse, down ? "Press" : "Release", g_variant_new("(u)", which),
		                  G_DBUS_CALL_FLAGS_NONE, -1, link->cancel, nullptr, nullptr);
}

void screen_window::mousePressEvent(QMouseEvent *event)
{
	button(event, true);
}

void screen_window::mouseReleaseEvent(QMouseEvent *event)
{
	button(event, false);
}

void screen_window::wheelEvent(QWheelEvent *event)
{
	const guint32 which = event->angleDelta().y() > 0 ? 3u : 4u;

	if (link == nullptr || event->angleDelta().y() == 0)
		return;
	g_dbus_proxy_call(link->mouse, "Press", g_variant_new("(u)", which), G_DBUS_CALL_FLAGS_NONE, -1,
	                  link->cancel, nullptr, nullptr);
	g_dbus_proxy_call(link->mouse, "Release", g_variant_new("(u)", which), G_DBUS_CALL_FLAGS_NONE, -1,
	                  link->cancel, nullptr, nullptr);
}
