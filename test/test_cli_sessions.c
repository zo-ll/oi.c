#include "cli_sessions.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

TEST(default_root_uses_xdg_state_home) {
    char state_home[128];
    char *root = NULL;

    snprintf(state_home, sizeof state_home, "/tmp/oi-state-root-%ld",
             (long)getpid());
    CHECK_EQ(setenv("XDG_STATE_HOME", state_home, 1), 0);
    CHECK_EQ(oi_cli_sessions_default_root(&root), OI_OK);
    {
        char expected[160];
        snprintf(expected, sizeof expected, "%s/oi/sessions", state_home);
        CHECK_STREQ(root, expected);
    }
    free(root);
}

TEST(create_makes_private_timestamped_session) {
    char root[128];
    struct oi_cli_session_location location;
    struct stat info;

    snprintf(root, sizeof root, "/tmp/oi-session-location-%ld",
             (long)getpid());
    oi_cli_session_location_init(&location);
    CHECK_EQ(oi_cli_session_location_create(root, &location), OI_OK);
    CHECK(location.id != NULL);
    CHECK(strlen(location.id) < 64);
    CHECK(strstr(location.id, "/") == NULL);
    CHECK(strstr(location.history_path, "history.oilog") != NULL);
    CHECK_EQ(stat(location.directory, &info), 0);
    CHECK(S_ISDIR(info.st_mode));
    CHECK_EQ(info.st_mode & 077, 0);
    CHECK(access(location.history_path, F_OK) != 0);

    CHECK_EQ(rmdir(location.directory), 0);
    oi_cli_session_location_free(&location);
    CHECK_EQ(rmdir(root), 0);
}

TEST(two_creations_are_unique) {
    char root[128];
    struct oi_cli_session_location first;
    struct oi_cli_session_location second;

    snprintf(root, sizeof root, "/tmp/oi-session-unique-%ld",
             (long)getpid());
    oi_cli_session_location_init(&first);
    oi_cli_session_location_init(&second);
    CHECK_EQ(oi_cli_session_location_create(root, &first), OI_OK);
    CHECK_EQ(oi_cli_session_location_create(root, &second), OI_OK);
    CHECK(strcmp(first.id, second.id) != 0);

    CHECK_EQ(rmdir(first.directory), 0);
    CHECK_EQ(rmdir(second.directory), 0);
    oi_cli_session_location_free(&first);
    oi_cli_session_location_free(&second);
    CHECK_EQ(rmdir(root), 0);
}

TEST(bad_arguments_are_rejected) {
    struct oi_cli_session_location location;

    oi_cli_session_location_init(&location);
    CHECK_EQ(oi_cli_sessions_default_root(NULL), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_location_create(NULL, NULL), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_location_create("", &location), OI_ERR_INVAL);
    oi_cli_session_location_free(&location);
    oi_cli_session_location_free(&location);
}

int main(void) {
    RUN(default_root_uses_xdg_state_home);
    RUN(create_makes_private_timestamped_session);
    RUN(two_creations_are_unique);
    RUN(bad_arguments_are_rejected);
    return oi_test_report();
}
