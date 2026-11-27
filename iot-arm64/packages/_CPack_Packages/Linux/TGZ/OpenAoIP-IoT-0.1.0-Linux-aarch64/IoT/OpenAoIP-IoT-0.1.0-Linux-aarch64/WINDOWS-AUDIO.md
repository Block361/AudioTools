# Windows-Audio mit OpenAoIP 0.6

Die neue eigenständige **Audio Bridge** verbindet vorhandene WASAPI-Geräte, virtuelle Kabel
und Systemaudio ohne OpenAoIP-Kerneltreiber. Bedienung: [Windows-Anleitung](./WINDOWS-START.md).
Dieser Text beschreibt den separat versionierten WDM-Prototyp 0.5 mit Protokoll v2.


## Aktueller Stand – vor der Installation lesen

Die WDM-Implementierung für normale Windows-Programme ist im Quelltext und in der
Soundcard-Anwendung vorhanden. Sie ist **noch nicht für den produktiven Einsatz freigegeben**.
Hier wurden Build, INF-Prüfung, Katalogerstellung, die Puffer-/Kanalzuordnung und der
Installationsschutz geprüft. **Ein Test im laufenden Windows-Kernel steht aus.**

Das Entwicklungsarchiv ist ausdrücklich **UNSIGNED**. Es erzeugt noch keine verwendbaren
Audioeinträge auf deinem normalen Windows-PC. Dafür benötigt das Projekt ein von Windows
akzeptiertes signiertes Treiberpaket und einen erfolgreichen Test auf einem separaten System.
Das reguläre Soundcard-Setup installiert weiterhin keinen ungeprüften Kerneltreiber.

Für eine separate Windows-11-VirtualBox-VM gibt es zusätzlich ein **TESTSIGNED**-Paket mit
eigenem öffentlichen Testzertifikat und Schrittfolge. Es ist keine Produktionsfreigabe:
[VM-Test](drivers/wdm/VM-TEST.md). Nur die explizite Aktion `Prepare` dieses VM-Pakets
vertraut dem Zertifikat im Gast und aktiviert dort TESTSIGNING; sie verweigert die
Ausführung außerhalb einer VirtualBox-VM. Der private Schlüssel bleibt auf dem Build-PC.

## Vorgesehene Windows-Geräte

Der Treiber meldet 16 feste Stereo-Paare pro Richtung, jeweils PCM16 / 48 kHz:

| In den Windows-Soundeinstellungen | Bedeutung |
|---|---|
| OpenAoIP Senden 01/02 | Wiedergabegerät: Programm-Ausgang → Netzwerk TX 1/2 |
| OpenAoIP Senden 03/04 | Wiedergabegerät: Programm-Ausgang → Netzwerk TX 3/4 |
| … bis Senden 31/32 | Weitere getrennte Wiedergabegeräte |
| OpenAoIP Empfang 01/02 | Aufnahmegerät: Netzwerk RX 1/2 → Programm-Eingang |
| OpenAoIP Empfang 03/04 | Aufnahmegerät: Netzwerk RX 3/4 → Programm-Eingang |
| … bis Empfang 31/32 | Weitere getrennte Aufnahmegeräte |

Windows-Anwendungen können diese Geräte über ihre üblichen Aufnahme-/Wiedergabeauswahlen
verwenden. Es wird kein ASIO-Support in der jeweiligen Anwendung benötigt. Windows mischt
im Shared-Modus mehrere Programme auf demselben Gerät. Jedes Paar besitzt einen eigenen
Treiberpuffer. In der Soundcard wird festgelegt, wie viele dieser Kanäle tatsächlich über
das Netzwerk übertragen werden. Nicht konfigurierte Kanäle bleiben stumm; bei einer
ungeraden Kanalzahl bleibt die zweite Seite des letzten Paars stumm.

Die festen Stereo-Geräte bleiben unabhängig von der Netzwerk-Kanalzahl gleich benannt.
Es gibt keine dynamische Änderung des Windows-Geräteformats beim Start eines Streams.
Der WDM-Modus ist zunächst auf **48 kHz** beschränkt. ASIO unterstützt weiterhin
44,1/48/96 kHz und bis zu 32 Kanäle pro Richtung. ASIO und WDM verwenden die
Netzwerk-Brücke derzeit alternativ, nicht gleichzeitig.

## Bedienung nach erfolgreicher Signierung und Treiberprüfung

