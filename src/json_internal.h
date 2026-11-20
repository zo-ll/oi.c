#ifndef OI_JSON_INTERNAL_H
#define OI_JSON_INTERNAL_H

#include "oi/json.h"

struct oi_json_member {
    const char *key;
    size_t key_len;
    oi_json_value *value;
    struct oi_json_member *next;
};

struct oi_json_value {
    oi_json_type type;
    union {
        int boolean;
        double number;
        struct {
            const char *ptr;
            size_t len;
        } string;
        struct {
            oi_json_value **items;
            size_t count;
            size_t capacity;
        } array;
        struct {
            struct oi_json_member *head;
            struct oi_json_member *tail;
            size_t count;
        } object;
    } u;
};

oi_json_value *oi_json_new_null(oi_arena *a);
oi_json_value *oi_json_new_bool(oi_arena *a, int value);
oi_json_value *oi_json_new_number(oi_arena *a, double value);
oi_json_value *oi_json_new_string(oi_arena *a, const char *ptr, size_t len);
oi_json_value *oi_json_new_array(oi_arena *a);
oi_json_value *oi_json_new_object(oi_arena *a);

oi_status oi_json_array_push(oi_arena *a, oi_json_value *array,
                              oi_json_value *item);

oi_status oi_json_object_append(oi_arena *a, oi_json_value *object,
                                 const char *key, size_t key_len,
                                 oi_json_value *value);

#endif
