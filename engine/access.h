/*
 * access.h -- reading a plan, for engines (hull_engine.h: "accessor
 * functions are provided by the core").
 *
 * A plan is `(plan (class ...) (machine ...))` in canonical form, already
 * checked by the planner (project.md section 4.7), so these look things
 * up and copy them out and check nothing about their shape.
 */

#ifndef HULL_ACCESS_H
#define HULL_ACCESS_H

#include "sexpr.h"
#include "stack.h"
#include "hostdev.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The entry `(key ...)` among `list`'s elements after its first. */
int hull_access_find(const hull_sexpr_doc *doc, const hull_sexpr_form *list, const char *key,
                     hull_sexpr_form *entry);

/* An atom's bytes as a C string the caller frees, or NULL if it holds a
 * NUL byte or memory ran out. */
char *hull_access_text(const hull_sexpr_doc *doc, const hull_sexpr_form *form);

/* The value of `(key value)` in `record`, as hull_access_text() gives it,
 * or NULL where there is none. */
char *hull_access_value(const hull_sexpr_doc *doc, const hull_sexpr_form *record,
                        const char *key);

/* `machine`'s (devices ...), each kind's in the order written, into
 * `out`, `*count` of them: NULL, or why it cannot be had. */
const char *hull_access_devices(const hull_sexpr_doc *doc, const hull_sexpr_form *machine,
                                hull_device out[HULL_MAX_DEVICES], unsigned *count);

/* Whether `(key true)` is in `record`. */
int hull_access_true(const hull_sexpr_doc *doc, const hull_sexpr_form *record, const char *key);

/* Whether an exec, `(exec ... (tty ...))`, asks for a terminal: 1 with
 * its size from `(tty (cols n) (rows n))`, else 0. The core has checked
 * both are 1 to 65535. */
int hull_access_terminal(const hull_sexpr_doc *doc, const hull_sexpr_form *exec, int *cols,
                         int *rows);

/* `path` as the plan means it: relative to its `(base ...)`, if it has one
 * and the path is not absolute. The caller frees it; NULL if memory ran
 * out. */
char *hull_access_path(const hull_sexpr_doc *doc, const char *path);

/* A volume's (layers "dir:..." ...) and (upper PATH), into `stack`, its
 * work directory beside the upper and `readonly` its mode's; a volume
 * with no layers leaves `stack` empty. NULL, or why it cannot be read. */
const char *hull_access_stack(const hull_sexpr_doc *doc, const hull_sexpr_form *volume,
                              int readonly, hull_stack *stack);

/* The MAC of a machine's NIC on segment `segment`, its `index`th: qemu's
 * prefix, 52:54:00, then three bytes of FNV-1a over the machine's
 * `address`, the segment and the index, so that two machines, or two NICs
 * of one, do not share one, and a machine keeps its own on every engine. */
void hull_access_segment_mac(const char *address, const char *segment, unsigned index,
                             unsigned char mac[6]);

/* A (segments ...) element's name: the element, or the head of a
 * `(NAME (subnet ...))`, as hull_access_text() gives it. */
char *hull_access_segment_name(const hull_sexpr_doc *doc, const hull_sexpr_form *item);

/* The machine's address on `segment` from `names`, a plan's `(names ...)`
 * (Appendix D), into `address` with its prefix: 1, or 0 where hull does
 * not address that segment. */
int hull_access_named(const hull_sexpr_doc *doc, const hull_sexpr_form *names,
                      const char *segment, char address[16], unsigned *prefix);

/* Its IPv6 address there, where the segment has an IPv6 network as well:
 * 1, or 0 where it has none. */
int hull_access_named6(const hull_sexpr_doc *doc, const hull_sexpr_form *names,
                       const char *segment, char address[46], unsigned *prefix);

/* Whether `names` makes the segment's .1 the machine's gateway, `(gateway
 * true)`, and so its default route where it has no other: 1 or 0. */
int hull_access_gateway(const hull_sexpr_doc *doc, const hull_sexpr_form *names,
                        const char *segment);

/* The /etc/resolv.conf `names` gives a machine whose only network is its
 * segments: a nameserver at .1 of each addressed segment, hull's (Appendix
 * D), or at ::1 of its IPv6 network where it has no IPv4 one, at most
 * three, and `search hull`; as a C string the caller frees, or NULL where
 * it addresses none or memory ran out. */
char *hull_access_resolver(const hull_sexpr_doc *doc, const hull_sexpr_form *names);

/* The /etc/hosts `names` gives the machine: localhost, then a line for
 * each machine on each segment, `ADDRESS ID ID.hull`, its IPv6 address's
 * after its IPv4 one's where the segment has both, as a C string the
 * caller frees; NULL if memory ran out or `names` is not one. */
char *hull_access_hosts(const hull_sexpr_doc *doc, const hull_sexpr_form *names);

/* The secrets the plan's machine puts in its environment, `(expose env)`
 * (J.3), each as `NAME=VALUE`, VALUE read from the file the core wrote in
 * the plan's `(secrets DIR)`: `each` is called with one, which it does
 * not keep. Returns 0, or -1 when one cannot be read or holds a NUL. */
int hull_access_exposed(const hull_sexpr_doc *doc, const hull_sexpr_form *machine,
                        void (*each)(void *context, const char *pair), void *context);

/* `argv` executed with `envp`, as docker runs a container's command: a
 * file with a slash in it as it is, and any other searched for in the
 * PATH `envp` gives, or docker's default where it gives none, the first
 * that runs taken; where `envp` gives none, that default is added to it,
 * as docker gives a container one. Returns only when none ran, with errno EACCES where one
 * was found and could not be run, else ENOENT, or what the last said. It
 * allocates nothing, so a child may call it between fork and exec. */
int hull_access_execute(char *const argv[], char *const envp[]);

/* Open `plan` and find its machine. Returns 0 if it is not a plan. */
int hull_access_machine(hull_sexpr_doc *doc, const uint8_t *plan, size_t length,
                        hull_sexpr_form *machine);

#ifdef __cplusplus
}
#endif

#endif /* HULL_ACCESS_H */
