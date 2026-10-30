#include "../../test.h"
#include "../../../../lib/common/http3client.c"

struct st_body_callback_test_t {
    int ret;
    unsigned calls;
};

static int on_body(h2o_httpclient_t *client, const char *errstr, h2o_header_t *trailers, size_t num_trailers)
{
    struct st_body_callback_test_t *test = client->data;
    ++test->calls;
    return test->ret;
}

static void test_body_callback(void)
{
    const char *errors[] = {NULL, h2o_httpclient_error_is_eos, h2o_httpclient_error_io};

    for (size_t i = 0; i != sizeof(errors) / sizeof(errors[0]); ++i) {
        for (int ret = -1; ret <= 1; ++ret) {
            struct st_body_callback_test_t test = {.ret = ret};
            struct st_h2o_http3client_req_t req = {
                .super = {.data = &test, ._cb = {.on_body = on_body}},
                .response_state = H2O_HTTP3CLIENT_RESPONSE_STATE_BODY,
            };

            ok(call_on_body(&req, errors[i]) == ret);
            ok(test.calls == 1);
            if (errors[i] != NULL || ret != 0) {
                ok(req.response_state == H2O_HTTP3CLIENT_RESPONSE_STATE_CLOSED);
                /* on_receive reports an error after a rejected body; completion must not be delivered twice. */
                notify_response_error(&req, h2o_httpclient_error_io);
                ok(test.calls == 1);
            } else {
                ok(req.response_state == H2O_HTTP3CLIENT_RESPONSE_STATE_BODY);
                notify_response_error(&req, h2o_httpclient_error_io);
                ok(test.calls == 2);
            }
            ok(req.response_state == H2O_HTTP3CLIENT_RESPONSE_STATE_CLOSED);
        }
    }
}

void test_lib__common__http3client_c(void)
{
    subtest("body callback terminal state", test_body_callback);
}
