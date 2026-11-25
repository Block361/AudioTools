# OpenAoIP: mehrere Geräte verbinden

Das Windows-Paket enthält eine eigenständige C++-Anwendung. Auf den beteiligten Rechnern sind kein Node.js und keine C++-Entwicklungsumgebung erforderlich. ZIP entpacken und die folgenden Befehle im entpackten Ordner ausführen.

## 1. Controller starten

Auf dem Steuerrechner:

```powershell
.\openaoip.exe controller --host 0.0.0.0 --port 3210
```

Im Browser `http://127.0.0.1:3210` öffnen. **Connect device** zeigt die verfügbaren LAN-Adressen und erzeugt einen Startbefehl für weitere Geräte. Bei mehreren Netzwerkadaptern die Adresse des gemeinsamen Audio-Netzwerks wählen. Zum Beispiel `http://192.168.1.10:3210`.

Nur ein Controller muss laufen. Der bisherige Befehl `demo` startet zusätzlich lokale Testquellen; für das eigentliche Netzwerk genügt `controller`.

## 2. Geräte starten

Auf Rechner A, beispielsweise an der Bühne:

```powershell
.\openaoip.exe device --id buehne --controller http://192.168.1.10:3210 --capture default --capture-map 1,1 --playback default
```

Auf Rechner B, beispielsweise am FOH:

```powershell
.\openaoip.exe device --id foh --controller http://192.168.1.10:3210 --capture default --capture-map 1,1 --playback default
```

**Die IP-Adresse durch die tatsächliche Controller-Adresse ersetzen. Jedes Gerät braucht eine andere, dauerhaft beibehaltene ID.** Beide Rechner erscheinen automatisch als Geräte mit jeweils einem Sende- und Empfangsstream. Jeder Stream ist ein Stereo-Paar. `--capture-map 1,1` dupliziert den ersten Mikrofoneingang auf L/R; für eine Stereoquelle stattdessen `1,2` verwenden.

Ein Gerät kann auf dem Controller-Rechner selbst laufen. Auch dort die LAN-Adresse des Controllers verwenden, wenn Audio mit anderen Rechnern ausgetauscht werden soll.

## 3. Im Controller routen

- **Bühne → FOH:** Zeile `buehne / Input 1/1`, Spalte `foh / Stereo input 1` verbinden.
- **FOH → Bühne:** Die umgekehrte Verbindung setzen.
- Ein Sender kann mehrere Empfänger versorgen; pro Empfängereingang wird eine Quelle gewählt.
- Unter **Receiver signals** zeigen Pegel und `receiving`, ob Pakete und Audio ankommen.
- Unter **Devices** stehen IP-Adresse, Sende-/Empfangskanäle, UDP-Ports und Hardwarestatus.

Audio fließt direkt zwischen den Geräten. Der Controller verwaltet Anmeldung und Routing; laufende Verbindungen arbeiten bei einem Controller-Ausfall mit dem zuletzt erhaltenen Plan weiter. Nach einem Neustart melden sich Geräte erneut an und gespeicherte Routen werden wieder angewendet.

Für lokales Mikrofon-Monitoring Kopfhörer verwenden. Es werden beim erstmaligen Start keine Routen automatisch angelegt. Bereits gespeicherte Routen reconnecten.

## Geräte und Kanäle auswählen

```powershell
.\openaoip.exe audio-devices
```

Statt `default` kann ein Geräteindex oder ein exakt geschriebener, in Anführungszeichen gesetzter Gerätename verwendet werden.

Vier Stereo-Paare benötigen acht vom Betriebssystem bereitgestellte Hardwarekanäle:

```powershell
.\openaoip.exe device --id rack8 --controller http://192.168.1.10:3210 --streams 4 --inputs 4 --capture-map 1,2,3,4,5,6,7,8 --playback-map 1,2,3,4,5,6,7,8
```

`--streams 0` deaktiviert das Senden, `--inputs 0` das Empfangen. `--volume 25` setzt die Wiedergabe auf 25 %. `--host 192.168.1.20` bindet das Gerät ausdrücklich an eine lokale Netzwerkschnittstelle, einschließlich der Anmeldung am Controller.

## Ohne Mikrofon oder Soundkarte testen

```powershell
.\openaoip.exe device --id test-a --controller http://192.168.1.10:3210 --tone 440 --playback off --port 0 --monitor-port 0
.\openaoip.exe device --id test-b --controller http://192.168.1.10:3210 --tone 660 --playback off --port 0 --monitor-port 0
```

Die beiden Befehle können in zwei Terminals auf demselben Rechner oder auf verschiedenen Rechnern laufen. Audio wird über echte UDP-Verbindungen übertragen und lässt sich unter **Audition** als Empfängeraufnahme prüfen. Die Hardware bleibt in diesem Modus geschlossen. Der Test mit mehreren Prozessen ersetzt keinen Last- oder Latenztest über zwei physische Rechner.

## Konfiguration speichern

In den mitgelieferten Beispielen [device-stage.json](./examples/device-stage.json) und [device-foh.json](./examples/device-foh.json) Controller-Adresse und lokale Audiogeräte anpassen:

```powershell
.\openaoip.exe device --config examples/device-stage.json
```

CLI-Optionen überschreiben gleichnamige Werte aus der Datei. WAV-Pfade in einer Konfigurationsdatei sind relativ zu dieser Datei. Die Controller-Routen werden separat gespeichert; eine Geräte-ID sollte deshalb beim Neustart gleich bleiben.

## Netzwerk und Ports

Im selben vertrauenswürdigen, vorzugsweise kabelgebundenen IPv4-LAN arbeiten:

| Rechner | Standardmäßig eingehend erforderlich |
|---|---|
| Controller | TCP 3210 |
| Gerät | UDP 5004 für ein Stereo-Paar; bei weiteren Paaren folgende Ports |
| Gerät | TCP 5100 für Audition/Download der Empfängeraufnahme |

Die Software ändert keine Firewall-Regeln. Bei mehreren Geräten auf einem Rechner andere Portbereiche wählen oder `--port 0 --monitor-port 0` nutzen. Dabei werden freie Ports zugeteilt und im Controller angezeigt.

Die Controller-Adresse muss direkt erreichbar sein; NAT und Reverse-Proxys sind nicht vorgesehen. Die LAN-Steuerung besitzt noch keine Anmeldung mit Kennwort, Verschlüsselung oder PTP-Synchronisation. Diese CLI-Anleitung nutzt explizite URLs und WASAPI. Die nativen Anwendungen unterstützen Controller-Suche und eine eigene ASIO-Virtual-Soundcard; siehe [Windows-Anleitung](./WINDOWS-START.md).
