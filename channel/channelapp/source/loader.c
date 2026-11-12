#include <sys/errno.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include <ogcsys.h>
#include <network.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <ogc/cond.h>
#include <zlib.h>

#include "../config.h"
#include "../build_tag.h"
#include "blob.h"
#include "tcp.h"
#include "appentry.h"
#include "apprun.h"
#include "manage.h"
#include "dialogs.h"
#include "i18n.h"
#include "panic.h"

#include "loader.h"
#include "netinit.h"
#include "netrecovery.h"
#include "netdiag.h"
#include "netwatch.h"
#include "netprobe.h"
#include "title.h"
#include "xml.h"
#include "devfile.h"
#include "devnet.h"
#include "bootlog.h"
#include "usbstart.h"
#include "discover.h"
#include "zmem.h"

#define USBGECKO_RETRIES 1000

static const char *text_err_read;
static const char *text_err_receive;
static const char *text_err_uncompress;
static const char *text_err_invalid_bin;
static const char *text_extract_zip;
static const char *text_err_invalid_zip;
static const char *text_err_extract_zip;
static const char *text_warn_overwrite;
static const char *text_err_oom;

static inline u16 buf_u16(const u8 *buf, u8 pos) {
	return (buf[pos] << 8) | buf[pos + 1];
}

static inline u32 buf_u32(const u8 *buf, u8 pos) {
	return (buf[pos] << 24) | (buf[pos + 1] << 16) |
			(buf[pos + 2] << 8) | buf[pos + 3];
}

// gecko handshake thread

static lwp_t ld_gecko_thread;
static u8 ld_gecko_stack[LD_THREAD_STACKSIZE] ATTRIBUTE_ALIGN (32);

typedef enum {
	LDGECKOCMD_IDLE = 0,
	LDGECKOCMD_EXIT,
	LDGECKOCMD_POLL,

} ld_gecko_cmd;

typedef struct {
	bool running;
	bool handshaked;
	ld_gecko_cmd cmd;
	u32 data_len;
	u32 data_len_un;
	u16 args_len;
	mutex_t cmutex;
	cond_t cond;
} ld_gecko_arg;

static void * ld_gecko_func (void *arg) {
	ld_gecko_arg *ta = (ld_gecko_arg *) arg;
	u8 buf[16];
	u8 left;
	int res;
	s64 t;
	u16 wiiload_version;

	ta->running = true;

	LWP_MutexLock (ta->cmutex);
	while (true) {
		usb_flush(USBGECKO_CHANNEL);

		while (ta->cmd == LDGECKOCMD_IDLE)
			LWP_CondWait(ta->cond, ta->cmutex);

		if (ta->cmd == LDGECKOCMD_EXIT)
			break;

		ta->cmd = LDGECKOCMD_IDLE;

		ta->handshaked = false;

		left = 16;
		t = gettime ();
		while (left) {
			res = usb_recvbuffer_safe_ex(USBGECKO_CHANNEL, &buf[16 - left],
										left, USBGECKO_RETRIES);

			if (res) {
				left -= res;

				continue;
			}

			if (ticks_to_millisecs (diff_ticks (t, gettime ())) > LD_TIMEOUT)
				break;
		}

		if (left)
			continue;

		wiiload_version = buf_u16(buf, 4);
		ta->args_len = buf_u16(buf, 6);
		ta->data_len = buf_u32(buf, 8);
		ta->data_len_un = buf_u32(buf, 12);

		if (strncmp((char *) buf, "HAXX", 4) ||
				(wiiload_version < WIILOAD_MIN_VERSION) ||
				(ta->args_len > ARGS_MAX_LEN) ||
				(!ta->data_len || ta->data_len > LD_MAX_SIZE) ||
				(ta->data_len_un > LD_MAX_SIZE)) {
			continue;
		}

		gprintf_enable(0);

		ta->handshaked = true;
	}

	LWP_MutexUnlock (ta->cmutex);

	gprintf("gecko handshake thread done\n");

	ta->running = false;
	return NULL;
}

// tcp handshake thread

static lwp_t ld_tcp_thread;
static u8 ld_tcp_stack[LD_THREAD_STACKSIZE] ATTRIBUTE_ALIGN (32);

typedef enum {
	LDTCPCMD_IDLE = 0,
	LDTCPCMD_EXIT,
	LDTCPCMD_INIT,
	LDTCPCMD_ACCEPT
} ld_tcp_cmd;

typedef enum {
	LDTCPS_UNINITIALIZED = 0,
	LDTCPS_INITIALIZING,
	LDTCPS_INITIALIZED
} ld_tcp_state;

typedef struct {
	bool running;
	ld_tcp_cmd cmd;
	ld_tcp_state state;
	bool handshaked;
	u16 args_len;
	u32 data_len;
	u32 data_len_un;
	int s;
	char *client;
	mutex_t cmutex;
	cond_t cond;
} ld_tcp_arg;

// Times the network came up (HBCS "net_starts": a restart adds one).
static volatile u32 net_starts;
// UDP discovery (HBCU) on the Wiiload port; reopened with each network start.
static s32 discover_fd = -1;
// The address the network came up with, 0 while it is down: HOME shows it
// without asking IOS, which may be hanging in a cleanup.
static volatile u32 net_ip;

// The keep-alive and the gateway probe (netprobe.h). The probe is a
// non-blocking TCP connect polled with net_connect, as tcp_connect does.
static s32 keepalive_fd = -1, probe_fd = -1;
static u64 probe_started, probe_polled;
#define PROBE_POLL_MS 50
// Just after the network comes up, IOS often resets its stack once: every
// socket then gets -ENETRESET (-126), 0.7-1.27 s after net_init returned.
// In sd:/hbc-network.log (375 starts, 2026-11-12) it followed 26 of 26
// starts after an app with the link still at 4 (wireless down), 65 of 320
// with it at 5, and 4 of 22 after HBC's own relaunch. A net_deinit and
// net_init in the same IOS never came back from it: each start then failed
// (-116) after about 13 s, and only a fresh IOS worked.
//
// So the listener waits for NCD's link to have been up NET_SETTLE_MS in a
// row from the start. A link that drops (4, wireless down, or 1, busy) in
// that time goes to the fresh-IOS relaunch (netinit_reset_relaunch, at most
// HBC_NET_JOIN_RELAUNCHES in a row): on 2026-11-12 such a link went 4 at
// +60 ms, 5 for 50 ms at about +1.95 s, then stayed 4 for the 13 s left of a
// 15 s wait, every time, and a clean exit by the app did not prevent it.
// Only a fresh IOS fixed it, sometimes the second or third. A reset once
// listening closes HBC's sockets, waits up to RELISTEN_LINK_MS for the link
// and listens again (net_relisten); a reset that fresh listener does not
// survive goes to the relaunch too. Where HBC may not relaunch, the old
// Restart network. For the first LINK_TRACE_MS after each start every change
// of the link is logged ("Link:").
//
// 3.5 s, not 2.5 s (1.16.4): on the second Wii (192.168.1.247, 2026-11-12)
// the link held 5 through a 2.5 s hold, then went 4 at +3.03 s; the fresh
// listener then found no address and HBC relaunched anyway, about 4 s later
// than the hold would have. These relaunches skip the countdown
// (net_relaunch_now): nothing on the network was usable yet.
#define NET_SETTLE_MS 3500
#define NET_JOIN_MS NET_SETTLE_MS   // the decision: at the end of the hold
#define LINK_TRACE_MS 30000
#define LINK_TRACE_POLL_MS 250
#define RELISTEN_LINK_MS 5000
static u64 net_up_at;
static bool relisten_used;
// The link trace after a start: the last state logged, the next sample.
static s32 trace_link;
static u32 trace_changes, trace_next_ms;
static bool trace_done;

static void discover_stop(void) {
	// The keep-alive may borrow this socket (netprobe.h): it goes too.
	if (keepalive_fd >= 0 && keepalive_fd == discover_fd)
		keepalive_fd = -1;
	if (discover_fd >= 0) {
		net_close(discover_fd);
		discover_fd = -1;
	}
}

static void netprobe_close (void);

// Restart and stop requests, read by the worker even while a busy start
// holds its mutex: the menu never waits on that mutex for them.
static volatile bool tcp_restart, tcp_stop;
// A start was cancelled by a restart or stop, or ran out of its 30 s while
// IOS was still busy: its IOS step may still be running.
static bool start_cancelled;

