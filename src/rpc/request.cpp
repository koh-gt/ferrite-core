// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/request.h>

#include <fs.h>

#include <random.h>
#include <rpc/protocol.h>
#include <util/system.h>
#include <util/strencodings.h>

/**
 * JSON-RPC protocol.  Bitcoin speaks version 1.0 for maximum compatibility,
 * but uses JSON-RPC 1.1/2.0 standards for parts of the 1.0 standard that were
 * unspecified (HTTP errors and contents of 'error').
 *
 * 1.0 spec: http://json-rpc.org/wiki/specification
 * 1.2 spec: http://jsonrpc.org/historical/json-rpc-over-http.html
 */

UniValue JSONRPCRequestObj(const std::string& strMethod, const UniValue& params, const UniValue& id)
{
    UniValue request(UniValue::VOBJ);
    request.pushKV("method", strMethod);
    request.pushKV("params", params);
    request.pushKV("id", id);
    return request;
}

UniValue JSONRPCReplyObj(const UniValue& result, const UniValue& error, const UniValue& id)
{
    UniValue reply(UniValue::VOBJ);
    if (!error.isNull())
        reply.pushKV("result", NullUniValue);
    else
        reply.pushKV("result", result);
    reply.pushKV("error", error);
    reply.pushKV("id", id);
    return reply;
}

namespace {
/** Append s to out escaped as UniValue::write() escapes the text of a string. */
void AppendEscaped(std::string& out, const std::string& s)
{
    size_t begin = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = s[i];
        if (c >= 0x20 && c != '"' && c != '\\' && c != 0x7f) continue;
        out.append(s, begin, i - begin);
        begin = i + 1;
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\f': out += "\\f"; break;
        case '\r': out += "\\r"; break;
        default:
            out += "\\u00";
            out += "0123456789abcdef"[c >> 4];
            out += "0123456789abcdef"[c & 0xf];
        }
    }
    out.append(s, begin, std::string::npos);
}

/** Append v to out exactly as v.write() writes it, without the copies that write() makes. */
void AppendJSON(std::string& out, const UniValue& v)
{
    switch (v.getType()) {
    case UniValue::VNULL: out += "null"; break;
    case UniValue::VBOOL: out += v.isTrue() ? "true" : "false"; break;
    case UniValue::VNUM: out += v.getValStr(); break;
    case UniValue::VSTR:
        out += '"';
        AppendEscaped(out, v.getValStr());
        out += '"';
        break;
    case UniValue::VARR: {
        const std::vector<UniValue>& values = v.getValues();
        out += '[';
        for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) out += ',';
            AppendJSON(out, values[i]);
        }
        out += ']';
        break;
    }
    case UniValue::VOBJ: {
        // As UniValue::writeObject(): values.at() and the separator taken from the number of values, so
        // that an object which a failed parse left with a key but no value fails in the same way.
        const std::vector<std::string>& keys = v.getKeys();
        const std::vector<UniValue>& values = v.getValues();
        out += '{';
        for (size_t i = 0; i < keys.size(); ++i) {
            out += '"';
            AppendEscaped(out, keys[i]);
            out += "\":";
            AppendJSON(out, values.at(i));
            if (i != values.size() - 1) out += ',';
        }
        out += '}';
        break;
    }
    }
}
} // namespace

void JSONRPCAppendReply(std::string& out, const UniValue& result, const UniValue& error, const UniValue& id)
{
    out += "{\"result\":";
    AppendJSON(out, error.isNull() ? result : NullUniValue);
    out += ",\"error\":";
    AppendJSON(out, error);
    out += ",\"id\":";
    AppendJSON(out, id);
    out += '}';
}

std::string JSONRPCReply(const UniValue& result, const UniValue& error, const UniValue& id)
{
    std::string reply;
    JSONRPCAppendReply(reply, result, error, id);
    reply += '\n';
    return reply;
}

UniValue JSONRPCError(int code, const std::string& message)
{
    UniValue error(UniValue::VOBJ);
    error.pushKV("code", code);
    error.pushKV("message", message);
    return error;
}

/** Username used when cookie authentication is in use (arbitrary, only for
 * recognizability in debugging/logging purposes)
 */
