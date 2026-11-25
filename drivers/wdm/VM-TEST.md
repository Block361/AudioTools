# OpenAoIP – Test in deiner Windows-11-VirtualBox-VM

Dieses Paket ist **testsigniert**, nicht für den produktiven Betrieb signiert. Es ist
für eine separate VirtualBox-VM gedacht. Der Treiber wurde noch nicht im Kernelbetrieb
validiert; ein Bluescreen in der VM ist möglich. Auf dem Host wurde weder ein Zertifikat
vertraut noch ein Treiber installiert oder eine Bootoption geändert.

## 1. Windows fertig einrichten

Windows 11 **x64, Version 24H2 / Build 26100 oder neuer** installieren. Danach die VM
herunterfahren und in VirtualBox einen Snapshot „Vor OpenAoIP-Treibertest“ erstellen.
Das Testpaket in der VM vollständig entpacken, etwa nach `C:\OpenAoIP-Test`.

Für den ersten Audiotest ist keine Verbindung zum Controller auf dem Host nötig. Der
Test startet einen eigenen Controller und prüft Audio vollständig innerhalb der VM.
Die normale VirtualBox-NAT-Verbindung genügt dafür. Ein Compiler oder WDK ist in der VM
nicht erforderlich. OpenAoIP-Soundcard und andere OpenAoIP-Audiotests vorher schließen.

## 2. Nur innerhalb der VM: Testmodus vorbereiten

Windows PowerShell in der VM **als Administrator** öffnen und zum entpackten Paket wechseln:

```powershell
cd C:\OpenAoIP-Test
powershell -NoProfile -ExecutionPolicy Bypass -File .\VM-TEST.ps1 -Action Status
powershell -NoProfile -ExecutionPolicy Bypass -File .\VM-TEST.ps1 -Action Prepare
```

`Prepare` vertraut das mitgelieferte öffentliche Testzertifikat ausschließlich in der
VM und aktiviert dort TESTSIGNING. Danach **die VM neu starten**. Die Skriptausführung
ändert die gespeicherte PowerShell-Ausführungsrichtlinie nicht.

Falls Secure Boot in der VM aktiv ist, stoppt das Skript. Secure Boot muss für diesen
Test in der **VM-Konfiguration** entsprechend eingestellt werden; die genaue Bedienung
hängt von deiner VirtualBox-Version ab. EFI nicht einfach abschalten, wenn Windows als
UEFI-System installiert wurde. Am physischen Host nichts ändern. Ist BitLocker-Schutz
in der VM aktiv, stoppt das Skript ebenfalls; vor Bootänderungen muss der Zugriff auf
den Wiederherstellungsschlüssel gesichert sein. Das Skript ändert weder Secure Boot
noch BitLocker automatisch.

## 3. Treiber installieren und Audioweg prüfen

Nach dem Neustart wieder eine Administrator-PowerShell im Paketordner öffnen:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\VM-TEST.ps1 -Action Install
powershell -NoProfile -ExecutionPolicy Bypass -File .\VM-TEST.ps1 -Action Test
```

Einen vom Installer angeforderten Neustart vor `Test` durchführen.
Der Installationshelfer prüft weiterhin Katalogsignatur und Datei-Hashes; es gibt
keinen Schalter zum Umgehen dieser Prüfung.

Der Audiotest spielt unterschiedliche Signale auf **OpenAoIP Senden 01/02** und
**Senden 31/32**, vertauscht ihre Ziele über echtes RTP/UDP und liest sie aus
**Empfang 01/02** sowie **Empfang 31/32** zurück. Er öffnet ausschließlich diese
OpenAoIP-Geräte. Die Ergebnisse stehen im Unterordner `test-logs`.

- **PASS:** Der getestete Windows-Audioweg funktioniert.
- **SKIP / Exitcode 77:** Der Treiber ist nicht verfügbar; kein bestandener Audiotest.
- **FAIL:** Fehlerausgabe und Gerätestatus in `test-logs` für die Fehlersuche aufbewahren.
- **Bluescreen:** Stopcode fotografieren/notieren; Snapshot für die Wiederherstellung nutzen.

Es werden zunächst nur Installation, Endpunkterkennung und der Audio-Rückweg getestet.
Driver Verifier, Neustart-/Suspend-/PnP-Tests und Dauerbetrieb folgen erst danach.

## 4. Rückbau

Am einfachsten den sauberen VirtualBox-Snapshot wiederherstellen. Alternativ in der VM:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\VM-TEST.ps1 -Action Remove
```

Die Geräteentfernung im Dialog bestätigen. Die Aktion entfernt anschließend nur die
von `Prepare` neu hinzugefügten Testzertifikate und schaltet TESTSIGNING wieder aus,
falls es vorher nicht aktiv war. Danach die VM neu starten. Das Treiberpaket kann im
Windows-Treiberspeicher verbleiben; eine Snapshot-Wiederherstellung setzt die VM vollständig zurück.

Der private Signaturschlüssel ist **nicht** im Paket enthalten. Das mitgelieferte
Zertifikat gehört nur in diese Test-VM, nicht auf normale Arbeitsrechner.

Microsoft: [Testsigned Code laden](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option),
[Testzertifikate](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/creating-test-certificates).
