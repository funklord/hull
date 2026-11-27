#!/bin/sh
# pc_test.sh -- the pc class (project.md section 5.0): a whole PC whose
# system is its own, booted by its firmware from its media. Not part of
# `make check`, which needs no qemu: `make check-pc` runs it. Its arguments
# are build/hull and the static probe. The system booted is test/pc_boot.asm, a sector that says
# on COM1 where it came from, and for the last cases a Linux of
# `hull export --bootable`. It writes only under $BUILD_DIR/test/pc,
# removed at its start and left on failure, what it starts is under
# `timeout`, and the core is the test's own, whose idle clock is a second.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
probe=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
dir=${BUILD_DIR:?}/test/pc
case $dir in
*/test/pc) ;;
*) printf 'pc: refusing to clear %s\n' "$dir"; exit 1 ;;
esac
rm -rf "$dir"
mkdir -p "$dir/run" "$dir/cache"
chmod 700 "$dir/run"
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$dir" && pwd)
XDG_RUNTIME_DIR=$root/run
XDG_CACHE_HOME=$root/cache
XDG_DATA_HOME=$root/data
HULL_CORE_IDLE=1
export XDG_RUNTIME_DIR XDG_CACHE_HOME XDG_DATA_HOME HULL_CORE_IDLE
unset DISPLAY
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

# says NAME OUT WORD: OUT holds WORD.
says() {
	cases=$((cases + 1))
	printf '%s\n' "$2" | grep -qF "$3" || fail "$1" "$2"
}

# status NAME GOT WANTED OUT
status() {
	cases=$((cases + 1))
	[ "$2" = "$3" ] || fail "$1: exited $2, wanted $3" "$4"
}

# conf ENTRIES...: the machine of $root/hull.conf.
conf() {
	printf '(machine (backend pc) %s)\n' "$*" > "$root/hull.conf"
}

run() {
	out=$(timeout 120 "$hull" run -f "$root/hull.conf" "$@" 2>&1)
	got=$?
}

for tool in nasm qemu-system-x86_64 qemu-img; do
	if ! command -v "$tool" > /dev/null; then
		printf 'pc: %s is not installed\n' "$tool"
		exit 1
	fi
done
nasm -f bin -o "$root/boot.bin" "$here/pc_boot.asm" || exit 1

# image SIZE PATH: the sector and its next on a new image.
image() {
	truncate -s "$1" "$2"
	dd if="$root/boot.bin" of="$2" conv=notrunc status=none
}

# --- what the plan refuses ---------------------------------------------------

image 1M "$root/disk.img"
conf '(source "media:disk") (media (medium (type disk) (source "image:./disk.img")))'
out=$("$hull" plan -f "$root/hull.conf" --backend vm 2>&1)
got=$?
[ "$got" != 0 ] || fail 'a media: source is refused on vm' "$out"
cases=$((cases + 1))
says 'a media: source named as what vm lacks' "$out" 'source.media'

conf '(source "media:cdrom") (media (medium (type disk) (source "image:./disk.img")))'
run
status 'a drive with no medium is refused' "$got" 125 "$out"
says 'and said' "$out" 'no medium'

conf '(source "media:disk") (media (medium (type disk) (source "image:./gone.img")))'
run
status 'an image not there is refused' "$got" 125 "$out"

conf '(source "media:disk") (ports (port (host 7000) (guest 7000)))' \
	'(media (medium (type disk) (source "image:./disk.img")))'
run
status 'ports with no network are refused' "$got" 125 "$out"
says 'and said' "$out" '(network nat)'

conf '(source "media:disk") (display window)' \
	'(media (medium (type disk) (source "image:./disk.img")))'
run
status 'a window with no DISPLAY is refused' "$got" 125 "$out"

conf '(source "media:disk") (command "/bin/sh")' \
	'(media (medium (type disk) (source "image:./disk.img")))'
run
status 'a command is refused, no agent running it' "$got" 125 "$out"

# --- a disk: its writes kept or not ------------------------------------------

before=$(sha256sum < "$root/disk.img")
conf '(source "media:disk") (media (medium (type disk) (source "image:./disk.img")))'
run
status 'a disk boots' "$got" 0 "$out"
says 'and says so on its serial port' "$out" 'booted from a drive'
says 'its word fresh' "$out" 'word: fresh'
run
says 'an overlay forgets the write' "$out" 'word: fresh'
cases=$((cases + 1))
[ "$(sha256sum < "$root/disk.img")" = "$before" ] || fail 'an overlay left the image alone' ''

conf '(source "media:disk") (mutable direct)' \
	'(media (medium (type disk) (source "image:./disk.img")))'
run
says 'a direct disk boots fresh' "$out" 'word: fresh'
run
says 'and keeps the write' "$out" 'word: written'

# --- an attached image, as it came -------------------------------------------

image 1M "$root/raw.img"
qemu-img convert -f raw -O vmdk "$root/raw.img" "$root/old.vmdk"
before=$(sha256sum < "$root/old.vmdk")
conf '(source "image:./old.vmdk")'
run
status 'a vmdk attached boots' "$got" 0 "$out"
says 'its word read through the container' "$out" 'word: fresh'
cases=$((cases + 1))
[ "$(sha256sum < "$root/old.vmdk")" = "$before" ] || fail 'an attached overlay left it alone' ''

# --- another hypervisor's machine file, as it stands --------------------------

# A .vmx is read as a pc machine: its memory, processors and name, its
# disk where it attaches it -- here VMware's descriptor and flat extent,
# which qemu follows -- and what it cannot carry said. Its disk boots, and
# an overlay leaves the extent alone.
mkdir -p "$root/vmware"
qemu-img convert -f raw -O vmdk -o subformat=monolithicFlat "$root/raw.img" "$root/vmware/old.vmdk"
cat > "$root/vmware/Old Box.vmx" <<'EOF'
.encoding = "UTF-8"
displayName = "Old Box"
memsize = "256"
numvcpus = "2"
sata0.present = "TRUE"
sata0:0.present = "TRUE"
sata0:0.fileName = "old.vmdk"
sata0:1.present = "TRUE"
sata0:1.deviceType = "cdrom-raw"
sata0:1.fileName = "auto detect"
ethernet0.present = "TRUE"
ethernet0.virtualDev = "vmxnet3"
EOF
before=$(sha256sum < "$root/vmware/old-flat.vmdk")
out=$("$hull" plan --resolved -f "$root/vmware/Old Box.vmx" 2>&1)
says 'a .vmx is a pc machine of its name' "$out" '(hostname "old-box") (source "media:disk") (memory "256M") (cpus 2) (network nat)'
says 'its disk where it was' "$out" '(medium (type disk) (source "image:./old.vmdk"))'
says 'a machine of SATA is a q35' "$out" '(tuning (pc (machine q35) (cpu max) (nic vmxnet3)))'
sed 's/"vmxnet3"/"vmxnet"/' "$root/vmware/Old Box.vmx" > "$root/vmware/vmxnet.vmx"
says 'a card qemu lacks said' "$("$hull" plan --resolved -f "$root/vmware/vmxnet.vmx" 2>&1)" \
	'ethernet0, vmxnet, is given as an e1000'
rm -f "$root/vmware/vmxnet.vmx"
says 'a host drive said' "$out" "sata0:1, the host's own drive, is not given"
# Its sound and USB: VMware's default ES1371 is qemu's ES1370, and USB a
# tablet.
{ cat "$root/vmware/Old Box.vmx"; printf 'sound.present = "TRUE"\nusb.present = "TRUE"\n'; } \
	> "$root/vmware/sound.vmx"
out=$("$hull" plan --resolved -f "$root/vmware/sound.vmx" 2>&1)
says "VMware's sound card and a tablet for its USB" "$out" '(nic vmxnet3) (sound es1370) (pointer tablet)))'
out=$(timeout 120 "$hull" run -f "$root/vmware/Old Box.vmx" 2>&1)
got=$?
status 'a .vmx boots' "$got" 0 "$out"
says 'its disk through the descriptor' "$out" 'word: fresh'
cases=$((cases + 1))
[ "$(sha256sum < "$root/vmware/old-flat.vmdk")" = "$before" ] || fail 'a .vmx run left its disk alone' ''
# UEFI, Secure Boot and a vTPM carried as q35's.
cat > "$root/vmware/efi.vmx" <<'EOF'
firmware = "efi"
uefi.secureBoot.enabled = "TRUE"
vtpm.present = "TRUE"
nvme0.present = "TRUE"
nvme0:0.present = "TRUE"
nvme0:0.fileName = "old.vmdk"
EOF
out=$("$hull" plan --resolved -f "$root/vmware/efi.vmx" 2>&1)
says 'Secure Boot and a TPM carried' "$out" '(tuning (pc (machine q35) (cpu max) (firmware uefi-secure) (tpm true)))'
says 'and the TPM keys left behind said' "$out" "its TPM's keys stay with its own hypervisor"
printf 'displayName = "empty"\n' > "$root/vmware/empty.vmx"
out=$("$hull" plan -f "$root/vmware/empty.vmx" 2>&1)
got=$?
status 'a .vmx with no disk is refused' "$got" 2 "$out"
says 'and said' "$out" 'names no disk or disc image'

