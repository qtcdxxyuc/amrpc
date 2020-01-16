#include "amrpc.h"
#include <boost/container/flat_map.hpp>
#include <folly/fibers/FiberManager.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/system/ThreadName.h>
#include <folly/Expected.h>
#include <folly/Format.h>
#include <glog/logging.h>
#include <ecv/ecvdef.h>
#include <ecv/net.h>
#include <ecv/time_scope_guard.hpp>
#include <ecv/signals.hpp>
#include <ecv/strings.h>

#include "conversion.h"

using namespace std;
using namespace amrpc;
namespace net = ecv::net;
using folly::Expected;
using folly::SemiFuture;
using boost::container::flat_map;

namespace {
////////////////////////////////////////////////////////
// 一些重要的全局变量
////////////////////////////////////////////////////////
constexpr const char* CONTENTTYPE = "content-type";
constexpr const char* ACCEPT = "accept";
constexpr const char* CHECKENABLED = "amrpc_check_enabled";
constexpr const char* SV_MSGPACK = "application/x-msgpack";
constexpr const char* SV_JSON = "application/json";
constexpr const char* SV_TEXT = "text/plain";
constexpr const char* SV_BIN = "application/octet-stream";
constexpr const char* SV_WS_SUBPROTOCOL = "Sec-WebSocket-Protocol";
constexpr const char* SV_SUBPROTOCOL_MSGPACK = "ecv_amrpc_msgpack";
constexpr const char* SV_SUBPROTOCOL_JSON = "ecv_amrpc_json";
constexpr const char* SV_SUBPROTOCOL_TEXT = "ecv_amrpc_text";
constexpr const char* SV_SUBPROTOCOL_BIN = "ecv_amrpc_bin";

////////////////////////////////////////////////////////
// 辅助函数
////////////////////////////////////////////////////////
template<typename T>
T ExceptionWarp(folly::exception_wrapper&& e) {
    throw Exception(e.what().data());
    return T{};
}

inline void DataConvertible(bool ok) {
    if (!ok) throw Exception("Data cannot be converted");
}

template<typename Map>
string find_or(const Map& map, const string& key, string_view defult_value) {
    auto iter = map.find(key);
    if (iter == map.end()) return string(defult_value);
    else return string(iter->second);
}

string Type2String(detail::MessageType type) {
    switch (type) {
        case detail::MessageType::BIN:
            return SV_BIN;
        case detail::MessageType::TEXT:
            return SV_TEXT;
        case detail::MessageType::MSGPACK:
            return SV_MSGPACK;
        default:
            LOG(FATAL) << "unknown type";
    }
    return "";
}

detail::MessageType String2Type(string_view view) {
    if (view == SV_MSGPACK) return detail::MessageType::MSGPACK;
    else if (view == SV_TEXT || view == SV_JSON) return detail::MessageType::TEXT;
    else return detail::MessageType::BIN;
}
}// namespace

////////////////////////////////////////////////////////
// Data
// Msgpack -auto-> Text | Bin
// Text -auto-> Bin
// Bin -not-> Text | Msgpack
////////////////////////////////////////////////////////
class Data {
public:
    using Type = detail::MessageType;

    Data(Type type, string&& data);

    string_view MsgpackView();

    string_view TextTiew();

    string_view BinView();

    string_view View(Type);

    static string Conversion(Type from, Type to, string&&);

private:
    //<convertible,data>
    pair<bool, optional<string>> msgpack_data_ = {true, {}};
    pair<bool, optional<string>> text_data_ = {true, {}};
    string_view bin_data_;
    string bin_buffer_;
};

Data::Data(Type type, string&& data) {
    if (type == Type::MSGPACK) {
        msgpack_data_.second = move(data);
        bin_data_ = msgpack_data_.second.value();
    } else if (type == Type::TEXT) {
        text_data_.second = move(data);
        bin_data_ = text_data_.second.value();
    } else if (type == Type::BIN) {
        msgpack_data_.first = false;
        text_data_.first = false;
        bin_buffer_ = move(data);
        bin_data_ = bin_buffer_;
    } else {
        throw Exception("Data: unknown type");
    }
}