// libogc's net_deinit() waits for an IOS callback. The menu watches it from
// outside (netrecovery.c), so a cleanup that never returns can be escaped.
// The listener, if *listener holds one, is closed inside the watch too:
// net_close() waits on IOS as well, and a Restart used to close it first,
// where a hang had no watchdog at all.
static void tcp_net_deinit (const char *reason, int *listener) {
	net_ip = 0;
	netwatch_down();
	netdiag_event("%s: net_deinit begin", reason);
	// A restart (or exit) cancels a start between its IOS steps, and
	// net_deinit() then waits for the step in flight. That step may take most
	// of the start's own 30 s, so the 5 s cleanup watchdog would reload HBC
	// for a slow IOS that is not hung. After a cancelled start the watchdog
	// gets the start's budget; any other cleanup keeps 5 s.
	if (start_cancelled) {
		start_cancelled = false;
		netrecovery_cleanup_begin_after_start();
	} else {
		netrecovery_cleanup_begin();
	}
	// Every path that closes the network stops discovery first: its socket
	// belongs to the stack that net_deinit() takes down. Inside the watchdog,
	// because net_close() waits on IOS too.
	discover_stop();
	netprobe_close();
	if (listener && *listener >= 0) {
		net_close(*listener);
		*listener = -1;
	}
#ifdef HBC_TEST_STUCK_NET_CLEANUP
	// Test builds only: a restart's cleanup never returns, holding the mutex.
	if (!strcmp(reason, "Restart"))
		for (;;) usleep(50 * 1000);
#endif
	net_deinit();
	netrecovery_cleanup_complete();
	netdiag_event("%s: net_deinit complete", reason);
}

u32 loader_net_starts (void) {
	return net_starts;
}

u32 loader_net_ip (void) {
	return net_ip;
}

// A failed start, or a gateway still silent after an automatic Restart
// network, asks main() for a fresh-IOS relaunch (netinit.h).
static volatile bool net_relaunch_wanted;
// The relaunch is for a silent gateway (netprobe.h): the network may well be
// up, the relaunch still goes.
static volatile bool net_relaunch_gateway;
// The relaunch is for a network that never settled (net_reset_relaunch):
// main() relaunches at once, without the countdown.
static volatile bool net_relaunch_now;

bool loader_take_net_relaunch_now (void) {
	u32 level;
	bool now;

	_CPU_ISR_Disable(level);
	now = net_relaunch_now;
	net_relaunch_now = false;
	_CPU_ISR_Restore(level);
	return now;
}

bool loader_take_gateway_relaunch (void) {
	u32 level;
	bool wanted;

	_CPU_ISR_Disable(level);
	wanted = net_relaunch_gateway;
	net_relaunch_gateway = false;
	_CPU_ISR_Restore(level);
	return wanted;
}

bool loader_net_relaunch_pending (void) {
	return net_relaunch_wanted;
}

bool loader_take_net_relaunch (void) {
	u32 level;
	bool wanted;

	_CPU_ISR_Disable(level);
	wanted = net_relaunch_wanted;
	net_relaunch_wanted = false;
	_CPU_ISR_Restore(level);
	return wanted;
}

// Whether HBC may relaunch itself for the network: the installed channel,
// running as itself (a DOL started by a loader has nothing to come back to),
// unless `hbc.py net-hang wait` says never.
static bool relaunch_allowed (void) {
	return settings.net_hang != NET_HANG_WAIT && title_get_id() == MY_TITLEID;
}

// NCD's link status (netwatch.h): /dev/net/ncd/manage ioctlv 7, the call
// libogc's net_init makes first. Word 0 of the answer is NCD's result, word
// 1 the link status. Opened and closed for each call, as libogc does; the
// answer and the vector have whole cache lines of their own.
static s32 ncd_link_status (s32 *ncd_result) {
	static char path[] ATTRIBUTE_ALIGN(32) = "/dev/net/ncd/manage";
	static struct {
		u32 out[8];
		ioctlv v[1];
	} ATTRIBUTE_ALIGN(32) io;
	s32 fd, res;

	*ncd_result = 0;
	fd = IOS_Open(path, 0);
	if (fd < 0)
		return fd;
	memset(io.out, 0, sizeof(io.out));
	io.v[0].data = io.out;
	io.v[0].len = sizeof(io.out);
	DCFlushRange(&io, sizeof(io));
	res = IOS_Ioctlv(fd, 7, 0, 1, io.v);
	DCInvalidateRange(io.out, sizeof(io.out));
	IOS_Close(fd);
	if (res < 0)
		return res;
	*ncd_result = (s32) io.out[0];
	if (*ncd_result)
		return *ncd_result < 0 ? *ncd_result : -*ncd_result;
	return (s32) io.out[1];
}

// What IOS says about the network right now, for the log: NCD's link status
// and result, and the IP stack's address (0 when there is no stack).
static void net_ios_report (const char *when) {
	s32 ncd, link = ncd_link_status(&ncd);
	struct in_addr ip = { net_get_status() == 0 ? net_gethostip() : 0 };

	netdiag_event("%s: IOS link %d (%s, NCD %d), ip %s, net_get_status %d", when,
		(int) link, netwatch_link_name(link), (int) ncd, inet_ntoa(ip),
		(int) net_get_status());
}

// The idle check found the network gone (netwatch.h): from then until it is
// up again, starts are Restart network with the backoff retries only, never
// the fresh-IOS relaunch; every attempt logs what IOS reports.
static volatile bool watch_recovery;
static u32 watch_inits;
static u64 watch_lost_at;

// NCD's current configuration (/dev/net/ncd/manage ioctlv 3, GetConfig):
// out vector 0 the 0x1b5c-byte config.dat image, out vector 1 the result.
// Layout as Dolphin's WiiNetConfig: an 8-byte header, then 3 connections of
// 0x91c bytes: flags (0x80 selected, 0x04 address by DHCP), ip, netmask,
// gateway. A static selected connection whose address is ours gives the
// gateway; otherwise .1 of our /24. The image holds the Wi-Fi key: only the
// three addresses are read and the copy is wiped.
#define NCD_CONFIG_SIZE 0x1b5c
#define NCD_CONN_SIZE 0x91c
static u32 rd32 (const u8 *p) {
	return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
}

static u32 find_gateway (u32 ip, char *how, size_t size) {
	static char path[] ATTRIBUTE_ALIGN(32) = "/dev/net/ncd/manage";
	static struct {
		u32 result[8];
		ioctlv v[2];
	} ATTRIBUTE_ALIGN(32) io;
	u32 derived = (ip & 0xffffff00) | 1;
	u8 *cfg;
	s32 fd, res;
	int i;

	cfg = memalign(32, (NCD_CONFIG_SIZE + 31) & ~31);
	if (!cfg) {
		snprintf(how, size, "derived .1, no memory");
		return derived;
	}
	res = fd = IOS_Open(path, 0);
	if (fd >= 0) {
		memset(cfg, 0, NCD_CONFIG_SIZE);
		memset(io.result, 0, sizeof(io.result));
		io.v[0].data = cfg;
		io.v[0].len = NCD_CONFIG_SIZE;
		io.v[1].data = io.result;
		io.v[1].len = sizeof(io.result);
		DCFlushRange(cfg, NCD_CONFIG_SIZE);
		DCFlushRange(&io, sizeof(io));
		res = IOS_Ioctlv(fd, 3, 0, 2, io.v);
		DCInvalidateRange(cfg, NCD_CONFIG_SIZE);
		DCInvalidateRange(io.result, sizeof(io.result));
		IOS_Close(fd);
		if (res >= 0 && io.result[0])
			res = -(s32) (io.result[0] & 0x7fffffff);
	}
	if (res < 0) {
		snprintf(how, size, "derived .1, NCD config %d", (int) res);
	} else {
		snprintf(how, size, "derived .1, no selected connection");
		for (i = 0; i < 3; i++) {
			const u8 *c = cfg + 8 + i * NCD_CONN_SIZE;
			u32 cip = rd32(c + 4), mask = rd32(c + 8), gw = rd32(c + 12);

			if (!(c[0] & 0x80))
				continue;
			if (c[0] & 0x04) {
				snprintf(how, size, "derived .1, DHCP");
				break;
			}
			if (cip == ip && mask && gw && gw != ip && (gw & mask) == (ip & mask) &&
					(gw & ~mask) != ~mask) {
				snprintf(how, size, "NCD static config");
				derived = gw;
			} else {
				snprintf(how, size, "derived .1, NCD static config unusable");
			}
			break;
		}
	}
	memset(cfg, 0, NCD_CONFIG_SIZE);
	free(cfg);
	return derived;
}

static s32 nonblocking_socket (u32 type) {
	s32 fd = net_socket(PF_INET, type, 0), flags;

	if (fd < 0)
		return fd;
	flags = net_fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		flags = net_fcntl(fd, F_SETFL, flags | 4);
	if (flags < 0) {
		net_close(fd);
		return flags;
	}
	return fd;
}

static void gateway_addr (struct sockaddr_in *sa, u16 port) {
	memset(sa, 0, sizeof(*sa));
	sa->sin_family = AF_INET;
	sa->sin_len = sizeof(*sa);
	sa->sin_port = htons(port);
	sa->sin_addr.s_addr = netprobe_gateway();
}

static void probe_end (bool answered, s32 res) {
	net_close(probe_fd);
	probe_fd = -1;
	netprobe_probe_result(gettime(), answered, res);
	if (answered)
		netinit_gateway_ok();
}

