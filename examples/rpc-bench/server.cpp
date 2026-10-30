#include <arpa/inet.h>
#include <h2o.h>
#include <h2o/http2.h>
#include <h2o/http3_server.h>
#include <netinet/in.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <picotls/openssl.h>
#include <quicly.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

namespace {

struct ServerConfig {
  std::string listen_address = "0.0.0.0";
  std::uint16_t port = 0;
  std::string certificate_file;
  std::string key_file;
};

std::atomic<bool> stop_requested{false};

void requestStop(int) { stop_requested.store(true, std::memory_order_release); }

bool parseUnsigned(const char *text, std::uint64_t *value) {
  if (text == nullptr || *text == '\0' || *text == '-') {
    return false;
  }
  char *end = nullptr;
  errno = 0;
  const unsigned long long parsed = std::strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    return false;
  }
  *value = parsed;
  return true;
}

bool parseArgs(int argc, char **argv, ServerConfig *config,
               std::string *error) {
  for (int index = 1; index < argc; ++index) {
    const std::string option = argv[index];
    if (option == "--help") {
      std::cout << "h2o-native-rpc-server --listen-address ADDRESS --port N "
                   "--tls-cert-file PATH --tls-key-file PATH\n";
      std::exit(0);
    }
    if (++index >= argc || *argv[index] == '\0') {
      *error = option + " requires a value";
      return false;
    }
    if (option == "--listen-address") {
      config->listen_address = argv[index];
    } else if (option == "--tls-cert-file") {
      config->certificate_file = argv[index];
    } else if (option == "--tls-key-file") {
      config->key_file = argv[index];
    } else if (option == "--port") {
      std::uint64_t value = 0;
      if (!parseUnsigned(argv[index], &value) ||
          value > std::numeric_limits<std::uint16_t>::max()) {
        *error = "--port requires a valid uint16";
        return false;
      }
      config->port = static_cast<std::uint16_t>(value);
    } else {
      *error = "unknown option: " + option;
      return false;
    }
  }
  if (config->port == 0 || config->certificate_file.empty() ||
      config->key_file.empty()) {
    *error = "port, TLS certificate, and TLS key are required";
    return false;
  }
  return true;
}

int createBoundSocket(const ServerConfig &config, int type,
                      std::string *error) {
  const bool ipv6 = config.listen_address.find(':') != std::string::npos;
  const int family = ipv6 ? AF_INET6 : AF_INET;
  const int fd = socket(family, type | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    *error = std::strerror(errno);
    return -1;
  }
  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_storage address{};
  socklen_t address_size;
  void *ip;
  if (ipv6) {
    auto *v6 = reinterpret_cast<sockaddr_in6 *>(&address);
    v6->sin6_family = AF_INET6;
    v6->sin6_port = htons(config.port);
    address_size = sizeof(*v6);
    ip = &v6->sin6_addr;
  } else {
    auto *v4 = reinterpret_cast<sockaddr_in *>(&address);
    v4->sin_family = AF_INET;
    v4->sin_port = htons(config.port);
    address_size = sizeof(*v4);
    ip = &v4->sin_addr;
  }
  if (inet_pton(family, config.listen_address.c_str(), ip) != 1) {
    *error = "--listen-address must be an IP address";
    close(fd);
    return -1;
  }
  if (bind(fd, reinterpret_cast<sockaddr *>(&address), address_size) != 0) {
    *error = std::strerror(errno);
    close(fd);
    return -1;
  }
  return fd;
}

class H2oNativeServer {
public:
  ~H2oNativeServer() { dispose(); }

  bool initialize(const ServerConfig &options, std::string *error) {
    h2o_config_init(&config_);
    configured_ = true;
    config_.http2.idle_timeout = 120'000;
    config_.http2.max_concurrent_requests_per_connection = 1024;
    config_.http3.idle_timeout = 120'000;

    h2o_hostconf_t *host = h2o_config_register_host(
        &config_, h2o_iovec_init(H2O_STRLIT("default")), 65535);
    h2o_pathconf_t *path = h2o_config_register_path(host, "/rpc", 0);
    h2o_handler_t *handler = h2o_create_handler(path, sizeof(*handler));
    handler->on_req = onRequest;

    loop_ = h2o_evloop_create();
    if (loop_ == nullptr) {
      *error = "h2o_evloop_create failed";
      return false;
    }
    h2o_context_init(&context_, loop_, &config_);
    context_initialized_ = true;
    accept_context_.ctx = &context_;
    accept_context_.hosts = config_.hosts;

    if (!initializeTcp(options, error) || !initializeQuic(options, error)) {
      return false;
    }
    return true;
  }

