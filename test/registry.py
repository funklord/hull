#!/usr/bin/env python3
"""registry.py -- an OCI registry of the test's own, for test/pull_test.sh.

    registry.py build DIR PROBE ARCH   write the images into DIR/blobs, DIR/repos
    registry.py serve DIR CERT KEY     serve them on 127.0.0.1, its port to DIR/port

It speaks enough of the distribution spec to be pulled from, and as a real
registry does: a manifest wants a bearer token, which /token gives for the
scope asked; a blob is redirected to localhost, another host by name, which
refuses a request that carries the token; and a layer is sent in chunks,
the rest by length. It serves for 300 seconds at most.
"""

import gzip
import hashlib
import io
import json
import os
import ssl
import sys
import tarfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOKEN = "test-token"
# test/private's token is given only for these credentials; test/basic
# asks for them itself, by Basic.
PRIVATE_TOKEN = "private-token"
CREDENTIALS = "Basic dGVzdGVyOnNlY3JldA=="  # tester:secret


def digest(data):
	return "sha256:" + hashlib.sha256(data).hexdigest()


def tar_of(entries, fmt=tarfile.PAX_FORMAT):
	"""A tar of (TarInfo, bytes or None) pairs, in order."""
	out = io.BytesIO()
	with tarfile.open(fileobj=out, mode="w", format=fmt) as t:
		for info, data in entries:
			info.mtime = 0
			if data is not None:
				info.size = len(data)
				t.addfile(info, io.BytesIO(data))
			else:
				t.addfile(info)
	return out.getvalue()


def node(name, kind=tarfile.REGTYPE, mode=0o644, link="", pax=None):
	info = tarfile.TarInfo(name)
	info.type = kind
	info.mode = mode
	info.linkname = link
	info.uid = info.gid = 0
	if pax:
		info.pax_headers = pax
	return info


