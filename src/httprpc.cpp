// Copyright (c) 2026 The OmniLite Standalone developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <httprpc.h>

#include <logging.h>
#include <random.h>
#include <rpc/protocol.h>
#include <rpc/server.h>
#include <rpcclient.h>
#include <status.h>
#include <util/strencodings.h>

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <event2/thread.h>

#include <boost/algorithm/string/trim.hpp>

#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <vector>

// OmniLite's registration functions (Core's rpc/register.h).
void RegisterOmniDataRetrievalRPCCommands(CRPCTable& tableRPC);
void RegisterOmniPayloadCreationRPCCommands(CRPCTable& tableRPC);
void RegisterOmniRawTransactionRPCCommands(CRPCTable& tableRPC);
void RegisterOmniTransactionCreationRPCCommands(CRPCTable& tableRPC);

const std::set<std::string> PASSTHROUGH_METHODS = {
    "getbestblockhash", "getblock", "getblockchaininfo", "getblockcount", "getblockhash",
    "getblockheader", "getchaintips", "getdifficulty", "getmempoolinfo", "getrawmempool",
    "gettxout", "getrawtransaction", "decoderawtransaction", "decodescript",
    "getnetworkinfo", "validateaddress", "estimatesmartfee",
};

namespace {
std::set<std::string> g_sendingMethods;
} // namespace

const std::set<std::string>& SendingMethods()
{
    return g_sendingMethods;
}

void RegisterOmniRPCCommands()
{
    static bool registered = false;
    if (registered) return;
    registered = true;
    RegisterOmniDataRetrievalRPCCommands(tableRPC);
    RegisterOmniPayloadCreationRPCCommands(tableRPC);
    RegisterOmniRawTransactionRPCCommands(tableRPC);
    const std::vector<std::string> before = tableRPC.listCommands();
    RegisterOmniTransactionCreationRPCCommands(tableRPC); // through the node's wallet, src/nodewallet.cpp
    for (const std::string& name : tableRPC.listCommands()) {
        if (std::find(before.begin(), before.end(), name) == before.end()) g_sendingMethods.insert(name);
    }
    // Listed for help; answered in Execute, also while loading.
    static const CRPCCommand status{"omnilite", "omnilite_getstatus", &omnilite_getstatus, {}};
    tableRPC.appendCommand(status.name, &status);
}

namespace {

/** Username of cookie authentication, as in Core. */
const std::string COOKIEAUTH_USER = "__cookie__";

void WriteReply(evhttp_request* req, int status, const std::string& body)
{
    evkeyvalq* headers = evhttp_request_get_output_headers(req);
    evhttp_add_header(headers, "Content-Type", "application/json");
    evbuffer* out = evbuffer_new();
    evbuffer_add(out, body.data(), body.size());
    evhttp_send_reply(req, status, nullptr, out);
    evbuffer_free(out);
}

/** As Core's httprpc.cpp: the HTTP status follows the error code. */
void JSONErrorReply(evhttp_request* req, const UniValue& objError, const UniValue& id)
{
    int nStatus = HTTP_INTERNAL_SERVER_ERROR;
    int code = find_value(objError, "code").get_int();

    if (code == RPC_INVALID_REQUEST)
        nStatus = HTTP_BAD_REQUEST;
    else if (code == RPC_METHOD_NOT_FOUND)
        nStatus = HTTP_NOT_FOUND;

    WriteReply(req, nStatus, JSONRPCReply(NullUniValue, objError, id));
}

/** A command of the table, or one passed on to the node. */
UniValue Execute(const JSONRPCRequest& jreq, const RPCClient* node, bool send)
{
    if (!send && SendingMethods().count(jreq.strMethod)) {
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, jreq.strMethod + " is switched off (-apisend=0)");
    }
    // The status needs neither the loaded state nor cs_main: it answers while
    // OmniLite loads and while a block is processed.
    if (jreq.strMethod == "omnilite_getstatus") {
        try {
            return omnilite_getstatus(jreq);
        } catch (const std::exception& e) {
            throw JSONRPCError(RPC_MISC_ERROR, e.what());
        }
    }
    if (tableRPC[jreq.strMethod] == nullptr && node != nullptr && PASSTHROUGH_METHODS.count(jreq.strMethod)) {
        std::string status;
        if (RPCIsInWarmup(&status)) throw JSONRPCError(RPC_IN_WARMUP, status);
        try {
            return node->Call(jreq.strMethod, jreq.params);
        } catch (const RPCError& e) {
            throw JSONRPCError(e.code, e.message);
        } catch (const std::exception& e) {
            throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, std::string("litecoind: ") + e.what());
        }
    }
    return tableRPC.execute(jreq);
}

