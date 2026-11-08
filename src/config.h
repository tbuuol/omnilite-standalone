// Copyright (c) 2026 The OmniLite Standalone developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef OMNILITE_STANDALONE_CONFIG_H
#define OMNILITE_STANDALONE_CONFIG_H

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

/** Name of the config file in the data directory. */
extern const char* const CONFIG_FILENAME;

/** A bad command line or config file. */
class ConfigError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/**
 * Settings from a litecoin.conf-style file (key=value, '#' comments) and
 * the command line (-key=value). The command line wins over the file.
 */
struct Config {
    std::string dataDir;
    std::string network = "main";

    std::string rpcConnect = "127.0.0.1";
    int rpcPort = 0;
    std::string rpcUser;
    std::string rpcPassword;
    /** Used when rpcPassword is empty; default: litecoind's .cookie. */
    std::string rpcCookieFile;

    /** Seconds between checks for new blocks once synced. */
    int pollInterval = 5;
    /** Blocks of undo data kept for reorgs. */
    int undoDepth = 100;

    // OmniLite's -omniactivationallowsender, -omnialertallowsender (they change
    // the state; for regtest only) and -omnidebug.
    std::vector<std::string> activationAllowSenders;
    std::vector<std::string> alertAllowSenders;
    std::vector<std::string> omniDebug;

    /** The JSON-RPC API (omni_* calls). */
    bool api = true;
    std::string apiBind = "127.0.0.1";
    int apiPort = 0;
    std::string apiUser;
    /** Empty: a cookie file is written at start-up, see APICookieFile. */
    std::string apiPassword;
    /** The omni_send* calls, signed and broadcast by the node's wallet. */
    bool apiSend = true;

    bool check = false;
    bool status = false;
    // -exportsnapshot, -importsnapshot, -snapshothash (command line only)
    std::string exportSnapshot;
    std::string importSnapshot;
    std::string snapshotHash;
    bool help = false;

    /** URL path of the node's RPC endpoint, e.g. http://127.0.0.1:9332. */
    std::string RPCURL() const;
    /** Directory of the selected network: <datadir>/<network>. */
    std::string NetDir() const;
    /** Database directory of the selected network. */
    std::string DBPath() const;
    /** Directory of OmniLite's own state files and databases. */
    std::string OmniDataDir() const;
    /** RPC user and password, read from the cookie file when no password is set. */
    std::pair<std::string, std::string> Credentials() const;
    /** API user and password, read from the API's cookie file when no password is set. */
    std::pair<std::string, std::string> APICredentials() const;
    /** The API's cookie file: <datadir>/<network>/.cookie. */
    std::string APICookieFile() const;
};

/** Default data directory: ~/.omnilite-standalone. */
std::string DefaultDataDir();

/** Default cookie file of litecoind for the selected network. */
std::string DefaultCookieFile();

/**
 * Parses the command line (without the program name) and the config file
 * and selects the chain parameters. Throws ConfigError.
 */
Config LoadConfig(const std::vector<std::string>& args);

/** Usage text for -help. */
std::string HelpMessage();

#endif // OMNILITE_STANDALONE_CONFIG_H
