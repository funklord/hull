/*
 * segment.h -- Appendix D's segments, the layer 2 a machine's NIC shares
 * with others: a learning switch in the core for each, named by the
 * machines that join it.
 */

#ifndef HULL_SEGMENT_H
#define HULL_SEGMENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A new port on the segment `key`, made if there is none: the machine's
 * end of a stream socket, framed as qemu's `-netdev stream` frames it,
 * each frame after its length as 32 bits in network order. The port goes
 * when that end closes, and the segment with its last port. The fd, or -1
 * and the reason in `why`.
 */
int hull_segment_port(const char *key, char *why, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HULL_SEGMENT_H */
