#define _GNU_SOURCE

#include "cli_tool_run.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "oi/arena.h"
#include "oi/json.h"

#define TOOL_ARGUMENTS_MAX (4u * 1024u * 1024u)
#define TOOL_FILE_MAX (16u * 1024u * 1024u)
#define READ_OUTPUT_MAX (256u * 1024u)
#define READ_DEFAULT_LIMIT 2000

static int fail(FILE *out, const char *format, const char *path) {
    fprintf(out, format, path, strerror(errno));
    return 1;
}

static int get_text(const oi_json_value *args, const char *key,
                    const char **out, size_t *out_len) {
    return oi_json_get_string(oi_json_object_get(args, key), out, out_len) ==
           OI_OK;
}

static int get_path(const oi_json_value *args, FILE *out,
                    const char **out_path) {
    size_t len;

    if (!get_text(args, "path", out_path, &len) || len == 0 ||
        strlen(*out_path) != len) {
        fputs("error: path must be a non-empty string\n", out);
        return 0;
    }
    return 1;
}

static int get_count(const oi_json_value *args, const char *key,
                     size_t fallback, FILE *out, size_t *out_value) {
    const oi_json_value *value = oi_json_object_get(args, key);
    double number;

    if (value == NULL) {
        *out_value = fallback;
        return 1;
    }
    if (oi_json_get_number(value, &number) != OI_OK || number < 1 ||
        number > 1e12 || (double)(unsigned long long)number != number) {
        fprintf(out, "error: %s must be a positive integer\n", key);
        return 0;
    }
    *out_value = (size_t)number;
    return 1;
}

static int open_regular(const char *path, int flags, FILE *out,
                        struct stat *out_stat) {
    int fd = open(path, flags | O_CLOEXEC);

    if (fd < 0) {
        fail(out, "error: cannot open %s: %s\n", path);
        return -1;
    }
    if (fstat(fd, out_stat) != 0) {
        fail(out, "error: cannot stat %s: %s\n", path);
        close(fd);
        return -1;
    }
    if (!S_ISREG(out_stat->st_mode)) {
        fprintf(out, "error: %s is not a regular file\n", path);
        close(fd);
        return -1;
    }
    return fd;
}

static int write_all(int fd, const char *data, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return 0;
        }
        data += n;
        len -= (size_t)n;
    }
    return 1;
}

static int run_read(const oi_json_value *args, FILE *out) {
    const char *path;
    size_t offset;
    size_t limit;
    struct stat st;
    FILE *file;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t line_len;
    size_t number = 0;
    size_t last = 0;
    size_t written = 0;
    int stopped = 0;

    if (!get_path(args, out, &path) ||
        !get_count(args, "offset", 1, out, &offset) ||
        !get_count(args, "limit", READ_DEFAULT_LIMIT, out, &limit)) {
        return 1;
    }
    int fd = open_regular(path, O_RDONLY, out, &st);
    if (fd < 0) {
        return 1;
    }
    file = fdopen(fd, "r");
    if (file == NULL) {
        close(fd);
        return fail(out, "error: cannot read %s: %s\n", path);
    }
    while ((line_len = getline(&line, &capacity, file)) >= 0) {
        char prefix[32];
        int prefix_len;
        size_t shown = (size_t)line_len;

        number++;
        if (number < offset || stopped || number - offset >= limit) {
            continue;
        }
        prefix_len = snprintf(prefix, sizeof prefix, "%6zu\t", number);
        if (written + (size_t)prefix_len + shown + 1 > READ_OUTPUT_MAX) {
            if (last > 0) {
                stopped = 1;
                continue;
            }
            shown = READ_OUTPUT_MAX - written - (size_t)prefix_len - 1;
        }
        fputs(prefix, out);
        fwrite(line, 1, shown, out);
        if (shown < (size_t)line_len) {
            fprintf(out, "\n[line %zu cut at %zu of %zd bytes]\n", number,
                    shown, line_len);
        } else if (line[line_len - 1] != '\n') {
            putc('\n', out);
        }
        written += (size_t)prefix_len + shown + 1;
        last = number;
    }
    free(line);
    if (ferror(file)) {
        fclose(file);
        return fail(out, "error: cannot read %s: %s\n", path);
    }
    fclose(file);
    if (number > 0 && offset > number) {
        fprintf(out, "error: offset %zu is past the end of %s (%zu lines)\n",
                offset, path, number);
        return 1;
    }
    if (last < number) {
        fprintf(out, "[%zu more lines; next offset %zu]\n", number - last,
                last + 1);
    }
    return 0;
}

