#ifndef HBC_NETPROBE_H
#define HBC_NETPROBE_H

// Keeping the access point aware of the Wii, and noticing when the LAN can
// no longer reach it, while HBC sits idle with the network up.
//
// On the 1.14.7 Wii (static IP, one access point) HBC idled in its menu and
// after some minutes nothing on the LAN could reach it, while NCD still said
// wireless up and the address stayed (so netwatch.c saw nothing). Restart
// network brought it straight back. The access point drops or stops
// forwarding to a client that has been silent, and the Wii's 802.11b/g stack
// never notices. So, from the idle TCP worker:
//
// - Keep-alive: every HBC_KEEPALIVE_PERIOD_MS one 1-byte UDP datagram to the
//   default gateway's discard port (9). A unicast frame to the access point,
//   with an ARP exchange for the gateway whenever IOS's ARP entry expires.
//   AP inactivity timeouts are typically 300 s, some 60-180 s.
// - Reachability probe: every HBC_PROBE_PERIOD_MS a non-blocking TCP connect
//   to the gateway (port 80, else 53, else 443 until one answers). A SYN-ACK
//   or a reset (-ECONNREFUSED/-ECONNRESET) is an answer: the gateway got the
//   frame and its reply reached the Wii. No answer within
//   HBC_PROBE_TIMEOUT_MS, or any other error, is a miss. TCP is used because
//   IOS's stack has no ping or raw socket that libogc exposes, and a
//   non-blocking connect polled with net_connect is what tcp.c and
//   hbc_netlog already do on hardware.
// - HBC_PROBE_MISSES misses in a row restart the network (as HOME > HBC >
//   Restart network). If the gateway has not answered HBC_PROBE_RESTART_MS
//   after that restart, the worker asks for the fresh-IOS relaunch (at most
//   HBC_NET_FAST_RELAUNCHES in a row, netinit.h).
//
// Misses only count once this gateway has answered a probe (on any port):
// a gateway that never answers is probed but never acted on. Never during a
// transfer: a busy worker clears the misses, as does a gap in the worker's
// idle loop longer than HBC_NETPROBE_GAP_MS.
//
// The keep-alive failing HBC_KEEPALIVE_ERRORS times in a row after it once
// worked also restarts the network, but only while the probe can't vouch for
// the network (gateway not known to answer, or missing already).
//
// How the keep-alive is sent: in 1.14.8 IOS answered every net_sendto on a
// fresh unbound UDP socket with -22 (EINVAL), while discover.c's sendto on a
// socket bound to INADDR_ANY:4299 works. So it starts with a socket bound to
// INADDR_ANY:HBC_KEEPALIVE_LOCAL_PORT; while no send has worked yet, -EINVAL
// moves on to a UDP socket connected to the gateway with net_send, then to
// the discovery socket itself. The mode found carries over restarts. Keep-alive errors are logged at most
// once every HBC_NETPROBE_ERROR_LOG_MS; every missed probe is logged.

#include <stdbool.h>
#include <gctypes.h>

#define HBC_KEEPALIVE_PERIOD_MS 30000u
#define HBC_KEEPALIVE_PORT 9
#define HBC_KEEPALIVE_ERRORS 3u
#define HBC_KEEPALIVE_LOCAL_PORT 4298
#define HBC_PROBE_PERIOD_MS 15000u
#define HBC_PROBE_TIMEOUT_MS 3000u
#define HBC_PROBE_MISSES 3u
#define HBC_PROBE_RESTART_MS 60000u
#define HBC_NETPROBE_GAP_MS 10000u
#define HBC_NETPROBE_ERROR_LOG_MS 60000u
#define HBC_PROBE_PORTS 3

typedef enum {
	KEEPALIVE_BOUND_SENDTO = 0,   // UDP bound to INADDR_ANY:HBC_KEEPALIVE_LOCAL_PORT, net_sendto
	KEEPALIVE_CONNECTED_SEND,     // UDP net_connect to the gateway, net_send
	KEEPALIVE_DISCOVER_SENDTO,    // net_sendto on the HBCU discovery socket
	KEEPALIVE_MODES
} netprobe_keepalive_mode;

typedef enum {
	NETPROBE_NONE = 0,
	NETPROBE_KEEPALIVE,   // send the keep-alive, report with netprobe_keepalive_result
	NETPROBE_PROBE,       // start a probe to netprobe_port(), report with netprobe_probe_result
	NETPROBE_RESTART,     // Restart network now
	NETPROBE_RELAUNCH     // the restart did not bring the gateway back: fresh IOS
} netprobe_action;

// The network came up at ip; gateway as found (source: how, for the log).
// Trust and a pending restart's deadline carry over when the gateway is the
// same.
void netprobe_up(u64 now, u32 ip, u32 gateway, const char *source);
// Before the first netprobe_up: this gateway answered before a relaunch
// for it (netinit_known_gateway), so its misses count from the start.
void netprobe_trust(u32 gateway);
// The network went down: no sends until netprobe_up. A pending restart's
// deadline still runs.
void netprobe_down(void);
// What to do now: at most one action per call. up: the network is up and
// the worker idle in its accept loop; busy: a transfer runs. With up false
// only the restart's deadline is checked.
netprobe_action netprobe_poll(u64 now, bool up, bool busy);
// The keep-alive's send result (or the socket setup's error). True when the
// send mode changed: the caller drops its socket and sets up the new mode.
bool netprobe_keepalive_result(u64 now, s32 res);
netprobe_keepalive_mode netprobe_keepalive_how(void);
const char *netprobe_keepalive_name(netprobe_keepalive_mode mode);
// The probe's result: answered, or not, with the connect result for the log.
void netprobe_probe_result(u64 now, bool answered, s32 res);
// The relaunch was not possible (limit, or not the installed channel).
void netprobe_relaunch_declined(void);

u32 netprobe_gateway(void);
u16 netprobe_port(void);
bool netprobe_trusted(void);
const char *netprobe_source(void);
u32 netprobe_keepalive_sent(void);
u32 netprobe_keepalive_errors(void);
u32 netprobe_probes_ok(void);
u32 netprobe_probes_failed(void);
u32 netprobe_restarts(void);
u32 netprobe_relaunches(void);

#endif
