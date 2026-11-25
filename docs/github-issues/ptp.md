# PTP und Software-Audiotakt

Stand: OpenAoIP 0.6.3, experimentell.

## Einrichten

1. Beide PCs auf denselben Stand aktualisieren; vorher Audioanwendungen und ASIO-Host schließen.
2. PC Sender: **PTPv2 grandmaster (software, experimental)**.
3. Laptop: **PTPv2 follower (software, experimental)**.
4. Explizite lokale LAN-Adressen, dieselbe Domain 0–255 und dieselbe Netzwerk-Samplerate wählen.
5. Bridge-/VSC-Sitzungen starten und Routen im Controller verbinden.

Genau ein manuell gewählter GM je Domain. Der Controller ist keine Clock. Gleichzeitige Bridge-
und VSC-Sitzungen auf demselben PC teilen den PTP-Dienst und benötigen dieselbe Rolle, Domain
und Schnittstelle. `clockSource: "ptp-monitor"` bleibt als Konfigurationswert für den Follower erhalten.

## Was gekoppelt wird

PTP misst Masterfrequenz und Offset. Der gemeinsame Servo regelt die TX-/RX-Netzwerkrate
maximal ±1000 ppm und höchstens 20 ppm pro Sekunde. Lokale WASAPI-/ASIO-/WDM-Takte werden
über kontinuierliches SRC angebunden. VSC fügt im PTP-Modus je Richtung 20 ms Zielpuffer hinzu.
Windows-Zeit und physische Audioclocks werden nicht verstellt. Große konstante Offsets erzeugen
keinen Sprung im Audiotakt. RTP behält 240 Frames je Paket.

| Status | Bedeutung |
|---|---|
| Software reference | Lokaler Software-GM und aktive Netzwerk-Audioworker |
| Acquiring rate | Messfenster fehlt oder gleitende Nachführung läuft |
| PTP rate locked | Frische stabile Messung und tatsächlich angewendete Software-Netzwerkrate |
| Holdover | Zuletzt gültige Rate bei Ausfall/Instabilität weiterverwenden |

Stabile Messung benötigt mindestens acht Werte über sechs Sekunden und Residuum unter 250 µs.
Bei Rückkehr/Masterwechsel wird das Messfenster erneut geprüft. Starkes Scheduling-/Netzwerkrauschen
kann das Einrasten verhindern. `rateCorrectionPpm` zeigt die angewendete Korrektur.

**Keine samplegenaue Phasensynchronisation:** Gemeinsame RTP-Präsentationszeit, synchroner Start,
Hardware-Zeitstempel und automatische GM-Election sind weiterhin offen. `audioRateLocked` ist
ein Software-Ratenstatus; die bisherigen Sample-Lock-Flags bleiben false.

## Transport und Testgrenzen

PTPv2 E2E: 224.0.1.129, UDP 319/320. Anwendungstimestamps besitzen keine garantierte
Mikrosekundenpräzision. Der Software-GM behauptet keine GPS-/TAI-Traceability. Firewall und
Windows-Sicherheitsfunktionen werden nicht automatisch verändert.

Geprüft werden Protokoll, reale Loopback-UDP-Kommunikation, Drift/Regelslew/Holdover/Wiederfang,
SRC-Puffer und ASIO-RTP-Wellenformen bei 44,1/48/96 kHz. Der Mehr-PC-PTP-Austausch in 0.6.2
wurde vom Nutzer beobachtet; die neue Audioregelung braucht noch Mehr-PC-Hör- und Langzeittests.
WDM ist weiterhin experimentell. Gemeldete Aussetzer und Bridge-Recovery gelten nicht als behoben.

## Weitere Dokumentation

[Übersicht](./uebersicht.md) · [Installation](./installation.md) · [Controller](./controller.md) ·
[Audio Bridge](./audio-bridge.md) · [Virtual Soundcard](./virtual-soundcard.md) ·
[Puffer und Latenz](./puffer-latenz.md) · [Audioqualität](./audioqualitaet.md) ·
[Recovery](./recovery.md) · [Diagnose](./diagnose.md)
