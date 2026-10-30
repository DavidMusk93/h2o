#include "benchmark.h"
#include "response_contract.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <h2o.h>
#include <h2o/hostinfo.h>
#include <h2o/httpclient.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <picotls/openssl.h>
#include <quicly.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef AUTO_BENCH_H2O_NATIVE_RUNTIME
#define AUTO_BENCH_H2O_NATIVE_RUNTIME "h2o-native-unknown"
#endif

namespace flowkit::auto_bench::rpc {
namespace {

using Clock = std::chrono::steady_clock;

enum class Protocol { H1, H2, H3 };

struct ClientConfig {
  Protocol protocol = Protocol::H2;
  std::string protocol_name = "h2";
  std::string host;
  std::uint16_t port = 0;
  std::uint64_t operations = 0;
  std::size_t payload_bytes = 256;
  std::size_t response_bytes = 0;
  std::string response_content = "periodic";
  std::size_t concurrency = 1;
  std::size_t socket_pool_capacity = 1;
  std::uint64_t debug_operations = 10;
  std::string ready_file;
  std::string start_file;
  std::string measurement_stop_file;
  std::string ca_file;
  std::string server_name = "rpc-bench.local";
  std::uint64_t warmup_seconds = 0;
  std::string warmup_mode = "serial-seed";
  std::string request_storage = "reuse";
};

struct BatchResult {
  std::uint64_t errors = 0;
  std::string first_error;
  std::vector<std::uint64_t> latency_ns;
  std::size_t observed_connections = 0;
};

std::uint64_t hostToNetwork64(std::uint64_t value) {
  const std::uint32_t high = htonl(static_cast<std::uint32_t>(value >> 32));
  const std::uint32_t low = htonl(static_cast<std::uint32_t>(value));
  return (static_cast<std::uint64_t>(low) << 32) | high;
}

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

bool parseArgs(int argc, char **argv, ClientConfig *config,
               std::string *error) {
  for (int index = 1; index < argc; ++index) {
    const std::string option = argv[index];
    if (option == "--output") {
      if (++index >= argc || std::string(argv[index]) != "json") {
        *error = "--output requires json";
        return false;
      }
      continue;
    }
    if (option == "--protocol" || option == "--host" ||
        option == "--tls-ca-file" || option == "--tls-server-name" ||
        option == "--warmup-mode" || option == "--request-storage" ||
        option == "--response-content" ||
        option == "--ready-file" || option == "--start-file" ||
        option == "--measurement-stop-file") {
      if (++index >= argc || *argv[index] == '\0') {
        *error = option + " requires a non-empty value";
        return false;
      }
      const std::string value = argv[index];
      if (option == "--response-content") {
        if (value != "periodic" && value != "entropy") {
          *error = "--response-content requires periodic or entropy";
          return false;
        }
        config->response_content = value;
        continue;
      }
      if (option == "--protocol") {
        config->protocol_name = value;
        if (value == "h1") {
          config->protocol = Protocol::H1;
        } else if (value == "h2") {
          config->protocol = Protocol::H2;
        } else if (value == "h3") {
          config->protocol = Protocol::H3;
        } else {
          *error = "--protocol requires h1, h2, or h3";
          return false;
        }
      } else if (option == "--host") {
        config->host = value;
      } else if (option == "--ready-file") {
        config->ready_file = value;
      } else if (option == "--start-file") {
        config->start_file = value;
      } else if (option == "--tls-ca-file") {
        config->ca_file = value;
      } else if (option == "--tls-server-name") {
        config->server_name = value;
      } else if (option == "--warmup-mode") {
        if (value != "serial-seed" && value != "concurrent") {
          *error = "--warmup-mode requires serial-seed or concurrent";
          return false;
        }
        config->warmup_mode = value;
      } else if (option == "--request-storage") {
        if (value != "fresh" && value != "reuse") {
          *error = "--request-storage requires fresh or reuse";
          return false;
        }
        config->request_storage = value;
      } else {
        config->measurement_stop_file = value;
      }
      continue;
    }
    if (option == "--help") {
      std::cout
          << "h2o-native-rpc-client --protocol h1|h2|h3 --host ADDRESS "
             "--port N --operations N --payload-bytes N --concurrency N "
             "--socket-pool-capacity N --debug-operations N [--ready-file PATH "
             "--start-file PATH --measurement-stop-file PATH] "
             "--tls-ca-file PATH [--tls-server-name NAME --warmup-seconds N "
             "--warmup-mode serial-seed|concurrent --request-storage fresh|reuse] --output json\n";
      std::exit(0);
    }
    if (++index >= argc) {
      *error = option + " requires an unsigned integer";
      return false;
    }
    std::uint64_t value = 0;
    if (!parseUnsigned(argv[index], &value)) {
      *error = option + " requires an unsigned integer";
      return false;
    }
    if (option == "--port") {
      if (value > std::numeric_limits<std::uint16_t>::max()) {
        *error = "--port exceeds uint16 range";
        return false;
      }
      config->port = static_cast<std::uint16_t>(value);
    } else if (option == "--operations") {
      config->operations = value;
    } else if (option == "--payload-bytes") {
      config->payload_bytes = value;
    } else if (option == "--response-bytes") {
      if (value == 0 || value > 65536) {
        *error = "--response-bytes requires 1..65536";
        return false;
      }
      config->response_bytes = value;
    } else if (option == "--concurrency") {
      config->concurrency = value;
    } else if (option == "--socket-pool-capacity") {
      config->socket_pool_capacity = value;
    } else if (option == "--debug-operations") {
      config->debug_operations = value;
    } else if (option == "--warmup-seconds") {
      config->warmup_seconds = value;
    } else {
      *error = "unknown option: " + option;
      return false;
    }
  }
  if (config->response_bytes == 0 && config->response_content != "periodic") {
    *error = "--response-content requires --response-bytes";
    return false;
  }
  if (config->host.empty() || config->ca_file.empty() ||
      config->port == 0 || config->operations == 0 ||
      config->payload_bytes == 0 || config->payload_bytes > 1024 * 1024 ||
      config->concurrency == 0 || config->concurrency > config->operations ||
      config->socket_pool_capacity == 0 || config->debug_operations == 0) {
    *error =
        "require host, CA file, port, positive operations/payload/concurrency/debug "
        "values, payload <= 1 MiB, concurrency <= operations, and positive "
        "socket pool capacity";
    return false;
  }
  if (config->ready_file.empty() != config->start_file.empty()) {
    *error = "--ready-file and --start-file must be used together";
    return false;
  }
  return true;
}

bool createMarker(const std::string &path, std::string *error) {
  if (path.empty()) {
    return true;
  }
  std::ofstream marker(path, std::ios::out | std::ios::trunc);
  if (!marker) {
    *error = "cannot create marker file: " + path;
    return false;
  }
  marker << "ready\n";
  return true;
}

bool waitForStart(const ClientConfig &config, std::string *error) {
  if (config.ready_file.empty()) {
    return true;
  }
  if (!createMarker(config.ready_file, error)) {
    return false;
  }
  const auto deadline = Clock::now() + std::chrono::seconds(120);
  while (Clock::now() < deadline) {
    if (std::ifstream(config.start_file).good()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  *error = "timed out waiting for start file: " + config.start_file;
  return false;
}

std::string makePayload(std::size_t size) {
  std::string payload(size, '\0');
  for (std::size_t index = 0; index < size; ++index) {
    payload[index] = static_cast<char>((index * 31 + 17) % 251 + 1);
  }
  return payload;
}

std::string sha256(const std::string &value) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char *>(value.data()), value.size(),
         digest);
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned char byte : digest) {
    output << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return output.str();
}

std::string resultDigest(std::uint64_t operations, const std::string &payload,
                          const ClientConfig &config) {
  std::string contract = "operations=" + std::to_string(operations) +
                         ";payload_bytes=" + std::to_string(payload.size()) +
                         ";payload_sha256=" + sha256(payload);
  if (config.response_bytes != 0) {
    contract += ";response_contract=response-v1;response_bytes=" +
                std::to_string(config.response_bytes) + ";response_content=" +
                config.response_content + ";response_base_sha256=" +
                sha256(responseBase(config.response_bytes,
                                     config.response_content));
  }
  return sha256(contract);
}

double percentileMs(const std::vector<std::uint64_t> &sorted_ns,
                    double percentile) {
  if (sorted_ns.empty()) {
    return 0.0;
  }
  const double offset = percentile * static_cast<double>(sorted_ns.size() - 1);
  return static_cast<double>(sorted_ns[static_cast<std::size_t>(offset)]) /
         1'000'000.0;
}

std::string jsonEscape(const std::string &value) {
  std::ostringstream output;
  for (unsigned char character : value) {
    switch (character) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (character < 0x20) {
        output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
               << static_cast<unsigned int>(character) << std::dec;
      } else {
        output << static_cast<char>(character);
      }
    }
  }
  return output.str();
}

const char *protocolVersionName(Protocol protocol) {
  switch (protocol) {
  case Protocol::H1:
    return "HTTP/1.1";
  case Protocol::H2:
    return "HTTP/2";
  case Protocol::H3:
    return "HTTP/3";
  }
  return "unknown";
}

int protocolVersion(Protocol protocol) {
  switch (protocol) {
  case Protocol::H1:
    return 0x101;
  case Protocol::H2:
    return 0x200;
  case Protocol::H3:
    return 0x300;
  }
  return 0;
}

class H2oNativeClient {
public:
  explicit H2oNativeClient(const ClientConfig &config)
      : config_(config),
        response_base_(responseBase(config.response_bytes,
                                    config.response_content)) {}

