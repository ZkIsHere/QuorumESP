#pragma once
/* Central task-watchdog registry (AGENTS.md §11).
 *
 * A deadlocked QDevice task must reset the chip, never look alive.
 * All long-running tasks register here; every loop wake feeds.
 * Timeout comes from sdkconfig task WDT (10 s); loops wake at <=5 s,
 * so a missed feed always means a real stall. HW reset on expiry is
 * handled by IDF (panic + reset); this layer only organizes add/feed.
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t quorumesp_watchdog_init(void);
esp_err_t quorumesp_watchdog_add_current(void);
void quorumesp_watchdog_feed(void);
void quorumesp_watchdog_remove_current(void);

#ifdef __cplusplus
}
#endif
