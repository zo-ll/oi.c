#include "cli_tool_run.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char directory[] = "/tmp/oi-tool-run-XXXXXX";

static char *read_stream(FILE *stream) {
    long len;
    char *text;

    CHECK_EQ(fflush(stream), 0);
    CHECK_EQ(fseek(stream, 0, SEEK_END), 0);
    len = ftell(stream);
    CHECK(len >= 0);
    rewind(stream);
    text = malloc((size_t)len + 1);
    CHECK(text != NULL);
    if (text == NULL) {
        return NULL;
    }
    CHECK_EQ(fread(text, 1, (size_t)len, stream), (size_t)len);
    text[len] = '\0';
    return text;
}

static int run(const char *name, const char *arguments, char **out_text) {
    FILE *out = tmpfile();
    int status;

    CHECK(out != NULL);
    status = oi_cli_tool_run(name, arguments, strlen(arguments), out);
    *out_text = read_stream(out);
    fclose(out);
    return status;
}

static void put_file(const char *path, const char *content) {
    FILE *file = fopen(path, "w");

    CHECK(file != NULL);
    if (file != NULL) {
        fputs(content, file);
        fclose(file);
    }
}

static char *get_file(const char *path) {
    FILE *file = fopen(path, "r");
    char *text;

    CHECK(file != NULL);
    if (file == NULL) {
        return NULL;
    }
    text = read_stream(file);
    fclose(file);
    return text;
}

TEST(read_returns_numbered_lines_in_a_range) {
    char *text;

    put_file("notes.txt", "one\ntwo\nthree\nfour");
    CHECK_EQ(run("read", "{\"path\":\"notes.txt\"}", &text), 0);
    CHECK_STREQ(text,
                "     1\tone\n     2\ttwo\n     3\tthree\n     4\tfour\n");
    free(text);

    CHECK_EQ(run("read", "{\"path\":\"notes.txt\",\"offset\":2,\"limit\":2}",
                 &text),
             0);
    CHECK_STREQ(text, "     2\ttwo\n     3\tthree\n"
                      "[1 more lines; next offset 4]\n");
    free(text);
}

TEST(read_reports_bad_requests) {
    char *text;

    CHECK_EQ(run("read", "{\"path\":\"missing.txt\"}", &text), 1);
    CHECK(strstr(text, "error: cannot open missing.txt") != NULL);
    free(text);

    put_file("short.txt", "only\n");
    CHECK_EQ(run("read", "{\"path\":\"short.txt\",\"offset\":5}", &text), 1);
    CHECK(strstr(text, "offset 5 is past the end of short.txt (1 lines)") !=
          NULL);
    free(text);

    CHECK_EQ(run("read", "{\"path\":\"short.txt\",\"limit\":0}", &text), 1);
    CHECK(strstr(text, "limit must be a positive integer") != NULL);
    free(text);

    CHECK_EQ(run("read", "{\"path\":\".\"}", &text), 1);
    CHECK(strstr(text, "is not a regular file") != NULL);
    free(text);

    CHECK_EQ(run("read", "[1]", &text), 1);
    CHECK(strstr(text, "not a JSON object") != NULL);
    free(text);
}

TEST(read_output_stays_under_its_byte_limit) {
    FILE *file = fopen("many.txt", "w");
    char *text;

    CHECK(file != NULL);
    for (int i = 0; i < 300000 && file != NULL; i++) {
        putc('\n', file);
    }
    if (file != NULL) {
        fclose(file);
    }
    CHECK_EQ(run("read", "{\"path\":\"many.txt\",\"limit\":300000}",
                 &text),
             0);
    CHECK(strlen(text) <= 256u * 1024u + 64u);
    CHECK(strstr(text, "more lines; next offset") != NULL);
    free(text);
}

TEST(edit_replaces_one_exact_block) {
    char *text;
    char *content;

    put_file("code.c", "int a;\nint b;\nint c;\n");
    CHECK_EQ(run("edit",
                 "{\"path\":\"code.c\",\"old_text\":\"int b;\\n\","
                 "\"new_text\":\"long b;\\nlong bb;\\n\"}",
                 &text),
             0);
    CHECK_STREQ(text, "edited code.c\n");
    free(text);
    content = get_file("code.c");
    CHECK_STREQ(content, "int a;\nlong b;\nlong bb;\nint c;\n");
    free(content);
}

