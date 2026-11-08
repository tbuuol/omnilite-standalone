// Copyright (c) 2026 The OmniLite Standalone developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <config.h>
#include <fs.h>
#include <httprpc.h>
#include <indexer.h>
#include <logging.h>
#include <mempool.h>
#include <pubkey.h>
#include <rpc/server.h>
#include <rpcclient.h>
#include <shutdown.h>
#include <snapshot.h>
#include <status.h>
#include <store.h>
#include <syncer.h>
#include <util/time.h>
#include <wallet/wallet.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

void Line(const std::string& label, const std::string& text)
{
    std::printf("%-12s %s\n", (label + ":").c_str(), text.c_str());
}

/** Prints the node status and reports whether everything needed for indexing is in place. */
bool RunCheck(const Config& cfg)
{
    bool ok = true;
    auto fail = [&](const std::string& label, const std::string& text) {
        ok = false;
        Line(label, "ERROR: " + text);
    };

    Line("RPC", cfg.RPCURL());
    std::pair<std::string, std::string> credentials;
    try {
        credentials = cfg.Credentials();
    } catch (const ConfigError& e) {
        fail("Login", e.what());
        return false;
    }
    RPCClient node(cfg.rpcConnect, cfg.rpcPort, credentials.first, credentials.second);

    try {
        UniValue net = node.Call("getnetworkinfo");
        Line("Node", find_value(net, "subversion").get_str() + " (" + std::to_string(find_value(net, "version").get_int()) + ")");

        UniValue info = node.Call("getblockchaininfo");
        std::string chain = find_value(info, "chain").get_str();
        if (chain == Params().CoreChain()) {
            Line("Chain", chain);
        } else {
            fail("Chain", "node runs \"" + chain + "\", configured network is \"" + Params().NetworkIDString() + "\"");
        }
        int64_t blocks = find_value(info, "blocks").get_int64();
        Line("Height", std::to_string(blocks) + " blocks, " + std::to_string(find_value(info, "headers").get_int64()) + " headers");
        if (find_value(info, "initialblockdownload").isTrue()) {
            char progress[32];
            std::snprintf(progress, sizeof(progress), "%.2f", find_value(info, "verificationprogress").get_real() * 100);
            Line("Sync", std::string("node is still syncing (") + progress + " %)");
        } else {
            Line("Sync", "done");
        }

        try {
            TxIndexStatus tx = GetTxIndexStatus(node);
            if (!tx.enabled) {
                fail("txindex", "off, litecoind needs -txindex=1");
            } else {
                Line("txindex", tx.synced ? "on" : "on, still building");
            }
        } catch (const std::exception& e) {
            fail("txindex", std::string("cannot check: ") + e.what());
        }
        if (find_value(info, "pruned").isTrue()) {
            fail("Pruning", "node is pruned, all blocks from " + std::to_string(Params().OmniGenesisBlock()) + " are needed");
        }
        Line("Omni start", "block " + std::to_string(Params().OmniGenesisBlock()) +
            (blocks >= Params().OmniGenesisBlock() ? " (reached)" : " (node is not there yet)"));
    } catch (const NodeUnauthorized& e) {
        fail("Login", e.what());
    } catch (const std::exception& e) {
        fail("Node", e.what());
    }
    return ok;
}

std::string FormatDuration(int64_t seconds)
{
    const int64_t d = seconds / 86400, h = seconds / 3600 % 24, m = seconds / 60 % 60;
    if (d > 0) return std::to_string(d) + "d " + std::to_string(h) + "h";
    if (h > 0) return std::to_string(h) + "h " + std::to_string(m) + "m";
    return std::to_string(m) + "m " + std::to_string(seconds % 60) + "s";
}

