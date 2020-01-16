#include <gtest/gtest.h>
#include <glog/logging.h>

#include <ecv/net.h>
#include "amrpc.h"

using namespace std;
using namespace amrpc;

struct TestMsg {
    int int_num = 1;
    double double_num = 1.0;
    string str = "abcd";
    AMRPC_DEFINE(int_num, double_num, str);
};

constexpr static string_view SERVER_ADDRESS = "ipc://amrpc_test.ipc";

TEST(rpc, msg) {
    constexpr static string_view METHOD = "/test";
    constexpr static string_view RET = "rpc.msg";
    amrpc::Server server(SERVER_ADDRESS);
    server.AddRpc<string(TestMsg)>(METHOD, [](TestMsg&& msg) {
        return string(RET);
    });
    amrpc::RemoteFunction<string(TestMsg)> func(SERVER_ADDRESS, METHOD);
    ASSERT_TRUE(func.Enabled().wait().hasValue());
    TestMsg msg;
    auto res_future = func(move(msg)).wait();
    ASSERT_TRUE(res_future.hasValue());
    auto res = move(res_future).get();
    ASSERT_EQ(res, RET);
}

TEST(rpc, string) {
    constexpr static string_view METHOD = "/test";
    constexpr static string_view RET = "rpc.string";
    amrpc::Server server(SERVER_ADDRESS);
    server.AddRpc<string(string)>(METHOD, [](string&& msg) {
        return move(msg);
    });
    amrpc::RemoteFunction<string(string)> func(SERVER_ADDRESS, METHOD);
    ASSERT_TRUE(func.Enabled().wait().hasValue());
    string msg(RET);
    auto res_future = func(move(msg)).wait();
    ASSERT_TRUE(res_future.hasValue());
    auto res = move(res_future).get();
    ASSERT_EQ(res, RET);
}

TEST(rpc, Bytes) {
    constexpr static string_view METHOD = "/test";
    constexpr static string_view RET = "rpc.bytes";
    amrpc::Server server(SERVER_ADDRESS);
    server.AddRpc<string(Bytes)>(METHOD, [](string&& msg) {
        return string(msg);
    });
    amrpc::RemoteFunction<string(Bytes)> func(SERVER_ADDRESS, METHOD);
    ASSERT_TRUE(func.Enabled().wait().hasValue());
    Bytes msg(RET);
    auto res_future = func(move(msg)).wait();
    ASSERT_TRUE(res_future.hasValue());
    auto res = move(res_future).get();
    ASSERT_EQ(res, RET);
}

TEST(rpc, voidType) {
    constexpr static string_view METHOD = "/test";
    constexpr static string_view RET = "rpc.void";
    amrpc::Server server(SERVER_ADDRESS);
    server.AddRpc<string(void)>(METHOD, []() {
        return string(RET);
    });
    amrpc::RemoteFunction<string(void)> func(SERVER_ADDRESS, METHOD);
    ASSERT_TRUE(func.Enabled().wait().hasValue());
    auto res_future = func().wait();
    ASSERT_TRUE(res_future.hasValue());
    auto res = move(res_future).get();
    ASSERT_EQ(res, RET);
}

TEST(rpc, autoConv) {
    constexpr static string_view METHOD = "/test";
    constexpr static string_view RET = "rpc.autoConv";
    amrpc::Server server(SERVER_ADDRESS);
    server.AddRpc<string(TestMsg)>(METHOD, [](TestMsg&& msg) {
        EXPECT_EQ(msg.int_num, 2);
        EXPECT_EQ(msg.double_num, 2.0);
        EXPECT_EQ(msg.str, "abcde");
        return string(RET);
    });
    ecv::net::Message msg = {};
    msg.headers.emplace(make_pair("content-type", "application/json"));
    msg.headers.emplace(make_pair("accept", "application/json"));
    msg.headers.emplace(make_pair("connection", "close"));
    msg.body = R"([{"int_num":2,"double_num":2.0,"str":"abcde"}])";
    auto res_future = ecv::net::Client::TransactUnary("GET", SERVER_ADDRESS, METHOD, msg).wait();
    ASSERT_TRUE(res_future.hasValue());
    auto res = move(res_future).get();
    ASSERT_EQ(res.status.code, (unsigned int) 200) << res.status.reason;
    ASSERT_EQ(res.body.substr(1, res.body.size() - 2), RET); // result is \"rpc.autoConv\"
}

