# Änderungen

## Unveröffentlicht

Sync etwa 45-mal so schnell (AMD A8-9600 gegen `omnilited` 0.18: 0,40 → 0,009 s pro Block).

- Der volle Omni-State wird nicht mehr nach jedem Block geschrieben, sondern wie bei Core im Initial-Download
  nur alle 5000 Blöcke und in den letzten 200 vor dem Tip der Node.
- Blöcke kommen roh (`getblock` mit Verbosity 0) und werden selbst dekodiert, mit Litecoin-0.21-Format:
  HogEx wird zu der Transaktion, die 0.18-Nodes sehen, der MWEB-Block wird übersprungen; bei echten
  MWEB-Daten in einer Transaktion gilt der alte Weg über Verbosity 2.
- Blöcke werden parallel vorab gelesen (`prefetch`, Vorgabe 4 Threads), nur tiefer als `undodepth`.

## 0.1.0

Erste Fassung zum Testen auf einem echten Server.

- Indexer: folgt der Chain per RPC ab dem Omni-Genesis-Block, jeder Block läuft durch OmniLite 0.18 selbst,
  mit Reorgs und Sicherung des Stands.
- API: die `omni_*`-Aufrufe wie bei `omnilitecored`, unbestätigte Transaktionen eingeschlossen, dazu
  `omnilite_getstatus`.
- Senden über die Wallet der Node, mit Probelauf vor jeder Transaktion; DEx-Zahlung nur auf ausdrücklichen
  Aufruf und mit Wächter.
- Snapshot-Export und -Import, systemd-Unit.

Geprüft: auf Regtest gegen `omnilited` v0.18.2 (331 Vergleiche, keine Abweichung). Noch nicht geprüft: Mainnet,
ein aktueller `litecoind`, ein echter Dienstbetrieb unter systemd, DEx-Zahlungen gegen eine echte Node.