static const std::string COOKIEAUTH_USER = "__cookie__";
/** Default name for auth cookie file */
static const std::string COOKIEAUTH_FILE = ".cookie";

/** Get name of RPC authentication cookie file */
static fs::path GetAuthCookieFile(bool temp=false)
{
    std::string arg = gArgs.GetArg("-rpccookiefile", COOKIEAUTH_FILE);
    if (temp) {
        arg += ".tmp";
    }
    return AbsPathForConfigVal(fs::path(arg));
}

bool GenerateAuthCookie(std::string *cookie_out)
{
    const size_t COOKIE_SIZE = 32;
    unsigned char rand_pwd[COOKIE_SIZE];
    GetRandBytes(rand_pwd, COOKIE_SIZE);
    std::string cookie = COOKIEAUTH_USER + ":" + HexStr(rand_pwd);

    /** the umask determines what permissions are used to create this file -
     * these are set to 077 in init.cpp unless overridden with -sysperms.
     */
    fsbridge::ofstream file;
    fs::path filepath_tmp = GetAuthCookieFile(true);
    file.open(filepath_tmp);
    if (!file.is_open()) {
        LogPrintf("Unable to open cookie authentication file %s for writing\n", filepath_tmp.string());
        return false;
    }
    file << cookie;
    file.close();

    fs::path filepath = GetAuthCookieFile(false);
    if (!RenameOver(filepath_tmp, filepath)) {
        LogPrintf("Unable to rename cookie authentication file %s to %s\n", filepath_tmp.string(), filepath.string());
        return false;
    }
    LogPrintf("Generated RPC authentication cookie %s\n", filepath.string());

    if (cookie_out)
        *cookie_out = cookie;
    return true;
}

bool GetAuthCookie(std::string *cookie_out)
{
    fsbridge::ifstream file;
    std::string cookie;
    fs::path filepath = GetAuthCookieFile();
    file.open(filepath);
    if (!file.is_open())
        return false;
    std::getline(file, cookie);
    file.close();

    if (cookie_out)
        *cookie_out = cookie;
    return true;
}

void DeleteAuthCookie()
{
    try {
        fs::remove(GetAuthCookieFile());
    } catch (const fs::filesystem_error& e) {
        LogPrintf("%s: Unable to remove random auth cookie file: %s\n", __func__, fsbridge::get_filesystem_error_message(e));
    }
}

std::vector<UniValue> JSONRPCProcessBatchReply(const UniValue& in)
{
    if (!in.isArray()) {
        throw std::runtime_error("Batch must be an array");
    }
    const size_t num {in.size()};
    std::vector<UniValue> batch(num);
    for (const UniValue& rec : in.getValues()) {
        if (!rec.isObject()) {
            throw std::runtime_error("Batch member must be an object");
        }
        size_t id = rec["id"].get_int();
        if (id >= num) {
            throw std::runtime_error("Batch member id is larger than batch size");
        }
        batch[id] = rec;
    }
    return batch;
}

void JSONRPCRequest::parse(const UniValue& valRequest)
{
    // Parse request
    if (!valRequest.isObject())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Invalid Request object");
    const UniValue& request = valRequest.get_obj();

    // Parse id now so errors from here on will have the id
    id = find_value(request, "id");

    // Parse method
    UniValue valMethod = find_value(request, "method");
    if (valMethod.isNull())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Missing method");
    if (!valMethod.isStr())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Method must be a string");
    strMethod = valMethod.get_str();
    if (fLogIPs)
        LogPrint(BCLog::RPC, "ThreadRPCServer method=%s user=%s peeraddr=%s\n", SanitizeString(strMethod),
            this->authUser, this->peerAddr);
    else
        LogPrint(BCLog::RPC, "ThreadRPCServer method=%s user=%s\n", SanitizeString(strMethod), this->authUser);

    // Parse params
    UniValue valParams = find_value(request, "params");
    if (valParams.isArray() || valParams.isObject())
        params = valParams;
    else if (valParams.isNull())
        params = UniValue(UniValue::VARR);
    else
        throw JSONRPCError(RPC_INVALID_REQUEST, "Params must be an array or object");
}
