//
// Created by yyz on 2020/1/15.
//

#include <benchmark/benchmark.h>
#include <ecv/strings.h>

#include "amrpc.h"

using namespace std;
using namespace amrpc;

static void BM_IPC_RPC(benchmark::State& state) {
    constexpr static string_view SERVER_ADDRESS = "ipc://bm.ipc";
    constexpr static string_view METHOD = "/bm_rpc";
    Server server(SERVER_ADDRESS);
    server.AddRpc<string(string)>(METHOD, [](string&& data) {
        return string();
    });

    auto data_size = state.range(0);
    auto data = ecv::RandomString(data_size);
    RemoteFunction<string(string)> func(SERVER_ADDRESS, METHOD);
    while (func.Enabled().wait().hasException()) /*wait for server ready*/;
    for (auto _ : state) {
        state.PauseTiming();
        auto cp_data = data;
        state.ResumeTiming();
        func(move(cp_data)).get();
    }

    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * data_size);
}

BENCHMARK(BM_IPC_RPC)->Range(1 << 10, 1 << 10 << 10)->UseRealTime();