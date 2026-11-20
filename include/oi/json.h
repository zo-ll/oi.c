#ifndef OI_JSON_H
#define OI_JSON_H

#include <stddef.h>

#include "oi/arena.h"
#include "oi/status.h"

typedef struct oi_json_parser oi_json_parser;
typedef struct oi_json_value oi_json_value;

typedef enum oi_json_type {
    OI_JSON_NULL,
    OI_JSON_BOOL,
    OI_JSON_NUMBER,
    OI_JSON_STRING,
    OI_JSON_ARRAY,
    OI_JSON_OBJECT
} oi_json_type;

oi_json_parser *oi_json_parser_create(oi_arena *arena);

void oi_json_parser_destroy(oi_json_parser *p);

oi_status oi_json_parser_feed(oi_json_parser *p, const void *bytes,
                               size_t len);

oi_status oi_json_parser_finish(oi_json_parser *p);

int oi_json_parser_done(const oi_json_parser *p);

int oi_json_parser_failed(const oi_json_parser *p);

oi_json_value *oi_json_parser_root(const oi_json_parser *p);

void oi_json_parser_reset(oi_json_parser *p);

oi_json_type oi_json_type_of(const oi_json_value *v);

oi_status oi_json_get_bool(const oi_json_value *v, int *out);
oi_status oi_json_get_number(const oi_json_value *v, double *out);

oi_status oi_json_get_string(const oi_json_value *v, const char **out_ptr,
                              size_t *out_len);

size_t oi_json_array_len(const oi_json_value *v);
oi_json_value *oi_json_array_get(const oi_json_value *v, size_t index);

size_t oi_json_object_len(const oi_json_value *v);
oi_json_value *oi_json_object_get(const oi_json_value *v, const char *key);

typedef struct oi_json_writer oi_json_writer;

oi_json_writer *oi_json_writer_create(void);
void oi_json_writer_destroy(oi_json_writer *w);

oi_status oi_json_write_null(oi_json_writer *w);
oi_status oi_json_write_bool(oi_json_writer *w, int value);
oi_status oi_json_write_number(oi_json_writer *w, double value);
oi_status oi_json_write_string(oi_json_writer *w, const char *s, size_t len);

oi_status oi_json_write_array_begin(oi_json_writer *w);
oi_status oi_json_write_array_end(oi_json_writer *w);

oi_status oi_json_write_object_begin(oi_json_writer *w);
oi_status oi_json_write_object_key(oi_json_writer *w, const char *key,
                                    size_t len);
oi_status oi_json_write_object_end(oi_json_writer *w);

const char *oi_json_writer_data(const oi_json_writer *w, size_t *out_len);

#endif
