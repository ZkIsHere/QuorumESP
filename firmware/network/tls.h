#pragma once
/* TLS via mbedTLS (server side). See docs/tls.md.
 *
 * Round 2a: server presents a cert; client cert NOT required.
 * Round 2b: require_client_cert=1 enforces chain + CN == cluster_name.
 * No dev certs embedded -> TLS unavailable (fail-closed, plaintext only).
 */
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t network_tls_init(void);
int network_tls_available(void);

typedef struct qesp_tls_session qesp_tls_session_t;

/* Upgrade an accepted plaintext fd after STARTTLS. On success returns a
 * session (release with network_tls_close, which closes exactly once).
 * On NULL the fd is already dead — do not touch it. */
qesp_tls_session_t *network_tls_upgrade(int fd, const char *expected_cn,
                                        int require_client_cert);
/* Blocking, exact length (loops internally). <0 on any failure. */
int network_tls_read(qesp_tls_session_t *s, uint8_t *buf, size_t len);
int network_tls_write(qesp_tls_session_t *s, const uint8_t *buf, size_t len);
void network_tls_close(qesp_tls_session_t *s);

#ifdef __cplusplus
}
#endif