  void run() {
    while (!stop_requested.load(std::memory_order_acquire)) {
      h2o_evloop_run(loop_, 10);
    }
  }

private:
  struct ResumptionTokenGenerator {
    quicly_generate_resumption_token_t super;
    ptls_aead_context_t *aead = nullptr;
  };

  static int onRequest(h2o_handler_t *, h2o_req_t *request) {
    if (!h2o_memis(request->path_normalized.base, request->path_normalized.len,
                   H2O_STRLIT("/rpc"))) {
      return -1;
    }
    if (!h2o_memis(request->method.base, request->method.len,
                   H2O_STRLIT("POST"))) {
      request->res.status = 405;
      request->res.reason = "Method Not Allowed";
      h2o_send_inline(request, H2O_STRLIT("method not allowed"));
      return 0;
    }
    if (request->entity.len < sizeof(std::uint64_t)) {
      request->res.status = 400;
      request->res.reason = "Bad Request";
      h2o_send_inline(request, H2O_STRLIT("invalid rpc frame"));
      return 0;
    }
    request->res.status = 200;
    request->res.reason = "OK";
    request->res.content_length = request->entity.len;
    h2o_add_header(&request->pool, &request->res.headers,
                   H2O_TOKEN_CONTENT_TYPE, nullptr,
                   H2O_STRLIT("application/octet-stream"));
    h2o_send_inline(request, request->entity.base, request->entity.len);
    return 0;
  }

  static void onTcpAccept(h2o_socket_t *listener, const char *error) {
    if (error != nullptr) {
      return;
    }
    auto *self = static_cast<H2oNativeServer *>(listener->data);
    h2o_socket_t *socket = h2o_evloop_socket_accept(listener);
    if (socket != nullptr) {
      h2o_accept(&self->accept_context_, socket);
    }
  }

  static int onClientHello(ptls_on_client_hello_t *, ptls_t *tls,
                           ptls_on_client_hello_parameters_t *parameters) {
    for (const ptls_iovec_t &supported : h2o_http3_alpn) {
      for (std::size_t index = 0;
           index < parameters->negotiated_protocols.count; ++index) {
        const ptls_iovec_t offered =
            parameters->negotiated_protocols.list[index];
        if (h2o_memis(supported.base, supported.len, offered.base,
                      offered.len)) {
          return ptls_set_negotiated_protocol(
              tls, reinterpret_cast<const char *>(supported.base),
              supported.len);
        }
      }
    }
    return PTLS_ALERT_NO_APPLICATION_PROTOCOL;
  }

  static h2o_quic_conn_t *onQuicAccept(h2o_quic_ctx_t *context,
                                       quicly_address_t *destination,
                                       quicly_address_t *source,
                                       quicly_decoded_packet_t *packet) {
    auto *server_context = reinterpret_cast<h2o_http3_server_ctx_t *>(context);
    h2o_http3_conn_t *connection = h2o_http3_server_accept(
        server_context, destination, source, packet, nullptr, &h3_callbacks_);
    if (connection == &h2o_http3_accept_conn_closed) {
      return nullptr;
    }
    return connection == nullptr ? nullptr : &connection->super;
  }

  static quicly_error_t
  generateResumptionToken(quicly_generate_resumption_token_t *generator,
                          quicly_conn_t *, ptls_buffer_t *buffer,
                          quicly_address_token_plaintext_t *token) {
    auto *self = reinterpret_cast<ResumptionTokenGenerator *>(generator);
    return quicly_encrypt_address_token(ptls_openssl_random_bytes, self->aead,
                                        buffer, buffer->off, token);
  }

