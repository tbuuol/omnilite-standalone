// Copyright (c) 2026 The OmniLite Standalone developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The state behind the Core stand-in headers.

#if defined(NDEBUG)
# error "OmniLite Standalone cannot be compiled without assertions: OmniLite relies on side effects inside assert()."
#endif

#include <chain.h>
#include <core_io.h>
#include <index/txindex.h>
#include <random.h>
#include <shutdown.h>
#include <streams.h>
#include <txmempool.h>
#include <ui_interface.h>
#include <util/system.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>
#include <version.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <random>
#include <thread>

// --- util/system.h ---

ArgsManager gArgs;

std::vector<std::string> ArgsManager::GetArgs(const std::string& arg) const
{
    auto it = m_args.find(arg);
    return it == m_args.end() ? std::vector<std::string>() : it->second;
}

std::string ArgsManager::GetArg(const std::string& arg, const std::string& def) const
{
    auto it = m_args.find(arg);
    return it == m_args.end() || it->second.empty() ? def : it->second.back();
}

int64_t ArgsManager::GetArg(const std::string& arg, int64_t def) const
{
    auto it = m_args.find(arg);
    return it == m_args.end() || it->second.empty() ? def : atoi64(it->second.back());
}

bool ArgsManager::GetBoolArg(const std::string& arg, bool def) const
{
    auto it = m_args.find(arg);
    if (it == m_args.end() || it->second.empty()) return def;
    const std::string& v = it->second.back();
    return v.empty() || atoi(v) != 0; // like Core's InterpretBool
}

namespace {
fs::path g_dataDir = fs::current_path();
} // namespace

const fs::path& GetDataDir(bool)
{
    return g_dataDir;
}

void SetDataDir(const fs::path& path)
{
    g_dataDir = path;
}

bool TryCreateDirectories(const fs::path& p)
{
    return fs::create_directories(p);
}

fs::path AbsPathForConfigVal(const fs::path& path, bool)
{
    return fs::absolute(GetDataDir() / path);
}

bool RenameOver(fs::path src, fs::path dest)
{
    std::error_code ec;
    fs::rename(src, dest, ec);
    return !ec;
}

namespace {
const int64_t g_startupTime = std::time(nullptr);
} // namespace

int64_t GetStartupTime()
{
    return g_startupTime;
}

// --- random.h ---

void GetRandBytes(unsigned char* buf, int num)
{
    std::random_device rd; // /dev/urandom on Linux
    for (int i = 0; i < num; ++i) buf[i] = static_cast<unsigned char>(rd());
}

// --- util/time.h ---

int64_t GetTime()
{
    return std::time(nullptr);
}

int64_t GetTimeMillis()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t GetTimeMicros()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void MilliSleep(int64_t n)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(n));
}

std::string FormatISO8601DateTime(int64_t nTime)
{
    std::time_t t = nTime;
    std::tm ts;
    gmtime_r(&t, &ts);
    return strprintf("%04i-%02i-%02iT%02i:%02i:%02iZ", ts.tm_year + 1900, ts.tm_mon + 1, ts.tm_mday, ts.tm_hour, ts.tm_min, ts.tm_sec);
}

// --- chain.h, validation.h ---

void CChain::SetTip(CBlockIndex* pindex)
{
    if (pindex == nullptr) {
        vChain.clear();
        return;
    }
    vChain.resize(pindex->nHeight + 1);
    while (pindex && vChain[pindex->nHeight] != pindex) {
        vChain[pindex->nHeight] = pindex;
        pindex = pindex->pprev;
    }
}

CCriticalSection cs_main;
CChain chainActive;
BlockMap mapBlockIndex;
std::atomic_bool fReindex(false);

std::unique_ptr<TxIndex> g_txindex = std::make_unique<TxIndex>();

std::function<bool(const CBlockIndex*, CBlock&)> g_readBlock;
std::function<bool(const uint256&, CTransactionRef&, uint256&)> g_getTransaction;
std::function<std::vector<CTransactionRef>(const uint256&)> g_blockTxs;
std::atomic<int> g_bestHeaderHeight(-1);

bool ReadBlockFromDisk(CBlock& block, const CBlockIndex* pindex, const Consensus::Params&)
{
    return g_readBlock && g_readBlock(pindex, block);
}

bool GetTransaction(const uint256& hash, CTransactionRef& tx, const Consensus::Params&, uint256& hashBlock, const CBlockIndex* const)
{
    return g_getTransaction && g_getTransaction(hash, tx, hashBlock);
}

