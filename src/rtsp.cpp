/**
 * @file src/rtsp.cpp
 * @brief Definitions for RTSP streaming.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

extern "C" {
#include <moonlight-common-c/src/Limelight-internal.h>
#include <moonlight-common-c/src/Rtsp.h>
}

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef __linux__
  // platform includes
  #include <fcntl.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif

// lib includes
#include <boost/asio.hpp>
#include <boost/bind.hpp>

// local includes
#include "config.h"
#include "globals.h"
#include "handoff/hooks.h"
#include "input.h"
#include "logging.h"
#include "network.h"
#include "rtsp.h"
#include "stream.h"
#include "sync.h"
#include "video.h"

#ifdef __linux__
  #include "handoff/terminate.h"
#endif

namespace asio = boost::asio;

using asio::ip::tcp;
using asio::ip::udp;

using namespace std::literals;

namespace rtsp_stream {
  /**
   * @brief Release msg resources.
   *
   * @param msg Raw RTSP message buffer being parsed or encrypted.
   */
  void free_msg(PRTSP_MESSAGE msg) {
    freeMessage(msg);

    delete msg;
  }

#pragma pack(push, 1)

  /**
   * @brief Encrypted RTSP message header used by GameStream.
   */
  struct encrypted_rtsp_header_t {
    // We set the MSB in encrypted RTSP messages to allow format-agnostic
    // parsing code to be able to tell encrypted from plaintext messages.
    static constexpr std::uint32_t ENCRYPTED_MESSAGE_TYPE_BIT = 0x80000000;  ///< Protocol or platform constant for encrypted message type bit.

    /**
     * @brief Return a pointer to the protocol payload following the packet header.
     *
     * @return Parsed or serialized payload data.
     */
    uint8_t *payload() {
      return (uint8_t *) (this + 1);
    }

    /**
     * @brief Decode the RTSP interleaved-frame payload length.
     *
     * @return Payload byte count with the encrypted-message flag masked out.
     */
    std::uint32_t payload_length() {
      return util::endian::big<std::uint32_t>(typeAndLength) & ~ENCRYPTED_MESSAGE_TYPE_BIT;
    }

    /**
     * @brief Check whether encrypted.
     *
     * @return True when the RTSP connection is using TLS.
     */
    bool is_encrypted() {
      return !!(util::endian::big<std::uint32_t>(typeAndLength) & ENCRYPTED_MESSAGE_TYPE_BIT);
    }

    // This field is the length of the payload + ENCRYPTED_MESSAGE_TYPE_BIT in big-endian
    std::uint32_t typeAndLength;  ///< Type and length.

    // This field is the number used to initialize the bottom 4 bytes of the AES IV in big-endian
    std::uint32_t sequenceNumber;  ///< Sequence number.

    // This field is the AES GCM authentication tag
    std::uint8_t tag[16];  ///< Authentication tag appended to the encrypted payload.
  };