/** Asks the running program over its API; 0 if it is synced, 1 otherwise. */
int RunStatus(const Config& cfg)
{
    UniValue s;
    try {
        std::pair<std::string, std::string> credentials = cfg.APICredentials();
        // An API listening on all addresses is reached locally.
        const bool any = cfg.apiBind == "0.0.0.0" || cfg.apiBind == "::";
        RPCClient api(any ? "127.0.0.1" : cfg.apiBind, cfg.apiPort, credentials.first, credentials.second, 10);
        s = api.Call("omnilite_getstatus");
    } catch (const std::exception& e) {
        Line("Status", std::string("ERROR: not running or not reachable: ") + e.what());
        return 1;
    }

    Line("Version", find_value(s, "version").get_str() + " (" + find_value(s, "network").get_str() + ")");
    const UniValue block = find_value(s, "block");
    if (block.isNull()) {
        Line("Omni", "no block yet (starts at " + std::to_string(find_value(s, "startblock").get_int64()) + ")");
    } else {
        Line("Omni", "block " + std::to_string(block.get_int64()) + " (" + FormatISO8601DateTime(find_value(s, "blocktime").get_int64()) + ")");
    }
    const UniValue nodeBlock = find_value(s, "nodeblock");
    Line("Node", nodeBlock.isNull() ? "not reached yet" : "block " + std::to_string(nodeBlock.get_int64()) +
        (find_value(s, "nodeconnected").isTrue() ? "" : " (last seen)"));
    std::string sync;
    if (find_value(s, "loading").isTrue()) {
        sync = "loading: " + find_value(s, "loadingstatus").get_str();
    } else if (find_value(s, "synced").isTrue()) {
        sync = "synced";
    } else if (!find_value(s, "behind").isNull() && find_value(s, "behind").get_int64() > 0) {
        sync = std::to_string(find_value(s, "behind").get_int64()) + " blocks behind the node";
    } else {
        sync = "waiting for the node";
    }
    Line("Sync", sync);
    const UniValue error = find_value(s, "lastnodeerror");
    if (error.isObject()) {
        Line(find_value(error, "current").isTrue() ? "Node error" : "Last error",
            find_value(error, "message").get_str() + " (" + FormatISO8601DateTime(find_value(error, "time").get_int64()) + ")");
    }
    const UniValue reorg = find_value(s, "lastreorg");
    Line("Reorgs", std::to_string(find_value(s, "reorgblocks").get_int64()) + " blocks disconnected since the start" +
        (reorg.isObject() ? ", last at block " + std::to_string(find_value(reorg, "block").get_int64()) + " (" +
            FormatISO8601DateTime(find_value(reorg, "time").get_int64()) + ")" : ""));
    Line("Uptime", FormatDuration(find_value(s, "uptime").get_int64()));
    return find_value(s, "synced").isTrue() ? 0 : 1;
}

OmniOptions OptionsOf(const Config& cfg)
{
    OmniOptions options;
    options.activationAllowSenders = cfg.activationAllowSenders;
    options.alertAllowSenders = cfg.alertAllowSenders;
    options.debug = cfg.omniDebug;
    return options;
}

void PrintSnapshot(const SnapshotInfo& info)
{
    Line("Network", info.network);
    Line("Block", std::to_string(info.height) + " " + info.blockHash + " (" + FormatISO8601DateTime(info.blockTime) + ")");
    Line("Files", std::to_string(info.files));
    Line("Consensus", info.consensusHash);
}

/** -exportsnapshot / -importsnapshot; 0 on success. */
int RunSnapshot(const Config& cfg)
{
    try {
        std::pair<std::string, std::string> credentials = cfg.Credentials();
        RPCClient node(cfg.rpcConnect, cfg.rpcPort, credentials.first, credentials.second);
        RPCNodeData nodeData(node);
        if (!cfg.exportSnapshot.empty()) {
            SnapshotInfo info = ExportSnapshot(cfg.NetDir(), cfg.undoDepth, nodeData, OptionsOf(cfg), cfg.exportSnapshot);
            Line("Snapshot", cfg.exportSnapshot);
            PrintSnapshot(info);
            std::printf("\nImport elsewhere with -importsnapshot=<file> -snapshothash=%s\n", info.consensusHash.c_str());
        } else {
            std::string chain = find_value(node.Call("getblockchaininfo"), "chain").get_str();
            if (chain != Params().CoreChain()) {
                throw std::runtime_error("node runs chain \"" + chain + "\", configured network is \"" + Params().NetworkIDString() + "\"");
            }
            RPCChainSource source(node);
            SnapshotInfo info = ImportSnapshot(cfg.NetDir(), cfg.undoDepth, nodeData, source, OptionsOf(cfg),
                                               cfg.importSnapshot, cfg.snapshotHash);
            Line("Imported", cfg.importSnapshot + " into " + cfg.NetDir());
            PrintSnapshot(info);
        }
        return 0;
    } catch (const std::exception& e) {
        Line("Snapshot", std::string("ERROR: ") + e.what());
        return 1;
    }
}

void HandleSignal(int) { StartShutdown(); }

