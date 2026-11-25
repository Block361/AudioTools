# PTP-Integration – erster Schritt (0.6.1)

## Was jetzt implementiert ist

**PTPv2 monitor (experimental)** ist ein echter UDP/IPv4-E2E-Monitor für einen externen
PTP-Grandmaster. Er wird von Audio Bridge und Virtual Soundcard über dieselbe Engine benutzt.
Er misst Zeit und Laufzeit. **Die Audio-Clock wird damit noch nicht diszipliniert.**
„Audio clock: Free running“, `synchronized=false` und `audioLocked=false` bleiben deshalb korrekt.

- Standard-PTPv2-Nachrichten: Announce, Sync, Follow_Up, Delay_Req und Delay_Resp.
- One-Step- und Two-Step-Sync, auch wenn Follow_Up vor Sync eintrifft.
- Zuordnung über Domain, SourcePortIdentity, Sequenznummer und RequestingPortIdentity.
- Auswahl unter empfangenen Announce-Datensätzen nach Priorität/Clock-Qualität; Ausfall/Wechsel löscht alte Messungen.
- Berechnung von Master-Offset, mittlerer Pfadlaufzeit und relativer Frequenzabweichung.
- Berücksichtigung der Correction Fields und des angekündigten UTC-Offsets.
- Ablauf von Announce-, Sync- und Delay-Messungen; veraltete Werte werden als solche gekennzeichnet.
- Ein gemeinsamer Monitor je Engine-Prozess: Bridge und Soundcard teilen dieselbe Schnittstelle/Domain.
- Neue Clock-Anzeigen im Controller und Details per Doppelklick.

Der Monitor sendet keine Announce-/Sync-Nachrichten und wird nicht selbst Grandmaster.
Das ist keine vollständige BMCA-Ordinary-Clock-Implementierung, keine AES67-/Dante-Freigabe und
keine Aussage über samplegenaue Synchronisation von Soundkarten.

## Testbetrieb

1. Einen externen **PTPv2-Grandmaster mit UDP/IPv4 und E2E-Delay-Mechanismus** bereitstellen.
   Die Gegenstelle muss zum unterstützten Profil passen. Ein zweiter OpenAoIP-Monitor allein
   ist noch kein Grandmaster. LinuxPTP ist eine mögliche unabhängige Testgegenstelle; die
   Kompatibilität mit echter externer Hardware wurde hier noch nicht geprüft.
2. In Bridge oder Virtual Soundcard die **konkrete IPv4-Adresse des LAN-Adapters** auswählen.
   „Auto / all IPv4 interfaces“ ist im Monitor-Modus bewusst nicht zulässig.
3. Als Clock-Auswahl **PTPv2 monitor (experimental)** und dieselbe **Domain (0–255)** wie am
   Grandmaster wählen. Standard ist 0.
4. Audio starten. Die bisherige Audioverarbeitung läuft intern freilaufend weiter.
5. Im Controller **Clock Status** öffnen. Doppelklick auf ein Gerät zeigt Master-Adresse,
   Grandmaster-ID, Domain, Offset, Pfadlaufzeit und gemessene relative Frequenz.
   In der Bridge ist derselbe Dialog per Doppelklick auf die untere Statusfläche erreichbar,
   in der Soundcard per Doppelklick auf den rechten Statusbereich.

Netzwerk: **224.0.1.129**, UDP **319** für Event- und **320** für General-Nachrichten,
Multicast-TTL 1. Der Monitor bindet diese Ports exklusiv im üblichen Windows-Socket-Verhalten;
ein anderer PTP-Dienst kann das Öffnen verhindern. Es wird kein Port-Sharing erzwungen.
Bei einem Bindefehler wird der Start mit einer Fehlermeldung abgebrochen.
Bei Bedarf die Engine für diese Ports im privaten Audio-LAN zulassen; die Software ändert
Firewall, Windows-Time-Dienst und Sicherheitskonfiguration nicht automatisch.

Bei gleichzeitiger Bridge-/Soundcard-Nutzung müssen PTP-Schnittstelle und Domain übereinstimmen.
Andere Kombinationen werden abgelehnt, damit kein zweiter konkurrierender Portbesitzer entsteht.

## Messwerte richtig lesen

