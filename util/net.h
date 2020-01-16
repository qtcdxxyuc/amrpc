#ifndef ECV_INCLUDE_ECV_NET_H
#define ECV_INCLUDE_ECV_NET_H

#include "ecv/ecvdef.h"
#include <memory>
#include <functional>
#include <unordered_map>

namespace folly {
template<typename Type>
class SemiFuture;

struct Unit;
} // namespace folly


namespace ecv::net {
/////////////////////////////////////////////////////////
// ECV NET
// @HTTP @WebSocket @ipc
// 2019.12.27 ... by YYZ
/////////////////////////////////////////////////////////

//Scheme
struct Ipc {
  static constexpr std::string_view scheme = "ipc";   //Inter Process Communication
  static constexpr std::string_view unary = "stp";    //Signaling Transfer Protocol
  static constexpr std::string_view stream = "ips";   //Inter Process Socket
};

struct Tcp {
  static constexpr std::string_view scheme = "tcp";   //Transmission Control Protocol
  static constexpr std::string_view unary = "http";   //HyperText Transfer Protocol
  static constexpr std::string_view stream = "ws";    //Web Socket
};

// NET 相关异常
struct Exception : public std::exception {
  explicit Exception(const std::string_view &sv) noexcept : detail(sv) {
  }

  [[nodiscard]] const char *what() const noexcept override {
      return detail.data();
  }

  std::string detail;
};

struct Disconnect : public Exception {
  explicit Disconnect(const std::string_view &sv) noexcept : Exception(sv) {}
};

struct PeerClosed : public Exception {
  explicit PeerClosed(const std::string_view &sv) noexcept : Exception(sv) {}
};

using Headers = std::unordered_map<std::string, std::string>;

//消息类型, 请求回应通用
struct Message {
  std::string body;
  Headers headers;

  struct status {
    unsigned int code = 200;
    std::string reason;
  } status;
};


// 连接实例
// read被调用时,自动处理控制信号.
// close调用后session不再可用,close与write不能并发调用.
// IsOpen在通讯静默期间不会变化.
class ECV_EXPORTS Session {
public:
  virtual ~Session() = default;

  virtual bool IsOpen() = 0;

  virtual folly::SemiFuture<std::string> Read() = 0;

  virtual folly::SemiFuture<folly::Unit> Read(std::string &) = 0;

  virtual folly::SemiFuture<folly::Unit> Write(std::string &&buf) = 0;

  virtual folly::SemiFuture<folly::Unit> Write(std::string_view ref) = 0;

  virtual void Close(std::string_view error) = 0;
};


class ECV_EXPORTS Server {
public:
  struct ConnectProfile {
    std::string peer;
    std::string method;
    std::string uri;
    Headers request_headers;
    std::optional<Headers> response_headers;
  };

  using UnaryListen = std::function<folly::SemiFuture<Message>(ConnectProfile &&, Message &&)>;
  using StreamListen = std::function<void(ConnectProfile &&, std::unique_ptr<Session> &&)>;
  using Accept = std::function<Message(const Headers &)>;

  explicit Server(std::string_view uri, int64_t max_req_body_size = 57 * 1024 * 1024) noexcept;

  folly::SemiFuture<bool> Add(std::string_view path, UnaryListen &&);

  folly::SemiFuture<bool> Add(std::string_view path, StreamListen &&, Accept && = {});

  folly::SemiFuture<bool> Del(std::string_view scheme, std::string_view path);

private:
  class Impl;

  std::shared_ptr<Impl> pimpl_;
};


class ECV_EXPORTS Client {
public:
  static folly::SemiFuture<Message> TransactUnary(
      std::string_view method,
      std::string_view host,
      std::string_view target = {},
      const Message &request = {});

  static folly::SemiFuture<std::unique_ptr<Session>> TransactStream(
      std::string_view host,
      std::string_view target = {},
      const Headers &headers = {});
};

} //namespace ecv::net

#endif //ECV_INCLUDE_ECV_NET_H