TEST(rpc, errorConv) {
    constexpr static string_view METHOD = "/test";
    constexpr static string_view RET = "rpc.errorConv";
    amrpc::Server server(SERVER_ADDRESS);
    server.AddRpc<string(TestMsg)>(METHOD, [](TestMsg&& msg) {
        return string(RET);
    });
    ecv::net::Message msg = {};
    msg.headers.emplace(make_pair("content-type", "application/octet-stream"));
    msg.headers.emplace(make_pair("accept", "application/json"));
    msg.headers.emplace(make_pair("connection", "close"));
    msg.body = R"([{"int_num":2,"double_num":2.0,"str":"abcde"}])";
    auto res_future = ecv::net::Client::TransactUnary("GET", SERVER_ADDRESS, METHOD, msg).wait();
    ASSERT_TRUE(res_future.hasValue());
    auto res = move(res_future).get();
    ASSERT_EQ(res.status.code, (unsigned int) 500) << res.status.reason;
}

TEST(publish, msg) {
    constexpr static string_view METHOD = "/test";
    amrpc::Server server(SERVER_ADDRESS);
    server.AddPublish<TestMsg>(METHOD);
    atomic_bool continue_ = {false};
    auto puller_future = amrpc::Pull<TestMsg>(SERVER_ADDRESS, METHOD, [&continue_](folly::Try<TestMsg>&& t) {
        if (t.hasValue()) {
            auto msg = move(t.value());
            EXPECT_EQ(msg.int_num, 2);
            EXPECT_EQ(msg.double_num, 2.0);
            EXPECT_EQ(msg.str, "abcde");
            continue_ = true;
        }
    });
    puller_future.wait();
    ASSERT_TRUE(puller_future.hasValue());
    auto puller = move(puller_future).get();
    TestMsg msg;
    msg.int_num = 2;
    msg.double_num = 2.0;
    msg.str = "abcde";
    server.Publish(METHOD, move(msg));
    while (!continue_) /*wait for callbaack run*/;
}

TEST(publish, string) {
    constexpr static string_view METHOD = "/test";
    constexpr static string_view RET = "publish.string";
    amrpc::Server server(SERVER_ADDRESS);
    server.AddPublish<string>(METHOD);
    atomic_bool continue_ = {false};
    auto puller_future = amrpc::Pull<string>(SERVER_ADDRESS, METHOD, [&continue_](folly::Try<string>&& t) {
        if (t.hasValue()) {
            EXPECT_EQ(t.value(), RET);
            continue_ = true;
        }
    });
    puller_future.wait();
    ASSERT_TRUE(puller_future.hasValue());
    auto puller = move(puller_future).get();
    server.Publish(METHOD, string(RET));
    while (!continue_) /*wait for callbaack run*/;
}

TEST(publish, bytes) {
    constexpr static string_view METHOD = "/test";
    constexpr static string_view RET = "publish.bytes";
    amrpc::Server server(SERVER_ADDRESS);
    server.AddPublish<amrpc::Bytes>(METHOD);
    atomic_bool continue_ = {false};
    auto puller_future = amrpc::Pull<amrpc::Bytes>(SERVER_ADDRESS, METHOD, [&continue_](folly::Try<amrpc::Bytes>&& t) {
        if (t.hasValue()) {
            EXPECT_EQ(t.value(), RET);
            continue_ = true;
        }
    });
    puller_future.wait();
    ASSERT_TRUE(puller_future.hasValue());
    auto puller = move(puller_future).get();
    server.Publish(METHOD, amrpc::Bytes(RET));
    while (!continue_) /*wait for callbaack run*/;
}

TEST(publish, serverClosed) {
    constexpr static string_view METHOD = "/test";
    auto server = make_shared<amrpc::Server>(SERVER_ADDRESS);
    server->AddPublish<amrpc::Bytes>(METHOD);
    atomic_bool continue_ = {false};
    auto puller_future = amrpc::Pull<amrpc::Bytes>(SERVER_ADDRESS, METHOD, [&continue_](folly::Try<amrpc::Bytes>&& t) {
        EXPECT_TRUE(t.hasException());
        continue_ = true;
    });
    puller_future.wait();
    ASSERT_TRUE(puller_future.hasValue());
    auto puller = move(puller_future).get();
    server.reset();
    while (!continue_) /*wait for callbaack run*/;
}
