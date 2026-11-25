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
