// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/request.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>

#include <univalue.h>

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
//! Any value the setters and the parser can make, including what a failed parse leaves behind.
UniValue ConsumeValue(FuzzedDataProvider& provider, int depth)
{
    UniValue value;
    switch (provider.ConsumeIntegralInRange<int>(0, depth > 0 ? 10 : 8)) {
    case 0:
        break;
    case 1:
        value.setBool(provider.ConsumeBool());
        break;
    case 2:
        value = UniValue{UniValue::VBOOL, provider.ConsumeRandomLengthString(4)};
        break;
    case 3:
        value = UniValue{provider.ConsumeIntegral<int64_t>()};
        break;
    case 4:
        value = UniValue{provider.ConsumeIntegral<uint64_t>()};
        break;
    case 5:
        value = UniValue{provider.ConsumeFloatingPoint<double>()};
        break;
    case 6:
        value = UniValue{UniValue::VNUM, provider.ConsumeRandomLengthString(32)};
        break;
    case 7:
        value.setStr(provider.ConsumeRandomLengthString(256));
        break;
    case 8:
        (void)value.read(provider.ConsumeRandomLengthString(256));
        break;
    case 9:
        value.setArray();
        for (int n = provider.ConsumeIntegralInRange<int>(0, 8); n > 0; --n) {
            value.push_back(ConsumeValue(provider, depth - 1));
        }
        break;
    case 10:
        value.setObject();
        for (int n = provider.ConsumeIntegralInRange<int>(0, 8); n > 0; --n) {
            const std::string key = provider.ConsumeRandomLengthString(32);
            if (provider.ConsumeBool()) {
                value.pushKV(key, ConsumeValue(provider, depth - 1));
            } else {
                value.__pushKV(key, ConsumeValue(provider, depth - 1));
            }
        }
        break;
    }
    return value;
}

//! The reply as UniValue::write() writes the reply object, or the error that writing it raises.
std::string ByObject(const UniValue& result, const UniValue& error, const UniValue& id)
{
    try {
        return JSONRPCReplyObj(result, error, id).write() + "\n";
    } catch (const std::out_of_range& e) {
        return std::string{"out_of_range: "} + e.what();
    }
}

//! The reply as JSONRPCReply() writes it, or the error that writing it raises.
std::string ByWriter(const UniValue& result, const UniValue& error, const UniValue& id)
{
    try {
        return JSONRPCReply(result, error, id);
    } catch (const std::out_of_range& e) {
        return std::string{"out_of_range: "} + e.what();
    }
}
} // namespace

void test_one_input(const std::vector<uint8_t>& buffer)
{
    // The input as JSON text, complete or not.
    UniValue parsed;
    (void)parsed.read(std::string{buffer.begin(), buffer.end()});
    assert(ByWriter(parsed, NullUniValue, NullUniValue) == ByObject(parsed, NullUniValue, NullUniValue));

    // The input as the description of a result, an error and an id.
    FuzzedDataProvider provider{buffer.data(), buffer.size()};
    const UniValue result = ConsumeValue(provider, 3);
    const UniValue error = provider.ConsumeBool() ? NullUniValue : ConsumeValue(provider, 2);
    const UniValue id = ConsumeValue(provider, 2);
    assert(ByWriter(result, error, id) == ByObject(result, error, id));
    assert(ByWriter(parsed, NullUniValue, result) == ByObject(parsed, NullUniValue, result));

    // Two replies appended as a batch appends them, against an array of reply objects.
    UniValue replies{UniValue::VARR};
    replies.push_back(JSONRPCReplyObj(result, error, id));
    replies.push_back(JSONRPCReplyObj(parsed, NullUniValue, result));
    std::string expected;
    try {
        expected = replies.write() + "\n";
    } catch (const std::out_of_range&) {
        return; // compared above: both writers raise the same error for each reply
    }
    std::string batch{"["};
    JSONRPCAppendReply(batch, result, error, id);
    batch += ',';
    JSONRPCAppendReply(batch, parsed, NullUniValue, result);
    batch += "]\n";
    assert(batch == expected);
}