/** As JSONRPCExecOne in Core's rpc/server.cpp, with Execute. */
UniValue ExecOne(JSONRPCRequest jreq, const UniValue& req, const RPCClient* node, bool send)
{
    UniValue rpc_result(UniValue::VOBJ);
    try {
        jreq.parse(req);
        UniValue result = Execute(jreq, node, send);
        rpc_result = JSONRPCReplyObj(result, NullUniValue, jreq.id);
    } catch (const UniValue& objError) {
        rpc_result = JSONRPCReplyObj(NullUniValue, objError, jreq.id);
    } catch (const std::exception& e) {
        rpc_result = JSONRPCReplyObj(NullUniValue, JSONRPCError(RPC_PARSE_ERROR, e.what()), jreq.id);
    }
    return rpc_result;
}

std::string BodyOf(evhttp_request* req)
{
    evbuffer* buf = evhttp_request_get_input_buffer(req);
    size_t size = evbuffer_get_length(buf);
    const char* data = reinterpret_cast<const char*>(evbuffer_pullup(buf, size));
    return data ? std::string(data, size) : std::string();
}

} // namespace

APIServer::APIServer(APIOptions options, const RPCClient* node)
    : m_options(std::move(options)), m_node(node)
{
}

APIServer::~APIServer()
{
    Stop();
}

void APIServer::Start()
{
    if (m_options.password.empty()) {
        // As Core: a fresh cookie each start, readable by the owner only.
        unsigned char rand_pwd[32];
        GetRandBytes(rand_pwd, sizeof(rand_pwd));
        m_userColonPass = COOKIEAUTH_USER + ":" + HexStr(rand_pwd, rand_pwd + sizeof(rand_pwd));
        fs::path tmp = m_options.cookieFile;
        tmp += ".tmp";
        {
            std::ofstream file(tmp, std::ios::trunc);
            if (!file) throw std::runtime_error("cannot write the API cookie file " + tmp.string());
            fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
            file << m_userColonPass;
        }
        fs::rename(tmp, m_options.cookieFile);
        m_cookieWritten = true;
        LogInfo("API cookie written to %s", m_options.cookieFile.string());
    } else {
        m_userColonPass = m_options.user + ":" + m_options.password;
    }

    evthread_use_pthreads();
    m_base = event_base_new();
    if (!m_base) throw std::runtime_error("cannot create event base");
    m_http = evhttp_new(m_base);
    if (!m_http) throw std::runtime_error("cannot create HTTP server");
    evhttp_set_allowed_methods(m_http, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_HEAD | EVHTTP_REQ_PUT | EVHTTP_REQ_DELETE | EVHTTP_REQ_OPTIONS);
    evhttp_set_max_body_size(m_http, 32 * 1024 * 1024); // Core's MAX_SIZE
    evhttp_set_timeout(m_http, 30);                      // Core's -rpcservertimeout
    evhttp_set_gencb(m_http, HandleRequest, this);

    evhttp_bound_socket* sock = evhttp_bind_socket_with_handle(m_http, m_options.bind.c_str(), m_options.port);
    if (!sock) {
        throw std::runtime_error("cannot listen on " + m_options.bind + ":" + std::to_string(m_options.port));
    }
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    getsockname(evhttp_bound_socket_get_fd(sock), reinterpret_cast<sockaddr*>(&addr), &len);
    m_port = addr.ss_family == AF_INET6 ? ntohs(reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port)
                                        : ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);

    m_stop = event_new(m_base, -1, 0, [](evutil_socket_t, short, void* base) { event_base_loopbreak(static_cast<event_base*>(base)); }, m_base);
    StartRPC();
    m_thread = std::thread([this] { event_base_dispatch(m_base); });
    LogInfo("API listening on %s:%d", m_options.bind, m_port);
}

