# Diagnose bei Audiofehlern

Stand: OpenAoIP 0.6.0. Eine grüne Route zeigt Paketempfang, nicht automatisch einen
fehlerfreien hörbaren Signalweg. Aufnahme, Netzwerk und tatsächliche Wiedergabe einzeln prüfen.

## Reihenfolge

1. Läuft die Quelle ohne OpenAoIP lokal störungsfrei?
2. Hat die Sender-Bridge einen plausiblen TX-Pegel und das richtige Capture-/Loopback-Gerät?
3. Stimmen Netzwerk-Samplerate, TX/RX-Zuordnung, Schnittstelle und Controller-Adresse?
4. Kommen am Empfänger RTP-Pakete und ein RX-Pegel an?
5. Ist das richtige Playback-Gerät mit passenden lokalen Kanälen gewählt?
6. Ändern sich Fehlerzähler im gleichen Moment wie die hörbare Störung?

## Anzeigen einordnen

| Anzeige | Aussage |
|---|---|
| Packet loss | Im Empfang erwartete, aber nicht rechtzeitig verfügbare RTP-Pakete |
| Late | Für den aktuellen Empfang zu spät eingeordnete Pakete |
| Buffer underruns, Bridge | Fehlende Audioframes bei Unterlauf eines lokalen adaptiven Puffers; keine Anzahl einzelner Störereignisse |
| Dropped frames, Bridge | Verworfene lokale Frames, z. B. bei Überlauf oder dem Entfernen alter aufgestauter Daten |
| Queued frames | Momentaner Füllstand eines lokalen Puffers |
| Drift ppm | Aktuelle lokale Regelkorrektur; kein Nachweis einer PTP-Clock |
| RTP bandwidth | Aus RTP-Zählern abgeleiteter Verkehr, ohne Ethernet/IP/UDP- und Steuerungsanteil |

Zählerdifferenzen über ein festes Zeitintervall vergleichen. Neustart, Routingänderung oder
Neuaufbau eines Puffers können Zähler zurücksetzen; absolute Werte sind deshalb kein vollständiges Protokoll.

## Aussetzer eingrenzen

Bei „stolperndem“ Audio zuerst Puffer und Paketstatistik vergleichen. Nur eine Einstellung
ändern und denselben Abschnitt erneut testen. Bei lauten Spitzen zusätzlich den Quellpegel
vor der Bridge absenken und auf Clipping prüfen. Für einen Vergleich nach Möglichkeit ein
kabelgebundenes, wenig belastetes Netzwerk verwenden. Die konkrete Ursache bleibt bis zum Test offen.

PTP ist nicht implementiert. Clock Locked ist deshalb aktuell kein erreichbarer Normalzustand.
Die WDM-Runtime-Prüfung wird ohne akzeptierten installierten Treiber als **Skipped** gemeldet;
dies ist keine erfolgreiche Kernel-Audioprüfung.

## Angaben für einen Fehlerbericht

- App-Versionen beider PCs und Windows-Versionen.
- Sender-/Empfängerrolle, Geräte-IDs, Samplerate und Kanalzuordnung.
- Quelle, Windows-Audiogeräte, I/O- und Netzwerkpuffer.
- Netzwerkverbindung und Ort/Zeitpunkt des Ausfalls.
- Änderungen an Loss/Late/Underrun-Zählern während der Störung.
- Ob Stop/Start einer Bridge genügt oder die Gegenseite ebenfalls neu gestartet werden muss.

## Weitere Dokumentation

[Übersicht](./uebersicht.md) · [Installation](./installation.md) · [Controller](./controller.md) ·
[Audio Bridge](./audio-bridge.md) · [Virtual Soundcard](./virtual-soundcard.md) ·
[Puffer und Latenz](./puffer-latenz.md) · [Audioqualität](./audioqualitaet.md) · [Recovery](./recovery.md)
