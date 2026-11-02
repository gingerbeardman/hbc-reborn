#include <sys/errno.h>
#include <unistd.h>
#include <network.h>
#include <ogc/lwp_watchdog.h>

#include "netinit.h"
#include "netdiag.h"

s32 netinit_wait(u64 started, unsigned attempt,
		const volatile bool *stop, const volatile bool *restart) {
	u64 attempt_start = gettime(), busy_note = attempt_start;
	s32 status;

	for (;;) {
		if (*stop || *restart) {
			netdiag_event("INIT cancelled: stop=%d restart=%d", *stop, *restart);
			return -ECANCELED;
		}
		if (ticks_to_millisecs(gettime() - started) >= HBC_NET_INIT_TIMEOUT_MS) {
			netdiag_event("INIT deadline reached: %u ms", HBC_NET_INIT_TIMEOUT_MS);
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
