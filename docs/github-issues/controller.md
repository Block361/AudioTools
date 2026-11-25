# Controller und Routing

Stand: OpenAoIP 0.6.0.

## Matrix bedienen

- Transmitter/TX stehen horizontal, Receiver/RX vertikal.
- **+ / −**, **Expand all** und **Collapse all** klappen Geräte auf oder zu.
- Ein Kreuzungspunkt weist einem RX-Kanal genau einen TX-Kanal zu.
- Ein anderer TX am selben RX ersetzt die bisherige Quelle. Erneutes Anklicken derselben Route entfernt sie.
- Stereo benötigt zwei Einzelkanalrouten; die beiden Seiten können unabhängig geroutet werden.
- Ein TX kann mehrere RX versorgen. Mehrere TX auf einem RX würden einen zusätzlichen Mischer benötigen.

Geräte und Kanäle lassen sich über die beiden Suchfelder filtern. **Online only** blendet
Offline-Geräte aus. Ein Doppelklick auf einen Namen bearbeitet einen gespeicherten
Controller-Alias; Audioformat und Hardware werden in der jeweiligen Audio-Anwendung konfiguriert.

## Status und Tabs

| Anzeige | Bedeutung |
|---|---|
| Grün | Der Empfänger bestätigt den Paketempfang |
| Gelb | Routing ausstehend bzw. Audiopuffer baut sich auf |
| Rot | Keine Pakete/Fehler bei einer erwarteten aktiven Verbindung |
| Grau | Offline/inaktiv |

Grün bestätigt weder einen hörbaren Ausgang noch fehlerfreie analoge Audioqualität.
Zusätzlich stehen **Device Info**, **Clock Status**, **Network Status** und **Events** zur Verfügung.
Die Clock bleibt freilaufend; PTP ist nicht implementiert.

## Presets und Ausfälle

Routen werden automatisch gespeichert. **Save preset / Load preset** exportiert oder ersetzt
Routen sowie Geräte-/Kanalaliasse. Ungültige Presets werden vollständig abgelehnt. Routen zu
fehlenden Geräten bleiben gespeichert und können nach deren Anmeldung wieder aktiv werden.

Bei einem Controller-Ausfall behalten bestehende Endpunkte ihren letzten Plan. Audio kann
direkt weiterlaufen; neue Verbindungen und Routingänderungen benötigen den Controller.
Ein neu gestartetes Audiogerät muss sich erneut anmelden. Beim Controller-Wiederanlauf sind
kurze Unterbrechungen möglich. Das gemeldete Bridge-Recovery-Problem ist davon getrennt.

## Weitere Dokumentation

[Übersicht](./uebersicht.md) · [Installation](./installation.md) · [Audio Bridge](./audio-bridge.md) ·
[Virtual Soundcard](./virtual-soundcard.md) · [Puffer und Latenz](./puffer-latenz.md) ·
[Audioqualität](./audioqualitaet.md) · [Recovery](./recovery.md) · [Diagnose](./diagnose.md)