// One connect step: answered on a connection or a reset, missed on any other
// error or after HBC_PROBE_TIMEOUT_MS.
static void probe_step (void) {
	struct sockaddr_in sa;
	u64 now = gettime();
	s32 res;

	if (ticks_to_millisecs(now - probe_polled) < PROBE_POLL_MS)
		return;
	probe_polled = now;
	gateway_addr(&sa, netprobe_port());
	res = net_connect(probe_fd, (struct sockaddr *) &sa, sizeof(sa));
	if (res == 0 || res == -EISCONN || res == -ECONNREFUSED || res == -ECONNRESET)
		probe_end(true, res);
	else if (res != -EINPROGRESS && res != -EALREADY)
		probe_end(false, res);
	else if (ticks_to_millisecs(now - probe_started) >= HBC_PROBE_TIMEOUT_MS)
		probe_end(false, -ETIMEDOUT);
}

static void probe_start (void) {
	probe_fd = nonblocking_socket(SOCK_STREAM);
	if (probe_fd < 0) {
		s32 res = probe_fd;

		probe_fd = -1;
		netprobe_probe_result(gettime(), false, res);
		return;
	}
	probe_started = gettime();
	probe_polled = 0;
	probe_step();
}

// Sets up the keep-alive's socket for the current mode (netprobe.h): a fd,
// or a negative error. The discovery socket is borrowed, never closed here.
static s32 keepalive_open (void) {
	struct sockaddr_in sa;
	u32 reuse = 1;
	s32 fd, res;

	if (netprobe_keepalive_how() == KEEPALIVE_DISCOVER_SENDTO)
		return discover_fd >= 0 ? discover_fd : -EBADF;
	fd = nonblocking_socket(SOCK_DGRAM);
	if (fd < 0)
		return fd;
	if (netprobe_keepalive_how() == KEEPALIVE_BOUND_SENDTO) {
		// As discover_open: IOS sends from a socket bound to INADDR_ANY.
		net_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_len = sizeof(sa);
		sa.sin_port = htons(HBC_KEEPALIVE_LOCAL_PORT);
		sa.sin_addr.s_addr = 0;
		res = net_bind(fd, (struct sockaddr *) &sa, sizeof(sa));
	} else {
		gateway_addr(&sa, HBC_KEEPALIVE_PORT);
		res = net_connect(fd, (struct sockaddr *) &sa, sizeof(sa));
	}
	if (res < 0) {
		net_close(fd);
		return res;
	}
	return fd;
}

static void keepalive_drop (void) {
	if (keepalive_fd >= 0 && keepalive_fd != discover_fd)
		net_close(keepalive_fd);
	keepalive_fd = -1;
}

static void keepalive_send (void) {
	static u8 byte;
	struct sockaddr_in sa;
	s32 res;

	if (keepalive_fd < 0 || (netprobe_keepalive_how() == KEEPALIVE_DISCOVER_SENDTO &&
			keepalive_fd != discover_fd)) {
		keepalive_fd = -1;
		res = keepalive_open();
		if (res < 0) {
			if (netprobe_keepalive_result(gettime(), res))
				keepalive_drop();
			return;
		}
		keepalive_fd = res;
	}
	if (netprobe_keepalive_how() == KEEPALIVE_CONNECTED_SEND) {
		res = net_send(keepalive_fd, &byte, 1, 0);
	} else {
		gateway_addr(&sa, HBC_KEEPALIVE_PORT);
		res = net_sendto(keepalive_fd, &byte, 1, 0, (struct sockaddr *) &sa, sizeof(sa));
	}
	if (netprobe_keepalive_result(gettime(), res))
		keepalive_drop();
}

static void netprobe_close (void) {
	if (probe_fd >= 0) {
		net_close(probe_fd);
		probe_fd = -1;
	}
	keepalive_drop();
	netprobe_down();
}

// The gateway stayed silent after the automatic restart: the fresh-IOS
// relaunch, at most HBC_NET_FAST_RELAUNCHES in a row (netinit.h).
static void netprobe_relaunch (void) {
	if (relaunch_allowed() && netinit_gateway_relaunch(netprobe_gateway())) {
		netdiag_event("Probe: fresh-IOS relaunch requested");
		hlog("Network: gateway silent after a restart, restarting HBC\n");
		net_relaunch_gateway = true;
		net_relaunch_wanted = true;
	} else {
		netdiag_event("Probe: no relaunch (%s)", relaunch_allowed() ?
			"limit reached" : "not the installed channel, or net-hang wait");
		netprobe_relaunch_declined();
	}
}

static unsigned since_up_ms (void) {
	return (unsigned) ticks_to_millisecs(gettime() - net_up_at);
}

enum { SETTLE_OK, SETTLE_STOPPED, SETTLE_JOINING };

// The states seen while the Wi-Fi joins. Anything else (up, wired, an error,
// or an IOS or Dolphin reporting something odd) counts as up, so a setup
// whose link status means nothing is never held back.
static bool link_joining (s32 link) {
	return link == NCD_LINK_WIFI_DOWN || link == NCD_LINK_BUSY;
}

// One sample for the link trace: logs a change, and the summary once
// LINK_TRACE_MS have passed.
static void link_trace (s32 link) {
	unsigned t = since_up_ms();

	if (trace_done)
		return;
	if (link != trace_link) {
		netdiag_event("Link: %d (%s) at +%u ms", (int) link, netwatch_link_name(link), t);
		trace_link = link;
		trace_changes++;
	}
	if (t >= LINK_TRACE_MS) {
		netdiag_event("Link: %d (%s) at +%u ms, %u changes since the start", (int) link,
			netwatch_link_name(link), t, (unsigned) trace_changes);
		trace_done = true;
	}
}

// Before listening: the link up NET_SETTLE_MS in a row, at most NET_JOIN_MS
// after the start. SETTLE_JOINING when it never held, SETTLE_STOPPED when
// stopped or restarted meanwhile.
static int net_settle (void) {
	s32 ncd, link = ncd_link_status(&ncd);
	unsigned now = since_up_ms(), up_since = now;
	bool held = !link_joining(link);

	netdiag_event("Settle: link %d (%s), listening once up %u ms in a row (at most %u ms)",
		(int) link, netwatch_link_name(link), (unsigned) NET_SETTLE_MS, (unsigned) NET_JOIN_MS);
	trace_link = link;
	for (;;) {
		if (held && now - up_since >= NET_SETTLE_MS) {
			if (trace_changes)
				netdiag_event("Settle: up since +%u ms, listening at +%u ms", up_since, now);
			return SETTLE_OK;
		}
		if (now >= NET_JOIN_MS) {
			netdiag_event("Settle: link %d (%s), not up %u ms in a row by +%u ms: "
				"the Wi-Fi did not finish joining", (int) link, netwatch_link_name(link),
				(unsigned) NET_SETTLE_MS, now);
			return SETTLE_JOINING;
		}
		if (tcp_stop || tcp_restart)
			return SETTLE_STOPPED;
		usleep(50 * 1000);
		link = ncd_link_status(&ncd);
		now = since_up_ms();
		link_trace(link);
		if (link_joining(link)) {
			held = false;
		} else if (!held) {
			held = true;
			up_since = now;
		}
	}
}

// The stack reset under the listener (see NET_SETTLE_MS): HBC's sockets
// closed, the link waited for, and a fresh listener checked with one accept.
// discovery: open HBCU discovery again (the start opens it itself after its
// listener). 0 with *s listening, -ECANCELED when stopped or restarted, else
// the error.
static s32 net_relisten (int *s, u32 *mask, const char *why, bool discovery) {
	struct sockaddr_in sa;
	u32 len = sizeof(sa);
	s32 ncd, link, res;
	u64 start = gettime();
	char how[48];

	netdiag_event("Relisten: %s at +%u ms: closing the sockets, waiting for the link",
		why, since_up_ms());
	if (*s >= 0) {
		net_close(*s);
		*s = -1;
	}
	discover_stop();
	netprobe_close();
	for (;;) {
		if (tcp_stop || tcp_restart)
			return -ECANCELED;
		link = ncd_link_status(&ncd);
		if (!link_joining(link) || ticks_to_millisecs(gettime() - start) >= RELISTEN_LINK_MS)
			break;
		usleep(100 * 1000);
	}
	net_ios_report("Relisten");
	*mask = net_gethostip() & 0xffff0000;
	res = tcp_listen(LD_TCP_PORT, 3);
	netdiag_event("Relisten: tcp_listen %d", (int) res);
	if (res < 0)
		return res;
	*s = res;
	memset(&sa, 0, sizeof(sa));
	res = tcp_accept(*s, (struct sockaddr *) &sa, &len);
	if (res >= 0) {
		net_close(res);   // a client already: it tries again
	} else if (res != -EAGAIN) {
		netdiag_event("Relisten: the fresh listener failed too (%d)", (int) res);
		net_close(*s);
		*s = -1;
		return res;
	}
	if (discovery) {
		discover_fd = discover_open();
		if (discover_fd < 0)
			discover_fd = -1;
	}
	netprobe_up(gettime(), net_ip, find_gateway(net_ip, how, sizeof(how)), how);
	netdiag_event("Relisten: listening again after %llu ms, link %d (%s)",
		(unsigned long long) ticks_to_millisecs(gettime() - start), (int) link,
		netwatch_link_name(link));
	return 0;
}

