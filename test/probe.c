/*
 * probe.c -- the one program in the reference engine's test rootfs.
 *
 * Linked static, since the rootfs holds nothing else. Its arguments are
 * steps, taken in order, each printing what the machine looks like from
 * inside:
 *
 *   say TEXT    TEXT on stdout        err TEXT    TEXT on stderr
 *   env NAME    NAME's value          cwd         the working directory
 *   host        the hostname          uid         the uid
 *   write PATH  whether PATH can be created
 *   remove PATH whether PATH can be removed
 *   mkdir PATH  whether PATH can be made, as a directory
 *   rmdir PATH  whether the directory PATH can be removed
 *   list PATH   the names in the directory PATH, sorted
 *   owner PATH  PATH's owner, group and mode, not following a link
 *   read PATH   PATH's first line
 *   stdin       the first line of its stdin
 *   count       all of its stdin: how many bytes, and their FNV-1a hash
 *   spew N      N bytes on stdout, from a xorshift that does not repeat
 *               in any window a ring or a buffer would have
 *   sleep N     sleep N seconds
 *   nnp         whether no_new_privs is set
 *   session     whether it leads a session of its own
 *   pid         its pid
 *   procs       how many processes /proc lists, or -1 without one
 *   orphan      leave a grandchild to the machine's init, which ends a
 *               second later, and wait two for the init to reap it
 *   exit N      exit with N           die         end by SIGTERM
 *   wait        wait for a signal, for 20 seconds at most, so that a
 *               test whose stop fails leaves no probe behind
 *   trap        catch SIGHUP, SIGINT or SIGTERM, for 20 seconds at most,
 *               and say which; the probe goes on
 *   tty         whether stdin is a terminal, its size, and whether it is
 *               this process's controlling terminal
 *   winch       wait for the terminal to be resized, as trap waits, and
 *               say its size
 *   serve P N   listen on port N, P tcp or udp, for 20 seconds at most;
 *               greet the one peer over tcp and say the line it sends,
 *               or echo the one datagram over udp and say it
 *   echo N      echo each datagram on udp port N to its sender, until
 *               one says `end`, for 20 seconds at most, naming each
 *               sender's port as it first comes, and say how many came
 *   dial A N    connect to address A, port N, over tcp within 5 seconds,
 *               and say, after A:N, the first line it sends or why it
 *               could not
 *   pour N B    listen on tcp port N, for 20 seconds at most, and send
 *               the one peer B bytes of spew's, then close; a peer that
 *               goes is said, SIGPIPE being ignored from then on
 *   drink N     listen on tcp port N, for 20 seconds at most, count what
 *               the one peer sends until it shuts its writing, as count
 *               does, and send it the count
 *   resolve A N ask the DNS server at address A for NAME's IPv4 address,
 *               within 5 seconds, and say its answer's rcode and count
 *   spin MS     use MS milliseconds of CPU time, by its own clock
 *   reap MS     spin MS in a child, and reap it
 *   hold N      take N MiB and touch every page, held until it exits
 *   fill PATH N write N MiB to PATH, and sync it
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t caught;

static void catch(int signal)
{
	caught = signal;
}

/* Wait for one of `signals`, for 20 seconds at most; the one caught, or 0. */
static int await(const int *signals, unsigned n)
{
	struct sigaction action;

	memset(&action, 0, sizeof action);
	action.sa_handler = catch;
	caught = 0;
	for (unsigned i = 0u; i < n; i++)
		sigaction(signals[i], &action, NULL);
	printf("ready\n");
	alarm(20);
	while (caught == 0)
		pause();
	alarm(0);
	return caught;
}

static void print_size(void)
{
	struct winsize size;

	if (ioctl(0, TIOCGWINSZ, &size) == 0)
		printf("size=%ux%u\n", (unsigned)size.ws_col, (unsigned)size.ws_row);
	else
		printf("size=%s\n", strerror(errno));
}

/* `left` bytes from a xorshift that does not repeat in any window a ring
 * or a buffer would have, to `fd`; 0 if it would not take them. */
static int spew_to(int fd, unsigned long long left)
{
	unsigned      x = 1u;
	unsigned char buffer[4096];

	while (left > 0u) {
		size_t n = left < sizeof buffer ? (size_t)left : sizeof buffer;

		for (size_t j = 0u; j < n; j++) {
			x ^= x << 13;
			x ^= x >> 17;
			x ^= x << 5;
			buffer[j] = (unsigned char)x;
		}
		for (size_t done = 0u; done < n;) {
			ssize_t w = write(fd, buffer + done, n - done);

			if (w < 0 && errno == EINTR)
				continue;
			if (w < 0)
				return 0;
			done += (size_t)w;
		}
		left -= n;
	}
	return 1;
}

