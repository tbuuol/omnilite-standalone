cmake_minimum_required(VERSION 3.22)
project(omnilite-standalone VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Like Litecoin Core: OmniLite relies on side effects inside assert(), so
# assertions must stay on in every build type.
foreach(flags CMAKE_C_FLAGS_RELEASE CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_C_FLAGS_MINSIZEREL
              CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL)
  string(REPLACE "-DNDEBUG" "" ${flags} "${${flags}}")
endforeach()

option(OMNILITE_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(OMNILITE_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# A release binary: runs on any x86-64 Linux, whatever its glibc and libraries.
option(OMNILITE_STATIC "Link everything statically" OFF)
if(OMNILITE_STATIC)
  set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
  add_link_options(-static)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(EVENT REQUIRED IMPORTED_TARGET libevent)
pkg_check_modules(EVENT_PTHREADS REQUIRED IMPORTED_TARGET libevent_pthreads)
find_package(Boost 1.64 REQUIRED CONFIG COMPONENTS unit_test_framework)
find_path(LEVELDB_INCLUDE_DIR leveldb/db.h REQUIRED)
find_library(LEVELDB_LIBRARY leveldb REQUIRED)
if(OMNILITE_STATIC)
  # A static leveldb does not bring its dependency along.
  find_library(SNAPPY_LIBRARY snappy REQUIRED)
  list(APPEND LEVELDB_LIBRARY ${SNAPPY_LIBRARY})
endif()

set(VENDOR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/core)

# --- libsecp256k1, configured like Litecoin Core's build on 64-bit Linux ---
add_library(secp256k1 STATIC ${VENDOR}/secp256k1/src/secp256k1.c)
target_include_directories(secp256k1
  SYSTEM PUBLIC ${VENDOR}/secp256k1/include
  PRIVATE ${VENDOR}/secp256k1 ${VENDOR}/secp256k1/src)
target_compile_definitions(secp256k1 PRIVATE
  USE_NUM_NONE=1 USE_FIELD_INV_BUILTIN=1 USE_SCALAR_INV_BUILTIN=1
  USE_FIELD_5X52=1 USE_SCALAR_4X64=1 HAVE___INT128=1
  ENABLE_MODULE_RECOVERY=1)
target_compile_options(secp256k1 PRIVATE -w)

# --- UniValue ---
add_library(univalue STATIC
  ${VENDOR}/univalue/lib/univalue.cpp
  ${VENDOR}/univalue/lib/univalue_get.cpp
  ${VENDOR}/univalue/lib/univalue_read.cpp
  ${VENDOR}/univalue/lib/univalue_write.cpp)
target_include_directories(univalue SYSTEM PUBLIC ${VENDOR}/univalue/include)
target_compile_options(univalue PRIVATE -w)

# --- The parts of Litecoin Core the indexer builds on ---
add_library(litecoin_core STATIC
  ${VENDOR}/arith_uint256.cpp
  ${VENDOR}/base58.cpp
  ${VENDOR}/bech32.cpp
  ${VENDOR}/crypto/hmac_sha512.cpp
  ${VENDOR}/crypto/ripemd160.cpp
  ${VENDOR}/crypto/sha256.cpp
  ${VENDOR}/crypto/sha512.cpp
  ${VENDOR}/hash.cpp
  ${VENDOR}/key_io.cpp
  ${VENDOR}/primitives/transaction.cpp
  ${VENDOR}/pubkey.cpp
  ${VENDOR}/script/script.cpp
  ${VENDOR}/script/standard.cpp
  ${VENDOR}/support/cleanse.cpp
  ${VENDOR}/uint256.cpp
  ${VENDOR}/util/strencodings.cpp
  ${VENDOR}/util/moneystr.cpp
  ${VENDOR}/policy/feerate.cpp
  src/chainparams.cpp)
# src/ comes first: its chainparams.h replaces Litecoin Core's.
target_include_directories(litecoin_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# SYSTEM: warnings in vendored headers are not ours to fix.
target_include_directories(litecoin_core SYSTEM PUBLIC ${VENDOR})
# What configure detects for Litecoin Core on glibc.
target_compile_definitions(litecoin_core PUBLIC HAVE_ENDIAN_H=1 HAVE_BYTESWAP_H=1
  HAVE_DECL_BSWAP_16=1 HAVE_DECL_BSWAP_32=1 HAVE_DECL_BSWAP_64=1)
target_link_libraries(litecoin_core PUBLIC secp256k1 univalue Boost::headers)
target_compile_options(litecoin_core PRIVATE -w)

# --- OmniLite Standalone ---
add_library(omnilite STATIC
  src/config.cpp
  src/httprpc.cpp
  src/indexer.cpp
  src/logging.cpp
  src/mempool.cpp
  src/nodewallet.cpp
  src/dexguard.cpp
  src/dryrun.cpp
  src/status.cpp
  src/snapshot.cpp
  src/tar.cpp
  src/coreshim/coreshim.cpp
  src/coreshim/policy.cpp
  src/coreshim/rpc/server.cpp
  ${VENDOR}/rpc/protocol.cpp
  ${VENDOR}/rpc/util.cpp
  src/omnicore/activation.cpp
  src/omnicore/consensushash.cpp
  src/omnicore/createpayload.cpp
  src/omnicore/createtx.cpp
  src/omnicore/dbbase.cpp
  src/omnicore/dbspinfo.cpp
  src/omnicore/dbstolist.cpp
  src/omnicore/dbtransaction.cpp
  src/omnicore/dbtxlist.cpp
  src/omnicore/dex.cpp
  src/omnicore/encoding.cpp
  src/omnicore/log.cpp
  src/omnicore/nftdb.cpp
  src/omnicore/notifications.cpp
  src/omnicore/omnicore.cpp
  src/omnicore/parsing.cpp
  src/omnicore/pending.cpp
  src/omnicore/persistence.cpp
  src/omnicore/rpc.cpp
  src/omnicore/rpcmbstring.cpp
  src/omnicore/rpcpayload.cpp
  src/omnicore/rpcrawtx.cpp
  src/omnicore/rpcrequirements.cpp
  src/omnicore/rpctx.cpp
  src/omnicore/rpctxobject.cpp
  src/omnicore/rpcvalues.cpp
  src/omnicore/rules.cpp
  src/omnicore/script.cpp
  src/omnicore/sp.cpp
  src/omnicore/sto.cpp
  src/omnicore/tally.cpp
  src/omnicore/tx.cpp
  src/omnicore/utilsbitcoin.cpp
  src/omnicore/utilsui.cpp
  src/omnicore/walletcache.cpp
  src/omnicore/walletfetchtxs.cpp
  src/omnicore/walletutils.cpp
  src/omnicore/convert.cpp
  src/omnicore/parse_string.cpp
  src/omnicore/version.cpp
  src/rpcclient.cpp
  src/store.cpp
  src/prefetch.cpp
  src/syncer.cpp)
target_include_directories(omnilite PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/coreshim)
target_include_directories(omnilite SYSTEM PUBLIC ${LEVELDB_INCLUDE_DIR})
target_link_libraries(omnilite PUBLIC litecoin_core PkgConfig::EVENT PkgConfig::EVENT_PTHREADS ${LEVELDB_LIBRARY})
target_compile_options(omnilite PRIVATE -Wall -Wextra)
target_compile_definitions(omnilite PRIVATE OMNILITE_STANDALONE_VERSION="${PROJECT_VERSION}")
# Code taken from OmniLite: warnings that come from building it without a wallet.
file(GLOB OMNICORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/omnicore/*.cpp)
set_source_files_properties(${OMNICORE_SOURCES} PROPERTIES COMPILE_OPTIONS
  "-Wno-unused-parameter;-Wno-unused-variable;-Wno-unused-function;-Wno-range-loop-construct")
# Code taken from Litecoin Core.
set_source_files_properties(${VENDOR}/rpc/protocol.cpp ${VENDOR}/rpc/util.cpp src/coreshim/rpc/server.cpp
  PROPERTIES COMPILE_OPTIONS "-w")

add_executable(omnilite-standalone src/main.cpp)
target_link_libraries(omnilite-standalone PRIVATE omnilite)
target_compile_options(omnilite-standalone PRIVATE -Wall -Wextra)

# --- Tests ---
enable_testing()
add_executable(test_omnilite
  test/main.cpp
  test/httprpc_tests.cpp # first: its warmup test needs a fresh process
  test/config_tests.cpp
  test/core_tests.cpp
  test/decode_tests.cpp
  test/indexer_tests.cpp
  test/rpcclient_tests.cpp
  test/store_tests.cpp
  test/syncer_tests.cpp
  test/tar_tests.cpp
  test/omnicore/alert_tests.cpp
  test/omnicore/change_issuer_tests.cpp
  test/omnicore/checkpoint_tests.cpp
  test/omnicore/create_payload_tests.cpp
  test/omnicore/crowdsale_participation_tests.cpp
  test/omnicore/dex_purchase_tests.cpp
  test/omnicore/encoding_b_tests.cpp
  test/omnicore/encoding_c_tests.cpp
  test/omnicore/mbstring_tests.cpp
  test/omnicore/nftdb_tests.cpp
  test/omnicore/params_tests.cpp
  test/omnicore/rounduint64_tests.cpp
  test/omnicore/rules_txs_tests.cpp
  test/omnicore/script_dust_tests.cpp
  test/omnicore/strtoint64_tests.cpp
  test/omnicore/swapbyteorder_tests.cpp
  test/omnicore/tally_tests.cpp
  test/omnicore/uint256_extensions_tests.cpp
  test/omnicore/marker_tests.cpp
  test/omnicore/obfuscation_tests.cpp
  test/omnicore/output_restriction_tests.cpp
  test/omnicore/parsing_a_tests.cpp
  test/omnicore/parsing_b_tests.cpp
  test/omnicore/parsing_c_tests.cpp
  test/omnicore/script_extraction_tests.cpp
  test/omnicore/script_solver_tests.cpp
  test/omnicore/sender_bycontribution_tests.cpp
  test/omnicore/sender_firstin_tests.cpp
  test/omnicore/test_shim.cpp
  test/omnicore/utils_tx.cpp)
target_link_libraries(test_omnilite PRIVATE omnilite Boost::unit_test_framework)
target_include_directories(test_omnilite PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_omnilite PRIVATE -Wall -Wextra)
# Tests taken from OmniLite.
file(GLOB OMNICORE_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/test/omnicore/*.cpp)
set_source_files_properties(${OMNICORE_TESTS} PROPERTIES COMPILE_OPTIONS
  "-Wno-unused-parameter;-Wno-unused-variable;-Wno-dangling-reference;-Wno-sign-compare")
add_test(NAME unit COMMAND test_omnilite)

# --- Install: the program; the systemd unit is installed by hand (see README) ---
install(TARGETS omnilite-standalone RUNTIME DESTINATION bin)
