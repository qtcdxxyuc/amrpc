#ifndef AMRPC_CONVERSION_H
#define AMRPC_CONVERSION_H

#include <string_view>

namespace amrpc::util {
std::string Msgpack2Json(std::string_view);

std::string Json2Msgpack(std::string_view);
}//amrpc::util

#endif //AMRPC_CONVERSION_H
