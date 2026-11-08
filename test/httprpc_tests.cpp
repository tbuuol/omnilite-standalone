// Copyright (c) 2026 The OmniLite Standalone developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The API's HTTP side: login, cookie, request formats, status codes and the
// calls passed on to the node. The omni_* calls on a real state are in
// indexer_tests.

#include <httprpc.h>

#include <chainparams.h>

#include <rpc/server.h>
#include <rpcclient.h>
#include <test/fakelitecoind.h>
#include <test/util.h>

#include <boost/test/unit_test.hpp>

#include <fstream>

namespace {

struct HTTPResult {
    int status = 0;
    std::string body;
};

/** A raw HTTP request to the API, for what RPCClient does not show (status codes, other methods). */
HTTPResult Request(int port, const std::string& body, const std::string& userPass = "u:p",
                   evhttp_cmd_type method = EVHTTP_REQ_POST, const std::string& path = "/")
{
    HTTPResult result;
    event_base* base = event_base_new();
    evhttp_connection* conn = evhttp_connection_base_new(base, nullptr, "127.0.0.1", port);
    evhttp_request* req = evhttp_request_new([](evhttp_request* r, void* ctx) {
        HTTPResult* res = static_cast<HTTPResult*>(ctx);
        if (!r) return;
        res->status = evhttp_request_get_response_code(r);
        evbuffer* buf = evhttp_request_get_input_buffer(r);
        size_t n = evbuffer_get_length(buf);
        res->body.assign(reinterpret_cast<const char*>(evbuffer_pullup(buf, n)), n);
    }, &result);
    evkeyvalq* headers = evhttp_request_get_output_headers(req);
    evhttp_add_header(headers, "Host", "127.0.0.1");
    evhttp_add_header(headers, "Connection", "close"); // else the loop waits on the idle connection
    if (!userPass.empty()) evhttp_add_header(headers, "Authorization", ("Basic " + EncodeBase64(userPass)).c_str());
    evbuffer_add(evhttp_request_get_output_buffer(req), body.data(), body.size());
    evhttp_make_request(conn, req, method, path.c_str());
    event_base_dispatch(base);
    evhttp_connection_free(conn);
    event_base_free(base);
    return result;
}

UniValue Parse(const std::string& s)
{
    UniValue v;
    BOOST_REQUIRE(v.read(s));
    return v;
}

void FinishWarmup()
{
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
}

APIOptions PasswordOptions()
{
    APIOptions o;
    o.user = "u";
    o.password = "p";
    return o;
}

} // namespace

BOOST_AUTO_TEST_SUITE(httprpc_tests)

// Runs first (see CMakeLists.txt): warmup can only be finished once per process.
BOOST_AUTO_TEST_CASE(warmup)
{
    BOOST_REQUIRE(RPCIsInWarmup(nullptr));
    SelectParams("regtest"); // omnilite_getstatus names the network
    RegisterOmniRPCCommands();
    SetRPCWarmupStatus("Loading the Omni state...");
    APIServer api(PasswordOptions(), nullptr);
    api.Start();

    HTTPResult r = Request(api.Port(), R"({"method":"omni_getinfo","params":[],"id":1})");
    BOOST_CHECK_EQUAL(r.status, 500);
    UniValue error = find_value(Parse(r.body), "error");
    BOOST_CHECK_EQUAL(find_value(error, "code").get_int(), RPC_IN_WARMUP);
    BOOST_CHECK_EQUAL(find_value(error, "message").get_str(), "Loading the Omni state...");

    // The status answers already, and says what is going on.
    r = Request(api.Port(), R"({"method":"omnilite_getstatus","params":[],"id":1})");
    BOOST_CHECK_EQUAL(r.status, 200);
    UniValue status = find_value(Parse(r.body), "result");
    BOOST_CHECK(find_value(status, "loading").isTrue());
    BOOST_CHECK_EQUAL(find_value(status, "loadingstatus").get_str(), "Loading the Omni state...");
    BOOST_CHECK(find_value(status, "synced").isFalse());

    FinishWarmup();
    r = Request(api.Port(), R"({"method":"uptime","params":[],"id":1})");
    BOOST_CHECK_EQUAL(r.status, 200);
}

BOOST_AUTO_TEST_CASE(login)
{
    RegisterOmniRPCCommands();
    FinishWarmup();
    APIServer api(PasswordOptions(), nullptr);
    api.Start();
    const std::string call = R"({"method":"uptime","params":[],"id":1})";

    BOOST_CHECK_EQUAL(Request(api.Port(), call).status, 200);
    BOOST_CHECK_EQUAL(Request(api.Port(), call, "u:wrong").status, 401);
    BOOST_CHECK_EQUAL(Request(api.Port(), call, "").status, 401);
    BOOST_CHECK_EQUAL(Request(api.Port(), call, "nocolon").status, 401);
}