/* All of `fd` until its end, as "bytes=N fnv=H", into `out`. */
static void count_from(int fd, char *out, size_t cap)
{
	unsigned long long total = 0u;
	unsigned           hash  = 2166136261u;
	char               text[4096];
	ssize_t            n;

	while ((n = read(fd, text, sizeof text)) > 0 || (n < 0 && errno == EINTR))
		for (ssize_t j = 0; j < n; j++, total++)
			hash = (hash ^ (unsigned char)text[j]) * 16777619u;
	snprintf(out, cap, "bytes=%llu fnv=%08x", total, hash);
}

/* The one peer of tcp port `port`, within 20 seconds, or -1, said why. */
static int accept_one(const char *port)
{
	int                one = 1;
	int                fd  = socket(AF_INET, SOCK_STREAM, 0);
	int                peer;
	struct sockaddr_in at;
	struct pollfd      p;

	memset(&at, 0, sizeof at);
	at.sin_family = AF_INET;
	at.sin_port   = htons((unsigned short)atoi(port));
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (fd < 0 || bind(fd, (struct sockaddr *)&at, sizeof at) != 0 || listen(fd, 1) != 0) {
		printf("serve=%s\n", strerror(errno));
		return -1;
	}
	printf("serving\n");
	p.fd     = fd;
	p.events = POLLIN;
	peer = poll(&p, 1u, 20000) == 1 ? accept(fd, NULL, NULL) : -1;
	if (peer < 0)
		printf("serve=nobody came\n");
	close(fd);
	return peer;
}

/* A line from `fd` within `seconds`, without its newline, into `out`. */
static void read_line(int fd, char *out, size_t cap, int seconds)
{
	size_t        n = 0u;
	struct pollfd p = { fd, POLLIN, 0 };

	while (n + 1u < cap && poll(&p, 1u, seconds * 1000) == 1) {
		char c;

		if (read(fd, &c, 1u) != 1 || c == '\n')
			break;
		out[n++] = c;
	}
	out[n] = '\0';
}

static void echo(const char *port)
{
	static char        datagram[65536];
	struct sockaddr_in senders[16];
	unsigned           count = 0u;
	int                fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in at;
	time_t             end = time(NULL) + 20;

	memset(&at, 0, sizeof at);
	at.sin_family = AF_INET;
	at.sin_port   = htons((unsigned short)atoi(port));
	if (fd < 0 || bind(fd, (struct sockaddr *)&at, sizeof at) != 0) {
		printf("echo=%s\n", strerror(errno));
		return;
	}
	printf("echoing\n");
	fflush(stdout);
	for (;;) {
		struct pollfd      p = { fd, POLLIN, 0 };
		struct sockaddr_in from;
		socklen_t          length = sizeof from;
		ssize_t            n;
		unsigned           i;

		if (time(NULL) >= end || poll(&p, 1u, 1000) < 0)
			break;
		if (p.revents == 0)
			continue;
		n = recvfrom(fd, datagram, sizeof datagram, 0, (struct sockaddr *)&from, &length);
		if (n < 0)
			continue;
		sendto(fd, datagram, (size_t)n, 0, (struct sockaddr *)&from, length);
		for (i = 0u; i < count; i++)
			if (senders[i].sin_port == from.sin_port &&
			    senders[i].sin_addr.s_addr == from.sin_addr.s_addr)
				break;
		if (i == count && count < 16u) {
			senders[count++] = from;
			printf("from %u\n", (unsigned)ntohs(from.sin_port));
		}
		if (n == 3 && memcmp(datagram, "end", 3u) == 0)
			break;
	}
	printf("echoed from %u\n", count);
	close(fd);
}

