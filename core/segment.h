/*
 * segment.h -- Appendix D's segments, the layer 2 a machine's NIC shares
 * with others: a learning switch in the core for each, named by the
 * machines that join it, which can be made to lose, hold back, reorder,
 * duplicate and damage what crosses it, and counts what it does.
 */

#ifndef HULL_SEGMENT_H
#define HULL_SEGMENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A direction of a port: what its machine sends, or what it is given. */
#define HULL_SEGMENT_OUT 0
#define HULL_SEGMENT_IN  1

/*
 * What one direction of a port does to a frame, in netem's terms and
 * order (tc-netem(8)), so a profile means here what it means there: a
 * link that is `down` drops it; one longer than `mtu` after its Ethernet
 * header is dropped, as a path that cannot carry it; it is lost with
 * `loss` at random, or by netem's Gilbert-Elliott model where `gemodel`
 * is set, which leaves its good state with `p` and its bad with `r`, and
 * loses with `bad_loss` (netem's 1-h) in the bad state and `good_loss`
 * (1-k) in the good; it is sent twice with `duplicate`, and a bit of its
 * payload flipped with `corrupt`; it is held back `delay_ms`, give or
 * take up to `jitter_ms`, drawn evenly, unless with `reorder` it is not
 * held and so overtakes those that are; at `rate` bits a second it waits
 * for the link; and a direction holds at most `limit` frames, netem's
 * 1000 where it is 0, dropping one more. Probabilities are from 0 to 1;
 * all zero is a wire. The draws come from `seed`, mixed with the machine
 * and the direction, so the same frames in the same order meet the same
 * fate.
 */
typedef struct hull_impairment {
	int      down;
	unsigned mtu;
	double   loss;
	int      gemodel;
	double   p;
	double   r;
	double   bad_loss;
	double   good_loss;
	double   duplicate;
	double   corrupt;
	unsigned delay_ms;
	unsigned jitter_ms;
	double   reorder;
	uint64_t rate;
	unsigned limit;
	uint64_t seed;
} hull_impairment;

/*
 * A targeted fault on one direction of a link, for testing a protocol
 * against a hostile wire rather than against random noise. A frame is
 * matched when `match_length` is 0, or its Ethernet payload (past the
 * 14-byte header) has `match` at byte `match_offset`. A matched frame is,
 * with probability `drop`, dropped; else, with `rewrite`, its payload
 * bytes at `set_offset` overwritten with `set`; else, with `flip`, one
 * bit of its payload flipped. `replay` keeps a copy and sends it again
 * `copies` times, `every_ms` apart, as a captured frame re-sent. All zero
 * does nothing. Draws share the link's seed.
 */
typedef struct hull_tamper {
	unsigned      match_offset;
	unsigned char match[32];
	unsigned      match_length;
	double        drop;
	double        rewrite;
	unsigned      set_offset;
	unsigned char set[32];
	unsigned      set_length;
	double        flip;
	double        replay;
	unsigned      copies;
	unsigned      every_ms;
} hull_tamper;

/* What one direction of a port has seen: frames and their bytes, those
 * handed on, those dropped by why, and the copies and damage made. A
 * frame dropped for a socket that would not take it is `full`. */
typedef struct hull_segment_count {
	uint64_t frames;
	uint64_t bytes;
	uint64_t passed;
	uint64_t passed_bytes;
	uint64_t lost;
	uint64_t too_big;
	uint64_t overflow;
	uint64_t full;
	uint64_t duplicated;
	uint64_t corrupted;
	uint64_t matched;      /* frames a tamper matched */
	uint64_t tampered;     /* matched frames dropped, rewritten or flipped */
	uint64_t replayed;     /* copies a tamper re-sent */
	uint64_t injected;     /* frames the switch injected toward this port (in) */
} hull_segment_count;

typedef struct hull_segment_counts {
	char               machine[64];
	hull_segment_count way[2];   /* HULL_SEGMENT_OUT, HULL_SEGMENT_IN */
} hull_segment_counts;

/*
 * A new port on the segment `key`, made if there is none, for `machine`'s
 * NIC: the machine's end of a stream socket, framed as qemu's
 * `-netdev stream` frames it, each frame after its length as 32 bits in
 * network order. The port goes when that end closes, and the segment with
 * its last port. The fd, or -1 and the reason in `why`.
 */
int hull_segment_port(const char *key, const char *machine, char *why, size_t cap);

/*
 * `impairment` for `direction` of every port `machine` has on `key`, or
 * every machine's where `machine` is NULL, replacing what it had; a port
 * the machine joins later takes it too, for as long as the segment lasts.
 * 0, or -1 and the reason in `why`: a segment no machine is on.
 */
int hull_segment_impair(const char *key, const char *machine, int direction,
                        const hull_impairment *impairment, char *why, size_t cap);

/* `tamper` for `direction` of every port `machine` has on `key`, or every
 * machine's where `machine` is NULL, replacing what it had; a port the
 * machine joins later takes it too. 0, or -1 and the reason in `why`. */
int hull_segment_tamper(const char *key, const char *machine, int direction,
                        const hull_tamper *tamper, char *why, size_t cap);

/*
 * Put a frame into the segment `key` from the switch itself, as an
 * attacker on the wire would: toward the port of `machine`, or every
 * port where `machine` is NULL. `count` frames, `every_ms` apart; the
 * first at once. Where `randomize`, each is `size` bytes of fresh pseudo-
 * random payload from `seed` rather than `frame`. `frame` is a whole
 * Ethernet frame (destination and source MACs and on), at least 14 bytes.
 * It bypasses the links' impairment and tamper, being the switch's own.
 * 0, or -1 and the reason in `why`: a segment no machine is on, or too
 * many sources. A `count` of 0 removes the machine's injectors.
 */
int hull_segment_inject(const char *key, const char *machine, const unsigned char *frame,
                        size_t length, int randomize, unsigned size, uint64_t count,
                        unsigned every_ms, uint64_t seed, char *why, size_t cap);

/* The counts of each port on `key`, at most `cap` of them: how many there
 * are, or -1 where no machine is on it. */
int hull_segment_counts_of(const char *key, hull_segment_counts *out, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* HULL_SEGMENT_H */
