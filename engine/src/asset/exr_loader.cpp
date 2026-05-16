// =============================================================================
// exr_loader.cpp
// MARS 3D Engine — tinyexr implementation unit
//
// tinyexr is a single-header library.  Its implementation is compiled exactly
// once here.  All other translation units that include tinyexr.h must NOT
// define TINYEXR_IMPLEMENTATION (they just get the declarations).
// =============================================================================

#pragma warning(push, 0)   // suppress all warnings from third-party header

// miniz implementation is compiled separately in miniz.c.
// tinyexr uses miniz for zlib decompression.
#define TINYEXR_IMPLEMENTATION
#define TINYEXR_USE_MINIZ 1
#include <third_party/tinyexr.h>

#pragma warning(pop)
