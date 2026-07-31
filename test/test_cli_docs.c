/*
 * Documentation drift guard.
 *
 * The user-facing docs restate two things the code owns: the slash-command
 * registry and the key bindings /help prints. Neither restatement can be
 * generated (they are prose with examples around them), so they are checked
 * instead: every command name, usage string, and description in
 * cli_commands.c must appear in docs/CLI.md, and so must every Ctrl+key
 * binding /help mentions. Adding or renaming a command therefore fails the
 * suite until the guide is updated.
 *
 * Doc paths are repository-relative, like OI_CLI_BIN in the Makefile: the
 * test binaries are run from the repository root.
 */

#include "cli_command_dispatch.h"
#include "cli_commands.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char cli_guide_path[] = "docs/CLI.md";
static const char readme_path[] = "README.md";

/*
 * Reads a text file with every run of whitespace collapsed to one space, so
 * a phrase the guide wrapped across two lines still matches the single-line
 * string the registry holds.
 */
static char *read_flattened(const char *path) {
    FILE *file = fopen(path, "r");
    char *text;
    size_t cap = 1u << 16;
    size_t len = 0;
    int c;
    int in_space = 0;

    if (file == NULL) {
        return NULL;
    }
    text = malloc(cap);
    if (text == NULL) {
        fclose(file);
        return NULL;
    }
    while ((c = fgetc(file)) != EOF) {
        int is_space = c == ' ' || c == '\t' || c == '\n' || c == '\r';

        if (is_space) {
            in_space = 1;
            continue;
        }
        if (len + 2 >= cap) {
            char *grown = realloc(text, cap * 2);

            if (grown == NULL) {
                free(text);
                fclose(file);
                return NULL;
            }
            text = grown;
            cap *= 2;
        }
        if (in_space && len != 0) {
            text[len++] = ' ';
        }
        in_space = 0;
        text[len++] = (char)c;
    }
    text[len] = '\0';
    fclose(file);
    return text;
}

/* Runs /help through the real dispatcher and returns what it printed. */
static char *capture_help(void) {
    struct oi_cli_command_parse parse;
    struct oi_cli_permission permission = {OI_CLI_TOOLS_ASK};
    struct oi_cli_command_context context;
    enum oi_cli_command_result result;
    FILE *sink = tmpfile();
    char *text;
    long size;

    if (sink == NULL) {
        return NULL;
    }
    memset(&context, 0, sizeof context);
    context.out = sink;
    context.err = sink;
    context.model = "test-model";
    context.permission = &permission;
    if (oi_cli_command_parse_text("/help", 5, &parse) != OI_OK ||
        oi_cli_command_dispatch(&parse, &context, &result) != OI_OK ||
        fflush(sink) != 0 || fseek(sink, 0, SEEK_END) != 0) {
        fclose(sink);
        return NULL;
    }
    size = ftell(sink);
    if (size < 0 || fseek(sink, 0, SEEK_SET) != 0) {
        fclose(sink);
        return NULL;
    }
    text = malloc((size_t)size + 1);
    if (text == NULL) {
        fclose(sink);
        return NULL;
    }
    text[fread(text, 1, (size_t)size, sink)] = '\0';
    fclose(sink);
    return text;
}

/* Reports which string was missing, not just that something was. */
static void check_contains(const char *haystack, const char *needle,
                           const char *where) {
    if (haystack == NULL || strstr(haystack, needle) == NULL) {
        fprintf(stderr, "FAIL %s: %s does not document \"%s\"\n",
                oi_test_current, where, needle);
        CHECK(0);
    }
}

TEST(guide_documents_every_registered_command) {
    char *guide = read_flattened(cli_guide_path);
    size_t index;

    CHECK(guide != NULL);
    for (index = 0; guide != NULL && index < oi_cli_command_count();
         index++) {
        const struct oi_cli_command_definition *command =
            oi_cli_command_at(index);

        CHECK(command != NULL);
        if (command == NULL) {
            continue;
        }
        check_contains(guide, command->name, cli_guide_path);
        check_contains(guide, command->usage, cli_guide_path);
        check_contains(guide, command->description, cli_guide_path);
    }
    free(guide);
}

TEST(guide_documents_every_key_binding_help_mentions) {
    char *guide = read_flattened(cli_guide_path);
    char *help = capture_help();
    const char *cursor;

    CHECK(guide != NULL);
    CHECK(help != NULL);
    if (guide == NULL || help == NULL) {
        free(guide);
        free(help);
        return;
    }
    /* Every "Ctrl+X" /help prints, whatever the set currently is. */
    for (cursor = strstr(help, "Ctrl+"); cursor != NULL;
         cursor = strstr(cursor + 5, "Ctrl+")) {
        char binding[7];

        if (cursor[5] == '\0') {
            break;
        }
        memcpy(binding, cursor, 5); /* "Ctrl+" */
        binding[5] = cursor[5];     /* the key it names */
        binding[6] = '\0';
        check_contains(guide, binding, cli_guide_path);
    }
    /* The two non-key affordances the same summary advertises. */
    check_contains(guide, "//text", cli_guide_path);
    check_contains(guide, "command menu", cli_guide_path);
    free(guide);
    free(help);
}

TEST(readme_points_at_the_guide) {
    char *readme = read_flattened(readme_path);

    CHECK(readme != NULL);
    if (readme == NULL) {
        return;
    }
    check_contains(readme, "docs/CLI.md", readme_path);
    check_contains(readme, "/help", readme_path);
    check_contains(readme, "OI_API_KEY", readme_path);
    free(readme);
}

int main(void) {
    RUN(guide_documents_every_registered_command);
    RUN(guide_documents_every_key_binding_help_mentions);
    RUN(readme_points_at_the_guide);
    return oi_test_report();
}
