"""mp_wire.py -- two ESP32 boards' WiFi on one segment (test/board_test.sh):
MicroPython on each, over `hull exec -t ID -- console` as mp_wifi.py does,
its station joined to its own qemu's open access point, which is bridged
onto the segment, an address given by hand (a segment has no DHCP), and
then a datagram from the first to the second, which prints it. Prints what
both REPLs said, the second's last. Usage: mp_wire.py HULL ID1 ID2.
Bounded: every wait ends, and both consoles are closed at the end."""
import os
import pty
import select
import subprocess
import sys
import time

hull = sys.argv[1]


class console:
	def __init__(self, machine):
		self.primary, secondary = pty.openpty()
		self.child = subprocess.Popen([hull, "exec", "-t", machine, "--", "console"],
		                              stdin=secondary, stdout=secondary, stderr=secondary)
		os.close(secondary)
		self.said = b""

	def read(self, seconds):
		end = time.monotonic() + seconds
		while time.monotonic() < end:
			ready, _, _ = select.select([self.primary], [], [], 0.2)
			if ready:
				try:
					self.said += os.read(self.primary, 4096)
				except OSError:
					return

	def line(self, text, seconds):
		for byte in (text + "\r").encode():
			os.write(self.primary, bytes([byte]))
			time.sleep(0.005)
		self.read(seconds)

	def join(self, address):
		self.read(1)
		self.line("", 1)
		self.line("import network, socket", 1)
		self.line("w = network.WLAN(network.STA_IF); w.active(True)", 6)
		# The address before the join: the station is connected only once
		# it has one, and a segment has no DHCP to give it.
		self.line("w.ifconfig(('%s', '255.255.255.0', '10.77.0.254', '10.77.0.254'))" % address, 1)
		self.line("w.connect('PICSimLabWifi', '')", 1)
		for _ in range(30):
			self.line("print('joined', w.isconnected())", 1)
			if b"joined True" in self.said:
				break

	def close(self):
		self.child.terminate()
		try:
			self.child.wait(5)
		except subprocess.TimeoutExpired:
			self.child.kill()
		return self.said.decode(errors="replace").replace("\r", "")


first = console(sys.argv[2])
second = console(sys.argv[3])
first.join("10.77.0.1")
second.join("10.77.0.2")
second.line("s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(('0.0.0.0', 5005)); s.settimeout(2)", 1)
first.line("t = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)", 1)
for _ in range(10):
	first.line("t.sendto(b'over the air', ('10.77.0.2', 5005))", 1)
	# One line: the REPL indents a block itself. A timeout is a traceback.
	second.line("print('heard', s.recvfrom(64)[0])", 3)
	if b"heard b'over the air'" in second.said:
		break
sys.stdout.write(first.close())
sys.stdout.write("\n---\n")
sys.stdout.write(second.close())