# A .vbox: its media registry's VDI where its IDE controller attaches it,
# an i440FX for a disk of that age; a snapshot's differencing image is
# refused, naming the way round.
mkdir -p "$root/vbox"
qemu-img convert -f raw -O vdi "$root/raw.img" "$root/vbox/old.vdi"
vbox() { # FILE DISK-ELEMENTS
	cat > "$root/vbox/$1" <<EOF
<?xml version="1.0"?>
<VirtualBox xmlns="http://www.virtualbox.org/" version="1.19-linux">
  <Machine uuid="{11111111-2222-3333-4444-555555555555}" name="Old &amp; VBox" OSType="Linux_64">
    <MediaRegistry>
      <HardDisks>
$2
      </HardDisks>
    </MediaRegistry>
    <Hardware>
      <CPU count="1"/>
      <Memory RAMSize="128"/>
      <Boot><Order position="1" device="HardDisk"/></Boot>
      <Network><Adapter slot="0" enabled="true" type="82540EM"><NAT/></Adapter></Network>
      <StorageControllers>
        <StorageController name="IDE" type="PIIX4" PortCount="2">
          <AttachedDevice type="HardDisk" port="0" device="0"><Image uuid="{aaaaaaaa-0000-0000-0000-00000000000$3}"/></AttachedDevice>
          <AttachedDevice passthrough="false" type="DVD" port="1" device="0"/>
        </StorageController>
      </StorageControllers>
    </Hardware>
  </Machine>
</VirtualBox>
EOF
}
vbox old.vbox '        <HardDisk uuid="{aaaaaaaa-0000-0000-0000-000000000001}" location="old.vdi" format="VDI" type="Normal"/>' 1
out=$("$hull" plan --resolved -f "$root/vbox/old.vbox" 2>&1)
says 'a .vbox is a pc machine of its name' "$out" '(hostname "old-vbox") (source "media:disk") (memory "128M") (network nat)'
says 'an IDE disk is an i440FX' "$out" '(tuning (pc (machine pc) (cpu max) (nic e1000)))'
out=$(timeout 120 "$hull" run -f "$root/vbox/old.vbox" 2>&1)
got=$?
status 'a .vbox boots' "$got" 0 "$out"
says 'its VDI' "$out" 'word: fresh'
vbox snap.vbox '        <HardDisk uuid="{aaaaaaaa-0000-0000-0000-000000000001}" location="old.vdi" format="VDI" type="Normal">
          <HardDisk uuid="{aaaaaaaa-0000-0000-0000-000000000002}" location="Snapshots/{b}.vdi" format="VDI"/>
        </HardDisk>' 2
out=$("$hull" plan -f "$root/vbox/snap.vbox" 2>&1)
got=$?
status 'a snapshot differencing disk is refused' "$got" 2 "$out"
says 'naming the way round' "$out" 'VBoxManage clonemedium'
# Its sound card and its USB: AC'97 where the AudioAdapter names no
# controller, VirtualBox's default and the chip qemu's ac97 is, and a USB
# controller a tablet, the pointer where the viewer's is (the holder's
# Windows XP, 2026-11-24); booted with both, the probe's disk as before.
sed 's|      <Boot>|      <AudioAdapter driver="ALSA" enabled="true"/>\n      <USB><Controllers><Controller name="OHCI" type="OHCI"/></Controllers></USB>\n      <Boot>|' \
	"$root/vbox/old.vbox" > "$root/vbox/av.vbox"
out=$("$hull" plan --resolved -f "$root/vbox/av.vbox" 2>&1)
says "its AC'97 and a tablet for its USB" "$out" '(nic e1000) (sound ac97) (pointer tablet)))'
sed 's|<AudioAdapter driver="ALSA" enabled="true"/>|<AudioAdapter controller="HDA" enabled="true"/>|' \
	"$root/vbox/av.vbox" > "$root/vbox/hda.vbox"
out=$("$hull" plan --resolved -f "$root/vbox/hda.vbox" 2>&1)
says 'an HDA card by name' "$out" '(sound hda)'
out=$(timeout 120 "$hull" run -f "$root/vbox/av.vbox" 2>&1)
got=$?
status 'a .vbox with its sound and a tablet boots' "$got" 0 "$out"
says 'its VDI, beside a UHCI and its tablet' "$out" 'word: fresh'

# A Mac's devices (hull setup macos's machine): Apple's SMC, its key read
# from the user's file as the machine starts, a Penryn by Intel's name
# and VMware's card, all planned native; no key file, and a key not 64
# characters, refused at the start naming the file; a key of 64 (not
# Apple's) and the test's sector boots on q35 with all of them there; a
# board with an SMC refused.
image 1M "$root/mac.img"
conf '(source "media:disk") (network nat) (media (medium (type disk) (source "image:./mac.img")))' \
	'(tuning (pc (machine q35) (cpu penryn) (nic vmxnet3) (smc "./osk.txt")))'
out=$("$hull" plan --explain -f "$root/hull.conf" 2>&1)
says "a Mac's SMC planned" "$out" 'pc.smc.path: native'
says 'a Penryn planned' "$out" 'pc.cpu.penryn: native'
says "VMware's card planned" "$out" 'pc.nic.vmxnet3: native'
rm -f "$root/osk.txt"
run
status 'no SMC key file is refused' "$got" 125 "$out"
says 'naming what it wants' "$out" "Apple's SMC key is 64 characters"
printf 'too short\n' > "$root/osk.txt"
run
status 'an SMC key not 64 characters is refused' "$got" 125 "$out"
printf '%064d\n' 0 > "$root/osk.txt"
run
status 'a machine with an SMC boots' "$got" 0 "$out"
says 'from its drive' "$out" 'booted from a drive'
printf '(machine (backend pc) (source "image:./mac.img") (tuning (pc (machine arduino-uno) (smc "./osk.txt"))))\n' \
	> "$root/hull.conf"
run
status 'a board with an SMC is refused' "$got" 125 "$out"
says 'and said' "$out" 'no Apple SMC'
rm -f "$root/osk.txt" "$root/mac.img"

# A PowerPC Mac, mac99, on qemu-system-ppc -- Debian's, or `make
# qemu-ppc`'s in the cache: planned emulated; a CPU model and a floppy
# refused; a blank disk reaching OpenBIOS, which says so on the serial
# port, and the machine stopped.
# The user's copy is linked into the test's own cache, where the core looks.
if [ ! -x /usr/bin/qemu-system-ppc ] &&
   [ -x "$HOME/.cache/hull/qemu-system-ppc/usr/bin/qemu-system-ppc" ]; then
	mkdir -p "$root/cache/hull"
	ln -sfn "$HOME/.cache/hull/qemu-system-ppc" "$root/cache/hull/qemu-system-ppc"
fi
if [ -x /usr/bin/qemu-system-ppc ] ||
   [ -x "$root/cache/hull/qemu-system-ppc/usr/bin/qemu-system-ppc" ]; then
	truncate -s 16M "$root/ppc.img"
	conf '(hostname "pc-g4") (source "media:disk") (memory "512M")' \
		'(media (medium (type disk) (source "image:./ppc.img"))) (tuning (pc (machine mac99)))'
	out=$("$hull" plan --explain -f "$root/hull.conf" 2>&1)
	says 'a G4 Mac planned' "$out" 'pc.machine.mac99: emulated'
	conf '(source "media:disk") (media (medium (type disk) (source "image:./ppc.img")))' \
		'(tuning (pc (machine mac99) (cpu host)))'
	run
	status 'a CPU model on a Mac is refused' "$got" 125 "$out"
	conf '(source "media:floppy") (media (medium (type floppy) (source "image:./ppc.img")))' \
		'(tuning (pc (machine mac99)))'
	run
	status 'a floppy on a Mac is refused' "$got" 125 "$out"
	conf '(hostname "pc-g4") (source "media:disk") (memory "512M")' \
		'(media (medium (type disk) (source "image:./ppc.img"))) (tuning (pc (machine mac99)))'
	(cd "$root" && exec timeout 120 "$hull" run -f "$root/hull.conf" > "$root/ppc.out" 2>&1) &
	ppc_pid=$!
	tries=0
	until grep -q 'OpenBIOS' "$root/ppc.out" 2> /dev/null || [ "$tries" -ge 60 ]; do
		tries=$((tries + 1))
		sleep 1
	done
	timeout 30 "$hull" stop pc-g4 > /dev/null 2>&1
	wait "$ppc_pid"
	says 'a G4 Mac boots OpenBIOS' "$(cat "$root/ppc.out")" 'OpenBIOS'
	rm -f "$root/ppc.img" "$root/ppc.out"
else
	printf 'pc: no qemu-system-ppc (make qemu-ppc), the PowerPC Mac not tried\n'
fi

# Stopped, a PC's power button is pressed and its guest given its grace,
# as VirtualBox's ACPI Shutdown gives it, where qemu's TERM had quit at
# once with no word to the guest: a boot sector that spins, which never
# acts on the button, is stopped only after its three seconds.
truncate -s 1440K "$root/spin.img"
printf '\353\376' | dd of="$root/spin.img" conv=notrunc status=none
printf '\125\252' | dd of="$root/spin.img" bs=1 seek=510 conv=notrunc status=none
conf '(hostname "pc-spin") (source "media:floppy") (stop_grace "3s")' \
	'(media (medium (type floppy) (source "image:./spin.img")))'
(cd "$root" && exec timeout 120 "$hull" run -f "$root/hull.conf" > "$root/spin.out" 2>&1) &
spin_pid=$!
tries=0
until timeout 10 "$hull" ps 2>/dev/null | grep -q 'pc-spin .*running' || [ "$tries" -ge 100 ]; do
	tries=$((tries + 1))
	sleep 0.1
done
began=$(date +%s)
timeout 30 "$hull" stop pc-spin > /dev/null 2>&1
took=$(($(date +%s) - began))
wait "$spin_pid"
cases=$((cases + 1))
[ "$took" -ge 3 ] && [ "$took" -le 8 ] ||
	fail "a PC's stop waits its grace on the power button: took ${took}s" "$(cat "$root/spin.out")"
# A snapshot of the running machine, its memory with its disk, as
# VirtualBox takes one: by its id, listed from its file while it runs,
# restored and deleted on it; restored on the stopped machine, said to be
# as after a power cut. Under (mutable overlay), refused: qemu's
# throwaway file would take it.
qemu-img convert -O qcow2 "$root/spin.img" "$root/spin.qcow2"
conf '(hostname "pc-live") (source "media:disk") (mutable direct) (stop_grace "1s")' \
	'(media (medium (type disk) (source "image:./spin.qcow2")))'