static void serve(const char *proto, const char *port)
{
	int                udp = strcmp(proto, "udp") == 0;
	int                one = 1;
	int                fd  = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
	struct sockaddr_in at;
	struct pollfd      p;
	char               text[256];

	memset(&at, 0, sizeof at);
	at.sin_family = AF_INET;
	at.sin_port   = htons((unsigned short)atoi(port));
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (fd < 0 || bind(fd, (struct sockaddr *)&at, sizeof at) != 0 ||
	    (!udp && listen(fd, 1) != 0)) {
		printf("serve=%s\n", strerror(errno));
		return;
	}
	printf("serving\n");
	p.fd     = fd;
	p.events = POLLIN;
	if (poll(&p, 1u, 20000) != 1) {
		printf("serve=nobody came\n");
	} else if (udp) {
		struct sockaddr_in from;
		socklen_t          length = sizeof from;
		ssize_t            n = recvfrom(fd, text, sizeof text - 1u, 0,
		                                (struct sockaddr *)&from, &length);

		if (n < 0)
			n = 0;
		text[n] = '\0';
		sendto(fd, text, (size_t)n, 0, (struct sockaddr *)&from, length);
		printf("served=%s\n", text);
	} else {
		int peer = accept(fd, NULL, NULL);

		if (write(peer, "hello from the machine\n", 23u) != 23)
			printf("serve=%s\n", strerror(errno));
		read_line(peer, text, sizeof text, 5);
		printf("served=%s\n", text);
		close(peer);
	}
	close(fd);
}

static void dial(const char *address, const char *port)
{
	int                fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	struct sockaddr_in to;
	struct pollfd      p;
	int                error = 0;
	socklen_t          length = sizeof error;
	char               text[256];

	memset(&to, 0, sizeof to);
	to.sin_family = AF_INET;
	to.sin_port   = htons((unsigned short)atoi(port));
	inet_pton(AF_INET, address, &to.sin_addr);
	if (connect(fd, (struct sockaddr *)&to, sizeof to) != 0 && errno != EINPROGRESS) {
		printf("dial %s:%s=%s\n", address, port, strerror(errno));
		close(fd);
		return;
	}
	p.fd     = fd;
	p.events = POLLOUT;
	if (poll(&p, 1u, 5000) != 1) {
		printf("dial %s:%s=timed out\n", address, port);
	} else if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
		printf("dial %s:%s=%s\n", address, port, strerror(error));
	} else {
		read_line(fd, text, sizeof text, 5);
		printf("dial %s:%s=%s\n", address, port, text);
	}
	close(fd);
}

static void resolve(const char *server, const char *name)
{
	int                fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in to;
	struct pollfd      p;
	uint8_t            query[300] = { 0x68, 0x75, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0 };
	uint8_t            answer[1500];
	size_t             n = 12u;
	ssize_t            got;

	/* The name as labels, then type A, class IN. */
	for (const char *label = name; *label != '\0' && n < sizeof query - 70u;) {
		size_t length = strcspn(label, ".");

		query[n++] = (uint8_t)length;
		memcpy(query + n, label, length);
		n += length;
		label += length;
		if (*label == '.')
			label++;
	}
	query[n++] = 0;
	query[n++] = 0;
	query[n++] = 1;
	query[n++] = 0;
	query[n++] = 1;
	memset(&to, 0, sizeof to);
	to.sin_family = AF_INET;
	to.sin_port   = htons(53);
	inet_pton(AF_INET, server, &to.sin_addr);
	if (sendto(fd, query, n, 0, (struct sockaddr *)&to, sizeof to) != (ssize_t)n) {
		printf("resolve=%s\n", strerror(errno));
		close(fd);
		return;
	}
	p.fd     = fd;
	p.events = POLLIN;
	if (poll(&p, 1u, 5000) != 1 || (got = recv(fd, answer, sizeof answer, 0)) < 12 ||
	    answer[0] != 0x68 || answer[1] != 0x75)
		printf("resolve=no answer\n");
	else
		printf("resolve=rcode %d answers %d\n", answer[3] & 0x0f, answer[6] << 8 | answer[7]);
	close(fd);
}

