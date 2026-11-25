/* Bundled MIT runtime for desktop plugin host: Lua 5.4.7 + qrcodegen */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif

#define MAKE_LIB
#if !defined(_WIN32)
#define LUA_USE_POSIX
#endif

#include "lprefix.h"
#include "lctype.c"
#include "lmem.c"
#include "lstring.c"
#include "lobject.c"
#include "ltable.c"
#include "ltm.c"
#include "lfunc.c"
#include "lstate.c"
#include "lgc.c"
#include "ldo.c"
#include "lzio.c"
#include "llex.c"
#include "lopcodes.c"
#include "lcode.c"
#include "lparser.c"
#include "ldump.c"
#include "lundump.c"
#include "ldebug.c"
#include "lvm.c"
#include "lapi.c"

#include "lauxlib.c"
#include "lbaselib.c"
#include "lcorolib.c"
#include "ldblib.c"
#include "liolib.c"
#include "lmathlib.c"
#include "loadlib.c"
#include "loslib.c"
#include "lstrlib.c"
#include "ltablib.c"
#include "lutf8lib.c"
#include "linit.c"

#include "qrcodegen.c"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
