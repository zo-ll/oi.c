#ifndef OI_JSON_INTERNAL_H
#define OI_JSON_INTERNAL_H

#include "oi/json.h"

/*
 * Value-tree representation shared between json_value.c (accessors and
 * arena-backed constructors) and json_parse.c (builds the tree while
 * parsing). json_write.c is deliberately excluded: it serializes text
 * directly from caller calls and never touches this representation, so
 * it doesn't need to know the tree layout.
 */

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
/* Copies `len` bytes from `ptr` into arena memory (plus a trailing NUL
 * convenience byte). */
oi_json_value *oi_json_new_string(oi_arena *a, const char *ptr, size_t len);
oi_json_value *oi_json_new_array(oi_arena *a);
oi_json_value *oi_json_new_object(oi_arena *a);

/* Appends to a growable arena-backed array, doubling capacity as needed. */
oi_status oi_json_array_push(oi_arena *a, oi_json_value *array,
                              oi_json_value *item);

/* Appends a member to the end of an object's member list. Key bytes are
 * copied into arena memory. Does not check for/dedupe duplicate keys;
 * oi_json_object_get resolves duplicates by returning the first match. */
oi_status oi_json_object_append(oi_arena *a, oi_json_value *object,
                                 const char *key, size_t key_len,
                                 oi_json_value *value);

#endif /* OI_JSON_INTERNAL_H */