string_view Data::MsgpackView() {
    DataConvertible(msgpack_data_.first);
    if (!msgpack_data_.second.has_value()) {
        try {
            msgpack_data_.second = util::Json2Msgpack(text_data_.second.value());
        } catch (exception& e) {
            msgpack_data_.first = false;
            throw Exception(string("Data cannot be converted: ") + e.what());
        }
    }
    return msgpack_data_.second.value();
}

string_view Data::BinView() {
    return bin_data_;
}

string_view Data::TextTiew() {
    DataConvertible(text_data_.first);
    if (!text_data_.second.has_value()) {
        try {
            text_data_.second = util::Msgpack2Json(msgpack_data_.second.value());
        } catch (exception& e) {
            text_data_.first = false;
            throw Exception(string("Data cannot be converted: ") + e.what());
        }
    }
    return text_data_.second.value();
}

string_view Data::View(Type type) {
    switch (type) {
        case Type::MSGPACK:
            return MsgpackView();
        case Type::TEXT:
            return TextTiew();
        case Type::BIN:
            return BinView();
    }
    LOG(FATAL) << "can not run here";
}

string Data::Conversion(Type from, Type to, string&& bin) {
    if (Type::BIN == to) return move(bin);
    try {
        Data data(from, move(bin));
        data.View(to);
        switch (to) {
            case Type::MSGPACK:
                return move(data.msgpack_data_.second.value());
            case Type::TEXT:
                return move(data.text_data_.second.value());
            default:
                /*do no thing*/;
        }
    } catch (Exception& e) {
        auto str = folly::format("{} -> {} error: {}", Type2String(from), Type2String(to), e.what()).str();
        throw Exception(str);
    }
    LOG(FATAL) << "can not run here";
    return "";
}

////////////////////////////////////////////////////////
// StaticFiberManager
////////////////////////////////////////////////////////
class SFM {
public:
    SFM() : fm_(folly::fibers::getFiberManager(evb_)) {
        evb_thread_ = thread([this]() {
            if (folly::canSetCurrentThreadName())
                folly::setThreadName("amrpc_evb");
            evb_.loopForever();
        });
    }

    virtual ~SFM() {
        evb_.terminateLoopSoon();
        evb_thread_.join();
    }

    static folly::fibers::FiberManager& GetFM() {
        static SFM sfm;
        return sfm.fm_;
    }

private:
    folly::EventBase evb_;
    folly::fibers::FiberManager& fm_;
    thread evb_thread_;
};

folly::Executor& detail::GetAmrpcExecutor() {
    return SFM::GetFM();
}

////////////////////////////////////////////////////////
// RawRemoteFunction
////////////////////////////////////////////////////////
detail::RawRemoteFunction::RawRemoteFunction(string_view host, string_view method) {
    DLOG_ASSERT(!host.empty() && !method.empty());
    host_ = host;
    method_ = method;
}

folly::SemiFuture<folly::Unit> detail::RawRemoteFunction::Enabled() {
    net::Message request = {};
    bool ret;
    ret = request.headers.emplace(make_pair(CHECKENABLED, "1")).second, LOG_ASSERT(ret);
    return net::Client::TransactUnary("GET", host_, method_, request).deferValue([](net::Message&& msg) {
        if (msg.status.code == 404) throw Exception("remote method not found");
    }).via(&SFM::GetFM());
}

