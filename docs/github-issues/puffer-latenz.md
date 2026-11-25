# Puffer und Latenz

Stand: OpenAoIP 0.6.3.

Ein Audiopuffer hält kurzzeitig Samples bereit. So kann die Wiedergabe gleichmäßig weiterlaufen,
auch wenn Windows oder das Netzwerk einen Audioblock etwas später liefert. Ein größerer Puffer
gibt mehr Zeitreserve und erhöht normalerweise die Verzögerung. Er kann dauerhaft fehlende Daten,
Übersteuerung oder einen Fehler beim Wiederverbinden nicht beheben.

## Was bedeutet „Frames“?

Ein Frame enthält ein Sample pro Audiokanal. Bei Stereo enthält ein Frame also zwei Samples.
Die Dauer hängt von der Samplerate ab: **Dauer in ms = Frames / Samplerate × 1000**.

| Frames | Dauer bei 48 kHz |
|---|---|
| 64 | 1,33 ms |
| 128 | 2,67 ms |
| 256 | 5,33 ms |
| 512 | 10,67 ms |
| 1024 | 21,33 ms |

Diese Zeiten beschreiben einen Block, nicht die gesamte Verzögerung vom Sender zum Lautsprecher.

## Drei verschiedene Einstellungen

| Einstellung | Aufgabe | Standardeinstellung |
|---|---|---|
| Buffer (frames) | Lokale Audioverarbeitung in WASAPI bzw. bevorzugter ASIO-Hostpuffer | 256 Frames |
| Network latency (ms) | RTP-Empfangspuffer gegen verspätete/ungeordnet eintreffende Pakete | 60 ms |
| Local / drift buffer (ms), Bridge | Puffer zwischen lokaler Geräte-Clock und Netzwerkverarbeitung | 20 ms |

Windows bzw. der ASIO-Host kann eine andere tatsächliche I/O-Puffergröße verwenden.
Die native Größe steht in der Bridge-Diagnose bzw. unter **Host buffer** in der Soundcard.
„Buffer“ ändert nicht die Paketgröße des Netzwerks: RTP verwendet weiterhin **240 Frames pro Paket**,
bei 48 kHz also 5 ms. Bei 44,1/96 kHz entspricht derselbe Paketinhalt einer anderen Dauer.

Capture-, Netzwerk-, Playback- und gegebenenfalls Resampling-Puffer addieren sich entlang des
Signalwegs. Mehrere Bridge-Puffer können beteiligt sein. Angezeigte Sollwerte sind keine
gemessene Ende-zu-Ende-Latenz und kein Beleg für eine samplegenaue gemeinsame Clock.

## Wenn der Ton „stolpert“

1. Prüfen, ob bei der Störung **Packet loss**, **Late** oder **Underruns** steigen.
2. Jeweils nur eine Einstellung ändern und die Wirkung vergleichen.
3. Bei Underruns testweise den lokalen Puffer erhöhen, z. B. auf 40 ms bzw. I/O auf 512 Frames.
4. Bei zu spät eintreffenden Paketen testweise die Netzwerklatenz auf 100 oder 200 ms erhöhen.
5. Lautstärkeabhängige Verzerrung zusätzlich auf Clipping prüfen; mehr Puffer behebt Clipping nicht.

Dies sind Diagnoseschritte, kein bereits nachgewiesener Fix für die gemeldeten Aussetzer.

## Weitere Dokumentation

[PTP](./ptp.md)

[Übersicht](./uebersicht.md) · [Installation](./installation.md) · [Controller](./controller.md) ·
[Audio Bridge](./audio-bridge.md) · [Virtual Soundcard](./virtual-soundcard.md) ·
[Audioqualität](./audioqualitaet.md) · [Recovery](./recovery.md) · [Diagnose](./diagnose.md)
