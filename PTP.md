# PTP – Software-Grandmaster und Monitor (0.6.2)

## Was jetzt implementiert ist

Die gemeinsame Engine unterstützt einen echten **PTPv2-Software-Grandmaster** und einen
**PTPv2-Monitor** über UDP/IPv4/E2E. Bridge und Virtual Soundcard können beide Rollen auswählen.
Ein PC kann damit die Zeitreferenz bereitstellen, die andere PCs messen.
**Die Audio-Clock wird damit noch nicht diszipliniert.**
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
- Software-GM: Announce einmal pro Sekunde, Two-Step-Sync/Follow_Up achtmal pro Sekunde und
  Delay_Resp mit dem tatsächlichen lokalen Empfangszeitstempel auf passende Delay_Req-Nachrichten.
- Warnung bei einem weiteren angekündigten Grandmaster in derselben Domain.

Die Monitor-Rolle wird nicht automatisch Grandmaster. Die GM-Rolle wird ausdrücklich ausgewählt;
sie bleibt eine manuell festgelegte Software-Referenz. Es gibt noch keine vollständige BMCA-Wahl
oder automatische Übernahme bei GM-Ausfall. AES67-/Dante-Kompatibilität und samplegenaue
Synchronisation von Soundkarten werden nicht behauptet.

## Testbetrieb

1. Beide PCs auf **0.6.2** aktualisieren. Vor dem Update alle OpenAoIP-Audioanwendungen und
   ASIO-Hosts schließen, damit auch die gemeinsame Engine aktualisiert wird.
2. In Bridge oder Virtual Soundcard die **konkrete IPv4-Adresse des LAN-Adapters** auswählen.
   „Auto / all IPv4 interfaces“ ist im Monitor-Modus bewusst nicht zulässig.
3. Auf **genau einem PC** in der Bridge **PTPv2 grandmaster (software, experimental)** wählen.
   In der Virtual Soundcard heißt diese Auswahl kurz **PTP GM**.
   Auf den anderen PCs **PTPv2 monitor** bzw. **PTP monitor** wählen.
   Überall dieselbe **Domain (0–255)** einstellen; Standard ist 0.
4. Audio starten. Die bisherige Audioverarbeitung läuft intern freilaufend weiter.
5. Im Controller **Clock Status** öffnen. Doppelklick auf ein Gerät zeigt Master-Adresse,
   Grandmaster-ID, Domain, Offset, Pfadlaufzeit und gemessene relative Frequenz.
   In der Bridge ist derselbe Dialog per Doppelklick auf die untere Statusfläche erreichbar,
   in der Soundcard per Doppelklick auf den rechten Statusbereich.

Beispiel: **PC Sender = Software-GM**, **Laptop = Monitor**, beide Domain 0. Der Controller
zeigt beim PC „Grandmaster (software)“ und beim Laptop nach vollständigem Austausch Messwerte.
Beim GM selbst bleiben Offset und Pfadlaufzeit leer: Er misst nicht gegen sich selbst.
Ein Monitor auf einem anderen PC kann diese Werte nach ein bis zwei Delay-Austauschen anzeigen.
Die Stabilitätsbewertung benötigt mindestens acht Messungen über sechs Sekunden.

Alternativ kann ein passender externer PTPv2-E2E-Grandmaster die Referenz stellen. Dann OpenAoIP
auf Monitor stellen. LinuxPTP ist eine mögliche unabhängige Testgegenstelle; reale externe
Hardware- und Mehr-PC-Multicast-Tests wurden hier noch nicht durchgeführt.

Netzwerk: **224.0.1.129**, UDP **319** für Event- und **320** für General-Nachrichten,
Multicast-TTL 1. Der Monitor bindet diese Ports exklusiv im üblichen Windows-Socket-Verhalten;
ein anderer PTP-Dienst kann das Öffnen verhindern. Es wird kein Port-Sharing erzwungen.
Bei einem Bindefehler wird der Start mit einer Fehlermeldung abgebrochen.
Bei Bedarf die Engine für diese Ports im privaten Audio-LAN zulassen; die Software ändert
Firewall, Windows-Time-Dienst und Sicherheitskonfiguration nicht automatisch.

Bei gleichzeitiger Bridge-/Soundcard-Nutzung müssen PTP-Schnittstelle, Domain und Rolle übereinstimmen.
Andere Kombinationen werden abgelehnt, damit kein zweiter konkurrierender Portbesitzer entsteht.
Zwei Apps desselben PCs teilen dann eine Clock-Identität und einen tatsächlichen PTP-Sender.
Der Controller selbst stellt keine Clock bereit. Solange mindestens eine zugehörige PTP-
Audiositzung läuft, bleibt der gemeinsame PTP-Dienst aktiv; die letzte Stop-Aktion beendet ihn.