// A reset that a fresh listener did not survive: the fresh-IOS relaunch if
// HBC may (true), else false for the caller's Restart network.
static bool net_reset_relaunch (s32 res) {
	if (!relaunch_allowed() || !netinit_reset_relaunch()) {
		netdiag_event("Network not settled (%d): no relaunch (%s), restarting the network",
			(int) res, relaunch_allowed() ? "limit reached" :
			"not the installed channel, or net-hang wait");
		return false;
	}
	hlog("Network: not settled after starting, restarting HBC\n");
	netdiag_event("Network not settled (%d): fresh-IOS relaunch requested", (int) res);
	// Before net_relaunch_wanted: main() takes that first, then this.
	net_relaunch_now = true;
	net_relaunch_wanted = true;
	return true;
}

static void * ld_tcp_func (void *arg) {
	ld_tcp_arg *ta = (ld_tcp_arg *) arg;
	int s = -1, sn;
	struct sockaddr_in sa;
	u32 mask, len_sa;
	s32 res;
	u8 buf[16];
	u16 wiiload_version;
	u8 retries;
	u64 init_start, attempt_start;
	u32 budget_ms;
	bool fast;

	mask = 0;

	ta->running = true;
	bootlog_thread("net");
	tcp_measure_this_thread();
	LWP_MutexLock (ta->cmutex);

	ta->cmd = LDTCPCMD_INIT;
	ta->state = LDTCPS_UNINITIALIZED;
	ta->handshaked = false;

	while (true) {
		while (ta->cmd == LDTCPCMD_IDLE && !tcp_stop && !tcp_restart)
			LWP_CondWait(ta->cond, ta->cmutex);

		ta->handshaked = false;

		if (tcp_stop || ta->cmd == LDTCPCMD_EXIT) {
			break;
		}

		if (tcp_restart) {
			// HOME > HBC > Restart network, also during a busy start: IOS
			// drops the connection and joins the network again, as at boot.
			u32 level;
			_CPU_ISR_Disable(level);
			tcp_restart = false;
			_CPU_ISR_Restore(level);
			gprintf ("reinit net\n");
			hlog("Network: restarting\n");
			netdiag_event("Restart network applied by the worker");
			tcp_net_deinit("Restart", &s);
			ta->state = LDTCPS_UNINITIALIZED;
			ta->cmd = LDTCPCMD_INIT;
		}

		if (ta->cmd == LDTCPCMD_INIT) {
			ta->cmd = LDTCPCMD_IDLE;

			// The installed channel's first relaunches in a row go fast
			// (netinit.h); otherwise the backoff retries. Not while the idle
			// check's recovery runs: that one is logged, not relaunched.
			fast = netinit_fast() && relaunch_allowed() && !watch_recovery;
			if (watch_recovery) {
				char when[48];

				snprintf(when, sizeof(when), "Watch recovery INIT %u", (unsigned) ++watch_inits);
				net_ios_report(when);
			}
			budget_ms = fast ? HBC_NET_INIT_FAST_MS : HBC_NET_INIT_TIMEOUT_MS;
			netdiag_event("INIT budget %u ms%s", (unsigned) budget_ms,
				fast ? ", fast relaunch on failure" : "");
			init_start = gettime();
			retries = 32;
			ta->state = LDTCPS_INITIALIZING;

			while (retries) {
				attempt_start = gettime();
				res = BOOTLOG("net_init_async", net_init_async(NULL, NULL));
				netdiag_event("Attempt %u/32: net_init_async %d", 33u - retries, (int) res);

				if (res) {
					gprintf("net_init_async failed: %d\n", res);
					break;
				}

				// One budget for all the attempts; restart and stop are
				// seen every 50 ms, so a start that IOS never finishes no
				// longer holds the worker, or an app launch, forever.
				res = BOOTLOG("net_init (wait)", netinit_wait(init_start, budget_ms,
						33u - retries, &tcp_stop, &tcp_restart));
				netdiag_event("Attempt %u ended: %d after %llu ms", 33u - retries, (int) res,
					(unsigned long long) ticks_to_millisecs(gettime() - attempt_start));
				if (watch_recovery && res < 0)
					net_ios_report("Watch recovery attempt failed");

				if (res == -ECANCELED) {
					start_cancelled = true;
					break;
				}
				if (ticks_to_millisecs(gettime() - init_start) >= budget_ms) {
					// netinit_wait's own -ETIMEDOUT: IOS never answered, so
					// the cleanup waits out that step like a cancelled one.
					if (res == -ETIMEDOUT)
						start_cancelled = true;
					break;
				}

				// Fast: libogc's own timeout (no address) goes straight to
				// the relaunch; a retry in the same IOS rarely helped.
				if (netinit_retry(res, fast)) {
					gprintf ("net_init failed: %d, trying again...\n", res);
					retries--;
					usleep(50 * 1000);
					continue;
				}

				break;
			}

#ifdef HBC_TEST_INIT_FAILURE
			// Test builds only: the first start that succeeds is failed instead.
			static bool injected_init_failure;
			if (!injected_init_failure && res >= 0) {
				injected_init_failure = true;
				res = -ETIMEDOUT;
				netdiag_event("TEST: injected INIT timeout");
			}
#endif
			if (res < 0) {
				gprintf ("net_init failed: %d\n", res);
				netdiag_event("INIT gave up: %d", (int) res);
				// Clean up the half-started stack before the menu's retry or
				// the relaunch; a cancelled start is cleaned up by the
				// restart or exit above. A cleanup that never returns goes to
				// the menu's watchdog (netrecovery.c) instead.
				if (res != -ECANCELED) {
					netrecovery_request();
					tcp_net_deinit("Failed INIT", NULL);
				}
				ta->state = LDTCPS_UNINITIALIZED;
				if (res != -ECANCELED && fast && !tcp_stop && !tcp_restart) {
					hlog("Network: no connection (%d), restarting HBC\n", (int) res);
					netdiag_event("Failed start: fresh-IOS relaunch requested");
					net_relaunch_wanted = true;
				} else if (res != -ECANCELED) {
					hlog("Network: no connection (%d), trying again\n", (int) res);
					if (watch_recovery)
						netdiag_event("Watch recovery INIT %u gave up: %d, backoff retry, no relaunch",
							(unsigned) watch_inits, (int) res);
				}
				// An automatic restart for a silent gateway that has not
				// come back in time goes to the relaunch (netprobe.h).
				if (!net_relaunch_wanted && !tcp_stop && !tcp_restart &&
						netprobe_poll(gettime(), false, false) == NETPROBE_RELAUNCH)
					netprobe_relaunch();
				continue;
			}

			gprintf ("net_init success: %d\n", res);
			{
				struct in_addr ip = { net_gethostip () };

				net_ip = ip.s_addr;
				hlog("Network: up at %s\n", inet_ntoa(ip));
				netdiag_event("Network up at %s", inet_ntoa(ip));
			}
			net_starts++;
			net_up_at = gettime();
			relisten_used = false;
			trace_changes = 0;
			trace_next_ms = 0;
			trace_done = false;
			netinit_up();
			{
				s32 ncd, link = ncd_link_status(&ncd);

				if (ncd)
					netdiag_event("NCD link status result %d", (int) ncd);
				netwatch_up(gettime(), net_ip, link);
			}
			{
				static bool known_checked;
				char how[48];
				u32 gw;

				if (!known_checked) {
					known_checked = true;
					if (netinit_known_gateway())
						netprobe_trust(netinit_known_gateway());
				}
				gw = find_gateway(net_ip, how, sizeof(how));
				netprobe_up(gettime(), net_ip, gw, how);
			}
			if (watch_recovery) {
				netdiag_event("Watch recovery: up again after %u INITs, %llu ms",
					(unsigned) watch_inits,
					(unsigned long long) ticks_to_millisecs(gettime() - watch_lost_at));
				watch_recovery = false;
			}

			res = net_settle ();
			if (res == SETTLE_STOPPED) {
				ta->state = LDTCPS_UNINITIALIZED;
				continue;
			}
			// Never held: a fresh IOS, not a listener IOS resets.
			if (res == SETTLE_JOINING && net_reset_relaunch(-ENETDOWN)) {
				netrecovery_request();
				tcp_net_deinit("Joining", NULL);
				ta->state = LDTCPS_UNINITIALIZED;
				continue;
			}

			mask = net_gethostip () & 0xffff0000;

			s = BOOTLOG("tcp_listen", tcp_listen (LD_TCP_PORT, 3));
			netdiag_event("tcp_listen %d", s);

			if (s == -ENETRESET && !relisten_used) {
				relisten_used = true;
				s = -1;
				res = net_relisten(&s, &mask, "tcp_listen -ENETRESET", false);
				if (res == -ECANCELED) {
					ta->state = LDTCPS_UNINITIALIZED;
					continue;
				}
				if (res < 0 && net_reset_relaunch(res)) {
					netrecovery_request();
					tcp_net_deinit("Reset", &s);
					ta->state = LDTCPS_UNINITIALIZED;
					continue;
				}
				if (res < 0)
					s = -ENETRESET;   // the Restart network below
			}

			if ((s == -ENETRESET) && retries) {
				gprintf("ENETRESET, reiniting\n");
				netrecovery_request();
				tcp_net_deinit("Network reset", NULL);
				s = -1;
				ta->state = LDTCPS_UNINITIALIZED;
				ta->cmd = LDTCPCMD_INIT;
				continue;
			}

			if (s < 0) {
				gprintf ("tcp_listen failed: %d\n",s);
				netrecovery_request();
				tcp_net_deinit("Listener failure", NULL);
				ta->state = LDTCPS_UNINITIALIZED;
				continue;
			}

			discover_fd = discover_open();
			if (discover_fd < 0) {
				gprintf("discover_open failed: %d\n", discover_fd);
				discover_fd = -1;
			}

			ta->state = LDTCPS_INITIALIZED;
			netrecovery_ready_unless(&tcp_restart);

			continue;
		}

		if (ta->cmd == LDTCPCMD_ACCEPT) {
			const char *lost = NULL;
			const char *name;
			u32 done, total;
			bool busy;

			ta->cmd = LDTCPCMD_IDLE;
			busy = devfile_progress(&name, &done, &total) != 0;
			// The link trace for the first LINK_TRACE_MS after a start.
			if (!trace_done && since_up_ms() >= trace_next_ms) {
				s32 ncd;

				trace_next_ms = since_up_ms() + LINK_TRACE_POLL_MS;
				link_trace(ncd_link_status(&ncd));
			}
			// While idle, ask IOS whether the network is still there
			// (netwatch.h): a few calls every few seconds.
			if (netwatch_due(gettime())) {
				s32 ncd, link = ncd_link_status(&ncd);
				u32 ip = net_gethostip();

				if (netwatch_sample(gettime(), link, ip, busy) == NETWATCH_LOST) {
					lost = "Watch: network lost";
					hlog("Network: lost while idle, restarting it\n");
				}
			}
			// The keep-alive and the gateway probe (netprobe.h).
			if (!lost) {
				if (probe_fd >= 0)
					probe_step();
				switch (netprobe_poll(gettime(), true, busy)) {
				case NETPROBE_PROBE:
					probe_start();
					break;
				case NETPROBE_KEEPALIVE:
					keepalive_send();
					break;
				case NETPROBE_RESTART:
					lost = "Probe: gateway silent";
					hlog("Network: gateway silent while idle, restarting the network\n");
					break;
				case NETPROBE_RELAUNCH:
					netprobe_relaunch();
					break;
				default:
					break;
				}
			}
			if (lost) {
				// Restart network, as HOME > HBC > Restart network. A failed
				// start then retries with the backoff in this IOS; after a
				// probe's restart, a gateway still silent HBC_PROBE_RESTART_MS
				// later goes to the fresh-IOS relaunch (netprobe.h). A
				// cleanup that hangs goes to the menu's watchdog.
				net_ios_report(lost);
				watch_recovery = true;
				watch_inits = 0;
				watch_lost_at = gettime();
				netrecovery_request();
				tcp_net_deinit(strncmp(lost, "Watch", 5) ? "Probe restart" : "Watch restart", &s);
				ta->state = LDTCPS_UNINITIALIZED;
				ta->cmd = LDTCPCMD_INIT;
				continue;
			}
			if (discover_fd >= 0 &&
					discover_poll(discover_fd, mask, "Homebrew Channel", false) < 0)
				discover_stop();
			memset (&sa, 0, sizeof (struct sockaddr_in));
			sa.sin_family = AF_INET;
			sa.sin_len = sizeof (struct sockaddr_in);

			len_sa = sizeof (struct sockaddr_in);

			sn = tcp_accept (s, (struct sockaddr *) &sa, &len_sa);
			if (sn == -EAGAIN)
				continue;
			if (sn < 0)
				netdiag_event("net_accept %d", sn);

			// The stack reset under the listener (NET_SETTLE_MS): once per
			// start, a fresh listener; if that fails, the fresh-IOS
			// relaunch; else, or a second reset, Restart network.
			if (sn == -ENETRESET && !relisten_used) {
				s32 res;

				relisten_used = true;
				res = net_relisten(&s, &mask, "net_accept -ENETRESET", true);
				if (res == 0 || res == -ECANCELED)
					continue;
				if (net_reset_relaunch(res)) {
					netrecovery_request();
					tcp_net_deinit("Reset", &s);
					ta->state = LDTCPS_UNINITIALIZED;
					continue;
				}
			}

			if (sn == -ENETRESET) {
				gprintf("ENETRESET, reiniting\n");
				netrecovery_request();
				tcp_net_deinit("Network reset", &s);
				ta->state = LDTCPS_UNINITIALIZED;
				ta->cmd = LDTCPCMD_INIT;
				continue;
			}

			if (sn < 0) {
				gprintf ("net_accept failed: %d\n", sn);

				netrecovery_request();
				tcp_net_deinit("Accept failure", &s);
				ta->state = LDTCPS_UNINITIALIZED;

				continue;
			}

			if ((sa.sin_addr.s_addr & 0xffff0000) != mask) {
				gprintf ("non local ip (%x)\n", sa.sin_addr.s_addr);
				net_close (sn);
				continue;
			}

			if (!tcp_read_timeout (sn, buf, 16, NULL, NULL, LD_HEADER_TIMEOUT)) {
				net_close (sn);
				continue;
			}

			if (!memcmp(buf, "HBCV", 4)) {
				tcp_write(sn, (const u8 *)CHANNEL_BUILD_STR,
						sizeof(CHANNEL_BUILD_STR), NULL, NULL);
				tcp_close(sn);
				continue;
			}

			if (devnet_handle(sn, buf, sa.sin_addr.s_addr)) {
				tcp_close(sn);
				continue;
			}

			wiiload_version = buf_u16(buf, 4);
			ta->args_len = buf_u16(buf, 6);
			ta->data_len = buf_u32(buf, 8);
			ta->data_len_un = buf_u32(buf, 12);

			if (strncmp((char *) buf, "HAXX", 4) ||
					(wiiload_version < WIILOAD_MIN_VERSION) ||
					(ta->args_len > ARGS_MAX_LEN) ||
					(!ta->data_len || ta->data_len > LD_MAX_SIZE) ||
					(ta->data_len_un > LD_MAX_SIZE)) {
				gprintf ("invalid upload request\n");
				hlog("Wiiload from %s: refused (bad header)\n", inet_ntoa(sa.sin_addr));
				net_close (sn);
				continue;
			}

			ta->s = sn;
			ta->client = inet_ntoa (sa.sin_addr);
			hlog("Wiiload from %s: %u bytes\n", ta->client, (unsigned) ta->data_len);
			ta->handshaked = true;

			continue;
		}
	}

	gprintf ("tcp thread deiniting\n");
	tcp_net_deinit("Shutdown", ta->state == LDTCPS_INITIALIZED ? &s : NULL);
	gprintf ("tcp thread exiting\n");
	ta->state = LDTCPS_UNINITIALIZED;

	ta->running = false;
	LWP_MutexUnlock (ta->cmutex);

	return NULL;
}

