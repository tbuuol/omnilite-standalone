// Copyright (c) 2026 The OmniLite Standalone developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <config.h>

#include <boost/test/unit_test.hpp>

#include <cstdio>
#include <fstream>
#include <unistd.h>

namespace {

/** A temporary data directory, removed at the end. */
struct TempDir {
    std::string path;
    TempDir()
    {
        char tmpl[] = "/tmp/omnilite-test-XXXXXX";
        path = mkdtemp(tmpl);
    }
    ~TempDir()
    {
        std::remove((path + "/" + CONFIG_FILENAME).c_str());
        std::remove((path + "/cookie").c_str());
        rmdir(path.c_str());
    }
    void Write(const std::string& name, const std::string& text) const
    {
        std::ofstream(path + "/" + name) << text;
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(config_tests)

BOOST_AUTO_TEST_CASE(defaults)
{
    TempDir dir;
    Config cfg = LoadConfig({"-datadir=" + dir.path});
    BOOST_CHECK_EQUAL(cfg.network, "main");
    BOOST_CHECK_EQUAL(cfg.rpcPort, 9332);
    BOOST_CHECK_EQUAL(cfg.RPCURL(), "http://127.0.0.1:9332");
    BOOST_CHECK_EQUAL(cfg.pollInterval, 5);
    BOOST_CHECK_EQUAL(cfg.undoDepth, 100);
    BOOST_CHECK(cfg.apiSend);
    BOOST_CHECK_EQUAL(cfg.DBPath(), dir.path + "/main/index");
}

BOOST_AUTO_TEST_CASE(file_and_flags)
{
    TempDir dir;
    dir.Write(CONFIG_FILENAME,
        "# comment\n"
        "network = regtest\n"
        "rpcuser=omni   # trailing comment\n"
        "rpcpassword=secret\n"
        "pollinterval=7\n"
        "apisend=0\n"
        "omnialertallowsender = a, b\n");
    Config cfg = LoadConfig({"-datadir=" + dir.path, "--pollinterval=2", "-check"});
    BOOST_CHECK_EQUAL(cfg.network, "regtest");
    BOOST_CHECK_EQUAL(Params().NetworkIDString(), "regtest");
    BOOST_CHECK_EQUAL(cfg.rpcPort, 19443);
    BOOST_CHECK_EQUAL(cfg.pollInterval, 2); // the flag wins
    BOOST_CHECK(cfg.check);
    BOOST_CHECK(!cfg.apiSend);
    BOOST_CHECK(cfg.alertAllowSenders == std::vector<std::string>({"a", "b"}));
    BOOST_CHECK(cfg.activationAllowSenders.empty());
    BOOST_CHECK(cfg.Credentials() == std::make_pair(std::string("omni"), std::string("secret")));
    SelectParams("main");
}

BOOST_AUTO_TEST_CASE(cookie)
{
    TempDir dir;
    dir.Write("cookie", "__cookie__:abc123\n");
    Config cfg = LoadConfig({"-datadir=" + dir.path, "-rpccookiefile=" + dir.path + "/cookie"});
    BOOST_CHECK(cfg.Credentials() == std::make_pair(std::string("__cookie__"), std::string("abc123")));
}

BOOST_AUTO_TEST_CASE(errors)
{
    TempDir dir;
    BOOST_CHECK_THROW(LoadConfig({"-datadir=" + dir.path, "-network=bogus"}), ConfigError);
    BOOST_CHECK_THROW(LoadConfig({"-datadir=" + dir.path, "-undodepth=5"}), ConfigError);
    BOOST_CHECK_THROW(LoadConfig({"-datadir=" + dir.path, "-nosuchoption"}), ConfigError);
    BOOST_CHECK_THROW(LoadConfig({"-datadir=" + dir.path, "-conf=" + dir.path + "/missing.conf"}), ConfigError);
    dir.Write(CONFIG_FILENAME, "typo=1\n");
    BOOST_CHECK_THROW(LoadConfig({"-datadir=" + dir.path}), ConfigError);
    SelectParams("main");
}

BOOST_AUTO_TEST_SUITE_END()