static char *read_file(const char *path, int fd, const struct stat *st,
                       FILE *out, size_t *out_len) {
    char *data;
    size_t len = 0;

    if ((unsigned long long)st->st_size > TOOL_FILE_MAX) {
        fprintf(out, "error: %s is larger than %u bytes\n", path,
                TOOL_FILE_MAX);
        return NULL;
    }
    data = malloc((size_t)st->st_size + 1);
    if (data == NULL) {
        fputs("error: out of memory\n", out);
        return NULL;
    }
    for (;;) {
        ssize_t n = read(fd, data + len, (size_t)st->st_size + 1 - len);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            free(data);
            fail(out, "error: cannot read %s: %s\n", path);
            return NULL;
        }
        if (n == 0) {
            break;
        }
        len += (size_t)n;
        if (len > (size_t)st->st_size) {
            free(data);
            fprintf(out, "error: %s changed while it was read\n", path);
            return NULL;
        }
    }
    *out_len = len;
    return data;
}

static int replace_file(const char *path, const struct stat *existing,
                        const char *const *parts, const size_t *lens,
                        size_t count, FILE *out) {
    char target[PATH_MAX];
    char temporary[PATH_MAX + 32];
    mode_t mode;
    int fd;

    if (realpath(path, target) == NULL) {
        if (errno != ENOENT || existing != NULL ||
            strlen(path) >= sizeof target) {
            return fail(out, "error: cannot write %s: %s\n", path);
        }
        strcpy(target, path);
    }
    if (existing != NULL) {
        if (access(target, W_OK) != 0) {
            return fail(out, "error: cannot open %s: %s\n", path);
        }
        mode = existing->st_mode & 07777;
    } else {
        mode = umask(0);
        umask(mode);
        mode = 0666 & ~mode;
    }
    snprintf(temporary, sizeof temporary, "%s.oi-tmp-XXXXXX", target);
    fd = mkostemp(temporary, O_CLOEXEC);
    if (fd < 0) {
        return fail(out, "error: cannot write %s: %s\n", path);
    }
    for (size_t i = 0; i < count; i++) {
        if (!write_all(fd, parts[i], lens[i])) {
            goto failed;
        }
    }
    if (existing != NULL &&
        fchown(fd, existing->st_uid, existing->st_gid) != 0) {
        (void)!fchown(fd, (uid_t)-1, existing->st_gid);
    }
    if (fchmod(fd, mode) != 0) {
        goto failed;
    }
    if (close(fd) != 0) {
        fd = -1;
        goto failed;
    }
    fd = -1;
    if (rename(temporary, target) != 0) {
        goto failed;
    }
    return 0;

failed:
    fail(out, "error: cannot write %s: %s; the file did not change\n", path);
    if (fd >= 0) {
        close(fd);
    }
    unlink(temporary);
    return 1;
}

static int run_edit(const oi_json_value *args, FILE *out) {
    const char *path;
    const char *old_text;
    const char *new_text;
    size_t old_len;
    size_t new_len;
    struct stat st;
    size_t len;
    char *data;
    int status = 1;

    if (!get_path(args, out, &path)) {
        return 1;
    }
    if (!get_text(args, "old_text", &old_text, &old_len) || old_len == 0 ||
        !get_text(args, "new_text", &new_text, &new_len)) {
        fputs("error: old_text must be a non-empty string and new_text a "
              "string\n",
              out);
        return 1;
    }
    int fd = open_regular(path, O_RDONLY, out, &st);
    if (fd < 0) {
        return 1;
    }
    data = read_file(path, fd, &st, out, &len);
    close(fd);
    if (data == NULL) {
        return 1;
    }
    char *match = memmem(data, len, old_text, old_len);
    if (match == NULL) {
        fprintf(out, "error: old_text does not occur in %s; the file did not "
                     "change\n",
                path);
    } else if (memmem(match + 1, len - (size_t)(match + 1 - data), old_text,
                      old_len) != NULL) {
        fprintf(out, "error: old_text occurs more than once in %s; the file "
                     "did not change\n",
                path);
    } else {
        size_t before = (size_t)(match - data);
        const char *parts[] = {data, new_text, match + old_len};
        size_t lens[] = {before, new_len, len - before - old_len};

        status = replace_file(path, &st, parts, lens, 3, out);
        if (status == 0) {
            fprintf(out, "edited %s\n", path);
        }
    }
    free(data);
    return status;
}

