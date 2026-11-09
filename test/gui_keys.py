# gui_keys.py -- hull-gui --tui on a pseudo-terminal of its own, sent keys
# and read back as text, for test/gui_test.sh.
#
#   python3 -I gui_keys.py GUI HULL WORKDIR OUT KEY... [-- FILE...]
#
# Each KEY is a Python bytes literal, sent three seconds after the last
# (four after the start), and the screen's text with its escapes taken out
# is written to OUT at the end. That text is the stream the window wrote,
# not the screen: a cell the terminal already shows is not written again,
# so a word can come out with letters missing. The run ends with Ctrl+L,
# which draws the screen whole once more, so what the screen shows at the
# end is there whole; a pattern for something shown only in between should
# match a part of it that is drawn once. FILEs are given to hull-gui to add. The
# window is killed at the end by its pid, and the whole run is bounded by
# the caller's timeout as well.

import ast
import fcntl
import os
import pty
import re
import select
import signal
import struct
import sys
import termios
import time

gui, hull, workdir, out = sys.argv[1:5]
rest = sys.argv[5:]
files = rest[rest.index('--') + 1:] if '--' in rest else []
keys = [ast.literal_eval(k) for k in (rest[:rest.index('--')] if '--' in rest else rest)]

pid, fd = pty.fork()
if pid == 0:
	os.chdir(workdir)
	os.environ['TERM'] = 'xterm-256color'
	os.environ.pop('DISPLAY', None)
	os.environ.pop('WAYLAND_DISPLAY', None)
	os.execv(gui, [gui, '--hull', hull, '--tui'] + files)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', 40, 160, 0, 0))
screen = b''


def pump(seconds):
	global screen
	end = time.time() + seconds
	while time.time() < end:
		ready, _, _ = select.select([fd], [], [], 0.1)
		if not ready:
			continue
		try:
			data = os.read(fd, 65536)
		except OSError:
			return
		if not data:
			return
		screen += data


pump(4)
for key in keys:
	os.write(fd, key)
	pump(3)
# Ctrl+L, qtty's redraw: the last frame drawn whole, so what is on the
# screen at the end is in the text whole, whatever was drawn before.
os.write(fd, b'\x0c')
pump(2)
with open(out, 'w') as f:
	f.write(re.sub(rb'\x1b\[[0-9;?]*[ -/]*[@-~]', b' ', screen).decode('utf-8', 'replace'))
os.kill(pid, signal.SIGKILL)
os.waitpid(pid, 0)