TEST(edit_keeps_the_mode_and_follows_a_link) {
    struct stat st;
    char *text;
    char *content;

    put_file("target.txt", "before\n");
    CHECK_EQ(chmod("target.txt", 0640), 0);
    CHECK_EQ(symlink("target.txt", "link.txt"), 0);
    CHECK_EQ(run("edit",
                 "{\"path\":\"link.txt\",\"old_text\":\"before\","
                 "\"new_text\":\"after\"}",
                 &text),
             0);
    free(text);
    CHECK_EQ(lstat("link.txt", &st), 0);
    CHECK(S_ISLNK(st.st_mode));
    CHECK_EQ(stat("target.txt", &st), 0);
    CHECK_EQ(st.st_mode & 07777, 0640);
    content = get_file("target.txt");
    CHECK_STREQ(content, "after\n");
    free(content);
}

TEST(edit_leaves_the_file_unchanged_when_the_text_is_absent_or_repeated) {
    char *text;
    char *content;

    put_file("twice.txt", "x = 1;\nx = 1;\n");
    CHECK_EQ(run("edit",
                 "{\"path\":\"twice.txt\",\"old_text\":\"x = 1;\","
                 "\"new_text\":\"x = 2;\"}",
                 &text),
             1);
    CHECK(strstr(text, "occurs more than once") != NULL);
    free(text);
    CHECK_EQ(run("edit",
                 "{\"path\":\"twice.txt\",\"old_text\":\"y\","
                 "\"new_text\":\"z\"}",
                 &text),
             1);
    CHECK(strstr(text, "does not occur") != NULL);
    free(text);
    put_file("overlap.txt", "aaa");
    CHECK_EQ(run("edit",
                 "{\"path\":\"overlap.txt\",\"old_text\":\"aa\","
                 "\"new_text\":\"b\"}",
                 &text),
             1);
    CHECK(strstr(text, "occurs more than once") != NULL);
    free(text);
    CHECK_EQ(run("edit", "{\"path\":\"absent.txt\",\"old_text\":\"a\","
                         "\"new_text\":\"b\"}",
                 &text),
             1);
    free(text);
    CHECK(access("absent.txt", F_OK) != 0);

    content = get_file("twice.txt");
    CHECK_STREQ(content, "x = 1;\nx = 1;\n");
    free(content);
    content = get_file("overlap.txt");
    CHECK_STREQ(content, "aaa");
    free(content);
}

TEST(edit_and_write_refuse_a_read_only_file) {
    char *text;
    char *content;

    put_file("locked.txt", "keep\n");
    CHECK_EQ(chmod("locked.txt", 0444), 0);
    CHECK_EQ(run("write", "{\"path\":\"locked.txt\",\"content\":\"y\"}",
                 &text),
             1);
    CHECK(strstr(text, "error: cannot open locked.txt") != NULL);
    free(text);
    CHECK_EQ(run("edit",
                 "{\"path\":\"locked.txt\",\"old_text\":\"keep\","
                 "\"new_text\":\"lost\"}",
                 &text),
             1);
    CHECK(strstr(text, "error: cannot open locked.txt") != NULL);
    free(text);
    content = get_file("locked.txt");
    CHECK_STREQ(content, "keep\n");
    free(content);
}

TEST(write_creates_and_replaces_files) {
    char *text;
    char *content;

    CHECK_EQ(run("write", "{\"path\":\"new.txt\",\"content\":\"hello\\n\"}",
                 &text),
             0);
    CHECK_STREQ(text, "wrote 6 bytes to new.txt\n");
    free(text);
    CHECK_EQ(run("write", "{\"path\":\"new.txt\",\"content\":\"\"}", &text),
             0);
    free(text);
    content = get_file("new.txt");
    CHECK_STREQ(content, "");
    free(content);

    CHECK_EQ(run("write", "{\"path\":\"no/such/dir/f\",\"content\":\"x\"}",
                 &text),
             1);
    CHECK(strstr(text, "error: cannot write no/such/dir/f") != NULL);
    free(text);
}

int main(void) {
    char *previous = getcwd(NULL, 0);

    CHECK(mkdtemp(directory) != NULL);
    CHECK_EQ(chdir(directory), 0);
    RUN(read_returns_numbered_lines_in_a_range);
    RUN(read_reports_bad_requests);
    RUN(read_output_stays_under_its_byte_limit);
    RUN(edit_replaces_one_exact_block);
    RUN(edit_keeps_the_mode_and_follows_a_link);
    RUN(edit_leaves_the_file_unchanged_when_the_text_is_absent_or_repeated);
    RUN(edit_and_write_refuse_a_read_only_file);
    RUN(write_creates_and_replaces_files);
    unlink("notes.txt");
    unlink("short.txt");
    unlink("code.c");
    unlink("twice.txt");
    unlink("overlap.txt");
    unlink("new.txt");
    unlink("many.txt");
    unlink("locked.txt");
    unlink("link.txt");
    unlink("target.txt");
    CHECK_EQ(chdir(previous), 0);
    rmdir(directory);
    free(previous);
    return oi_test_report();
}
