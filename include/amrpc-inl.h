#ifndef AMRPC_AMRPC_INL_H
#define AMRPC_AMRPC_INL_H

#include <folly/futures/Future.h>
#include <folly/dynamic.h>
#include <folly/json.h>
#include <msgpack.hpp>
#include "amrpc.h"

/////////////////////////////////////////////////////////
// AMRPC Msg Define & Copy & Move
/////////////////////////////////////////////////////////

#define AMRPC_DEFINE(...)                                               \
    std::shared_ptr<msgpack::object_handle> amrpc_oh;                   \
    void amrpc_msg_tag(){ /*amrpc_msg_tag*/ }                           \
    MSGPACK_DEFINE_MAP(__VA_ARGS__);

#define AMRPC_MOVE(MSG, KEY)                                            \
({                                                                      \
if constexpr (amrpc::detail::is_amrpc_msg<decltype(MSG)>::value){       \
if constexpr (amrpc::detail::is_amrpc_msg<decltype(MSG.KEY)>::value)    \
    MSG.KEY.amrpc_oh = MSG.amrpc_oh;                                    \
}                                                                       \
    std::move(MSG.KEY);                                                 \
})

#define AMRPC_COPY(MSG, KEY)                                            \
({                                                                      \
if constexpr (amrpc::detail::is_amrpc_msg<decltype(MSG)>::value){       \
if constexpr (amrpc::detail::is_amrpc_msg<decltype(MSG.KEY)>::value)    \
    MSG.KEY.amrpc_oh = MSG.amrpc_oh;                                    \
}                                                                       \
    MSG.KEY;                                                            \
})

namespace amrpc {

class Bytes : public std::string {
public:
    Bytes() : std::string() {}

    Bytes(const char* data, size_t size) : std::string(data, size) {}

    explicit Bytes(std::string&& data) : std::string(data) {}

    explicit Bytes(std::string_view view) : std::string(view) {}
};

class BytesView : public std::string_view {
public:
    BytesView() : std::string_view() {}

    BytesView(const char* data, size_t size) : std::string_view(data, size) {}

    explicit BytesView(std::string_view view) : std::string_view(view) {}

    explicit BytesView(Bytes bytes) : std::string_view(bytes.data(), bytes.size()) {}
};

namespace detail {

template<class T, class = void>
struct is_amrpc_msg : std::false_type {
};

template<class T>
struct is_amrpc_msg<T, std::void_t<decltype(std::declval<T&>().amrpc_msg_tag())>> : std::true_type {
};

inline bool MsgpackUnpackRef(msgpack::type::object_type, std::size_t, void*) {
    return true;
}

template<typename, typename = std::void_t<> >
struct get_type {
    using type = void;
};

template<typename T>
struct get_type<T, std::void_t<typename T::type>> {
    using type = typename T::type;
};

template<typename T>
using get_type_t = typename get_type<T>::type;

template<typename R, typename... Args>
struct DescriptionTraitImpl {
    using FuncType = void;
    using ArgsType = void;
    using RetType = void;

    static std::string GetMethodName(const std::string& method) { return "UNKNOWN_FUNC"; }

    template<typename Callback>
    struct is_legal_callback : std::false_type {
    };
};

template<typename R, typename... Args>
struct DescriptionTraitImpl<R(Args...)> {
    using FuncType = std::function<folly::SemiFuture<R>(Args...)>;
    using ArgsType = std::tuple<Args...>;
    using RetType = R;

    static std::string GetMethodName(const std::string& method) {
        auto name = folly::demangle(typeid(std::function<R(Args...)>)).substr(14);
        name.pop_back();
        name.insert(name.find('(') - 1, " [" + method + "]");
        return name.toStdString();
    }

    template<typename T>
    using is_legal_ret = std::integral_constant<bool,
        std::is_same_v<T, folly::SemiFuture<R>> || std::is_same_v<T, folly::Future<R>> || std::is_same_v<T, R>>;

    template<typename T>
    inline static constexpr bool is_legal_ret_v = is_legal_ret<T>::value;

