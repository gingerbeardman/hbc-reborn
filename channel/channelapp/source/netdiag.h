#ifndef _NETDIAG_H_
#define _NETDIAG_H_

#include <stdbool.h>

// Any thread may enqueue an event; only the menu thread touches the SD card.
void netdiag_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void netdiag_flush(bool force);

#endif