live() {
	(cd "$root" && exec timeout 120 "$hull" run -f "$root/hull.conf" > "$root/spin.out" 2>&1) &
	spin_pid=$!
	tries=0
	until timeout 10 "$hull" ps 2>/dev/null | grep -q 'pc-live .*running' || [ "$tries" -ge 100 ]; do
		tries=$((tries + 1))
		sleep 0.1
	done
}
live
out=$("$hull" snapshot take pc-live running 2>&1)
got=$?
status 'a running machine snapshot taken' "$got" 0 "$out"
says 'listed from its file as it runs' "$("$hull" snapshot ls "$root/hull.conf" 2>&1)" 'running	'
# qemu-img's VM_SIZE, the machine state, not 0 B.
held=$(qemu-img snapshot -U -l "$root/spin.qcow2" 2>&1 | grep ' running ')
cases=$((cases + 1))
case $held in
''|*' 0 B '*) fail 'its memory in it' "$held" ;;
esac
out=$("$hull" snapshot restore pc-live running 2>&1)
got=$?
status 'and restored on it' "$got" 0 "$out"
out=$("$hull" snapshot restore pc-live none 2>&1)
got=$?
status 'a name it lacks is refused' "$got" 125 "$out"
timeout 30 "$hull" stop pc-live > /dev/null 2>&1
wait "$spin_pid"
out=$("$hull" snapshot restore "$root/hull.conf" running 2>&1)
got=$?
status 'restored on the stopped machine' "$got" 0 "$out"
says 'said as after a power cut' "$out" 'as after a power cut'
live
out=$("$hull" snapshot delete pc-live running 2>&1)
got=$?
status 'and deleted on the running machine' "$got" 0 "$out"
timeout 30 "$hull" stop pc-live > /dev/null 2>&1
wait "$spin_pid"
cases=$((cases + 1))
[ -z "$("$hull" snapshot ls "$root/hull.conf" 2>&1)" ] ||
	fail 'a deleted live snapshot is listed no more' "$("$hull" snapshot ls "$root/hull.conf" 2>&1)"
# Saved, VirtualBox's Save the machine state: `hull stop --save` keeps
# its memory and disk as hull-saved and powers it off; started again it
# resumes from them, says so, and the saved state is spent.
live
out=$("$hull" stop --save pc-live 2>&1)
got=$?
status 'a running machine saved' "$got" 0 "$out"
wait "$spin_pid"
says 'its state kept' "$(qemu-img snapshot -l "$root/spin.qcow2" 2>&1)" 'hull-saved'
out=$("$hull" export --bootable -f "$root/hull.conf" "$root/saved.ova" 2>&1)
got=$?
status 'a saved machine exported is refused' "$got" 125 "$out"
says 'naming the way to discard its state' "$out" 'hull-saved`, and export it then'
# A snapshot restored on the saved machine discards the saved state,
# which would otherwise put its own disk back at the next start.
"$hull" snapshot take "$root/hull.conf" before-save > /dev/null 2>&1 ||
	fail 'a snapshot beside a saved state taken' ''
out=$("$hull" snapshot restore "$root/hull.conf" before-save 2>&1)
says 'a restore discards the saved state' "$out" 'its saved state is discarded'
cases=$((cases + 1))
! "$hull" snapshot ls "$root/hull.conf" 2>&1 | grep -q '^hull-saved' ||
	fail 'and it is gone' "$("$hull" snapshot ls "$root/hull.conf" 2>&1)"
"$hull" snapshot delete "$root/hull.conf" before-save > /dev/null 2>&1
# Saved again, for the resume below.
live
"$hull" stop --save pc-live > /dev/null 2>&1
wait "$spin_pid"
live
cases=$((cases + 1))
tries=0
until grep -q 'resuming from its saved state' "$root/spin.out" 2>/dev/null || [ "$tries" -ge 50 ]; do
	tries=$((tries + 1))
	sleep 0.1
done
grep -q 'resuming from its saved state' "$root/spin.out" ||
	fail 'started again, it resumes from its saved state' "$(cat "$root/spin.out")"
cases=$((cases + 1))
tries=0
while qemu-img snapshot -U -l "$root/spin.qcow2" 2>/dev/null | grep -q hull-saved && [ "$tries" -lt 50 ]; do
	tries=$((tries + 1))
	sleep 0.1
done
! qemu-img snapshot -U -l "$root/spin.qcow2" 2>/dev/null | grep -q hull-saved ||
	fail 'and its saved state is spent' "$(qemu-img snapshot -U -l "$root/spin.qcow2" 2>&1)"
timeout 30 "$hull" stop pc-live > /dev/null 2>&1
wait "$spin_pid"
conf '(hostname "pc-live") (source "media:disk") (mutable overlay) (stop_grace "1s")' \
	'(media (medium (type disk) (source "image:./spin.qcow2")))'
live
out=$("$hull" snapshot take pc-live running 2>&1)
got=$?
status 'a running machine under overlay is refused a snapshot' "$got" 125 "$out"
says 'saying why' "$out" '(mutable direct) keeps one'
timeout 30 "$hull" stop pc-live > /dev/null 2>&1
wait "$spin_pid"
rm -f "$root/spin.img" "$root/spin.out" "$root/spin.qcow2"

# Imported, as hull-gui's Import is: the .vbox copied into a folder of
# its own, its VDI converted to qcow2 there and its hull.conf saying so,
# changes kept in the copy; the copy boots, the VDI is as it was, a
# second import into the folder and a file of no hypervisor's refused.
before=$(sha256sum < "$root/vbox/old.vdi")
out=$("$hull" import --machine "$root/vbox/av.vbox" "$root/imported" 2>&1)
got=$?
status 'a .vbox imported into a folder' "$got" 0 "$out"
conf=$(cat "$root/imported/hull.conf" 2>&1)
says 'its hull.conf names the copy' "$conf" '(medium (type disk) (source "image:./old.qcow2"))'
says 'kept in place' "$conf" '(mutable direct)'
says 'and its sound and tablet still' "$conf" '(sound ac97) (pointer tablet)'
says 'the copy is a qcow2' "$(qemu-img info "$root/imported/old.qcow2" 2>&1)" 'file format: qcow2'
# A snapshot of its disk taken before it first boots, as VirtualBox's
# Take Snapshot: listed; refused a second time and with a name that is no
# word.
out=$("$hull" snapshot take "$root/imported/hull.conf" clean 2>&1)
got=$?
status 'a snapshot taken' "$got" 0 "$out"
says 'and listed' "$("$hull" snapshot ls "$root/imported/hull.conf" 2>&1)" 'clean	'
out=$("$hull" snapshot take "$root/imported/hull.conf" clean 2>&1)
got=$?
status 'a snapshot of a name taken is refused' "$got" 1 "$out"
out=$("$hull" snapshot take "$root/imported/hull.conf" 'no word' 2>&1)
got=$?
status 'a snapshot name that is no word is refused' "$got" 2 "$out"
out=$(timeout 120 "$hull" run -f "$root/imported/hull.conf" 2>&1)
got=$?
status 'the imported machine boots' "$got" 0 "$out"
says 'from its copy' "$out" 'word: fresh'
# It wrote its word; booted again it says so. The snapshot restored, it
# boots fresh, as it was; deleted, it is listed no more. A tree's machine
# and a raw disk have no snapshot to take.
out=$(timeout 120 "$hull" run -f "$root/imported/hull.conf" 2>&1)
says 'its write kept' "$out" 'word: written'
out=$("$hull" snapshot restore "$root/imported/hull.conf" clean 2>&1)
got=$?
status 'the snapshot restored' "$got" 0 "$out"
out=$(timeout 120 "$hull" run -f "$root/imported/hull.conf" 2>&1)
says 'the disk as the snapshot had it' "$out" 'word: fresh'
out=$("$hull" snapshot delete "$root/imported/hull.conf" clean 2>&1)
got=$?
status 'the snapshot deleted' "$got" 0 "$out"
cases=$((cases + 1))
[ -z "$("$hull" snapshot ls "$root/imported/hull.conf" 2>&1)" ] ||
	fail 'a deleted snapshot is listed no more' "$("$hull" snapshot ls "$root/imported/hull.conf" 2>&1)"
conf '(source "image:./raw.img")'
out=$("$hull" snapshot take "$root/hull.conf" clean 2>&1)
got=$?
status 'a raw disk has no snapshot' "$got" 125 "$out"
says 'and is told how' "$out" 'qemu-img convert -O qcow2'
cases=$((cases + 1))
[ "$(sha256sum < "$root/vbox/old.vdi")" = "$before" ] ||
	fail 'the VDI imported from is unchanged' "$(qemu-img info "$root/vbox/old.vdi")"
out=$("$hull" import --machine "$root/vbox/av.vbox" "$root/imported" 2>&1)
got=$?
status 'an import into a folder holding a machine is refused' "$got" 1 "$out"
says 'and said' "$out" 'a machine is there already'
out=$("$hull" import --machine "$root/imported/hull.conf" "$root/again" 2>&1)
got=$?
status 'a file of no hypervisor is refused' "$got" 2 "$out"
cases=$((cases + 1))
[ ! -e "$root/again" ] || fail 'and no folder made for it' "$(ls -la "$root/again")"

