#include "conversion.h"

#include <boost/endian/buffers.hpp>
#include <folly/json.h>
#include <folly/dynamic.h>
#include <ecv/strings.h>
#include <msgpack.hpp>

using namespace std;

namespace detail{

struct MsgPackObjVisitor : public msgpack::object_stringize_visitor {
public:
    explicit MsgPackObjVisitor(ostream& os) : object_stringize_visitor(os), os_(os) {}

    bool visit_bin(const char* v, uint32_t size) {
        auto base64 = ecv::Base64Encode({v, size});
        (os_ << '"').write(base64.data(), static_cast<std::streamsize>(base64.size())) << '"';
        return true;
    }

private:
    std::ostream& os_;
};

template<typename NUM>
void write_number(std::string& data, NUM num) {
    std::array<char, sizeof(NUM)> vec = {};
    std::memcpy(vec.data(), &num, sizeof(NUM));
    if constexpr (boost::endian::order::native == boost::endian::order::little)
        std::reverse(vec.begin(), vec.end());
    data += string(vec.data(), vec.size());
}

void write_msgpack(const folly::dynamic& j, std::string& data) {
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
                write_msgpack(el, data);
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
                write_msgpack(el.first, data);
                write_msgpack(el.second, data);
            }
            break;
        }

        default:
            break;
    }
}

}//detail

namespace amrpc::util {

std::string Msgpack2Json(std::string_view msgpack_bin) {
    auto oh = msgpack::unpack(msgpack_bin.data(), msgpack_bin.size());
    const auto& obj = oh.get();
    stringstream ss;
    detail::MsgPackObjVisitor visitor(ss);
    msgpack::object_parser(obj).parse(visitor);
    return ss.str();
}

std::string Json2Msgpack(std::string_view json_bin) {
    auto json = folly::parseJson(json_bin);
    string msgpack_bin;
    detail::write_msgpack(json, msgpack_bin);
    return msgpack_bin;
}

} //amrpc::util