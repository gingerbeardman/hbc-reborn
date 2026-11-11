#ifndef _LOADER_H_
#define _LOADER_H_

#include <unistd.h>
#include <limits.h>

#include <gctypes.h>

#include "loader_reloc.h"
#include "appentry.h"
#include "view.h"

typedef enum {
	LT_UNKNOWN = 0,
	LT_EXECUTABLE,
	LT_ZIP_APP,
	LT_ZIP_THEME
} loader_type;

typedef struct {
	loader_type type;

	u8 *data;
	u32 data_len;

	char args[ARGS_MAX_LEN];
	u32 args_len;

	char dirname[PATH_MAX];
	u32 bytes;

	// Received over the network (or a USB Gecko), not read from a device:
	// its popups close themselves, and its result goes to HBCS "upload".
	bool remote;
} loader_result;

void loader_init (void);
// False when the TCP worker did not stop: a channel reload must reset IOS.
bool loader_deinit (void);
// Suspends a worker that did not stop, before IOS and HBC are replaced.
void loader_tcp_park (void);

void loader_tcp_init (void);
// Shuts the network down and starts it again, on the next frame.
void loader_tcp_restart (void);
void loader_signal_threads (void);
bool loader_gecko_initialized (void);
bool loader_tcp_initializing (void);
bool loader_tcp_initialized (void);
// Times the network came up since HBC started.
u32 loader_net_starts (void);
// The address the network came up with, 0 while it is down (no IOS call).
u32 loader_net_ip (void);
// A failed start, or a gateway silent after an automatic restart, wants a
// fresh-IOS relaunch (netinit.h): pending, and taken once by main().
bool loader_net_relaunch_pending (void);
bool loader_take_net_relaunch (void);
// Whether the relaunch taken was for a silent gateway (taken once too).
bool loader_take_gateway_relaunch (void);
u32 loader_tcp_stack_used (void);
bool loader_handshaked (void);

void loader_load(loader_result *result, view *sub_view, app_entry *entry);
bool loader_load_executable(entry_point *ep, loader_result *result,
							view *sub_view);
bool loader_handle_zip_app(loader_result *result, view *sub_view);

#endif

