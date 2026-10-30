#include <unistd.h>
#include "../../../../lib/common/http2client.c"
#include "picotest.h"

static void on_write(h2o_socket_t *sock, const char *err)
{
    /* Synchronous disposal must cancel this callback with the socket. */
    ok(0);
}

static void test_idle_dispose(int pending_write)
{
    h2o_loop_t *loop = h2o_evloop_create();
    h2o_httpclient_ctx_t ctx = {.loop = loop};
    h2o_linklist_t connections;
    h2o_linklist_init_anchor(&connections);
    int fds[2];
    ok(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    struct st_h2o_http2client_conn_t *conn = h2o_mem_alloc(sizeof(*conn));
    memset(conn, 0, sizeof(*conn));
    conn->super.ctx = &ctx;
    conn->super.sock = h2o_evloop_socket_create(loop, fds[0], 0);
    conn->streams = kh_init(stream);
    h2o_linklist_insert(&connections, &conn->super.link);
    h2o_linklist_init_anchor(&conn->output.sending_streams);
    h2o_linklist_init_anchor(&conn->output.sent_streams);
    h2o_buffer_init(&conn->output.buf, &h2o_socket_buffer_prototype);
    h2o_mem_init_pool(&conn->rst_streams_pool);
    conn->super.origin_url.authority = h2o_strdup(NULL, "rpc-bench.local", SIZE_MAX);
    conn->super.origin_url.host = h2o_strdup(NULL, "rpc-bench.local", SIZE_MAX);
    conn->super.origin_url.path = h2o_strdup(NULL, "/rpc", SIZE_MAX);
    h2o_timer_link(loop, 5000, &conn->io_timeout);
    h2o_timer_link(loop, 120000, &conn->keepalive_timeout);
    h2o_timer_link(loop, 0, &conn->output.defer_timeout);
    if (pending_write) {
        h2o_buffer_init(&conn->output.buf_in_flight, &h2o_socket_buffer_prototype);
        h2o_buffer_append(&conn->output.buf_in_flight, "pending", 7);
        h2o_iovec_t bytes = h2o_iovec_init(conn->output.buf_in_flight->bytes, conn->output.buf_in_flight->size);
        h2o_socket_write(conn->super.sock, &bytes, 1, on_write);
    }
    h2o_httpclient__h2_dispose_connection(&conn->super);
    ok(h2o_linklist_is_empty(&connections));
    ok(h2o_timerwheel_get_wake_at(loop->_timeouts) == UINT64_MAX);
    h2o_evloop_run(loop, 0);
    h2o_evloop_destroy(loop);
    close(fds[1]);
}

void test_lib__common__http2client_c(void)
{
    subtest("idle connection timers", test_idle_dispose, 0);
    subtest("idle connection pending write", test_idle_dispose, 1);
}
