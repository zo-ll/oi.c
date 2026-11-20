#include "cli_composer.h"
#include "cli_input_history.h"
#include "cli_login.h"
#include "cli_terminal.h"
#include "oi/reactor.h"
#include "test.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t start_token_server(int delay_ms, unsigned short *out_port,
                                int *out_received_fd) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    int received[2];
    CHECK(listen_fd >= 0);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQ(bind(listen_fd, (struct sockaddr *)&addr, sizeof addr), 0);
    CHECK_EQ(listen(listen_fd, 1), 0);
    socklen_t alen = sizeof addr;
    CHECK_EQ(getsockname(listen_fd, (struct sockaddr *)&addr, &alen), 0);
    *out_port = ntohs(addr.sin_port);
    CHECK_EQ(pipe(received), 0);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid != 0) {
        close(listen_fd);
        close(received[1]);
        *out_received_fd = received[0];
        return pid;
    }
    close(received[0]);
    int cfd = accept(listen_fd, NULL, NULL);
    if (cfd < 0) {
        _exit(1);
    }
    char request[8192];
    size_t len = 0;
    for (;;) {
        ssize_t n = read(cfd, request + len, sizeof request - 1 - len);
        if (n <= 0) {
            _exit(1);
        }
        len += (size_t)n;
        request[len] = '\0';
        const char *body = strstr(request, "\r\n\r\n");
        const char *cl = strstr(request, "Content-Length: ");
        if (body != NULL &&
            (size_t)(request + len - (body + 4)) >=
                (cl != NULL ? strtoul(cl + 16, NULL, 10) : 0)) {
            break;
        }
    }
    (void)!write(received[1], "r", 1);
    struct timespec delay = {delay_ms / 1000,
                             (long)(delay_ms % 1000) * 1000000L};
    nanosleep(&delay, NULL);
    static const char body[] =
        "{\"access_token\":\"fresh\",\"refresh_token\":\"rotated\","
        "\"expires_in\":3600}";
    char head[128];
    int head_len = snprintf(head, sizeof head,
                            "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                            "Connection: close\r\n\r\n",
                            sizeof body - 1);
    (void)!write(cfd, head, (size_t)head_len);
    (void)!write(cfd, body, sizeof body - 1);
    close(cfd);
    _exit(0);
}

TEST(login_finishes_a_sent_renewal_when_isig_cannot_be_set) {
    char config_root[] = "/tmp/oi-cli-login-isig-XXXXXX";
    char path[256];
    char lock_path[sizeof path + 8];
    char base[64];
    unsigned short port;
    int received_fd = -1;

    CHECK(mkdtemp(config_root) != NULL);
    pid_t server = start_token_server(1500, &port, &received_fd);
    snprintf(base, sizeof base, "http://127.0.0.1:%u", (unsigned)port);
    snprintf(path, sizeof path, "%s/credentials.json", config_root);
    snprintf(lock_path, sizeof lock_path, "%s.lock", path);
    {
        char credentials[512];
        int len = snprintf(
            credentials, sizeof credentials,
            "{\"version\":1,\"active\":\"openai\",\"openai\":{"
            "\"auth\":\"oauth\",\"client_id\":\"app_test\",\"host_id\":\"h\","
            "\"access_token\":\"old\",\"refresh_token\":\"refresh\","
            "\"expires_at\":%.0f,\"welcome_shown\":true}}",
            (double)time(NULL) + 100);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        CHECK(fd >= 0);
        CHECK_EQ(write(fd, credentials, (size_t)len), (ssize_t)len);
        close(fd);
    }
    setenv("OI_CHATGPT_TEST_BASE", base, 1);

    oi_reactor *reactor = oi_reactor_create();
    FILE *err = tmpfile();
    CHECK(err != NULL);
    struct oi_cli_login login;
    memset(&login, 0, sizeof login);
    login.credentials_path = strdup(path);
    login.timeout_ms = 20000;
    login.err = err;
    login.oauth_active = 1;
    login.expires_at = (double)time(NULL) + 100;
    login.reactor = reactor;
    CHECK_EQ(oi_cli_login_before_request(&login), OI_OK);
    CHECK(login.renewal != NULL);
    {
        int flags = fcntl(received_fd, F_GETFL);
        fcntl(received_fd, F_SETFL, flags | O_NONBLOCK);
        char byte;
        int sent = 0;
        for (int i = 0; i < 500 && !sent; i++) {
            oi_status st;
            oi_reactor_step(reactor, 10, &st);
            sent = read(received_fd, &byte, 1) == 1;
        }
        CHECK(sent);
    }

    int master_fd = -1;
    int slave_fd = -1;
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    struct oi_cli_input_history history;
    oi_cli_input_history_init(&history);
    struct oi_cli_composer composer;
    CHECK_EQ(oi_cli_composer_init(&composer, slave_fd, slave_fd, &history),
             OI_OK);
    int terminal_fd = composer.terminal.input_fd;
    int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(null_fd >= 0);
    composer.terminal.input_fd = null_fd;
    CHECK(oi_cli_terminal_set_isig(&composer.terminal, 1) != OI_OK);
    CHECK_EQ(write(master_fd, "\x1b", 1), 1);
    int terminate_signal = 0;
    CHECK_EQ(oi_cli_login_run(&login, &composer, -1, &terminate_signal),
             OI_OK);
    CHECK_EQ(terminate_signal, 0);
    composer.terminal.input_fd = terminal_fd;
    oi_cli_composer_free(&composer);
    oi_cli_input_history_free(&history);
    close(null_fd);

    {
        char saved[2048] = "";
        int fd = open(path, O_RDONLY);
        CHECK(fd >= 0);
        ssize_t n = read(fd, saved, sizeof saved - 1);
        close(fd);
        saved[n > 0 ? n : 0] = '\0';
        CHECK(strstr(saved, "\"refresh_token\":\"rotated\"") != NULL);
        CHECK(strstr(saved, "\"access_token\":\"fresh\"") != NULL);
    }
    {
        char printed[512] = "";
        rewind(err);
        size_t n = fread(printed, 1, sizeof printed - 1, err);
        printed[n] = '\0';
        CHECK(strstr(printed, "finishing the ChatGPT token renewal") != NULL);
        CHECK(strstr(printed, "oi: login cancelled") != NULL);
    }

    oi_cli_login_free(&login);
    fclose(err);
    oi_reactor_destroy(reactor);
    unsetenv("OI_CHATGPT_TEST_BASE");
    kill(server, SIGKILL);
    waitpid(server, NULL, 0);
    close(received_fd);
    close(master_fd);
    close(slave_fd);
    unlink(path);
    unlink(lock_path);
    rmdir(config_root);
}

int main(void) {
    oi_test_set_deadline(120);
    signal(SIGCHLD, SIG_DFL);
    RUN(login_finishes_a_sent_renewal_when_isig_cannot_be_set);
    return oi_test_report();
}
