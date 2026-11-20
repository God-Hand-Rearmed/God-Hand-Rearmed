/* raylib's inflate (sinfl.h) for the unit tests, which do not link raylib: GHR itself gets the same sinflate from
   raylib's rcore.c (SUPPORT_COMPRESSION_API). Compiled as C, so sinflate has C linkage as it has in raylib. */
#define SINFL_IMPLEMENTATION
#include "sinfl.h"
