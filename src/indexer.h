// Copyright (c) 2026 The OmniLite Standalone developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef OMNILITE_STANDALONE_INDEXER_H
#define OMNILITE_STANDALONE_INDEXER_H

// Feeds the blocks the syncer connects into OmniLite's handlers, in the
// order Litecoin Core calls them (validation.cpp, ConnectTip and
// DisconnectTip). OmniLite keeps the Omni state itself, in memory, in its
// databases and in its state files, and handles reorgs on its own.

#include <fs.h>
#include <primitives/transaction.h>
#include <syncer.h>
#include <uint256.h>

#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

class CBlockIndex;
class RPCClient;

/**
 * Decodes a transaction from hex; throws on failure. Takes Litecoin Core 0.18
 * and 0.21 serialization: a HogEx transaction (MWEB flag without MWEB data)
 * becomes the plain transaction older nodes see. Real MWEB data throws.
 */
CTransactionRef DecodeTx(const std::string& hex);

/**
 * The transactions of a raw block as getblock with verbosity 0 returns it
 * (Litecoin 0.18 or 0.21, the MWEB block after the transactions is ignored).
 * False if a transaction carries MWEB data that cannot be skipped; then the
 * caller has to fall back to the verbose block.
 */
bool DecodeBlockTxs(const std::vector<unsigned char>& block, const std::string& hash, std::vector<CTransactionRef>& txs);

/** What the indexer needs from the node. Failures to reach it throw NodeError. */
class NodeData
{
public:
    virtual ~NodeData() = default;
    /** The transactions of a block; nullptr for those Litecoin 0.18 cannot read (MWEB). */
    virtual std::vector<CTransactionRef> GetBlockTxs(const std::string& hash) = 0;
    /** A transaction and the block it is in; false if the node does not know it. */
    virtual bool GetTx(const uint256& txid, CTransactionRef& tx, uint256& blockHash) = 0;
    /** The txids in the node's mempool. */
    virtual std::vector<uint256> GetMempool() = 0;
};

/** NodeData over RPC: getblock (verbosity 2) and getrawtransaction (needs -txindex). */
class RPCNodeData : public NodeData
{
public:
    explicit RPCNodeData(const RPCClient& node) : m_node(node) {}
    std::vector<CTransactionRef> GetBlockTxs(const std::string& hash) override;
    bool GetTx(const uint256& txid, CTransactionRef& tx, uint256& blockHash) override;
    std::vector<uint256> GetMempool() override;

private:
    const RPCClient& m_node;
};

/** The settings OmniLite reads as -omni* arguments. */
struct OmniOptions {
    std::vector<std::string> activationAllowSenders;
    std::vector<std::string> alertAllowSenders;
    /** -omnidebug categories. */
    std::vector<std::string> debug;
};

class OmniIndexer : public Processor
{
public:
    OmniIndexer(NodeData& node, CStore& store);
    ~OmniIndexer() override;

    /**
     * Rebuilds the chain index from the store and starts OmniLite
     * (mastercore_init), which loads its saved state from dataDir and catches
     * up to the store's tip.
     */
    void Start(const fs::path& dataDir, const OmniOptions& options);

    void ConnectBlock(CBlockTx& tx, const BlockHeader& header) override;
    void DisconnectBlock(const BlockInfo& block) override;

private:
    /** Inputs fetched ahead of a block: txid -> transaction (nullptr if unknown) and its block. */
    using Prefetched = std::map<uint256, std::pair<CTransactionRef, uint256>>;

    CBlockIndex* AddIndex(int64_t height, const std::string& hash, int64_t time);
    /** Fetches the inputs of the transactions with an Omni marker. */
    Prefetched Prefetch(const std::vector<CTransactionRef>& txs, const std::map<uint256, CTransactionRef>& blockTxs, int64_t height);
    bool GetTransaction(const uint256& txid, CTransactionRef& tx, uint256& blockHash);
    bool ReadBlock(const CBlockIndex* pindex, std::vector<CTransactionRef>& txs);
    void ClearBlockCaches();

    NodeData& m_node;
    CStore& m_store;
    bool m_started = false;
    std::list<CBlockIndex> m_index;
    /**
     * Transactions of the block being processed, of the block OmniLite
     * replays and the inputs fetched for them. Changed under cs_main only,
     * and empty between blocks.
     */
    std::map<uint256, CTransactionRef> m_blockTxs;
    std::map<uint256, CTransactionRef> m_replayTxs;
    Prefetched m_prefetched;
    /** Whether OmniLite runs for the indexer (start-up, a block), not for an API call. */
    bool m_processing = false;

    /** While it lives the indexer is processing; the caches are empty before and after. */
    struct Processing {
        OmniIndexer& self;
        explicit Processing(OmniIndexer& s) : self(s)
        {
            self.ClearBlockCaches();
            self.m_processing = true;
        }
        ~Processing()
        {
            self.m_processing = false;
            self.ClearBlockCaches();
        }
    };
};

#endif // OMNILITE_STANDALONE_INDEXER_H
