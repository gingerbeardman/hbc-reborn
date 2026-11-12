#ifndef HBC_NETINIT_H
#define HBC_NETINIT_H

// Starting the network, and the fresh-IOS relaunch when a start fails.
//
// After an app goes back to HBC, the network now and then will not come up
// in the same IOS: libogc's own start times out (-ETIMEDOUT, no address
// after ~10 s) again and again, or stays busy. In the 2026-11-02 hardware
// log a retry in the same IOS helped 2 times in 22, while a fresh IOS (HBC
// relaunched through its own reload stub) brought the network back after
// 1-4 relaunches. The slowest of 82 successful starts took 13.3 s.
//
// So while fewer than HBC_NET_FAST_RELAUNCHES relaunches in a row have
// happened, the installed channel gives a start HBC_NET_INIT_FAST_MS, goes
// to the relaunch at libogc's timeout rather than retrying in the same IOS,
// and counts down HBC_NET_RELAUNCH_COUNTDOWN_S. After them, in a DOL started by a
// loader (which cannot relaunch the channel), and with `hbc.py net-hang
// wait`, a start gets HBC_NET_INIT_TIMEOUT_MS and the loader's backoff
// retries (2 s, 4 s, up to 32 s apart).
//
// A network cleanup that never returns (netrecovery.h) leaves libogc unable
// to start the network again in this HBC, so the same relaunch, counted in
// the same run, is the only way back: after HBC_NET_FAST_RELAUNCHES in a row
// HBC carries on without the network until it is started again, as it does
// in a DOL or with net-hang wait. net-hang reload relaunches every time.

#include <stdbool.h>
#include <gctypes.h>

#define HBC_NET_INIT_TIMEOUT_MS 30000u
// 8 s, not 20 s (1.16.6): on 192.168.1.224 a start that IOS never finishes
// followed 5 of 20 returns (1.16.5, 2026-11-12), each already busy at 5 s.
// Of about 900 starts that worked in both Wiis' logs, 10 took over 8 s
// (the slowest 14.4 s) and 3 over 10 s; a relaunch costs those about 6 s,
// and saves about 12 s on every start that would never have finished.
#define HBC_NET_INIT_FAST_MS 8000u
#define HBC_NET_FAST_RELAUNCHES 3
// A Wi-Fi that does not join after the reload (netinit_reset_relaunch) can
// stay that way through two or three fresh IOSes (2026-11-12, about 8 s
// each), so its relaunches in a row get more room.
#define HBC_NET_JOIN_RELAUNCHES 6
// Every automatic relaunch's countdown, whatever led to it (a failed start,
// a hung cleanup, a silent gateway): the message HBC shows before it goes.
#define HBC_NET_RELAUNCH_COUNTDOWN_S 3

// Relaunches in a row, carried across the relaunch in MEM2 just past HBC's
// records (0x91800000-0x91801140: the log target's copy, the crash block,
// the kept log; docs/devnet.md). HBC writes it only as it relaunches itself
// for the network, and reads and clears it at boot, so an app's return,
// Reload HBC or a boot from the Wii Menu starts from 0.
#define HBC_NETINIT_ADDR 0x91801140
#define HBC_NETINIT_MAGIC 0x48424352 /* 'HBCR' */

typedef struct {
	u32 magic;
	u32 relaunches;
	u32 check;       // magic ^ ~relaunches
	u32 reserved;    // a relaunch for a silent gateway: its address, else 0
} netinit_record;

// At boot, with the record as found (any contents).
void netinit_boot(const netinit_record *found);
// The relaunches in a row that led to this boot (HBCS "net_relaunches").
u32 netinit_relaunches(void);
// Whether a failed start may still go to a fast relaunch.
bool netinit_fast(void);
// The network came up: the next failure starts a new run of relaunches.
void netinit_up(void);
// The gateway answered a probe (netprobe.h): its relaunches in a row are over.
void netinit_gateway_ok(void);
// The gateway stayed silent after a Restart network: whether a fresh-IOS
// relaunch may still go (fewer than HBC_NET_FAST_RELAUNCHES in a row without
// an answer); if so the record left for it continues that run.
// The gateway's address goes in the record so the next boot knows it answers.
bool netinit_gateway_relaunch(u32 gateway);
// The network reset itself just after coming up and a fresh listener did
// not bring it back (loader.c): whether a fresh-IOS relaunch may go. Counted
// from the relaunches that led to this boot, which the start's success does
// not clear, so a reset after every start stops after
// HBC_NET_JOIN_RELAUNCHES in a row.
bool netinit_reset_relaunch(void);
// The gateway that answered before the relaunch that led to this boot, or 0.
u32 netinit_known_gateway(void);
// The record to leave for the relaunch.
void netinit_next(netinit_record *out);

// Polls net_get_status() every 50 ms until it is not -EBUSY: that status,
// -ETIMEDOUT once budget_ms have passed since started (one budget across all
// attempts), or -ECANCELED when *stop or *restart turns true. Cancellation
// does not need the TCP mutex.
s32 netinit_wait(u64 started, u32 budget_ms, unsigned attempt,
		const volatile bool *stop, const volatile bool *restart);

// After an attempt ends with status: whether to try again in this IOS.
bool netinit_retry(s32 status, bool fast);

#endif
