# Recovery nach Ausfällen

Stand: OpenAoIP 0.6.0. Erwartetes Verhalten, automatisierte Prüfungen und gemeldetes Verhalten
müssen getrennt betrachtet werden.

## Controller-Ausfall

Der Controller verteilt Routingpläne; bestehende Verbindungen übertragen Audio direkt zwischen
Sender und Empfänger. Bei Controller-Ausfall behalten laufende Endpunkte den letzten Plan.
Solange Endpunkte/Engine und das Netzwerk arbeiten, kann Audio weiterlaufen.

Ohne Controller sind neue Verbindungen und Routingänderungen nicht möglich. Nach dessen
Rückkehr melden sich die Geräte erneut an. Gespeicherte Routen werden wieder angewendet;
kurze Unterbrechungen beim erneuten Anwenden sind möglich. Dieses Grundverhalten ist
automatisiert geprüft und wurde im Praxistest als funktionierend zurückgemeldet.

## Bridge-/Endpunkt-Ausfall: offenes Problem

Gemeldeter Aufbau: **PC als Sender → Netzwerk → Laptop als Empfänger**.
Nach einem Bridge-Ausfall musste auch die weiterlaufende Bridge auf der Gegenseite neu
gestartet werden, bevor Audio wieder funktionierte.

**Das ist ein offener Recovery-Fehler. Ein Fix oder eine gesicherte Ursache liegt noch nicht vor.**
Ziel: Nur den ausgefallenen Endpunkt wieder starten; die weiterlaufende Gegenseite übernimmt
automatisch neue Registrierung, RTP-Identität und Empfangsports, ohne Neustart oder manuelles Neu-Routing.

Neu gestartete Geräte benötigen aktuell einen erreichbaren Controller. Ein abrupt beendetes
Gerät kann bis zum Ablauf seines Heartbeat-Zeitfensters von ungefähr 3,5 Sekunden noch als
online gelten. Die bestehende Instanzsicherung darf durch den Recovery-Fix nicht umgangen werden.

## Reproduzierbarer Test

1. Version, Geräte-IDs, Controller-Adresse, Netzwerkschnittstellen und Puffer notieren.
2. Stereo-Audio senden und beide RX-Kanäle tatsächlich abhören/prüfen.
3. Sender-Bridge sauber stoppen und wieder starten, Empfänger unverändert lassen.
4. Dasselbe mit der Empfänger-Bridge durchführen, Sender unverändert lassen.
5. Beide Fälle zusätzlich durch abruptes Beenden der jeweiligen Engine prüfen.
6. Neue Geräteinstanz, SSRC, Ports, Onlinezustand und Zähler vor/nach dem Ausfall vergleichen.
7. Erfolg erst bestätigen, wenn echter Audioinhalt wieder ankommt; „online“ allein reicht nicht.

Der Neustart beider Bridges ist der beobachtete Workaround, keine gewünschte Voraussetzung.
Ein Mikrofon-/USB-Geräteverlust innerhalb einer weiterlaufenden Bridge ist ein weiterer,
separater Testfall; hierfür enthält die Bridge Wiederöffnungsversuche über die Windows-Geräte-ID.

## Weitere Dokumentation

[Übersicht](./uebersicht.md) · [Installation](./installation.md) · [Controller](./controller.md) ·
[Audio Bridge](./audio-bridge.md) · [Virtual Soundcard](./virtual-soundcard.md) ·
[Puffer und Latenz](./puffer-latenz.md) · [Audioqualität](./audioqualitaet.md) · [Diagnose](./diagnose.md)
