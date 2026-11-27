# PTP – Software-Audiotakt

Stand: OpenAoIP **0.6.3**, experimenteller Entwicklungsprototyp.

PTPv2 liefert jetzt den **Frequenztakt für die TX- und RX-Netzwerkverarbeitung** der Bridge und
Virtual Soundcard. Lokale Audiogeräte werden über kontinuierliche Sampleratenanpassung angebunden.
Das ist eine Software-Frequenzregelung. Gemeinsame Präsentationszeiten, samplegenauer Start,
Phasenkohärenz sowie Dante-/AES67-Interoperabilität sind weiterhin offen.

## Zwei Rechner einrichten

1. Auf beiden PCs alle OpenAoIP-Audioanwendungen schließen, Suite aktualisieren und neu starten.
2. Auf **PC Sender** als Clock Source **PTPv2 grandmaster (software, experimental)** wählen.
3. Auf dem **Laptop** **PTPv2 follower (software, experimental)** wählen.
4. Auf beiden die tatsächliche lokale **LAN-IPv4-Adresse** und dieselbe **PTP-Domain**, z. B. 0, einstellen.
5. Audioquellen, Wiedergabe, gleiche Netzwerk-Samplerate und Kanalzuordnung konfigurieren; beide starten.
6. Im Controller die Routen verbinden und **Clock Status** öffnen.

Pro Domain genau **einen** manuell gewählten Software-GM betreiben. Automatische Grandmaster-Election
ist noch offen. Der Controller liefert keine Clock und ist für laufendes Audio nicht erforderlich.
Bridge und VSC auf demselben PC teilen einen PTP-Dienst; Schnittstelle, Domain und Rolle müssen
übereinstimmen. Die letzte zugehörige Audiositzung beendet den Dienst.

Konfiguration: `clockSource: "ptp-monitor"` bezeichnet nun den Software-Follower einschließlich
Frequenzregelung; `"ptp-grandmaster"` den GM, `"internal"` den bisherigen unabhängigen Betrieb.
`ptpDomain` ist 0–255. Der unpräzise Wert `"ptp"` wird abgelehnt.

## Anzeigen

| Anzeige | Bedeutung |
|---|---|
| Software reference | Software-GM sendet; TX/RX laufen mit seinem lokalen QPC-Takt |
| Acquiring rate | Messfenster fehlt oder begrenzte Nachführung läuft |
| PTP rate locked | Frische stabile Referenz, weniger als 1 ppm verbleibende Regeldifferenz und beide Netzwerk-Audioworker aktiv |
| Holdover | Referenz war gültig, ist jetzt ausgefallen, verrauscht oder gewechselt; zuletzt angewendete Rate bleibt erhalten |
| Listening / Waiting for Sync | Noch kein passender Master bzw. vollständiger Sync-Austausch |
| Measuring | Messungen vorhanden, Stabilitätsbedingungen noch nicht erfüllt |
| Offset | Masterzeit minus lokalem UTC-verankertem Sitzungstakt; keine Audioverzögerung |
| Path delay | Geschätzte mittlere Netzwerklaufzeit bei angenommener Pfadsymmetrie |
| Relative frequency | Aus dem Offsetverlauf geschätzte Masterfrequenz relativ zum lokalen QPC |
| Applied rate / rateCorrectionPpm | Tatsächlich von TX und RX verwendete Frequenzkorrektur |

Ein großer konstanter Offset, z. B. −1,3 Sekunden, verschiebt **keine Audiodaten**. Nur die
Frequenz beeinflusst Paketintervalle. Der GM misst sich nicht selbst: Offset und Pfadlaufzeit
bleiben dort leer. Bei schlechter Messqualität bleibt der Follower im Erwerb oder Holdover.
Software-Rate-Lock beweist keine Hardware- oder Phasensynchronisation.

## Regelung und Audio-I/O

- Clock-Modul: Auswertung alle 100 ms. Voraussetzung sind mindestens acht frische Messungen
  über sechs Sekunden, Regressionsresiduum unter 250 µs und akzeptierte Frequenz ±1000 ppm.
- Korrektur maximal ±1000 ppm, Änderung höchstens **20 ppm pro Sekunde**. +50 ppm verkürzt das
  lokale Paketintervall entsprechend. RTP-Zeitstempel steigen weiter pro 240-Frame-Block;
  die Timeline springt nicht wegen eines PTP-Offsets.
- TX und RX lesen dieselbe atomare Ratenkorrektur ohne PTP-Mutex oder JSON-Verarbeitung.
  OS-Stalls haben begrenztes Catch-up; ausgelassene TX-Blöcke erscheinen als RTP-Lücken.
