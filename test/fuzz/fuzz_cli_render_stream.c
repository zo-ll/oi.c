
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli_render_stream.h"

#define FUZZ_MAX_INPUT (16u * 1024)

struct run_result {
    char *data;
    size_t len;
    int nomem;
};

static void run(const uint8_t *data, size_t size, size_t split,
                struct run_result *out) {
    struct oi_cli_render_stream stream;
    FILE *mem = open_memstream(&out->data, &out->len);
    oi_status status;

    assert(mem != NULL);
    out->nomem = 0;

    status = oi_cli_render_stream_init(&stream, mem, 1);
    assert(status == OI_OK);

    if (split == 0 || split >= size) {
        status =
            oi_cli_render_stream_feed(&stream, (const char *)data, size);
    } else {
        status =
            oi_cli_render_stream_feed(&stream, (const char *)data, split);
        if (status == OI_OK) {
            status = oi_cli_render_stream_feed(
                &stream, (const char *)data + split, size - split);
        }
    }
    if (status == OI_ERR_NOMEM) {
        out->nomem = 1;
    } else {
        assert(status == OI_OK);
        status = oi_cli_render_stream_finish(&stream);
        if (status == OI_ERR_NOMEM) {
            out->nomem = 1;
        } else {
            assert(status == OI_OK);
        }
    }

    oi_cli_render_stream_free(&stream);
    fclose(mem);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct run_result whole = {0};
    struct run_result chunked = {0};
    size_t split;

    if (size > FUZZ_MAX_INPUT) {
        return 0;
    }
    split = size < 2 ? 0 : 1 + ((size_t)data[0] % (size - 1));

    run(data, size, 0, &whole);
    run(data, size, split, &chunked);

    if (!whole.nomem && !chunked.nomem) {
        assert(whole.len == chunked.len);
        if (whole.len == chunked.len) {
            assert(memcmp(whole.data, chunked.data, whole.len) == 0);
        }
    }

    free(whole.data);
    free(chunked.data);
    return 0;
}