def build(root, probe, arch):
	blobs = os.path.join(root, "blobs")
	repos = os.path.join(root, "repos")
	os.makedirs(blobs, exist_ok=True)
	os.makedirs(repos, exist_ok=True)

	def put(data):
		name = digest(data)
		with open(os.path.join(blobs, name), "wb") as f:
			f.write(data)
		return name

	def manifest(repo, tag, layers, config_arch=arch, index=None, include=True, defaults=None):
		config = json.dumps({"architecture": config_arch, "os": "linux",
		                     "config": defaults or {"Cmd": ["/probe"]},
		                     "rootfs": {"type": "layers", "diff_ids": []}}).encode()
		body = json.dumps({
			"schemaVersion": 2,
			"mediaType": "application/vnd.oci.image.manifest.v1+json",
			"config": {"mediaType": "application/vnd.oci.image.config.v1+json",
			           "digest": put(config), "size": len(config)},
			"layers": [{"mediaType": layer[0], "digest": put(layer[1]),
			            "size": layer[2] if len(layer) > 2 else len(layer[1])}
			           for layer in layers]}, indent=1).encode()
		name = put(body)
		top = body
		if index is not None:
			top = json.dumps({
				"schemaVersion": 2,
				"mediaType": "application/vnd.oci.image.index.v1+json",
				"manifests": [{"mediaType": "application/vnd.oci.image.manifest.v1+json",
				               "digest": "sha256:" + "0" * 64, "size": 1,
				               "platform": {"architecture": other, "os": "linux"}}
				              for other in index] +
				             ([{"mediaType": "application/vnd.oci.image.manifest.v1+json",
				                "digest": name, "size": len(body),
				                "platform": {"architecture": config_arch, "os": "linux"}}]
				              if include else [])}).encode()
			put(top)
		path = os.path.join(repos, repo, "tags")
		os.makedirs(path, exist_ok=True)
		with open(os.path.join(path, tag), "wb") as f:
			f.write(top)
		return name

	probe_bytes = open(probe, "rb").read()
	gz = "application/vnd.oci.image.layer.v1.tar+gzip"
	plain = "application/vnd.oci.image.layer.v1.tar"
	long_name = "deep/" + "d" * 120 + "/file"
	first = tar_of([
		(node("probe", mode=0o755), probe_bytes),
		(node("etc", tarfile.DIRTYPE, 0o755), None),
		(node("etc/greeting"), b"hello\n"),
		(node("etc/noted", pax={"SCHILY.xattr.user.note": "a note"}), b"noted\n"),
		(node("etc/again", tarfile.LNKTYPE, link="etc/greeting"), None),
		(node("etc/gone"), b"gone\n"),
		(node("opaque", tarfile.DIRTYPE, 0o755), None),
		(node("opaque/old"), b"old\n"),
		(node("usr/lib", tarfile.DIRTYPE, 0o755), None),
		(node("lib", tarfile.SYMTYPE, 0o777, "usr/lib"), None),
		(node("ro", tarfile.DIRTYPE, 0o555), None),
		(node("ro/inside"), b"inside\n"),
		(node(long_name), b"long\n"),
		(node("dev/null", tarfile.CHRTYPE, 0o666), None),
	])
	second = tar_of([
		(node("etc/.wh.gone"), b""),
		# Before the marker, and this layer's: the marker takes only what
		# lower layers made.
		(node("opaque/early"), b"early\n"),
		(node("opaque/.wh..wh..opq"), b""),
		(node("opaque/new"), b"new\n"),
		(node("lib/libx"), b"libx\n"),
		(node("ro/added"), b"added\n"),
		(node("etc/greeting", mode=0o600), b"replaced\n"),
		# An absolute symlink is the tree's: what is written through it
		# lands in the tree's root, not the host's.
		(node("abs", tarfile.SYMTYPE, 0o777, "/"), None),
		(node("abs/rooted"), b"rooted\n"),
	], tarfile.GNU_FORMAT)
	third = tar_of([(node("srv/plain"), b"plain\n")], tarfile.USTAR_FORMAT)
	manifest("test/image", "1", [(gz, gzip.compress(first, mtime=0)),
	                             (gz, gzip.compress(second, mtime=0)),
	                             (plain, third)],
	         index=["arm64" if arch != "arm64" else "amd64"],
	         # A.4's defaults, each in OCI's spelling: a name given twice
	         # its last value, and the maps' keys in no order.
	         defaults={"Entrypoint": ["/probe", "say"], "Cmd": ["from-config", "cwd", "env", "GREETING"],
	                   "Env": ["GREETING=first", "PATH=/bin", "GREETING=hi"], "WorkingDir": "/etc",
	                   "ExposedPorts": {"80/tcp": {}, "53/udp": {}}, "Volumes": {"/data": {}},
	                   "StopSignal": "SIGTERM", "User": "1000"})
	# What a pull refuses.
	good = gzip.compress(tar_of([(node("probe", mode=0o755), probe_bytes)]), mtime=0)
	# Its own layer: one shared with a later image would be written again
	# whole when that image is built.
	corrupt = gzip.compress(tar_of([(node("corrupt"), b"x" * 4096)]), mtime=0)
	bad = manifest("test/bad", "corrupt", [(gz, corrupt)])
	layer = json.loads(open(os.path.join(blobs, bad), "rb").read())["layers"][0]["digest"]
	with open(os.path.join(blobs, layer), "r+b") as f:
		middle = len(corrupt) // 2
		f.seek(middle)
		byte = f.read(1)
		f.seek(middle)
		f.write(bytes([byte[0] ^ 1]))
	outside = os.path.join(root, "outside")
	os.makedirs(outside, exist_ok=True)
	manifest("test/bad", "escape", [(gz, gzip.compress(tar_of([
		(node("up", tarfile.SYMTYPE, 0o777, outside), None),
		(node("up/written"), b"escaped\n"),
	]), mtime=0))])
	manifest("test/bad", "dotdot", [(gz, gzip.compress(tar_of([(node("a/../../dotdot"), b"x\n")]),
	                                                   mtime=0))])
	manifest("test/bad", "zstd", [("application/vnd.oci.image.layer.v1.tar+zstd", b"not read")])
	manifest("test/bad", "elsewhere", [(gz, good)], index=["s390x", "ppc64le"], include=False)
	# The image again, where credentials are wanted.
	for repo in ("test/private", "test/basic", "test/hopped"):
		os.makedirs(os.path.join(repos, repo, "tags"), exist_ok=True)
		with open(os.path.join(repos, "test/image", "tags", "1"), "rb") as f:
			image = f.read()
		with open(os.path.join(repos, repo, "tags", "1"), "wb") as f:
			f.write(image)
	manifest("test/bad", "foreign", [(gz, good)], config_arch="s390x")
	# An image of another arch alone, which `hull pull --arch` fetches
	# and a plain pull refuses: an index with that platform and no other.
	manifest("test/other", "1", [(gz, good)], config_arch="riscv64", index=[])
	# A layer longer than its manifest says.
	manifest("test/bad", "short", [(gz, good, len(good) - 1)])
	# An index whose manifest, fetched by its digest, is another manifest.
	real = manifest("test/bad", "real", [(gz, good)])
	lie = "sha256:" + "1" * 64
	with open(os.path.join(blobs, lie), "wb") as f:
		f.write(open(os.path.join(blobs, real), "rb").read())
	index = json.dumps({"schemaVersion": 2,
	                    "mediaType": "application/vnd.oci.image.index.v1+json",
	                    "manifests": [{"mediaType": "application/vnd.oci.image.manifest.v1+json",
	                                   "digest": lie, "size": 1,
	                                   "platform": {"architecture": arch, "os": "linux"}}]}).encode()
	os.makedirs(os.path.join(repos, "test/bad", "tags"), exist_ok=True)
	with open(os.path.join(repos, "test/bad", "tags", "lying"), "wb") as f:
		f.write(index)


