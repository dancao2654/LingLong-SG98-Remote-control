# KFM-Ersatzsteuerung: Offline-Kandidat vom 03.11.2026

**Entwicklungsstand 2026.11.03.1, auf Controller und Waveshare-AMOLED-1.75 geflasht und teilweise unter Aufsicht geprüft.** Der Stand basiert auf 2026.10.28.6. Die reale Prüfrunde vom 03.11.2026 bestätigt kurze manuelle Start-/Stop-Läufe bis 2000 U/min, Funkverlust mit Quittierung, einen Servo-Einschaltzyklus, niedrigen zeitgesteuerten Purge und eine grobe GBW-Zielabschaltung. Vollständige Abnahme und Weitergabefreigabe stehen weiterhin aus. Siehe [Prüfbericht und offene Punkte](TSE-KFM-Pruefstand-20261103.md).

Die folgenden Abschnitte beschreiben überwiegend den ursprünglichen Vorbereitungsstand. Aussagen über damals ausstehende Hardwareprüfungen sind anhand des verlinkten Prüfberichts einzuordnen; der Betrieb ohne Laptop und die vollständige Prüfung aller Grenzen bleiben offen.

Der Nutzer hat den identischen Treiber **TSE-B-D-100-KFM** an der zweiten Mühle bestätigt und einen Bereich bis **2000 U/min** gewünscht. Die Software ist als länger verwendbarer Ersatz geplant. Die Vorgabe ist ein Entwicklungsziel, kein Nachweis einer mechanisch zulässigen Mühlendrehzahl oder einer Betriebssicherheit über mehrere Monate.

## Funktionsumfang

| Bereich | Vorbereiteter Umfang |
|---|---|
| Manuelles Mahlen | Einstellbare Drehzahl bis 2000 U/min; ausdrücklicher Start und Stop am Touchscreen |
| Gewichtsmahlen | Bookoo, automatisches Tarieren, Hauptphase bis 1500 U/min, Schlussphase bis 500 U/min; bestehende Umschalt-, Vorhersage- und Lernparameter |
| Zeitgesteuerter Purge | Button-Purge, Vorwärts- und Rückwärtsdrehzahl bis zum eingestellten Maximum, Verzögerung, Dauer und Null-Drehzahl-Pause |
| Drehzahlwechsel / Umkehr | Bestehende begrenzte Sollwertverarbeitung; Umkehr erst nach geprüftem Stillstand |
| Start ohne PC | Neuer bewusster Start kann die Freigabe nach gültiger KFM-Erkennung, geprüftem Stillstand und frischem Fernbedienungskontakt selbst anfordern |
| Abbruch | Stop bei Funkverlust, Waagenverlust, Treiberfehler, unplausibler Richtung/Überdrehzahl und Ablaufgrenzen |
| Wiederkehr nach Fehler | Kein neuer Lauf allein durch Wiederverbinden; nach einem verriegelten Abbruch muss der geprüfte Stillstand über die Idle-Seite bestätigt werden |
| Einstellungen | Bleiben einstellbar und werden nicht durch einen Werksreset ersetzt |

Die Haupt-/Schlussphasen-Grenzen 1500/500 entsprechen dem bisherigen Gewichtsmenü. Es wurde kein stiller Wechsel auf 2000 U/min in beiden Gewichtsphasen vorgenommen. Der zuvor erfolgreiche reale Bezug lief mit 800/300 U/min auf 18,0 g; höhere Werte in beiden Phasen bleiben für eine spätere reale Prüfrunde offen.

**Nicht als einsatzbereit umgesetzt ist das drehmomentabhängige Auto-Purge samt Drehmomentkalibrierung.** Dafür fehlen verifizierte KFM-Skalierung und reale Kalibrierdaten. Die betroffenen Optionen sind im neuen gekoppelten Profil ausgeblendet, statt eine funktionierende Automatik vorzutäuschen. Alle zeitgesteuerten Purge-Einstellungen bleiben verfügbar. Motor-Rampen-/Drehmomentfunktionen werden nicht allein deshalb freigegeben, weil ein Menüeintrag existiert.

## Bedienung des Kandidaten

Für den neuen Stand müssen Controller **und** Touchscreen aus demselben Kandidatenpaket zusammen verwendet werden. Der Controller fordert Kennungen für jeden Start an; ein unverändert alter Touchscreen liefert diese nicht. Ein alter Touchscreen kann den Kandidaten deshalb nicht einfach starten. Der neue Touchscreen behält die bisherige Kommunikation zu älteren Controllern bei.

Nach einem normalen abgeschlossenen Lauf genügt ein neuer bewusster Start am Touchscreen. Eine USB-Freigabe ist in diesem Profil nicht erforderlich. Nach einem verriegelten Fehler die Ursache beseitigen, Stillstand abwarten und gezielt zur normalen Idle-/Mahlansicht zurückkehren. Die Quittierung wird nur bei vollständig geprüftem Stillstand akzeptiert; sie startet keinen Motor. Ein laufender Ablauf oder ein Menü wird durch einen Start nicht heimlich übersprungen.

Bei aktiviertem **Button-Purge** löst der erste Stop während des manuellen Mahlens bewusst den Purge aus. Ein weiterer Stop bricht den Purge ab. Diese bisherige Bedienbedeutung bleibt erhalten und muss dem späteren Nutzer erklärt werden. Der mechanische Main Power Switch muss erreichbar bleiben.

