#!/usr/bin/env python3
# rfb.py -- a machine's screen and keyboard over VNC (RFC 6143), for a
# system that has nothing else to say: python3 test/rfb.py ADDRESS PORT
# [--key NAME]... [--type TEXT] [--wait SECONDS] [--png FILE] [--colours].
# Presses the named keys, types TEXT, a newline being Enter, waits, and
# writes one frame as a PNG or says how many colours it holds. Stops after
# that frame; every read is under a socket timeout, so a server that goes
# quiet ends it.

import argparse
import socket
import struct
import sys
import time
import zlib

KEY_RETURN = 0xff0d
NAMED = {"up": 0xff52, "down": 0xff54, "left": 0xff51, "right": 0xff53,
	 "escape": 0xff1b, "tab": 0xff09, "return": KEY_RETURN}


def exactly(sock, count):
	data = b""
	while len(data) < count:
		part = sock.recv(count - len(data))
		if not part:
			raise SystemExit("rfb: the server closed the connection")
		data += part
	return data


def handshake(sock):
	if not exactly(sock, 12).startswith(b"RFB 003."):
		raise SystemExit("rfb: not an RFB server")
	sock.sendall(b"RFB 003.008\n")
	count = exactly(sock, 1)[0]
	if 1 not in exactly(sock, count):
		raise SystemExit("rfb: the server asks for a password")
	sock.sendall(b"\x01")
	if struct.unpack(">I", exactly(sock, 4))[0] != 0:
		raise SystemExit("rfb: the server refused")
	sock.sendall(b"\x01")
	width, height = struct.unpack(">HH", exactly(sock, 4))
	exactly(sock, 16)
	exactly(sock, struct.unpack(">I", exactly(sock, 4))[0])
	# 32 bits, true colour, little-endian, red at bit 16: BGRX on the wire.
	sock.sendall(b"\x00\0\0\0" + struct.pack(">BBBBHHHBBB3x", 32, 24, 0, 1,
						 255, 255, 255, 16, 8, 0))
	sock.sendall(struct.pack(">BxHi", 2, 1, 0))
	return width, height


def key(sock, symbol):
	sock.sendall(struct.pack(">BBxxI", 4, 1, symbol))
	sock.sendall(struct.pack(">BBxxI", 4, 0, symbol))
	time.sleep(0.05)


def type_text(sock, text):
	for character in text:
		key(sock, KEY_RETURN if character == "\n" else ord(character))


def frame(sock, width, height):
	sock.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, width, height))
	screen = bytearray(width * height * 3)
	while True:
		kind = exactly(sock, 1)[0]
		if kind == 2:	# a bell
			continue
		if kind == 1:	# colours we did not ask for
			exactly(sock, 3)
			exactly(sock, 6 * struct.unpack(">H", exactly(sock, 2))[0])
			continue
		if kind == 3:	# cut text
			exactly(sock, 3)
			exactly(sock, struct.unpack(">I", exactly(sock, 4))[0])
			continue
		if kind != 0:
			raise SystemExit("rfb: message %d not understood" % kind)
		exactly(sock, 1)
		rectangles = struct.unpack(">H", exactly(sock, 2))[0]
		for _ in range(rectangles):
			x, y, w, h, encoding = struct.unpack(">HHHHi", exactly(sock, 12))
			if encoding != 0:
				raise SystemExit("rfb: encoding %d not asked for" % encoding)
			pixels = exactly(sock, w * h * 4)
			for row in range(h):
				if y + row >= height:
					break
				at = ((y + row) * width + x) * 3
				for column in range(min(w, width - x)):
					blue, green, red = pixels[(row * w + column) * 4:
								  (row * w + column) * 4 + 3]
					screen[at:at + 3] = bytes((red, green, blue))
					at += 3
		return screen


def png(path, width, height, screen):
	def chunk(kind, data):
		body = kind + data
		return (struct.pack(">I", len(data)) + body +
			struct.pack(">I", zlib.crc32(body)))
	raw = b"".join(b"\0" + bytes(screen[row * width * 3:(row + 1) * width * 3])
		       for row in range(height))
	with open(path, "wb") as out:
		out.write(b"\x89PNG\r\n\x1a\n" +
			  chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
			  chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("address")
	parser.add_argument("port", type=int)
	parser.add_argument("--key", action="append", default=[], choices=NAMED)
	parser.add_argument("--type", default="")
	parser.add_argument("--wait", type=float, default=0)
	parser.add_argument("--png")
	parser.add_argument("--colours", action="store_true")
	options = parser.parse_args()
	sock = socket.create_connection((options.address, options.port), timeout=20)
	width, height = handshake(sock)
	for name in options.key:
		key(sock, NAMED[name])
	type_text(sock, options.type.encode().decode("unicode_escape"))
	time.sleep(options.wait)
	screen = frame(sock, width, height) if options.png or options.colours else b""
	if options.png:
		png(options.png, width, height, screen)
	if options.colours:
		print("colours %d" % len({bytes(screen[at:at + 3])
					  for at in range(0, len(screen), 3)}))
	else:
		print("%dx%d" % (width, height))


if __name__ == "__main__":
	sys.exit(main())