folly::SemiFuture<string> detail::RawRemoteFunction::RawCall(MessageType type, string_view data) const {
    net::Message request = {};
    bool ret = false;
    auto type_str = Type2String(type);
    ret = request.headers.emplace(make_pair(CONTENTTYPE, type_str)).second, LOG_ASSERT(ret);
    ret = request.headers.emplace(make_pair(ACCEPT, type_str)).second, LOG_ASSERT(ret);
    ret = request.headers.emplace(make_pair("connection", "close")).second, LOG_ASSERT(ret);
    request.body = data;
    return net::Client::TransactUnary("GET", host_, method_, request).deferValue([](net::Message&& msg) {
        auto code = msg.status.code;
        if (LIKELY(code == 200)) return msg.body;
        else if (code == 404) throw Exception("remote method not found");
        else if (code == 500) throw Exception(string("server error: ") + msg.status.reason);
        else throw Exception(string("unknown status: ") + std::to_string(code));
    }).deferError(ExceptionWarp<string>);
}

////////////////////////////////////////////////////////
// Puller Impl
////////////////////////////////////////////////////////
class amrpc::detail::Puller::Impl {
public:
    virtual ~Impl() noexcept{
        session_.reset();
    }

    unique_ptr<net::Session> session_;
    function<void(folly::Try<string>&&)> handler_;
    struct Info {
        MessageType type_ = {MessageType::BIN};
        string method_;
        string host;
    };
    Info info_;

    static bool RunHandleLoop(const weak_ptr<Impl>& weak);
};

bool detail::Puller::Impl::RunHandleLoop(const weak_ptr<Impl>& weak) {
    optional<folly::SemiFuture<string>> opt_future;
    if (auto handle = weak.lock();LIKELY(handle != nullptr)) {
        if (auto& session = handle->session_;LIKELY(session != nullptr))
            opt_future = session->Read();
    }
    if (UNLIKELY(!opt_future.has_value())) return false;
    auto try_res = move(opt_future.value()).getTry();
    try {
        DTIMEOUT_ASSERT(chrono::milliseconds(50)) << " puller.callback timeout 50ms";
        if (auto handle = weak.lock()) handle->handler_(move(try_res));
        return true;
    } catch (exception& e) {
        VLOG(ECV_VLOG_LEVEL)  << " Puller handle error: " << e.what();
        return false;
    }
}


////////////////////////////////////////////////////////
//Puller
////////////////////////////////////////////////////////
amrpc::detail::Puller::Puller() noexcept {
    pimpl_ = make_shared<Impl>();
}

detail::Puller::Puller(detail::Puller&& rhs) noexcept {
    if (&rhs != this) {
        pimpl_ = move(rhs.pimpl_);
    }
}

std::string_view detail::Puller::Method() const {
    DLOG_ASSERT(pimpl_);
    return pimpl_->info_.method_;
}

bool detail::Puller::IsOpen() const {
    DLOG_ASSERT(pimpl_);
    if (auto& session = pimpl_->session_) return session->IsOpen();
    else return false;
}

SemiFuture<detail::Puller> detail::Puller::Create(detail::MessageType type, string_view host, string_view method,
                                                  function<void(folly::Try<std::string>&&)>&& func) {
    string sub_protocol;
    switch (type) {
        case detail::MessageType::MSGPACK :
            sub_protocol = SV_SUBPROTOCOL_MSGPACK;
            break;
        case detail::MessageType::TEXT :
            sub_protocol = SV_SUBPROTOCOL_TEXT;
            break;
        default:
            sub_protocol = SV_SUBPROTOCOL_BIN;
            break;
    }
    Impl::Info info;
    info.type_ = type;
    info.host = host;
    info.method_ = method;
    return net::Client::TransactStream(host, method, {make_pair(SV_WS_SUBPROTOCOL, sub_protocol)})
        .deferValue([func{move(func)}, info{move(info)}](unique_ptr<net::Session>&& session) mutable {
            Puller puller;
            auto impl = puller.pimpl_;
            impl->info_ = move(info);
            impl->session_ = move(session);
            impl->handler_ = move(func);
            weak_ptr<Puller::Impl> weak = impl;
            SFM::GetFM().add([weak]() {
                while (Impl::RunHandleLoop(weak)) /*do in loop*/;
            });
            return puller;
        }).deferError(ExceptionWarp<detail::Puller>)
        .via(&SFM::GetFM());
}