# Exported, as hull-gui's Export Appliance is: the imported machine's own
# disk as an appliance, VirtualBox's shape -- its i440FX's disk on IDE --
# the same bytes twice, and imported back into a folder of its own, where
# it boots. A board, and a pc machine exported as anything but an .ova,
# are refused.
out=$("$hull" export --bootable -f "$root/imported/hull.conf" "$root/back.ova" 2>&1)
got=$?
status 'a pc machine exported as an appliance' "$got" 0 "$out"
ovf_text=$(tar xOf "$root/back.ova" old-vbox.ovf 2>&1)
says 'its disk on IDE, as it was' "$ovf_text" '<rasd:ResourceSubType>PIIX4</rasd:ResourceSubType>'
says 'its memory' "$ovf_text" '<rasd:VirtualQuantity>128</rasd:VirtualQuantity>'
says 'the descriptor first' "$(tar tf "$root/back.ova" 2>&1 | head -1)" 'old-vbox.ovf'
"$hull" export --bootable -f "$root/imported/hull.conf" "$root/back2.ova" > /dev/null 2>&1
cases=$((cases + 1))
cmp -s "$root/back.ova" "$root/back2.ova" || fail 'an appliance exported twice is the same bytes' ''
cached=$(ls "$root/cache/hull/ova" 2>/dev/null)
out=$("$hull" import --machine "$root/back.ova" "$root/reimported" 2>&1)
got=$?
status 'and imported back' "$got" 0 "$out"
conf=$(cat "$root/reimported/hull.conf" 2>&1)
says 'its controller read back' "$conf" '(machine pc)'
says 'and its sound and tablet' "$conf" '(sound ac97) (pointer tablet)'
out=$(timeout 120 "$hull" run -f "$root/reimported/hull.conf" 2>&1)
got=$?
status 'the appliance boots' "$got" 0 "$out"
says 'from its own disk' "$out" 'booted from a drive'
out=$("$hull" export --bootable -f "$root/imported/hull.conf" "$root/back.img" 2>&1)
got=$?
status 'a pc machine exported as an image is refused' "$got" 125 "$out"
printf '(machine (backend pc) (source "image:./raw.img") (tuning (pc (machine mps2-an385))))\n' \
	> "$root/board-export.conf"
out=$("$hull" export --bootable -f "$root/board-export.conf" "$root/board.ova" 2>&1)
got=$?
status 'a board exported as an appliance is refused' "$got" 125 "$out"
says 'and said' "$out" 'an appliance describes a PC'
printf '(machine (backend pc) (source "media:disk") (media (medium (type disk) (source "dir:./"))))\n' \
	> "$root/board-export.conf"
out=$("$hull" export --bootable -f "$root/board-export.conf" "$root/board.ova" 2>&1)
got=$?
status 'a collection disk exported as an appliance is refused' "$got" 125 "$out"
says 'naming what makes it an image' "$out" 'hull media export DIR IMAGE'
rm -rf "$root/reimported"
rm -f "$root/back.ova" "$root/back2.ova" "$root/board-export.conf"
# The cache the appliance was unpacked into, so the cases below count
# their own alone.
for entry in $(ls "$root/cache/hull/ova" 2>/dev/null); do
	case " $(printf '%s ' $cached)" in
	*" $entry "*) ;;
	*) rm -rf "${root:?}/cache/hull/ova/${entry:?}" ;;
	esac
done

# An appliance: its .ovf read as a pc machine, VirtualBox's export shape
# -- a streamOptimized VMDK, an IDE controller, an E1000 -- and the .ova
# around it unpacked once into the cache, then booted from there.
# VMware's shape: a disk on SATA by ovf:/disk/, UEFI by its extra
# configuration. A drive naming what the appliance lacks is refused.
mkdir -p "$root/ovf"
qemu-img convert -f raw -O vmdk -o subformat=streamOptimized "$root/raw.img" \
	"$root/ovf/box-disk001.vmdk"
ovf() { # FILE DISK-RESOURCE CONTROLLER-TYPE EXTRA
	cat > "$root/ovf/$1" <<EOF
<?xml version="1.0"?>
<Envelope ovf:version="1.0" xmlns="http://schemas.dmtf.org/ovf/envelope/1" xmlns:ovf="http://schemas.dmtf.org/ovf/envelope/1" xmlns:rasd="http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_ResourceAllocationSettingData" xmlns:vmw="http://www.vmware.com/schema/ovf">
  <References>
    <File ovf:id="file1" ovf:href="box-disk001.vmdk"/>
  </References>
  <DiskSection>
    <Disk ovf:capacity="1048576" ovf:diskId="vmdisk1" ovf:fileRef="file1"/>
  </DiskSection>
  <VirtualSystem ovf:id="Exported Box">
    <VirtualHardwareSection>
      <Item><rasd:InstanceID>1</rasd:InstanceID><rasd:ResourceType>3</rasd:ResourceType><rasd:VirtualQuantity>2</rasd:VirtualQuantity></Item>
      <Item><rasd:AllocationUnits>byte * 2^20</rasd:AllocationUnits><rasd:InstanceID>2</rasd:InstanceID><rasd:ResourceType>4</rasd:ResourceType><rasd:VirtualQuantity>192</rasd:VirtualQuantity></Item>
      <Item><rasd:InstanceID>3</rasd:InstanceID><rasd:ResourceType>$3</rasd:ResourceType></Item>
      <Item><rasd:InstanceID>5</rasd:InstanceID><rasd:ResourceSubType>E1000</rasd:ResourceSubType><rasd:ResourceType>10</rasd:ResourceType></Item>
      <Item><rasd:HostResource>$2</rasd:HostResource><rasd:InstanceID>7</rasd:InstanceID><rasd:Parent>3</rasd:Parent><rasd:ResourceType>17</rasd:ResourceType></Item>
      <Item><rasd:InstanceID>8</rasd:InstanceID><rasd:Parent>3</rasd:Parent><rasd:ResourceType>15</rasd:ResourceType></Item>
      $4
    </VirtualHardwareSection>
  </VirtualSystem>
</Envelope>
EOF
}
ovf box.ovf /disk/vmdisk1 5 ''
out=$("$hull" plan --resolved -f "$root/ovf/box.ovf" 2>&1)
says 'an .ovf is a pc machine of its name' "$out" '(hostname "exported-box") (source "media:disk") (memory "192M") (cpus 2) (network nat)'
says 'its disk by its file' "$out" '(medium (type disk) (source "image:./box-disk001.vmdk"))'
sed 's/>E1000</>VmxNet3</' "$root/ovf/box.ovf" > "$root/ovf/vmxnet3.ovf"
says "VMware's vmxnet3 kept" "$("$hull" plan --resolved -f "$root/ovf/vmxnet3.ovf" 2>&1)" '(nic vmxnet3)'
rm -f "$root/ovf/vmxnet3.ovf"
says 'on IDE, an i440FX' "$out" '(tuning (pc (machine pc) (cpu max) (nic e1000)))'
(cd "$root/ovf" && tar -cf box.ova box.ovf box-disk001.vmdk)
out=$(timeout 120 "$hull" run -f "$root/ovf/box.ova" 2>&1)
got=$?
status 'an .ova boots' "$got" 0 "$out"
says 'its streamOptimized disk' "$out" 'word: fresh'
"$hull" plan -f "$root/ovf/box.ova" > /dev/null 2>&1
cases=$((cases + 1))
[ "$(ls "$root/cache/hull/ova" | wc -l)" = 1 ] && [ -f "$root/cache/hull/ova/$(ls "$root/cache/hull/ova")/box.ovf" ] ||
	fail 'an .ova unpacked once into the cache' "$(ls -R "$root/cache/hull/ova")"
# Kept while the .ova is as it was, and taken by hull gc once it is not:
# what a machine leaves is counted after the kill, below.
out=$(timeout 60 "$hull" gc 2>&1)
cases=$((cases + 1))
[ "$(ls "$root/cache/hull/ova" | wc -l)" = 1 ] || fail 'hull gc kept an unpacked .ova still there' "$out"
touch -d '2001-01-01' "$root/ovf/box.ova"
out=$(timeout 60 "$hull" gc 2>&1)
says 'hull gc takes an unpacked .ova once it has changed' "$out" 'removed 1 unpacked appliance whose .ova is gone or changed'
cases=$((cases + 1))
[ -z "$(ls -A "$root/cache/hull/ova")" ] || fail 'and its files are gone' "$(ls -R "$root/cache/hull/ova")"

# And the way out: a tree exported as an .ova, `hull export --bootable -f
# FILE X.ova`, its title, memory and processors the spec's, the same
# bytes twice, its descriptor first; read back as a machine, which boots
# the image by BIOS on q35's AHCI, its init the probe. VirtualBox's own
# import of it is test/virtualbox_test.sh's. Without Limine built from
# source no .ova is made, which is said and not counted.
mkdir -p "$root/ovaroot/sbin" "$root/ovaroot/etc" "$root/ovaroot/dev" "$root/ovaroot/proc" \
	"$root/ovaroot/sys" "$root/ovaroot/run"
cp "$probe" "$root/ovaroot/sbin/booted"
printf '(machine (source "dir:./ovaroot") (hostname "hull-box") (memory "512M") (cpus 2) %s)\n' \
	'(init "/sbin/booted")' > "$root/ova.conf"
out=$(timeout 300 "$hull" export --bootable -f "$root/ova.conf" "$root/ovf/out.ova" 2>&1)
got=$?
if [ "$got" -ne 0 ] && printf '%s\n' "$out" | grep -q 'make limine'; then
	printf 'pc: no .ova exported: %s\n' "$out"
else
	status 'a tree exported as an .ova' "$got" 0 "$out"
	timeout 300 "$hull" export --bootable -f "$root/ova.conf" "$root/ovf/again.ova" > /dev/null 2>&1
	cases=$((cases + 1))
	cmp -s "$root/ovf/out.ova" "$root/ovf/again.ova" ||
		fail 'two .ova exports of one tree differ' "$(cmp "$root/ovf/out.ova" "$root/ovf/again.ova" 2>&1)"
	out=$(tar -tf "$root/ovf/out.ova" 2>&1)
	cases=$((cases + 1))
	[ "$(printf '%s\n' "$out" | tr '\n' ' ')" = 'hull-box.ovf hull-box-disk1.vmdk ' ] ||
		fail 'the .ova holds its descriptor first, then its disk' "$out"
	out=$("$hull" plan --resolved -f "$root/ovf/out.ova" 2>&1)
	says 'the .ova read back as its machine' "$out" '(hostname "hull-box") (source "media:disk") (memory "512M") (cpus 2) (network nat)'
	out=$(timeout 180 "$hull" run -f "$root/ovf/out.ova" 2>&1)
	got=$?
	status 'the exported .ova boots' "$got" 0 "$out"
	says 'by BIOS, its init the probe' "$out" 'booted pid=1 root=ext4'
	rm -f "$root/ovf/out.ova" "$root/ovf/again.ova"
	timeout 60 "$hull" gc > /dev/null 2>&1