unsigned int TxIndex::ReadTxPos(const uint256& txid) const
{
    CTransactionRef tx;
    uint256 hashBlock;
    if (!g_getTransaction || !g_blockTxs || !g_getTransaction(txid, tx, hashBlock) || hashBlock.IsNull()) return 0;
    std::vector<CTransactionRef> txs;
    try {
        txs = g_blockTxs(hashBlock);
    } catch (const std::exception&) {
        return 0; // only the order of wallet transactions in one block suffers
    }
    for (size_t i = 0; i < txs.size(); ++i) {
        if (txs[i] && txs[i]->GetHash() == txid) return i;
    }
    return 0;
}

// --- shutdown.h, ui_interface.h, txmempool.h, core_io.h ---

namespace {
std::atomic<bool> g_shutdown{false};
std::atomic<bool> g_aborted{false};
} // namespace

void StartShutdown() { g_shutdown = true; }
bool ShutdownRequested() { return g_shutdown; }
const std::atomic<bool>& ShutdownFlag() { return g_shutdown; }

void DoAbortNode(const std::string& strMessage, const std::string& userMessage)
{
    LogError("%s", userMessage.empty() ? strMessage : userMessage);
    g_aborted = true;
    StartShutdown();
}

void DoWarning(const std::string& strWarning)
{
    LogWarn("%s", strWarning);
}

bool NodeAborted() { return g_aborted; }

CClientUIInterface uiInterface;
CTxMemPool mempool;

void CTxMemPool::queryHashes(std::vector<uint256>& vtxid) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    vtxid.assign(m_txids.begin(), m_txids.end());
}

bool CTxMemPool::exists(const uint256& hash) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_txids.count(hash) > 0;
}

void CTxMemPool::SetHashes(const std::set<uint256>& txids)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_txids = txids;
}

void CTxMemPool::Add(const uint256& txid)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_txids.insert(txid);
}

void CTxMemPool::RemoveForBlock(const std::vector<uint256>& txids)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const uint256& txid : txids) m_txids.erase(txid);
}

std::string ScriptToAsmStr(const CScript& script, const bool)
{
    return HexStr(script.begin(), script.end());
}

// From Core's core_read.cpp.
static bool CheckTxScriptsSanity(const CMutableTransaction& tx)
{
    // Check input scripts for non-coinbase txs
    if (!CTransaction(tx).IsCoinBase()) {
        for (unsigned int i = 0; i < tx.vin.size(); i++) {
            if (!tx.vin[i].scriptSig.HasValidOps() || tx.vin[i].scriptSig.size() > MAX_SCRIPT_SIZE) {
                return false;
            }
        }
    }
    // Check output scripts
    for (unsigned int i = 0; i < tx.vout.size(); i++) {
        if (!tx.vout[i].scriptPubKey.HasValidOps() || tx.vout[i].scriptPubKey.size() > MAX_SCRIPT_SIZE) {
            return false;
        }
    }

    return true;
}

bool DecodeHexTx(CMutableTransaction& tx, const std::string& hex_tx, bool try_no_witness, bool try_witness)
{
    if (!IsHex(hex_tx)) {
        return false;
    }

    std::vector<unsigned char> txData(ParseHex(hex_tx));

    if (try_no_witness) {
        CDataStream ssData(txData, SER_NETWORK, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS);
        try {
            ssData >> tx;
            if (ssData.eof() && (!try_witness || CheckTxScriptsSanity(tx))) {
                return true;
            }
        } catch (const std::exception&) {
            // Fall through.
        }
    }

    if (try_witness) {
        CDataStream ssData(txData, SER_NETWORK, PROTOCOL_VERSION);
        try {
            ssData >> tx;
            if (ssData.empty()) {
                return true;
            }
        } catch (const std::exception&) {
            // Fall through.
        }
    }

    return false;
}

// From Core's core_write.cpp.
UniValue ValueFromAmount(const CAmount& amount)
{
    bool sign = amount < 0;
    int64_t n_abs = (sign ? -amount : amount);
    int64_t quotient = n_abs / COIN;
    int64_t remainder = n_abs % COIN;
    return UniValue(UniValue::VNUM,
            strprintf("%s%d.%08d", sign ? "-" : "", quotient, remainder));
}

std::string EncodeHexTx(const CTransaction& tx, const int serializeFlags)
{
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION | serializeFlags);
    ssTx << tx;
    return HexStr(ssTx.begin(), ssTx.end());
}
