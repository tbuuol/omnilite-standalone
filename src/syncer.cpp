// Copyright (c) 2026 The OmniLite Standalone developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <syncer.h>

#include <status.h>

#include <logging.h>
#include <rpcclient.h>
#include <validation.h>

#include <thread>

// --- RPCChainSource ---

namespace {

template <typename F>
auto CallNode(F f) -> decltype(f())
{
    try {
        return f();
    } catch (const NodeUnauthorized&) {
        throw; // retrying cannot fix the credentials
    } catch (const RPCError& e) {
        throw NodeError(e.what());
    } catch (const NodeUnreachable& e) {
        throw NodeError(e.what());
    }
}

} // namespace

int64_t RPCChainSource::GetBlockCount()
{
    return CallNode([&] { return m_node.Call("getblockcount").get_int64(); });
}

std::string RPCChainSource::GetBlockHash(int64_t height)
{
    UniValue params(UniValue::VARR);
    params.push_back(height);
    return CallNode([&] {
        try {
            return m_node.Call("getblockhash", params).get_str();
        } catch (const RPCError& e) {
            if (e.code == RPC_INVALID_PARAMETER) throw HeightOutOfRange(e.what()); // "Block height out of range"
            throw;
        }
    });
}

BlockHeader RPCChainSource::GetBlockHeader(const std::string& hash)
{
    UniValue params(UniValue::VARR);
    params.push_back(hash);
    params.push_back(UniValue(true));
    UniValue r = CallNode([&] { return m_node.Call("getblockheader", params); });
    BlockHeader h;
    h.hash = find_value(r, "hash").get_str();
    h.height = find_value(r, "height").get_int64();
    const UniValue prev = find_value(r, "previousblockhash");
    if (prev.isStr()) h.prevHash = prev.get_str();
    h.time = find_value(r, "time").get_int64();
    return h;
}

// --- Syncer ---

Syncer::Syncer(ChainSource& source, CStore& store, Processor& processor, int64_t startHeight,
    std::chrono::milliseconds poll)
    : m_source(source), m_store(store), m_processor(processor), m_start(startHeight), m_poll(poll)
{
    GetSyncStatus().SetTip(m_store.Tip());
}

bool Syncer::Step()
{
    std::optional<BlockInfo> tip = m_store.Tip();
    int64_t next = tip ? tip->height + 1 : m_start;

    if (next > m_nodeHeight) {
        m_nodeHeight = m_source.GetBlockCount();
        GetSyncStatus().SetNodeHeight(m_nodeHeight);
        g_bestHeaderHeight = static_cast<int>(m_nodeHeight);
    }
    if (next > m_nodeHeight) {
        // Caught up. Make sure our tip is still the node's block at that
        // height; a reorg to an equally long or shorter chain shows up only here.
        if (!tip) return false;
        if (tip->height <= m_nodeHeight && m_source.GetBlockHash(tip->height) == tip->hash) return false;
        Disconnect();
        return true;
    }

    std::string hash;
    try {
        hash = m_source.GetBlockHash(next);
    } catch (const HeightOutOfRange&) {
        // The node switched to a shorter chain since we last asked for its height.
        m_nodeHeight = -1;
        return true;
    }
    BlockHeader header = m_source.GetBlockHeader(hash);
    if (header.height != next) {
        // The node reorganized between the two calls; ask again.
        throw NodeError("block " + hash + " is at height " + std::to_string(header.height) +
            ", expected " + std::to_string(next));
    }
    if (tip && header.prevHash != tip->hash) {
        Disconnect();
        return true;
    }
    Connect(header);
    return true;
}

void Syncer::Connect(const BlockHeader& header)
{
    BlockInfo info{header.height, header.hash, header.prevHash, header.time};
    std::unique_ptr<CBlockTx> tx = m_store.Begin(info);
    try {
        m_processor.ConnectBlock(*tx, header);
    } catch (const NodeError&) {
        throw;
    } catch (const std::exception& e) {
        throw std::runtime_error("block " + std::to_string(header.height) + " " + header.hash + ": " + e.what());
    }
    tx->Commit();
    GetSyncStatus().SetTip(info);
    auto now = std::chrono::steady_clock::now();
    if (header.height == m_nodeHeight || now - m_lastLog > std::chrono::seconds(10)) {
        m_lastLog = now;
        LogInfo("block connected: height %d, node %d, hash %s", header.height, m_nodeHeight, header.hash);
    }
}

void Syncer::Disconnect()
{
    BlockInfo b = m_store.Disconnect();
    m_processor.DisconnectBlock(b);
    GetSyncStatus().Disconnected(b);
    GetSyncStatus().SetTip(m_store.Tip());
    LogWarn("block disconnected (reorg): height %d, hash %s", b.height, b.hash);
}

void Syncer::Run(const std::atomic<bool>& stop)
{
    while (!stop) {
        bool progressed = false;
        try {
            progressed = Step();
            GetSyncStatus().NodeAnswered();
        } catch (const NodeError& e) {
            // The node may be restarting or briefly unreachable: keep trying.
            LogWarn("node request failed, retrying: %s", e.what());
            GetSyncStatus().NodeFailed(e.what());
            m_nodeHeight = -1;
        }
        if (progressed) continue;
        // Wait for the next poll, but react to stop quickly.
        auto until = std::chrono::steady_clock::now() + m_poll;
        while (!stop && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