/** Syncs the index with the node and answers the API until SIGINT, SIGTERM or the stop call. */
int Run(const Config& cfg)
{
    struct sigaction sa {};
    sa.sa_handler = HandleSignal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    try {
        std::pair<std::string, std::string> credentials = cfg.Credentials();
        RPCClient node(cfg.rpcConnect, cfg.rpcPort, credentials.first, credentials.second);
        std::string chain = find_value(node.Call("getblockchaininfo"), "chain").get_str();
        if (chain != Params().CoreChain()) {
            LogError("node at %s runs chain \"%s\", configured network is \"%s\"", cfg.RPCURL(), chain, Params().NetworkIDString());
            return 1;
        }
        if (!GetTxIndexStatus(node).enabled) {
            LogError("node runs without -txindex; restart litecoind with -txindex=1");
            return 1;
        }

        CStore store(cfg.DBPath(), Params().NetworkIDString(), cfg.undoDepth);
        GetSyncStatus().SetTip(store.Tip()); // for omnilite_getstatus while OmniLite loads
        RPCChainSource source(node);
        RPCNodeData nodeData(node);
        OmniIndexer processor(nodeData, store);
        // OmniLite logs at debug level; -omnidebug only makes sense if that is shown.
        if (!cfg.omniDebug.empty()) SetLogLevel(LogLevel::Debug);

        // The API answers from the start; until OmniLite has loaded its
        // state, omni_* calls get RPC_IN_WARMUP, as from omnilitecored.
        std::unique_ptr<APIServer> api;
        if (cfg.api) {
            RegisterOmniRPCCommands();
            SetWalletNode(&node); // omni_send* sign and broadcast through the node's wallet
            SetRPCWarmupStatus("Loading the Omni state...");
            APIOptions apiOptions;
            apiOptions.bind = cfg.apiBind;
            apiOptions.port = cfg.apiPort;
            apiOptions.user = cfg.apiUser;
            apiOptions.password = cfg.apiPassword;
            apiOptions.cookieFile = cfg.APICookieFile();
            apiOptions.send = cfg.apiSend;
            fs::create_directories(fs::path(cfg.APICookieFile()).parent_path());
            api = std::make_unique<APIServer>(apiOptions, &node);
            api->Start();
            if (cfg.apiBind != "127.0.0.1" && cfg.apiBind != "::1" && cfg.apiBind != "localhost") {
                LogWarn("the API listens on %s: anyone who can reach it and knows the login sees your Omni data", cfg.apiBind);
            }
            if (!cfg.apiSend) LogInfo("-apisend=0: the API answers no omni_send* calls, it cannot spend from the node's wallet");
        }

        processor.Start(cfg.OmniDataDir(), OptionsOf(cfg));
        if (api) SetRPCWarmupFinished();

        // Unconfirmed Omni transactions only matter to the API (omni_listpendingtransactions).
        std::thread mempoolThread;
        if (api) {
            mempoolThread = std::thread([&nodeData, &cfg] {
                MempoolWatcher watcher(nodeData);
                watcher.Run(ShutdownFlag(), std::chrono::seconds(cfg.pollInterval));
            });
        }
        struct JoinOnExit {
            std::thread& t;
            ~JoinOnExit()
            {
                if (!t.joinable()) return;
                StartShutdown(); // also on an error: the watcher must not outlive what it uses
                t.join();
            }
        } joinMempool{mempoolThread};

        Syncer syncer(source, store, processor, Params().OmniGenesisBlock(), std::chrono::seconds(cfg.pollInterval));
        LogInfo("syncing: network %s, node %s, database %s, start %d", Params().NetworkIDString(), cfg.RPCURL(),
            cfg.DBPath(), Params().OmniGenesisBlock());
        syncer.Run(ShutdownFlag());
        if (api) api->Stop();
        LogInfo("stopped");
        return 0;
    } catch (const std::exception& e) {
        LogError("stopped: %s", e.what());
        return 1;
    }
}

} // namespace

int main(int argc, char* argv[])
{
    // The secp256k1 context CPubKey::IsFullyValid and friends use.
    ECCVerifyHandle verifyHandle;

    Config cfg;
    try {
        cfg = LoadConfig(std::vector<std::string>(argv + 1, argv + argc));
    } catch (const ConfigError& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 2;
    }
    if (cfg.help) {
        std::cout << HelpMessage();
        return 0;
    }
    if (cfg.check) {
        return RunCheck(cfg) ? 0 : 1;
    }
    if (cfg.status) {
        return RunStatus(cfg);
    }
    if (!cfg.exportSnapshot.empty() || !cfg.importSnapshot.empty()) {
        return RunSnapshot(cfg);
    }
    return Run(cfg);
}