- Bridge verwendet ihre bestehenden adaptiven Puffer zwischen WASAPI-Geräten und Netzwerktakt.
- VSC erhält im PTP-Modus adaptive TX-/RX-Puffer mit **je 20 ms Zielpuffer** und einen lokalen
  I/O-Worker. ASIO-Speicher-ABI, ASIO-Callback-Timer und WDM-Protokoll bleiben erhalten.
  Die Puffer addieren Latenz; Host-Latenzangaben sind keine exakte Ende-zu-Ende-Messung.
  Queue, SRC-Drift, verworfene Frames und Underruns stehen im Status-JSON.
- Bei Referenzverlust laufen Audio und die zuletzt angewendete Rate weiter. Rückkehr oder
  Master-Wechsel erfordern erneut ein stabiles Messfenster und gleitende Nachführung.
  Langfristige Genauigkeit im Holdover ist nicht garantiert.

Physische Geräte-Clock, Windows-Uhr und ASIO-Host-Clock werden nicht verstellt. Ihre Abweichung
wird durch SRC ausgeglichen. `audioLocked`, `locked` und `synchronized` bleiben deshalb **false**.
`audioRateActive` und `audioRateLocked` beschreiben ausschließlich die Software-Netzwerkrate;
`phaseSynchronized` bleibt false.

## Transport und Grenzen

PTPv2 E2E über **224.0.1.129**, UDP **319/320**, TTL 1. GM: Announce 1 Hz,
Two-Step Sync/Follow_Up 8 Hz, Delay_Req/Resp. Der Follower unterstützt auch One-Step.
Ein anderer PTP-Dienst kann die Ports blockieren. Firewall, Windows-Time-Dienst und
Sicherheitsfunktionen werden nicht automatisch verändert.

Anwendungs-Zeitstempel sind durch Windows-Scheduling, Warteschlangen und Pfadasymmetrie begrenzt.
Clock-Class 248, unbekannte Genauigkeit 0xFE und Time-Source 0xA0 kennzeichnen den Software-Oszillator.
Der GM behauptet keine GPS-/TAI-/UTC-Traceability und verwendet eine beliebige, beim Start an lokale
UTC angelehnte Zeitskala. Uhrstufen über einer Sekunde setzen das Messfenster zurück. Ein fremder
PTP-Zeitskalen-Master benötigt einen gültigen UTC-Offset für die umgerechnete Offsetanzeige.

Kernel-/NIC-Zeitstempel, Hardware-Clock-Steuerung, BMCA-Election, gPTP/P2P, gemeinsame RTP-
Präsentationszeit und samplegenaue Wiedergabe sind offen. WDM bleibt ein separat testsignierter,
nicht produktionsreifer Prototyp. Software-Rate-Lock ist keine Produktionsfreigabe.

## Prüfung

Automatisierte Tests verwenden getrennte Ports und ausdrücklich aktivierte Test-Audioquellen:

- Wire-Fixtures, One-/Two-Step, E2E/UTC, Sequenz-Wrap, ungültige Pakete und Masterwechsel.
- Tatsächlicher Software-GM/Follower-Austausch über UDP-Loopback und GM-Neustart.
- Aus PTP-Austauschen abgeleitete −500/+50/+500 ppm, Regelslew, Intervallvorzeichen, großer
  Offset, Holdover, unzulässige Frequenz und Wiederfang.
- Kontinuierliches SRC über simulierte drei Minuten je Driftwert, Stereoerhalt und Pufferstatistik.
- Zwei Bridge-Engines, bidirektionales RTP und Testbackend-Wiedergabe während GM-Ausfall/-Rückkehr.
- ASIO-Host ↔ VSC ↔ RTP ↔ ASIO bei 44,1/48/96 kHz, mit und ohne PTP-/SRC-Pfad.

Der Nutzer beobachtete GM/Follower-Multicast zwischen PC und Laptop in 0.6.2. Die neue Audioregelung
benötigt noch Mehr-PC-Hörtests, unabhängige Referenzmessungen und Langzeitprüfungen. Bekannte
Bridge-Recovery-Probleme und Aussetzer gelten damit weiterhin nicht als nachweislich behoben.

- [Suite-Bedienung](./WINDOWS-START.md)
- [Architektur und Testgrenzen](./ARCHITECTURE.md)
- [LinuxPTP: E2E und Software-Zeitstempel](https://www.linuxptp.org/documentation/ptp4l/)
- [Microsoft: Kernel-Timestamping-Konfiguration](https://learn.microsoft.com/en-us/windows/win32/api/mstcpip/ns-mstcpip-timestamping_config)
