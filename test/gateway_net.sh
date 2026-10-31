# The host's network as a gateway's tests want it, in a namespace of the
# test's own, `unshare -rn`, where the host has no IPv6 route: a dummy
# interface with an address of each family and a default route of each,
# a listener on each answering what came, and hull run there with a core
# of its own. Its arguments: hull, the spec, and a runtime directory.
ip link set lo up
ip link add d0 type dummy && ip link set d0 up
ip addr add 192.0.2.10/24 dev d0 && ip route add default dev d0
ip -6 addr add 2001:db8:7::10/64 dev d0 nodad && ip -6 route add default dev d0
timeout -k 2 100 python3 -c '
import select, socket
four = socket.socket(socket.AF_INET)
four.bind(("192.0.2.10", 7778))
six = socket.socket(socket.AF_INET6)
six.bind(("2001:db8:7::10", 7777))
for s in (four, six):
	s.listen(8)
while True:
	for s in select.select([four, six], [], [], 90)[0]:
		c, _ = s.accept()
		c.sendall(b"out through ::1\n" if s is six else b"out through .1\n")
		c.close()
' > /dev/null 2>&1 &
listener=$!
sleep 0.5
cd / && XDG_RUNTIME_DIR="$3" HULL_CORE_IDLE=2 timeout 90 "$1" run -f "$2"
status=$?
kill "$listener" 2>/dev/null
wait "$listener" 2>/dev/null
exit "$status"
