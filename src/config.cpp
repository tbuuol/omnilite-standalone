// Copyright (c) 2026 The OmniLite Standalone developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <config.h>

#include <chainparams.h>

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

const char* const CONFIG_FILENAME = "omnilite-standalone.conf";

namespace {

/** Keys allowed in the config file and as -key=value. */
const std::set<std::string> SETTINGS = {
    "network", "rpcconnect", "rpcport", "rpcuser", "rpcpassword", "rpccookiefile",
    "pollinterval", "undodepth",
    "omniactivationallowsender", "omnialertallowsender", "omnidebug",
    "api", "apibind", "apiport", "apiuser", "apipassword", "apisend",
};

/** The API listens 100 above the node's default port: 9432, test 19432, regtest 19543. */
const int API_PORT_OFFSET = 100;

std::string Trim(const std::string& s)
{
    const char* ws = " \t\r\n";
    size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

std::string HomeDir()
{
    const char* home = std::getenv("HOME");
    return home && *home ? home : ".";
}

std::string JoinPath(const std::string& a, const std::string& b)
{
    if (a.empty() || a.back() == '/') return a + b;
    return a + "/" + b;
}

/** Splits a comma-separated list and drops empty entries. */
std::vector<std::string> SplitList(const std::string& value)
{
    std::vector<std::string> out;
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = Trim(item);
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

int ParseInt(const std::string& key, const std::string& value, int min)
{
    char* end = nullptr;
    errno = 0;
    long n = std::strtol(value.c_str(), &end, 10);
    if (value.empty() || *end != '\0' || errno != 0 || n < min || n > 1000000000) {
        throw ConfigError(key + ": want a whole number >= " + std::to_string(min) + ", got \"" + value + "\"");
    }
    return static_cast<int>(n);
}

bool ParseBool(const std::string& key, const std::string& value)
{
    if (value == "1" || value.empty()) return true; // -api alone switches it on
    if (value == "0") return false;
    throw ConfigError(key + ": want 0 or 1, got \"" + value + "\"");
}

/** Reads key=value lines. Returns false if the file does not exist. */
bool ReadConfigFile(const std::string& path, std::map<std::string, std::string>& values)
{
    std::ifstream file(path);
    if (!file) return false;
    std::string line;
    int lineNo = 0;
    while (std::getline(file, line)) {
        ++lineNo;
        line = Trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            throw ConfigError(path + ":" + std::to_string(lineNo) + ": expected key=value");
        }
        std::string key = Trim(line.substr(0, eq));
        if (!SETTINGS.count(key)) {
            throw ConfigError(path + ":" + std::to_string(lineNo) + ": unknown setting \"" + key + "\"");
        }
        values[key] = Trim(line.substr(eq + 1));
    }
    return true;
}

} // namespace

std::string Config::RPCURL() const
{
    return "http://" + rpcConnect + ":" + std::to_string(rpcPort);
}

std::string Config::NetDir() const
{
    return JoinPath(dataDir, Params().NetworkIDString());
}

std::string Config::DBPath() const
{
    return JoinPath(NetDir(), "index");
}

std::string Config::OmniDataDir() const
{
    return JoinPath(JoinPath(dataDir, Params().NetworkIDString()), "omnicore");
}

namespace {

/** user:password from a cookie file; what names the setting that was not given instead. */
std::pair<std::string, std::string> ReadCookie(const std::string& path, const std::string& what)
{
    std::ifstream file(path);
    std::string cookie;
    if (!file || !std::getline(file, cookie)) {
        throw ConfigError("no " + what + " set and cookie file " + path + " is not readable");
    }
    cookie = Trim(cookie);
    size_t colon = cookie.find(':');
    if (colon == std::string::npos) {
        throw ConfigError("cookie file " + path + " has no user:password");
    }
    return {cookie.substr(0, colon), cookie.substr(colon + 1)};
}

} // namespace

std::pair<std::string, std::string> Config::Credentials() const
{
    if (!rpcPassword.empty()) return {rpcUser, rpcPassword};
    return ReadCookie(rpcCookieFile.empty() ? DefaultCookieFile() : rpcCookieFile, "rpcpassword");
}

std::pair<std::string, std::string> Config::APICredentials() const
{
    if (!apiPassword.empty()) return {apiUser, apiPassword};
    return ReadCookie(APICookieFile(), "apipassword");
}

std::string Config::APICookieFile() const
{
    return JoinPath(JoinPath(dataDir, Params().NetworkIDString()), ".cookie");
}

std::string DefaultDataDir()
{
    return JoinPath(HomeDir(), ".omnilite-standalone");
}

std::string DefaultCookieFile()
{
    std::string dir = JoinPath(HomeDir(), ".litecoin");
    if (!Params().DataDirSuffix().empty()) dir = JoinPath(dir, Params().DataDirSuffix());
    return JoinPath(dir, ".cookie");
}

Config LoadConfig(const std::vector<std::string>& args)
{
    Config cfg;
    cfg.dataDir = DefaultDataDir();
    std::string confPath;
    std::map<std::string, std::string> flags;

    for (std::string arg : args) {
        if (arg.rfind("--", 0) == 0) arg.erase(0, 1);
        if (arg.empty() || arg[0] != '-') {
            throw ConfigError("unexpected argument \"" + arg + "\"");
        }
        arg.erase(0, 1);
        size_t eq = arg.find('=');
        std::string key = arg.substr(0, eq);
        std::string value = eq == std::string::npos ? "" : arg.substr(eq + 1);
        if (key == "help" || key == "h" || key == "?") {
            cfg.help = true;
        } else if (key == "check") {
            cfg.check = true;
        } else if (key == "status") {
            cfg.status = true;
        } else if (key == "exportsnapshot") {
            cfg.exportSnapshot = value;
        } else if (key == "importsnapshot") {
            cfg.importSnapshot = value;
        } else if (key == "snapshothash") {
            cfg.snapshotHash = value;
        } else if (key == "datadir") {
            cfg.dataDir = value;
        } else if (key == "conf") {
            confPath = value;
        } else if (SETTINGS.count(key)) {
            flags[key] = value;
        } else {
            throw ConfigError("unknown option -" + key + " (see -help)");
        }
    }
    if (cfg.dataDir.empty()) throw ConfigError("-datadir must not be empty");
    if (!cfg.importSnapshot.empty() && cfg.snapshotHash.empty()) {
        throw ConfigError("-importsnapshot needs -snapshothash=<consensus hash>, as -exportsnapshot printed it");
    }
    if (!cfg.exportSnapshot.empty() && !cfg.importSnapshot.empty()) throw ConfigError("-exportsnapshot and -importsnapshot exclude each other");

    std::map<std::string, std::string> values;
    std::string path = confPath.empty() ? JoinPath(cfg.dataDir, CONFIG_FILENAME) : confPath;
    if (!ReadConfigFile(path, values) && !confPath.empty()) {
        throw ConfigError("config file " + path + " not found");
    }
    for (const auto& kv : flags) values[kv.first] = kv.second;

    cfg.network = values.count("network") ? values["network"] : "main";
    try {
        SelectParams(cfg.network);
    } catch (const std::runtime_error& e) {
        throw ConfigError(e.what());
    }
    cfg.rpcPort = Params().RPCPort();
    if (values.count("rpcconnect") && !values["rpcconnect"].empty()) cfg.rpcConnect = values["rpcconnect"];
    if (values.count("rpcport")) cfg.rpcPort = ParseInt("rpcport", values["rpcport"], 1);
    cfg.rpcUser = values["rpcuser"];
    cfg.rpcPassword = values["rpcpassword"];
    cfg.rpcCookieFile = values["rpccookiefile"];
    if (values.count("pollinterval")) cfg.pollInterval = ParseInt("pollinterval", values["pollinterval"], 1);
    if (values.count("undodepth")) cfg.undoDepth = ParseInt("undodepth", values["undodepth"], 10);
    cfg.activationAllowSenders = SplitList(values["omniactivationallowsender"]);
    cfg.alertAllowSenders = SplitList(values["omnialertallowsender"]);
    cfg.omniDebug = SplitList(values["omnidebug"]);
    if (values.count("api")) cfg.api = ParseBool("api", values["api"]);
    if (values.count("apibind") && !values["apibind"].empty()) cfg.apiBind = values["apibind"];
    cfg.apiPort = Params().RPCPort() + API_PORT_OFFSET;
    if (values.count("apiport")) cfg.apiPort = ParseInt("apiport", values["apiport"], 1);
    if (cfg.apiPort > 65535) throw ConfigError("apiport: want at most 65535");
    cfg.apiUser = values["apiuser"];
    cfg.apiPassword = values["apipassword"];
    if (values.count("apisend")) cfg.apiSend = ParseBool("apisend", values["apisend"]);
    if (!cfg.apiPassword.empty() && cfg.apiUser.empty()) throw ConfigError("apipassword is set but apiuser is not");
    return cfg;
}

std::string HelpMessage()
{
    std::ostringstream s;
    s << "Usage: omnilite-standalone [options]\n\n"
      << "Indexes OmniLite transactions from any litecoind over standard RPC.\n\n"
      << "Options:\n"
      << "  -check              check the node connection and exit\n"
      << "  -status             show the status of the running program (over its API) and exit;\n"
      << "                      exit code 0 if it is synced\n"
      << "  -exportsnapshot=<f> write the index and the Omni state to the tar file f and exit;\n"
      << "                      only while the program is stopped; prints the consensus hash\n"
      << "  -importsnapshot=<f> start from the snapshot f instead of the Omni start block, then exit;\n"
      << "                      needs -snapshothash and an empty <datadir>/<net>\n"
      << "  -snapshothash=<h>   the consensus hash the imported state must have\n"
      << "  -datadir=<dir>      data directory (default: " << DefaultDataDir() << ")\n"
      << "  -conf=<file>        config file (default: <datadir>/" << CONFIG_FILENAME << ")\n"
      << "  -network=<net>      main, test or regtest (default: main)\n"
      << "  -rpcconnect=<host>  litecoind RPC host (default: 127.0.0.1)\n"
      << "  -rpcport=<port>     litecoind RPC port (default: 9332, test 19332, regtest 19443)\n"
      << "  -rpcuser=<user>     litecoind RPC user\n"
      << "  -rpcpassword=<pw>   litecoind RPC password (default: use the cookie file)\n"
      << "  -rpccookiefile=<f>  litecoind cookie file (default: ~/.litecoin/[net/].cookie)\n"
      << "  -pollinterval=<s>   seconds between checks for new blocks and mempool transactions (default: 5)\n"
      << "  -undodepth=<n>      blocks of undo data kept for reorgs (default: 100)\n"
      << "  -omniactivationallowsender=<a,b>  regtest only: extra senders of feature activations\n"
      << "  -omnialertallowsender=<a,b>       regtest only: extra senders of alerts\n"
      << "  -omnidebug=<a,b>    OmniLite debug categories, e.g. parser,tally or all\n"
      << "  -api=<0|1>          answer omni_* calls over JSON-RPC (default: 1)\n"
      << "  -apibind=<addr>     address the API listens on (default: 127.0.0.1)\n"
      << "  -apiport=<port>     API port (default: 9432, test 19432, regtest 19543)\n"
      << "  -apiuser=<user>     API user\n"
      << "  -apipassword=<pw>   API password (default: a new cookie in <datadir>/<net>/.cookie)\n"
      << "  -apisend=<0|1>      answer omni_send* and the other calls that spend from litecoind's wallet (default: 1)\n"
      << "\nEvery option except -check, -status, the snapshot options, -datadir and -conf can also be set in the config file.\n";
    return s.str();
}