static int run_write(const oi_json_value *args, FILE *out) {
    const char *path;
    const char *content;
    size_t content_len;
    struct stat st;
    int exists;

    if (!get_path(args, out, &path)) {
        return 1;
    }
    if (!get_text(args, "content", &content, &content_len)) {
        fputs("error: content must be a string\n", out);
        return 1;
    }
    exists = stat(path, &st) == 0;
    if (exists && !S_ISREG(st.st_mode)) {
        fprintf(out, "error: %s is not a regular file\n", path);
        return 1;
    }
    if (replace_file(path, exists ? &st : NULL, &content, &content_len, 1,
                     out) != 0) {
        return 1;
    }
    fprintf(out, "wrote %zu bytes to %s\n", content_len, path);
    return 0;
}

static oi_json_value *parse_arguments(oi_arena *arena, const char *arguments,
                                      size_t arguments_len) {
    oi_json_parser *parser = oi_json_parser_create(arena);
    oi_json_value *root = NULL;

    if (parser == NULL) {
        return NULL;
    }
    if (oi_json_parser_feed(parser, arguments, arguments_len) == OI_OK &&
        oi_json_parser_finish(parser) == OI_OK) {
        root = oi_json_parser_root(parser);
    }
    oi_json_parser_destroy(parser);
    return root != NULL && oi_json_type_of(root) == OI_JSON_OBJECT ? root
                                                                   : NULL;
}

int oi_cli_tool_run(const char *name, const char *arguments,
                    size_t arguments_len, FILE *out) {
    oi_arena *arena = oi_arena_create(0);
    oi_json_value *args;
    int status = 1;

    if (arena == NULL) {
        fputs("error: out of memory\n", out);
        return 1;
    }
    args = parse_arguments(arena, arguments, arguments_len);
    if (args == NULL) {
        fputs("error: the arguments are not a JSON object\n", out);
    } else if (strcmp(name, "read") == 0) {
        status = run_read(args, out);
    } else if (strcmp(name, "edit") == 0) {
        status = run_edit(args, out);
    } else if (strcmp(name, "write") == 0) {
        status = run_write(args, out);
    } else {
        fprintf(out, "error: unknown tool %s\n", name);
    }
    oi_arena_destroy(arena);
    return status;
}

static int run_bash(const char *arguments, size_t arguments_len) {
    oi_arena *arena = oi_arena_create(0);
    oi_json_value *args =
        arena == NULL ? NULL : parse_arguments(arena, arguments, arguments_len);
    const char *command;
    size_t command_len;
    int null_fd;

    if (args == NULL || !get_text(args, "command", &command, &command_len) ||
        command_len == 0 || strlen(command) != command_len) {
        fputs("error: command must be a non-empty string\n", stdout);
        oi_arena_destroy(arena);
        return 1;
    }
    null_fd = open("/dev/null", O_RDONLY);
    if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0) {
        fprintf(stdout, "error: cannot open /dev/null: %s\n", strerror(errno));
        oi_arena_destroy(arena);
        return 1;
    }
    close(null_fd);
    fflush(stdout);
    execlp("bash", "bash", "-c", command, (char *)NULL);
    fprintf(stdout, "error: cannot run bash: %s\n", strerror(errno));
    oi_arena_destroy(arena);
    return 127;
}

int oi_cli_tool_main(const char *name) {
    char *arguments = malloc(TOOL_ARGUMENTS_MAX);
    size_t len = 0;
    int status;

    if (arguments == NULL) {
        fputs("error: out of memory\n", stdout);
        return 1;
    }
    for (;;) {
        ssize_t n =
            read(STDIN_FILENO, arguments + len, TOOL_ARGUMENTS_MAX - len);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            fprintf(stdout, "error: cannot read the arguments: %s\n",
                    strerror(errno));
            free(arguments);
            return 1;
        }
        if (n == 0) {
            break;
        }
        len += (size_t)n;
        if (len == TOOL_ARGUMENTS_MAX) {
            fputs("error: the arguments are too large\n", stdout);
            free(arguments);
            return 1;
        }
    }
    status = strcmp(name, "bash") == 0
                 ? run_bash(arguments, len)
                 : oi_cli_tool_run(name, arguments, len, stdout);
    free(arguments);
    return status;
}
