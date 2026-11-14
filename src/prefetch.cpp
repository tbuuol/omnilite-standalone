// Copyright (c) 2026 The OmniLite Standalone developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <prefetch.h>

#include <logging.h>
#include <rpcclient.h>
#include <util/strencodings.h>

#include <algorithm>

BlockPrefetcher::BlockPrefetcher(const RPCClient& node, ChainSource& inner, NodeData& innerData, int threads, int ahead, int depth)
    : m_node(node), m_inner(inner), m_innerData(innerData), m_ahead(std::max(1, ahead)), m_depth(std::max(1, depth))
{
    for (int i = 0; i < threads; ++i) m_threads.emplace_back([this] { Work(); });
}

BlockPrefetcher::~BlockPrefetcher()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_work.notify_all();
    for (std::thread& t : m_threads) t.join();
}

void BlockPrefetcher::ScheduleFrom(int64_t from)
{
    if (m_nodeHeight < 0) return;
    const int64_t to = std::min(from + m_ahead - 1, m_nodeHeight - m_depth);
    bool added = false;
    for (int64_t h = std::max(from, m_scheduledTo + 1); h <= to; ++h) {
        if (m_entries.count(h)) continue;
        m_entries[h];
        m_queue.push_back(h);
        added = true;
    }
    if (to > m_scheduledTo) m_scheduledTo = to;
    if (added) {
        // Workers take the lowest height first: the syncer needs it next.
        std::sort(m_queue.begin(), m_queue.end(), std::greater<int64_t>());
        m_work.notify_all();
    }
}

void BlockPrefetcher::Work()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    while (true) {
        m_work.wait(lock, [this] { return m_stop || !m_queue.empty(); });
        if (m_stop) return;
        const int64_t height = m_queue.back();
        m_queue.pop_back();
        lock.unlock();
        Entry fetched;
        Fetch(height, fetched);
        lock.lock();
        auto it = m_entries.find(height);
        if (it == m_entries.end()) continue; // dropped meanwhile (the syncer went back below it)
        it->second = std::move(fetched);
        it->second.done = true;
        if (!it->second.failed) m_byHash[it->second.header.hash] = height;
        m_ready.notify_all();
    }
}

void BlockPrefetcher::Fetch(int64_t height, Entry& e)
{
    try {
        const std::string hash = m_inner.GetBlockHash(height);
        UniValue params(UniValue::VARR);
        params.push_back(hash);
        params.push_back(0);
        const std::string hex = m_node.Call("getblock", params).get_str();
        if (!IsHex(hex)) throw std::runtime_error("getblock " + hash + ": not hex");
        const std::vector<unsigned char> raw = ParseHex(hex);
        if (!DecodeBlockTxs(raw, hash, e.txs)) {
            LogWarn("block %s: MWEB data in a transaction, reading it verbosely", hash);
            e.txs = m_innerData.GetBlockTxs(hash);
        }
        // Header fields straight from the raw header (DecodeBlockTxs checked its hash).
        uint256 prev;
        std::copy(raw.begin() + 4, raw.begin() + 36, prev.begin());
        e.header.hash = hash;
        e.header.height = height;
        e.header.prevHash = height > 0 ? prev.GetHex() : "";
        e.header.time = static_cast<int64_t>(raw[68]) | static_cast<int64_t>(raw[69]) << 8 |
                        static_cast<int64_t>(raw[70]) << 16 | static_cast<int64_t>(raw[71]) << 24;
    } catch (const std::exception& ex) {
        // The syncer asks the node itself then and handles the error as usual.
        LogDebug("prefetch of block %d failed: %s", height, ex.what());
        e.failed = true;
        e.txs.clear();
    }
}

int64_t BlockPrefetcher::GetBlockCount()
{
    const int64_t height = m_inner.GetBlockCount();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_nodeHeight = height;
    return height;
}

std::string BlockPrefetcher::GetBlockHash(int64_t height)
{
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        // Blocks below the one asked for are processed (or abandoned after a
        // reorg): drop them, except those a worker is still reading.
        for (auto it = m_entries.begin(); it != m_entries.end() && it->first < height;) {
            const bool queued = std::find(m_queue.begin(), m_queue.end(), it->first) != m_queue.end();
            if (!it->second.done && !queued) {
                ++it;
                continue;
            }
            if (queued) m_queue.erase(std::find(m_queue.begin(), m_queue.end(), it->first));
            if (it->second.done && !it->second.failed) m_byHash.erase(it->second.header.hash);
            it = m_entries.erase(it);
        }
        if (height <= m_scheduledTo - m_ahead) m_scheduledTo = height - 1; // went back: schedule again
        ScheduleFrom(height);
        auto it = m_entries.find(height);
        if (it != m_entries.end()) {
            m_ready.wait(lock, [&] { return it->second.done || m_stop; });
            if (it->second.done && !it->second.failed) return it->second.header.hash;
            if (it->second.done) m_entries.erase(it);
        }
    }
    return m_inner.GetBlockHash(height);
}

BlockHeader BlockPrefetcher::GetBlockHeader(const std::string& hash)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto h = m_byHash.find(hash);
        if (h != m_byHash.end()) return m_entries.at(h->second).header;
    }
    return m_inner.GetBlockHeader(hash);
}

std::vector<CTransactionRef> BlockPrefetcher::GetBlockTxs(const std::string& hash)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto h = m_byHash.find(hash);
        if (h != m_byHash.end()) {
            Entry& e = m_entries.at(h->second);
            if (!e.txsTaken) {
                e.txsTaken = true;
                return std::move(e.txs);
            }
        }
    }
    return m_innerData.GetBlockTxs(hash);
}

bool BlockPrefetcher::GetTx(const uint256& txid, CTransactionRef& tx, uint256& blockHash)
{
    return m_innerData.GetTx(txid, tx, blockHash);
}

std::vector<uint256> BlockPrefetcher::GetMempool()
{
    return m_innerData.GetMempool();
}