// thread setup

static ld_gecko_arg ta_gecko;
static ld_tcp_arg ta_tcp;

void loader_init (void) {
	int res;

	netdiag_event("HBC %s boot: IOS%u rev %u, title %016llx", CHANNEL_BUILD_STR,
		(unsigned) IOS_GetVersion(), (unsigned) IOS_GetRevision(),
		(unsigned long long) title_get_id());
	if (netinit_relaunches())
		netdiag_event("Network relaunch %u in a row led to this boot",
			(unsigned) netinit_relaunches());
	devnet_init ();

	if (usb_isgeckoalive (USBGECKO_CHANNEL)) {
		gprintf ("starting gecko thread\n");

		memset (&ta_gecko, 0, sizeof (ld_gecko_arg));

		memset (&ld_gecko_stack, 0, LD_THREAD_STACKSIZE);

		res = LWP_MutexInit (&ta_gecko.cmutex, false);
		if (res) {
			gprintf ("error creating cmutex: %d\n", res);
			return;
		}

		res = LWP_CondInit (&ta_gecko.cond);
		if (res) {
			gprintf ("error creating cond: %d\n", res);
			return;
		}

		res = LWP_CreateThread (&ld_gecko_thread, ld_gecko_func, &ta_gecko,
								ld_gecko_stack, LD_THREAD_STACKSIZE,
								LD_THREAD_PRIO);

		if (res) {
			gprintf ("error creating thread: %d\n", res);
		}
	}

	gprintf ("starting tcp thread\n");

	netrecovery_reset();
	tcp_restart = tcp_stop = false;
	start_cancelled = false;
	memset (&ta_tcp, 0, sizeof (ld_tcp_arg));

	memset (&ld_tcp_stack, 0, LD_THREAD_STACKSIZE);
	res = LWP_MutexInit (&ta_tcp.cmutex, false);
	if (res) {
		gprintf ("error creating cmutex: %d\n", res);
		return;
	}
	res = LWP_CondInit (&ta_tcp.cond);
	if (res) {
		gprintf ("error creating cond: %d\n", res);
		return;
	}
	res = LWP_CreateThread (&ld_tcp_thread, ld_tcp_func, &ta_tcp, ld_tcp_stack,
							LD_THREAD_STACKSIZE, LD_THREAD_PRIO);
	if (res) {
		gprintf ("error creating thread: %d\n", res);
	}
}

