#ifndef AMRPC_JSON2MSGPACK_HPP
#define AMRPC_JSON2MSGPACK_HPP

#include <folly/json.h>
#include <folly/dynamic.h>
#include <boost/endian/buffers.hpp>

namespace {
template<typename NUM>
void write_number(std::string& data, NUM num) {
    std::array<char, sizeof(NUM)> vec = {};
    std::memcpy(vec.data(), &num, sizeof(NUM));
    if constexpr (boost::endian::order::native == boost::endian::order::little)
        std::reverse(vec.begin(), vec.end());
    data += std::string(vec.data(), vec.size());
}

void ToMsgpack(const folly::dynamic& j, std::string& data) {
    switch (j.type()) {
        case folly::dynamic::Type::NULLT : {
            data.push_back(0xC0);
            break;
        }

        case folly::dynamic::Type::BOOL : {
            data.push_back(j.asBool() ? 0xC3 : 0xC2);
            break;
        }

        case folly::dynamic::Type::INT64 : {
            data.push_back(0xD3);
            write_number(data, j.asInt());
            break;
        }

        case folly::dynamic::Type::DOUBLE: {
            data.push_back(0xCB);
            write_number(data, j.asDouble());
            break;
        }

        case folly::dynamic::Type::STRING: {
            // step 1: write control byte and the string length
            const auto N = j.asString().size();
            if (N <= 31) {
                // fixstr
                write_number(data, static_cast<std::uint8_t>((unsigned) 0xA0 | N));
            } else if (N <= (std::numeric_limits<std::uint8_t>::max)()) {
                // str 8
                data.push_back(0xD9);
                write_number(data, static_cast<std::uint8_t>(N));
            } else if (N <= (std::numeric_limits<std::uint16_t>::max)()) {
                // str 16
                data.push_back(0xDA);
                write_number(data, static_cast<std::uint16_t>(N));
            } else if (N <= (std::numeric_limits<std::uint32_t>::max)()) {
                // str 32
                data.push_back(0xDB);
                write_number(data, static_cast<std::uint32_t>(N));
            }

            // step 2: write the string
            data += j.asString();
            break;
        }

        case folly::dynamic::Type::ARRAY: {
            // step 1: write control byte and the array size
            const auto N = j.size();
            if (N <= 15) {
                // fixarray
                write_number(data, static_cast<std::uint8_t>((unsigned) 0x90 | N));
            } else if (N <= (std::numeric_limits<std::uint16_t>::max)()) {
                // array 16
                data.push_back(0xDC);
                write_number(data, static_cast<std::uint16_t>(N));
            } else if (N <= (std::numeric_limits<std::uint32_t>::max)()) {
                // array 32
                data.push_back(0xDD);
                write_number(data, static_cast<std::uint32_t>(N));
            }

            // step 2: write each element
            for (const auto& el : j) {
                ToMsgpack(el, data);
            }
            break;
        }

        case folly::dynamic::Type::OBJECT: {
            // step 1: write control byte and the object size
            const auto N = j.size();
            if (N <= 15) {
                // fixmap
                write_number(data, static_cast<std::uint8_t>((unsigned) 0x80 | (unsigned) (N & 0xF)));
            } else if (N <= (std::numeric_limits<std::uint16_t>::max)()) {
                // map 16
                data.push_back(0xDE);
                write_number(data, static_cast<std::uint16_t>(N));
            } else if (N <= (std::numeric_limits<std::uint32_t>::max)()) {
                // map 32
                data.push_back(0xDF);
                write_number(data, static_cast<std::uint32_t>(N));
            }

            // step 2: write each element
            for (const auto& el : j.items()) {
                ToMsgpack(el.first, data);
                ToMsgpack(el.second, data);
            }
            break;
        }

        default:
            break;
    }
}

}//namespace

namespace amrpc::util{

}//amrpc::util

#endif //AMRPC_JSON2MSGPACK_HPP
