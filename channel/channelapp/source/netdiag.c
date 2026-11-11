#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <ogc/machine/processor.h>
#include <ogc/lwp_watchdog.h>

#include "netdiag.h"

#define RECORDS 64
#define RECORD_SIZE 192
#define LOG_PATH "sd:/hbc-network.log"
#define OLD_PATH "sd:/hbc-network.old.log"
#define LOG_LIMIT (1024 * 1024)

static char records[RECORDS][RECORD_SIZE];
static unsigned head, count, dropped;
static u64 start, last_flush;

void netdiag_event(const char *fmt, ...) {
	char message[128];
	va_list ap;
	u32 level;
	u64 now;
	va_start(ap, fmt);
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);
	_CPU_ISR_Disable(level);
	now = gettime();
	if (!start)
		start = now;
	if (count < RECORDS) {
		snprintf(records[(head + count) % RECORDS], RECORD_SIZE, "+%llu ms %s\n",
				 (unsigned long long) ticks_to_millisecs(now - start), message);
		count++;
	} else {
		dropped++;
	}
	_CPU_ISR_Restore(level);
}

void netdiag_flush(bool force) {
	FILE *f;
	char line[RECORD_SIZE];
	u32 level;
	unsigned pending, lost;
	u64 now = gettime();
	if (!force && last_flush && ticks_to_millisecs(now - last_flush) < 1000)
		return;
	last_flush = now;
	_CPU_ISR_Disable(level);
	pending = count;
	lost = dropped;
	_CPU_ISR_Restore(level);
	if (!pending && !lost)
		return;
	f = fopen(LOG_PATH, "a");
	if (!f)
		return; // Keep queued events until SD is mounted/available.
	if (fseek(f, 0, SEEK_END) == 0 && ftell(f) >= LOG_LIMIT) {
		fclose(f);
		remove(OLD_PATH);
		if (rename(LOG_PATH, OLD_PATH) != 0)
			return;
		f = fopen(LOG_PATH, "a");
		if (!f)
			return;
	}
	// One timestamp per batch; event times above are relative to this HBC boot.
	fprintf(f, "[Wii time %lld]\n", (long long) time(NULL));
	while (pending--) {
		_CPU_ISR_Disable(level);
		memcpy(line, records[head], sizeof(line));
		_CPU_ISR_Restore(level);
		if (fputs(line, f) == EOF || fflush(f) != 0)
			break;
		_CPU_ISR_Disable(level);
		head = (head + 1) % RECORDS;
		count--;
		_CPU_ISR_Restore(level);
	}
	if (lost && fprintf(f, "Dropped %u diagnostic events (queue full)\n", lost) > 0 &&
			fflush(f) == 0) {
		_CPU_ISR_Disable(level);
		dropped -= lost;
		_CPU_ISR_Restore(level);
	}
	fclose(f);
}
