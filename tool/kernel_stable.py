#!/usr/bin/env python3
"""kernel_stable.py -- is hull's own kernel the newest of its longterm series?

    kernel_stable.py VERSION SHA256

VERSION and SHA256 are the Makefile's KERNEL_VERSION and KERNEL_SHA256.
Reads kernel.org's releases.json for the newest release of VERSION's
series (6.12 for 6.12.107), and the sha256sums.asc of its directory,
whose signature is checked by gpgv against kernel/autosigner.asc, the
Kernel.org checksum autosigner's key, by its fingerprint. Then:

    exit 0  the pin is the newest, its sha256 the one kernel.org signs
    exit 1  a newer release, whose two Makefile lines are printed, or a
            pin whose sha256 is not the signed one, or a series at its end
    exit 2  kernel.org not reached, or its signature not good

What it does not do is move the pin: a newer stable is a build and a run
of the suites, which `make kernel` and `make test` are, by whoever moves
it (project.md, Appendix F's CVE response).
"""

import json
import os
import subprocess
import sys
import tempfile
import urllib.request

FINGERPRINT = 'B8868C80BA62A1FFFAF5FDA9632D3A06589DA6B1'
KEY = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'kernel', 'autosigner.asc')


def fetch(url):
	request = urllib.request.Request(url, headers={'User-Agent': 'hull'})
	with urllib.request.urlopen(request, timeout=60) as response:
		return response.read()


def signed_sums(major, scratch):
	text = fetch('https://cdn.kernel.org/pub/linux/kernel/v%s.x/sha256sums.asc' % major)
	sums = os.path.join(scratch, 'sha256sums.asc')
	with open(sums, 'wb') as out:
		out.write(text)
	keyring = os.path.join(scratch, 'autosigner.gpg')
	with open(KEY, 'rb') as key, open(keyring, 'wb') as out:
		subprocess.run(['gpg', '--batch', '--dearmor'], stdin=key, stdout=out, check=True)
	status = subprocess.run(['gpgv', '--status-fd', '1', '--keyring', keyring, sums],
	                        capture_output=True, text=True)
	if status.returncode != 0 or ('[GNUPG:] VALIDSIG ' + FINGERPRINT) not in status.stdout:
		sys.stderr.write(status.stderr)
		print('kernel: sha256sums.asc is not signed by the autosigner %s' % FINGERPRINT)
		sys.exit(2)
	listed = {}
	for line in text.decode().splitlines():
		parts = line.split()
		if len(parts) == 2 and len(parts[0]) == 64:
			listed[parts[1]] = parts[0]
	return listed


def main():
	if len(sys.argv) != 3:
		sys.exit('usage: kernel_stable.py VERSION SHA256')
	version, pinned = sys.argv[1], sys.argv[2]
	series = '.'.join(version.split('.')[:2])
	try:
		releases = json.loads(fetch('https://www.kernel.org/releases.json'))['releases']
		newest = [r for r in releases if r['moniker'] in ('longterm', 'stable')
		          and r['version'].startswith(series + '.')]
		with tempfile.TemporaryDirectory() as scratch:
			listed = signed_sums(version.split('.')[0], scratch)
	except (OSError, ValueError, KeyError) as error:
		print('kernel: kernel.org not read: %s' % error)
		sys.exit(2)
	status = 0
	signed = listed.get('linux-%s.tar.xz' % version)
	if signed != pinned:
		print('kernel: the pin of %s is not the sha256 kernel.org signs (%s)' % (version, signed))
		status = 1
	if not newest:
		print('kernel: kernel.org lists no %s any more: the series has ended, move to another' % series)
		return 1
	release = newest[0]
	if release.get('iseol'):
		print('kernel: %s is at its end of life at %s' % (series, release['version']))
		status = 1
	if release['version'] != version:
		latest = listed.get('linux-%s.tar.xz' % release['version'])
		if latest is None:
			print('kernel: %s is out, its sha256 not yet signed' % release['version'])
			return 1
		print('kernel: %s is behind %s, released %s:' % (version, release['version'],
		                                                 release['released']['isodate']))
		print('KERNEL_VERSION = %s' % release['version'])
		print('KERNEL_SHA256  = %s' % latest)
		return 1
	if status == 0:
		print('kernel: %s is the newest %s, its sha256 the one kernel.org signs' % (version, series))
	return status


if __name__ == '__main__':
	sys.exit(main())
