#include "json_internal.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

oi_json_value *oi_json_new_null(oi_arena *a) {
    oi_json_value *v = oi_arena_alloc(a, sizeof *v);
    if (v == NULL) {
        return NULL;
    }
    v->type = OI_JSON_NULL;
    return v;
}

oi_json_value *oi_json_new_bool(oi_arena *a, int value) {
    oi_json_value *v = oi_arena_alloc(a, sizeof *v);
    if (v == NULL) {
        return NULL;
    }
    v->type = OI_JSON_BOOL;
    v->u.boolean = value != 0;
    return v;
}

oi_json_value *oi_json_new_number(oi_arena *a, double value) {
    oi_json_value *v = oi_arena_alloc(a, sizeof *v);
    if (v == NULL) {
        return NULL;
    }
    v->type = OI_JSON_NUMBER;
    v->u.number = value;
    return v;
}

oi_json_value *oi_json_new_string(oi_arena *a, const char *ptr, size_t len) {
    oi_json_value *v = oi_arena_alloc(a, sizeof *v);
    if (v == NULL) {
        return NULL;
    }

    char *copy = oi_arena_alloc(a, len + 1);
    if (copy == NULL) {
        return NULL;
    }
    if (len > 0) {
        memcpy(copy, ptr, len);
    }
    copy[len] = '\0';

    v->type = OI_JSON_STRING;
    v->u.string.ptr = copy;
    v->u.string.len = len;
    return v;
}

oi_json_value *oi_json_new_array(oi_arena *a) {
    oi_json_value *v = oi_arena_alloc(a, sizeof *v);
    if (v == NULL) {
        return NULL;
    }
    v->type = OI_JSON_ARRAY;
    v->u.array.items = NULL;
    v->u.array.count = 0;
    v->u.array.capacity = 0;
    return v;
}

oi_json_value *oi_json_new_object(oi_arena *a) {
    oi_json_value *v = oi_arena_alloc(a, sizeof *v);
    if (v == NULL) {
        return NULL;
    }
    v->type = OI_JSON_OBJECT;
    v->u.object.head = NULL;
    v->u.object.tail = NULL;
    v->u.object.count = 0;
    return v;
}

oi_status oi_json_array_push(oi_arena *a, oi_json_value *array,
                              oi_json_value *item) {
    assert(array->type == OI_JSON_ARRAY);

    if (array->u.array.count == array->u.array.capacity) {
        size_t new_capacity =
            array->u.array.capacity == 0 ? 4 : array->u.array.capacity * 2;
        if (new_capacity > (size_t)-1 / sizeof(oi_json_value *)) {
            return OI_ERR_NOMEM;
        }
        oi_json_value **new_items =
            oi_arena_alloc(a, new_capacity * sizeof *new_items);
        if (new_items == NULL) {
            return OI_ERR_NOMEM;
        }
        if (array->u.array.count > 0) {
            memcpy(new_items, array->u.array.items,
                   array->u.array.count * sizeof *new_items);
        }
        array->u.array.items = new_items;
        array->u.array.capacity = new_capacity;
    }

    array->u.array.items[array->u.array.count++] = item;
    return OI_OK;
}

oi_status oi_json_object_append(oi_arena *a, oi_json_value *object,
                                 const char *key, size_t key_len,
                                 oi_json_value *value) {
    assert(object->type == OI_JSON_OBJECT);

    char *key_copy = oi_arena_alloc(a, key_len + 1);
    if (key_copy == NULL) {
        return OI_ERR_NOMEM;
    }
    if (key_len > 0) {
        memcpy(key_copy, key, key_len);
    }
    key_copy[key_len] = '\0';

    struct oi_json_member *m = oi_arena_alloc(a, sizeof *m);
    if (m == NULL) {
        return OI_ERR_NOMEM;
    }
    m->key = key_copy;
    m->key_len = key_len;
    m->value = value;
    m->next = NULL;

    if (object->u.object.tail == NULL) {
        object->u.object.head = m;
    } else {
        object->u.object.tail->next = m;
    }
    object->u.object.tail = m;
    object->u.object.count++;
    return OI_OK;
}

oi_json_type oi_json_type_of(const oi_json_value *v) {
    assert(v != NULL);
    return v->type;
}

oi_status oi_json_get_bool(const oi_json_value *v, int *out) {
    if (v == NULL || v->type != OI_JSON_BOOL) {
        return OI_ERR_INVAL;
    }
    *out = v->u.boolean;
    return OI_OK;
}

oi_status oi_json_get_number(const oi_json_value *v, double *out) {
    if (v == NULL || v->type != OI_JSON_NUMBER) {
        return OI_ERR_INVAL;
    }
    *out = v->u.number;
    return OI_OK;
}

oi_status oi_json_get_string(const oi_json_value *v, const char **out_ptr,
                              size_t *out_len) {
    if (v == NULL || v->type != OI_JSON_STRING) {
        return OI_ERR_INVAL;
    }
    *out_ptr = v->u.string.ptr;
    *out_len = v->u.string.len;
    return OI_OK;
}

size_t oi_json_array_len(const oi_json_value *v) {
    if (v == NULL || v->type != OI_JSON_ARRAY) {
        return 0;
    }
    return v->u.array.count;
}

oi_json_value *oi_json_array_get(const oi_json_value *v, size_t index) {
    if (v == NULL || v->type != OI_JSON_ARRAY || index >= v->u.array.count) {
        return NULL;
    }
    return v->u.array.items[index];
}

size_t oi_json_object_len(const oi_json_value *v) {
    if (v == NULL || v->type != OI_JSON_OBJECT) {
        return 0;
    }
    return v->u.object.count;
}

oi_json_value *oi_json_object_get(const oi_json_value *v, const char *key) {
    if (v == NULL || v->type != OI_JSON_OBJECT) {
        return NULL;
    }
    size_t key_len = strlen(key);
    for (struct oi_json_member *m = v->u.object.head; m != NULL;
         m = m->next) {
        if (m->key_len == key_len && memcmp(m->key, key, key_len) == 0) {
            return m->value;
        }
    }
    return NULL;
}
