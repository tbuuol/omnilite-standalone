// Copyright (c) 2026 The OmniLite Standalone developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef OMNILITE_STANDALONE_PREFETCH_H
#define OMNILITE_STANDALONE_PREFETCH_H

// Reads blocks ahead of the syncer while OmniLite processes the current one.
// Worker threads fetch hash and raw block of the next heights from the node;
// the syncer and the indexer then take them from memory. Only blocks with at
// least `depth` confirmations are read ahead, deeper than any reorg the store
// can undo, so a prefetched block never has to be thrown away. Everything else
// (the tip, the mempool, single transactions) goes straight to the node.

#include <indexer.h>
#include <syncer.h>

#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

class RPCClient;

class BlockPrefetcher : public ChainSource, public NodeData
{
public:
    /**
     * threads workers, at most ahead blocks in memory, only blocks at least
     * depth below the node's tip. inner/innerData serve whatever is not
     * prefetched.
     */
    BlockPrefetcher(const RPCClient& node, ChainSource& inner, NodeData& innerData, int threads, int ahead, int depth);
    ~BlockPrefetcher() override;

    // ChainSource
    int64_t GetBlockCount() override;
    std::string GetBlockHash(int64_t height) override;
    BlockHeader GetBlockHeader(const std::string& hash) override;
    // NodeData
    std::vector<CTransactionRef> GetBlockTxs(const std::string& hash) override;
    bool GetTx(const uint256& txid, CTransactionRef& tx, uint256& blockHash) override;
    std::vector<uint256> GetMempool() override;

private:
    struct Entry {
        bool done = false;
        bool failed = false;
        BlockHeader header;
        std::vector<CTransactionRef> txs;
        bool txsTaken = false;
    };

    void Work();
    void Fetch(int64_t height, Entry& e);
    /** Schedules the heights after `from` that may be read ahead; call with m_mutex held. */
    void ScheduleFrom(int64_t from);

    const RPCClient& m_node;
    ChainSource& m_inner;
    NodeData& m_innerData;
    const int m_ahead;
    const int m_depth;

    std::mutex m_mutex;
    std::condition_variable m_work;  // a height to fetch or stop
    std::condition_variable m_ready; // a fetch finished
    std::map<int64_t, Entry> m_entries;      // by height
    std::map<std::string, int64_t> m_byHash; // finished entries
    std::vector<int64_t> m_queue;            // heights not started yet, lowest last
    int64_t m_nodeHeight = -1;
    int64_t m_scheduledTo = -1;
    bool m_stop = false;
    std::vector<std::thread> m_threads;
};

#endif // OMNILITE_STANDALONE_PREFETCH_H