| Wert | Bedeutung |
|---|---|
| Listening / Waiting for Sync | Noch kein passender Master bzw. keine vollständige Sync-Messung |
| Measuring | Austausch läuft, Stabilitätsfenster ist noch nicht ausreichend |
| PTP tracking stable | Mindestens 8 frische Messungen über mindestens 6 Sekunden, kleine Regressionsabweichung; kein Audio-Lock |
| PTP data stale / Grandmaster lost | Sync/Delay-Messungen oder Announce-Nachrichten fehlen |
| Offset | Masterzeit minus lokalem, UTC-verankertem Sitzungstakt; mit gültigem UTC-Offset umgerechnet |
| Mean path delay | Aus dem Vier-Zeitstempel-E2E-Austausch geschätzte mittlere Laufzeit |
| Relative frequency | Steigung der gemessenen Zeitabweichung in ppm; keine angewendete Audiokorrektur |

Der lokale Messtakt wird beim Start an UTC verankert und läuft danach monoton über QPC/steady_clock.
Die Windows-Uhr wird nicht verändert. Spätere Windows-Uhrkorrekturen springen nicht in die Messreihe.
Wenn ein PTP-Zeitskalen-Master keinen gültigen UTC-Offset ankündigt, bleibt der umgerechnete
Offset leer; der rohe Zeitskalenunterschied steht im Diagnose-JSON als `rawOffsetNs`.
Bei Zeitstufen über einer Sekunde wird das Stabilitätsfenster neu aufgebaut.

Diese erste Fassung verwendet **Anwendungs-Zeitstempel** beim Senden/Empfangen. Windows-Scheduling,
Netzwerk-Asymmetrie und Warteschlangen beeinflussen das Ergebnis. Es gibt noch keine Kernel-/NIC-
Hardware-Zeitstempel, keine garantierte Mikrosekundenpräzision und keinen virtuellen Clock-Servo.
Ein symmetrischer Pfad wird angenommen; Messungen mit negativer oder über einer Sekunde liegender
mittlerer Pfadlaufzeit werden verworfen. Messfenster und Kandidaten-/Paketlisten sind begrenzt.

Unterstützt wird das PTPv2-Default-Transportfeld. PTPv1, L2/gPTP, P2P, Unicast-Aushandlung,
Authentifizierungsprofile, Grandmaster-Election und Hardware-Clock-Steuerung sind noch offen.
Der Testbetrieb gehört in ein vertrauenswürdiges Audio-LAN.

## Konfigurationsdatei

Beide Audio-Anwendungen speichern die neuen Felder:

```json
{
  "clockSource": "ptp-monitor",
  "ptpDomain": 0,
  "interface": "192.168.1.20"
}
```

Die übrigen bereits vorhandenen Audio-/Kanal-/Gerätefelder bleiben erforderlich.
`internal` bleibt Standard. `ptp` als disziplinierte Audio-Clock wird weiterhin ausdrücklich
abgelehnt; Monitor-Betrieb und tatsächliche Audio-Synchronisation werden nicht gleichgesetzt.

## Prüfung und nächste Schritte

Automatisiert geprüft: feste Wire-Fixtures, Parser-Grenzen, signierte Korrekturwerte,
One-/Two-Step, vertauschte Reihenfolge, Sequenz-Wrap, falsche Antworten, E2E-/UTC-Rechnung,
synthetische 50-ppm-Drift, Timeout/Master-Wechsel/Zeitstufen sowie ein tatsächlicher UDP-Austausch
über Loopback. Software-Peers sind ausschließlich Testfixtures. Außerdem wird geprüft, dass
die Engine denselben Monitor teilt und keine Audio-Synchronisation erfindet.

Noch erforderlich:

1. Interoperabilität und Messfehler gegen eine unabhängige PTP-Referenz auf mehreren PCs messen.
2. Kernel-/NIC-Zeitstempel und die jeweils unterstützte Windows-/Adapter-Hardware erfassen.
3. Einen begrenzten Clock-Servo mit Holdover, Wiederfang und Verhalten bei Master-Wechsel aufbauen.
4. Audio-Pacing/Resampling für Bridge und anschließend die ASIO-/WDM-Clock-Domänen daran koppeln.
5. RTP-Zeitstempel, gemeinsame Präsentationszeit und synchronen Start integrieren.
6. Erst nach geprüfter Audio-Taktkopplung einen echten **Clock Locked**-Zustand freigeben.

Das gemeldete Bridge-Recovery-Problem und die hörbaren Aussetzer sind damit noch nicht als behoben nachgewiesen.

## Referenzen

- [LinuxPTP: Transport, E2E, Software-Zeitstempel, Profile und Intervalle](https://www.linuxptp.org/documentation/ptp4l/)
- [Microsoft: Windows-Timestamping-Konfiguration für spätere Kernel-Anbindung](https://learn.microsoft.com/en-us/windows/win32/api/mstcpip/ns-mstcpip-timestamping_config)
- [Suite-Bedienung](./WINDOWS-START.md)
- [Modularchitektur und Testgrenzen](./ARCHITECTURE.md)