  ~H2oNativeClient() { dispose(); }

  bool initialize(std::string *error) {
    SSL_load_error_strings();
    SSL_library_init();
    OpenSSL_add_all_algorithms();

    loop_ = h2o_evloop_create();
    if (loop_ == nullptr) {
      *error = "h2o_evloop_create failed";
      return false;
    }
    queue_ = h2o_multithread_create_queue(loop_);
    if (queue_ == nullptr) {
      *error = "h2o_multithread_create_queue failed";
      return false;
    }
    h2o_multithread_register_receiver(queue_, &getaddr_receiver_,
                                      h2o_hostinfo_getaddr_receiver);
    getaddr_registered_ = true;

    client_context_.loop = loop_;
    client_context_.getaddr_receiver = &getaddr_receiver_;
    client_context_.io_timeout = 5'000;
    client_context_.connect_timeout = 5'000;
    client_context_.first_byte_timeout = 5'000;
    client_context_.keepalive_timeout = 120'000;
    client_context_.max_buffer_size = 128 * 1024;
    client_context_.http2.max_concurrent_streams = 1024;
    client_context_.protocol_selector.ratio.http2 =
        config_.protocol == Protocol::H2 ? 100 : 0;
    client_context_.protocol_selector.ratio.http3 =
        config_.protocol == Protocol::H3 ? 100 : 0;

    if (config_.protocol == Protocol::H3 && !initializeHttp3(error)) {
      return false;
    }

    h2o_mem_init_pool(&origin_pool_);
    origin_pool_initialized_ = true;
    const bool ipv6 = config_.host.find(':') != std::string::npos;
    url_ = "https://" + (ipv6 ? "[" + config_.host + "]" : config_.host) +
           ":" + std::to_string(config_.port) + "/rpc";
    if (h2o_url_parse(&origin_pool_, url_.c_str(), url_.size(), &origin_) !=
        0) {
      *error = "failed to parse H2O RPC target URL";
      return false;
    }

    sockaddr_storage tcp_address{};
    socklen_t tcp_address_size = 0;
    if (config_.protocol != Protocol::H3) {
      void *ip;
      if (ipv6) {
        auto *v6 = reinterpret_cast<sockaddr_in6 *>(&tcp_address);
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(config_.port);
        ip = &v6->sin6_addr;
        tcp_address_size = sizeof(*v6);
      } else {
        auto *v4 = reinterpret_cast<sockaddr_in *>(&tcp_address);
        v4->sin_family = AF_INET;
        v4->sin_port = htons(config_.port);
        ip = &v4->sin_addr;
        tcp_address_size = sizeof(*v4);
      }
      if (inet_pton(ipv6 ? AF_INET6 : AF_INET, config_.host.c_str(), ip) != 1) {
        *error = "H1/H2 requires a numeric --host";
        return false;
      }
    }
    h2o_socketpool_target_t *target =
        h2o_socketpool_create_target(&origin_, nullptr);
    if (config_.protocol != Protocol::H3) {
      // The socket address remains numeric; the URL host supplies SNI and
      // H2O's DNS SAN verifier, which does not handle IP SAN entries.
      if (target->type == H2O_SOCKETPOOL_TYPE_NAMED) {
        free(target->peer.named_serv.base);
      }
      target->type = H2O_SOCKETPOOL_TYPE_SOCKADDR;
      target->peer.sockaddr.bytes = tcp_address;
      target->peer.sockaddr.len = tcp_address_size;
      free(target->url.host.base);
      target->url.host = h2o_strdup(nullptr, config_.server_name.c_str(), SIZE_MAX);
    }
    h2o_socketpool_init_specific(&socket_pool_, config_.socket_pool_capacity,
                                 &target, 1, nullptr);
    socket_pool_initialized_ = true;
    h2o_socketpool_set_timeout(&socket_pool_, 120'000);
    h2o_socketpool_register_loop(&socket_pool_, loop_);
    h2o_httpclient_connection_pool_init(&connection_pool_, &socket_pool_);

    SSL_CTX *ssl_context = SSL_CTX_new(TLS_client_method());
    if (ssl_context == nullptr) {
      *error = "SSL_CTX_new failed";
      return false;
    }
    if (SSL_CTX_set_min_proto_version(ssl_context, TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(ssl_context, TLS1_3_VERSION) != 1 ||
        SSL_CTX_load_verify_locations(ssl_context, config_.ca_file.c_str(),
                                      nullptr) != 1) {
      SSL_CTX_free(ssl_context);
      *error = "failed to load TLS CA or require TLS 1.3";
      return false;
    }
    SSL_CTX_set_verify(ssl_context, SSL_VERIFY_PEER, nullptr);
    h2o_socketpool_set_ssl_ctx(&socket_pool_, ssl_context);
    SSL_CTX_free(ssl_context);
    return true;
  }

  BatchResult run(std::uint64_t operations, std::size_t concurrency,
                  const std::string &payload, std::uint64_t first_sequence = 0,
                  bool serialize_connections = false) {
    result_ = BatchResult{};
    result_.latency_ns.reserve(operations);
    payload_ = &payload;
    total_ = operations;
    concurrency_ = std::min<std::uint64_t>(concurrency, operations);
    if (config_.request_storage == "reuse") {
      while (requests_.size() < concurrency_) {
        auto request = std::make_unique<Request>();
        idle_requests_.push_back(request.get());
        requests_.push_back(std::move(request));
      }
    }
    first_sequence_ = first_sequence;
    started_ = 0;
    completed_ = 0;
    active_ = 0;
    serialize_connections_ =
        serialize_connections && config_.protocol == Protocol::H2;
    connects_pending_ = 0;
    observed_sockets_.clear();

    while (completed_ != total_) {
      while (active_ < concurrency_ && started_ < total_ &&
             (!serialize_connections_ || connects_pending_ == 0)) {
        ++started_;
        ++active_;
        startRequest(first_sequence_ + started_ - 1);
      }
      h2o_evloop_run(loop_, 10);
    }
    std::sort(result_.latency_ns.begin(), result_.latency_ns.end());
    result_.observed_connections = config_.protocol == Protocol::H3
                                       ? countHttp3Connections()
                                       : observed_sockets_.size();
    return result_;
  }

private:
  struct Request {
    H2oNativeClient *owner;
    h2o_mem_pool_t pool;
    h2o_url_t target{};
    std::string body;
    std::string expected;
    std::string response;
    Clock::time_point started_at;
    bool status_valid = false;
    bool content_type_valid = false;
    bool awaiting_connection = false;

    const std::string &expectedFrame() const {
      return expected.empty() ? body : expected;
    }
  };

  static h2o_httpclient_head_cb
  onConnect(h2o_httpclient_t *client, const char *error, h2o_iovec_t *method,
            h2o_url_t *url, const h2o_header_t **headers,
            std::size_t *num_headers, h2o_iovec_t *body,
            h2o_httpclient_proceed_req_cb *proceed_request,
            h2o_httpclient_properties_t *, h2o_url_t *) {
    auto *request = static_cast<Request *>(client->data);
    if (request->owner->launching_request_ == request) {
      // Reusing an established H2 connection invokes this callback inline.
      request->owner->releaseConnection(request);
    }
    if (error != nullptr) {
      request->owner->finishRequest(request, error);
      return nullptr;
    }

    *method = h2o_iovec_init(H2O_STRLIT("POST"));
    *url = request->target;
    h2o_headers_t request_headers{};
    h2o_add_header(&request->pool, &request_headers, H2O_TOKEN_CONTENT_TYPE,
                   nullptr, H2O_STRLIT("application/octet-stream"));
    char *length = static_cast<char *>(h2o_mem_alloc_pool(
        &request->pool, char, sizeof(H2O_UINT64_LONGEST_STR)));
    const int length_size = std::snprintf(
        length, sizeof(H2O_UINT64_LONGEST_STR), "%zu", request->body.size());
    h2o_add_header(&request->pool, &request_headers, H2O_TOKEN_CONTENT_LENGTH,
                   nullptr, length, static_cast<std::size_t>(length_size));
    *headers = request_headers.entries;
    *num_headers = request_headers.size;
    *body = h2o_iovec_init(request->body.data(), request->body.size());
    *proceed_request = nullptr;
    return onHead;
  }

  static h2o_httpclient_body_cb onHead(h2o_httpclient_t *client,
                                       const char *error,
                                       h2o_httpclient_on_head_t *response) {
    auto *request = static_cast<Request *>(client->data);
    // A new connection becomes reusable after its first response, when peer
    // SETTINGS have been consumed. Do not fan out on its provisional limit.
    request->owner->releaseConnection(request);
    if (error != nullptr) {
      request->owner->finishRequest(request, error);
      return nullptr;
    }
    request->status_valid =
        response->status == 200 &&
        response->version == protocolVersion(request->owner->config_.protocol);
    h2o_httpclient_conn_properties_t properties{};
    client->get_conn_properties(client, &properties);
    if (properties.sock != nullptr) {
      request->owner->observed_sockets_.insert(properties.sock);
    }
    for (std::size_t index = 0; index < response->num_headers; ++index) {
      const h2o_header_t &header = response->headers[index];
      if (header.name == &H2O_TOKEN_CONTENT_TYPE->buf &&
          h2o_memis(header.value.base, header.value.len,
                    H2O_STRLIT("application/octet-stream"))) {
        request->content_type_valid = true;
        break;
      }
    }
    return onBody;
  }

  static int onBody(h2o_httpclient_t *client, const char *error, h2o_header_t *,
                    std::size_t) {
    auto *request = static_cast<Request *>(client->data);
    if (client->buf != nullptr && *client->buf != nullptr &&
        (*client->buf)->size != 0) {
      if ((*client->buf)->size >
          request->expectedFrame().size() - request->response.size()) {
        request->owner->finishRequest(request, "response exceeds expected frame");
        return -1;
      }
      request->response.append((*client->buf)->bytes, (*client->buf)->size);
      h2o_buffer_consume(client->buf, (*client->buf)->size);
    }
    if (error == h2o_httpclient_error_is_eos) {
      if (!request->status_valid) {
        request->owner->finishRequest(
            request, "response status or protocol version mismatch");
      } else if (!request->content_type_valid) {
        request->owner->finishRequest(request,
                                      "response content-type mismatch");
      } else if (request->response != request->expectedFrame()) {
        request->owner->finishRequest(request, "response frame mismatch");
      } else {
        request->owner->finishRequest(request, nullptr);
      }
      return 0;
    }
    if (error != nullptr) {
      request->owner->finishRequest(request, error);
      return -1;
    }
    if (client->update_window != nullptr) {
      client->update_window(client);
    }
    return 0;
  }

  bool initializeHttp3(std::string *error) {
    h3_context_.tls.random_bytes = ptls_openssl_random_bytes;
    h3_context_.tls.get_time = &ptls_get_time;
    h3_context_.tls.key_exchanges = ptls_openssl_key_exchanges;
    h3_context_.tls.cipher_suites = ptls_openssl_cipher_suites;
    X509_STORE *store = X509_STORE_new();
    if (store == nullptr ||
        X509_STORE_load_locations(store, config_.ca_file.c_str(), nullptr) != 1 ||
        ptls_openssl_init_verify_certificate(&h3_context_.verify_cert, store) != 0) {
      if (store != nullptr) {
        X509_STORE_free(store);
      }
      *error = "failed to initialize H3 certificate verifier";
      return false;
    }
    X509_STORE_free(store);
    h3_verifier_initialized_ = true;
    h3_context_.tls.verify_certificate = &h3_context_.verify_cert.super;
    quicly_amend_ptls_context(&h3_context_.tls);

    h3_context_.quic = quicly_spec_context;
    h3_context_.quic.transport_params.max_streams_uni = 10;
    h3_context_.quic.transport_params.max_stream_data.uni =
        h2o_http3_calc_min_flow_control_size(H2O_MAX_REQLEN);
    h3_context_.quic.transport_params.max_datagram_frame_size = 1500;
    h3_context_.quic.receive_datagram_frame =
        &h2o_httpclient_http3_on_receive_datagram_frame;
    h3_context_.quic.tls = &h3_context_.tls;
    h3_context_.quic.stream_open = &h2o_httpclient_http3_on_stream_open;
    h3_context_.qpack.encoder_table_capacity = 4096;
    h3_context_.max_frame_payload_size = 16384;

    std::uint8_t random_key[PTLS_SHA256_DIGEST_SIZE];
    h3_context_.tls.random_bytes(random_key, sizeof(random_key));
    h3_context_.quic.cid_encryptor = quicly_new_default_cid_encryptor(
        &ptls_openssl_quiclb, &ptls_openssl_aes128ecb, &ptls_openssl_sha256,
        ptls_iovec_init(random_key, sizeof(random_key)));
    ptls_clear_memory(random_key, sizeof(random_key));
    if (h3_context_.quic.cid_encryptor == nullptr) {
      *error = "failed to create H3 CID encryptor";
      return false;
    }

    h2o_socket_t *socket = h2o_quic_create_client_socket(
        loop_, config_.host.find(':') == std::string::npos ? AF_INET : AF_INET6);
    if (socket == nullptr) {
      *error = "failed to create H3 client UDP socket";
      return false;
    }
    h2o_quic_init_context(&h3_context_.h3, loop_, socket, nullptr,
                          &h3_context_.quic, &h3_next_cid_, nullptr,
                          h2o_httpclient_http3_notify_connection_update, 1,
                          nullptr);
    h3_initialized_ = true;
    client_context_.http3 = &h3_context_;
    return true;
  }

  void startRequest(std::uint64_t sequence) {
    Request *request;
    if (config_.request_storage == "reuse") {
      request = idle_requests_.back();
      idle_requests_.pop_back();
    } else {
      request = new Request;
    }
    request->owner = this;
    request->started_at = Clock::now();
    request->status_valid = false;
    request->content_type_valid = false;
    request->awaiting_connection = false;
    request->response.clear();
    h2o_mem_init_pool(&request->pool);
    request->body.resize(sizeof(sequence) + payload_->size());
    const std::uint64_t network_sequence = hostToNetwork64(sequence);
    std::memcpy(request->body.data(), &network_sequence,
                sizeof(network_sequence));
    std::memcpy(request->body.data() + sizeof(network_sequence),
                payload_->data(), payload_->size());
    if (config_.response_bytes == 0) {
      request->expected.clear();
    } else {
      request->expected.resize(sizeof(sequence) + config_.response_bytes);
      std::memcpy(request->expected.data(), &network_sequence, sizeof(sequence));
      std::memcpy(request->expected.data() + sizeof(sequence),
                  response_base_.data(), response_base_.size());
      mixResponseSequence(&request->expected, sequence, sizeof(sequence));
    }
    if (config_.request_storage == "reuse") {
      // Origin strings live until client disposal; the URL is immutable.
      // Each request still owns its headers and H2O allocation pool.
      request->target = origin_;
      request->response.reserve(request->expectedFrame().size());
    } else {
      if (h2o_url_parse(&request->pool, url_.c_str(), url_.size(),
                        &request->target) != 0) {
        finishRequest(request, "failed to parse request URL");
        return;
      }
    }
    if (serialize_connections_) {
      request->awaiting_connection = true;
      ++connects_pending_;
      launching_request_ = request;
    }
    h2o_httpclient_connect(nullptr, &request->pool, request, &client_context_,
                           &connection_pool_, &request->target, nullptr,
                           onConnect);
    launching_request_ = nullptr;
  }

  std::size_t countHttp3Connections() const {
    std::size_t count = 0;
    const h2o_linklist_t *anchor = &connection_pool_.http3.conns;
    for (const h2o_linklist_t *link = anchor->next; link != anchor;
         link = link->next) {
      ++count;
    }
    return count;
  }

  void releaseConnection(Request *request) {
    if (request->awaiting_connection) {
      request->awaiting_connection = false;
      --connects_pending_;
    }
  }

  void finishRequest(Request *request, const char *error) {
    releaseConnection(request);
    result_.latency_ns.push_back(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - request->started_at)
            .count());
    if (error != nullptr) {
      ++result_.errors;
      if (result_.first_error.empty()) {
        result_.first_error = error;
      }
    }
    --active_;
    ++completed_;
    h2o_mem_clear_pool(&request->pool);
    if (config_.request_storage == "reuse") {
      idle_requests_.push_back(request);
    } else {
      delete request;
    }
  }

  void dispose() {
    if (h3_initialized_) {
      h2o_quic_close_all_connections(&h3_context_.h3);
      while (h2o_quic_num_connections(&h3_context_.h3) != 0) {
        h2o_evloop_run(loop_, 10);
      }
      h2o_quic_dispose_context(&h3_context_.h3);
      h3_initialized_ = false;
    }
    if (h3_context_.quic.cid_encryptor != nullptr) {
      quicly_free_default_cid_encryptor(h3_context_.quic.cid_encryptor);
      h3_context_.quic.cid_encryptor = nullptr;
    }
    if (h3_verifier_initialized_) {
      ptls_openssl_dispose_verify_certificate(&h3_context_.verify_cert);
      h3_verifier_initialized_ = false;
    }
    if (socket_pool_initialized_) {
      h2o_socketpool_dispose(&socket_pool_);
      socket_pool_initialized_ = false;
    }
    if (getaddr_registered_) {
      h2o_multithread_unregister_receiver(queue_, &getaddr_receiver_);
      getaddr_registered_ = false;
    }
    if (queue_ != nullptr) {
      h2o_multithread_destroy_queue(queue_);
      queue_ = nullptr;
    }
    if (origin_pool_initialized_) {
      h2o_mem_clear_pool(&origin_pool_);
      origin_pool_initialized_ = false;
    }
    if (loop_ != nullptr) {
      h2o_evloop_destroy(loop_);
      loop_ = nullptr;
    }
  }

  ClientConfig config_;
  const std::string response_base_;
  h2o_loop_t *loop_ = nullptr;
  h2o_multithread_queue_t *queue_ = nullptr;
  h2o_multithread_receiver_t getaddr_receiver_{};
  h2o_httpclient_ctx_t client_context_{};
  h2o_socketpool_t socket_pool_{};
  h2o_httpclient_connection_pool_t connection_pool_{};
  h2o_mem_pool_t origin_pool_{};
  h2o_url_t origin_{};
  h2o_http3client_ctx_t h3_context_{};
  quicly_cid_plaintext_t h3_next_cid_{};
  std::string url_;
  std::vector<std::unique_ptr<Request>> requests_;
  std::vector<Request *> idle_requests_;
  const std::string *payload_ = nullptr;
  BatchResult result_;
  std::uint64_t total_ = 0;
  std::uint64_t first_sequence_ = 0;
  std::uint64_t started_ = 0;
  std::uint64_t completed_ = 0;
  std::size_t concurrency_ = 0;
  std::size_t active_ = 0;
  bool serialize_connections_ = false;
  std::size_t connects_pending_ = 0;
  Request *launching_request_ = nullptr;
  std::set<const h2o_socket_t *> observed_sockets_;
  bool getaddr_registered_ = false;
  bool socket_pool_initialized_ = false;
  bool origin_pool_initialized_ = false;
  bool h3_initialized_ = false;
  bool h3_verifier_initialized_ = false;
};

} // namespace
} // namespace flowkit::auto_bench::rpc

