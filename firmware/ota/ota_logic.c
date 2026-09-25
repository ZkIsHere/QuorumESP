/* OTA attempt policy. See ota_logic.h. */
#include "ota_logic.h"

#include <string.h>

int ota_should_attempt(const char *stored_ver, unsigned stored_n,
                       const char *server_ver) {
    if (server_ver == NULL || server_ver[0] == '\0') {
        return 0;
    }
    if (stored_ver != NULL && stored_ver[0] != '\0' &&
        strcmp(stored_ver, server_ver) == 0 && stored_n >= OTA_MAX_ATTEMPTS) {
        return 0;
    }
    return 1;
}
