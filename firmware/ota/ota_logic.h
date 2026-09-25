#pragma once
/* OTA attempt policy, pure logic (no IDF, host-testable).
 *
 * Problem: without memory, a server stuck serving a crashing image causes
 * an endless update/crash/rollback loop (observed live 2026-09-24).
 * Fix: remember the last attempted version + consecutive attempts; after
 * OTA_MAX_ATTEMPTS failures on the SAME version, stop trying until the
 * server offers something else. A confirmed boot clears the memory.
 */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_MAX_ATTEMPTS 3

/* 1 = attempt the download, 0 = skip (known-bad version). */
int ota_should_attempt(const char *stored_ver, unsigned stored_n,
                       const char *server_ver);

#ifdef __cplusplus
}
#endif