    template<typename Callback>
    struct is_legal_callback : std::integral_constant<bool,
        std::is_invocable_r_v<folly::SemiFuture<R>, Callback, Args...> &&
        std::is_convertible_v<Callback, FuncType> &&
        is_legal_ret_v<get_type_t<typename std::invoke_result<Callback, Args...>>>> {
    };
};

template<typename Desc>
struct DescriptionTrait {
    using Func = typename DescriptionTraitImpl<Desc>::FuncType;
    using Args = typename DescriptionTraitImpl<Desc>::ArgsType;
    using Ret = typename DescriptionTraitImpl<Desc>::RetType;

    static auto GetMethodName(std::string_view method) {
        return DescriptionTraitImpl<Desc>::GetMethodName(std::string(method));
    }

    //Check callback matches the description
    //If assertion fails here,
    //check return value and parameters.
    template<typename Callback>
    using is_legal_callback = typename DescriptionTraitImpl<Desc>::template is_legal_callback<Callback>;

};

}//detail

/////////////////////////////////////////////////////////
// RemoteFunction
/////////////////////////////////////////////////////////
template<typename R, typename... Args>
folly::SemiFuture<R> RemoteFunction<R(Args...)>::operator()(Args&& ... args) const {
    auto buffer = std::make_shared<msgpack::sbuffer>();
    msgpack::pack(*buffer, std::make_tuple(std::forward<Args>(args)...));
    return folly::makeSemiFutureWith([this, buffer]() {
        return RawCall(detail::MessageType::MSGPACK, std::string_view{buffer->data(), buffer->size()});
    })
        .via(&detail::GetAmrpcExecutor())
        .thenValue([buffer](std::string&& raw) -> R {
            auto oh = std::make_shared<msgpack::object_handle>();
            msgpack::unpack(*oh, raw.data(), raw.size(), detail::MsgpackUnpackRef);
            R ret = oh->get().as<R>();
            if constexpr (amrpc::detail::is_amrpc_msg<R>::value) ret.amrpc_oh = oh;
            return ret;
        }).semi();
}

template<>
inline folly::SemiFuture<std::string> RemoteFunction<std::string(std::string)>::operator()(std::string&& args) const {
    return RawCall(detail::MessageType::TEXT, args);
}

template<>
inline folly::SemiFuture<Bytes> RemoteFunction<Bytes(BytesView)>::operator()(BytesView&& args) const {
    return RawCall(detail::MessageType::BIN, args).via(&detail::GetAmrpcExecutor()).thenValue([](std::string&& raw) {
        return Bytes(std::move(raw));
    }).semi();
}

template<>
inline folly::SemiFuture<Bytes> RemoteFunction<Bytes(Bytes)>::operator()(Bytes&& args) const {
    return RawCall(detail::MessageType::BIN, args).via(&detail::GetAmrpcExecutor()).thenValue([](std::string&& raw) {
        return Bytes(std::move(raw));
    }).semi();
}

/////////////////////////////////////////////////////////
// Pull
/////////////////////////////////////////////////////////
template<typename MSG>
folly::SemiFuture<detail::Puller>
Pull(std::string_view host, std::string_view method, std::function<void(folly::Try<MSG>&&)>&& func) {
    return detail::Puller::Create(detail::MessageType::MSGPACK, host, method,
                                  [func{std::move(func)}](folly::Try<std::string>&& raw_try) {
                                      folly::makeSemiFuture(move(raw_try)).deferValue([](std::string&& raw) {
                                          auto oh = std::make_shared<msgpack::object_handle>();
                                          msgpack::unpack(*oh, raw.data(), raw.size(), detail::MsgpackUnpackRef);
                                          auto ret = oh->get().as<MSG>();
                                          if constexpr (amrpc::detail::is_amrpc_msg<MSG>::value) ret.amrpc_oh = oh;
                                          return ret;
                                      }).defer([&func](folly::Try<MSG>&& try_msg) {
                                          func(std::move(try_msg));
                                      }).get();
                                  });
}

template<>
inline folly::SemiFuture<detail::Puller>
Pull<std::string>(std::string_view host, std::string_view method,
                  std::function<void(folly::Try<std::string>&&)>&& func) {
    return detail::Puller::Create(detail::MessageType::TEXT, host, method, std::move(func));
}

template<>
inline folly::SemiFuture<detail::Puller>
Pull<Bytes>(std::string_view host, std::string_view method, std::function<void(folly::Try<Bytes>&&)>&& func) {
    return detail::Puller::Create(detail::MessageType::BIN, host, method,
                                  [f{std::move(func)}](folly::Try<std::string>&& raw_try) {
                                      folly::makeSemiFuture(std::move(raw_try)).deferValue([](std::string&& raw) {
                                          return Bytes(std::move(raw));
                                      }).defer([&](folly::Try<Bytes>&& b_try) {
                                          f(std::move(b_try));
                                      }).get();
                                  });
}

/////////////////////////////////////////////////////////
// AddRpc
/////////////////////////////////////////////////////////
template<typename Description, typename Callback>
void Server::AddRpc(std::string_view m, Callback&& cb) {
    using namespace std;
    using Type = detail::MessageType;
    using Trait = detail::DescriptionTrait<Description>;
    using Args = typename Trait::Args;
    using Ret = typename Trait::Ret;
    static_assert(Trait::template is_legal_callback<Callback>::value, "Check your callback args and return");
    typename Trait::Func func(forward<Callback>(cb));

    if constexpr (is_same_v<string, Ret> && (is_same_v<tuple<string_view>, Args> || is_same_v<tuple<string>, Args>)) {
        //string(string)
        AddRawRpc(Type::TEXT, m, Trait::GetMethodName(m), move(func));
    } else if constexpr (is_same_v<folly::dynamic, Ret> && is_same_v<tuple<folly::dynamic>, Args>) {
        //dynamic(dynamic)
        AddRawRpc(Type::TEXT, m, Trait::GetMethodName(m), [func{move(func)}](string&& raw) {
            return func(folly::parseJson(raw)).deferValue([](folly::dynamic&& res) -> string {
                return folly::toJson(res);
            }).via(&detail::GetAmrpcExecutor()).semi();
        });
    } else if constexpr (is_same_v<Bytes, Ret> &&
                         (is_same_v<tuple<Bytes>, Args> || is_same_v<tuple<BytesView>, Args>)) {
        //Bytes(Bytes)
        AddRawRpc(Type::BIN, m, Trait::GetMethodName(m), [func{move(func)}](string&& raw) {
            return func(Bytes(move(raw))).deferValue([](Bytes&& bytes) -> string {
                return move(bytes);
            }).via(&detail::GetAmrpcExecutor()).semi();
        });
    } else {
        //msgpack(msgpack)
        AddRawRpc(Type::MSGPACK, m, Trait::GetMethodName(m), [this, func{move(func)}](string&& raw) mutable {
            return folly::makeSemiFutureWith([raw{move(raw)}, &func]() {
                msgpack::object_handle oh;
                optional<typename Trait::Args> args;
                try {
                    msgpack::unpack(oh, raw.data(), raw.size());
                    args = oh.get().as<typename Trait::Args>();
                } catch (exception& e) {
                    throw Exception(string("bad rpc request: ") + e.what());
                }
                return apply(func, move(args).value());
            }).deferValue([](const typename Trait::Ret& ret) {
                    msgpack::sbuffer buffer;
                    msgpack::pack(buffer, ret);
                    auto size = buffer.size();
                    return string(buffer.release(), size);
                })
                .via(&detail::GetAmrpcExecutor()).semi();
        });
    }
}

/////////////////////////////////////////////////////////
// AddPublish
/////////////////////////////////////////////////////////
template<typename Msg>
inline void Server::AddPublish(std::string_view method, unsigned int queue_size) {
    using Trait = detail::DescriptionTrait<Msg(void)>;
    AddRawPublish(detail::MessageType::MSGPACK, method, Trait::GetMethodName(method), queue_size);
}

template<>
inline void Server::AddPublish<std::string>(std::string_view method, unsigned int queue_size) {
    using Trait = detail::DescriptionTrait<std::string(void)>;
    AddRawPublish(detail::MessageType::TEXT, method, Trait::GetMethodName(method), queue_size);
}

template<>
inline void Server::AddPublish<folly::dynamic>(std::string_view method, unsigned int queue_size) {
    using Trait = detail::DescriptionTrait<std::string(void)>;
    AddRawPublish(detail::MessageType::TEXT, method, Trait::GetMethodName(method), queue_size);
}

template<>
inline void Server::AddPublish<Bytes>(std::string_view method, unsigned int queue_size) {
    using Trait = detail::DescriptionTrait<Bytes(void)>;
    AddRawPublish(detail::MessageType::BIN, method, Trait::GetMethodName(method), queue_size);
}

/////////////////////////////////////////////////////////
// Publish
/////////////////////////////////////////////////////////
template<typename Msg>
void Server::Publish(std::string_view method, const Msg& msg) {
    msgpack::sbuffer buffer;
    msgpack::pack(buffer, msg);
    auto size = buffer.size();
    std::string data(buffer.release(), size);
    RawPublish(detail::MessageType::MSGPACK, method, std::move(data));
}

template<>
inline void Server::Publish<folly::dynamic>(std::string_view method, const folly::dynamic& msg) {
    RawPublish(detail::MessageType::TEXT, method, folly::toJson(msg));
}

template<>
inline void Server::Publish<std::string>(std::string_view method, const std::string& msg) {
    RawPublish(detail::MessageType::TEXT, method, std::string(msg));
}

template<>
inline void Server::Publish<Bytes>(std::string_view method, const Bytes& msg) {
    RawPublish(detail::MessageType::BIN, method, std::string(msg));
}

template<typename Msg>
void Server::Publish(std::string_view method, Msg&& msg) {
    msgpack::sbuffer buffer;
    msgpack::pack(buffer, msg);
    auto size = buffer.size();
    std::string data(buffer.release(), size);
    RawPublish(detail::MessageType::MSGPACK, method, std::move(data));
}

template<>
inline void Server::Publish<folly::dynamic>(std::string_view method, folly::dynamic&& msg) {
    RawPublish(detail::MessageType::TEXT, method, folly::toJson(msg));
}

template<>
inline void Server::Publish<std::string>(std::string_view method, std::string&& msg) {
    RawPublish(detail::MessageType::TEXT, method, std::move(msg));
}

template<>
inline void Server::Publish<Bytes>(std::string_view method, Bytes&& msg) {
    RawPublish(detail::MessageType::BIN, method, std::move(msg));
}
}//amrpc

