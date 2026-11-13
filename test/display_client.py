"""display_client.py -- a client of a machine's local screen, as hull-gui
is one (project.md section 5.2): qemu's D-Bus display, no bus, reached by
the QMP monitor linked as $XDG_RUNTIME_DIR/hull/display/ADDRESS. One end
of a socket pair is handed to qemu by getfd and add_client, D-Bus is
spoken on the other, a listener is registered on the first console, and
its first frame awaited.

Usage: display_client.py LINK SECONDS. Prints `console WIDTHxHEIGHT`, then
`frame WIDTHxHEIGHT` for the first Scanout, then `dmabuf FOURCC` where it
was a GPU's buffer (a 3D machine's), else `lit N` -- how many of
its bytes are not zero -- and exits 0; exits 1, saying why, otherwise.
Bounded: every wait ends at SECONDS."""
import socket
import sys

import gi

gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib  # noqa: E402

LISTENER = '''<node><interface name="org.qemu.Display1.Listener">
<method name="Scanout"><arg type="u" direction="in"/><arg type="u" direction="in"/>
<arg type="u" direction="in"/><arg type="u" direction="in"/><arg type="ay" direction="in"/></method>
<method name="Update"><arg type="i" direction="in"/><arg type="i" direction="in"/>
<arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="in"/>
<arg type="u" direction="in"/><arg type="ay" direction="in"/></method>
<method name="ScanoutDMABUF"><arg type="h" direction="in"/><arg type="u" direction="in"/>
<arg type="u" direction="in"/><arg type="u" direction="in"/><arg type="u" direction="in"/>
<arg type="t" direction="in"/><arg type="b" direction="in"/></method>
<method name="UpdateDMABUF"><arg type="i" direction="in"/><arg type="i" direction="in"/>
<arg type="i" direction="in"/><arg type="i" direction="in"/></method>
<method name="Disable"/>
<method name="MouseSet"><arg type="i" direction="in"/><arg type="i" direction="in"/>
<arg type="i" direction="in"/></method>
<method name="CursorDefine"><arg type="i" direction="in"/><arg type="i" direction="in"/>
<arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="ay" direction="in"/></method>
<property name="Interfaces" type="as" access="read"/>
</interface></node>'''


def qmp(monitor, command, fds=()):
	"""A command said, its answer's line returned; events pass by."""
	data = (command + '\n').encode()
	if fds:
		socket.send_fds(monitor, [data], list(fds))
	else:
		monitor.sendall(data)
	while True:
		line = b''
		while not line.endswith(b'\n'):
			chunk = monitor.recv(1)
			if not chunk:
				raise SystemExit('display: the monitor closed')
			line += chunk
		if line.startswith(b'{"return"') or line.startswith(b'{"error"'):
			return line.decode()


def main():
	link, seconds = sys.argv[1], float(sys.argv[2])
	monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
	monitor.settimeout(seconds)
	monitor.connect(link)
	qmp(monitor, '{"execute":"qmp_capabilities"}')
	ours, theirs = socket.socketpair()
	answer = qmp(monitor, '{"execute":"getfd","arguments":{"fdname":"hullgui"}}', [theirs.fileno()])
	theirs.close()
	if '"return"' not in answer:
		raise SystemExit('display: getfd: ' + answer)
	answer = qmp(monitor, '{"execute":"add_client","arguments":'
	             '{"protocol":"@dbus-display","fdname":"hullgui"}}')
	if '"return"' not in answer:
		raise SystemExit('display: add_client: ' + answer)

	# qemu is in a user namespace, where our uid is not ours: an empty
	# EXTERNAL identity lets it take the socket's credentials instead.
	stream = Gio.Socket.new_from_fd(ours.detach()).connection_factory_create_connection()
	bus = Gio.DBusConnection.new_sync(stream, None,
	                                  Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
	                                  Gio.DBusConnectionFlags.CROSS_NAMESPACE, None, None)
	console = Gio.DBusProxy.new_sync(bus, Gio.DBusProxyFlags.NONE, None, None,
	                                 '/org/qemu/Display1/Console_0', 'org.qemu.Display1.Console',
	                                 None)
	width = console.get_cached_property('Width')
	height = console.get_cached_property('Height')
	if width is None or height is None:
		raise SystemExit('display: the first console has no size')
	print('console %ux%u' % (width.unpack(), height.unpack()), flush=True)

	loop = GLib.MainLoop()
	got = {}

	def called(conn, sender, path, interface, method, params, invocation):
		if method == 'ScanoutDMABUF' and 'frame' not in got:
			_handle, w, h, _stride, fourcc, _modifier, _top = params.unpack()
			got['frame'] = (w, h, None, fourcc)
			loop.quit()
		if method == 'Scanout' and 'frame' not in got:
			# The pixels as one buffer, looked at by bytes' own C: a
			# frame is megabytes, which a loop in Python takes longer
			# over than the waits allow, blocking the loop they run on.
			w, h, stride, _format = (params.get_child_value(i).unpack() for i in range(4))
			data = params.get_child_value(4).get_data_as_bytes().get_data()[:stride * h]
			got['frame'] = (w, h, len(data) - data.count(0))
			loop.quit()
		invocation.return_value(None)

	def interfaces(conn, sender, path, interface, name):
		return GLib.Variant('as', [])

	mine, qemus = socket.socketpair()
	fds = Gio.UnixFDList.new_from_array([qemus.fileno()])
	console.call_with_unix_fd_list('RegisterListener', GLib.Variant('(h)', (0,)),
	                               Gio.DBusCallFlags.NONE, int(seconds * 1000), fds, None,
	                               lambda proxy, result: proxy.call_with_unix_fd_list_finish(result))
	listener_stream = Gio.Socket.new_from_fd(
	    mine.detach()).connection_factory_create_connection()
	listener = Gio.DBusConnection.new_sync(
	    listener_stream, None,
	    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.CROSS_NAMESPACE |
	    Gio.DBusConnectionFlags.DELAY_MESSAGE_PROCESSING, None, None)
	qemus.detach()  # the fd list owns it now
	node = Gio.DBusNodeInfo.new_for_xml(LISTENER)
	listener.register_object('/org/qemu/Display1/Listener', node.interfaces[0], called,
	                         interfaces, None)
	listener.start_message_processing()
	GLib.timeout_add(int(seconds * 1000), loop.quit)
	loop.run()
	if 'frame' not in got:
		raise SystemExit('display: no frame within %g s' % seconds)
	print('frame %ux%u' % got['frame'][:2])
	if got['frame'][2] is None:
		fourcc = got['frame'][3].to_bytes(4, 'little').decode('ascii', 'replace')
		print('dmabuf %s' % fourcc)
	else:
		print('lit %u' % got['frame'][2])


main()
