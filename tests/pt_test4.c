#define PT_LEAF_FANOUT 4
#define PT_FANOUT      4
#define PT_PAGE_SIZE   512
#define PT_STATIC_API
#ifndef PT_POOL_STATS
# define PT_POOL_STATS
#endif

#include "pt_tests.h"

/* T1: lifecycle */

static void test_lifecycle(void) {}

#define TESTS(X) X(lifecycle)

#define X(name) {#name, test_##name},
PT_TEST_MAIN("piecetab tests")
#undef X