/////////////////////////////////////////////////////////
// Msgpack Define Bytes & BytesView
/////////////////////////////////////////////////////////
namespace msgpack {
MSGPACK_API_VERSION_NAMESPACE(MSGPACK_DEFAULT_API_NS) {
namespace adaptor {

template<>
struct convert<amrpc::Bytes> {
    msgpack::object const& operator()(msgpack::object const& o, amrpc::Bytes& v) const {
        if (o.type != msgpack::type::BIN) throw msgpack::type_error();
        v = amrpc::Bytes(std::string_view(o.via.str.ptr, o.via.str.size));
        return o;
    }
};

template<>
struct pack<amrpc::Bytes> {
    template<typename Stream>
    packer<Stream>& operator()(msgpack::packer<Stream>& o, amrpc::Bytes const& v) const {
        o.pack_bin(v.size());
        o.pack_bin_body(v.data(), v.size());
        return o;
    }
};

template<>
struct object_with_zone<amrpc::Bytes> {
    void operator()(msgpack::object::with_zone& o, amrpc::Bytes const& v) const {
        o.type = type::BIN;
        o.via.bin.ptr = v.data();
        o.via.bin.size = v.size();
    }
};

template<>
struct convert<amrpc::BytesView> {
    msgpack::object const& operator()(msgpack::object const& o, amrpc::BytesView& v) const {
        if (o.type != msgpack::type::BIN) throw msgpack::type_error();
        v = amrpc::BytesView(o.via.str.ptr, o.via.str.size);
        return o;
    }
};

template<>
struct pack<amrpc::BytesView> {
    template<typename Stream>
    packer<Stream>& operator()(msgpack::packer<Stream>& o, amrpc::BytesView const& v) const {
        o.pack_bin(v.size());
        o.pack_bin_body(v.data());
        return o;
    }
};

template<>
struct object_with_zone<amrpc::BytesView> {
    void operator()(msgpack::object::with_zone& o, amrpc::BytesView const& v) const {
        o.type = type::BIN;
        o.via.bin.ptr = v.data();
        o.via.bin.size = v.size();
    }
};

} // namespace adaptor
} // MSGPACK_API_VERSION_NAMESPACE(MSGPACK_DEFAULT_API_NS)
} // namespace msgpack
#endif //AMRPC_AMRPC_INL_H