void APIServer::Stop()
{
    InterruptRPC();
    if (m_thread.joinable()) {
        event_active(m_stop, 0, 0);
        m_thread.join();
    }
    if (m_stop) event_free(m_stop);
    if (m_http) evhttp_free(m_http);
    if (m_base) event_base_free(m_base);
    m_stop = nullptr;
    m_http = nullptr;
    m_base = nullptr;
    if (m_cookieWritten) {
        std::error_code ec;
        fs::remove(m_options.cookieFile, ec);
        m_cookieWritten = false;
    }
}

void APIServer::HandleRequest(evhttp_request* req, void* self)
{
    static_cast<APIServer*>(self)->Handle(req);
}

bool APIServer::CheckAuth(evhttp_request* req) const
{
    const char* header = evhttp_find_header(evhttp_request_get_input_headers(req), "Authorization");
    if (!header) return false;
    std::string auth = header;
    if (auth.substr(0, 6) != "Basic ") return false;
    std::string userPass64 = auth.substr(6);
    boost::trim(userPass64);
    std::string userPass = DecodeBase64(userPass64);
    if (userPass.find(':') == std::string::npos) return false;
    return TimingResistantEqual(userPass, m_userColonPass);
}

void APIServer::Handle(evhttp_request* req)
{
    // As Core: "/" and "/wallet/<name>" (the wallet name does not matter here).
    const char* uri = evhttp_request_get_uri(req);
    std::string path = uri ? uri : "";
    if (path != "/" && path.rfind("/wallet/", 0) != 0) {
        evhttp_send_reply(req, HTTP_NOT_FOUND, nullptr, nullptr);
        return;
    }
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        WriteReply(req, HTTP_BAD_METHOD, "JSONRPC server handles only POST requests");
        return;
    }
    if (!CheckAuth(req)) {
        LogWarn("API: incorrect password attempt");
        // Deter brute-forcing, as Core.
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        evhttp_add_header(evhttp_request_get_output_headers(req), "WWW-Authenticate", "Basic realm=\"jsonrpc\"");
        evhttp_send_reply(req, HTTP_UNAUTHORIZED, nullptr, nullptr);
        return;
    }

    JSONRPCRequest jreq;
    jreq.URI = path;
    try {
        UniValue valRequest;
        if (!valRequest.read(BodyOf(req)))
            throw JSONRPCError(RPC_PARSE_ERROR, "Parse error");

        std::string strReply;
        if (valRequest.isObject()) {
            jreq.parse(valRequest);
            UniValue result = Execute(jreq, m_node, m_options.send);
            strReply = JSONRPCReply(result, NullUniValue, jreq.id);
        } else if (valRequest.isArray()) {
            UniValue ret(UniValue::VARR);
            for (size_t i = 0; i < valRequest.size(); ++i) ret.push_back(ExecOne(jreq, valRequest[i], m_node, m_options.send));
            strReply = ret.write() + "\n";
        } else {
            throw JSONRPCError(RPC_PARSE_ERROR, "Top-level object parse error");
        }
        WriteReply(req, HTTP_OK, strReply);
    } catch (const UniValue& objError) {
        JSONErrorReply(req, objError, jreq.id);
    } catch (const std::exception& e) {
        JSONErrorReply(req, JSONRPCError(RPC_PARSE_ERROR, e.what()), jreq.id);
    }
}