  bool initializeTcp(const ServerConfig &options, std::string *error) {
    ssl_context_ = SSL_CTX_new(TLS_server_method());
    if (ssl_context_ == nullptr ||
        SSL_CTX_set_min_proto_version(ssl_context_, TLS1_3_VERSION) != 1 ||
        SSL_CTX_use_certificate_chain_file(
            ssl_context_, options.certificate_file.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(ssl_context_, options.key_file.c_str(),
                                    SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ssl_context_) != 1) {
      *error = "failed to configure TCP TLS context";
      return false;
    }
    h2o_ssl_register_alpn_protocols(ssl_context_, h2o_http2_alpn_protocols);
    accept_context_.ssl_ctx = ssl_context_;

    const int fd = createBoundSocket(options, SOCK_STREAM, error);
    if (fd < 0) {
      return false;
    }
    if (listen(fd, H2O_SOMAXCONN) != 0) {
      *error = std::strerror(errno);
      close(fd);
      return false;
    }
    tcp_listener_ =
        h2o_evloop_socket_create(loop_, fd, H2O_SOCKET_FLAG_DONT_READ);
    if (tcp_listener_ == nullptr) {
      close(fd);
      *error = "h2o_evloop_socket_create failed for TCP";
      return false;
    }
    tcp_listener_->data = this;
    h2o_socket_read_start(tcp_listener_, onTcpAccept);
    return true;
  }

  bool initializeQuic(const ServerConfig &options, std::string *error) {
    tls_context_.random_bytes = ptls_openssl_random_bytes;
    tls_context_.get_time = &ptls_get_time;
    tls_context_.key_exchanges = ptls_openssl_key_exchanges;
    tls_context_.cipher_suites = ptls_openssl_cipher_suites;
    client_hello_.cb = onClientHello;
    tls_context_.on_client_hello = &client_hello_;
    if (ptls_load_certificates(&tls_context_,
                               options.certificate_file.c_str()) != 0) {
      *error = "failed to load QUIC TLS certificate";
      return false;
    }
    FILE *key_file = std::fopen(options.key_file.c_str(), "r");
    if (key_file == nullptr) {
      *error = "failed to open QUIC TLS key";
      return false;
    }
    EVP_PKEY *private_key =
        PEM_read_PrivateKey(key_file, nullptr, nullptr, nullptr);
    std::fclose(key_file);
    if (private_key == nullptr ||
        ptls_openssl_init_sign_certificate(&signer_, private_key) != 0) {
      if (private_key != nullptr) {
        EVP_PKEY_free(private_key);
      }
      *error = "failed to load QUIC TLS private key";
      return false;
    }
    EVP_PKEY_free(private_key);
    signer_initialized_ = true;
    tls_context_.sign_certificate = &signer_.super;
    quicly_amend_ptls_context(&tls_context_);

    quic_context_ = quicly_spec_context;
    quic_context_.tls = &tls_context_;
    quic_context_.transport_params.max_streams_bidi = 1024;
    h2o_http3_server_amend_quicly_context(&config_, &quic_context_);
    std::uint8_t token_secret[PTLS_MAX_DIGEST_SIZE];
    tls_context_.random_bytes(token_secret, ptls_openssl_sha256.digest_size);
    token_generator_.super.cb = generateResumptionToken;
    token_generator_.aead = ptls_aead_new(
        &ptls_openssl_aes128gcm, &ptls_openssl_sha256, 1, token_secret, "");
    ptls_clear_memory(token_secret, sizeof(token_secret));
    if (token_generator_.aead == nullptr) {
      *error = "failed to create QUIC resumption token encryptor";
      return false;
    }
    quic_context_.generate_resumption_token = &token_generator_.super;

    std::uint8_t random_key[PTLS_SHA256_DIGEST_SIZE];
    tls_context_.random_bytes(random_key, sizeof(random_key));
    quic_context_.cid_encryptor = quicly_new_default_cid_encryptor(
        &ptls_openssl_quiclb, &ptls_openssl_aes128ecb, &ptls_openssl_sha256,
        ptls_iovec_init(random_key, sizeof(random_key)));
    ptls_clear_memory(random_key, sizeof(random_key));
    if (quic_context_.cid_encryptor == nullptr) {
      *error = "failed to create QUIC CID encryptor";
      return false;
    }

    const int fd = createBoundSocket(options, SOCK_DGRAM, error);
    if (fd < 0) {
      return false;
    }
    udp_listener_ =
        h2o_evloop_socket_create(loop_, fd, H2O_SOCKET_FLAG_DONT_READ);
    if (udp_listener_ == nullptr) {
      close(fd);
      *error = "h2o_evloop_socket_create failed for UDP";
      return false;
    }
    h2o_http3_server_init_context(&context_, &h3_server_context_.super, loop_,
                                  udp_listener_, nullptr, &quic_context_,
                                  &h3_next_cid_, onQuicAccept, nullptr, 1);
    h3_server_context_.accept_ctx = &accept_context_;
    h3_server_context_.send_retry = 0;
    h3_server_context_.qpack.encoder_table_capacity = 4096;
    quic_initialized_ = true;
    return true;
  }

  void dispose() {
    if (tcp_listener_ != nullptr) {
      h2o_socket_close(tcp_listener_);
      tcp_listener_ = nullptr;
    }
    if (quic_initialized_) {
      h2o_quic_close_all_connections(&h3_server_context_.super);
      while (h2o_quic_num_connections(&h3_server_context_.super) != 0) {
        h2o_evloop_run(loop_, 10);
      }
      h2o_quic_dispose_context(&h3_server_context_.super);
      quic_initialized_ = false;
      udp_listener_ = nullptr;
    } else if (udp_listener_ != nullptr) {
      h2o_socket_close(udp_listener_);
      udp_listener_ = nullptr;
    }
    if (context_initialized_) {
      h2o_context_dispose(&context_);
      context_initialized_ = false;
    }
    if (loop_ != nullptr) {
      h2o_evloop_destroy(loop_);
      loop_ = nullptr;
    }
    if (ssl_context_ != nullptr) {
      SSL_CTX_free(ssl_context_);
      ssl_context_ = nullptr;
    }
    if (quic_context_.cid_encryptor != nullptr) {
      quicly_free_default_cid_encryptor(quic_context_.cid_encryptor);
      quic_context_.cid_encryptor = nullptr;
    }
    if (token_generator_.aead != nullptr) {
      ptls_aead_free(token_generator_.aead);
      token_generator_.aead = nullptr;
    }
    if (signer_initialized_) {
      ptls_openssl_dispose_sign_certificate(&signer_);
      signer_initialized_ = false;
    }
    if (configured_) {
      h2o_config_dispose(&config_);
      configured_ = false;
    }
  }

  static h2o_http3_conn_callbacks_t h3_callbacks_;

  h2o_globalconf_t config_{};
  h2o_context_t context_{};
  h2o_accept_ctx_t accept_context_{};
  h2o_loop_t *loop_ = nullptr;
  h2o_socket_t *tcp_listener_ = nullptr;
  h2o_socket_t *udp_listener_ = nullptr;
  SSL_CTX *ssl_context_ = nullptr;
  ptls_context_t tls_context_{};
  ptls_on_client_hello_t client_hello_{};
  ptls_openssl_sign_certificate_t signer_{};
  quicly_context_t quic_context_{};
  ResumptionTokenGenerator token_generator_{};
  quicly_cid_plaintext_t h3_next_cid_{};
  h2o_http3_server_ctx_t h3_server_context_{};
  bool configured_ = false;
  bool context_initialized_ = false;
  bool signer_initialized_ = false;
  bool quic_initialized_ = false;
};

h2o_http3_conn_callbacks_t H2oNativeServer::h3_callbacks_ =
    H2O_HTTP3_CONN_CALLBACKS;

} // namespace

int main(int argc, char **argv) {
  ServerConfig config;
  std::string error;
  if (!parseArgs(argc, argv, &config, &error)) {
    std::cerr << "error: " << error << '\n';
    return 1;
  }
  stop_requested.store(false, std::memory_order_release);
  std::signal(SIGINT, requestStop);
  std::signal(SIGTERM, requestStop);

  H2oNativeServer server;
  if (!server.initialize(config, &error)) {
    std::cerr << "error: " << error << '\n';
    return 1;
  }
  std::cout << "{\"schema_version\":1,\"mode\":\"server\","
               "\"backend\":\"h2o_native_rpc\",\"port\":"
            << config.port
            << ",\"tls\":true,\"protocols\":[\"h1\",\"h2\","
               "\"h3\"]}\n"
            << std::flush;
  server.run();
  return 0;
}
