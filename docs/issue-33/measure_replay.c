/* Measures the actual request bytes oi sends when resuming a session,
 * using the integration test's capture mock server. Throwaway tool. */
#include "mock_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

int main(int argc, char **argv) {
    const char *session_dir = argv[1];
    const char *session_id = argv[2];
    const char *prompt = argv[3];
    struct mock_turn turns[1] = {{NULL, "data: {\"choices\":[{\"index\":0,"
        "\"delta\":{\"content\":\"ok\"}}]}\n\ndata: [DONE]\n\n", 0}};
    struct mock_api api;
    int status;
    pid_t pid;
    size_t req_len;
    char *req;
    char *body;
    char port_text[16];

    (void)argc;
    if (!mock_api_start(&api, turns, 1)) {
        fprintf(stderr, "mock start failed\n");
        return 1;
    }
    snprintf(port_text, sizeof port_text, "%u", api.port);
    pid = fork();
    if (pid == 0) {
        char *argv_child[] = {"build/oi", "--host", "127.0.0.1", "--port",
            port_text, "--no-tls", "--api-key", "test-key", "--deny-tools",
            "--session-dir", (char *)session_dir, "--session",
            (char *)session_id, (char *)prompt, NULL};
        execv("build/oi", argv_child);
        _exit(127);
    }
    waitpid(pid, &status, 0);
    req = mock_api_request(&api, 0, &req_len);
    if (req == NULL) {
        fprintf(stderr, "no request captured\n");
        return 1;
    }
    /* strip headers; find the JSON body */
    body = strstr(req, "\r\n\r\n");
    if (body) {
        body += 4;
    } else {
        body = req;
    }
    printf("wire bytes (headers+body): %zu\n", req_len);
    printf("body bytes: %zu\n", strlen(body));
    printf("first 120 body chars: %.120s\n", body);
    {
        FILE *dump = fopen("/tmp/oi-body.json", "w");
        if (dump != NULL) {
            fwrite(body, 1, strlen(body), dump);
            fclose(dump);
        }
    }
    mock_api_stop(&api);
    free(req);
    return 0;
}