fi
ovf vmware.ovf ovf:/disk/vmdisk1 20 '<vmw:Config ovf:required="false" vmw:key="firmware" vmw:value="efi"/>'
out=$("$hull" plan --resolved -f "$root/ovf/vmware.ovf" 2>&1)
says "VMware's shape: SATA, q35, UEFI" "$out" '(tuning (pc (machine q35) (cpu max) (firmware uefi) (nic e1000)))'
ovf lacking.ovf /disk/vmdisk9 5 ''
out=$("$hull" plan -f "$root/ovf/lacking.ovf" 2>&1)
got=$?
status 'a drive the appliance lacks is refused' "$got" 2 "$out"
says 'and named' "$out" 'names /disk/vmdisk9, which the appliance does not hold'

# --- a floppy ----------------------------------------------------------------

image 1440K "$root/floppy.img"
conf '(source "media:floppy") (media (medium (type floppy) (source "image:./floppy.img")))'
run
status 'a floppy boots' "$got" 0 "$out"
says 'and reads its next sector' "$out" 'word: fresh'

# --- a collection: plain files, and the disk they are ------------------------

image 1M "$root/c.img"
"$hull" media import "$root/c.img" "$root/c" > /dev/null 2>&1
conf '(source "media:disk") (media (medium (type disk) (source "dir:./c")))'
run
status 'a collection boots' "$got" 0 "$out"
says 'as the disk it was' "$out" 'word: fresh'
run
says 'its writes gone with the machine' "$out" 'word: fresh'
cases=$((cases + 1))
"$hull" media verify "$root/c" > /dev/null 2>&1 || fail 'the collection is as it was' ''

# Under (mutable direct) its writes go back into it when the machine
# powers off: taken apart again, swapped in whole, and verifying as the
# disk it now is; the next boot reads the word the first one left. On a
# copy, the original kept for the cases after.
cp -a "$root/c" "$root/cw"
conf '(source "media:disk") (mutable direct)' \
	'(media (medium (type disk) (source "dir:./cw")))'
run
status 'a collection written under direct' "$got" 0 "$out"
says 'and said' "$out" 'cw written back'
# Served, not copied: the server kept the blocks the guest wrote, one
# sector's worth here, and only those went back.
says 'by the blocks it wrote' "$out" 'written back: 1 blocks written'
run
says 'its write kept in its files' "$out" 'word: written'
cases=$((cases + 1))
"$hull" media verify "$root/cw" > /dev/null 2>&1 || fail 'the collection verifies as the disk it is now' ''
cases=$((cases + 1))
[ ! -e "$root/cw.hull-new" ] && [ ! -e "$root/cw.unsaved.img" ] ||
	fail 'nothing left beside the collection' "$(ls "$root")"

# Live: under direct the server takes the guest's writes into the
# collection whenever it pauses, not only at power-off. A held disk -- its
# third sector says 'h' -- runs about 8 s after its write; the collection
# holds the write while the machine still runs, and at power-off nothing
# is left to take.
image 1M "$root/held.img"
printf 'hold' | dd of="$root/held.img" bs=1 seek=1024 conv=notrunc status=none
"$hull" media import "$root/held.img" "$root/cl" > /dev/null 2>&1
conf '(source "media:disk") (mutable direct) (media (medium (type disk) (source "dir:./cl")))'
(timeout 60 "$hull" run -f "$root/hull.conf" > "$root/held.out" 2>&1) &
heldrun=$!
live=no
# The second sector, the one the guest writes, read from the collection
# exported -- not a grep of its files, the boot sector's own code holding
# the same words.
for i in $(seq 1 40); do
	rm -f "$root/held-now.img"
	if "$hull" media export "$root/cl" "$root/held-now.img" > /dev/null 2>&1 &&
	   [ "$(dd if="$root/held-now.img" bs=1 skip=512 count=13 status=none)" = 'word: written' ] &&
	   ! grep -q '^held' "$root/held.out"; then
		live=yes
		break
	fi
	sleep 0.25
done
wait "$heldrun"
got=$?
cases=$((cases + 1))
[ "$live" = yes ] || fail 'the write in the collection while the machine ran' "$(cat "$root/held.out")"
out=$(cat "$root/held.out")
status 'the held machine ran to its end' "$got" 0 "$out"
says 'and its writes were taken as it ran' "$out" 'taken in 1 times as the machine ran'
cases=$((cases + 1))
"$hull" media verify "$root/cl" > /dev/null 2>&1 && [ ! -e "$root/cl.hull-new" ] ||
	fail 'the collection verifies, nothing beside it' "$(ls "$root")"

# (size ...): the disk served larger than its collection, zeros after
# it; under direct the collection takes the larger disk back, its own
# media.conf saying the new size and verifying. Never smaller, and an
# image: disk is the user's file, not grown by a word.
cp -a "$root/c" "$root/cg"
conf '(source "media:disk") (mutable direct)' \
	'(media (medium (type disk) (source "dir:./cg") (size "4M")))'
run
status 'a collection served at 4M' "$got" 0 "$out"
says 'and written back' "$out" 'cg written back'
cases=$((cases + 1))
grep -q '(bytes 4194304)' "$root/cg/media.conf" &&
	"$hull" media verify "$root/cg" > /dev/null 2>&1 ||
	fail 'the collection grown to its disk' "$(cat "$root/cg/media.conf")"
run
says 'and its write kept at the new size' "$out" 'word: written'
conf '(source "media:disk") (media (medium (type disk) (source "dir:./c") (size "512K")))'
run
status 'a size below the collection refused' "$got" 125 "$out"
says 'and said' "$out" 'grown, not cut'
conf '(source "media:disk") (media (medium (type disk) (source "image:./c.img") (size "4M")))'
run
status 'a size on an image: refused' "$got" 125 "$out"
says 'and said' "$out" "a disk collection's"

# One file a collection can travel as, tar: or pack:, as a machine's source.
tar -cf "$root/c.tar" -C "$root/c" .
conf '(source "media:disk") (media (medium (type disk) (source "tar:./c.tar")))'
run
status 'a collection as a tar boots' "$got" 0 "$out"
says 'as the disk it was' "$out" 'word: fresh'
"$hull" pack "$root/c" "$root/c.hull" > /dev/null 2>&1
conf '(source "media:disk") (media (medium (type disk) (source "pack:./c.hull")))'
run
status 'a collection as a pack boots' "$got" 0 "$out"
says 'as the disk it was' "$out" 'word: fresh'
# A tar: collection is the store's checkout: not written back into.
conf '(source "media:disk") (mutable direct) (media (medium (type disk) (source "tar:./c.tar")))'
run
status 'a tar: collection is not written under direct' "$got" 125 "$out"
says 'and said' "$out" "the store's"

first=$(find "$root/c/quirks" -type f | head -1)
cp "$first" "$root/quirk.saved"
printf 'x' | dd of="$first" bs=1 seek=1 conv=notrunc status=none
conf '(source "media:disk") (media (medium (type disk) (source "dir:./c")))'
run
status 'a collection changed by a byte is refused' "$got" 125 "$out"
cp "$root/quirk.saved" "$first"

# Two machines, each with a collection of its own, started from one file:
# each reassembles its disk on its own thread, and media.c's copies went
# through static buffers, so the two wrote through each other's and both
# disks failed their hash -- every time, with 8 MiB of data each. Random
# bytes past the boot sectors, as zeros are a hole the copy passes over.
image 16M "$root/pair.img"
head -c 8M /dev/urandom | dd of="$root/pair.img" bs=1M seek=1 conv=notrunc status=none
"$hull" media import "$root/pair.img" "$root/pa" > /dev/null 2>&1
"$hull" media import "$root/pair.img" "$root/pb" > /dev/null 2>&1
printf '(machines\n  (pa (backend pc) (source "media:disk") (media (medium (type disk) (source "dir:./pa"))))\n  (pb (backend pc) (source "media:disk") (media (medium (type disk) (source "dir:./pb")))))\n' \
	> "$root/pair.conf"
out=$(timeout 120 "$hull" run -f "$root/pair.conf" 2>&1)
got=$?
status 'two machines with collections start together' "$got" 0 "$out"
cases=$((cases + 1))
[ "$(printf '%s\n' "$out" | grep -c 'word: fresh')" = 2 ] ||
	fail 'and both boot their disks' "$out"

# --- a disc installs, and the disk boots, by one line ------------------------

python3 -I "$here/pc_disc.py" "$root/setup.iso" "$root/boot.bin"
truncate -s 1M "$root/install.img"
conf '(source "media:cdrom") (mutable direct)' \
	'(media (medium (type disk) (source "image:./install.img"))' \
	'(medium (type cdrom) (source "image:./setup.iso")))'
run
status 'a disc boots' "$got" 0 "$out"
says 'and installs' "$out" 'installing from a disc'
conf '(source "media:disk") (mutable direct)' \
	'(media (medium (type disk) (source "image:./install.img"))' \
	'(medium (type cdrom) (source "image:./setup.iso")))'
run
status 'the disk it installed boots' "$got" 0 "$out"
says 'its word there' "$out" 'word: fresh'

# Both drives boot now, so it is the source that chooses, the firmware
# otherwise falling through to whichever one will.
conf '(source "media:cdrom")' \
	'(media (medium (type disk) (source "image:./install.img"))' \
	'(medium (type cdrom) (source "image:./setup.iso")))'
run
says 'with both bootable the source chooses the disc' "$out" 'installing from a disc'

"$hull" media import "$root/setup.iso" "$root/setup" > /dev/null 2>&1
truncate -s 1M "$root/install2.img"
conf '(source "media:cdrom") (mutable direct)' \
	'(media (medium (type disk) (source "image:./install2.img"))' \
	'(medium (type cdrom) (source "dir:./setup")))'