////////////////////////////////////////////////////////
// Info
////////////////////////////////////////////////////////
struct Info {
    using Type = detail::MessageType;
    Type type;
    string method;
    string func_name;
};

////////////////////////////////////////////////////////
// Distributor
// 负责单个publish的数据分发
////////////////////////////////////////////////////////
class Distributor {
public:
    using Type = detail::MessageType;

    Distributor(Info&& info, unsigned int sz) : info_(move(info)), queue_size_(++sz) {}

    Distributor(Distributor&& rhs) = default;

    ~Distributor() { signal_.DisconnectAll(); }

    void AddClient(Type, std::string&&, unique_ptr<net::Session>&&);

    void Update(Type, string&&);

    std::size_t getPollerSize() { return signal_.Size(); }

    [[nodiscard]] const Info& getInfo() const { return info_; };

private:
    const Info info_;
    const unsigned int queue_size_;
    ecv::SSignal<void(const shared_ptr<Data>&)> signal_;
};

void Distributor::AddClient(Type type, std::string&& peer, unique_ptr<net::Session>&& session) {
    struct Context {
        Type type = Type::BIN;
        std::string peer;
        unsigned int high_watermark = 1;
        std::queue<shared_ptr<Data>> data_queue;
        unique_ptr<net::Session> session;
    };
    auto context = make_shared<Context>();
    context->type = type;
    context->peer = move(peer);
    context->high_watermark = queue_size_;
    context->session = move(session);
    weak_ptr<Context> weak = context;
    auto connection = signal_.Connect([context](shared_ptr<Data> data) mutable {
        LOG_ASSERT(data);
        if (UNLIKELY(context == nullptr)) return;
        context->data_queue.emplace(move(data));
        if (context->high_watermark <= context->data_queue.size()) {
            VLOG(ECV_VLOG_LEVEL) << context->peer <<" overflow high-watermark";
            context.reset();
        }
    });
    auto ctrl = context->session->Read();
    SFM::GetFM().add([weak, c{move(connection)}, ctrl{move(ctrl)}]() mutable {
        folly::fibers::Baton sleep_baton;
        optional<folly::SemiFuture<folly::Unit>> opt_send_future;
        shared_ptr<Data> data;
        while (true) {
            auto context = weak.lock();
            if (UNLIKELY(context == nullptr)) break;
            if (!context->data_queue.empty()) {
                data = context->data_queue.front();
                context->data_queue.pop();
                opt_send_future = context->session->Write(data->View(context->type));
            }
            context.reset();
            if (opt_send_future.has_value()) {
                if (opt_send_future.value().wait().hasException()) break;
                if (ctrl.isReady()) break; //get 'close' or other data
            }
            sleep_baton.try_wait_for(chrono::milliseconds(11)), sleep_baton.reset();
        }
        c.Disconnect();
    });
}

void Distributor::Update(Type type, string&& data) {
    if (signal_.Empty()) return;
    auto s_data = make_shared<Data>(type, move(data));
    if constexpr (DCHECK_IS_ON()) {
        try {
            s_data->View(type);
        } catch (exception& e) {
            LOG(FATAL) << folly::format("can not conversion data {} to {} :{}",
                                        Type2String(type), Type2String(type), e.what());
        }
    }
    signal_(move(s_data));
}

////////////////////////////////////////////////////////
// RawServer Impl
////////////////////////////////////////////////////////
class amrpc::detail::RawServer::Impl {
public:
    using Type = detail::MessageType;

    explicit Impl(string_view uri) : server_(uri) {}

    virtual ~Impl();

    void AddRawRpc(Info&&, function<SemiFuture<string>(string&&)>&&);

    void DelRpc(string_view);

    void AddRawPublish(Info&& info, unsigned int);

