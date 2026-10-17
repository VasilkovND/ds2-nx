// deko3d build: posix_memalign, which FFmpeg (libavutil's av_malloc) calls. devkitPro's newlib declares it
// in stdlib.h without defining it; in the NVK build the Mesa SDK's static libraries bring one. Blocks from newlib's
// memalign are released with free(), as POSIX requires of posix_memalign's.

#include <malloc.h>

#include <cerrno>
#include <cstddef>

// Moved from nfsmw (nfsmw_dk_libc.cpp): every deko3d title links the SDK's FFmpeg, so the SDK provides it.
extern "C" int posix_memalign(void** result, size_t alignment, size_t bytes) {
  if (alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0) {
    return EINVAL;
  }
  void* block = memalign(alignment, bytes);
  if (!block && bytes) {
    return ENOMEM;
  }
  *result = block;
  return 0;
}
