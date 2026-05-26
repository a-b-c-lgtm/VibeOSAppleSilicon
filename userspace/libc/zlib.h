/* userspace/libc/zlib.h — chapter 131e stub.
 *
 * binutils-2.44's bfd/compress.c and libctf/ctf-*.c include
 * <zlib.h> unconditionally for the compressed-debug-section
 * code path.  Our linker run never produces or consumes
 * compressed sections — normal C programs don't carry
 * .zdebug_* or SHF_COMPRESSED data — so the calls are reachable
 * but cold.
 *
 * We provide just enough surface to compile bfd and libctf:
 *   - the z_stream struct (zeroed/zero-initialised by callers)
 *   - inflateInit / inflate / inflateReset / inflateEnd
 *   - compress / compressBound / uncompress
 *   - deflateInit / deflate / deflateEnd
 *   - zError
 *
 * All functions return Z_STREAM_ERROR so any caller that
 * actually exercises them fails cleanly.  A future chapter
 * that needs DWARF compression can drop in a real zlib.a.
 */
#ifndef USER_ZLIB_H
#define USER_ZLIB_H 1

#include <stddef.h>
#include <stdint.h>

typedef unsigned char Bytef;
typedef unsigned int  uInt;
typedef unsigned long uLong;
typedef unsigned long uLongf;

#define Z_OK            0
#define Z_STREAM_END    1
#define Z_NEED_DICT     2
#define Z_ERRNO        (-1)
#define Z_STREAM_ERROR (-2)
#define Z_DATA_ERROR   (-3)
#define Z_MEM_ERROR    (-4)
#define Z_BUF_ERROR    (-5)
#define Z_VERSION_ERROR (-6)

#define Z_NO_FLUSH      0
#define Z_PARTIAL_FLUSH 1
#define Z_SYNC_FLUSH    2
#define Z_FULL_FLUSH    3
#define Z_FINISH        4
#define Z_BLOCK         5
#define Z_TREES         6

#define Z_DEFAULT_COMPRESSION (-1)
#define Z_BEST_SPEED          1
#define Z_BEST_COMPRESSION    9

#define ZLIB_VERSION "1.2.11-osdev-stub"

typedef struct z_stream_s {
    const Bytef *next_in;
    uInt avail_in;
    uLong total_in;
    Bytef *next_out;
    uInt avail_out;
    uLong total_out;
    const char *msg;
    void *state;
    void *zalloc;
    void *zfree;
    void *opaque;
    int data_type;
    uLong adler;
    uLong reserved;
} z_stream;

typedef z_stream *z_streamp;

static inline int inflateInit_(z_streamp strm,
                               const char *version, int stream_size)
{
    (void)strm; (void)version; (void)stream_size;
    return Z_STREAM_ERROR;
}

#define inflateInit(strm) inflateInit_((strm), ZLIB_VERSION, (int)sizeof(z_stream))

static inline int inflate(z_streamp strm, int flush)
{
    (void)strm; (void)flush;
    return Z_STREAM_ERROR;
}

static inline int inflateReset(z_streamp strm)
{
    (void)strm;
    return Z_STREAM_ERROR;
}

static inline int inflateEnd(z_streamp strm)
{
    (void)strm;
    return Z_STREAM_ERROR;
}

static inline int deflateInit_(z_streamp strm, int level,
                               const char *version, int stream_size)
{
    (void)strm; (void)level; (void)version; (void)stream_size;
    return Z_STREAM_ERROR;
}

#define deflateInit(strm, level) deflateInit_((strm), (level), ZLIB_VERSION, (int)sizeof(z_stream))

static inline int deflate(z_streamp strm, int flush)
{
    (void)strm; (void)flush;
    return Z_STREAM_ERROR;
}

static inline int deflateEnd(z_streamp strm)
{
    (void)strm;
    return Z_STREAM_ERROR;
}

static inline int compress(Bytef *dest, uLongf *destLen,
                           const Bytef *source, uLong sourceLen)
{
    (void)dest; (void)destLen; (void)source; (void)sourceLen;
    return Z_STREAM_ERROR;
}

static inline int compress2(Bytef *dest, uLongf *destLen,
                            const Bytef *source, uLong sourceLen,
                            int level)
{
    (void)dest; (void)destLen; (void)source; (void)sourceLen; (void)level;
    return Z_STREAM_ERROR;
}

static inline uLong compressBound(uLong sourceLen)
{
    /* Worst-case bound zlib promises: sourceLen + 13 (rounded
     * up).  Returning the real bound is harmless even when the
     * stub compress() refuses — bfd uses this to size buffers
     * before the call. */
    return sourceLen + (sourceLen >> 12) + (sourceLen >> 14)
         + (sourceLen >> 25) + 13;
}

static inline int uncompress(Bytef *dest, uLongf *destLen,
                             const Bytef *source, uLong sourceLen)
{
    (void)dest; (void)destLen; (void)source; (void)sourceLen;
    return Z_STREAM_ERROR;
}

static inline const char *zError(int err)
{
    switch (err) {
    case Z_OK:            return "Z_OK";
    case Z_STREAM_END:    return "Z_STREAM_END";
    case Z_NEED_DICT:     return "Z_NEED_DICT";
    case Z_ERRNO:         return "Z_ERRNO";
    case Z_STREAM_ERROR:  return "stub: zlib disabled (chapter 131e)";
    case Z_DATA_ERROR:    return "Z_DATA_ERROR";
    case Z_MEM_ERROR:     return "Z_MEM_ERROR";
    case Z_BUF_ERROR:     return "Z_BUF_ERROR";
    case Z_VERSION_ERROR: return "Z_VERSION_ERROR";
    default:              return "unknown zlib error";
    }
}

static inline const char *zlibVersion(void)
{
    return ZLIB_VERSION;
}

/* gz* high-level API.  libctf/ctf-serialize.c references `gzFile`
 * in a function prototype and calls gzwrite once.  The function
 * itself is never reached in our link path. */
typedef struct gzFile_s *gzFile;

static inline int gzwrite(gzFile file, const void *buf, unsigned len)
{
    (void)file; (void)buf; (void)len;
    return 0;  /* documented zlib failure return for gzwrite */
}

#endif /* USER_ZLIB_H */
