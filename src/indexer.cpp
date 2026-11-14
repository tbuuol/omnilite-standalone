// Copyright (c) 2026 The OmniLite Standalone developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <indexer.h>

#include <chain.h>
#include <chainparams.h>
#include <hash.h>
#include <logging.h>
#include <omnicore/omnicore.h>
#include <omnicore/parsing.h>
#include <rpcclient.h>
#include <store.h>
#include <streams.h>
#include <txmempool.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <validation.h>
#include <version.h>

// OmniLite's handlers (omnicore.cpp), declared in Core's validation.cpp.
int mastercore_handler_block_begin(int nBlockNow, CBlockIndex const* pBlockIndex);
int mastercore_handler_block_end(int nBlockNow, CBlockIndex const* pBlockIndex, unsigned int);
bool mastercore_handler_tx(const CTransaction& tx, int nBlock, unsigned int idx, CBlockIndex const* pBlockIndex, std::shared_ptr<std::map<COutPoint, Coin>> removedCoins);
void mastercore_handler_disc_begin(const int nHeight);

namespace {

/** Thrown for a transaction with real MWEB data, which has no length we could skip. */
struct MwebData : std::runtime_error {
    MwebData() : std::runtime_error("transaction carries MWEB data") {}
};

/** Bounds-checked reader over serialized data. */
struct Reader {
    const unsigned char* p;
    const unsigned char* end;
    void Need(uint64_t n) const
    {
        if (n > static_cast<uint64_t>(end - p)) throw std::runtime_error("unexpected end of data");
    }
    unsigned char Byte()
    {
        Need(1);
        return *p++;
    }
    void Skip(uint64_t n)
    {
        Need(n);
        p += n;
    }
    uint64_t CompactSize()
    {
        unsigned char c = Byte();
        int len = c < 253 ? 0 : c == 253 ? 2 : c == 254 ? 4 : 8;
        if (len == 0) return c;
        Need(len);
        uint64_t v = 0;
        for (int i = 0; i < len; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
        p += len;
        return v;
    }
};

/**
 * Reads one transaction in Litecoin 0.21 serialization (UnserializeTransaction
 * in primitives/transaction.h) and appends it to out as Litecoin 0.18 writes
 * it: the MWEB flag of a HogEx transaction and its empty MWEB part are dropped,
 * as for peers without MWEB. Throws MwebData for real MWEB data.
 */
void ReadTx018(Reader& r, std::vector<unsigned char>& out)
{
    const unsigned char* start = r.p;
    r.Skip(4); // nVersion
    const unsigned char* flagPos = nullptr;
    unsigned char flags = 0;
    uint64_t nIn = r.CompactSize();
    auto inputs = [&](uint64_t n) {
        for (uint64_t i = 0; i < n; ++i) {
            r.Skip(36);              // prevout
            r.Skip(r.CompactSize()); // scriptSig
            r.Skip(4);               // nSequence
        }
    };
    auto outputs = [&] {
        uint64_t n = r.CompactSize();
        for (uint64_t i = 0; i < n; ++i) {
            r.Skip(8);               // nValue
            r.Skip(r.CompactSize()); // scriptPubKey
        }
    };
    if (nIn == 0) {
        flagPos = r.p;
        flags = r.Byte();
        if (flags != 0) {
            nIn = r.CompactSize();
            inputs(nIn);
            outputs();
        }
    } else {
        inputs(nIn);
        outputs();
    }
    if (flags & 1) {
        for (uint64_t i = 0; i < nIn; ++i) {
            uint64_t items = r.CompactSize();
            for (uint64_t j = 0; j < items; ++j) r.Skip(r.CompactSize());
        }
    }
    const unsigned char* mwebPos = r.p;
    bool hogEx = false;
    if (flags & 8) {
        if (r.Byte() != 0) throw MwebData(); // OptionalPtr<mw::Transaction> is set
        hogEx = true;
    }
    if (flags & ~9) throw std::runtime_error("unknown optional data in transaction");
    r.Skip(4); // nLockTime
    if (!hogEx) {
        out.insert(out.end(), start, r.p);
        return;
    }
    // HogEx: same bytes with the MWEB flag and its null byte removed; without
    // a witness the marker and flag go away entirely.
    const unsigned char rest = flags & 1;
    out.insert(out.end(), start, flagPos - 1); // nVersion
    if (rest) {
        out.push_back(0);
        out.push_back(rest);
        out.insert(out.end(), flagPos + 1, mwebPos);
    } else {
        out.insert(out.end(), flagPos + 1, mwebPos);
    }
    out.insert(out.end(), mwebPos + 1, r.p); // nLockTime
}

CTransactionRef Decode018(const std::vector<unsigned char>& data)
{
    CDataStream ss(data, SER_NETWORK, PROTOCOL_VERSION);
    CMutableTransaction mtx;
    ss >> mtx;
    if (!ss.empty()) throw std::runtime_error("extra data after the transaction");
    return MakeTransactionRef(std::move(mtx));
}

} // namespace

CTransactionRef DecodeTx(const std::string& hex)
{
    if (!IsHex(hex)) throw std::runtime_error("transaction is not hex");
    std::vector<unsigned char> data = ParseHex(hex);
    Reader r{data.data(), data.data() + data.size()};
    std::vector<unsigned char> tx;
    ReadTx018(r, tx);
    if (r.p != r.end) throw std::runtime_error("extra data after the transaction");
    return Decode018(tx);
}

bool DecodeBlockTxs(const std::vector<unsigned char>& block, const std::string& hash, std::vector<CTransactionRef>& txs)
{
    if (block.size() < 80) throw std::runtime_error("block shorter than its header");
    uint256 h;
    CHash256().Write(block.data(), 80).Finalize(h.begin());
    if (h.GetHex() != hash) throw std::runtime_error("block data hashes to " + h.GetHex() + ", expected " + hash);
    Reader r{block.data() + 80, block.data() + block.size()};
    uint64_t n = r.CompactSize();
    txs.clear();
    txs.reserve(n);
    std::vector<unsigned char> tx;
    for (uint64_t i = 0; i < n; ++i) {
        tx.clear();
        try {
            ReadTx018(r, tx);
        } catch (const MwebData&) {
            return false;
        }
        try {
            txs.push_back(Decode018(tx));
        } catch (const std::exception& e) {
            // As the verbose path: the position in the block still counts.
            LogWarn("block %s: transaction %u skipped, not readable as Litecoin 0.18 (%s)", hash, (unsigned)i, e.what());
            txs.push_back(nullptr);
        }
    }
    // What follows is the MWEB block (Litecoin 0.21); OmniLite does not see it.
    return true;
}

// --- RPCNodeData ---

std::vector<CTransactionRef> RPCNodeData::GetBlockTxs(const std::string& hash)
{
    // The raw block is a fraction of the work for the node and for us; the
    // verbose one stays as fallback for blocks with MWEB data in a transaction.
    {
        UniValue params(UniValue::VARR);
        params.push_back(hash);
        params.push_back(0);
        std::string hex;
        try {
            hex = m_node.Call("getblock", params).get_str();
        } catch (const RPCError& e) {
            throw NodeError("getblock " + hash + ": " + e.what());
        } catch (const NodeUnreachable& e) {
            throw NodeError(e.what());
        }
        if (!IsHex(hex)) throw NodeError("getblock " + hash + ": not hex");
        std::vector<CTransactionRef> txs;
        if (DecodeBlockTxs(ParseHex(hex), hash, txs)) return txs;
        LogWarn("block %s: MWEB data in a transaction, reading it verbosely", hash);
    }
    UniValue params(UniValue::VARR);
    params.push_back(hash);
    params.push_back(2);
    UniValue block;
    try {
        block = m_node.Call("getblock", params);
    } catch (const RPCError& e) {
        throw NodeError("getblock " + hash + ": " + e.what());
    } catch (const NodeUnreachable& e) {
        throw NodeError(e.what());
    }
    if (find_value(block, "hash").get_str() != hash) throw NodeError("node returned another block for " + hash);

    std::vector<CTransactionRef> txs;
    const UniValue blockTxs = find_value(block, "tx");
    for (const UniValue& t : blockTxs.getValues()) {
        const std::string txid = find_value(t, "txid").get_str();
        try {
            CTransactionRef ptx = DecodeTx(find_value(t, "hex").get_str());
            if (ptx->GetHash().GetHex() != txid) throw std::runtime_error("decodes to " + ptx->GetHash().GetHex());
            txs.push_back(ptx);
        } catch (const std::exception& e) {
            // Transactions with MWEB data (Litecoin 0.21) do not exist for
            // OmniLite 0.18; the position in the block still counts.
            LogWarn("block %s: transaction %s skipped, not readable as Litecoin 0.18 (%s)", hash, txid, e.what());
            txs.push_back(nullptr);
        }
    }
    return txs;
}

bool RPCNodeData::GetTx(const uint256& txid, CTransactionRef& tx, uint256& blockHash)
{
    UniValue params(UniValue::VARR);
    params.push_back(txid.GetHex());
    params.push_back(UniValue(true));
    UniValue r;
    try {
        r = m_node.Call("getrawtransaction", params);
    } catch (const RPCError& e) {
        if (e.code == RPC_INVALID_ADDRESS_OR_KEY) return false; // unknown transaction
        throw NodeError("getrawtransaction " + txid.GetHex() + ": " + e.what());
    } catch (const NodeUnreachable& e) {
        throw NodeError(e.what());
    }
    tx = DecodeTx(find_value(r, "hex").get_str());
    const UniValue hash = find_value(r, "blockhash");
    blockHash = hash.isStr() ? uint256S(hash.get_str()) : uint256();
    return true;
}

std::vector<uint256> RPCNodeData::GetMempool()
{
    UniValue r;
    try {
        r = m_node.Call("getrawmempool");
    } catch (const RPCError& e) {
        throw NodeError(std::string("getrawmempool: ") + e.what());
    } catch (const NodeUnreachable& e) {
        throw NodeError(e.what());
    }
    std::vector<uint256> txids;
    for (const UniValue& id : r.getValues()) txids.push_back(uint256S(id.get_str()));
    return txids;
}

// --- OmniIndexer ---

OmniIndexer::OmniIndexer(NodeData& node, CStore& store) : m_node(node), m_store(store) {}

OmniIndexer::~OmniIndexer()
{
    g_readBlock = nullptr;
    g_getTransaction = nullptr;
    g_blockTxs = nullptr;
    if (m_started) mastercore_shutdown();
    LOCK(cs_main);
    chainActive.SetTip(nullptr);
    mapBlockIndex.clear();
}

CBlockIndex* OmniIndexer::AddIndex(int64_t height, const std::string& hash, int64_t time)
{
    uint256 h = uint256S(hash);
    auto it = mapBlockIndex.find(h);
    if (it != mapBlockIndex.end()) return it->second; // seen before, e.g. a block that comes back after a reorg
    m_index.emplace_back();
    CBlockIndex* pindex = &m_index.back();
    pindex->nHeight = static_cast<int>(height);
    pindex->nTime = static_cast<uint32_t>(time);
    pindex->pprev = chainActive.Tip();
    it = mapBlockIndex.emplace(h, pindex).first;
    pindex->phashBlock = &it->first;
    return pindex;
}

void OmniIndexer::Start(const fs::path& dataDir, const OmniOptions& options)
{
    fs::create_directories(dataDir);
    SetDataDir(dataDir);
    gArgs.ClearArgs();
    if (!options.activationAllowSenders.empty()) gArgs.ForceSetMultiArg("-omniactivationallowsender", options.activationAllowSenders);
    if (!options.alertAllowSenders.empty()) gArgs.ForceSetMultiArg("-omnialertallowsender", options.alertAllowSenders);
    if (!options.debug.empty()) gArgs.ForceSetMultiArg("-omnidebug", options.debug);

    g_readBlock = [this](const CBlockIndex* pindex, CBlock& block) { return ReadBlock(pindex, block.vtx); };
    g_getTransaction = [this](const uint256& txid, CTransactionRef& tx, uint256& blockHash) { return GetTransaction(txid, tx, blockHash); };
    g_blockTxs = [this](const uint256& blockHash) { return m_node.GetBlockTxs(blockHash.GetHex()); };

    // The active chain is what the store holds: OmniLite's state belongs to it.
    {
        LOCK(cs_main);
        std::optional<BlockInfo> tip = m_store.Tip();
        if (tip) {
            for (int64_t h = Params().OmniGenesisBlock(); h <= tip->height; ++h) {
                std::optional<BlockInfo> b = m_store.BlockAt(h);
                if (!b) continue; // the store may start later on test networks
                chainActive.SetTip(AddIndex(b->height, b->hash, b->time));
            }
        }
    }

    m_started = true;
    try {
        Processing processing(*this);
        mastercore_init();
    } catch (const NodeError& e) {
        throw FatalError(std::string("loading the Omni state: ") + e.what());
    }
    if (NodeAborted()) throw FatalError("OmniLite stopped while loading its state, see the log");
}

void OmniIndexer::ClearBlockCaches()
{
    m_blockTxs.clear();
    m_replayTxs.clear();
    m_prefetched.clear();
}

bool OmniIndexer::ReadBlock(const CBlockIndex* pindex, std::vector<CTransactionRef>& txs)
{
    // OmniLite replays blocks after a reorg or at start-up; the node must
    // answer, a missing block would leave the state incomplete.
    txs = m_node.GetBlockTxs(pindex->GetBlockHash().GetHex());
    // Only a replay keeps the block for the lookups that follow. API calls
    // read blocks too (omni_listblocktransactions); they must leave nothing
    // behind, or later lookups would take its transactions for unconfirmed.
    if (m_processing) {
        m_replayTxs.clear();
        for (const CTransactionRef& tx : txs) {
            if (tx) m_replayTxs[tx->GetHash()] = tx;
        }
    }
    return true;
}

bool OmniIndexer::GetTransaction(const uint256& txid, CTransactionRef& tx, uint256& blockHash)
{
    // The caches only hold something while a block is being processed;
    // API calls in between go to the node.
    for (const auto* txs : {&m_blockTxs, &m_replayTxs}) {
        auto it = txs->find(txid);
        if (it != txs->end()) {
            tx = it->second;
            blockHash = uint256(); // the block being processed
            return true;
        }
    }
    auto pit = m_prefetched.find(txid);
    if (pit != m_prefetched.end()) {
        tx = pit->second.first;
        blockHash = pit->second.second;
        return tx != nullptr;
    }
    return m_node.GetTx(txid, tx, blockHash);
}

OmniIndexer::Prefetched OmniIndexer::Prefetch(const std::vector<CTransactionRef>& txs, const std::map<uint256, CTransactionRef>& blockTxs, int64_t height)
{
    Prefetched prefetched;
    for (const CTransactionRef& tx : txs) {
        if (!tx || tx->IsCoinBase()) continue;
        if (mastercore::GetEncodingClass(*tx, static_cast<int>(height)) == NO_MARKER) continue;
        for (const CTxIn& in : tx->vin) {
            const uint256& h = in.prevout.hash;
            if (blockTxs.count(h) || prefetched.count(h)) continue;
            CTransactionRef prev;
            uint256 blockHash;
            if (!m_node.GetTx(h, prev, blockHash)) prev = nullptr; // OmniLite then rejects the transaction (-101)
            prefetched[h] = {prev, blockHash};
        }
    }
    return prefetched;
}

void OmniIndexer::ConnectBlock(CBlockTx&, const BlockHeader& header)
{
    // Everything that needs the node comes first: a failure here leaves the
    // state untouched and the syncer retries. It runs without cs_main, so
    // the API keeps answering meanwhile.
    std::vector<CTransactionRef> txs = m_node.GetBlockTxs(header.hash);
    std::map<uint256, CTransactionRef> blockTxs;
    for (const CTransactionRef& tx : txs) {
        if (tx) blockTxs[tx->GetHash()] = tx;
    }
    Prefetched prefetched = Prefetch(txs, blockTxs, header.height);

    LOCK(cs_main);
    Processing processing(*this);
    m_blockTxs.swap(blockTxs);
    m_prefetched.swap(prefetched);

    CBlockIndex* tip = chainActive.Tip();
    if (tip && tip->GetBlockHash().GetHex() != header.prevHash) {
        throw FatalError("block " + header.hash + " does not extend OmniLite's chain tip " + tip->GetBlockHash().GetHex());
    }
    CBlockIndex* pindex = AddIndex(header.height, header.hash, header.time);
    pindex->pprev = tip;

    // As ConnectTip: begin with the previous height, then the new tip, the
    // transactions and the end of the block.
    try {
        unsigned int nNumMetaTxs = mastercore_handler_block_begin(chainActive.Height(), pindex);
        chainActive.SetTip(pindex);
        for (unsigned int idx = 0; idx < txs.size(); ++idx) {
            if (txs[idx] && mastercore_handler_tx(*txs[idx], pindex->nHeight, idx, pindex, nullptr)) ++nNumMetaTxs;
        }
        mastercore_handler_block_end(pindex->nHeight, pindex, nNumMetaTxs);
    } catch (const std::exception& e) {
        throw FatalError("block " + std::to_string(header.height) + " failed half way, restart to reload the last saved state: " + e.what());
    }

    // As Core after ConnectTip: the block's transactions leave the mempool
    // and the marker cache (validation.cpp, ActivateBestChain).
    std::vector<uint256> txids;
    for (const CTransactionRef& tx : txs) {
        if (!tx) continue;
        txids.push_back(tx->GetHash());
        RemoveFromMarkerCache(tx->GetHash());
    }
    mempool.RemoveForBlock(txids);
    if (NodeAborted()) throw FatalError("OmniLite stopped at block " + std::to_string(header.height) + ", see the log");
}

void OmniIndexer::DisconnectBlock(const BlockInfo& block)
{
    // As DisconnectTip: the previous block becomes the tip, then OmniLite
    // learns about it and rewinds its state when the next block comes.
    LOCK(cs_main);
    CBlockIndex* tip = chainActive.Tip();
    if (!tip || tip->GetBlockHash().GetHex() != block.hash) {
        throw FatalError("disconnected block " + block.hash + " is not OmniLite's chain tip");
    }
    chainActive.SetTip(tip->pprev);
    mastercore_handler_disc_begin(block.height);
}
