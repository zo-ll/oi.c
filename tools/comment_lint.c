#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct entry {
    char *path;
    long count;
};

struct table {
    struct entry *items;
    size_t len;
    size_t cap;
};

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    char *data = NULL;
    size_t len = 0;
    size_t cap = 0;
    size_t n;
    char chunk[65536];

    if (f == NULL) {
        return NULL;
    }
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (len + n + 1 > cap) {
            char *grown;
            cap = (len + n + 1) * 2;
            grown = realloc(data, cap);
            if (grown == NULL) {
                free(data);
                fclose(f);
                return NULL;
            }
            data = grown;
        }
        memcpy(data + len, chunk, n);
        len += n;
    }
    fclose(f);
    if (data == NULL) {
        data = malloc(1);
        if (data == NULL) {
            return NULL;
        }
    }
    data[len] = '\0';
    *out_len = len;
    return data;
}

static int is_c_source(const char *path) {
    size_t n = strlen(path);
    return (n > 2 && strcmp(path + n - 2, ".c") == 0) ||
           (n > 2 && strcmp(path + n - 2, ".h") == 0);
}

static long count_c(const char *s, size_t len) {
    long count = 0;
    size_t i = 0;

    while (i < len) {
        char c = s[i];
        if (c == '"' || c == '\'') {
            i++;
            while (i < len && s[i] != c && s[i] != '\n') {
                if (s[i] == '\\') {
                    i++;
                }
                i++;
            }
            i++;
        } else if (c == '/' && i + 1 < len && s[i + 1] == '*') {
            const char *end = strstr(s + i + 2, "*/");
            count++;
            i = end == NULL ? len : (size_t)(end - s) + 2;
        } else if (c == '/' && i + 1 < len && s[i + 1] == '/') {
            count++;
            while (i < len && s[i] != '\n') {
                i++;
            }
        } else {
            i++;
        }
    }
    return count;
}

static long count_hash_lines(const char *s, size_t len) {
    long count = 0;
    size_t i = 0;

    while (i < len) {
        while (i < len && (s[i] == ' ' || s[i] == '\t')) {
            i++;
        }
        if (i < len && s[i] == '#') {
            count++;
        }
        while (i < len && s[i] != '\n') {
            i++;
        }
        i++;
    }
    return count;
}

static long count_comments(const char *path) {
    size_t len = 0;
    char *data = read_file(path, &len);
    long count;

    if (data == NULL) {
        return 0;
    }
    count = is_c_source(path) ? count_c(data, len) : count_hash_lines(data, len);
    free(data);
    return count;
}

static int table_put(struct table *t, const char *path, long count) {
    if (t->len == t->cap) {
        size_t cap = t->cap == 0 ? 64 : t->cap * 2;
        struct entry *grown = realloc(t->items, cap * sizeof *grown);
        if (grown == NULL) {
            return -1;
        }
        t->items = grown;
        t->cap = cap;
    }
    t->items[t->len].path = strdup(path);
    if (t->items[t->len].path == NULL) {
        return -1;
    }
    t->items[t->len].count = count;
    t->len++;
    return 0;
}

static long table_get(const struct table *t, const char *path, int *found) {
    for (size_t i = 0; i < t->len; i++) {
        if (strcmp(t->items[i].path, path) == 0) {
            *found = 1;
            return t->items[i].count;
        }
    }
    *found = 0;
    return 0;
}

static int load_baseline(const char *path, struct table *t) {
    FILE *f = fopen(path, "r");
    char line[4096];

    if (f == NULL) {
        return 1;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        char name[4096];
        long count;
        if (sscanf(line, "%ld %4095s", &count, name) == 2 &&
            table_put(t, name, count) != 0) {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

static int compare_entries(const void *a, const void *b) {
    return strcmp(((const struct entry *)a)->path,
                  ((const struct entry *)b)->path);
}

static int check(const struct table *baseline, const struct table *now) {
    int failed = 0;

    for (size_t i = 0; i < now->len; i++) {
        int found;
        long allowed = table_get(baseline, now->items[i].path, &found);
        if (now->items[i].count > allowed) {
            fprintf(stderr,
                    "comment-lint: %s has %ld comment(s), the baseline allows "
                    "%ld. Do not add comments: put the reason in the commit "
                    "message or an ADR in docs/adr/.\n",
                    now->items[i].path, now->items[i].count, allowed);
            failed = 1;
        } else if (now->items[i].count < allowed) {
            fprintf(stderr,
                    "comment-lint: %s has %ld comment(s), fewer than the "
                    "baseline (%ld). Run `make comment-ratchet` to lower the "
                    "baseline.\n",
                    now->items[i].path, now->items[i].count, allowed);
            failed = 1;
        }
    }
    for (size_t i = 0; i < baseline->len; i++) {
        int found;
        (void)table_get(now, baseline->items[i].path, &found);
        if (!found) {
            fprintf(stderr,
                    "comment-lint: the baseline names %s, which is not "
                    "scanned. Run `make comment-ratchet`.\n",
                    baseline->items[i].path);
            failed = 1;
        }
    }
    return failed;
}

static int ratchet(const char *baseline_path, const struct table *baseline,
                   struct table *now, int bootstrap) {
    FILE *f;

    for (size_t i = 0; !bootstrap && i < now->len; i++) {
        int found;
        long allowed = table_get(baseline, now->items[i].path, &found);
        if (now->items[i].count > allowed) {
            fprintf(stderr,
                    "comment-ratchet: %s has %ld comment(s), more than the "
                    "baseline (%ld). The baseline only goes down.\n",
                    now->items[i].path, now->items[i].count, allowed);
            return 1;
        }
    }
    qsort(now->items, now->len, sizeof *now->items, compare_entries);
    f = fopen(baseline_path, "w");
    if (f == NULL) {
        perror(baseline_path);
        return 1;
    }
    for (size_t i = 0; i < now->len; i++) {
        if (now->items[i].count > 0) {
            fprintf(f, "%ld %s\n", now->items[i].count, now->items[i].path);
        }
    }
    return fclose(f) == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    struct table baseline = {0};
    struct table now = {0};
    long total = 0;
    int status;
    int loaded;

    if (argc < 3 || (strcmp(argv[1], "check") != 0 &&
                     strcmp(argv[1], "ratchet") != 0)) {
        fprintf(stderr, "usage: %s check|ratchet BASELINE FILE...\n", argv[0]);
        return 2;
    }
    loaded = load_baseline(argv[2], &baseline);
    if (loaded < 0) {
        fprintf(stderr, "comment-lint: out of memory\n");
        return 2;
    }
    for (int i = 3; i < argc; i++) {
        long count = count_comments(argv[i]);
        int found;
        total += count;
        (void)table_get(&baseline, argv[i], &found);
        if ((count > 0 || found) && table_put(&now, argv[i], count) != 0) {
            fprintf(stderr, "comment-lint: out of memory\n");
            return 2;
        }
    }
    status = strcmp(argv[1], "check") == 0
                 ? check(&baseline, &now)
                 : ratchet(argv[2], &baseline, &now, loaded == 1);
    if (status == 0) {
        printf("comment-lint: %ld comment(s) remain in the baseline\n", total);
    }
    return status;
}
