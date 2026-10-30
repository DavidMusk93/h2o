# Native H2O RPC client reuse experiment

This branch is based on H2O `534c5fd9f10d185bd9b87ac75ec05dcc0722369e`.
It adds a standalone C++17/Linux client and TLS echo server using the existing
libh2o HTTP/1.1, HTTP/2 and HTTP/3 APIs. The optimization is in the client
adapter. The branch also fixes HTTP/3 body rejection: a nonzero body callback
return closes the response state before the receive error path can invoke that
callback again. `t/00unit/lib/common/http3client.c` covers continuing bodies,
rejection, EOS and I/O errors.
It also adds an internal idle H2 connection disposal API. After all streams
complete, the adapter disposes retained H2 connections before the socket pool,
then drains pending socket disposal before destroying the loop.
`t/00unit/lib/common/http2client.c` checks timers and pending writes.

The client shares one event loop and connection pool across logical lanes.
`--request-storage reuse` retains lane objects and body/response capacity,
borrows the immutable URL for the client lifetime, and clears request-scoped
H2O header pools after completion. `fresh` is the same-binary allocation
control. For HTTP/2, `--warmup-mode serial-seed` waits for the first response
before another new connection is started, so peer SETTINGS are consumed
before multiplexing. `concurrent` is the controlled cold-start alternative.

Socket-pool capacity controls idle retention, not active sockets. Logical
concurrency and the number of observed connection objects are reported
separately. Every response must have the expected HTTP version, status,
content type, sequence and complete payload. TLS verifies the CA and SAN.
The allocation counter counts glibc allocation-family calls; it does not
measure allocated bytes or justify comparisons to other allocators.

Build the locked H2O tree's `libh2o-evloop` target, then:

```sh
make -C examples/rpc-bench H2O_BUILD=/absolute/path/to/h2o-build
```

The H2O archive, headers and OpenSSL version must agree. The Makefile permits
explicit `CPPFLAGS`, `LDFLAGS` and `LDLIBS` for a pinned dependency closure.
`OUT` must already exist. Run server and client on **different hosts**:

```sh
./h2o-native-rpc-server --listen-address :: --port 24400 \
  --tls-cert-file server.crt --tls-key-file server.key
./h2o-native-rpc-client --protocol h2 --host SERVER_IP --port 24400 \
  --tls-ca-file ca.crt --tls-server-name rpc-bench.local \
  --operations 16384 --payload-bytes 256 --concurrency 128 \
  --socket-pool-capacity 1 --warmup-seconds 10 \
  --warmup-mode serial-seed --request-storage reuse \
  --debug-operations 1 --output json
```

For H3, the certificate must contain the server IP SAN. The server creates
neither credentials nor production traffic. Supply your own test certificates.
Compare `fresh` and `reuse` in three interleaved rounds at identical settings;
include both hosts' CPU/RSS-time and scoped wire counters. Local loopback
numbers are not distributed RPC evidence. The example does not establish
production cancellation, reconnection or weak-network behavior.

To measure responses independently of requests, pass identical
`--response-bytes 16384 --response-content entropy` options to both server and
client. Sizes range from 1 through 65536 bytes. `periodic` is compressible;
`entropy` uses a fixed xorshift32 fixture. The server mixes the request sequence
into every response byte, and the client checks the complete result. The
sequence prefix adds eight framing bytes. Omitting the option preserves echo.
These synthetic bytes do not model SQL row encoding.

An oversized or different response must terminate with a validation error,
including H3 with both `--request-storage fresh` and `reuse`. For example, set
the server response to 16384 bytes and the client expectation to 1 byte, using
one operation, one lane and zero warmup. This branch fixes the duplicate
completion observed in that bounded cross-host rejection case.
