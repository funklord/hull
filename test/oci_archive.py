#!/usr/bin/env python3
"""oci_archive.py -- image archives for test/build_test.sh's hull load.

    oci_archive.py docker OUT FILE   docker save's form, one layer holding
                                     FILE, tagged test/saved:1
    oci_archive.py oci OUT FILE      an OCI layout's, under an index of two
                                     platforms, this host's among them

Written here rather than by docker, which `make check` does not need.
"""

import hashlib
import io
import json
import sys
import tarfile


def add(tar, name, data):
	info = tarfile.TarInfo(name)
	info.size = len(data)
	tar.addfile(info, io.BytesIO(data))


def sha(data):
	return hashlib.sha256(data).hexdigest()


def main():
	form, out, path = sys.argv[1:4]
	with open(path, 'rb') as f:
		content = f.read()
	layer = io.BytesIO()
	with tarfile.open(fileobj=layer, mode='w', format=tarfile.USTAR_FORMAT) as t:
		add(t, 'greeting', content)
	layer = layer.getvalue()
	config = json.dumps({
		'architecture': 'amd64', 'os': 'linux',
		'config': {'Cmd': ['/greeting'], 'Env': ['SAVED=1'], 'WorkingDir': '/'},
		'rootfs': {'type': 'layers', 'diff_ids': ['sha256:' + sha(layer)]},
	}).encode()
	with tarfile.open(out, 'w', format=tarfile.USTAR_FORMAT) as tar:
		add(tar, 'blobs/sha256/' + sha(config), config)
		add(tar, 'blobs/sha256/' + sha(layer), layer)
		if form == 'docker':
			add(tar, 'manifest.json', json.dumps([{
				'Config': 'blobs/sha256/' + sha(config),
				'RepoTags': ['test/saved:1'],
				'Layers': ['blobs/sha256/' + sha(layer)],
			}]).encode())
			return
		manifest = json.dumps({
			'schemaVersion': 2,
			'config': {'digest': 'sha256:' + sha(config), 'size': len(config)},
			'layers': [{'digest': 'sha256:' + sha(layer), 'size': len(layer)}],
		}).encode()
		add(tar, 'blobs/sha256/' + sha(manifest), manifest)
		add(tar, 'oci-layout', b'{"imageLayoutVersion":"1.0.0"}')
		add(tar, 'index.json', json.dumps({'schemaVersion': 2, 'manifests': [
			{'digest': 'sha256:' + '0' * 64, 'platform': {'os': 'linux', 'architecture': 'riscv64'}},
			{'digest': 'sha256:' + sha(manifest),
			 'platform': {'os': 'linux', 'architecture': 'amd64'}},
		]}).encode())


main()
