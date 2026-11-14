// omnilite-standalone: stands in for Litecoin Core's validation.h: the chain
// state OmniLite reads. Blocks and transactions come from the node via RPC.

#ifndef OMNILITE_STANDALONE_CORESHIM_VALIDATION_H
#define OMNILITE_STANDALONE_CORESHIM_VALIDATION_H

#include <chain.h>
#include <chainparams.h>
#include <fs.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <sync.h>
#include <uint256.h>
#include <util/system.h>
#include <util/time.h>

#include <atomic>
#include <functional>
#include <map>
#include <string>

/** Default for -minrelaytxfee, minimum relay fee for transactions (Litecoin Core). */
static const unsigned int DEFAULT_MIN_RELAY_TX_FEE = 1000;

typedef std::map<uint256, CBlockIndex*> BlockMap;

extern CCriticalSection cs_main;
extern CChain chainActive;
extern BlockMap mapBlockIndex;
/** Whether Core rebuilds its block index; never here. */
extern std::atomic_bool fReindex;

/** Reads the transactions of a block of the active chain. */
bool ReadBlockFromDisk(CBlock& block, const CBlockIndex* pindex, const Consensus::Params& consensusParams);

/** Looks up a transaction and the block it is in. */
bool GetTransaction(const uint256& hash, CTransactionRef& tx, const Consensus::Params& consensusParams, uint256& hashBlock, const CBlockIndex* const blockIndex = nullptr);

/**
 * OmniLite calls this when its state cannot be trusted any more (e.g. a
 * feature it does not know gets activated). Logs, marks the state as
 * broken and asks the program to stop.
 */
void DoAbortNode(const std::string& strMessage, const std::string& userMessage = "");

/** Logs a warning (Core shows it in the GUI and getinfo). */
void DoWarning(const std::string& strWarning);

/** Whether DoAbortNode was called; processing must stop then. */
bool NodeAborted();

/** How ReadBlockFromDisk and GetTransaction reach the node; set by the indexer. */
extern std::function<bool(const CBlockIndex*, CBlock&)> g_readBlock;
extern std::function<bool(const uint256&, CTransactionRef&, uint256&)> g_getTransaction;
/** A block's transactions from the node, for TxIndex::ReadTxPos; unlike
 *  g_readBlock without the replay of OmniLite (its cache stays as it is). */
extern std::function<std::vector<CTransactionRef>(const uint256&)> g_blockTxs;

/** Height of the node's best block (Core: pindexBestHeader), -1 while unknown; set by the syncer.
 *  OmniLite stores its full state only near this height (persistence.cpp). */
extern std::atomic<int> g_bestHeaderHeight;

#endif // OMNILITE_STANDALONE_CORESHIM_VALIDATION_H