## Messwerte richtig lesen

| Wert | Bedeutung |
|---|---|
| Grandmaster (software) | Announce und Two-Step-Sync wurden über UDP gesendet; kein Nachweis des Empfangs auf anderen PCs |
| Grandmaster / other GM detected | Ein weiterer GM ist sichtbar; bei manueller Konfiguration nur einen Software-GM pro Domain betreiben |
| Listening / Waiting for Sync | Noch kein passender Master bzw. keine vollständige Sync-Messung |
| Measuring | Austausch läuft, Stabilitätsfenster ist noch nicht ausreichend |
| PTP tracking stable | Mindestens 8 frische Messungen über mindestens 6 Sekunden, kleine Regressionsabweichung; kein Audio-Lock |
| PTP data stale / Grandmaster lost | Sync/Delay-Messungen oder Announce-Nachrichten fehlen |
| Offset | Masterzeit minus lokalem, UTC-verankertem Sitzungstakt; mit gültigem UTC-Offset umgerechnet |
| Mean path delay | Aus dem Vier-Zeitstempel-E2E-Austausch geschätzte mittlere Laufzeit |
| Relative frequency | Steigung der gemessenen Zeitabweichung in ppm; keine angewendete Audiokorrektur |

Der lokale Messtakt wird beim Start an UTC verankert und läuft danach monoton über QPC/steady_clock.
Die Windows-Uhr wird nicht verändert. Spätere Windows-Uhrkorrekturen springen nicht in die Messreihe.
Der Software-GM verwendet diesen lokalen OS/QPC-Oszillator. Er kündigt Clock-Class **248**,
unbekannte Genauigkeit **0xFE** und Time-Source **0xA0** an. Er setzt keine GPS-/UTC-/TAI-
Traceability-Flags und deklariert eine beliebige Zeitskala (`ptpTimescale=false`), keine
nachgewiesene TAI-Zeit. Die Ausgangsepoche wird lediglich einmal an die lokale Windows-Zeit angelehnt.
Wenn ein PTP-Zeitskalen-Master keinen gültigen UTC-Offset ankündigt, bleibt der umgerechnete
Offset leer; der rohe Zeitskalenunterschied steht im Diagnose-JSON als `rawOffsetNs`.
Bei Zeitstufen über einer Sekunde wird das Stabilitätsfenster neu aufgebaut.

Diese erste Fassung verwendet **Anwendungs-Zeitstempel** beim Senden/Empfangen. Windows-Scheduling,
Netzwerk-Asymmetrie und Warteschlangen beeinflussen das Ergebnis. Es gibt noch keine Kernel-/NIC-
Hardware-Zeitstempel, keine garantierte Mikrosekundenpräzision und keinen virtuellen Clock-Servo.
Ein symmetrischer Pfad wird angenommen; Messungen mit negativer oder über einer Sekunde liegender
mittlerer Pfadlaufzeit werden verworfen. Messfenster und Kandidaten-/Paketlisten sind begrenzt.

Unterstützt wird das PTPv2-Default-Transportfeld. PTPv1, L2/gPTP, P2P, Unicast-Aushandlung,
Authentifizierungsprofile, automatische Grandmaster-Election und Hardware-Clock-Steuerung sind noch offen.
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
Für die ausdrücklich gewählte GM-Rolle gilt `"clockSource": "ptp-grandmaster"`.
`internal` bleibt Standard. `ptp` als disziplinierte Audio-Clock wird weiterhin ausdrücklich
abgelehnt; Monitor-Betrieb und tatsächliche Audio-Synchronisation werden nicht gleichgesetzt.

## Prüfung und nächste Schritte

Automatisiert geprüft: feste Wire-Fixtures, Parser-Grenzen, signierte Korrekturwerte,
One-/Two-Step, vertauschte Reihenfolge, Sequenz-Wrap, falsche Antworten, E2E-/UTC-Rechnung,
synthetische 50-ppm-Drift, Timeout/Master-Wechsel/Zeitstufen sowie ein tatsächlicher UDP-Austausch
über Loopback. Synthetische Referenzpakete bleiben auf die isolierten Protokolltests beschränkt. Außerdem wird geprüft, dass
die Engine denselben Monitor teilt und keine Audio-Synchronisation erfindet.
Zusätzlich werden die tatsächlichen Engine-Implementierungen von Software-GM und Monitor gemeinsam über echte Loopback-UDP-Sockets
getestet: erzeugte Wire-Nachrichten, Freilauf-Flags, Delay-Antworten, Domain-Filterung, Konfliktwarnung,
GM-Ausfall und erneute Übernahme nach GM-Neustart ohne Neustart des Monitors.

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
