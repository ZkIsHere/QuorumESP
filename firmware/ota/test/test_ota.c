/* QuorumESP — OTA attempt-policy tests (gcc, WSL). No IDF needed. */
#include <stdio.h>
#include <string.h>

#include "../ota_logic.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                 \
        }                                                               \
    } while (0)

static void test_first_try_allowed(void) {
    CHECK(ota_should_attempt("", 0, "v0.2.5") == 1);
    CHECK(ota_should_attempt(NULL, 0, "v0.2.5") == 1);
}

static void test_retry_below_limit(void) {
    CHECK(ota_should_attempt("vX", 1, "vX") == 1);
    CHECK(ota_should_attempt("vX", 2, "vX") == 1);
}

static void test_blocked_at_limit(void) {
    CHECK(ota_should_attempt("vX", 3, "vX") == 0);
    CHECK(ota_should_attempt("vX", 99, "vX") == 0);
}

static void test_new_version_resets(void) {
    /* Different server version: always allowed regardless of history. */
    CHECK(ota_should_attempt("vX", 99, "vY") == 1);
    CHECK(ota_should_attempt("", 99, "vY") == 1);
}

static void test_bad_input(void) {
    CHECK(ota_should_attempt("vX", 0, "") == 0);
    CHECK(ota_should_attempt("vX", 0, NULL) == 0);
}

int main(void) {
    test_first_try_allowed();
    test_retry_below_limit();
    test_blocked_at_limit();
    test_new_version_resets();
    test_bad_input();
    if (failures == 0) {
        printf("ota: all tests passed\n");
        return 0;
    }
    printf("ota: %d FAILURES\n", failures);
    return 1;
}
