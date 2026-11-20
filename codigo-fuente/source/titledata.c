#include "titledata.h"

#include <zlib.h>

size_t titledata_inflate(const void *input, unsigned size, void *output, unsigned capacity)
{
    z_stream stream = {0};
    if (!size || inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        return 0;
    stream.next_in = (z_const Bytef *)input;
    stream.avail_in = size;
    stream.next_out = output;
    stream.avail_out = capacity;
    int result = inflate(&stream, Z_FINISH);
    size_t written = result == Z_STREAM_END && !stream.avail_in ? stream.total_out : 0;
    inflateEnd(&stream);
    return written;
}