    void DelPublish(string_view);

    void RawPublish(MessageType, string_view, string&&);

    std::size_t GetPullerSize(string_view);

    void EnabledDebug();

private:
    flat_map<string, Info> rpc_info_map_;
    flat_map<string, shared_ptr<Distributor>> distributor_map_;
    net::Server server_;
};

detail::RawServer::Impl::~Impl() {
    rpc_info_map_.clear();
    distributor_map_.clear();
}

void detail::RawServer::Impl::EnabledDebug() {
    server_.Add("/debug/reflection", [this](net::Server::ConnectProfile&&, net::Message&&) {
        net::Message msg = {};
        folly::dynamic rpc = folly::dynamic::object();
        folly::dynamic publish = folly::dynamic::object();
        folly::dynamic root = folly::dynamic::object();
        for (const auto& pair : rpc_info_map_) {
            const auto& info = pair.second;
            rpc.insert(info.method, info.func_name);
        }
        for (const auto& pair : distributor_map_) {
            if (auto distributor = pair.second)
                publish.insert(distributor->getInfo().method, distributor->getInfo().func_name);
        }
        root.insert("rpc", rpc);
        root.insert("publish", publish);
        msg.status.code = 200;
        msg.body = folly::toPrettyJson(root);
        return msg;
    });
}

void amrpc::detail::RawServer::Impl::AddRawRpc(Info&& info, function<SemiFuture<string>(string&&)>&& func) {
    auto func_ptr = make_shared<function<SemiFuture<string>(string&&)>>(move(func));
    auto handle_type = info.type;
    rpc_info_map_.insert(make_pair(info.method, info));
    server_.Add(info.method, [handle_type, func_ptr](net::Server::ConnectProfile&&, net::Message&& message) {
        if (message.headers.count(CHECKENABLED)) {
            net::Message res = {};
            res.status.code = 200;
            return folly::makeSemiFuture(move(res));
        }
        auto req_type = String2Type(find_or(message.headers, CONTENTTYPE, ""));
        auto res_type_str = find_or(message.headers, ACCEPT, "");
        return folly::makeSemiFutureWith([&]() {
            return Data::Conversion(req_type, handle_type, move(message.body));
        }).deferValue([func_ptr](string&& req) {
            DTIMEOUT_ASSERT(chrono::milliseconds(50)) << " Rpc.callback timeout 50ms";
            return (*func_ptr)(move(req));
        }).deferValue([handle_type, res_type_str{move(res_type_str)}](string&& res) {
            net::Message msg = {};
            auto ret = msg.headers.emplace(make_pair(CONTENTTYPE, res_type_str)).second;
            LOG_ASSERT(ret);
            msg.status.code = 200;
            msg.body = Data::Conversion(handle_type, String2Type(res_type_str), move(res));
            return msg;
        }).deferError([](folly::exception_wrapper&& e) {
            net::Message msg = {};
            msg.status.code = 500;
            msg.status.reason = e.what().toStdString();
            return msg;
        }).via(&SFM::GetFM()).semi();
    }).get();
}

void amrpc::detail::RawServer::Impl::DelRpc(string_view method) {
    rpc_info_map_.erase(string(method));
    server_.Del(net::Tcp::unary, method).get();
}