Ohne PC braucht der Controller weiterhin eine passende eigene Versorgung. Der Programmier-USB-Anschluss und die nachgerüstete Servo-USB-C-Verbindung sind unterschiedliche Anschlüsse; die Servo-Verbindung führt benutzerdefiniertes RS485 und ist kein Computer-USB-Port.

## Schutzfunktionen und Grenzen

Der neue Kandidat hat ein Softwaremaximum von 2000 U/min und eine Überdrehzahlschwelle von 2150 U/min. Der gelesene Treibergrenzwert muss zum Profil passen. Diese Parameter sind noch nicht am Motor validiert. Normale andere Builds behalten ihre bisherigen eigenen Grenzen.

Ein Vorgang ist ab dem ersten Nichtnull-Sollwert auf **120 Sekunden einschließlich Purge-Pausen** begrenzt. Das separate Auto-off-Menü ist entsprechend auf maximal 120 Sekunden begrenzt; eine dortige Null hebt die unabhängige Gesamtgrenze nicht auf. Eine angeforderte Freigabe ohne beginnenden Lauf verfällt weiter nach 30 Sekunden. Frische Fernbedienungsbefehle innerhalb von drei Sekunden bleiben erforderlich.

Die bisherigen Prüfungen auf gültigen Einschaltzustand, Null-Preset, SON/SP1-Zustand und vollständigen Stop-Lesenachweis bleiben erhalten. Keine automatische Änderung von Motorabstimmung, Eingangsfunktionen oder EEPROM-PA24. Diagnose-Fehlernachstellung der Waagenverbindung ist im neuen Standalone-Profil deaktiviert.

**Noch ungeklärt bleibt die unabhängig wirksame Abschaltung bei ausgefallenem ESP32 oder unterbrochener RS485-Leitung.** Ein Programm kann über eine unterbrochene Verbindung keinen Stop senden. Die bestehenden Funk-/Waagenabbruchtests beweisen diesen Fall nicht. Vor einer Zusage zur Nutzung über Monate muss die konkrete Abschaltstrategie geklärt und geprüft sein; weder erfolgreiche Simulationen noch ein einzelner 18-g-Bezug liefern diese Zusage.

## Warum Controller und Touchscreen angepasst wurden

Der bisherige Touchscreen wiederholt noch nicht bestätigte Befehle. Ohne die frühere USB-Sperre könnte ein verspätetes Startpaket nach einem Abbruch erneut wirksam werden. Das neue Paar versieht Start, Stop und Idle-/Moduswechsel mit einer monotonen Tastendruckkennung innerhalb einer zufälligen Controller-Epoche.

Wiederholungen behalten dieselbe Kennung. Derselbe akzeptierte Start löst keinen zweiten Lauf aus. Derselbe Stop startet einen Purge nur einmal; ein zweiter echter Tastendruck erhält dagegen eine neue Kennung und kann abbrechen, auch wenn die Statusantwort noch nicht angekommen ist. Ein neuerer Stop/Idle-Befehl sperrt ältere Startpakete. Ein Controller-Neustart oder ein nicht zuordenbarer Not-Stop wechselt die Epoche. Die Kennungen dienen der eindeutigen Befehlsverarbeitung, nicht der Authentifizierung.

## Prüfstand und nächste reale Runde

Die Offline-Suite enthält vorhandene TSE-/Transportprüfungen und neue Integrationen mit echten API-, Backend-, Purge- und Gewichts-Funktionen. Modelliert sind UART, Zeitablauf und mechanische Reaktion. Neue Fälle prüfen Start ohne USB, 2000-U/min-Begrenzung, 1500/500-Gewichtsphasen, Vor-/Rückwärts-Purge, Stop-Quittierung, Funk-/Waagenverlust, Wiederholungen, Überholen eines Startpakets durch einen Stop, Controller-Neustart und fehlende/alte Kennungen. Die echten Touchscreen-Funktionen für Menüfilter und Grenzen laufen zusätzlich im nativen Modell. Sanitizer prüfen Speicher- und Laufzeitfehler. Das beweist noch keine reale Funkkommunikation oder mechanische Sicherheit dieser neuen Version.

Für die reale Runde: zunächst kompatible Gerätekennung, Daten-/Einstellungsbackup und ruhigen Einschaltzustand prüfen. Danach Start ohne PC bei niedriger Drehzahl, erneuter Start nach regulärem Stop, Abbruch/Quittierung und Ein-/Ausschaltzyklen prüfen. Anschließend den gesamten vorgesehenen Drehzahlbereich, Purge-Richtungswechsel und wiederholte Gewichtsbezüge unter Aufsicht prüfen. Das konkrete Konzept für Leitungs-/Controllerausfall muss vor entsprechenden Bewegungsversuchen stehen. Ergebnisse der früheren Firmware bleiben ihren ursprünglichen Versionen zugeordnet.

Es ist noch keine Monats-Erprobung oder Wiederholgenauigkeit nachgewiesen. Der Kandidat wurde inzwischen auf beide Geräte übertragen und teilweise geprüft; maßgeblich ist der verlinkte Prüfbericht.

## Builds

```sh
python3 tools/run_kfm_checks.py --report outputs/kfm-checks.json
pio run -e controller-kfm-standalone-candidate -e remote-waveshare-175-standalone-candidate -j 2
```

Die App-Dateien dieses Pakets sind Controller- und Touchscreen-Kandidaten, keine pauschalen Full-Flash-Pakete. Vor einem späteren Einsatz müssen Gerätekennung, Partitionierung und der dann bestätigte Anschluss-/Versorgungszustand geprüft werden. Das Paket enthält bewusst keinen automatisch ausführenden Flasher.
