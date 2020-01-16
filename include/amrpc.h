#ifndef AMRPC_AMRPC_H
#define AMRPC_AMRPC_H

#include <string_view>
#include <functional>
#include <memory>

#include <ecv/ecvdef.h>
#include <ecv/utils.hpp>

namespace folly {
struct Unit;

class Executor;

template<typename T>
class SemiFuture;

template<class T>
class Try;
}//folly

namespace amrpc {

struct Exception : public std::exception {

    explicit Exception(std::string_view info) : detail(info) {}

    [[nodiscard]] const char* what() const noexcept override {
        return detail.c_str();
    }

    std::string detail;
};

namespace detail {

enum MessageType {
    BIN = 0,
    TEXT,
    MSGPACK
};

folly::Executor& GetAmrpcExecutor();

class RawRemoteFunction : ecv::MoveOnly {
public:
    RawRemoteFunction(std::string_view host, std::string_view method);

    virtual ~RawRemoteFunction() = default;

    folly::SemiFuture<folly::Unit> Enabled();

protected:
    [[nodiscard]] folly::SemiFuture<std::string> RawCall(MessageType, std::string_view data) const;

private:
    std::string_view host_;
    std::string_view method_;
};

class Puller : ecv::MoveOnly {
public:
    Puller() noexcept;

    virtual ~Puller() = default;

    Puller(Puller&& rhs) noexcept;

    [[nodiscard]] bool IsOpen() const;

    [[nodiscard]] std::string_view Method() const;

    static folly::SemiFuture<Puller> Create(MessageType, std::string_view host, std::string_view method,
                                            std::function<void(folly::Try<std::string>&&)>&&);

private:
    class Impl;

    std::shared_ptr<Impl> pimpl_;
};

class RawServer : ecv::MoveOnly {
public:
    explicit RawServer(std::string_view uri, bool enable_debug = true);

    virtual ~RawServer() = default;

    void Del(std::string_view method);

    std::size_t GetPullerSize(std::string_view method);

protected:

    void AddRawRpc(MessageType, std::string_view method, std::string_view func_name,
                   std::function<folly::SemiFuture<std::string>(std::string&&)>&&);

    void AddRawPublish(MessageType, std::string_view method, std::string_view func_name, unsigned int queue_size);

    void RawPublish(MessageType, std::string_view method, std::string&& data);

private:
    class Impl;

    std::shared_ptr<Impl> pimpl_;
};

}//detail

/*
 * Simple case
 * amrpc provides 2 types of communication:
 * rpc (remote procedure call) & publish
 * amrpc server:
 *
 * amrpc::Server server("tcp://127.0.0.1:57000");
 * //Registering a rpc like this:
 * server.AddRpc<string(int)>("/rpc", [](int data) {
 *      return std::to_string(data);
 *  });
 * //Registering a publish like this:
 * server.AddPublish<string>("/publish");
 * //Send push like this:
 * server.Publish("/publish","hello world");
 *
 * amrpc client:
 *
 * //Create rpc like this:
 * RemoteFunction<string(int)> func("tcp://127.0.0.1","/rpc");
 * //check rpc status:
 * func.Enabled().wait().hasException();
 * //run rpc like this:
 * func(int(1)).wait();
 *
 * //Create publish like this:
 *  auto puller = Pull<string>("tcp://127.0.0.1:57000","/publish",[](folly::Try<string>&& t){
 *      //do something
 *  });
 *  puller can check the status of the publish or close publish
 */

class Bytes;

class BytesView;

template<typename R, typename...Args>
class RemoteFunction;

template<typename R, typename...Args>
class RemoteFunction<R(Args...)> : public detail::RawRemoteFunction {
public:
    RemoteFunction(const std::string_view& host, const std::string_view& method) noexcept
        : RawRemoteFunction(host, method) {}

    folly::SemiFuture<R> operator()(Args&& ... args) const;
};

template<typename MSG>
folly::SemiFuture<detail::Puller>
Pull(std::string_view host, std::string_view method, std::function<void(folly::Try<MSG>&&)>&&);

class Server : public detail::RawServer {
public:
    explicit Server(std::string_view uri) noexcept;

    template<typename Description, typename Callback>
    void AddRpc(std::string_view m, Callback&&);

    template<typename Msg>
    void AddPublish(std::string_view method, unsigned int queue_size = 10);

    template<typename Msg>
    void Publish(std::string_view method, const Msg& msg);

    template<typename Msg>
    void Publish(std::string_view method, Msg&& msg);
};
}//amrpc

#include "amrpc-inl.h"

#endif //AMRPC_AMRPC_H