void amrpc::detail::RawServer::Impl::AddRawPublish(Info&& info, unsigned int queue_size) {
    if (!distributor_map_.count(info.method)) {
        auto distributor = make_shared<Distributor>(folly::copy(info), queue_size);
        auto ret = distributor_map_.emplace(make_pair(info.method, distributor));
        LOG_ASSERT(ret.second);
        weak_ptr<Distributor> weak = distributor;
        server_.Add(info.method, [weak](net::Server::ConnectProfile&& profile, unique_ptr<net::Session>&& session) {
            LOG_ASSERT(profile.response_headers.has_value());
            auto type = String2Type(find_or(profile.response_headers.value(), ACCEPT, ""));
            auto peer = profile.peer;
            SFM::GetFM().add([type, peer, weak, s{move(session)}]() mutable {
                if (auto publisher = weak.lock()) publisher->AddClient(type, move(peer), move(s));
            });
        }, [](const net::Headers& header) {
            net::Message res = {};
            auto sub_protocol = find_or(header, SV_WS_SUBPROTOCOL, "");
            string accept;
            if (sub_protocol == SV_SUBPROTOCOL_MSGPACK) accept = SV_MSGPACK;
            else if (sub_protocol == SV_SUBPROTOCOL_JSON) accept = SV_JSON;
            else if (sub_protocol == SV_SUBPROTOCOL_TEXT) accept = SV_TEXT;
            else accept = SV_BIN;
            res.status.code = 101;
            res.headers.emplace(make_pair(ACCEPT, accept));
            res.headers.emplace(make_pair(SV_WS_SUBPROTOCOL, sub_protocol));
            return res;
        });
    }
}

void amrpc::detail::RawServer::Impl::DelPublish(string_view method) {
    distributor_map_.erase(string(method));
}

void amrpc::detail::RawServer::Impl::RawPublish(MessageType type, string_view method, string&& data) {
    auto iter = distributor_map_.find(string(method));
    if (UNLIKELY(iter == distributor_map_.end())) throw Exception("publish method not registered");
    iter->second->Update(type, move(data));
}

std::size_t amrpc::detail::RawServer::Impl::GetPullerSize(string_view method) {
    auto iter = distributor_map_.find(string(method));
    if (UNLIKELY(iter == distributor_map_.end())) throw Exception("publish method not registered");
    return iter->second->getPollerSize();
}

////////////////////////////////////////////////////////
// RawServer
////////////////////////////////////////////////////////
amrpc::detail::RawServer::RawServer(std::string_view uri, bool enable_debug) {
    pimpl_ = make_shared<Impl>(uri);
    if (enable_debug) pimpl_->EnabledDebug();
}

void amrpc::detail::RawServer::AddRawRpc(MessageType type, string_view method, string_view func_name,
                                         function<SemiFuture<string>(string&&)>&& func) {
    DLOG_ASSERT(pimpl_);
    SFM::GetFM().addTaskRemoteFuture([&]() {
        Info info = {};
        info.type = type;
        info.method = method;
        info.func_name = func_name;
        pimpl_->AddRawRpc(move(info), move(func));
    }).get();
}

void amrpc::detail::RawServer::AddRawPublish(MessageType type, string_view method, string_view func_name,
                                             unsigned int queue_size) {
    DLOG_ASSERT(pimpl_);
    SFM::GetFM().addTaskRemoteFuture([&]() {
        Info info = {};
        info.type = type;
        info.method = method;
        info.func_name = func_name;
        pimpl_->AddRawPublish(move(info), queue_size);
    }).get();
}

void amrpc::detail::RawServer::RawPublish(MessageType type, string_view method, string&& data) {
    DLOG_ASSERT(pimpl_);
    SFM::GetFM().addTaskRemoteFuture([&]() {
        pimpl_->RawPublish(type, method, move(data));
    }).get();
}

void amrpc::detail::RawServer::Del(string_view method) {
    DLOG_ASSERT(pimpl_);
    SFM::GetFM().addTaskRemoteFuture([&]() {
        pimpl_->DelRpc(method);
        pimpl_->DelPublish(method);
    }).get();
}

std::size_t amrpc::detail::RawServer::GetPullerSize(string_view method) {
    DLOG_ASSERT(pimpl_);
    return SFM::GetFM().addTaskRemoteFuture([&]() {
        return pimpl_->GetPullerSize(method);
    }).get();
}

////////////////////////////////////////////////////////
// Constrator
////////////////////////////////////////////////////////
Server::Server(string_view uri) noexcept: RawServer(uri) {
    // do no thing
}