run
says 'a disc from its collection installs too' "$out" 'installing from a disc'

# --- the other PCs -----------------------------------------------------------

conf '(source "media:disk") (tuning (pc (accel tcg) (machine q35) (vga cirrus)))' \
	'(media (medium (type disk) (source "image:./raw.img")))'
run
status 'under emulation, on a q35' "$got" 0 "$out"
says 'it boots the same' "$out" 'word: fresh'

# The cards a system of each age looks for, each there as it boots.
for card in sb16 adlib gus es1370 ac97 hda; do
	conf "(source \"media:disk\") (tuning (pc (sound $card)))" \
		'(media (medium (type disk) (source "image:./raw.img")))'
	run
	status "with a sound card, $card" "$got" 0 "$out"
done

# --- a stop, a killed run, and nothing left ----------------------------------

printf '\353\376' > "$root/spin.bin"
truncate -s 1M "$root/spin.img"
dd if="$root/spin.bin" of="$root/spin.img" conv=notrunc status=none
printf '\125\252' | dd of="$root/spin.img" bs=1 seek=510 conv=notrunc status=none
conf '(source "media:disk") (media (medium (type disk) (source "image:./spin.img")))'
timeout 60 "$hull" run -f "$root/hull.conf" > "$root/spin.out" 2>&1 &
pid=$!
sleep 2
id=$("$hull" ps | awk '$5 == "running" { print $1 }')
out=$(timeout 30 "$hull" stop "$id" 2>&1)
status 'a stop' "$?" 0 "$out"
wait "$pid"
cases=$((cases + 1))
sleep 1
out=$(ps -eo pid=,args= | grep -F "$root/" | grep '[q]emu-system')
[ -z "$out" ] || fail 'a stop left no qemu' "$out"

# A core killed with a collection's disk open: qemu goes with it, and
# its snapshot with qemu.
conf '(source "media:disk") (media (medium (type disk) (source "dir:./spinc")))'
"$hull" media import "$root/spin.img" "$root/spinc" > /dev/null 2>&1
timeout 60 "$hull" run -f "$root/hull.conf" > "$root/spin.out" 2>&1 &
pid=$!
sleep 2
core=$(ps -eo pid=,args= | awk '/[h]ull core/ { print $1 }' | while read -r p; do
	[ "$(tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | sed -n 's/^XDG_RUNTIME_DIR=//p')" = "$root/run" ] && echo "$p"
done | head -1)
qemu=$(ps -o pid= --ppid "${core:-1}" | while read -r p; do
	[ "$(cat "/proc/$p/comm" 2>/dev/null)" = qemu-system-x86 ] && echo "$p"
done | head -1)
# Its disk served where its files lie, by NBD, no image made of it.
cases=$((cases + 1))
[ -n "$qemu" ] && tr '\0' ' ' < "/proc/$qemu/cmdline" | grep -q 'file.driver=nbd,file.server.type=fd' ||
	fail 'a disk collection is served, not copied' "$(tr '\0' ' ' < "/proc/${qemu:-1}/cmdline" 2>&1)"
cases=$((cases + 1))
if [ -z "$core" ] || [ -z "$qemu" ]; then
	fail 'the core and its qemu are found' "core ${core:-none}, qemu ${qemu:-none}"
else
	kill -9 "$core"
fi
wait "$pid"
sleep 2
cases=$((cases + 1))
[ -n "$qemu" ] && [ ! -d "/proc/$qemu" ] || fail 'a killed core left no qemu' "qemu ${qemu:-none}"
cases=$((cases + 1))
out=$(find "$root/cache" -type f)
[ -z "$out" ] || fail 'nothing of the machines is left in the cache' "$out"

# --- its window --------------------------------------------------------------

if command -v Xvfb > /dev/null && command -v xwininfo > /dev/null; then
	number=$((90 + $$ % 300))
	Xvfb ":$number" -screen 0 800x600x24 -nolisten tcp > /dev/null 2>&1 &
	x=$!
	sleep 1
	conf '(source "media:disk") (display window)' \
		'(media (medium (type disk) (source "image:./spin.img")))'
	DISPLAY=":$number" timeout 30 "$hull" run -f "$root/hull.conf" > "$root/x.out" 2>&1 &
	pid=$!
	sleep 3
	out=$(DISPLAY=":$number" xwininfo -root -tree 2>&1)
	says 'a window on the display' "$out" 'QEMU'
	id=$("$hull" ps | awk '$5 == "running" { print $1 }')
	timeout 30 "$hull" stop "$id" > /dev/null 2>&1
	wait "$pid"
	kill "$x"
else
	printf 'pc: no Xvfb, the window not tried\n'
fi

# --- a medium changed under a running machine --------------------------------

# A sector that spins, so the machine is there to be asked.
truncate -s 1440K "$root/swap.img"
dd if="$root/spin.bin" of="$root/swap.img" conv=notrunc status=none
printf '\125\252' | dd of="$root/swap.img" bs=1 seek=510 conv=notrunc status=none
conf '(source "media:floppy") (media (medium (type floppy) (source "image:./swap.img")))'
timeout 40 "$hull" run -f "$root/hull.conf" > "$root/swap.out" 2>&1 &
pid=$!
sleep 2
id=$("$hull" ps | awk '$5 == "running" { print $1 }')
out=$(timeout 10 "$hull" media change "$id" cdrom "$root/swap.img" 2>&1)
status 'a drive the machine lacks is refused' "$?" 125 "$out"
says 'and said to be lacking' "$out" 'no such drive'
out=$(timeout 10 "$hull" media change "$id" disk "$root/swap.img" 2>&1)
status 'a fixed disk is refused' "$?" 125 "$out"
says 'being no removable drive' "$out" 'floppy or a cdrom'
out=$(timeout 10 "$hull" media change "$id" floppy2 "$root/swap.img" 2>&1)
status 'a second drive not there is refused' "$?" 125 "$out"
out=$(timeout 10 "$hull" media change "$id" floppy "$root/gone.img" 2>&1)
status 'an image not there is refused' "$?" 125 "$out"
out=$(timeout 10 "$hull" media change "$id" floppy "$root" 2>&1)
status 'a directory is refused' "$?" 125 "$out"
says 'and export named' "$out" 'hull media export'
out=$(timeout 10 "$hull" media change "$id" floppy "$root/floppy.img" 2>&1)
status 'a floppy changes' "$?" 0 "$out"
out=$(timeout 10 "$hull" media change "$id" floppy 2>&1)
status 'and comes out' "$?" 0 "$out"
out=$(timeout 10 "$hull" media change nobody floppy 2>&1)
status 'a machine not there is refused' "$?" 125 "$out"
timeout 30 "$hull" stop "$id" > /dev/null 2>&1
wait "$pid"

# --- its screen by VNC, at its address, with no X -----------------------------

conf '(source "media:disk") (display vnc)' \
	'(media (medium (type disk) (source "image:./spin.img")))'
timeout 30 "$hull" run -f "$root/hull.conf" > "$root/vnc.out" 2>&1 &
pid=$!
sleep 2
address=$("$hull" ps | awk '$5 == "running" { print $2 }')
out=$(timeout 5 nc -w 2 "$address" 5900 < /dev/null | head -c 12)
says 'the screen answers at the machine'"'"'s address' "$out" 'RFB 003'
# And by qemu's D-Bus display, hull-gui's (project.md section 5.2): its
# monitor linked by the address, a frame read through it, the link gone
# with the machine.
link=${XDG_RUNTIME_DIR:-/nonexistent}/hull/display/$address
cases=$((cases + 1))
[ -S "$link" ] || fail "a pc with (display vnc) linked no D-Bus display monitor" "$(ls -l "$link" 2>&1)"
if python3 -c 'import gi; gi.require_version("Gio", "2.0")' 2> /dev/null; then
	out=$(timeout 30 python3 "$(dirname "$0")/display_client.py" "$link" 15 2>&1)
	says 'the screen is read by its D-Bus display' "$out" 'frame '
fi
id=$("$hull" ps | awk '$5 == "running" { print $1 }')
timeout 30 "$hull" stop "$id" > /dev/null 2>&1
wait "$pid"
cases=$((cases + 1))
[ ! -L "$link" ] || fail "a pc's display link outlived it" "$(ls -l "$link" 2>&1)"

# --- a real system, its port at the machine's address ------------------------

busybox=$(command -v busybox)
module=$(ls "/lib/modules/$(uname -r)/kernel/drivers/net/ethernet/intel/e1000/"e1000.ko* 2>/dev/null)
if [ -n "$busybox" ] && [ -n "$module" ]; then
	tree=$root/tree
	mkdir -p "$tree/bin" "$tree/proc" "$tree/sys" "$tree/dev"
	cp "$busybox" "$tree/bin/busybox"
	for lib in $(ldd "$busybox" | grep -oE '/[^ ]+'); do
		mkdir -p "$tree$(dirname "$lib")"
		cp -L "$lib" "$tree$lib"
	done
	case $module in
	*.xz) xz -dc "$module" > "$tree/e1000.ko" ;;
	*) cp "$module" "$tree/e1000.ko" ;;
	esac
	cat > "$tree/init.sh" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox --install -s /bin