BOOST_AUTO_TEST_CASE(cookie)
{
    RegisterOmniRPCCommands();
    FinishWarmup();
    TempDir dir;
    APIOptions o;
    o.cookieFile = dir.Sub(".cookie");
    std::string cookie;
    {
        APIServer api(o, nullptr);
        api.Start();
        std::ifstream file(o.cookieFile);
        BOOST_REQUIRE(std::getline(file, cookie));
        BOOST_CHECK_EQUAL(cookie.substr(0, 11), "__cookie__:");
        BOOST_CHECK_EQUAL(cookie.size(), 11u + 64u);
        BOOST_CHECK((fs::status(o.cookieFile).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);

        // The same login as litecoin-cli would read from the file.
        RPCClient client("127.0.0.1", api.Port(), "__cookie__", cookie.substr(11), 10);
        BOOST_CHECK(client.Call("uptime").isNum());
        BOOST_CHECK_EQUAL(Request(api.Port(), R"({"method":"uptime","id":1})", "__cookie__:other").status, 401);
    }
    // Removed on stop.
    BOOST_CHECK(!fs::exists(o.cookieFile));
}

BOOST_AUTO_TEST_CASE(requests_and_status_codes)
{
    RegisterOmniRPCCommands();
    FinishWarmup();
    APIServer api(PasswordOptions(), nullptr);
    api.Start();
    const int port = api.Port();

    // Unknown method: 404 with Core's error.
    HTTPResult r = Request(port, R"({"method":"nosuchcall","params":[],"id":7})");
    BOOST_CHECK_EQUAL(r.status, 404);
    UniValue reply = Parse(r.body);
    BOOST_CHECK_EQUAL(find_value(find_value(reply, "error"), "code").get_int(), RPC_METHOD_NOT_FOUND);
    BOOST_CHECK_EQUAL(find_value(reply, "id").get_int(), 7);

    // Not JSON, not an object or array, no method.
    r = Request(port, "{nope");
    BOOST_CHECK_EQUAL(r.status, 500);
    BOOST_CHECK_EQUAL(find_value(find_value(Parse(r.body), "error"), "code").get_int(), RPC_PARSE_ERROR);
    r = Request(port, "42");
    BOOST_CHECK_EQUAL(find_value(find_value(Parse(r.body), "error"), "code").get_int(), RPC_PARSE_ERROR);
    r = Request(port, R"({"params":[],"id":1})");
    BOOST_CHECK_EQUAL(r.status, 400);
    BOOST_CHECK_EQUAL(find_value(find_value(Parse(r.body), "error"), "code").get_int(), RPC_INVALID_REQUEST);

    // Only POST, only "/" and "/wallet/...".
    BOOST_CHECK_EQUAL(Request(port, "", "u:p", EVHTTP_REQ_GET).status, 405);
    BOOST_CHECK_EQUAL(Request(port, R"({"method":"uptime","id":1})", "u:p", EVHTTP_REQ_POST, "/rest").status, 404);
    BOOST_CHECK_EQUAL(Request(port, R"({"method":"uptime","id":1})", "u:p", EVHTTP_REQ_POST, "/wallet/w1").status, 200);

    // A batch answers each request on its own, errors included, with HTTP 200.
    r = Request(port, R"([{"method":"uptime","id":1},{"method":"nosuchcall","id":2},{"id":3}])");
    BOOST_CHECK_EQUAL(r.status, 200);
    UniValue batch = Parse(r.body);
    BOOST_REQUIRE_EQUAL(batch.size(), 3u);
    BOOST_CHECK(find_value(batch[0], "result").isNum());
    BOOST_CHECK_EQUAL(find_value(find_value(batch[1], "error"), "code").get_int(), RPC_METHOD_NOT_FOUND);
    BOOST_CHECK_EQUAL(find_value(find_value(batch[2], "error"), "code").get_int(), RPC_INVALID_REQUEST);

    // Help covers OmniLite's calls; arguments are checked as in omnilitecored.
    r = Request(port, R"({"method":"help","params":[],"id":1})");
    std::string help = find_value(Parse(r.body), "result").get_str();
    BOOST_CHECK(help.find("omni_getinfo") != std::string::npos);
    BOOST_CHECK(help.find("omni_createpayload_simplesend") != std::string::npos);
    BOOST_CHECK(help.find("omni_createrawtx_opreturn") != std::string::npos);
    r = Request(port, R"({"method":"omni_getproperty","params":[],"id":1})");
    BOOST_CHECK_EQUAL(r.status, 500);
    BOOST_CHECK_EQUAL(find_value(find_value(Parse(r.body), "error"), "code").get_int(), RPC_MISC_ERROR); // help text as error
}

BOOST_AUTO_TEST_CASE(passthrough)
{
    RegisterOmniRPCCommands();
    FinishWarmup();
    FakeLitecoind fake;
    RPCClient node("127.0.0.1", fake.port, "user", "pass", 10);
    APIServer api(PasswordOptions(), &node);
    api.Start();

    // Listed Core calls go to the node, with their parameters.
    HTTPResult r = Request(api.Port(), R"({"method":"getblockcount","params":[],"id":1})");
    BOOST_CHECK_EQUAL(find_value(Parse(r.body), "result").get_str(), "getblockcount/0");
    r = Request(api.Port(), R"({"method":"getrawtransaction","params":["ab",true],"id":1})");
    BOOST_CHECK_EQUAL(find_value(Parse(r.body), "result").get_str(), "getrawtransaction/2");

    // Others do not: the node's wallet stays out of reach.
    r = Request(api.Port(), R"({"method":"dumpprivkey","params":["x"],"id":1})");
    BOOST_CHECK_EQUAL(r.status, 404);

    // Without a node to pass on to, they are unknown.
    APIServer alone(PasswordOptions(), nullptr);
    alone.Start();
    BOOST_CHECK_EQUAL(Request(alone.Port(), R"({"method":"getblockcount","id":1})").status, 404);
}

BOOST_AUTO_TEST_CASE(no_send)
{
    RegisterOmniRPCCommands();
    FinishWarmup();
    // The calls that spend through the node's wallet, and only those.
    BOOST_CHECK(SendingMethods().count("omni_send"));
    BOOST_CHECK(SendingMethods().count("omni_senddexpay"));
    BOOST_CHECK(SendingMethods().count("omni_funded_sendall"));
    BOOST_CHECK(SendingMethods().count("omni_sendalert"));
    BOOST_CHECK(SendingMethods().count("omni_setnonfungibledata"));
    BOOST_CHECK(!SendingMethods().count("omni_getinfo"));
    BOOST_CHECK(!SendingMethods().count("omni_createpayload_simplesend"));
    BOOST_CHECK(!SendingMethods().count("omni_createrawtx_opreturn"));

    APIOptions o = PasswordOptions();
    o.send = false;
    APIServer api(o, nullptr);
    api.Start();

    // Refused before OmniLite sees them, alone and in a batch.
    HTTPResult r = Request(api.Port(), R"({"method":"omni_send","params":[],"id":1})");
    BOOST_CHECK_EQUAL(r.status, 404);
    UniValue error = find_value(Parse(r.body), "error");
    BOOST_CHECK_EQUAL(find_value(error, "code").get_int(), RPC_METHOD_NOT_FOUND);
    BOOST_CHECK(find_value(error, "message").get_str().find("-apisend=0") != std::string::npos);
    r = Request(api.Port(), R"([{"method":"omni_funded_send","id":1},{"method":"uptime","id":2}])");
    UniValue batch = Parse(r.body);
    BOOST_REQUIRE_EQUAL(batch.size(), 2u);
    BOOST_CHECK_EQUAL(find_value(find_value(batch[0], "error"), "code").get_int(), RPC_METHOD_NOT_FOUND);
    BOOST_CHECK(find_value(batch[1], "result").isNum()); // the other calls still answer

    // With the default, omni_send reaches OmniLite (here: its argument check).
    APIServer sending(PasswordOptions(), nullptr);
    sending.Start();
    BOOST_CHECK_EQUAL(Request(sending.Port(), R"({"method":"omni_send","params":[],"id":1})").status, 500);
}

BOOST_AUTO_TEST_CASE(passthrough_node_errors)
{
    RegisterOmniRPCCommands();
    FinishWarmup();
    int port;
    {
        FakeLitecoind fake;
        port = fake.port;
    }
    // Nobody listens there any more.
    RPCClient node("127.0.0.1", port, "user", "pass", 5);
    APIServer api(PasswordOptions(), &node);
    api.Start();
    HTTPResult r = Request(api.Port(), R"({"method":"getblockcount","id":1})");
    BOOST_CHECK_EQUAL(r.status, 500);
    BOOST_CHECK_EQUAL(find_value(find_value(Parse(r.body), "error"), "code").get_int(), RPC_CLIENT_NOT_CONNECTED);
}

BOOST_AUTO_TEST_SUITE_END()
