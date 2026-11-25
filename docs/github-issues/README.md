# OpenAoIP-Dokumentation

Stand: **0.6.3**. Zehn kurze Dokumentationsthemen für `Block361/AudioTools`, jeweils mit
Label **documentation** und Verweisen auf alle anderen Themen. Veröffentlichung und tatsächliche
Issue-URLs werden im [Manifest](./manifest.json) festgehalten. Einstieg: [Übersicht](./uebersicht.md).

Die Markdown-Dateien sind die gepflegten Quellen. Ihre relativen Links funktionieren im Repository.
Beim Veröffentlichen werden sie durch tatsächliche GitHub-Issue-URLs ersetzt und erneut geprüft.

```powershell
# Vorschau ohne Netzwerkänderung:
powershell -NoProfile -ExecutionPolicy Bypass -Command "& './tools/publish-documentation.ps1'"
# Explizit veröffentlichen oder vorhandene Docs-Issues aktualisieren:
powershell -NoProfile -ExecutionPolicy Bypass -Command "& './tools/publish-documentation.ps1' -Publish"
```

Das Hilfsskript prüft offene und geschlossene Issues auf passende Titel, erhält andere Labels,
speichert erfolgreiche Erstellungen sofort und prüft Inhalte, Titel, Label und Querverweise.
Bei unklarem Ergebnis einer Erstellung zuerst GitHub prüfen, bevor erneut veröffentlicht wird.
Vorhandene Git-Zugangsdaten werden nur im Prozessspeicher verwendet und nicht ausgegeben.

Aussetzer und das gemeldete Bridge-Recovery-Problem bleiben offen. PTP-Frequenzregelung wird
von gemeinsamer samplegenauer Präsentation getrennt; WDM bleibt experimentell.

## GitHub-Issues

Alle zehn Issues sind veröffentlicht und mit **documentation** versehen. Jeder Text verweist auf die neun anderen Themen.

| Thema | GitHub |
|---|---|
| [Übersicht](./uebersicht.md) | [#9](https://github.com/Block361/AudioTools/issues/9) |
| [Installation](./installation.md) | [#10](https://github.com/Block361/AudioTools/issues/10) |
| [Controller](./controller.md) | [#11](https://github.com/Block361/AudioTools/issues/11) |
| [Audio Bridge](./audio-bridge.md) | [#12](https://github.com/Block361/AudioTools/issues/12) |
| [Virtual Soundcard](./virtual-soundcard.md) | [#13](https://github.com/Block361/AudioTools/issues/13) |
| [Puffer und Latenz](./puffer-latenz.md) | [#14](https://github.com/Block361/AudioTools/issues/14) |
| [Audioqualität](./audioqualitaet.md) | [#15](https://github.com/Block361/AudioTools/issues/15) |
| [Recovery](./recovery.md) | [#16](https://github.com/Block361/AudioTools/issues/16) |
| [Diagnose](./diagnose.md) | [#17](https://github.com/Block361/AudioTools/issues/17) |
| [PTP](./ptp.md) | [#18](https://github.com/Block361/AudioTools/issues/18) |