echo "a linux on a pc"
insmod /e1000.ko
ip link set eth0 up
ip addr add 10.0.2.15/24 dev eth0
echo listening
nc -l -p 7000
echo guest-listening-on-22
nc -l -p 22
echo served
poweroff -f
INIT
	chmod +x "$tree/init.sh"
	out=$(timeout 120 "$hull" export --bootable "$tree" "$root/linux.img" --init /init.sh 2>&1)
	status 'a bootable export' "$?" 0 "$out"
	# UEFI, which a bootable export always boots by: its BIOS stage is
	# there only where Limine was built.
	conf '(source "image:./linux.img") (memory "512M") (network nat)' \
		'(ports (port (host 7000) (guest 7000)) (port (host 7022) (guest 22)))' \
		'(tuning (pc (nic e1000) (firmware uefi)))'
	timeout 90 "$hull" run -f "$root/hull.conf" > "$root/linux.out" 2>&1 &
	pid=$!
	sleep 2
	address=$("$hull" ps | awk '$5 == "running" { print $2 }')
	n=0
	while [ "$n" -lt 40 ] && ! grep -qa listening "$root/linux.out"; do
		sleep 1
		n=$((n + 1))
	done
	sleep 1
	printf 'from the host\n' | timeout 10 nc -w 3 "$address" 7000
	n=0
	while [ "$n" -lt 20 ] && ! grep -qa guest-listening-on-22 "$root/linux.out"; do
		sleep 1
		n=$((n + 1))
	done
	sleep 1
	# Port 22 forwarded is the guest's own server, which goes before any door.
	id=$("$hull" ps | awk '$5 == "running" { print $1 }')
	{ printf 'through proxy-ssh\n'; sleep 2; } | timeout 10 "$hull" proxy-ssh "$id" > /dev/null 2>&1
	wait "$pid"
	status 'a linux powers off' "$?" 0 "$(tail -5 "$root/linux.out")"
	says 'it booted' "$(cat "$root/linux.out")" 'a linux on a pc'
	says 'a client at its address reached it' "$(cat "$root/linux.out")" 'from the host'
	says 'proxy-ssh reached the guest'"'"'s own port 22' "$(cat "$root/linux.out")" 'through proxy-ssh'

	# What Windows 11 asks of a PC (fmake, 2026-11-12): a TPM 2.0 of
	# swtpm's, which the guest's own kernel finds, its state kept in
	# (state DIR) under (mutable direct) as the disk is; a CPU of the
	# host's, qemu's qemu64 lacking SSE4.2; and Secure Boot with
	# Microsoft's keys, which refuses this export's unsigned loader that
	# (firmware uefi) boots.
	cat > "$tree/init.sh" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sys /sys
echo "tpm=$(/bin/busybox cat /sys/class/tpm/tpm0/tpm_version_major 2>/dev/null)"
echo "sse42=$(/bin/busybox grep -c sse4_2 /proc/cpuinfo)"
/bin/busybox poweroff -f
INIT
	out=$(timeout 120 "$hull" export --bootable "$tree" "$root/tpm.img" --init /init.sh 2>&1)
	status 'a bootable export for the TPM' "$?" 0 "$out"
	rm -rf "${root:?}/tpmstate"
	conf '(source "image:./tpm.img") (memory "512M") (mutable direct)' \
		'(tuning (pc (machine q35) (firmware uefi) (tpm true) (cpu host) (state "./tpmstate")))'
	out=$(timeout 120 "$hull" run -f "$root/hull.conf" 2>&1)
	status 'a machine with a TPM' "$?" 0 "$out"
	says 'its kernel found a TPM 2.0' "$out" 'tpm=2'
	says 'its CPU the host'"'"'s, SSE4.2 and all' "$out" 'sse42=1'
	cases=$((cases + 1))
	[ -f "$root/tpmstate/tpm/tpm2-00.permall" ] ||
		fail "the TPM's state was not kept in (state DIR)" "$(ls -R "$root/tpmstate" 2>&1)"
	conf '(source "image:./tpm.img") (memory "512M")' \
		'(tuning (pc (machine q35) (firmware uefi-secure)))'
	out=$(timeout 40 "$hull" run -f "$root/hull.conf" 2>&1)
	says 'Secure Boot refused an unsigned loader' "$out" 'Access Denied'
	cases=$((cases + 1))
	! printf '%s\n' "$out" | grep -q 'tpm=' || fail "an unsigned loader ran under Secure Boot" "$out"
	conf '(source "image:./tpm.img") (memory "512M") (tuning (pc (firmware uefi-secure)))'
	out=$(timeout 30 "$hull" run -f "$root/hull.conf" 2>&1)
	status 'Secure Boot on the older machine is refused' "$?" 125 "$out"
	says 'and said to be q35'"'"'s' "$out" '(machine q35)'

	# (medium (type usb)): a USB stick, removable usb-storage on an xHCI,
	# which Windows Setup searches for its answer file. The guest's own
	# kernel, given the host's USB and SCSI modules, finds a removable disk
	# and reads the stick's first bytes; a type pc does not list is
	# refused at plan.
	mods=/lib/modules/$(uname -r)/kernel
	usbmods=
	for m in usb/common/usb-common usb/core/usbcore usb/host/xhci-hcd usb/host/xhci-pci \
	         scsi/scsi_common scsi/scsi_mod usb/storage/usb-storage scsi/sd_mod; do
		f=$(ls "$mods/drivers/$m.ko"* 2> /dev/null | head -1)
		[ -n "$f" ] || { usbmods=; break; }
		case $f in
		*.xz) xz -dc "$f" > "$tree/${m##*/}.ko" ;;
		*) cp "$f" "$tree/${m##*/}.ko" ;;
		esac
		usbmods="$usbmods ${m##*/}"
	done
	{ printf 'HULLSTICK'; head -c 1048567 /dev/zero; } > "$root/stick.img"
	if [ -z "$usbmods" ]; then
		printf 'pc: a USB stick not tried: the host has no USB storage modules to give the guest\n'
	else
		{
			printf '#!/bin/busybox sh\n'
			printf '/bin/busybox mount -t proc proc /proc\n'
			printf '/bin/busybox mount -t sysfs sys /sys\n'
			printf '/bin/busybox mount -t devtmpfs none /dev\n'
			for m in $usbmods; do printf '/bin/busybox insmod /%s.ko\n' "$m"; done
			# The boot disk is a disk too, on q35's AHCI: the stick is
			# the removable one.
			printf 'for i in 1 2 3 4 5 6 7 8 9 10; do\n'
			printf '  for d in /sys/block/sd*; do\n'
			printf '    [ "$(/bin/busybox cat $d/removable)" = 1 ] || continue\n'
			printf '    echo removable=1; echo "stick=$(/bin/busybox head -c 9 /dev/${d##*/})"; i=done; break\n'
			printf '  done\n'
			printf '  [ "$i" = done ] && break; /bin/busybox sleep 1\n'
			printf 'done\n'
			printf '/bin/busybox poweroff -f\n'
		} > "$tree/init.sh"
		out=$(timeout 120 "$hull" export --bootable "$tree" "$root/usb.img" --init /init.sh 2>&1)
		status 'a bootable export for the USB stick' "$?" 0 "$out"
		conf '(source "image:./usb.img") (memory "512M")' \
			'(media (medium (type usb) (source "image:./stick.img")))' \
			'(tuning (pc (machine q35) (firmware uefi)))'
		out=$(timeout 120 "$hull" run -f "$root/hull.conf" 2>&1)
		status 'a machine with a USB stick' "$?" 0 "$out"
		says 'its kernel found a removable disk' "$out" 'removable=1'
		says 'and read the stick' "$out" 'stick=HULLSTICK'
		for f in $usbmods; do rm -f "$tree/$f.ko"; done
	fi

	# A disk passes the guest's TRIM to its image: attached without
	# discard, a guest's image only grew whatever it freed (fmake's
	# Windows 11, 2026-11-18). An 8M disk of random bytes, fully
	# allocated, is the 16384-sector one; the guest discards it whole and
	# its image holds next to nothing after.
	cat > "$tree/init.sh" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sys /sys