int main(int argc, char **argv) {
  using namespace flowkit::auto_bench::rpc;
  ClientConfig config;
  std::string error;
  if (!parseArgs(argc, argv, &config, &error)) {
    std::cerr << "error: " << error << '\n';
    return 1;
  }

  H2oNativeClient client(config);
  if (!client.initialize(&error)) {
    std::cerr << "error: " << error << '\n';
    return 1;
  }
  const std::string payload = makePayload(config.payload_bytes);
  if (config.warmup_mode == "serial-seed") {
    // Establish H2 before fan-out; cold concurrent connects otherwise create
    // one TCP connection per lane instead of reusing the multiplexed session.
    const BatchResult seed =
        client.run(1, 1, payload, std::numeric_limits<std::uint64_t>::max());
    if (seed.errors != 0) {
      std::cerr << "error: H2O seed failed: " << seed.first_error << '\n';
      return 1;
    }
  }
  const auto warmup_deadline =
      Clock::now() + std::chrono::seconds(config.warmup_seconds);
  do {
    const BatchResult warmup = client.run(
        config.concurrency, config.concurrency, payload,
        std::numeric_limits<std::uint64_t>::max() - config.concurrency,
        config.warmup_mode == "serial-seed");
    if (warmup.errors != 0) {
      std::cerr << "error: H2O RPC warmup failed: " << warmup.first_error << '\n';
      return 1;
    }
  } while (Clock::now() < warmup_deadline);
  if (!waitForStart(config, &error)) {
    std::cerr << "error: " << error << '\n';
    return 1;
  }

  const std::uint64_t allocations_before = allocationCount();
  const auto benchmark_started = Clock::now();
  const BatchResult measured =
      client.run(config.operations, config.concurrency, payload);
  const auto benchmark_finished = Clock::now();
  if (!createMarker(config.measurement_stop_file, &error)) {
    std::cerr << "error: " << error << '\n';
    return 1;
  }
  const std::uint64_t allocations_after = allocationCount();
  if (allocations_after < allocations_before) {
    std::cerr << "error: allocation counter moved backwards\n";
    return 1;
  }

  const auto debug_started = Clock::now();
  const BatchResult debug =
      client.run(config.debug_operations, 1, payload, config.operations + 1);
  const auto debug_finished = Clock::now();
  const std::uint64_t errors = measured.errors + debug.errors;
  const std::string first_error =
      !measured.first_error.empty() ? measured.first_error : debug.first_error;
  const double main_elapsed_ms = std::chrono::duration<double, std::milli>(
                                     benchmark_finished - benchmark_started)
                                     .count();
  const double debug_task_ms =
      std::chrono::duration<double, std::milli>(debug_finished - debug_started)
          .count();

  std::cout << std::fixed << std::setprecision(6)
            << "{\"schema_version\":1,\"operations\":" << config.operations
            << ",\"errors\":" << errors << ",\"result_digest\":\""
            << resultDigest(config.operations, payload, config)
            << "\",\"latency_ms\":{\"p50\":"
            << percentileMs(measured.latency_ns, 0.50)
            << ",\"p95\":" << percentileMs(measured.latency_ns, 0.95)
            << ",\"p99\":" << percentileMs(measured.latency_ns, 0.99)
            << ",\"p999\":" << percentileMs(measured.latency_ns, 0.999)
            << "},\"allocations\":" << (allocations_after - allocations_before)
            << ",\"wire_bytes\":null,\"debug_task_ms\":" << debug_task_ms
            << ",\"metadata\":{\"backend\":\"h2o_native_rpc\",\"protocol\":\""
            << config.protocol_name << "\",\"negotiated_version\":\""
            << protocolVersionName(config.protocol)
            << "\",\"tls\":true,\"certificate_verification\":"
               "\"ca-and-san\",\"tls_server_name\":\""
            << jsonEscape(config.protocol == Protocol::H3 ? config.host
                                                          : config.server_name)
            << "\",\"warmup_seconds\":" << config.warmup_seconds
            << ",\"warmup_mode\":\"" << config.warmup_mode << "\""
            << ",\"response_bytes\":" << config.response_bytes
            << ",\"response_content\":\"" << config.response_content << "\""
            << ",\"warmup_connection_establishment\":\""
            << (config.protocol == Protocol::H2 &&
                        config.warmup_mode == "serial-seed"
                    ? "one-pending-until-first-head"
                    : "parallel")
            << "\""
            << ",\"connection_model\":"
               "\"h2o-native-shared-pool\",\"logical_concurrency\":"
            << config.concurrency
            << ",\"socket_pool_capacity\":" << config.socket_pool_capacity
            << ",\"allocation_counter\":\"glibc-malloc-family-call-count\","
               "\"wire_counter\":\"external-scoped-counter\","
               "\"main_elapsed_ms\":"
            << main_elapsed_ms << ",\"runtime\":\""
            << jsonEscape(AUTO_BENCH_H2O_NATIVE_RUNTIME)
            << "\",\"observed_connection_objects_lower_bound\":"
            << measured.observed_connections << ",\"first_error\":\""
            << jsonEscape(first_error) << "\"}}\n";
  return errors == 0 ? 0 : 1;
}
