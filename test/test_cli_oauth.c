#include "cli_oauth.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

TEST(challenge_matches_the_rfc_7636_vector) {
    char *challenge = NULL;
    CHECK_EQ(oi_cli_oauth_challenge(
                 "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk", &challenge),
             OI_OK);
    CHECK_STREQ(challenge, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    free(challenge);
}

TEST(random_tokens_are_url_safe_and_differ) {
    char *a = NULL;
    char *b = NULL;
    CHECK_EQ(oi_cli_oauth_random_token(&a), OI_OK);
    CHECK_EQ(oi_cli_oauth_random_token(&b), OI_OK);
    CHECK_EQ(strlen(a), 43);
    CHECK_EQ(strspn(a, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                       "0123456789-_"),
             43);
    CHECK(strcmp(a, b) != 0);
    free(a);
    free(b);
}

TEST(host_id_is_a_version_4_uuid_urn) {
    char *id = NULL;
    CHECK_EQ(oi_cli_oauth_host_id(&id), OI_OK);
    CHECK_EQ(strlen(id), 45);
    CHECK_EQ(strncmp(id, "urn:uuid:", 9), 0);
    CHECK_EQ(id[9 + 14], '4');
    free(id);
}

static const struct oi_cli_oauth_server auth = {"a.test", 443, 1};

TEST(first_authorize_url_registers_the_client) {
    struct oi_cli_oauth_authorize req = {
        NULL, "urn:uuid:1", "s t", "n", "c", NULL,
    };
    char *url = NULL;
    CHECK_EQ(oi_cli_oauth_authorize_url(&auth, &req, &url), OI_OK);
    CHECK_STREQ(
        url,
        "https://a.test/api/accounts/authorize?client_id=dynamic_agent_client"
        "&agent_name_hint=oi&ext_agent_host_id=urn%3Auuid%3A1"
        "&response_type=code"
        "&redirect_uri=http%3A%2F%2F127.0.0.1%3A1455%2Fauth%2Fcallback"
        "&scope=openid%20profile%20email%20offline_access%20resource.invoke"
        "%20chatgpt.tokens.use.direct"
        "&resource=https%3A%2F%2Fapi.openai.com%2Fv1"
        "&state=s%20t&nonce=n&code_challenge=c&code_challenge_method=S256");
    free(url);
}

TEST(later_authorize_url_reuses_the_client_and_hints_the_account) {
    struct oi_cli_oauth_authorize req = {
        "oaiapp_1", "h", "s", "n", "c", "idt",
    };
    char *url = NULL;
    CHECK_EQ(oi_cli_oauth_authorize_url(&auth, &req, &url), OI_OK);
    CHECK(strstr(url, "client_id=oaiapp_1&") != NULL);
    CHECK(strstr(url, "agent_name_hint") == NULL);
    CHECK(strstr(url, "&id_token_hint=idt") != NULL);
    free(url);
}

TEST(callback_parses_and_decodes_parameters) {
    struct oi_cli_oauth_callback callback;
    CHECK_EQ(oi_cli_oauth_parse_callback(
                 "GET /auth/callback?code=a%2Fb&state=s&client_id=oaiapp_1"
                 "&scope=x HTTP/1.1\r\nHost: h\r\n\r\n",
                 &callback),
             OI_OK);
    CHECK(callback.is_callback);
    CHECK_STREQ(callback.code, "a/b");
    CHECK_STREQ(callback.state, "s");
    CHECK_STREQ(callback.client_id, "oaiapp_1");
    CHECK(callback.error == NULL);
    oi_cli_oauth_callback_free(&callback);

    CHECK_EQ(oi_cli_oauth_parse_callback(
                 "GET /auth/callback?error=access_denied&state=s HTTP/1.1\r\n",
                 &callback),
             OI_OK);
    CHECK_STREQ(callback.error, "access_denied");
    oi_cli_oauth_callback_free(&callback);
}

TEST(other_requests_and_bad_escapes_are_not_callbacks) {
    struct oi_cli_oauth_callback callback;
    CHECK_EQ(oi_cli_oauth_parse_callback("GET /favicon.ico HTTP/1.1\r\n",
                                         &callback),
             OI_OK);
    CHECK(!callback.is_callback);
    CHECK_EQ(oi_cli_oauth_parse_callback("GET /auth/callbacks HTTP/1.1\r\n",
                                         &callback),
             OI_OK);
    CHECK(!callback.is_callback);
    CHECK_EQ(oi_cli_oauth_parse_callback("GET /auth/callback?code=%2 HTTP/1.1",
                                         &callback),
             OI_ERR_PARSE);
    CHECK_EQ(oi_cli_oauth_parse_callback("GET /auth/callback?code=%00 HTTP/1.1",
                                         &callback),
             OI_ERR_PARSE);
}

static oi_status parse_tokens(const char *body,
                              struct oi_cli_oauth_tokens *tokens) {
    return oi_cli_oauth_parse_tokens(body, strlen(body), tokens);
}

TEST(token_responses_and_refusals_parse) {
    struct oi_cli_oauth_tokens tokens;
    CHECK_EQ(parse_tokens("{\"access_token\":\"a\",\"refresh_token\":\"r\","
                          "\"id_token\":\"i\",\"expires_in\":120,"
                          "\"scope\":\"x\"}",
                          &tokens),
             OI_OK);
    CHECK_STREQ(tokens.access_token, "a");
    CHECK_STREQ(tokens.refresh_token, "r");
    CHECK(tokens.expires_in == 120);
    oi_cli_oauth_tokens_free(&tokens);

    const char *refusals[] = {
        "{\"error\":\"invalid_grant\"}",
        "{\"error\":{\"code\":\"invalid_grant\"}}",
        "{\"code\":\"invalid_grant\"}",
    };
    for (size_t i = 0; i < 3; i++) {
        CHECK_EQ(parse_tokens(refusals[i], &tokens), OI_OK);
        CHECK_STREQ(tokens.error, "invalid_grant");
        CHECK(oi_cli_oauth_error_is_terminal(tokens.error));
        oi_cli_oauth_tokens_free(&tokens);
    }
    CHECK(!oi_cli_oauth_error_is_terminal("server_error"));
    CHECK(!oi_cli_oauth_error_is_terminal(NULL));
    CHECK_EQ(parse_tokens("{}", &tokens), OI_ERR_PARSE);
    CHECK_EQ(parse_tokens("<html>", &tokens), OI_ERR_PARSE);
}

/* header.{"nonce":"n","aud":"c","exp":2000,"sub":"u","email":"e@x"}.sig */
static const char good_token[] =
    "e30.eyJub25jZSI6Im4iLCJhdWQiOiJjIiwiZXhwIjoyMDAwLCJzdWIiOiJ1IiwiZW1haWwi"
    "OiJlQHgifQ.sig";

TEST(id_token_claims_are_checked) {
    char *email = NULL;
    CHECK_EQ(oi_cli_oauth_check_id_token(good_token, "n", "c", 1000, &email),
             OI_OK);
    CHECK_STREQ(email, "e@x");
    free(email);
    CHECK_EQ(oi_cli_oauth_check_id_token(good_token, "other", "c", 1000,
                                         &email),
             OI_ERR_DENIED);
    CHECK_EQ(oi_cli_oauth_check_id_token(good_token, "n", "other", 1000,
                                         &email),
             OI_ERR_DENIED);
    CHECK_EQ(oi_cli_oauth_check_id_token(good_token, "n", "c", 3000, &email),
             OI_ERR_DENIED);
    CHECK_EQ(oi_cli_oauth_check_id_token("nodots", "n", "c", 1000, &email),
             OI_ERR_PARSE);
}

TEST(model_list_keeps_only_listed_models) {
    const char *body =
        "{\"models\":[{\"slug\":\"a\",\"display_name\":\"A\",\"visibility\":"
        "\"list\"},{\"slug\":\"b\",\"visibility\":\"hide\"},{\"slug\":\"c\","
        "\"visibility\":\"list\"}]}";
    struct oi_cli_oauth_models models;
    CHECK_EQ(oi_cli_oauth_parse_models(body, strlen(body), &models), OI_OK);
    CHECK_EQ(models.count, 2);
    CHECK_STREQ(models.items[0].slug, "a");
    CHECK_STREQ(models.items[0].name, "A");
    CHECK_STREQ(models.items[1].slug, "c");
    CHECK(models.items[1].name == NULL);
    oi_cli_oauth_models_free(&models);
    CHECK_EQ(oi_cli_oauth_parse_models("{}", 2, &models), OI_ERR_PARSE);
}

TEST(test_base_replaces_both_servers) {
    struct oi_cli_oauth_server a;
    struct oi_cli_oauth_server b;
    int is_test = 0;
    setenv("OI_CHATGPT_TEST_BASE", "http://127.0.0.1:9", 1);
    CHECK_EQ(oi_cli_oauth_servers(&a, &b, &is_test), OI_OK);
    CHECK(is_test);
    CHECK_STREQ(a.host, "127.0.0.1");
    CHECK_EQ(b.port, 9);
    CHECK_EQ(a.use_tls, 0);
    oi_cli_oauth_server_free(&a);
    oi_cli_oauth_server_free(&b);
    unsetenv("OI_CHATGPT_TEST_BASE");
    CHECK_EQ(oi_cli_oauth_servers(&a, &b, &is_test), OI_OK);
    CHECK(!is_test);
    CHECK_STREQ(a.host, "auth.openai.com");
    CHECK_STREQ(b.host, "api.openai.com");
    CHECK_EQ(b.use_tls, 1);
    oi_cli_oauth_server_free(&a);
    oi_cli_oauth_server_free(&b);
}

int main(void) {
    RUN(challenge_matches_the_rfc_7636_vector);
    RUN(random_tokens_are_url_safe_and_differ);
    RUN(host_id_is_a_version_4_uuid_urn);
    RUN(first_authorize_url_registers_the_client);
    RUN(later_authorize_url_reuses_the_client_and_hints_the_account);
    RUN(callback_parses_and_decodes_parameters);
    RUN(other_requests_and_bad_escapes_are_not_callbacks);
    RUN(token_responses_and_refusals_parse);
    RUN(id_token_claims_are_checked);
    RUN(model_list_keeps_only_listed_models);
    RUN(test_base_replaces_both_servers);
    return oi_test_report();
}
