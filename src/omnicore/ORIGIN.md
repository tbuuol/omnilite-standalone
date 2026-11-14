# Herkunft von src/omnicore

Die Dateien hier stammen aus OmniLite 0.18.1 (`github.com/OmniLite/omnilite`, `src/omnicore/`,
Commit `fbc27c1`, MIT). Sie sind so weit wie möglich unverändert; was das Original von Litecoin Core
braucht (`cs_main`, `chainActive`, `gArgs`, Blockdateien, UI-Signale), liefern die Stellvertreter in
`src/coreshim` (siehe dort). Jede Änderung an einer OmniLite-Datei ist im Code mit
`omnilite-standalone:` markiert.

## Unverändert übernommen

`activation`, `consensushash`, `convert`, `createpayload`, `dbbase`, `dbspinfo`, `dbstolist`,
`createtx`, `dbtransaction`, `dbtxlist`, `dex`, `errors.h`, `notifications`, `omnicore`, `parse_string`,
`pending`, `rpc`, `rpctx`, `rpcmbstring`, `rpcpayload`, `rpcrawtx`, `rpcrequirements`, `rpctxobject`, `rpcvalues`, `rules`, `script`, `sp`, `sto`, `tally`, `tx`, `uint256_extensions.h`, `utilsbitcoin`,
`utilsui`, `version`, `walletcache`, `walletfetchtxs`, `walletutils`.

Das umfasst den kompletten Ablauf von OmniLite: `parseTransaction`, die Logik aller Transaktionstypen,
die Handler für Blöcke und Transaktionen, die Sicherung des States in Dateien (`MP_persist`) und die
Rückkehr zu einem gesicherten State bei Reorgs (`RewindDBsAndState`, `msc_initial_scan`).

## Angepasst

| Datei | Änderung |
|---|---|
| `parsing.h` | `<stdint.h>` ergänzt (das Original verlässt sich auf vorher eingebundene Header) |
| `nftdb.h` | `<map>`, `<string>`, `<vector>` ergänzt; `std::filesystem` statt `boost::filesystem` |
| `persistence.h/.cpp`, `sp.cpp` | `<fs.h>` (`std::filesystem`) statt `<boost/filesystem.hpp>` |
| `persistence.cpp` | `IsPersistenceEnabled` misst den Abstand zum Tip an `g_bestHeaderHeight` (Höhe der Node, vom Syncer), wie Core im Initial-Download an `pindexBestHeader`. Sonst wird auf Mainnet nach jedem Block der ganze State geschrieben (`DONT_STORE_MAINNET_STATE_UNTIL` = 2 000 000 liegt vor dem Omni-Genesis-Block). Nicht konsensrelevant: betrifft nur, wann der State als Datei gesichert wird |
| `encoding.cpp` | `GetRand(256)` → `std::random_device` (nur die Wahl des letzten Bytes der Class-B-Schlüssel, nicht konsensrelevant) |
| `rpc.cpp` | `omni_setautocommit`, `omni_listtransactions`, `omni_getwalletbalances`, `omni_getwalletaddressbalances` und die Wallet für `ismine` auch ohne `ENABLE_WALLET`; `getWalletAddresses` fragt die Node-Wallet nach ihren Adressen; `omni_listtransactions` ohne Node leer statt Null-Zeiger |
| `rpcrawtx.cpp` | Wallet für `ismine` in `omni_decodetransaction` auch ohne `ENABLE_WALLET` |
| `rpctx.cpp` | `omni_senddexpay` ruft den Zahlungs-Wächter (`src/dexguard.cpp`) vor dem Bauen, vor dem Senden und danach |
| `walletutils.h`, `wallettxbuilder.h` | `SelectCoins`, `SelectAllCoins`, `CreateFundedTransaction` auch ohne `ENABLE_WALLET` deklariert |
| `walletutils.cpp`, `walletfetchtxs.cpp` | `IsMyAddress` und `FetchWalletOmniTransactions` auch ohne `ENABLE_WALLET` |
| `wallettxbuilder.cpp` | nicht übernommen: `WalletTxBuilder` und `CreateFundedTransaction` sind in `src/nodewallet.cpp` über die Wallet der Node neu geschrieben, mit dem Ablauf des Originals |
| `tx.h`, `tx.cpp` | Probelauf (`dryRun`, `setDryRun`, für `src/dryrun.cpp`): jede `logicMath_*` endet vor dem ersten Buchen mit 0, Sell Offer und Accept prüfen über `dryRun_TradeOffer`/`dryRun_AcceptOffer`; `interpretPacket` nimmt im Probelauf den Tip als Block und prüft Activations/Alerts nicht. Ohne Probelauf unverändert, bis auf eine Null-Prüfung des Blocks in `interpretPacket` (das Original würde dort abstürzen; im Blockablauf tritt der Fall nicht auf) |
| `log.h`, `log.cpp` | ersetzt: gleiche Flags, `PrintToLog`, `PrintToConsole` und `InitDebugLogLevels` (wörtlich), aber Ausgabe ins Programm-Log (Debug- bzw. Info-Ebene) statt `omnicore.log`; `g_omniLogCapture` sammelt die Zeilen für den Probelauf |

## Build

- Assertions bleiben in jedem Build-Typ an (`NDEBUG` wird entfernt): OmniLite bucht z. B. mit
  `assert(update_tally_map(...))`, ohne Assertions fehlen diese Buchungen.
- Ohne `ENABLE_WALLET` gebaut: die Wallet-Teile des Originals sind aus, soweit oben nicht freigeschaltet.
  Gesendet und gelistet wird über die Wallet des `litecoind` (`src/nodewallet.cpp`).

## Tests

`test/omnicore/` enthält die Unit-Tests aus `src/omnicore/test/`. Viele liefen gegen OmniLite selbst
nicht, weil sie noch die Werte von Omni Core (Bitcoin) erwarten. Angepasst, jeweils markiert:

- Decoder-Tests (`parsing_a/b/c`, `sender_bycontribution`, `sender_firstin`, `utils_tx`): Bitcoin-Adressen
  mechanisch auf Litecoin umgerechnet (gleicher Hash, Versionsbyte 48 für P2PKH, 50 für P2SH).
- `exodus_tests`: die Exodus-Adressen von OmniLite statt der von Omni Core.
- `alert_tests`: OmniLite erlaubt Alerts nur von `MJDAXHQMxdRfKS2b3hECsSCf4iyx1G8CCT`, nicht von Exodus.
- `params_tests`: `MIN_ACTIVATION_BLOCKS` 8192 und STO ab Block 2 010 500, wie in OmniLites `rules.cpp`.
- `create_payload_tests` (Freeze/Unfreeze): erwartetes Hex an die im Test angegebene Litecoin-Adresse angepasst.
- `script_dust_tests`: OmniLite multipliziert die Dust-Grenze mit 30, Omni Core mit 3.

Weggelassen: `lock_tests` (testet Cores Locking, braucht `boost::thread`) und `version_tests` (erwartet
eine Versionsnummer, die nicht zu OmniLites `version.h` passt). `test_shim.h/.cpp` stellt bereit, was
die Tests aus Cores Test-Umgebung erwarten (`BasicTestingSetup`, `GetRandInt`); `std::random_shuffle`
ist durch `std::shuffle` ersetzt.