#pragma pack(pop)

  class rtsp_server_t;

  /**
   * @brief Owning pointer for an RTSP message allocated by moonlight-common-c.
   */
  using msg_t = util::safe_ptr<RTSP_MESSAGE, free_msg>;
  /**
   * @brief RTSP command handler signature used by the server dispatch table.
   */
  using cmd_func_t = std::function<void(rtsp_server_t *server, tcp::socket &, launch_session_t &, msg_t &&)>;

  /**
   * @brief Write msg details to the log.
   *
   * @param msg Raw RTSP message buffer being parsed or encrypted.
   */
  void print_msg(PRTSP_MESSAGE msg);
  /**
   * @brief Send the RTSP response used for unsupported commands.
   *
   * @param sock Socket used to read or write the protocol message.
   * @param session Active streaming or pairing session for the request.
   * @param req Parsed RTSP request being handled.
   */
  void cmd_not_found(tcp::socket &sock, launch_session_t &, msg_t &&req);
  /**
   * @brief Send the protocol response for the current request.
   *
   * @param sock Socket used to read or write the protocol message.
   * @param session Active streaming or pairing session for the request.
   * @param options Request options or socket options to apply.
   * @param statuscode RTSP status code to send in the response.
   * @param status_msg Status msg.
   * @param seqn RTSP CSeq value associated with the request.
   * @param payload Optional payload body to include in the response.
   */
  void respond(tcp::socket &sock, launch_session_t &session, POPTION_ITEM options, int statuscode, const char *status_msg, int seqn, const std::string_view &payload);

  /**
   * @brief RTSP client socket state and receive buffer parser.
   */
  class socket_t: public std::enable_shared_from_this<socket_t> {
  public:
    /**
     * @brief Construct an RTSP socket wrapper around an accepted TCP connection.
     *
     * @param io_context Io context.
     * @param handle_data_fn Handle data.
     */
    socket_t(boost::asio::io_context &io_context, std::function<void(tcp::socket &sock, launch_session_t &, msg_t &&)> &&handle_data_fn):
        handle_data_fn {std::move(handle_data_fn)},
        sock {io_context} {
    }

    /**
     * @brief Queue an asynchronous read to begin the next message.
     */
    void read() {
      if (begin == std::end(msg_buf) || (session->rtsp_cipher && begin + sizeof(encrypted_rtsp_header_t) >= std::end(msg_buf))) {
        BOOST_LOG(error) << "RTSP: read(): Exceeded maximum rtsp packet size: "sv << msg_buf.size();

        respond(sock, *session, nullptr, 400, "BAD REQUEST", 0, {});

        boost::system::error_code ec;
        sock.close(ec);

        return;
      }

      if (session->rtsp_cipher) {
        // For encrypted RTSP, we will read the the entire header first
        boost::asio::async_read(sock, boost::asio::buffer(begin, sizeof(encrypted_rtsp_header_t)), boost::bind(&socket_t::handle_read_encrypted_header, shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
      } else {
        sock.async_read_some(
          boost::asio::buffer(begin, (std::size_t) (std::end(msg_buf) - begin)),
          boost::bind(
            &socket_t::handle_read_plaintext,
            shared_from_this(),
            boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred
          )
        );
      }
    }

    /**
     * @brief Read the encrypted RTSP header before receiving the encrypted payload.
     * @param socket The socket the message was received on.
     * @param ec The error code of the read operation.
     * @param bytes The number of bytes read.
     */
    static void handle_read_encrypted_header(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      BOOST_LOG(debug) << "handle_read_encrypted_header(): Handle read of size: "sv << bytes << " bytes"sv;

      auto sock_close = util::fail_guard([&socket]() {
        boost::system::error_code ec;
        socket->sock.close(ec);

        if (ec) {
          BOOST_LOG(error) << "RTSP: handle_read_encrypted_header(): Couldn't close tcp socket: "sv << ec.message();
        }
      });

      if (ec || bytes < sizeof(encrypted_rtsp_header_t)) {
        BOOST_LOG(error) << "RTSP: handle_read_encrypted_header(): Couldn't read from tcp socket: "sv << ec.message();

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      auto header = (encrypted_rtsp_header_t *) socket->begin;
      if (!header->is_encrypted()) {
        BOOST_LOG(error) << "RTSP: handle_read_encrypted_header(): Rejecting unencrypted RTSP message"sv;

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      auto payload_length = header->payload_length();

      // Check if we have enough space to read this message
      if (socket->begin + sizeof(*header) + payload_length >= std::end(socket->msg_buf)) {
        BOOST_LOG(error) << "RTSP: handle_read_encrypted_header(): Exceeded maximum rtsp packet size: "sv << socket->msg_buf.size();

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      sock_close.disable();

      // Read the remainder of the header and full encrypted payload
      boost::asio::async_read(socket->sock, boost::asio::buffer(socket->begin + bytes, payload_length), boost::bind(&socket_t::handle_read_encrypted_message, socket->shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
    }

    /**
     * @brief Read and decrypt the encrypted RTSP payload.
     * @param socket The socket the message was received on.
     * @param ec The error code of the read operation.
     * @param bytes The number of bytes read.
     */
    static void handle_read_encrypted_message(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      BOOST_LOG(debug) << "handle_read_encrypted(): Handle read of size: "sv << bytes << " bytes"sv;

      auto sock_close = util::fail_guard([&socket]() {
        boost::system::error_code ec;
        socket->sock.close(ec);

        if (ec) {
          BOOST_LOG(error) << "RTSP: handle_read_encrypted_message(): Couldn't close tcp socket: "sv << ec.message();
        }
      });

      auto header = (encrypted_rtsp_header_t *) socket->begin;
      auto payload_length = header->payload_length();
      auto seq = util::endian::big<std::uint32_t>(header->sequenceNumber);

      if (ec || bytes < payload_length) {
        BOOST_LOG(error) << "RTSP: handle_read_encrypted(): Couldn't read from tcp socket: "sv << ec.message();

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
      // Section 8.2.1. The sequence number is our "invocation" field and the 'RC' in the
      // high bytes is the "fixed" field. Because each client provides their own unique
      // key, our values in the fixed field need only uniquely identify each independent
      // use of the client's key with AES-GCM in our code.
      //
      // The sequence number is 32 bits long which allows for 2^32 RTSP messages to be
      // received from each client before the IV repeats.
      crypto::aes_t iv(12);
      std::copy_n((uint8_t *) &seq, sizeof(seq), std::begin(iv));
      iv[10] = 'C';  // Client originated
      iv[11] = 'R';  // RTSP

      std::vector<uint8_t> plaintext;
      if (socket->session->rtsp_cipher->decrypt(std::string_view {(const char *) header->tag, sizeof(header->tag) + bytes}, plaintext, &iv)) {
        BOOST_LOG(error) << "Failed to verify RTSP message tag"sv;

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      msg_t req {new msg_t::element_type {}};
      if (auto status = parseRtspMessage(req.get(), (char *) plaintext.data(), (int) plaintext.size())) {
        BOOST_LOG(error) << "Malformed RTSP message: ["sv << status << ']';

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      sock_close.disable();

      print_msg(req.get());

      socket->handle_data(std::move(req));
    }

    /**
     * @brief Queue an asynchronous read of the payload portion of a plaintext message.
     */
    void read_plaintext_payload() {
      if (begin == std::end(msg_buf)) {
        BOOST_LOG(error) << "RTSP: read_plaintext_payload(): Exceeded maximum rtsp packet size: "sv << msg_buf.size();

        respond(sock, *session, nullptr, 400, "BAD REQUEST", 0, {});

        boost::system::error_code ec;
        sock.close(ec);

        return;
      }

      sock.async_read_some(
        boost::asio::buffer(begin, (std::size_t) (std::end(msg_buf) - begin)),
        boost::bind(
          &socket_t::handle_plaintext_payload,
          shared_from_this(),
          boost::asio::placeholders::error,
          boost::asio::placeholders::bytes_transferred
        )
      );
    }

    /**
     * @brief Read the plaintext RTSP payload after the header is parsed.
     * @param socket The socket the message was received on.
     * @param ec The error code of the read operation.
     * @param bytes The number of bytes read.
     */
    static void handle_plaintext_payload(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      BOOST_LOG(debug) << "handle_plaintext_payload(): Handle read of size: "sv << bytes << " bytes"sv;

      auto sock_close = util::fail_guard([&socket]() {
        boost::system::error_code ec;
        socket->sock.close(ec);

        if (ec) {
          BOOST_LOG(error) << "RTSP: handle_plaintext_payload(): Couldn't close tcp socket: "sv << ec.message();
        }
      });

      if (ec) {
        BOOST_LOG(error) << "RTSP: handle_plaintext_payload(): Couldn't read from tcp socket: "sv << ec.message();

        return;
      }

      auto end = socket->begin + bytes;
      msg_t req {new msg_t::element_type {}};
      if (auto status = parseRtspMessage(req.get(), socket->msg_buf.data(), (int) (end - socket->msg_buf.data()))) {
        BOOST_LOG(error) << "Malformed RTSP message: ["sv << status << ']';

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      sock_close.disable();

      auto fg = util::fail_guard([&socket]() {
        socket->read_plaintext_payload();
      });

      auto content_length = 0;
      for (auto option = req->options; option != nullptr; option = option->next) {
        if ("Content-length"sv == option->option) {
          BOOST_LOG(debug) << "Found Content-Length: "sv << option->content << " bytes"sv;

          // If content_length > bytes read, then we need to store current data read,
          // to be appended by the next read.
          std::string_view content {option->content};
          auto begin = std::find_if(std::begin(content), std::end(content), [](auto ch) {
            return (bool) std::isdigit(ch);
          });

          content_length = (int) util::from_chars(begin, std::end(content));
          break;
        }
      }

      if (end - socket->crlf >= content_length) {
        if (end - socket->crlf > content_length) {
          BOOST_LOG(warning) << "(end - socket->crlf) > content_length -- "sv << (std::size_t) (end - socket->crlf) << " > "sv << content_length;
        }

        fg.disable();
        print_msg(req.get());

        socket->handle_data(std::move(req));
      }

      socket->begin = end;
    }

    /**
     * @brief Read and parse the plaintext RTSP request header.
     * @param socket The socket the message was received on.
     * @param ec The error code of the read operation.
     * @param bytes The number of bytes read.
     */
    static void handle_read_plaintext(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      BOOST_LOG(debug) << "handle_read_plaintext(): Handle read of size: "sv << bytes << " bytes"sv;

      if (ec) {
        BOOST_LOG(error) << "RTSP: handle_read_plaintext(): Couldn't read from tcp socket: "sv << ec.message();

        boost::system::error_code ec;
        socket->sock.close(ec);

        if (ec) {
          BOOST_LOG(error) << "RTSP: handle_read_plaintext(): Couldn't close tcp socket: "sv << ec.message();
        }

        return;
      }

      auto fg = util::fail_guard([&socket]() {
        socket->read();
      });

      auto begin = std::max(socket->begin - 4, socket->begin);
      auto buf_size = bytes + (begin - socket->begin);
      auto end = begin + buf_size;

      constexpr auto needle = "\r\n\r\n"sv;

      auto it = std::search(begin, begin + buf_size, std::begin(needle), std::end(needle));
      if (it == end) {
        socket->begin = end;

        return;
      }

      // Emulate read completion for payload data
      socket->begin = it + needle.size();
      socket->crlf = socket->begin;
      buf_size = end - socket->begin;

      fg.disable();
      handle_plaintext_payload(socket, ec, buf_size);
    }

    /**
     * @brief Dispatch a parsed RTSP request to the socket's command handler.
     *
     * @param req Parsed RTSP request being handled.
     */
    void handle_data(msg_t &&req) {
      handle_data_fn(sock, *session, std::move(req));
    }

    std::function<void(tcp::socket &sock, launch_session_t &, msg_t &&)> handle_data_fn;  ///< Command dispatcher installed by the RTSP server.

    tcp::socket sock;  ///< TCP socket connected to the RTSP client.

    std::array<char, 2048> msg_buf;  ///< Receive buffer for one RTSP request header and payload.

    char *crlf;  ///< Pointer to the next CRLF delimiter while parsing the receive buffer.
    char *begin = msg_buf.data();  ///< Start of the unparsed data currently in `msg_buf`.

    std::shared_ptr<launch_session_t> session;  ///< Launch session claimed by this RTSP socket.
  };

  /**
   * @brief Lowercase hexadecimal SHA-256 of a client certificate string.
   *
   * @param cert Certificate PEM string.
   * @return 64 lowercase hexadecimal characters.
   */
  std::string cert_hash_hex(std::string_view cert) {
    const auto digest = crypto::hash(cert);
    std::string hex;
    hex.reserve(digest.size() * 2);
    for (const auto byte : digest) {
      hex += std::format("{:02x}", static_cast<unsigned>(byte));
    }
    return hex;
  }

  /**
   * @brief Set while the listening callback runs on the thread that called `start()`; there the commands that wait for
   * the loop thread refuse at once, as they do on the loop thread.
   */
  thread_local bool in_listening_callback = false;

  /**
   * @brief RTSP listener that matches incoming clients to pending launch sessions.
   */
  class rtsp_server_t {
  public:
    ~rtsp_server_t() {
#ifdef SUNSHINE_TESTS
      test_stop_loop();
#endif
      clear();
    }

    /**
     * @brief Bind the underlying socket or graphics resource to its target.
     *
     * @param af Address family used for socket creation or binding.
     * @param port TCP or UDP port number.
     * @param ec Error code returned by the asynchronous operation.
     * @return Network operation status.
     */
    int bind(net::af_e af, std::uint16_t port, boost::system::error_code &ec) {
      af = net::get_effective_address_family(af);
      acceptor.open(af == net::IPV4 ? tcp::v4() : tcp::v6(), ec);
      if (ec) {
        return -1;
      }

      acceptor.set_option(boost::asio::socket_base::reuse_address {true});

      auto bind_addr_str = net::get_bind_address(af);
      const auto bind_addr = boost::asio::ip::make_address(bind_addr_str, ec);
      if (ec) {
        BOOST_LOG(error) << "Invalid bind address: "sv << bind_addr_str << " - " << ec.message();
        return -1;
      }

      acceptor.bind(tcp::endpoint(bind_addr, port), ec);
      if (ec) {
        return -1;
      }

      acceptor.listen(4096, ec);
      if (ec) {
        return -1;
      }

      return 0;
    }

#ifdef __linux__
    /**
     * @brief Take over a listening socket that already exists instead of binding one (drain handoff).
     *
     * Works on a close-on-exec duplicate of `fd`; the caller's descriptor is never closed or changed. On any failure
     * the duplicate is closed again and the acceptor is left unchanged.
     *
     * @param fd A bound, listening, stream TCP socket (`AF_INET` or `AF_INET6`).
     * @param ec Error code set on failure.
     * @return 0 on success, -1 on failure.
     */
    int adopt(int fd, boost::system::error_code &ec) {
      const int dup_fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
      if (dup_fd < 0) {
        ec.assign(errno, boost::system::system_category());
        return -1;
      }

      auto fail = [&](int err) {
        ::close(dup_fd);
        ec.assign(err, boost::system::system_category());
        return -1;
      };

      int type = 0;
      socklen_t type_len = sizeof(type);
      if (::getsockopt(dup_fd, SOL_SOCKET, SO_TYPE, &type, &type_len) < 0) {
        return fail(errno);
      }
      if (type != SOCK_STREAM) {
        return fail(ESOCKTNOSUPPORT);
      }

      int listening = 0;
      socklen_t listening_len = sizeof(listening);
      if (::getsockopt(dup_fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &listening_len) < 0) {
        return fail(errno);
      }
      if (!listening) {
        return fail(EINVAL);
      }

      sockaddr_storage addr {};
      socklen_t addr_len = sizeof(addr);
      if (::getsockname(dup_fd, reinterpret_cast<sockaddr *>(&addr), &addr_len) < 0) {
        return fail(errno);
      }

      tcp protocol = tcp::v4();
      if (addr.ss_family == AF_INET6) {
        protocol = tcp::v6();
      } else if (addr.ss_family != AF_INET) {
        return fail(EAFNOSUPPORT);
      }

      // On failure asio does not take ownership of the descriptor.
      acceptor.assign(protocol, dup_fd, ec);
      if (ec) {
        ::close(dup_fd);
        return -1;
      }

      return 0;
    }
#endif

    /**
     * @brief Finish setting up a listener that `bind` or `adopt` created, and start accepting unless paused.
     *
     * Takes the work guard, publishes the listener descriptor, calls the listening callback (on the calling thread,
     * where the loop-thread-only commands refuse to wait) and arms the first accept when not paused.
     *
     * @param paused Whether accepting starts paused.
     */
    void begin_listening(bool paused) {
      _work_guard.emplace(boost::asio::make_work_guard(io_context));
      _paused.store(paused);
      _listener_fd.store(acceptor.native_handle());
      _listener_state.store(listener_open);

      std::function<void(unsigned short)> callback;
      {
        std::lock_guard lg {_state_lock};
        callback = _listening_callback;
      }
      if (callback) {
        boost::system::error_code port_ec;
        const auto port = acceptor.local_endpoint(port_ec).port();
        in_listening_callback = true;
        try {
          callback(port);
        } catch (...) {
          in_listening_callback = false;
          throw;
        }
        in_listening_callback = false;
      }

      if (!paused) {
        arm_accept();
      }
    }

    /**
     * @brief Record a listener adoption request for `start()`.
     *
     * @param fd Descriptor to adopt.
     * @param paused Whether accepting starts paused.
     * @return `true` when recorded.
     */
    bool record_adoption(int fd, bool paused) {
      std::lock_guard lg {_state_lock};
      if (fd < 0 || _adopt_fd >= 0 || _started) {
        return false;
      }
      _adopt_fd = fd;
      _adopt_paused = paused;
      return true;
    }

    /**
     * @brief Mark `start()` as begun and fetch the adoption request.
     *
     * @param fd Receives the descriptor to adopt, or -1.
     * @param paused Receives the paused flag.
     */
    void begin_start(int &fd, bool &paused) {
      std::lock_guard lg {_state_lock};
      _started = true;
      fd = _adopt_fd;
      paused = _adopt_paused;
    }

    /**
     * @brief Register the function called once the listener exists.
     *
     * @param callback Function to call; empty removes it.
     */
    void set_listening_callback(std::function<void(unsigned short)> callback) {
      std::lock_guard lg {_state_lock};
      _listening_callback = std::move(callback);
    }

    /**
     * @brief The listening descriptor.
     *
     * @return The descriptor or -1.
     */
    int listener_fd() const {
      return _listener_fd.load();
    }

    /**
     * @brief Stop arming accepts and wait until the loop thread has done so.
     *
     * @param timeout Longest wait.
     * @return See `rtsp_stream::pause_accept`.
     */
    bool pause_accept(std::chrono::milliseconds timeout) {
      if (_listener_state.load() != listener_open) {
        return false;
      }
      return run_command(timeout, [this] {
        _paused.store(true);
        cancel_outstanding_silently();
        return acceptor.is_open();
      });
    }

    /**
     * @brief Arm accepting again.
     *
     * @param timeout Longest wait.
     * @return See `rtsp_stream::resume_accept`.
     */
    bool resume_accept(std::chrono::milliseconds timeout) {
      if (_listener_state.load() != listener_open) {
        return false;
      }
      return run_command(timeout, [this] {
        if (!acceptor.is_open()) {
          return false;
        }
        _paused.store(false);
        // At most one accept is outstanding: while one is, its completion handler arms the next one.
        if (_outstanding.load() == 0) {
          arm_accept();
        }
        return true;
      });
    }

    /**
     * @brief Close this process's copy of the listening socket.
     *
     * @param timeout Longest wait.
     * @return See `rtsp_stream::close_listener`.
     */
    bool close_listener(std::chrono::milliseconds timeout) {
      const auto state = _listener_state.load();
      if (state == listener_none || _stopped.load()) {
        return false;
      }
      if (state == listener_closed) {
        return true;
      }
      return run_command(timeout, [this] {
        _paused.store(true);
        cancel_outstanding_silently();
        boost::system::error_code ec;
        acceptor.close(ec);
        _listener_fd.store(-1);
        _listener_state.store(listener_closed);
        return true;
      });
    }

    /**
     * @brief Whether a launch session is pending.
     *
     * @return `true` while one is.
     */
    bool has_pending_launch() {
      std::lock_guard lg {_launch_mutex};
      return static_cast<bool>(launch_event.view(0s));
    }

    /**
     * @brief Drop the pending launch session and cancel its expiry timer.
     *
     * @return `true` when a pending launch existed.
     */
    bool drop_pending_launch() {
      std::lock_guard lg {_launch_mutex};
      raised_timer.cancel();
      auto dropped = launch_event.pop(0s);
      if (dropped) {
        BOOST_LOG(debug) << "Dropped pending launch session: "sv << dropped->unique_id;
        return true;
      }
      return false;
    }

    /**
     * @brief Dispatch a parsed RTSP request to its handler.
     *
     * @param sock Socket used to read or write the protocol message.
     * @param session Active streaming or pairing session for the request.
     * @param req Parsed RTSP request being handled.
     */
    void handle_msg(tcp::socket &sock, launch_session_t &session, msg_t &&req) {
      auto func = _map_cmd_cb.find(req->message.request.command);
      if (func != std::end(_map_cmd_cb)) {
        func->second(this, sock, session, std::move(req));
      } else {
        cmd_not_found(sock, session, std::move(req));
      }

      boost::system::error_code ec;
      sock.shutdown(boost::asio::socket_base::shutdown_type::shutdown_both, ec);
    }

    /**
     * @brief Accept a pending connection and arm the server for the next client.
     *
     * @param ec Error code returned by the asynchronous operation.
     */
    void handle_accept(boost::system::error_code ec, std::uint64_t generation) {
      if (generation <= _stale_below.load()) {
        // Completion of an accept armed before the last test reset.
        return;
      }
      _outstanding.store(0);

#ifdef SUNSHINE_TESTS
      {
        std::lock_guard lg {_state_lock};
        if (_injected_ec) {
          ec = *_injected_ec;
          _injected_ec.reset();
        }
      }
#endif

      if (ec) {
        if (ec == boost::asio::error::operation_aborted && generation == _silent_generation.load()) {
          // The accept that a pause or close cancelled: not an error. Arm the next one if accepting resumed meanwhile.
          if (!_paused.load() && acceptor.is_open()) {
            arm_accept();
          }
          return;
        }

        _accept_errors.fetch_add(1);
        BOOST_LOG(error) << "Couldn't accept incoming connections: "sv << ec.message();

        // Stop server
        clear();
        return;
      }

      _accepted.fetch_add(1);
      auto socket = std::move(next_socket);

      auto launch_session {launch_event.view(0s)};
      if (launch_session) {
        // Associate the current RTSP session with this socket and start reading
        socket->session = launch_session;
        socket->read();
      } else {
        // This can happen due to normal things like port scanning, so let's not make these visible by default
        BOOST_LOG(debug) << "No pending session for incoming RTSP connection"sv;

        // If there is no session pending, close the connection immediately
        boost::system::error_code ec;
        socket->sock.close(ec);
      }

      // Queue another asynchronous accept for the next incoming connection, unless accepting is paused
      if (!_paused.load() && acceptor.is_open()) {
        arm_accept();
      }
    }

    /**
     * @brief Register or visit handlers stored in the map.
     *
     * @param type Protocol, message, or resource type selector.
     * @param cb Callback invoked for each matching message or session.
     */
    void map(const std::string_view &type, cmd_func_t cb) {
      _map_cmd_cb.emplace(type, std::move(cb));
    }

    /**
     * @brief Launch a new streaming session.
     * @note If the client does not begin streaming within the ping_timeout,
     *       the session will be discarded.
     * @param launch_session Streaming session information.
     */
    void session_raise(std::shared_ptr<launch_session_t> launch_session) {
      // The launch event and its timer are serialised by one mutex (never held across a wait or a callback).
      std::lock_guard lg {_launch_mutex};

      // If a launch event is still pending, don't overwrite it.
      if (launch_event.view(0s)) {
        return;
      }

      // Raise the new launch session to prepare for the RTSP handshake
      launch_event.raise(launch_session);

      // Arm the timer to expire this launch session if the client times out
      raised_timer.expires_after(config::stream.ping_timeout);
      raised_timer.async_wait([this, armed = std::move(launch_session)](const boost::system::error_code &ec) {
        if (!ec) {
          // Compare and pop in one step, so a drop or a newer raise cannot be undone by this late expiry.
          std::lock_guard lg {_launch_mutex};
          auto current = launch_event.view(0s);
          if (current && current == armed) {
            auto discarded = launch_event.pop(0s);
            if (discarded) {
              BOOST_LOG(debug) << "Event timeout: "sv << discarded->unique_id;
            }
          }
        }
      });
    }

    /**
     * @brief Clear state for the oldest launch session.
     * @param launch_session_id The ID of the session to clear.
     */
    void session_clear(uint32_t launch_session_id) {
      std::lock_guard lg {_launch_mutex};

      // We currently only support a single pending RTSP session,
      // so the ID should always match the one for that session.
      auto launch_session = launch_event.view(0s);
      if (launch_session) {
        if (launch_session->id != launch_session_id) {
          BOOST_LOG(error) << "Attempted to clear unexpected session: "sv << launch_session_id << " vs "sv << launch_session->id;
        } else {
          raised_timer.cancel();
          launch_event.pop();
        }
      }
    }

    /**
     * @brief Get the number of active sessions.
     * @return Count of active sessions.
     */
    int session_count() {
      auto lg = _session_slots.lock();
      return (int) _session_slots->size();
    }

    safe::event_t<std::shared_ptr<launch_session_t>> launch_event;  ///< Launch event.

    /**
     * @brief Clear launch sessions.
     *
     * The sessions to end are selected and stopped under the slots lock, which is then released while they are
     * joined, so that listing, closing, counting and inserting sessions never wait for a join. Each ended session is
     * erased, and logged, under the slots lock again. Concurrent clears are serialised, so no session is joined twice.
     * With `all` set, this returns only after every session present when it started has been joined and erased.
     *
     * @param all If true, clear all sessions. Otherwise, only clear timed out and stopped sessions.
     * @param reason Stop reason to record for the sessions this call ends.
     * @examples
     * clear(false);
     * @examples_end
     */
    void clear(bool all = true, stream::stop_reason_e reason = stream::stop_reason_e::terminated) {
      std::lock_guard cg {_clear_lock};
      clear_locked(all, reason);
    }

    /**
     * @brief Clear stopped sessions unless another clear is already running.
     *
     * The clear that is already running ends the stopped sessions itself, so this returns at once instead of waiting
     * for its joins.
     */
    void clear_stopped_if_idle() {
      std::unique_lock cg {_clear_lock, std::try_to_lock};
      if (cg.owns_lock()) {
        clear_locked(false);
      }
    }

    /**
     * @brief Clear by cert state.
     *
     * Locking is the same as for `clear()`: sessions are stopped under the slots lock and joined outside of it.
     *
     * @param cert Certificate data or object used by the operation.
     */
    void clear_by_cert(std::string_view cert) {
      std::lock_guard cg {_clear_lock};
      end_sessions([cert](stream::session_t &slot) {
        return stream::session::client_cert(slot) == cert;
      });
    }

    /**
     * @brief Describe the sessions that are running or stopping.
     *
     * Sessions that have not started yet are skipped.
     *
     * @return One entry per running or stopping session, ordered by id.
     */
    std::vector<session_info_t> list() {
      std::vector<session_info_t> sessions;
      {
        auto lg = _session_slots.lock();
        sessions.reserve(_session_slots->size());
        for (const auto &slot : *_session_slots) {
          const auto state = stream::session::state(*slot);
          if (state != stream::session::state_e::RUNNING && state != stream::session::state_e::STOPPING) {
            continue;
          }
          sessions.push_back(stream::session::describe(*slot));
        }
      }

      std::ranges::sort(sessions, {}, &session_info_t::id);
      return sessions;
    }

    /**
     * @brief Ask the session with the given launch session id to stop.
     *
     * The session is neither joined nor erased here: `clear(false)`, run by the RTSP loop, joins and removes it
     * once it is `STOPPING`. Only the slots lock is held, and only for the state change: nothing is joined under
     * it. `stream::session::stop()` only waits while a session is `STARTING`, a state this method never reaches.
     *
     * @param id Launch session id.
     * @return `closed` when this call stopped the running session, `already_stopping` when the session was already
     *         stopping or another stop won the race, `not_found` for an unknown id or a session that has not started.
     */
    close_result_e close(std::uint32_t id) {
      auto lg = _session_slots.lock();
      for (const auto &slot : *_session_slots) {
        if (stream::session::id(*slot) != id) {
          continue;
        }

        // Sessions that are not started (or already finished) are not closable. This also avoids the stop() wait.
        const auto state = stream::session::state(*slot);
        if (state != stream::session::state_e::RUNNING && state != stream::session::state_e::STOPPING) {
          return close_result_e::not_found;
        }

        return stream::session::stop(*slot, stream::stop_reason_e::api_close) ? close_result_e::closed : close_result_e::already_stopping;
      }
      return close_result_e::not_found;
    }

    /**
     * @brief Removes the provided session from the set of sessions.
     * @param session The session to remove.
     */
    void remove(const std::shared_ptr<stream::session_t> &session) {
      auto lg = _session_slots.lock();
      _session_slots->erase(session);
    }

#ifdef SUNSHINE_TESTS
    /**
     * @brief Remove every session from the set without joining it (test cleanup).
     */
    void remove_all() {
      auto lg = _session_slots.lock();
      _session_slots->clear();
    }
#endif

    /**
     * @brief Inserts the provided session into the set of sessions.
     * @param session The session to insert.
     */
    void insert(const std::shared_ptr<stream::session_t> &session) {
      auto lg = _session_slots.lock();
      _session_slots->emplace(session);
      BOOST_LOG(info) << "New streaming session started [active sessions: "sv << _session_slots->size() << ']';
    }

    /**
     * @brief Runs an iteration of the RTSP server loop
     */
    void iterate() {
      // If we have a session, we will return to the server loop every
      // 500ms to allow session cleanup to happen.
      if (session_count() > 0) {
        io_context.run_one_for(500ms);
      } else {
        io_context.run_one();
      }
      _iterations.fetch_add(1);
    }

    /**
     * @brief Stop the RTSP server.
     */
    void stop() {
      _stopped.store(true);
      acceptor.close();
      _listener_fd.store(-1);
      _work_guard.reset();
      io_context.stop();
      clear();
    }

    /**
     * @brief Clear the sessions of a client by the SHA-256 of its certificate.
     *
     * Locking is the same as for `clear()`. Sessions with an empty certificate never match.
     *
     * @param sha256_hex 64 lowercase hexadecimal characters.
     * @param reason Stop reason to record.
     * @param certs Receives the distinct certificates of the matched sessions.
     * @return Number of sessions this call moved to stopping.
     */
    std::size_t clear_by_cert_hash(std::string_view sha256_hex, stream::stop_reason_e reason, std::set<std::string> &certs) {
      std::lock_guard cg {_clear_lock};
      return end_sessions(
        [&](stream::session_t &slot) {
          const auto &cert = stream::session::client_cert(slot);
          if (cert.empty() || cert_hash_hex(cert) != sha256_hex) {
            return false;
          }
          certs.insert(cert);
          return true;
        },
        reason
      );
    }

#ifdef SUNSHINE_TESTS
    /**
     * @brief Test seam: open the listener on a duplicate of a test-owned socket, as `start()` does for an adoption.
     *
     * @param fd Listening socket.
     * @param paused Whether accepting starts paused.
     * @return `true` on success.
     */
    bool test_open_listener(int fd, bool paused) {
  #ifdef __linux__
      if (fd < 0 || acceptor.is_open() || _listener_state.load() == listener_open) {
        return false;
      }
      reset_counters();
      boost::system::error_code ec;
      if (adopt(fd, ec)) {
        return false;
      }
      begin_listening(paused);
      return true;
  #else
      (void) fd;
      (void) paused;
      return false;
  #endif
    }

    /**
     * @brief Test seam: start the loop thread.
     */
    void test_start_loop() {
      if (_test_loop.joinable()) {
        return;
      }
      _test_stop.store(false);
      _test_loop = std::thread {[this] {
        while (!_test_stop.load()) {
          iterate();
          clear(false);
        }
      }};
    }

    /**
     * @brief Test seam: stop the loop thread, as the shutdown path does.
     */
    void test_stop_loop() {
      _test_stop.store(true);
      stop();
      if (_test_loop.joinable()) {
        _test_loop.join();
      }
    }

    /**
     * @brief Test seam: return the server to its state before `test_open_listener`.
     */
    void test_reset() {
      test_stop_loop();
      boost::system::error_code ec;
      acceptor.close(ec);
      _stale_below.store(_accept_generation.load());
      io_context.restart();
      // Run and discard the handlers left over from the stopped run, with the stale marker already set.
      io_context.poll();
      io_context.restart();
      _work_guard.reset();
      next_socket.reset();
      {
        std::lock_guard lg {_state_lock};
        _adopt_fd = -1;
        _adopt_paused = false;
        _started = false;
        _listening_callback = nullptr;
        _injected_ec.reset();
      }
      drop_pending_launch();
      remove_all();
      _paused.store(false);
      _stopped.store(false);
      _outstanding.store(0);
      _listener_fd.store(-1);
      _listener_state.store(listener_none);
      reset_counters();
    }

    /**
     * @brief Test seam: make the next accept completion carry `ec`.
     *
     * @param ec Error code; none removes a pending injection.
     */
    void test_inject_accept_error(const boost::system::error_code &ec) {
      std::lock_guard lg {_state_lock};
      if (ec) {
        _injected_ec = ec;
      } else {
        _injected_ec.reset();
      }
    }

    /**
     * @brief Number of times `iterate()` returned.
     *
     * @return The count.
     */
    std::uint64_t loop_iterations() const {
      return _iterations.load();
    }

    /**
     * @brief Number of successful accept completions handled.
     *
     * @return The count.
     */
    std::uint64_t accepted_connections() const {
      return _accepted.load();
    }

    /**
     * @brief Number of accept completions treated as errors.
     *
     * @return The count.
     */
    std::uint64_t accept_errors() const {
      return _accept_errors.load();
    }

    /**
     * @brief Number of accepts armed whose completion has not run.
     *
     * @return 0 or 1.
     */
    int outstanding_accepts() const {
      return _outstanding.load();
    }
#endif

  private:
    using slots_t = std::set<std::shared_ptr<stream::session_t>>;  ///< Container type of the tracked sessions.

    /**
     * @brief Stop the selected sessions, join them without the slots lock, then erase and log them.
     *
     * The caller must hold the clear lock. Sessions are erased by value, and logged only when actually erased, so a
     * concurrent `remove()` of the same session cannot cause a crash or a second log line.
     *
     * @param select Predicate choosing the sessions to end; called with the slots lock held.
     * @param reason Stop reason to record for the selected sessions; a session that is already stopping keeps its first reason.
     */
    template<class Select>
    std::size_t end_sessions(Select &&select, stream::stop_reason_e reason = stream::stop_reason_e::terminated) {
      std::vector<std::shared_ptr<stream::session_t>> ending;
      std::size_t stopped = 0;
      {
        auto lg = _session_slots.lock();
        for (const auto &slot : *_session_slots) {
          if (select(*slot)) {
            if (stream::session::stop(*slot, reason)) {
              ++stopped;
            }
            ending.push_back(slot);
          }
        }
      }

      // Joining may call into process and display callbacks, so it must not hold the slots lock.
      for (const auto &slot : ending) {
        stream::session::join(*slot);
      }

      auto lg = _session_slots.lock();
      for (const auto &slot : ending) {
        if (_session_slots->erase(slot) > 0) {
          BOOST_LOG(info) << "Session ended: reason="sv << stream::to_string(stream::session::stop_reason(*slot)) << ", active sessions="sv << _session_slots->size();
        }
      }

      return stopped;
    }

    /**
     * @brief Implementation of `clear()`; the caller must hold the clear lock.
     *
     * @param all If true, end all sessions. Otherwise, only end stopping ones.
     * @param reason Stop reason to record for the sessions this call ends.
     */
    void clear_locked(bool all, stream::stop_reason_e reason = stream::stop_reason_e::terminated) {
      end_sessions(
        [all](stream::session_t &slot) {
          return all || stream::session::state(slot) == stream::session::state_e::STOPPING;
        },
        reason
      );
    }

    /// Lifecycle of the listener, as seen by the commands that need one.
    enum listener_state_e {
      listener_none,  ///< No listener was ever opened.
      listener_open,  ///< The listener exists.
      listener_closed  ///< The listener was closed by `close_listener`.
    };

    /**
     * @brief Arm one accept on a fresh `next_socket`. Only for the thread that owns the acceptor state (the loop thread,
     * or the thread that runs `start()` before the loop runs), and only when no accept is outstanding.
     */
    void arm_accept() {
      next_socket = std::make_shared<socket_t>(io_context, [this](tcp::socket &sock, launch_session_t &session, msg_t &&msg) {
        handle_msg(sock, session, std::move(msg));
      });

      const auto generation = _accept_generation.fetch_add(1) + 1;
      _outstanding.store(1);
      acceptor.async_accept(next_socket->sock, [this, generation](const auto &ec) {
        handle_accept(ec, generation);
      });
    }

    /**
     * @brief Cancel the outstanding accept, if any, and mark its aborted completion as silent.
     */
    void cancel_outstanding_silently() {
      if (_outstanding.load() != 0) {
        _silent_generation.store(_accept_generation.load());
        boost::system::error_code ec;
        acceptor.cancel(ec);
      }
    }

    /**
     * @brief Whether the calling thread is the loop thread, or runs the listening callback.
     *
     * @return `true` when a command that waits for the loop thread would wait for itself.
     */
    bool on_loop_thread() {
      return in_listening_callback || io_context.get_executor().running_in_this_thread();
    }

    /**
     * @brief Run a function on the loop thread and wait for its result.
     *
     * The handlers run in posting order. The wait ends early when `stop()` runs.
     *
     * @param timeout Longest wait.
     * @param command Function to run on the loop thread.
     * @return The function's result; `false` on timeout, after `stop()`, or when called on the loop thread.
     */
    bool run_command(std::chrono::milliseconds timeout, std::function<bool()> command) {
      if (_stopped.load() || on_loop_thread()) {
        return false;
      }

      auto promise = std::make_shared<std::promise<bool>>();
      auto result = promise->get_future();
      boost::asio::post(io_context, [promise, command = std::move(command)]() {
        promise->set_value(command());
      });

      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (true) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (result.wait_for(std::clamp(remaining, std::chrono::milliseconds {0}, std::chrono::milliseconds {10})) == std::future_status::ready) {
          return result.get();
        }
        if (_stopped.load() || remaining <= std::chrono::milliseconds {0}) {
          return false;
        }
      }
    }

    /**
     * @brief Zero the observation counters.
     */
    void reset_counters() {
      _iterations.store(0);
      _accepted.store(0);
      _accept_errors.store(0);
    }

    std::mutex _launch_mutex;  ///< Serialises the launch event and `raised_timer` (never held across a wait or a callback).
    std::mutex _state_lock;  ///< Guards the adoption record, the listening callback and the injected error.
    int _adopt_fd = -1;  ///< Descriptor recorded by `record_adoption`, or -1.
    bool _adopt_paused = false;  ///< Paused flag recorded with the adoption.
    bool _started = false;  ///< `start()` has begun.
    std::function<void(unsigned short)> _listening_callback;  ///< Called once the listener exists.
    std::atomic<int> _listener_fd {-1};  ///< Atomic copy of the listening descriptor.
    std::atomic<int> _listener_state {listener_none};  ///< `listener_state_e`.
    std::atomic<bool> _paused {false};  ///< Accepting is paused.
    std::atomic<bool> _stopped {false};  ///< `stop()` ran.
    std::atomic<int> _outstanding {0};  ///< Accepts armed whose completion has not run (0 or 1).
    std::atomic<std::uint64_t> _accept_generation {0};  ///< Generation of the latest armed accept.
    std::atomic<std::uint64_t> _silent_generation {0};  ///< Generation whose aborted completion is silent.
    std::atomic<std::uint64_t> _stale_below {0};  ///< Completions of generations up to this are ignored.
    std::atomic<std::uint64_t> _iterations {0};  ///< Times `iterate()` returned.
    std::atomic<std::uint64_t> _accepted {0};  ///< Successful accept completions.
    std::atomic<std::uint64_t> _accept_errors {0};  ///< Accept completions handled as errors.
#ifdef SUNSHINE_TESTS
    std::optional<boost::system::error_code> _injected_ec;  ///< Error for the next accept completion.
    std::thread _test_loop;  ///< Loop thread of the test seam.
    std::atomic<bool> _test_stop {false};  ///< Asks the test loop thread to end.
#endif

    std::mutex _clear_lock;  ///< Serialises `clear()` and `clear_by_cert()`; taken before the slots lock, never while holding it.

    std::unordered_map<std::string_view, cmd_func_t> _map_cmd_cb;

    sync_util::sync_t<std::set<std::shared_ptr<stream::session_t>>> _session_slots;

    boost::asio::io_context io_context;
    tcp::acceptor acceptor {io_context};
    boost::asio::steady_timer raised_timer {io_context};

    std::shared_ptr<socket_t> next_socket;

    std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> _work_guard;  ///< Keeps the loop idle instead of spinning; declared after `io_context`, which it refers to.
  };

  rtsp_server_t server {};  ///< Singleton RTSP server used by GameStream launch sessions.

  /**
   * @brief Queue a launch session until the RTSP client connects.
   */
  void launch_session_raise(std::shared_ptr<launch_session_t> launch_session) {
    server.session_raise(std::move(launch_session));
  }

  void launch_session_clear(uint32_t launch_session_id) {
    server.session_clear(launch_session_id);
  }

  bool has_pending_launch() {
    return server.has_pending_launch();
  }

  bool drop_pending_launch() {
    return server.drop_pending_launch();
  }

  bool adopt_listener(int fd, bool paused) {
#ifdef __linux__
    return server.record_adoption(fd, paused);
#else
    (void) fd;
    (void) paused;
    return false;
#endif
  }

  void set_listening_callback(std::function<void(unsigned short)> callback) {
    server.set_listening_callback(std::move(callback));
  }

  int listener_fd() {
    return server.listener_fd();
  }

  bool pause_accept(std::chrono::milliseconds timeout) {
    return server.pause_accept(timeout);
  }

  bool resume_accept(std::chrono::milliseconds timeout) {
    return server.resume_accept(timeout);
  }

  bool close_listener(std::chrono::milliseconds timeout) {
    return server.close_listener(timeout);
  }

  int session_count() {
    // Ensure session_count is up-to-date. If a clear is already joining sessions, it ends the stopped ones itself.
    server.clear_stopped_if_idle();

    return server.session_count();
  }

  int total_session_count() {
    int total = session_count();
#ifdef __linux__
    // One SESSIONS? to the draining process, if there is one. An unknown answer counts as one session.
    const auto draining = handoff::draining_session_count(handoff::peer_request_timeout);
    total += draining ? static_cast<int>(*draining) : 1;
#endif
    return total;
  }

  std::vector<session_info_t> list_sessions() {
    return server.list();
  }

  close_result_e request_session_close(std::uint32_t id) {
    return server.close(id);
  }

#ifdef SUNSHINE_TESTS
  namespace testing {
    std::shared_ptr<stream::session_t> insert_fake_session(std::uint32_t id, stream::session::state_e state, std::shared_future<void> join_gate) {
      auto session = stream::session::testing::make_fake(id, state, std::move(join_gate));
      server.insert(session);
      return session;
    }

    void clear(bool all, stream::stop_reason_e reason) {
      server.clear(all, reason);
    }

    void remove_all() {
      server.remove_all();
    }
  }  // namespace testing
#endif

  /**
   * @brief Map a handoff shutdown reason to the session stop reason it records.
   *
   * @param reason Shutdown reason from `handoff::hooks::shutdown_reason()`.
   * @return `stop` for `shutdown_reason_e::stop`, otherwise `terminated`.
   */
  stream::stop_reason_e to_stop_reason(handoff::shutdown_reason_e reason) {
    switch (reason) {
      case handoff::shutdown_reason_e::stop:
        return stream::stop_reason_e::stop;
      case handoff::shutdown_reason_e::terminated:
        break;
    }
    return stream::stop_reason_e::terminated;
  }

  void terminate_sessions(stream::stop_reason_e reason) {
    server.clear(true, reason);
    input::terminate_gamepads();
  }

  /**
   * @brief Terminate active sessions associated with a client certificate.
   */
  void terminate_sessions_by_cert(std::string_view cert) {
    server.clear_by_cert(cert);
    input::terminate_gamepads(cert);
  }

  std::size_t terminate_sessions_by_cert_hash(std::string_view sha256_hex, stream::stop_reason_e reason) {
    const bool valid = sha256_hex.size() == 64 && std::ranges::all_of(sha256_hex, [](char c) {
                         return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                       });
    if (!valid) {
      BOOST_LOG(debug) << "Ignoring a certificate hash that is not 64 lowercase hexadecimal characters"sv;
      return 0;
    }

    std::set<std::string> certs;
    const auto ended = server.clear_by_cert_hash(sha256_hex, reason, certs);
    for (const auto &cert : certs) {
      input::terminate_gamepads(cert);
    }
    return ended;
  }

  /**
   * @brief Send the serialized response over the active socket.
   *
   * @param sock Socket used to read or write the protocol message.
   * @param sv String view containing the text to inspect.
   * @return Network operation status.
   */
  int send(tcp::socket &sock, const std::string_view &sv) {
    std::size_t bytes_send = 0;

    while (bytes_send != sv.size()) {
      boost::system::error_code ec;
      bytes_send += sock.send(boost::asio::buffer(sv.substr(bytes_send)), 0, ec);

      if (ec) {
        BOOST_LOG(error) << "RTSP: Couldn't send data over tcp socket: "sv << ec.message();
        return -1;
      }
    }

    return 0;
  }

  /**
   * @brief Send the protocol response for the current request.
   *
   * @param sock Socket used to read or write the protocol message.
   * @param session Active streaming or pairing session for the request.
   * @param resp RTSP response string to send to the client.
   */
  void respond(tcp::socket &sock, launch_session_t &session, msg_t &resp) {
    auto payload = std::make_pair(resp->payload, resp->payloadLength);

    // Restore response message for proper destruction
    auto lg = util::fail_guard([&]() {
      resp->payload = payload.first;
      resp->payloadLength = payload.second;
    });

    resp->payload = nullptr;
    resp->payloadLength = 0;

    int serialized_len;
    util::c_ptr<char> raw_resp {serializeRtspMessage(resp.get(), &serialized_len)};
    BOOST_LOG(debug)
      << "---Begin Response---"sv << std::endl
      << std::string_view {raw_resp.get(), (std::size_t) serialized_len} << std::endl
      << std::string_view {payload.first, (std::size_t) payload.second} << std::endl
      << "---End Response---"sv << std::endl;

    // Encrypt the RTSP message if encryption is enabled
    if (session.rtsp_cipher) {
      // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
      // Section 8.2.1. The sequence number is our "invocation" field and the 'RH' in the
      // high bytes is the "fixed" field. Because each client provides their own unique
      // key, our values in the fixed field need only uniquely identify each independent
      // use of the client's key with AES-GCM in our code.
      //
      // The sequence number is 32 bits long which allows for 2^32 RTSP messages to be
      // sent to each client before the IV repeats.
      crypto::aes_t iv(12);
      session.rtsp_iv_counter++;
      std::copy_n((uint8_t *) &session.rtsp_iv_counter, sizeof(session.rtsp_iv_counter), std::begin(iv));
      iv[10] = 'H';  // Host originated
      iv[11] = 'R';  // RTSP

      // Allocate the message with an empty header and reserved space for the payload
      auto payload_length = serialized_len + payload.second;
      std::vector<uint8_t> message(sizeof(encrypted_rtsp_header_t));
      message.reserve(message.size() + payload_length);

      // Copy the complete plaintext into the message
      std::copy_n(raw_resp.get(), serialized_len, std::back_inserter(message));
      std::copy_n(payload.first, payload.second, std::back_inserter(message));

      // Initialize the message header
      auto header = (encrypted_rtsp_header_t *) message.data();
      header->typeAndLength = util::endian::big<std::uint32_t>(encrypted_rtsp_header_t::ENCRYPTED_MESSAGE_TYPE_BIT + payload_length);
      header->sequenceNumber = util::endian::big<std::uint32_t>(session.rtsp_iv_counter);

      // Encrypt the RTSP message in place
      session.rtsp_cipher->encrypt(std::string_view {(const char *) header->payload(), (std::size_t) payload_length}, header->tag, &iv);

      // Send the full encrypted message
      send(sock, std::string_view {(char *) message.data(), message.size()});
    } else {
      std::string_view tmp_resp {raw_resp.get(), (size_t) serialized_len};

      // Send the plaintext RTSP message header
      if (send(sock, tmp_resp)) {
        return;
      }

      // Send the plaintext RTSP message payload (if present)
      send(sock, std::string_view {payload.first, (std::size_t) payload.second});
    }
  }

  /**
   * @brief Send the protocol response for the current request.
   */
  void respond(tcp::socket &sock, launch_session_t &session, POPTION_ITEM options, int statuscode, const char *status_msg, int seqn, const std::string_view &payload) {
    msg_t resp {new msg_t::element_type};
    createRtspResponse(resp.get(), nullptr, 0, const_cast<char *>("RTSP/1.0"), statuscode, const_cast<char *>(status_msg), seqn, options, const_cast<char *>(payload.data()), (int) payload.size());

    respond(sock, session, resp);
  }

  /**
   * @brief Handle an unsupported RTSP command.
   */
  void cmd_not_found(tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    respond(sock, session, nullptr, 404, "NOT FOUND", req->sequenceNumber, {});
  }

  /**
   * @brief Handle RTSP OPTIONS requests.
   *
   * @param server RTSP server instance handling the request.
   * @param sock Socket used to read or write the protocol message.
   * @param session Active streaming or pairing session for the request.
   * @param req Parsed RTSP request being handled.
   */
  void cmd_option(rtsp_server_t *server, tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    OPTION_ITEM option {};

    // I know these string literals will not be modified
    option.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    option.content = const_cast<char *>(seqn_str.c_str());

    respond(sock, session, &option, 200, "OK", req->sequenceNumber, {});
  }

  /**
   * @brief Handle RTSP DESCRIBE requests.
   *
   * @param server RTSP server instance.
   * @param sock Client socket.
   * @param session Launch session.
   * @param req RTSP request message.
   */
  void cmd_describe(rtsp_server_t *server, tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    OPTION_ITEM option {};

    // I know these string literals will not be modified
    option.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    option.content = const_cast<char *>(seqn_str.c_str());

    std::stringstream ss;

    // Tell the client about our supported features
    ss << "a=x-ss-general.featureFlags:" << (uint32_t) platf::get_capabilities() << std::endl;

    // Always request new control stream encryption if the client supports it
    uint32_t encryption_flags_supported = SS_ENC_CONTROL_V2 | SS_ENC_AUDIO;
    uint32_t encryption_flags_requested = SS_ENC_CONTROL_V2;

    // Determine the encryption desired for this remote endpoint
    auto encryption_mode = net::encryption_mode_for_address(sock.remote_endpoint().address());
    if (encryption_mode != config::ENCRYPTION_MODE_NEVER) {
      // Advertise support for video encryption if it's not disabled
      encryption_flags_supported |= SS_ENC_VIDEO;

      // If it's mandatory, also request it to enable use if the client
      // didn't explicitly opt in, but it otherwise has support.
      if (encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
        encryption_flags_requested |= SS_ENC_VIDEO | SS_ENC_AUDIO;
      }
    }

    // Report supported and required encryption flags
    ss << "a=x-ss-general.encryptionSupported:" << encryption_flags_supported << std::endl;
    ss << "a=x-ss-general.encryptionRequested:" << encryption_flags_requested << std::endl;

    if (video::last_encoder_probe_supported_ref_frames_invalidation) {
      ss << "a=x-nv-video[0].refPicInvalidation:1"sv << std::endl;
    }

    if (video::active_hevc_mode != 1) {
      ss << "sprop-parameter-sets=AAAAAU"sv << std::endl;
    }

    if (video::active_av1_mode != 1) {
      ss << "a=rtpmap:98 AV1/90000"sv << std::endl;
    }

    if (!session.surround_params.empty()) {
      // If we have our own surround parameters, advertise them twice first
      ss << "a=fmtp:97 surround-params="sv << session.surround_params << std::endl;
      ss << "a=fmtp:97 surround-params="sv << session.surround_params << std::endl;
    }

    for (int x = 0; x < audio::MAX_STREAM_CONFIG; ++x) {
      auto &stream_config = audio::stream_configs[x];
      std::uint8_t mapping[platf::speaker::MAX_SPEAKERS];

      auto mapping_p = stream_config.mapping;

      /**
       * GFE advertises incorrect mapping for normal quality configurations,
       * as a result, Moonlight rotates all channels from index '3' to the right
       * To work around this, rotate channels to the left from index '3'
       */
      if (x == audio::SURROUND51 || x == audio::SURROUND71) {
        std::copy_n(mapping_p, stream_config.channelCount, mapping);
        std::rotate(mapping + 3, mapping + 4, mapping + audio::MAX_STREAM_CONFIG);

        mapping_p = mapping;
      }

      ss << "a=fmtp:97 surround-params="sv << stream_config.channelCount << stream_config.streams << stream_config.coupledStreams;

      std::for_each_n(mapping_p, stream_config.channelCount, [&ss](std::uint8_t digit) {
        ss << (char) (digit + '0');
      });

      ss << std::endl;
    }

    respond(sock, session, &option, 200, "OK", req->sequenceNumber, ss.str());
  }

  /**
   * @brief Handle RTSP SETUP requests and prepare transport state.
   *
   * @param server RTSP server instance handling the request.
   * @param sock Socket used to read or write the protocol message.
   * @param session Active streaming or pairing session for the request.
   * @param req Parsed RTSP request being handled.
   */
  void cmd_setup(rtsp_server_t *server, tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    OPTION_ITEM options[4] {};

    auto &seqn = options[0];
    auto &session_option = options[1];
    auto &port_option = options[2];
    auto &payload_option = options[3];

    seqn.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    seqn.content = const_cast<char *>(seqn_str.c_str());

    std::string_view target {req->message.request.target};
    auto begin = std::find(std::begin(target), std::end(target), '=') + 1;
    auto end = std::find(begin, std::end(target), '/');
    std::string_view type {begin, (size_t) std::distance(begin, end)};

    std::uint16_t port;
    if (type == "audio"sv) {
      port = net::map_udp_port(stream::AUDIO_STREAM_PORT);
    } else if (type == "video"sv) {
      port = net::map_udp_port(stream::VIDEO_STREAM_PORT);
    } else if (type == "control"sv) {
      port = net::map_udp_port(stream::CONTROL_PORT);
    } else {
      cmd_not_found(sock, session, std::move(req));

      return;
    }

    seqn.next = &session_option;

    session_option.option = const_cast<char *>("Session");
    session_option.content = const_cast<char *>("DEADBEEFCAFE;timeout = 90");

    session_option.next = &port_option;

    // Moonlight merely requires 'server_port=<port>'
    auto port_value = std::format("server_port={}", static_cast<int>(port));

    port_option.option = const_cast<char *>("Transport");
    port_option.content = port_value.data();

    // Send identifiers that will be echoed in the other connections
    auto connect_data = std::to_string(session.control_connect_data);
    if (type == "control"sv) {
      payload_option.option = const_cast<char *>("X-SS-Connect-Data");
      payload_option.content = connect_data.data();
    } else {
      payload_option.option = const_cast<char *>("X-SS-Ping-Payload");
      payload_option.content = session.av_ping_payload.data();
    }

    port_option.next = &payload_option;

    respond(sock, session, &seqn, 200, "OK", req->sequenceNumber, {});
  }

  /**
   * @brief Handle RTSP ANNOUNCE requests from the client.
   *
   * @param server RTSP server instance handling the request.
   * @param sock Socket used to read or write the protocol message.
   * @param session Active streaming or pairing session for the request.
   * @param req Parsed RTSP request being handled.
   */
  void cmd_announce(rtsp_server_t *server, tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    OPTION_ITEM option {};

    // I know these string literals will not be modified
    option.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    option.content = const_cast<char *>(seqn_str.c_str());

    std::string_view payload {req->payload, (size_t) req->payloadLength};

    std::vector<std::string_view> lines;

    auto whitespace = [](char ch) {
      return ch == '\n' || ch == '\r';
    };

    {
      auto pos = std::begin(payload);
      auto begin = pos;
      while (pos != std::end(payload)) {
        if (whitespace(*pos++)) {
          lines.emplace_back(begin, pos - begin - 1);

          while (pos != std::end(payload) && whitespace(*pos)) {
            ++pos;
          }
          begin = pos;
        }
      }
    }

    std::string_view client;
    std::unordered_map<std::string_view, std::string_view> args;

    for (auto line : lines) {
      auto type = line.substr(0, 2);
      if (type == "s="sv) {
        client = line.substr(2);
      } else if (type == "a=") {
        auto pos = line.find(':');

        auto name = line.substr(2, pos - 2);
        auto val = line.substr(pos + 1);

        if (val[val.size() - 1] == ' ') {
          val = val.substr(0, val.size() - 1);
        }
        args.emplace(name, val);
      }
    }

    // Initialize any omitted parameters to defaults
    args.try_emplace("x-nv-video[0].encoderCscMode"sv, "0"sv);
    args.try_emplace("x-nv-vqos[0].bitStreamFormat"sv, "0"sv);
    args.try_emplace("x-nv-video[0].dynamicRangeMode"sv, "0"sv);
    args.try_emplace("x-nv-aqos.packetDuration"sv, "5"sv);
    args.try_emplace("x-nv-general.useReliableUdp"sv, "1"sv);
    args.try_emplace("x-nv-vqos[0].fec.minRequiredFecPackets"sv, "0"sv);
    args.try_emplace("x-nv-general.featureFlags"sv, "135"sv);
    args.try_emplace("x-ml-general.featureFlags"sv, "0"sv);
    args.try_emplace("x-nv-vqos[0].qosTrafficType"sv, "5"sv);
    args.try_emplace("x-nv-aqos.qosTrafficType"sv, "4"sv);
    args.try_emplace("x-ml-video.configuredBitrateKbps"sv, "0"sv);
    args.try_emplace("x-ss-general.encryptionEnabled"sv, "0"sv);
    args.try_emplace("x-ss-video[0].chromaSamplingType"sv, "0"sv);
    args.try_emplace("x-ss-video[0].intraRefresh"sv, "0"sv);
    args.try_emplace("x-nv-video[0].clientRefreshRateX100"sv, "0"sv);

    stream::config_t config;

    std::int64_t configuredBitrateKbps;
    config.audio.flags[audio::config_t::HOST_AUDIO] = session.host_audio;
    try {
      config.audio.channels = (int) util::from_view(args.at("x-nv-audio.surround.numChannels"sv));
      config.audio.mask = (int) util::from_view(args.at("x-nv-audio.surround.channelMask"sv));
      config.audio.packetDuration = (int) util::from_view(args.at("x-nv-aqos.packetDuration"sv));

      config.audio.flags[audio::config_t::HIGH_QUALITY] =
        util::from_view(args.at("x-nv-audio.surround.AudioQuality"sv));

      config.controlProtocolType = (int) util::from_view(args.at("x-nv-general.useReliableUdp"sv));
      config.packetsize = (int) util::from_view(args.at("x-nv-video[0].packetSize"sv));
      config.minRequiredFecPackets = (int) util::from_view(args.at("x-nv-vqos[0].fec.minRequiredFecPackets"sv));
      config.mlFeatureFlags = (int) util::from_view(args.at("x-ml-general.featureFlags"sv));
      config.audioQosType = (int) util::from_view(args.at("x-nv-aqos.qosTrafficType"sv));
      config.videoQosType = (int) util::from_view(args.at("x-nv-vqos[0].qosTrafficType"sv));
      config.encryptionFlagsEnabled = (uint32_t) util::from_view(args.at("x-ss-general.encryptionEnabled"sv));

      // Legacy clients use nvFeatureFlags to indicate support for audio encryption
      if (util::from_view(args.at("x-nv-general.featureFlags"sv)) & 0x20) {
        config.encryptionFlagsEnabled |= SS_ENC_AUDIO;
      }

      // Limit the packetsize to avoid fragmentation with clients that cannot configure this value
      if (config::stream.packetsize && config::stream.packetsize < config.packetsize) {
        if (config::stream.packetsize < config::PACKETSIZE_MIN || config::stream.packetsize > config::PACKETSIZE_MAX) {
          BOOST_LOG(warning) << "packetsize range: ["sv << config::PACKETSIZE_MIN << "-"sv << config::PACKETSIZE_MAX
                             << "] invalid value: "sv << config::stream.packetsize;
        } else {
          if (config::stream.packetsize < config::PACKETSIZE_SMALL) {
            BOOST_LOG(info) << "packetsize is small < "sv << config::PACKETSIZE_SMALL << " bytes, reduce bitrate if the stream breaks"sv;
          } else if (config::stream.packetsize > config::PACKETSIZE_LARGE) {
            BOOST_LOG(info) << "packetsize is large > "sv << config::PACKETSIZE_LARGE << " bytes, jumbo frames may be used"sv;
          }

          BOOST_LOG(info) << "packetsize limit: "sv << config.packetsize << " -> "sv << config::stream.packetsize << " bytes"sv;
          config.packetsize = config::stream.packetsize;
        }
      }

      config.monitor.height = (int) util::from_view(args.at("x-nv-video[0].clientViewportHt"sv));
      config.monitor.width = (int) util::from_view(args.at("x-nv-video[0].clientViewportWd"sv));
      config.monitor.framerate = (int) util::from_view(args.at("x-nv-video[0].maxFPS"sv));
      config.monitor.framerateX100 = (int) util::from_view(args.at("x-nv-video[0].clientRefreshRateX100"sv));
      // Validate framerateX100 against framerate. Some clients (e.g. Moonlight Android) send the
      // client display's refresh rate as clientRefreshRateX100, which may differ from the requested
      // streaming framerate. Discard framerateX100 if the derived fps is not within 1% of framerate.
      if (config.monitor.framerateX100 > 0) {
        double fps_strict = config.monitor.framerateX100 / 100.0;
        double ratio = fps_strict / config.monitor.framerate;
        if (ratio < 0.99 || ratio > 1.01) {
          config.monitor.framerateX100 = 0;
        }
      }
      config.monitor.bitrate = (int) util::from_view(args.at("x-nv-vqos[0].bw.maximumBitrateKbps"sv));
      config.monitor.slicesPerFrame = (int) util::from_view(args.at("x-nv-video[0].videoEncoderSlicesPerFrame"sv));
      config.monitor.numRefFrames = (int) util::from_view(args.at("x-nv-video[0].maxNumReferenceFrames"sv));
      config.monitor.encoderCscMode = (int) util::from_view(args.at("x-nv-video[0].encoderCscMode"sv));
      config.monitor.videoFormat = (int) util::from_view(args.at("x-nv-vqos[0].bitStreamFormat"sv));
      config.monitor.dynamicRange = (int) util::from_view(args.at("x-nv-video[0].dynamicRangeMode"sv));
      config.monitor.chromaSamplingType = (int) util::from_view(args.at("x-ss-video[0].chromaSamplingType"sv));
      config.monitor.enableIntraRefresh = (int) util::from_view(args.at("x-ss-video[0].intraRefresh"sv));

      configuredBitrateKbps = util::from_view(args.at("x-ml-video.configuredBitrateKbps"sv));
    } catch (std::out_of_range &) {
      respond(sock, session, &option, 400, "BAD REQUEST", req->sequenceNumber, {});
      return;
    }

    // When using stereo audio, the audio quality is (strangely) indicated by whether the Host field
    // in the RTSP message matches a local interface's IP address. Fortunately, Moonlight always sends
    // 0.0.0.0 when it wants low quality, so it is easy to check without enumerating interfaces.
    if (config.audio.channels == 2) {
      for (auto option = req->options; option != nullptr; option = option->next) {
        if ("Host"sv == option->option) {
          std::string_view content {option->content};
          BOOST_LOG(debug) << "Found Host: "sv << content;
          config.audio.flags[audio::config_t::HIGH_QUALITY] = (content.find("0.0.0.0"sv) == std::string::npos);
        }
      }
    } else if (session.surround_params.length() > 3) {
      // Channels
      std::uint8_t c = session.surround_params[0] - '0';
      // Streams
      std::uint8_t n = session.surround_params[1] - '0';
      // Coupled streams
      std::uint8_t m = session.surround_params[2] - '0';
      auto valid = false;
      if ((c == 6 || c == 8) && c == config.audio.channels && n + m == c && session.surround_params.length() == c + 3) {
        config.audio.customStreamParams.channelCount = c;
        config.audio.customStreamParams.streams = n;
        config.audio.customStreamParams.coupledStreams = m;
        valid = true;
        for (std::uint8_t i = 0; i < c; i++) {
          config.audio.customStreamParams.mapping[i] = session.surround_params[i + 3] - '0';
          if (config.audio.customStreamParams.mapping[i] >= c) {
            valid = false;
            break;
          }
        }
      }
      config.audio.flags[audio::config_t::CUSTOM_SURROUND_PARAMS] = valid;
    }
    if (session.continuous_audio) {
      BOOST_LOG(info) << "Client requested continuous audio"sv;
      config.audio.flags[audio::config_t::CONTINUOUS_AUDIO] = true;
    }

    // If the client sent a configured bitrate, we will choose the actual bitrate ourselves
    // by using FEC percentage and audio quality settings. If the calculated bitrate ends up
    // too low, we'll allow it to exceed the limits rather than reducing the encoding bitrate
    // down to nearly nothing.
    if (configuredBitrateKbps) {
      BOOST_LOG(debug) << "Client configured bitrate is "sv << configuredBitrateKbps << " Kbps"sv;

      // If the FEC percentage isn't too high, adjust the configured bitrate to ensure video
      // traffic doesn't exceed the user's selected bitrate when the FEC shards are included.
      if (config::stream.fec_percentage <= 80) {
        configuredBitrateKbps /= 100.f / (100 - config::stream.fec_percentage);
      }

      // Adjust the bitrate to account for audio traffic bandwidth usage (capped at 20% reduction).
      // The bitrate per channel is 256 Kbps for high quality mode and 96 Kbps for normal quality.
      auto audioBitrateAdjustment = (config.audio.flags[audio::config_t::HIGH_QUALITY] ? 256 : 96) * config.audio.channels;
      configuredBitrateKbps -= std::min((std::int64_t) audioBitrateAdjustment, configuredBitrateKbps / 5);

      // Reduce it by another 500Kbps to account for A/V packet overhead and control data
      // traffic (capped at 10% reduction).
      configuredBitrateKbps -= std::min((std::int64_t) 500, configuredBitrateKbps / 10);

      BOOST_LOG(debug) << "Final adjusted video encoding bitrate is "sv << configuredBitrateKbps << " Kbps"sv;
      config.monitor.bitrate = (int) configuredBitrateKbps;
    }

    if (config.monitor.videoFormat == 1 && video::active_hevc_mode == 1) {
      BOOST_LOG(warning) << "HEVC is disabled, yet the client requested HEVC"sv;

      respond(sock, session, &option, 400, "BAD REQUEST", req->sequenceNumber, {});
      return;
    }

    if (config.monitor.videoFormat == 2 && video::active_av1_mode == 1) {
      BOOST_LOG(warning) << "AV1 is disabled, yet the client requested AV1"sv;

      respond(sock, session, &option, 400, "BAD REQUEST", req->sequenceNumber, {});
      return;
    }

    // Check that any required encryption is enabled
    auto encryption_mode = net::encryption_mode_for_address(sock.remote_endpoint().address());
    if (encryption_mode == config::ENCRYPTION_MODE_MANDATORY && (config.encryptionFlagsEnabled & (SS_ENC_VIDEO | SS_ENC_AUDIO)) != (SS_ENC_VIDEO | SS_ENC_AUDIO)) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      respond(sock, session, &option, 403, "Forbidden", req->sequenceNumber, {});
      return;
    }

    auto stream_session = stream::session::alloc(config, session, sock.remote_endpoint().address().to_string());
    server->insert(stream_session);

    if (stream::session::start(*stream_session, sock.remote_endpoint().address().to_string())) {
      BOOST_LOG(error) << "Failed to start a streaming session"sv;

      server->remove(stream_session);
      respond(sock, session, &option, 500, "Internal Server Error", req->sequenceNumber, {});
      return;
    }

    respond(sock, session, &option, 200, "OK", req->sequenceNumber, {});
  }

  /**
   * @brief Handle RTSP PLAY requests and start streaming.
   *
   * @param server RTSP server instance handling the request.
   * @param sock Socket used to read or write the protocol message.
   * @param session Active streaming or pairing session for the request.
   * @param req Parsed RTSP request being handled.
   */
  void cmd_play(rtsp_server_t *server, tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    OPTION_ITEM option {};

    // I know these string literals will not be modified
    option.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    option.content = const_cast<char *>(seqn_str.c_str());

    respond(sock, session, &option, 200, "OK", req->sequenceNumber, {});
  }

  /**
   * @brief Register the RTSP command handlers on the server (repeated calls change nothing).
   */
  void register_commands() {
    server.map("OPTIONS"sv, &cmd_option);
    server.map("DESCRIBE"sv, &cmd_describe);
    server.map("SETUP"sv, &cmd_setup);
    server.map("ANNOUNCE"sv, &cmd_announce);
    server.map("PLAY"sv, &cmd_play);
  }

  void start() {
    platf::set_thread_name("rtsp");
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    register_commands();

    int adopt_fd = -1;
    bool adopt_paused = false;
    server.begin_start(adopt_fd, adopt_paused);

    boost::system::error_code ec;
#ifdef __linux__
    if (adopt_fd >= 0) {
      if (server.adopt(adopt_fd, ec)) {
        BOOST_LOG(fatal) << "Couldn't adopt the RTSP listener, "sv << ec.message();
        shutdown_event->raise(true);

        return;
      }
    } else
#endif
    {
      if (server.bind(net::af_from_enum_string(config::sunshine.address_family), net::map_port(rtsp_stream::RTSP_SETUP_PORT), ec)) {
        BOOST_LOG(fatal) << "Couldn't bind RTSP server to port ["sv << net::map_port(rtsp_stream::RTSP_SETUP_PORT) << "], " << ec.message();
        shutdown_event->raise(true);

        return;
      }
    }

    server.begin_listening(adopt_fd >= 0 && adopt_paused);

    std::jthread rtsp_thread {[&shutdown_event] {
      platf::set_thread_name("rtsp::handler");
      auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);

      while (!shutdown_event->peek()) {
        server.iterate();

        if (broadcast_shutdown_event->peek()) {
          server.clear(true, to_stop_reason(handoff::hooks::shutdown_reason()));
        } else {
          // cleanup all stopped sessions
          server.clear(false);
        }
      }

      server.clear(true, to_stop_reason(handoff::hooks::shutdown_reason()));
    }};

    // Wait for shutdown
    shutdown_event->view();

    // Stop the server and join the server thread
    server.stop();
    rtsp_thread.join();
  }

#ifdef SUNSHINE_TESTS
  namespace testing {
    bool open_listener(int fd, bool paused) {
      register_commands();
      return server.test_open_listener(fd, paused);
    }

    void start_loop() {
      server.test_start_loop();
    }

    void stop_loop() {
      server.test_stop_loop();
    }

    void reset_server() {
      server.test_reset();
    }

    std::uint64_t loop_iterations() {
      return server.loop_iterations();
    }

    std::uint64_t accepted_connections() {
      return server.accepted_connections();
    }

    std::uint64_t accept_errors() {
      return server.accept_errors();
    }

    void inject_accept_error(const boost::system::error_code &ec) {
      server.test_inject_accept_error(ec);
    }

    int outstanding_accepts() {
      return server.outstanding_accepts();
    }
  }  // namespace testing
#endif

  /**
   * @brief Write msg details to the log.
   */
  void print_msg(PRTSP_MESSAGE msg) {
    std::string_view type = msg->type == TYPE_RESPONSE ? "RESPONSE"sv : "REQUEST"sv;

    std::string_view payload {msg->payload, (size_t) msg->payloadLength};
    std::string_view protocol {msg->protocol};
    auto seqnm = msg->sequenceNumber;
    std::string_view messageBuffer {msg->messageBuffer};

    BOOST_LOG(debug) << "type ["sv << type << ']';
    BOOST_LOG(debug) << "sequence number ["sv << seqnm << ']';
    BOOST_LOG(debug) << "protocol :: "sv << protocol;
    BOOST_LOG(debug) << "payload :: "sv << payload;

    if (msg->type == TYPE_RESPONSE) {
      auto &resp = msg->message.response;

      auto statuscode = resp.statusCode;
      std::string_view status {resp.statusString};

      BOOST_LOG(debug) << "statuscode :: "sv << statuscode;
      BOOST_LOG(debug) << "status :: "sv << status;
    } else {
      auto &req = msg->message.request;

      std::string_view command {req.command};
      std::string_view target {req.target};

      BOOST_LOG(debug) << "command :: "sv << command;
      BOOST_LOG(debug) << "target :: "sv << target;
    }

    for (auto option = msg->options; option != nullptr; option = option->next) {
      std::string_view content {option->content};
      std::string_view name {option->option};

      BOOST_LOG(debug) << name << " :: "sv << content;
    }

    BOOST_LOG(debug) << "---Begin MessageBuffer---"sv << std::endl
                     << messageBuffer << std::endl
                     << "---End MessageBuffer---"sv << std::endl;
  }
}  // namespace rtsp_stream