class handler(BaseHTTPRequestHandler):
	protocol_version = "HTTP/1.1"

	def log_message(self, fmt, *args):
		sys.stderr.write("registry: " + (fmt % args) + "\n")

	def send(self, status, body=b"", headers=(), chunked=False):
		self.send_response(status)
		for name, value in headers:
			self.send_header(name, value)
		self.send_header("Connection", "close")
		if chunked:
			self.send_header("Transfer-Encoding", "chunked")
			self.end_headers()
			for at in range(0, len(body), 777):
				part = body[at:at + 777]
				self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
			self.wfile.write(b"0\r\n\r\n")
		else:
			self.send_header("Content-Length", str(len(body)))
			self.end_headers()
			self.wfile.write(body)
		self.close_connection = True

	def do_GET(self):
		root = self.server.root
		path, _, query = self.path.partition("?")
		port = self.server.server_address[1]
		if path == "/hop":
			# A realm that sends its asker on to another host.
			self.send(307, b"", [("Location", "https://localhost:%d/token?%s" % (port, query))])
			return
		if path == "/token" and self.headers.get("Host", "").startswith("localhost") and \
		   self.headers.get("Authorization") is not None:
			sys.stderr.write("registry: a token sent where it was not given\n")
			self.send(403, b"credentials sent to a host they were not for")
			return
		if path == "/token":
			ok = "scope=repository%3Atest%2F" in query and "service=test" in query
			if ok and "repository%3Atest%2Fprivate" in query:
				if self.headers.get("Authorization") != CREDENTIALS:
					self.send(401, b"")
					return
				self.send(200, json.dumps({"token": PRIVATE_TOKEN}).encode())
				return
			self.send(200 if ok else 400, json.dumps({"token": TOKEN}).encode() if ok else b"")
			return
		if path.startswith("/cdn/"):
			# Another host: the registry's token is not for it.
			if self.headers.get("Authorization") is not None:
				self.send(403, b"a token sent where it was not given")
				return
			name = path[len("/cdn/"):]
			try:
				data = open(os.path.join(root, "blobs", name), "rb").read()
			except OSError:
				self.send(404)
				return
			self.send(200, data, chunked=len(data) > 4096)
			return
		parts = path.split("/")
		if len(parts) < 5 or parts[1] != "v2":
			self.send(404)
			return
		if not self.authorized(parts):
			return
		repo, kind, reference = "/".join(parts[2:-2]), parts[-2], parts[-1]
		if kind == "blobs":
			self.send(307, b"", [("Location", "https://localhost:%d/cdn/%s" % (port, reference))])
			return
		if kind != "manifests":
			self.send(404)
			return
		if reference.startswith("sha256:"):
			name = os.path.join(root, "blobs", reference)
		else:
			name = os.path.join(root, "repos", repo, "tags", reference)
		try:
			data = open(name, "rb").read()
		except OSError:
			self.send(404)
			return
		media = json.loads(data).get("mediaType", "")
		self.send(200, data, [("Content-Type", media), ("Docker-Content-Digest", digest(data))])

	def authorized(self, parts):
		"""A request with the token, or a 401 asking for one, for push too
		where the request writes."""
		repo = "/".join(parts[2:-2]) if parts[-3] != "blobs" else "/".join(parts[2:-3])
		given = self.headers.get("Authorization")
		if repo == "test/basic":
			if given == CREDENTIALS:
				return True
			self.send(401, b"", [("WWW-Authenticate", 'Basic realm="test"')])
			return False
		if given == "Bearer " + (PRIVATE_TOKEN if repo.startswith("test/private") else TOKEN):
			return True
		scope = "pull,push" if self.command in ("POST", "PUT") else "pull"
		realm = "hop" if repo == "test/hopped" else "token"
		self.send(401, b"", [("WWW-Authenticate",
		                      'Bearer realm="https://127.0.0.1:%d/%s",service="test",'
		                      'scope="repository:%s:%s"' % (self.server.server_address[1], realm,
		                                                    repo, scope))])
		return False

	def body(self):
		return self.rfile.read(int(self.headers.get("Content-Length", "0")))

	def do_HEAD(self):
		parts = self.path.split("?")[0].split("/")
		if len(parts) < 5 or parts[1] != "v2" or parts[-2] != "blobs":
			self.send(404)
			return
		if not self.authorized(parts):
			return
		name = os.path.join(self.server.root, "blobs", parts[-1])
		self.send_response(200 if os.path.exists(name) else 404)
		self.send_header("Content-Length", str(os.path.getsize(name)) if os.path.exists(name) else "0")
		self.send_header("Connection", "close")
		self.end_headers()
		self.close_connection = True

	def do_POST(self):
		parts = self.path.split("?")[0].split("/")
		self.body()
		if len(parts) < 6 or parts[1] != "v2" or parts[-3:] != ["blobs", "uploads", ""]:
			self.send(404)
			return
		if not self.authorized(parts[:-1]):
			return
		repo = "/".join(parts[2:-3])
		self.server.uploads += 1
		self.send(202, b"", [("Location", "/v2/%s/blobs/uploads/u%d" % (repo, self.server.uploads))])

	def do_PUT(self):
		path, _, query = self.path.partition("?")
		parts = path.split("/")
		data = self.body()
		if len(parts) < 5 or parts[1] != "v2":
			self.send(404)
			return
		if parts[-3:-1] == ["blobs", "uploads"]:
			if not self.authorized(parts[:-1]):
				return
			given = dict(p.split("=", 1) for p in query.split("&") if "=" in p).get("digest", "")
			given = given.replace("%3A", ":").replace("%3a", ":")
			if given != digest(data):
				self.send(400, b"a blob that is not its digest")
				return
			with open(os.path.join(self.server.root, "blobs", given), "wb") as f:
				f.write(data)
			with open(os.path.join(self.server.root, "uploads"), "a") as f:
				f.write(given + "\n")
			self.send(201, b"", [("Location", "/v2/%s/blobs/%s" % ("/".join(parts[2:-3]), given))])
			return
		if parts[-2] != "manifests" or not self.authorized(parts):
			if parts[-2] != "manifests":
				self.send(404)
			return
		repo, tag = "/".join(parts[2:-2]), parts[-1]
		manifest = json.loads(data)
		for blob in [manifest["config"]] + manifest["layers"]:
			if not os.path.exists(os.path.join(self.server.root, "blobs", blob["digest"])):
				self.send(400, b"a manifest naming a blob not uploaded")
				return
		with open(os.path.join(self.server.root, "blobs", digest(data)), "wb") as f:
			f.write(data)
		os.makedirs(os.path.join(self.server.root, "repos", repo, "tags"), exist_ok=True)
		with open(os.path.join(self.server.root, "repos", repo, "tags", tag), "wb") as f:
			f.write(data)
		self.send(201, b"", [("Docker-Content-Digest", digest(data))])


def serve(root, cert, key):
	server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
	server.root = root
	server.uploads = 0
	context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
	context.load_cert_chain(cert, key)
	server.socket = context.wrap_socket(server.socket, server_side=True)
	with open(os.path.join(root, "port.new"), "w") as f:
		f.write("%d\n" % server.server_address[1])
	os.rename(os.path.join(root, "port.new"), os.path.join(root, "port"))
	threading.Thread(target=lambda: (time.sleep(300), server.shutdown()), daemon=True).start()
	server.serve_forever()


if __name__ == "__main__":
	if sys.argv[1] == "build":
		build(*sys.argv[2:5])
	else:
		serve(*sys.argv[2:5])
