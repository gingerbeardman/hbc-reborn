#include <sys/errno.h>
#include <unistd.h>
#include <network.h>
#include <ogc/lwp_watchdog.h>

#include "netinit.h"
#include "netdiag.h"

// As found at boot, for HBCS; and the run of relaunches still going on.
static u32 boot_relaunches, streak;
// Fresh-IOS relaunches in a row that the gateway has not answered since
// (netprobe.h); cleared by an answer, not by the network merely coming up.
static u32 gateway_streak;
// A gateway known to answer, carried across a relaunch for it (record word 3).
static u32 boot_gateway, next_gateway;

void netinit_boot(const netinit_record *found) {
	boot_relaunches = 0;
	if (found->magic == HBC_NETINIT_MAGIC &&
			found->check == (HBC_NETINIT_MAGIC ^ ~found->relaunches) &&
			found->relaunches < 1000)
		boot_relaunches = found->relaunches;
	boot_gateway = boot_relaunches ? found->reserved : 0;
	next_gateway = 0;
	streak = boot_relaunches;
	gateway_streak = boot_relaunches;
}

u32 netinit_relaunches(void) {
	return boot_relaunches;
}

bool netinit_fast(void) {
	return streak < HBC_NET_FAST_RELAUNCHES;
}

void netinit_up(void) {
	streak = 0;
}

void netinit_gateway_ok(void) {
	gateway_streak = 0;
}

bool netinit_gateway_relaunch(u32 gateway) {
	if (gateway_streak >= HBC_NET_FAST_RELAUNCHES)
		return false;
	streak = gateway_streak;
	next_gateway = gateway;
	return true;
}

u32 netinit_known_gateway(void) {
	return boot_gateway;
}

void netinit_next(netinit_record *out) {
	out->magic = HBC_NETINIT_MAGIC;
	out->relaunches = streak + 1;
	out->check = HBC_NETINIT_MAGIC ^ ~out->relaunches;
	out->reserved = next_gateway;
}

s32 netinit_wait(u64 started, u32 budget_ms, unsigned attempt,
		const volatile bool *stop, const volatile bool *restart) {
	u64 attempt_start = gettime(), busy_note = attempt_start;
	s32 status;

	for (;;) {
		if (*stop || *restart) {
			netdiag_event("INIT cancelled: stop=%d restart=%d", *stop, *restart);
			return -ECANCELED;
		}
		if (ticks_to_millisecs(gettime() - started) >= budget_ms) {
			netdiag_event("INIT deadline reached: %u ms", (unsigned) budget_ms);
			return -ETIMEDOUT;
		}
		status = net_get_status();
		if (status != -EBUSY)
			return status;
		if (ticks_to_millisecs(gettime() - busy_note) >= 5000) {
			netdiag_event("Still busy: attempt=%u elapsed=%llu ms", attempt,
				(unsigned long long) ticks_to_millisecs(gettime() - attempt_start));
			busy_note = gettime();
		}
		usleep(50 * 1000);
	}
}

bool netinit_retry(s32 status, bool fast) {
	if (status == -EAGAIN)
		return true;
	return status == -ETIMEDOUT && !fast;
}
