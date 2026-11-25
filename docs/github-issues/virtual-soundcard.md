# Virtual Soundcard

Stand: OpenAoIP 0.6.3. Netzwerk-TX sind Ausgänge der Audioanwendung, Netzwerk-RX sind deren Eingänge.

## ASIO

- Eigener x64-ASIO-Treiber mit 1–32 Kanälen je Richtung bei 44,1/48/96 kHz.
- Kein Windows-Testmodus erforderlich.
- Einmal **Register ASIO** wählen und den Windows-Dialog für die Registrierung bestätigen.
- Gerätenamen, LAN-Schnittstelle, Controller, Kanalzahlen, Samplerate und Netzwerklatenz einstellen.
- **Start**, danach in der x64-DAW **OpenAoIP Virtual Soundcard** als ASIO-Gerät wählen.
- Im Controller TX/RX verbinden. Ein ASIO-Host kann die Soundcard gleichzeitig belegen.

**Buffer** ist die bevorzugte Größe von 64/128/256/512/1024 Frames. Der Host handelt die
tatsächliche Größe aus; diese wird als **Host buffer** angezeigt. Für Format- oder
Pufferänderungen die Soundcard stoppen und den ASIO-Host schließen.

## WDM – experimentell

Der vorhandene PortCls/WaveRT-Treiber besitzt 16 Stereo-Paare je Richtung bei festen 48 kHz.
Er ist ein separat versionierter Testprototyp, keine produktionsreife Windows-Soundkarte.
Der WDM-Modus startet nur, wenn ein kompatibler Treiber geöffnet werden kann und Windows
die vorgesehenen Audio-Endpunkte tatsächlich bereitstellt.

Normale Setups installieren keinen Kerneltreiber und aktivieren keinen Testmodus.
Ein separates testsigniertes Paket ist für die Windows-11-VirtualBox-Testumgebung vorgesehen.
Produktionssignierung, Kernel-Laufzeit-, Power/PnP- und Langzeittests sind noch offen.
ASIO und WDM sind alternative Modi derselben Virtual Soundcard.

## Status und Beenden

Die Anwendung zeigt Running/Stopped, Controller-Verbindung, Samplerate, Kanalzahl,
RTP-Bandbreite, Treiber-/Hoststatus, Paketverlust und Buffer-Underruns.
Internal bleibt Standard. Ein PC kann Software-GM sein, andere arbeiten als PTP-Follower. Ein begrenzter Servo regelt die TX-/RX-Netzwerkrate; kontinuierliches SRC verbindet sie mit den lokalen ASIO-/WDM-Takten. Im PTP-Modus kommen je Richtung 20 ms Zielpuffer hinzu. PTP rate locked ist keine samplegenaue Phasensynchronisation. **Stop** oder Schließen beendet diese Audiositzung.

Wenn eine Anwendung bereits vorhandene Windows-Geräte, ein virtuelles Kabel oder den
Windows-Mix nutzen soll, ist die Audio Bridge der passende Zugang ohne OpenAoIP-Kerneltreiber.

## Weitere Dokumentation

[PTP](./ptp.md)

[Übersicht](./uebersicht.md) · [Installation](./installation.md) · [Controller](./controller.md) ·
[Audio Bridge](./audio-bridge.md) · [Puffer und Latenz](./puffer-latenz.md) ·
[Audioqualität](./audioqualitaet.md) · [Recovery](./recovery.md) · [Diagnose](./diagnose.md)
