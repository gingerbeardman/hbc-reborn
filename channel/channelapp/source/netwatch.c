#include <stdio.h>
#include <string.h>
#include <ogc/lwp_watchdog.h>

#include "netwatch.h"
#include "netdiag.h"

static bool watching, link_trusted;
static u32 up_ip, lost, blips;
static s32 last_link;
static u64 next_check, bad_since;
static char bad_reason[48], last_reason[48];

static void ip_text(char *out, size_t size, u32 a) {
	snprintf(out, size, "%u.%u.%u.%u", (unsigned) (a >> 24), (unsigned) ((a >> 16) & 0xff),
		(unsigned) ((a >> 8) & 0xff), (unsigned) (a & 0xff));
}

const char *netwatch_link_name(s32 link) {
	switch (link) {
	case NCD_LINK_BUSY: return "busy";
	case NCD_LINK_NONE: return "none";
	case NCD_LINK_WIRED: return "wired";
	case NCD_LINK_WIFI_DOWN: return "wireless down";
	case NCD_LINK_WIFI_UP: return "wireless up";
	}
	return link < 0 ? "error" : "unknown";
}

void netwatch_up(u64 now, u32 ip, s32 link) {
	char a[16];
	watching = true;
	up_ip = ip;
	last_link = link;
	bad_since = 0;
	next_check = now + millisecs_to_ticks(HBC_NETWATCH_PERIOD_MS);
	link_trusted = link == NCD_LINK_WIRED || link == NCD_LINK_WIFI_UP;
	ip_text(a, sizeof(a), ip);
	netdiag_event("Watch: up at %s, link %d (%s)%s", a, (int) link,
		netwatch_link_name(link), link_trusted ? "" : ": link status not used, address only");
}

void netwatch_down(void) {
	watching = false;
	bad_since = 0;
}

bool netwatch_due(u64 now) {
	return watching && now >= next_check;
}

netwatch_verdict netwatch_sample(u64 now, s32 link, u32 ip, bool busy) {
	char a[16], reason[48];

	next_check = now + millisecs_to_ticks(HBC_NETWATCH_PERIOD_MS);
	if (!watching)
		return NETWATCH_OK;
	ip_text(a, sizeof(a), ip);
	if (link != last_link)
		netdiag_event("Watch: link %d (%s) -> %d (%s), ip %s", (int) last_link,
			netwatch_link_name(last_link), (int) link, netwatch_link_name(link), a);
	last_link = link;

	reason[0] = 0;
	if (link_trusted && (link == NCD_LINK_NONE || link == NCD_LINK_WIFI_DOWN))
		snprintf(reason, sizeof(reason), "link %s", netwatch_link_name(link));
	else if (!ip)
		snprintf(reason, sizeof(reason), "no address");
	else if (ip != up_ip)
		snprintf(reason, sizeof(reason), "address changed to %s", a);

	if (busy) {
		if (bad_since)
			netdiag_event("Watch: transfer running, wait for %s starts again", bad_reason);
		bad_since = 0;
		return NETWATCH_OK;
	}
	if (!reason[0]) {
		if (bad_since) {
			blips++;
			netdiag_event("Watch: back by itself after %llu ms (%s), link %d, ip %s",
				(unsigned long long) ticks_to_millisecs(now - bad_since), bad_reason,
				(int) link, a);
			bad_since = 0;
		}
		return NETWATCH_OK;
	}
	if (!bad_since) {
		bad_since = now;
		strcpy(bad_reason, reason);
		netdiag_event("Watch: %s (link %d, ip %s), waiting %u ms", reason, (int) link, a,
			(unsigned) HBC_NETWATCH_PERSIST_MS);
		return NETWATCH_SUSPECT;
	}
	if (ticks_to_millisecs(now - bad_since) < HBC_NETWATCH_PERSIST_MS)
		return NETWATCH_SUSPECT;
	lost++;
	strcpy(last_reason, reason);
	netdiag_event("Watch: network lost (%s for %llu ms, link %d, ip %s), lost %u",
		reason, (unsigned long long) ticks_to_millisecs(now - bad_since), (int) link, a,
		(unsigned) lost);
	watching = false;
	bad_since = 0;
	return NETWATCH_LOST;
}

u32 netwatch_lost_count(void) {
	return lost;
}

u32 netwatch_blips(void) {
	return blips;
}

const char *netwatch_last_reason(void) {
	return last_reason;
}

s32 netwatch_link(void) {
	return last_link;
}