bool loader_deinit (void) {
	u8 i;

	// Published before trying the mutex: a busy start holds it while polling.
	tcp_stop = true;

	// A developer file transfer must stop before the card is unmounted.
	devfile_abort ();

	// the tcp thread does stuff on exit; the ideal order is stopping it first
	if (ta_tcp.running) {
		gprintf ("stopping tcp thread\n");

		for (i = 0; i < 25; ++i) {
			if (LWP_MutexTryLock (ta_tcp.cmutex) == 0)
				break;
			usleep (20 * 1000);
		}
		if (i >= 25) {
			gprintf("tcp thread didn't shutdown gracefully!\n");
		} else {
			gprintf ("sending tcp entry thread the exit cmd\n");
			ta_tcp.cmd = LDTCPCMD_EXIT;
			LWP_SetThreadPriority (ld_tcp_thread, LWP_PRIO_HIGHEST);
			LWP_CondBroadcast (ta_tcp.cond);
			LWP_MutexUnlock (ta_tcp.cmutex);

			for (i = 0; i < 25; ++i) {
				if (LWP_MutexTryLock (ta_tcp.cmutex) == 0) {
					if (!ta_tcp.running)
						break;
					LWP_MutexUnlock (ta_tcp.cmutex);
				}
				usleep (20 * 1000);
			}
			if (i >= 25) {
				gprintf("tcp thread didn't shutdown gracefully!\n");
			} else {
				LWP_MutexUnlock (ta_tcp.cmutex);
				LWP_JoinThread(ld_tcp_thread, NULL);
				LWP_CondDestroy (ta_tcp.cond);
				LWP_MutexDestroy (ta_tcp.cmutex);
			}
		}
	}

	if (ta_gecko.running) {
		gprintf ("stopping gecko thread\n");

		for (i = 0; i < 25; ++i) {
			if (LWP_MutexTryLock (ta_gecko.cmutex) == 0)
				break;
			usleep (20 * 1000);
		}
		if (i < 25) {
			gprintf ("sending gecko entry thread the exit cmd\n");
			ta_gecko.cmd = LDGECKOCMD_EXIT;
			LWP_SetThreadPriority (ld_gecko_thread, LWP_PRIO_HIGHEST);
			LWP_CondBroadcast (ta_gecko.cond);
			LWP_MutexUnlock (ta_gecko.cmutex);
			LWP_JoinThread(ld_gecko_thread, NULL);
			LWP_CondDestroy (ta_gecko.cond);
			LWP_MutexDestroy (ta_gecko.cmutex);
		} else {
			gprintf("gecko thread didn't shutdown gracefully!\n");
		}
	}

	return !ta_tcp.running;
}

void loader_tcp_park (void) {
	if (ta_tcp.running) {
		s32 res = LWP_SuspendThread(ld_tcp_thread);

		netdiag_event("TCP worker parked: %d", (int) res);
	}
}

void loader_tcp_init (void) {
	if (LWP_MutexTryLock(ta_tcp.cmutex) != 0)
		return;

	if (ta_tcp.running && ta_tcp.state == LDTCPS_UNINITIALIZED) {
		ta_tcp.cmd = LDTCPCMD_INIT;
		LWP_CondBroadcast(ta_tcp.cond);
	}
	LWP_MutexUnlock(ta_tcp.cmutex);
}

// When the network last failed to start (0: not waiting) and how long
// loader_signal_threads() waits before it tries again; reset once it is up.
static u64 net_failed_at;
static u32 net_retry_ms;

// HOME > HBC > Restart network. The worker sees the flag even in a busy
// start; an idle one is woken by loader_signal_threads().
void loader_tcp_restart (void) {
	netdiag_event("Restart network requested; state %d", ta_tcp.state);
	// Asked for by hand: the usual start, fresh-IOS relaunch included.
	watch_recovery = false;
	// The flag first: a worker coming up in between then keeps the request
	// (netrecovery_ready_unless), or clears it before it is made.
	tcp_restart = true;
	netrecovery_request();
}

void loader_signal_threads (void) {
	// Both the app list and the HOME overlay's frame loop come here.
	netdiag_flush(false);
	if (LWP_MutexTryLock(ta_gecko.cmutex) == 0) {
		if (loader_gecko_initialized () && !ta_gecko.handshaked) {
			ta_gecko.cmd = LDGECKOCMD_POLL;
			LWP_CondBroadcast(ta_gecko.cond);
		}
		LWP_MutexUnlock(ta_gecko.cmutex);
	}

	if (LWP_MutexTryLock(ta_tcp.cmutex) == 0) {
		if (tcp_restart && ta_tcp.running) {
			net_failed_at = 0;
			net_retry_ms = 0;
			LWP_CondBroadcast(ta_tcp.cond);
		} else if (loader_tcp_initialized () && !ta_tcp.handshaked) {
			net_failed_at = 0;
			net_retry_ms = 0;
			ta_tcp.cmd = LDTCPCMD_ACCEPT;
			LWP_CondBroadcast(ta_tcp.cond);
		} else if (ta_tcp.running && ta_tcp.state == LDTCPS_UNINITIALIZED &&
				   ta_tcp.cmd == LDTCPCMD_IDLE) {
			// A failed start (no DHCP answer yet, the access point busy) tries
			// again by itself, waiting 2 s, then 4, up to 32: an unattended Wii
			// would stay off the network until Z or the network icon is pressed.
			if (!net_failed_at) {
				net_failed_at = gettime ();
				net_retry_ms = net_retry_ms ? net_retry_ms * 2 : 2000;
				if (net_retry_ms > 32000)
					net_retry_ms = 32000;
			} else if (ticks_to_millisecs (diff_ticks (net_failed_at, gettime ())) >= net_retry_ms) {
				net_failed_at = 0;
				ta_tcp.cmd = LDTCPCMD_INIT;
				LWP_CondBroadcast(ta_tcp.cond);
			}
		}
		LWP_MutexUnlock(ta_tcp.cmutex);
	}
}

bool loader_gecko_initialized (void) {
	return ta_gecko.running;
}

bool loader_tcp_initializing (void) {
	return ta_tcp.running && ta_tcp.state == LDTCPS_INITIALIZING;
}

// Bytes of the loader TCP thread's stack ever used: the stack is zeroed at
// init and grows down, so untouched bytes remain zero at the low end.
u32 loader_tcp_stack_used (void) {
	u32 i;

	for (i = 0; i < LD_THREAD_STACKSIZE && !ld_tcp_stack[i]; ++i)
		;
	return LD_THREAD_STACKSIZE - i;
}

bool loader_tcp_initialized (void) {
	return ta_tcp.running && ta_tcp.state == LDTCPS_INITIALIZED;
}

bool loader_handshaked (void) {
	bool handshaked = false;
	if (LWP_MutexTryLock(ta_gecko.cmutex) == 0) {
		handshaked = handshaked || (ta_gecko.running && ta_gecko.handshaked);
		LWP_MutexUnlock(ta_gecko.cmutex);
	}
	if (LWP_MutexTryLock(ta_tcp.cmutex) == 0) {
		handshaked = handshaked || (ta_tcp.running && ta_tcp.handshaked);
		LWP_MutexUnlock(ta_tcp.cmutex);
	}

	return handshaked;
}

// loading thread

static lwp_t ld_load_thread;
static u8 ld_load_stack[LD_THREAD_STACKSIZE] ATTRIBUTE_ALIGN (32);

typedef enum {
	LDC_FILE = 0,
	LDC_GECKO,
	LDC_TCP
} ld_load_cmd;

typedef enum {
	LDS_RUNNING = 0,
	LDS_SUCCESS,
	LDS_ERR_READ,
	LDS_ERR_RECEIVE,
	LDS_ERR_UNCOMPRESS
} ld_load_state;

typedef struct {
	ld_load_cmd cmd;

	u32 data_len;
	u8 *data;

	u32 data_len_un;
	u8 *data_un;

	u16 args_len;
	u8 *args;

	int fd;

	mutex_t mutex;
	ld_load_state state;
	u32 progress;
} ld_load_arg;

// Receive a compressed Wiiload upload in LD_STREAM_CHUNK pieces and inflate
// each as it arrives, so decompression overlaps the transfer and the
// compressed data never needs a buffer of its own.
static ld_load_state ld_tcp_inflate_run (ld_load_arg *ta, z_stream *z, u32 left,
										 u32 received, int zres);