1. Das native Soundcard-Setup installieren oder aktualisieren.
2. In der Soundcard **Windows-Audio (WDM) – Stereo-Kanalpaare** wählen.
3. **WDM-Treiber installieren** wählen und die **OpenAoIPWdm.inf** eines signierten,
   geprüften Pakets auswählen. Das Programm prüft Katalogsignatur und Dateihashes,
   bevor es ein Root-Audiogerät anlegt. Windows prüft zusätzlich seine Treiber-Richtlinien.
4. Einen eventuell angeforderten Neustart durchführen.
5. In der Soundcard Netzwerkschnittstelle, TX-/RX-Kanalzahl und Controller wählen,
   dann **Starten**. Die Samplerate wird im Windows-Modus auf 48 kHz festgelegt.
6. Über **Windows-Soundeinstellungen** oder die Audioauswahl deines Programms das
   passende Senden-/Empfangspaar auswählen. Die Soundcard zeigt an, wie viele OpenAoIP-
   Aufnahme- und Wiedergabegeräte Windows tatsächlich als aktiv meldet.
7. Im OpenAoIP Controller die TX-/RX-Kanäle verbinden.

Beispiel: Ein Programm gibt über **Senden 01/02** aus. Im Controller TX 1/2 dieses PCs
mit RX 1/2 des Ziel-PCs verbinden. Dort nimmt ein Programm über **Empfang 01/02** auf.
Die Soundcard-Anwendungen müssen auf beiden PCs laufen. Der Controller transportiert kein Audio.

Falls der Treiber fehlt oder die Version nicht passt, zeigt die Soundcard einen Fehler;
sie legt keine scheinbaren Audioeinträge an. Eine positive Treiber-Verbindung allein
ersetzt nicht die Kontrolle, ob Windows die Audio-Endpunkte erfolgreich aufgebaut hat.

## Entwickler: Bauen und Testen

Mit Visual Studio, Windows SDK 26100 und passendem WDK:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ./build.ps1 -BuildDirectory build/desktop -Wdm -Test -Package
```

Ein separat installiertes WDK kann mit `-WdkRoot C:/pfad/zum/wdk/c` übergeben werden.
Die WDK-Bibliotheken werden nicht ins Repository oder in die Auslieferung kopiert.
Das Paket liegt anschließend unter `build/desktop/packages` und heißt
`OpenAoIP-Windows-Audio-0.5.0-UNSIGNED-development-x64.zip`.

`wdm_tests` prüft dieselbe Queue-Implementierung wie der Kernel, die Transportgrenzen,
ungerade Kanalzahlen, Stummschaltung, Format-/Versionsfehler und die Abbildung der
Stereo-Paare im Benutzerprozess. Diese Tests simulieren den DeviceIoControl-Transport.

`wdm_runtime_tests.exe` ist ein eigener echter Windows-Audiotest. Er öffnet ausschließlich
die OpenAoIP-Endpunkte 01/02 und 31/32, erzeugt zwei unterschiedliche Signale, vertauscht
ihre Ziele über RTP und prüft die aufgenommenen Frequenzen. Er verwendet weder physische
Lautsprecher noch Mikrofone. Ein installierter, kompatibler Treiber und eine gestoppte
Soundcard-Anwendung sind erforderlich. Ohne Treiber liefert der Test Exitcode 77 und
wird von CTest als **Skipped** ausgewiesen:

```powershell
ctest --test-dir build/desktop -C Release -R wdm_runtime_tests --output-on-failure
```

Zusätzlich sind Driver Verifier, Audio-HLK, PnP-/Power-/Suspend-Tests, Client-Abstürze,
Langzeitbetrieb und Messungen der DPC-Laufzeiten auf einem dedizierten System erforderlich.
Die regulären Installer und Audiotests ändern weder Secure Boot noch den Testsigning-Modus
und installieren keine Zertifikate. Die getrennte, explizite VM-Testeinrichtung ist oben
beschrieben. Ein bloß erstellter CAT-Katalog ist keine Signatur.

## Signierung und Veröffentlichung

Für die normale Auslieferung muss der zuständige Projektbetreiber den Treiber über den
Windows-Hardware-Entwicklungsprozess signieren lassen. Ein selbst erzeugtes Zertifikat
auf diesem PC ersetzt die Windows-Kernel-Signierungsanforderungen nicht.
Der Installationshelfer bietet keinen Schalter zum Überspringen der Signaturprüfung.

Quellen: [Microsoft-Treibersignierungsrichtlinie](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-policy--windows-vista-and-later-),
[Microsoft SysVAD-Beispiel](https://github.com/microsoft/Windows-driver-samples/tree/main/audio/sysvad).
Projekt-Repository: [Block361/AudioTools](https://github.com/Block361/AudioTools).
