// Copyright (c) 2026 The OmniLite Standalone developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef OMNILITE_STANDALONE_HTTPRPC_H
#define OMNILITE_STANDALONE_HTTPRPC_H

// The JSON-RPC API, over HTTP like litecoind and omnilitecored: Basic auth
// with a password or a cookie file, single and batch requests, Core's
// HTTP status codes. The omni_* commands come from OmniLite's RPC files
// (tableRPC); a fixed list of read-only Litecoin Core calls is passed on to
// the node, so a client can use one connection for both.

#include <fs.h>

#include <memory>
#include <set>
#include <string>
#include <thread>

class RPCClient;
struct event;
struct event_base;
struct evhttp;
struct evhttp_request;

/** The Litecoin Core calls the API passes on to the node. */
extern const std::set<std::string> PASSTHROUGH_METHODS;

/** Puts OmniLite's omni_* commands into tableRPC. Once, before Start. */
void RegisterOmniRPCCommands();

/** The calls that sign and broadcast through the node's wallet (omni_send*, omni_funded_*, ...), once registered. */
const std::set<std::string>& SendingMethods();

struct APIOptions {
    std::string bind = "127.0.0.1";
    int port = 0; // 0: any free port (tests)
    std::string user;
    /** Empty: a random cookie is written to cookieFile. */
    std::string password;
    fs::path cookieFile;
    /** false: SendingMethods() are refused, so the API cannot spend from the node's wallet. */
    bool send = true;
};

class APIServer
{
public:
    /** node may be null: then nothing is passed on. */
    APIServer(APIOptions options, const RPCClient* node);
    ~APIServer();

    /** Listens and answers on a thread of its own. Throws if it cannot listen. */
    void Start();
    /** Stops answering, waits for the thread and removes the cookie file. */
    void Stop();

    /** The port it listens on, once started. */
    int Port() const { return m_port; }

private:
    static void HandleRequest(evhttp_request* req, void* self);
    void Handle(evhttp_request* req);
    bool CheckAuth(evhttp_request* req) const;

    APIOptions m_options;
    const RPCClient* m_node;
    std::string m_userColonPass;
    event_base* m_base = nullptr;
    evhttp* m_http = nullptr;
    event* m_stop = nullptr;
    std::thread m_thread;
    int m_port = 0;
    bool m_cookieWritten = false;
};

#endif // OMNILITE_STANDALONE_HTTPRPC_H
