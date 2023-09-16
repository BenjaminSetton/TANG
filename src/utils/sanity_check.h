#ifndef SANITY_CHECK_H
#define SANITY_CHECK_H

#include <assert.h>
// TODO - Include any other useful assertion headers

// Regular run-time assertions. Msg parameter _must_ be a const char*
#define TNG_ASSERT(x) assert(x)
#define TNG_ASSERT_MSG(x, msg) assert(x && msg)

// Compile-time assert
#define TNG_ASSERT_COMPILE(x) static_assert(x)

// Marking parameters as unused
#define UNUSED(x) (void)x

#endif