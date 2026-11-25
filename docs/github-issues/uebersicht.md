# OpenAoIP im Überblick

Stand: OpenAoIP 0.6.3, Entwicklungsprototyp für Windows x64.

| Anwendung | Aufgabe |
|---|---|
| Controller | Geräte erkennen, TX/RX-Kanäle routen, Namen und Presets verwalten, Zustände anzeigen |
| Audio Bridge | Vorhandene Windows-Audiogeräte und Systemaudio über WASAPI ins Netzwerk einbinden |
| Virtual Soundcard | Windows-Audioanwendungen über den eigenen ASIO-Treiber oder experimentellen WDM-Treiber anbinden |
| Engine | Gemeinsamer Hintergrundprozess für Audio Bridge und Virtual Soundcard pro Windows-Benutzersitzung |

Audio wird direkt zwischen den Endpunkten übertragen. Der Controller verteilt Routingpläne;
er transportiert das Audio nicht. Bridge und Soundcard können gleichzeitig laufen, benötigen
aber unterschiedliche Geräte-IDs. Eine zweite aktive Sitzung mit derselben ID wird abgelehnt.

## Schnellstart mit zwei PCs

1. PCs über ein gemeinsames Audio-LAN verbinden und einen Controller starten.
2. Auf dem Sender eine Bridge-Capture- oder Loopback-Quelle einrichten.
3. Auf dem Empfänger eine Bridge-Playback-Zuordnung einrichten.
4. Gleiche Netzwerk-Samplerate und passende Kanalzahlen wählen, dann beide Bridges starten.
5. Im Controller TX des Senders mit RX des Empfängers verbinden. Für Stereo zwei Kanäle routen.

Für eine DAW kann stattdessen die Virtual Soundcard im ASIO-Modus verwendet werden.
Für Serato über ein virtuelles Audiokabel wird dessen Aufnahme-Endpunkt in der Audio Bridge ausgewählt.

## Grenzen des aktuellen Stands

- Echtes OpenAoIP-RTP mit L16/16 Bit und 44,1/48/96 kHz; keine behauptete Dante-/AES67-Kompatibilität.
- PTPv2-Software-GM/Follower und softwarebasierte Frequenzregelung sind implementiert; gemeinsame samplegenaue Präsentation bleibt offen.
- Bridge und VSC koppeln lokale Audiotakte über SRC an die PTP-Netzwerkrate; Phasenkohärenz wird nicht garantiert.
- Der WDM-Treiber ist experimentell; Kernel-Laufzeit- und Produktionsfreigabe stehen aus.
- Aussetzer und der nach einem Bridge-Ausfall erforderliche Neustart des Gegenübers wurden
  im Praxistest gemeldet und sind noch nicht abschließend untersucht.

## Weitere Dokumentation

[PTP](./ptp.md)

- [Installation](./installation.md)
- [Controller](./controller.md)
- [Audio Bridge](./audio-bridge.md)
- [Virtual Soundcard](./virtual-soundcard.md)
- [Puffer und Latenz](./puffer-latenz.md)
- [Audioqualität](./audioqualitaet.md)
- [Recovery](./recovery.md)
- [Diagnose](./diagnose.md)