static ld_load_state ld_tcp_inflate (ld_load_arg *ta) {
	z_stream z = { 0 };  // zlib reads zalloc/zfree/opaque from here
	u32 left = ta->data_len, received = 0;
	int zres = Z_OK;
	bool in_mem1 = zmem_use (&z);
	ld_load_state state;

	if (inflateInit (&z) != Z_OK) {
		if (in_mem1)
			zmem_release ();
		return LDS_ERR_UNCOMPRESS;
	}

	state = ld_tcp_inflate_run (ta, &z, left, received, zres);
	inflateEnd (&z);
	if (in_mem1)
		zmem_release ();
	return state;
}

static ld_load_state ld_tcp_inflate_run (ld_load_arg *ta, z_stream *z, u32 left,
										 u32 received, int zres) {
	z->next_out = ta->data_un;
	z->avail_out = ta->data_len_un;

	while (left) {
		u32 block = left > LD_STREAM_CHUNK ? LD_STREAM_CHUNK : left;

		if (!tcp_read (ta->fd, ta->data, block, NULL, NULL))
			return LDS_ERR_RECEIVE;

		received += block;
		left -= block;

		LWP_MutexLock (ta->mutex);
		ta->progress = received;
		LWP_MutexUnlock (ta->mutex);

		if (zres == Z_STREAM_END)
			continue;

		z->next_in = ta->data;
		z->avail_in = block;
		zres = inflate (z, Z_NO_FLUSH);
		if (zres != Z_OK && zres != Z_STREAM_END) {
			gprintf ("error uncompressing: %d\n", zres);
			return LDS_ERR_UNCOMPRESS;
		}
	}

	if (ta->args_len && !tcp_read (ta->fd, ta->args, ta->args_len, NULL, NULL))
		return LDS_ERR_RECEIVE;

	if (zres != Z_STREAM_END || z->total_out != ta->data_len_un) {
		gprintf ("short uncompress: %u\n", (u32) z->total_out);
		return LDS_ERR_UNCOMPRESS;
	}

	return LDS_SUCCESS;
}

static void * ld_load_func (void *arg) {
	ld_load_arg *ta = (ld_load_arg *) arg;
	u8 *d;
	u32 left, received;
	s32 block;
	int res;
	s64 t;

	d = ta->data;
	received = 0;
	left = ta->data_len + ta->args_len;

	switch (ta->cmd) {
	case LDC_FILE:
		while (left) {
			block = left;
			if (block > 32 * 1024)
				block = 32 * 1024;

			block = read (ta->fd, d, block);

			if (block <= 0) {
				gprintf ("read failed: %d\n", block);
				close (ta->fd);

				LWP_MutexLock (ta->mutex);
				ta->state = LDS_ERR_READ;
				LWP_MutexUnlock (ta->mutex);

				return NULL;
			} else {
				d += block;
				received += block;
				left -= block;

				LWP_MutexLock (ta->mutex);
				ta->progress = received;
				LWP_MutexUnlock (ta->mutex);
			}
		}

		close (ta->fd);

		break;

	case LDC_GECKO:
		block = 0;
		t = gettime ();

		while (left) {
			res = usb_recvbuffer_ex(USBGECKO_CHANNEL, d, left, USBGECKO_RETRIES);

			if (res) {
				d += res;
				received += res;
				left -= res;
				block += res;

				if (block >= 1024) {
					block = 0;

					LWP_MutexLock (ta->mutex);
					ta->progress = received;
					LWP_MutexUnlock (ta->mutex);

					t = gettime ();
				}
			}

			if (ticks_to_millisecs (diff_ticks (t, gettime ())) > LD_TIMEOUT) {
				LWP_MutexLock (ta->mutex);
				ta->state = LDS_ERR_RECEIVE;
				LWP_MutexUnlock (ta->mutex);

				gprintf_enable(1);

				return NULL;
			}
		}

		gprintf_enable(1);

		break;

	case LDC_TCP:
		if (ta->data_un) {
			ld_load_state state = ld_tcp_inflate (ta);

			net_close (ta->fd);

			LWP_MutexLock (ta->mutex);
			ta->state = state;
			LWP_MutexUnlock (ta->mutex);

			return NULL;
		}

		if (!tcp_read (ta->fd, ta->data, left, &ta->mutex, &ta->progress)) {
			LWP_MutexLock (ta->mutex);
			ta->state = LDS_ERR_RECEIVE;
			LWP_MutexUnlock (ta->mutex);

			net_close (ta->fd);

			return NULL;
		}

		net_close (ta->fd);

		break;
	}

	if (ta->data_un) {
		int res;
		uLongf len = ta->data_len_un;

		res = uncompress(ta->data_un, &len, ta->data, ta->data_len);

		if (res != Z_OK) {
			gprintf("error uncompressing: %d\n", res);

			LWP_MutexLock (ta->mutex);
			ta->state = LDS_ERR_UNCOMPRESS;
			LWP_MutexUnlock (ta->mutex);

			return NULL;
		}

		if (len != ta->data_len_un) {
			gprintf("short uncompress: %u\n", (u32) len);

			LWP_MutexLock (ta->mutex);
			ta->state = LDS_ERR_UNCOMPRESS;
			LWP_MutexUnlock (ta->mutex);

			return NULL;
		}
	}

	LWP_MutexLock (ta->mutex);
	ta->state = LDS_SUCCESS;
	LWP_MutexUnlock (ta->mutex);

	return NULL;
}

// Close the transfer source after a failure before the load thread runs.
static void ld_close_source (ld_load_arg *ta) {
	if (ta->cmd == LDC_TCP)
		net_close (ta->fd);
	else if (ta->cmd == LDC_FILE)
		close (ta->fd);
}

// public loading function

// An error loading result: HBCS "upload" hears of a remote one, whose popup
// closes itself so that automated tests carry on.
static void ld_error(const loader_result *result, view *sub_view,
						const char *error, const char *text) {
	if (result->remote)
		devnet_upload_result("error", error, text);
	show_message_timed (sub_view, DLGMT_ERROR, DLGB_OK, text, 0,
						result->remote ? NET_POPUP_TIMEOUT_S : 0);
}

