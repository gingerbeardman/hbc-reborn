#ifndef HBC_NETWATCH_H
#define HBC_NETWATCH_H

// Noticing that the network has gone while HBC sits idle with it up.
//
// When the Wi-Fi drops after a successful start, nothing in HBC fails: the
// listener stays open, net_accept() keeps saying -EAGAIN, and HBC shows the
// network as up while nothing on the LAN reaches the Wii. So the idle TCP
// worker asks IOS every HBC_NETWATCH_PERIOD_MS:
//
// - NCD's link status (/dev/net/ncd/manage ioctlv 7, NCDGetLinkStatus: the
//   call libogc's net_init makes before it starts the stack). Word 1 of its
//   answer is 1 busy, 2 none, 3 wired, 4 wireless down, 5 wireless up
//   (Dolphin's WiiNetConfig names them; Dolphin always answers 3).
// - The address IOS's IP stack has (net_gethostip, SO_GETHOSTID): 0 when it
//   has none, or not the one the network came up with.
//
// A sample is bad when the link reads none or wireless down, or the address
// is gone or changed. The link status counts only when it read wired or
// wireless up as the network came up: an IOS that reports anything else on a
// working network is not believed. One bad sample is logged; only a bad
// state that lasts HBC_NETWATCH_PERSIST_MS is a lost network, and the worker
// then restarts the network (as HOME > HBC > Restart network). Never during
// a transfer: a busy sample starts the wait again.

#include <stdbool.h>
#include <gctypes.h>

#define HBC_NETWATCH_PERIOD_MS 4000u
#define HBC_NETWATCH_PERSIST_MS 15000u

enum {
	NCD_LINK_BUSY = 1,
	NCD_LINK_NONE = 2,
	NCD_LINK_WIRED = 3,
	NCD_LINK_WIFI_DOWN = 4,
	NCD_LINK_WIFI_UP = 5
};

typedef enum {
	NETWATCH_OK = 0,
	NETWATCH_SUSPECT,   // bad, not for long enough yet
	NETWATCH_LOST       // bad for HBC_NETWATCH_PERSIST_MS: restart now
} netwatch_verdict;

// The network came up at ip, with link as NCD reported it then (or a
// negative error). Starts the checks.
void netwatch_up(u64 now, u32 ip, s32 link);
// The network went down for another reason: no checks until netwatch_up.
void netwatch_down(void);
// Whether a check is due (at most one every HBC_NETWATCH_PERIOD_MS).
bool netwatch_due(u64 now);
// One check: link as NCD reports it (or a negative error), ip from IOS,
// busy while a transfer runs. Logs to netdiag on every change.
netwatch_verdict netwatch_sample(u64 now, s32 link, u32 ip, bool busy);

// For HBCS: lost networks since HBC started, bad states that cleared by
// themselves, the last lost network's reason ("" when none), the link
// status of the last check (0 before any).
u32 netwatch_lost_count(void);
u32 netwatch_blips(void);
const char *netwatch_last_reason(void);
s32 netwatch_link(void);
// "wireless down" etc., or "error" for a negative status.
const char *netwatch_link_name(s32 link);

#endif
