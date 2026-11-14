"""mp_wifi.py -- MicroPython on an ESP32 board under hull, its WiFi joined
(test/board_test.sh): over `hull exec -t ID -- console`, as a person at
its serial port would, the station started, PICSimLabWifi -- the open
access point hull's patched qemu models -- joined, and its address asked.
Prints what the REPL said. Usage: mp_wifi.py HULL ID. Bounded: every wait
ends, and the console is closed at the end."""
import os
import pty
import select
import subprocess
import sys
import time

hull, machine = sys.argv[1], sys.argv[2]
primary, secondary = pty.openpty()
child = subprocess.Popen([hull, "exec", "-t", machine, "--", "console"],
                         stdin=secondary, stdout=secondary, stderr=secondary)
os.close(secondary)
said = b""


def read(seconds):
	global said
	end = time.monotonic() + seconds
	while time.monotonic() < end:
		ready, _, _ = select.select([primary], [], [], 0.2)
		if ready:
			try:
				said += os.read(primary, 4096)
			except OSError:
				return


def type_line(text, seconds):
	for byte in (text + "\r").encode():
		os.write(primary, bytes([byte]))
		time.sleep(0.005)
	read(seconds)


read(1)
type_line("", 1)
type_line("import network", 1)
type_line("w = network.WLAN(network.STA_IF); w.active(True)", 6)
type_line("w.connect('PICSimLabWifi', '')", 1)
for _ in range(30):
	type_line("print('wifi', w.isconnected(), w.ifconfig()[0])", 1)
	if b"wifi True" in said:
		break
child.terminate()
try:
	child.wait(5)
except subprocess.TimeoutExpired:
	child.kill()
sys.stdout.write(said.decode(errors="replace").replace("\r", ""))