/bin/busybox mount -t devtmpfs none /dev
for d in /sys/block/sd*; do
	[ "$(/bin/busybox cat $d/size)" = 16384 ] || continue
	echo "discard_max=$(/bin/busybox cat $d/queue/discard_max_bytes)"
	/bin/busybox blkdiscard /dev/${d##*/} && echo discarded=yes
done
/bin/busybox poweroff -f
INIT
	out=$(timeout 120 "$hull" export --bootable "$tree" "$root/trim.img" --init /init.sh 2>&1)
	status 'a bootable export for TRIM' "$?" 0 "$out"
	head -c 8388608 /dev/urandom > "$root/data.img"
	before=$(du -k "$root/data.img" | cut -f1)
	conf '(source "image:./trim.img") (memory "512M") (mutable direct)' \
		'(media (medium (type disk) (source "image:./data.img")))' \
		'(tuning (pc (machine q35) (firmware uefi)))'
	out=$(timeout 120 "$hull" run -f "$root/hull.conf" 2>&1)
	status 'a machine with a disk to trim' "$?" 0 "$out"
	says 'its disk offers discard' "$out" 'discarded=yes'
	after=$(du -k "$root/data.img" | cut -f1)
	cases=$((cases + 1))
	[ "$after" -lt $((before / 4)) ] ||
		fail "the guest's discard freed nothing in the image: ${before}K before, ${after}K after" "$out"
	rm -f "$root/data.img" "$root/trim.img"
	conf '(source "image:./stick.img") (memory "512M")' \
		'(media (medium (type tape) (source "image:./stick.img")))'
	out=$(timeout 30 "$hull" run -f "$root/hull.conf" 2>&1)
	status 'a medium of a type pc does not list is refused' "$?" 125 "$out"
	says 'at plan' "$out" 'does not list this medium type'

	# (boot_key N): Enter, pressed on the machine's keyboard once a second,
	# as a UEFI disc's "Press any key to boot from CD" wants and nobody is
	# there to give. The guest's own kernel reads its first virtual
	# terminal, the keyboard's, and says whether a key came; with no
	# boot_key none does, and then `hull type` types a line into it,
	# `hull key` presses its Enter, and `hull screenshot` takes its screen.
	cat > "$tree/init.sh" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t devtmpfs none /dev 2>/dev/null
if read -t 10 line < /dev/tty1; then echo "key=pressed"; else echo "key=none"; fi
echo "ready-to-type"
read -t 60 line < /dev/tty1 && echo "typed=$line"
/bin/busybox poweroff -f
INIT
	out=$(timeout 120 "$hull" export --bootable "$tree" "$root/key.img" --init /init.sh 2>&1)
	status 'a bootable export for the keyboard' "$?" 0 "$out"
	conf '(source "image:./key.img") (memory "512M") (tuning (pc (firmware uefi) (boot_key 60)))'
	out=$(timeout 120 "$hull" run -f "$root/hull.conf" 2>&1)
	status 'a machine pressed a key' "$?" 0 "$out"
	says 'its kernel read Enter from its keyboard' "$out" 'key=pressed'
	conf '(source "image:./key.img") (memory "512M") (tuning (pc (firmware uefi)))'
	timeout 120 "$hull" run -f "$root/hull.conf" > "$root/key.out" 2>&1 &
	pid=$!
	n=0
	while [ "$n" -lt 60 ] && ! grep -qa ready-to-type "$root/key.out"; do
		sleep 1
		n=$((n + 1))
	done
	says 'with no boot_key, no key came' "$(cat "$root/key.out")" 'key=none'
	id=$("$hull" ps | awk '$5 == "running" { print $1 }')
	rm -f "$root/screen.png"
	out=$("$hull" screenshot "$id" "$root/screen.png" 2>&1)
	status 'hull screenshot' "$?" 0 "$out"
	cases=$((cases + 1))
	[ "$(od -An -tx1 -N4 "$root/screen.png" 2>/dev/null | tr -d ' ')" = 89504e47 ] ||
		fail "the screenshot is not a PNG" "$(ls -l "$root/screen.png" 2>&1)"
	out=$("$hull" key "$id" no-such-key 2>&1)
	status 'a key qemu has no name for is refused' "$?" 125 "$out"
	out=$("$hull" type "$id" 'Hi, World!~' 2>&1)
	status 'hull type' "$?" 0 "$out"
	out=$("$hull" key "$id" ret 2>&1)
	status 'hull key' "$?" 0 "$out"
	wait "$pid"
	says 'the guest read what was typed, shifted and all' "$(cat "$root/key.out")" 'typed=Hi, World!~'

	# The serial bridge: a shell on COM1, driven from the host through
	# `hull exec ... console`, with nothing of hull's in the guest.
	cat > "$tree/init.sh" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t devtmpfs none /dev
/bin/busybox --install -s /bin
echo console-ready
while :; do sh < /dev/ttyS0 > /dev/ttyS0 2>&1; done
INIT
	out=$(timeout 120 "$hull" export --bootable "$tree" "$root/shell.img" --init /init.sh 2>&1)
	status 'a system with a shell on its serial port' "$?" 0 "$out"
	conf '(source "image:./shell.img") (memory "512M") (tuning (pc (firmware uefi)))'
	timeout 90 "$hull" run -f "$root/hull.conf" > "$root/shell.out" 2>&1 &
	pid=$!
	n=0
	while [ "$n" -lt 60 ] && ! grep -qa console-ready "$root/shell.out"; do
		sleep 1
		n=$((n + 1))
	done
	id=$("$hull" ps | awk '$5 == "running" { print $1 }')
	out=$(timeout 10 "$hull" exec "$id" -- ls 2>&1)
	status 'a command is refused' "$?" 125 "$out"
	says 'and the console named' "$out" 'console'
	# Its input ending ends nothing; the machine's end ends it.
	out=$( { sleep 1; printf 'echo marker-$((40 + 2))\n'; sleep 2; printf 'poweroff -f\n'; sleep 20; } |
		timeout 40 "$hull" exec "$id" -- console 2>&1)
	status 'a console ends with its machine' "$?" 0 "$out"
	says 'what was typed ran in the guest' "$out" 'marker-42'
	wait "$pid"
	status 'and that was the guest powering off' "$?" 0 "$(tail -3 "$root/shell.out")"

	# The door: real ssh, through `hull proxy-ssh`, to the host's OpenSSH
	# in front of that serial port, the machine's host key pinned strictly.
	if [ -x /usr/sbin/sshd ] && command -v ssh > /dev/null; then
		mkdir -p "$root/home/.ssh"
		chmod 700 "$root/home" "$root/home/.ssh"
		ssh-keygen -q -t ed25519 -N '' -f "$root/home/.ssh/id_ed25519"
		conf '(source "image:./shell.img") (memory "512M") (ssh true) (tuning (pc (firmware uefi)))'
		# The core keeps a host key in memory only, so a tmpfs of the user's.
		XDG_RUNTIME_DIR=$(mktemp -d /dev/shm/hull-pc.XXXXXX)
		door_run=$XDG_RUNTIME_DIR
		HOME=$root/home timeout 90 "$hull" run -f "$root/hull.conf" > "$root/door.out" 2>&1 &
		pid=$!
		n=0
		while [ "$n" -lt 60 ] && ! grep -qa console-ready "$root/door.out"; do
			sleep 1
			n=$((n + 1))
		done
		id=$("$hull" ps | awk '$5 == "running" { print $1 }')
		pub=$(find "$XDG_DATA_HOME/hull/machines/$id" -name host_key.pub)
		printf '%s.hull %s\n' "$id" "$(cut -d' ' -f1,2 "$pub")" > "$root/known"
		cat > "$root/door" <<WRAP
#!/bin/sh
exec "$(command -v ssh)" -F /dev/null -i "$root/home/.ssh/id_ed25519" -o IdentitiesOnly=yes \
	-o BatchMode=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile="$root/known" \
	-o ProxyCommand="$hull proxy-ssh %h" "\$@"
WRAP
		chmod +x "$root/door"
		me=$(id -un)
		# It is a door to a serial port and no login to the host: a command
		# or sftp asked for runs nothing here, and no port is forwarded.
		timeout 20 "$root/door" -T "$me@$id.hull" touch "$root/pwned" > /dev/null 2>&1
		cases=$((cases + 1))
		[ ! -e "$root/pwned" ] || fail 'a command does not run on the host' ''
		out=$(timeout 20 "$root/door" -T -s "$me@$id.hull" sftp < /dev/null 2>&1 | head -c 200)
		cases=$((cases + 1))
		case $out in
		*SSH*|*sftp*version*) fail 'sftp is not served' "$out" ;;
		esac
		# A listener of the test's own, so the case cannot pass for want of one.
		canary=$((30000 + $$ % 15000))
		timeout 15 python3 -I -c '
import socket, sys
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1])))
s.listen(1)
c, _ = s.accept()
c.sendall(b"canary\n")' "$canary" &
		listener=$!
		sleep 1
		timeout 8 "$root/door" -T -N -L "127.0.0.1:$((canary + 1)):127.0.0.1:$canary" "$me@$id.hull" \
			> /dev/null 2>&1 < /dev/null &
		sleep 4
		out=$(timeout 3 nc -w 2 127.0.0.1 $((canary + 1)) < /dev/null 2>&1)
		cases=$((cases + 1))
		case $out in
		*canary*) fail 'nothing is forwarded through the door' "$out" ;;
		esac
		kill "$listener" 2> /dev/null
		wait "$listener" 2> /dev/null
		sleep 4
		# hull ssh, as anyone would: the user logged in, by this ssh.
		mkdir -p "$root/doorbin"
		ln -s "$root/door" "$root/doorbin/ssh"
		out=$( { sleep 4; printf 'echo viahull-$((1 + 1))\n'; sleep 3; } |
			PATH=$root/doorbin:$PATH timeout 10 script -qec "$hull ssh $id" /dev/null 2>&1)
		says 'hull ssh comes through it too' "$out" 'viahull-2'
		out=$( { sleep 3; printf 'echo door-$((40 + 3))\n'; sleep 2; printf 'poweroff -f\n'; sleep 15; } |
			timeout 40 "$root/door" -tt "$me@$id.hull" 2>&1)
		says 'ssh through the door ran what was typed' "$out" 'door-43'
		wait "$pid"
		status 'and powered the guest off' "$?" 0 "$(tail -3 "$root/door.out")"
		XDG_RUNTIME_DIR=$root/run
		sleep 2
		rm -rf "$door_run"
	else
		printf 'pc: no sshd on this host, the door not tried\n'
	fi

	# Two of them on a wire of hull's: a legacy machine is a host of the
	# networks hull simulates. Each sets its own address, as it must.
	cat > "$tree/init.sh" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox --install -s /bin
insmod /e1000.ko
ip link set eth0 up
ip addr add 10.77.0.1/24 dev eth0
nc -l -p 7001
echo served on the wire
poweroff -f
INIT
	out=$(timeout 120 "$hull" export --bootable "$tree" "$root/one.img" --init /init.sh 2>&1)
	status 'a first end of a wire' "$?" 0 "$out"
	cat > "$tree/init.sh" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox --install -s /bin
insmod /e1000.ko
ip link set eth0 up
ip addr add 10.77.0.2/24 dev eth0
n=0
until ping -c 1 -W 1 10.77.0.1 > /dev/null 2>&1 || [ "$n" = 30 ]; do n=$((n + 1)); done
sleep 2
echo over the wire | nc -w 2 10.77.0.1 7001
poweroff -f
INIT
	out=$(timeout 120 "$hull" export --bootable "$tree" "$root/two.img" --init /init.sh 2>&1)
	status 'a second' "$?" 0 "$out"
	for end in one two; do
		cat <<CONF
  ($end (backend pc) (source "image:./$end.img") (memory "512M") (segments "pcwire")
    (tuning (pc (nic e1000) (firmware uefi))))
CONF
	done > "$root/ends"
	{ printf '(machines\n'; cat "$root/ends"; printf ')\n'; } > "$root/wire.conf"
	out=$(timeout 150 "$hull" run -f "$root/wire.conf" 2>&1)
	got=$?
	status 'two machines on a wire end' "$got" 0 "$(printf '%s\n' "$out" | tail -5)"
	says 'one heard the other' "$out" 'over the wire'
else
	printf 'pc: no busybox or e1000 module, the linux not tried\n'
fi

printf 'pc: %s cases, %s failed\n' "$cases" "$failures"
[ "$failures" = 0 ] && rm -rf "$dir"
[ "$failures" = 0 ]
