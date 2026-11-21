#!/usr/bin/env python3
"""kernel_cves.py -- which kernel CVEs apply to hull's own kernel, as built

    kernel_cves.py VULNS VERSION TREE...

VULNS is a checkout of the kernel CNA's records (git.kernel.org's
linux/security/vulns.git), VERSION the Makefile's KERNEL_VERSION, and each
TREE a kernel source tree `make kernel` built in. A record applies where
its semver ranges leave VERSION affected -- the fix not in this stable,
or not backported to its series at all -- and one of its programFiles, a
.c file, has an object beside it in a TREE: compiled into this config.
Prints a line for each that applies, the files that make it so, then a
count of each kind:

    applies      affects VERSION, and a file of it is built here
    not built    affects VERSION, in files this config does not build
    headers      affects VERSION, naming only headers, which a tree's
                 objects cannot settle: read by hand
    fixed        VERSION is not affected, being fixed or too old

Exits 0; the list is the note project.md's Appendix F asks for, each
advisory's applies or does not.
"""

import json
import os
import sys

INFINITE = 10 ** 9


def key(version):
	"""A version as a tuple to compare, `*` past any number."""
	parts = []
	for part in version.split('.'):
		parts.append(INFINITE if part == '*' else int(part) if part.isdigit() else -1)
	return tuple(parts + [0] * (4 - len(parts)))


def affected(record, version):
	"""Whether `version` is affected, by the semver half of the record:
	an unaffected range holding it wins, an affected one next, the default
	status last. git ranges, by commit, are the other half's."""
	unaffected_hit = affected_hit = False
	for entry in record.get('versions', []):
		if entry.get('versionType') == 'git':
			continue
		low = key(entry['version'])
		if 'lessThan' in entry:
			inside = low <= version < key(entry['lessThan'])
		elif 'lessThanOrEqual' in entry:
			inside = low <= version <= key(entry['lessThanOrEqual'])
		else:
			inside = low == version
		if inside:
			if entry['status'] == 'unaffected':
				unaffected_hit = True
			else:
				affected_hit = True
	if unaffected_hit:
		return False
	return affected_hit or record.get('defaultStatus') == 'affected'


def main():
	if len(sys.argv) < 4:
		sys.exit('usage: kernel_cves.py VULNS VERSION TREE...')
	vulns, version, trees = sys.argv[1], key(sys.argv[2]), sys.argv[3:]
	counts = {'applies': 0, 'not built': 0, 'headers': 0, 'fixed': 0}
	published = os.path.join(vulns, 'cve', 'published')
	if not os.path.isdir(published):
		sys.exit('kernel_cves: no cve/published in %s' % vulns)
	lines = []
	for top, _, files in os.walk(published):
		for name in files:
			if not name.endswith('.json'):
				continue
			with open(os.path.join(top, name)) as record_file:
				record = json.load(record_file)
			semver = [a for a in record['containers']['cna'].get('affected', [])
			          if any(e.get('versionType') != 'git' for e in a.get('versions', []))]
			if not semver or not affected(semver[0], version):
				counts['fixed'] += 1
				continue
			sources = [p for p in semver[0].get('programFiles', []) if p.endswith('.c')]
			built = sorted({p for p in sources for tree in trees
			                if os.path.exists(os.path.join(tree, p[:-2] + '.o'))})
			if built:
				counts['applies'] += 1
				lines.append('%s %s' % (name[:-5], ' '.join(built)))
			elif sources:
				counts['not built'] += 1
			else:
				counts['headers'] += 1
	for line in sorted(lines):
		print(line)
	print('kernel: %s: %d apply, %d not built, %d naming headers only, %d fixed or older' %
	      (sys.argv[2], counts['applies'], counts['not built'], counts['headers'], counts['fixed']))
	return 0


if __name__ == '__main__':
	sys.exit(main())
