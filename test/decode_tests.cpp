// Copyright (c) 2026 The OmniLite Standalone developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Transactions and raw blocks in Litecoin 0.21 serialization (MWEB), as
// getblock with verbosity 0 and getrawtransaction return them.

#include <indexer.h>

#include <hash.h>
#include <streams.h>
#include <util/strencodings.h>
#include <version.h>

#include <boost/test/unit_test.hpp>

namespace {

CMutableTransaction SampleTx(bool witness)
{
    CMutableTransaction mtx;
    mtx.nVersion = 2;
    mtx.vin.resize(2);
    mtx.vin[0].prevout = COutPoint(uint256S("11"), 3);
    mtx.vin[0].scriptSig = CScript() << std::vector<unsigned char>(20, 0xab);
    mtx.vin[1].prevout = COutPoint(uint256S("22"), 0);
    mtx.vout.resize(2);
    mtx.vout[0].nValue = 12345;
    mtx.vout[0].scriptPubKey = CScript() << OP_RETURN << std::vector<unsigned char>{'o', 'm', 'n', 'i'};
    mtx.vout[1].nValue = 678;
    mtx.vout[1].scriptPubKey = CScript() << OP_TRUE;
    mtx.nLockTime = 0x01020304;
    if (witness) mtx.vin[1].scriptWitness.stack = {{1, 2, 3}, {}};
    return mtx;
}

std::vector<unsigned char> Ser(const CMutableTransaction& mtx, int flags)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION | flags);
    ss << mtx;
    return std::vector<unsigned char>(ss.begin(), ss.end());
}

/** A HogEx transaction as Litecoin 0.21 writes it: MWEB flag 8 and an empty MWEB part. */
std::vector<unsigned char> HogEx(const CMutableTransaction& mtx)
{
    const bool witness = mtx.HasWitness();
    std::vector<unsigned char> b = Ser(mtx, witness ? 0 : SERIALIZE_TRANSACTION_NO_WITNESS);
    std::vector<unsigned char> out(b.begin(), b.begin() + 4);
    out.push_back(0);
    out.push_back(witness ? 9 : 8);
    out.insert(out.end(), b.begin() + (witness ? 6 : 4), b.end() - 4);
    out.push_back(0); // OptionalPtr<mw::Transaction>: not set
    out.insert(out.end(), b.end() - 4, b.end());
    return out;
}

std::vector<unsigned char> Block(const std::vector<std::vector<unsigned char>>& txs, const std::vector<unsigned char>& trailer, std::string& hash)
{
    std::vector<unsigned char> b(80, 0x5a);
    b.push_back(static_cast<unsigned char>(txs.size()));
    for (const auto& t : txs) b.insert(b.end(), t.begin(), t.end());
    b.insert(b.end(), trailer.begin(), trailer.end());
    uint256 h;
    CHash256().Write(b.data(), 80).Finalize(h.begin());
    hash = h.GetHex();
    return b;
}

} // namespace

BOOST_AUTO_TEST_SUITE(decode_tests)

BOOST_AUTO_TEST_CASE(plain_transactions_unchanged)
{
    for (bool witness : {false, true}) {
        CMutableTransaction mtx = SampleTx(witness);
        CTransactionRef tx = DecodeTx(HexStr(Ser(mtx, 0)));
        BOOST_CHECK_EQUAL(tx->GetHash().GetHex(), (mtx.GetHash()).GetHex());
        BOOST_CHECK_EQUAL(tx->GetWitnessHash().GetHex(), (CTransaction(mtx).GetWitnessHash()).GetHex());
    }
}

BOOST_AUTO_TEST_CASE(hogex_becomes_what_old_nodes_see)
{
    for (bool witness : {false, true}) {
        CMutableTransaction mtx = SampleTx(witness);
        CTransactionRef tx = DecodeTx(HexStr(HogEx(mtx)));
        BOOST_CHECK_EQUAL(tx->GetHash().GetHex(), (mtx.GetHash()).GetHex());
        BOOST_CHECK_EQUAL(tx->GetWitnessHash().GetHex(), (CTransaction(mtx).GetWitnessHash()).GetHex());
        BOOST_CHECK(tx->vout == mtx.vout);
        BOOST_CHECK_EQUAL(tx->nLockTime, mtx.nLockTime);
    }
}

BOOST_AUTO_TEST_CASE(mweb_data_and_garbage_rejected)
{
    std::vector<unsigned char> b = HogEx(SampleTx(false));
    std::vector<unsigned char> mweb = b;
    mweb[mweb.size() - 5] = 1; // MWEB part set
    BOOST_CHECK_THROW(DecodeTx(HexStr(mweb)), std::runtime_error);
    std::vector<unsigned char> unknown = b;
    unknown[5] = 0x18; // unknown flag
    BOOST_CHECK_THROW(DecodeTx(HexStr(unknown)), std::runtime_error);
    std::vector<unsigned char> extra = Ser(SampleTx(false), SERIALIZE_TRANSACTION_NO_WITNESS);
    extra.push_back(0);
    BOOST_CHECK_THROW(DecodeTx(HexStr(extra)), std::runtime_error);
    std::vector<unsigned char> cut(b.begin(), b.end() - 1);
    BOOST_CHECK_THROW(DecodeTx(HexStr(cut)), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(raw_block_with_mweb_trailer)
{
    CMutableTransaction a = SampleTx(true), h = SampleTx(false);
    h.nLockTime = 7;
    std::string hash;
    std::vector<unsigned char> block = Block({Ser(a, 0), HogEx(h)}, {1, 2, 3, 4, 5, 6, 7, 8}, hash);
    std::vector<CTransactionRef> txs;
    BOOST_REQUIRE(DecodeBlockTxs(block, hash, txs));
    BOOST_REQUIRE_EQUAL(txs.size(), 2U);
    BOOST_CHECK_EQUAL(txs[0]->GetWitnessHash().GetHex(), (CTransaction(a).GetWitnessHash()).GetHex());
    BOOST_CHECK_EQUAL(txs[1]->GetHash().GetHex(), (h.GetHash()).GetHex());

    // Another hash than the header's: refused.
    BOOST_CHECK_THROW(DecodeBlockTxs(block, uint256S("1234").GetHex(), txs), std::runtime_error);

    // A transaction with MWEB data cannot be skipped: the caller falls back.
    std::vector<unsigned char> mweb = HogEx(h);
    mweb[mweb.size() - 5] = 1;
    block = Block({Ser(a, 0), mweb}, {}, hash);
    BOOST_CHECK(!DecodeBlockTxs(block, hash, txs));
}

BOOST_AUTO_TEST_SUITE_END()