/* Until this process has used `ms` of CPU time. */
static void spin(long ms)
{
	struct timespec now;
	long long       until;
	volatile unsigned long turns = 0u;

	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
	until = (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000 + ms;
	do {
		for (unsigned i = 0u; i < 100000u; i++)
			turns++;
		clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
	} while ((long long)now.tv_sec * 1000 + now.tv_nsec / 1000000 < until);
}

int main(int argc, char **argv)
{
	char text[256];

	setvbuf(stdout, NULL, _IONBF, 0);
	/* Run as a machine's /bin/sh, as `hull ssh` runs one: what it was
	 * given, and whether that is on a terminal. */
	/* And as a build step's shell, `/bin/sh -c "probe STEP ..."`: the
	 * line's words after `probe` are its steps, so that a build runs with
	 * no shell in the tree. */
	if (strcmp(argv[0], "/bin/sh") == 0 && argc == 3 && strcmp(argv[1], "-c") == 0 &&
	    strncmp(argv[2], "probe ", 6u) == 0) {
		static char *words[64];
		int          n = 1;

		words[0] = argv[0];
		for (char *word = strtok(argv[2] + 6, " "); word != NULL && n < 63;
		     word = strtok(NULL, " "))
			words[n++] = word;
		words[n] = NULL;
		argv = words;
		argc = n;
	} else if (strcmp(argv[0], "/bin/sh") == 0) {
		for (int i = 1; i < argc; i++)
			printf("sh %d=%s\n", i, argv[i]);
		printf("tty=%s\n", isatty(0) ? "yes" : "no");
		return 0;
	}
	for (int i = 1; i < argc; i++) {
		const char *step = argv[i];
		const char *arg  = i + 1 < argc ? argv[i + 1] : "";

		if (strcmp(step, "say") == 0) {
			printf("%s\n", arg);
			i++;
		} else if (strcmp(step, "err") == 0) {
			fprintf(stderr, "%s\n", arg);
			i++;
		} else if (strcmp(step, "env") == 0) {
			printf("%s=%s\n", arg, getenv(arg) != NULL ? getenv(arg) : "(unset)");
			i++;
		} else if (strcmp(step, "cwd") == 0) {
			printf("cwd=%s\n", getcwd(text, sizeof text) != NULL ? text : "(unknown)");
		} else if (strcmp(step, "host") == 0) {
			printf("host=%s\n", gethostname(text, sizeof text) == 0 ? text : "(unknown)");
		} else if (strcmp(step, "uid") == 0) {
			printf("uid=%u\n", (unsigned)getuid());
		} else if (strcmp(step, "write") == 0) {
			int fd = open(arg, O_WRONLY | O_CREAT | O_TRUNC, 0644);

			printf("write=%s\n", fd >= 0 ? "ok" : strerror(errno));
			if (fd >= 0)
				close(fd);
			i++;
		} else if (strcmp(step, "remove") == 0) {
			printf("remove=%s\n", unlink(arg) == 0 ? "ok" : strerror(errno));
			i++;
		} else if (strcmp(step, "mkdir") == 0) {
			printf("mkdir=%s\n", mkdir(arg, 0755) == 0 ? "ok" : strerror(errno));
			i++;
		} else if (strcmp(step, "rmdir") == 0) {
			printf("rmdir=%s\n", rmdir(arg) == 0 ? "ok" : strerror(errno));
			i++;
		} else if (strcmp(step, "list") == 0) {
			struct dirent **names;
			int             n = scandir(arg, &names, NULL, alphasort);

			if (n < 0) {
				printf("list=%s\n", strerror(errno));
			} else {
				printf("list=");
				for (int k = 0; k < n; k++) {
					if (strcmp(names[k]->d_name, ".") != 0 && strcmp(names[k]->d_name, "..") != 0)
						printf(" %s", names[k]->d_name);
					free(names[k]);
				}
				free(names);
				printf("\n");
			}
			i++;
		} else if (strcmp(step, "owner") == 0) {
			struct stat file;

			if (lstat(arg, &file) != 0)
				printf("owner=%s\n", strerror(errno));
			else
				printf("owner=%u:%u %04o\n", (unsigned)file.st_uid, (unsigned)file.st_gid,
				       (unsigned)(file.st_mode & 07777));
			i++;
		} else if (strcmp(step, "session") == 0) {
			printf("leader=%d\n", getsid(0) == getpid());
		} else if (strcmp(step, "read") == 0) {
			FILE *file = fopen(arg, "r");

			if (file == NULL || fgets(text, sizeof text, file) == NULL)
				printf("read=%s\n", strerror(errno));
			else
				printf("read=%s", text);
			if (file != NULL)
				fclose(file);
			i++;
		} else if (strcmp(step, "stdin") == 0) {
			ssize_t n = read(0, text, sizeof text - 1u);

			text[n > 0 ? n : 0] = '\0';
			printf("stdin=%s", n > 0 ? text : "(none)\n");
		} else if (strcmp(step, "count") == 0) {
			count_from(0, text, sizeof text);
			printf("%s\n", text);
		} else if (strcmp(step, "spew") == 0) {
			if (!spew_to(1, strtoull(arg, NULL, 10)))
				return 1;
			i++;
		} else if (strcmp(step, "pour") == 0 && i + 2 < argc) {
			int peer = accept_one(arg);

			/* A peer that goes is an error to report, as a server
			 * would, rather than the end of the probe. */
			signal(SIGPIPE, SIG_IGN);

			if (peer >= 0) {
				printf("poured=%s\n", spew_to(peer, strtoull(argv[i + 2], NULL, 10)) ?
				                      argv[i + 2] : strerror(errno));
				close(peer);
			}
			i += 2;
		} else if (strcmp(step, "drink") == 0) {
			int peer = accept_one(arg);

			if (peer >= 0) {
				count_from(peer, text, sizeof text);
				dprintf(peer, "%s\n", text);
				printf("drank %s\n", text);
				close(peer);
			}
			i++;
		} else if (strcmp(step, "spin") == 0) {
			spin(atol(arg));
			printf("spun=%s\n", arg);
			i++;
		} else if (strcmp(step, "reap") == 0) {
			pid_t child = fork();

			if (child == 0) {
				spin(atol(arg));
				_exit(0);
			}
			waitpid(child, NULL, 0);
			printf("reaped=%s\n", arg);
			i++;
		} else if (strcmp(step, "hold") == 0) {
			size_t         n = (size_t)atol(arg) << 20;
			volatile char *held = malloc(n);

			for (size_t at = 0u; held != NULL && at < n; at += 4096u)
				held[at] = 1;
			printf("held=%s\n", held != NULL ? arg : "no");
			i++;
		} else if (strcmp(step, "fill") == 0 && i + 2 < argc) {
			static char block[1u << 20];
			int         fd = open(arg, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			long        n = atol(argv[i + 2]);
			long        done = 0;

			memset(block, 'f', sizeof block);
			while (fd >= 0 && done < n && write(fd, block, sizeof block) == (ssize_t)sizeof block)
				done++;
			/* Synced, so that it has reached storage, which a cgroup
			 * counts only once it has. */
			if (fd >= 0 && fsync(fd) != 0)
				done = -1;
			printf("filled=%ld\n", done);
			if (fd >= 0)
				close(fd);
			i += 2;
		} else if (strcmp(step, "sleep") == 0) {
			sleep((unsigned)atoi(arg));
			i++;
		} else if (strcmp(step, "pid") == 0) {
			printf("pid=%d\n", (int)getpid());
		} else if (strcmp(step, "procs") == 0) {
			DIR           *proc  = opendir("/proc");
			struct dirent *entry;
			int            count = 0;

			while (proc != NULL && (entry = readdir(proc)) != NULL)
				count += entry->d_name[0] >= '1' && entry->d_name[0] <= '9';
			printf("procs=%d\n", proc != NULL ? count : -1);
			if (proc != NULL)
				closedir(proc);
		} else if (strcmp(step, "orphan") == 0) {
			pid_t child = fork();

			if (child == 0) {
				if (fork() == 0) {
					sleep(1);
					_exit(0);
				}
				_exit(0);
			}
			waitpid(child, NULL, 0);
			sleep(2);
		} else if (strcmp(step, "nnp") == 0) {
			printf("nnp=%d\n", prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0));
		} else if (strcmp(step, "exit") == 0) {
			return atoi(arg);
		} else if (strcmp(step, "die") == 0) {
			raise(SIGTERM);
		} else if (strcmp(step, "trap") == 0) {
			static const int signals[] = { SIGHUP, SIGINT, SIGTERM };

			printf("signal=%d\n", await(signals, 3u));
		} else if (strcmp(step, "tty") == 0) {
			printf("tty=%s\n", isatty(0) ? "yes" : "no");
			print_size();
			printf("controlling=%s\n", tcgetpgrp(0) == getpgrp() ? "yes" : "no");
		} else if (strcmp(step, "winch") == 0) {
			static const int signals[] = { SIGWINCH };

			await(signals, 1u);
			print_size();
		} else if (strcmp(step, "serve") == 0 && i + 2 < argc) {
			serve(arg, argv[i + 2]);
			i += 2;
		} else if (strcmp(step, "echo") == 0) {
			echo(arg);
			i++;
		} else if (strcmp(step, "dial") == 0 && i + 2 < argc) {
			dial(arg, argv[i + 2]);
			i += 2;
		} else if (strcmp(step, "resolve") == 0 && i + 2 < argc) {
			resolve(arg, argv[i + 2]);
			i += 2;
		} else if (strcmp(step, "wait") == 0) {
			printf("waiting\n");
			alarm(20);
			pause();
		}
	}
	return 0;
}