void loader_load(loader_result *result, view *sub_view, app_entry *entry) {
	char caption[PATH_MAX + 32];
	char filename[PATH_MAX];
	char shown[64];
	const char *note = NULL;

	ld_load_arg ta;
	s32 res;
	bool running;
	u32 progress;

	view *v;

	text_err_read = _("Error while reading the application from the SD card");
	text_err_receive = _("Error while receiving the application");
	text_err_uncompress = _("Error uncompressing the received data");
	text_err_invalid_bin = _("This is not a valid Wii application");
	text_err_invalid_zip = _("This is not a usable ZIP file");
	text_err_oom = _("Not enough memory");

	memset(result, 0, sizeof (loader_result));
	memset(&ta, 0, sizeof (ld_load_arg));
	memset(filename, 0, sizeof(filename));

	bool gecko_handshaked = false;
	bool tcp_handshaked = false;

	if (LWP_MutexTryLock (ta_gecko.cmutex) == 0) {
		gecko_handshaked = ta_gecko.handshaked;
		ta_gecko.cmd = LDGECKOCMD_IDLE;
		LWP_MutexUnlock (ta_gecko.cmutex);
	}

	if (LWP_MutexTryLock (ta_tcp.cmutex) == 0) {
		tcp_handshaked = ta_tcp.handshaked;
		ta_tcp.cmd = LDTCPCMD_IDLE;
		LWP_MutexUnlock (ta_tcp.cmutex);
	}

	if (entry) {
		char *name;

		if (!app_entry_get_filename(filename, entry))
			return;

		gprintf ("loading %s\n", filename);

		ta.cmd = LDC_FILE;

		ta.fd = open (filename, O_RDONLY);
		if (ta.fd < 0)
			return;

		ta.data_len = entry->size;

		if (entry->meta && entry->meta->name)
			name = entry->meta->name;
		else
			name = entry->dirname;

		snprintf (caption, sizeof (caption), _("Loading %s"), name);
	} else if (gecko_handshaked) {
		ta.cmd = LDC_GECKO;
		ta_gecko.handshaked = false;

		ta.data_len = ta_gecko.data_len;
		ta.data_len_un = ta_gecko.data_len_un;
		ta.args_len = ta_gecko.args_len;

		sprintf (caption, _("Receiving over USBGecko"));
	} else if (tcp_handshaked) {
		ta.cmd = LDC_TCP;
		ta_tcp.handshaked = false;

		ta.data_len = ta_tcp.data_len;
		ta.data_len_un = ta_tcp.data_len_un;
		ta.args_len = ta_tcp.args_len;
		ta.fd = ta_tcp.s;

		// What the sender called it (HBCA), with where it came from below;
		// a plain Wiiload sender names nothing until the data is in.
		if (devnet_upload_caption (shown, sizeof (shown))) {
			snprintf (caption, sizeof (caption), "%s %s", _("Receiving"), shown);
			note = ta_tcp.client;
		} else {
			sprintf (caption, _("Receiving from %s"), ta_tcp.client);
		}
	} else {
		return;
	}

	result->remote = ta.cmd != LDC_FILE;

	if (ta.data_len == ta.data_len_un)
		ta.data_len_un = 0;

	if (ta.data_len_un) {
		ta.data_un = (u8 *) blob_alloc(ta.data_len_un);

		if (!ta.data_un) {
			ld_close_source (&ta);
			ld_error (result, sub_view, "out_of_memory", text_err_oom);
			return;
		}
	}

	if (ta.cmd == LDC_TCP && ta.data_len_un) {
		ta.data = (u8 *) blob_alloc(LD_STREAM_CHUNK + ta.args_len);
		ta.args = ta.data + LD_STREAM_CHUNK;
	} else {
		ta.data = (u8 *) blob_alloc(ta.data_len + ta.args_len);
		ta.args = ta.data + ta.data_len;
	}

	if (!ta.data) {
		blob_free(ta.data_un);
		ld_close_source (&ta);
		ld_error (result, sub_view, "out_of_memory", text_err_oom);
		return;
	}


	res = LWP_MutexInit (&ta.mutex, false);

	if (res) {
		gprintf ("error creating mutex: %d\n", res);
		blob_free (ta.data);
		blob_free (ta.data_un);
		ld_close_source (&ta);
		panic(); // if this happens, let's find out
		return;
	}

	memset (&ld_load_stack, 0, LD_THREAD_STACKSIZE);
	res = LWP_CreateThread (&ld_load_thread, ld_load_func, &ta, ld_load_stack,
							LD_THREAD_STACKSIZE, DEVNET_THREAD_PRIO);

	if (res) {
		gprintf ("error creating thread: %d\n", res);
		blob_free (ta.data);
		blob_free (ta.data_un);
		ld_close_source (&ta);
		panic(); // if this happens, let's find out
		return;
	}

	v = dialog_progress (sub_view, caption, note, ta.data_len + ta.args_len);

	dialog_fade (v, true);

	running = true;
	while (running) {
		LWP_MutexLock (ta.mutex);
		running = ta.state == LDS_RUNNING;
		progress = ta.progress;
		LWP_MutexUnlock (ta.mutex);

		dialog_set_progress (v, progress);

		view_plot (v, DIALOG_MASK_COLOR, NULL, NULL, NULL);
		// Status requests still get answers during a long receive.
		loader_signal_threads ();
	}

	dialog_fade (v, false);

	view_free (v);

	LWP_MutexDestroy (ta.mutex);

	if (ta.args_len) {
		memcpy(result->args, ta.args, ta.args_len);
		result->args_len = ta.args_len;
	}

	if (ta.data_len_un) {
		blob_free(ta.data);
		ta.data = ta.data_un;
		ta.data_len = ta.data_len_un;
		ta.data_un = NULL;
		ta.data_len_un = 0;
	}

	const char *text = NULL, *error = NULL;

	switch (ta.state) {
	case LDS_RUNNING:
	case LDS_SUCCESS:
		break;
	case LDS_ERR_READ:
		text = text_err_read;
		error = "read";
		break;
	case LDS_ERR_RECEIVE:
		text = text_err_receive;
		error = "receive";
		break;
	case LDS_ERR_UNCOMPRESS:
		text = text_err_uncompress;
		error = "uncompress";
		break;
	}

	if (text) {
		blob_free(ta.data);
		ld_error (result, sub_view, error, text);
		return;
	}

	result->data = ta.data;
	result->data_len = ta.data_len;

	if (ta.cmd == LDC_FILE) {
		switch(entry->type) {
		case AET_BOOT_ELF:
		case AET_BOOT_DOL:
			break;

		case AET_THEME:
			result->type = LT_ZIP_THEME;
			return;
		}

		result->type = LT_EXECUTABLE;

		strcpy(result->args, filename);
		result->args_len = strlen (result->args);

		if (entry->meta && entry->meta->args &&
				result->args_len + 1 + entry->meta->argslen + 2 <=
				ARGS_MAX_LEN) {
			result->args_len++;
			memcpy(result->args + result->args_len,
					entry->meta->args, entry->meta->argslen);
			result->args_len += entry->meta->argslen;
		}

		result->args[result->args_len + 1] = 0;
		result->args_len += 2;

		return;
	}

	if (manage_is_zip(ta.data)) {
		gprintf("we got a .zip\n");

		if (manage_check_zip_app(ta.data, ta.data_len, result->dirname,
									&result->bytes))
			result->type = LT_ZIP_APP;
		else if (manage_check_zip_theme(ta.data, ta.data_len))
			result->type = LT_ZIP_THEME;

		if (result->type == LT_UNKNOWN) {
			blob_free(ta.data);
			ta.data = NULL;
			ta.data_len = 0;
			ld_error (result, sub_view, "bad_zip", text_err_invalid_zip);
		}

		return;
	}

	result->type = LT_EXECUTABLE;
}

// A USB drive stopped responding at start-up and is still in: the app would
// most likely reload IOS and hang on it. A remote launch is refused (no one
// may be at the Wii); a menu launch asks, and goes on when OK is pressed.
static bool usb_guard(const loader_result *result, view *sub_view) {
	if (!usbstart_replug_needed())
		return true;
	if (result->remote) {
		hlog("Launch refused: the USB drive that stopped responding is still in\n");
		// Under 96 bytes, so HBCS "upload" carries all of it.
		ld_error(result, sub_view, "usb_stuck",
				 _("A USB drive stopped responding at start-up. "
				   "Unplug it and plug it back in, then send again."));
		return false;
	}
	// Cancel has the focus, so A pressed without reading does not launch;
	// either button launches once the drive has come out.
	bool ok = show_message(sub_view, DLGMT_WARNING, DLGB_OKCANCEL,
						   _("A USB drive stopped responding when the Homebrew Channel "
							 "started. Unplug it and plug it back in, then press A. If it "
							 "stays in, the app can stop on a black screen until the drive "
							 "is unplugged."),
						   1) == 0;
	bool out = !usbstart_replug_needed();

	hlog("Launch %s: the stuck USB drive %s\n", ok || out ? "goes on" : "cancelled",
		 out ? "was unplugged" : "is still in");
	return ok || out;
}

bool loader_load_executable(entry_point *ep, loader_result *result,
							view *sub_view) {
	bool res;
	char caption[64], installed[PATH_MAX];
	bool installed_run = false;

	if (!usb_guard(result, sub_view)) {
		blob_free(result->data);
		return false;
	}

	// A network launch of an installed app's own file (hbc.py send/run of
	// apps/X/boot.dol): its run counts. Checked before the loader touches the data.
	if (result->remote && settings.new_badges && devnet_upload_caption(caption, sizeof(caption)))
		installed_run = app_run_match_upload("sd:", caption, result->data,
				result->data_len, installed, sizeof(installed));
	res = loader_reloc(ep, result->data, result->data_len,
					   result->args, result->args_len, true);

	if (!res)
		ld_error (result, sub_view, "not_wii_app", text_err_invalid_bin);
	else if (result->remote) {
		devnet_upload_result("launched", NULL, NULL);
		if (installed_run && !app_run_record(installed))
			gprintf("could not save app launch marker\n");
	}
	// args starts with the boot file's path. Note the run so its app badge
	// clears (Options > App badges).
	else if (settings.new_badges && !app_run_record(result->args))
		gprintf("could not save app launch marker\n");

	blob_free(result->data);

	return res;
}

bool loader_handle_zip_app(loader_result *result, view *sub_view) {
	char buf[1024];
	char buf2[1024];

	text_extract_zip = _("Extract the received ZIP file?\n%s of free space are required.");
	text_warn_overwrite = _("WARNING: Files in '%s' will be overwritten");
	text_err_extract_zip = _("Error while extracting the ZIP file");

	if ((result->bytes / 1024u) > 999)
		sprintf(buf2, "%.02f MB",
				(float)(result->bytes) / (float)(1024 * 1024));
	else
		sprintf(buf2, "%u KB", result->bytes / 1024u);
	sprintf(buf, text_extract_zip, buf2);

	if (!app_entry_get_path(buf2)) {
		blob_free(result->data);
		return false;
	}

	strcat(buf2, ":/");
	strcat(buf2, result->dirname);
	if (dir_exists(buf2)) {
		strcat(buf, "\n\n");
		sprintf(buf2, text_warn_overwrite, result->dirname);
		strcat(buf, buf2);
	}

	// hbc.py send --yes answers for the sender; else the question times out
	// as No, like B.
	if (!(result->remote && (devnet_upload_flags() & DEVNET_UPLOAD_YES)) &&
			show_message_timed(sub_view, DLGMT_CONFIRM, DLGB_YESNO, buf, 0,
								result->remote ? NET_POPUP_TIMEOUT_S : 0) != 0) {
		if (result->remote)
			devnet_upload_result("declined", NULL, NULL);
		blob_free(result->data);
		return false;
	}

	if (!manage_run(sub_view, result->dirname, result->data,
					result->data_len, result->bytes)) {
		ld_error (result, sub_view, "extract", text_err_extract_zip);
		blob_free(result->data);
		return false;
	}

	blob_free(result->data);
	if (result->remote)
		devnet_upload_result("installed", NULL, result->dirname);

	return true;
}
