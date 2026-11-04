> Aktueller getesteter Stand: [28.10.2026, Version2026.10.28.6](TSE-KFM-Stand-20261028-DE.md). Der folgende Text dokumentiert eine frühere Phase.

# KFM: gemeinsame Funktionsprüfung mit Version 2026.10.27.10

Stand: 27.10.2026. **Offline vorbereiteter Kandidat, noch nicht geflasht oder am Motor geprüft.** Auf dem zuletzt geprüften Controller läuft .8. Diese Anleitung ersetzt für die nächste Betriebsprüfung die älteren .4/.6/.8-Einzelschritte; deren Nachweise bleiben erhalten.

## Was die neue Variante vorbereitet

Build `controller-kfm-run-candidate`, Controller MAC `B8:1F:3F:C8:F5:30`, Waveshare RS485-CAN. Der vorhandene Bildschirm kann bleiben; sein Start/Stop-Protokoll wird beibehalten. Kein Display-Update vorbereitet.

Der normale Zustandsablauf für Mahlen, manuell ausgelösten Purge und GBW verwendet beim KFM den temporären PA25-Sollwert (`0x99`), SON (`0x10F`) und unverändert Auswahl 2 (`0x110`). PA24 wird in diesem Kandidaten nicht geschrieben. Ein nichtnulliger PA24-Einschaltwert ist zulässig, sofern PA25 null und die übrigen Werkseinstellungen passend sind. Vor dem ersten Drehzahlbefehl werden Freigabe bei Sollwert null und Stillstand zurückgelesen. Bei Richtungswechsel wird erst Sollwert null/Freigabe aus und gemessener Stillstand abgewartet. Auch Wiederanlauf nach einer Nullpause wartet auf Stillstand.

Diese Version erlaubt **höchstens 500 U/min**, maximal **30 Sekunden seit der ersten Bewegungsanforderung einer Freigabe**, und eine Freigabe ohne Start verfällt nach 30 Sekunden. Die Softwarefristen wirken nur bei laufendem, kommunizierendem Controller. Die gespeicherten 1100 U/min werden nicht durch dieses Update absichtlich überschrieben; ein solcher Sollwert wird am Motor auf 500 begrenzt. Vor der Prüfung am Bildschirm bewusst 100 einstellen, damit Anzeige und tatsächlich angeforderte Geschwindigkeit zusammenpassen. Drehzahlgrenze, höhere Drehzahlen und Dauerbetrieb werden erst nach gesonderter Bewertung geändert.

Eine ausdrückliche USB-Freigabe (`api tse arm`) ist weiter nötig. Start ist vor Freigabe oder bei unvollständiger Rückleseprüfung gesperrt. Bei Funkverlust (keine frischen akzeptierten ESP-NOW-Befehle für mehr als 3 Sekunden oder Funkempfänger aus), Alarm, fehlerhaften Registerwerten und Kommunikationsfehlern wird Stop angefordert und die Freigabe verworfen. Wiederkehrende Verbindung startet den Motor nicht automatisch. Reine USB-Diagnoseabfragen halten den Funkkontakt nicht künstlich frisch.

Das Drehmoment ist noch nicht kalibriert. Deshalb sind drehmomentabhängige automatische Purge-Erkennung und Lernen für KFM ausgesetzt. Manuell ausgelöste Purge-Zeitabläufe und GBW bleiben als Prüffunktionen vorhanden. Kurze Purge-Vibrationsimpulse können durch das notwendige Warten auf Stillstand und Rückleseprüfung kürzer ausfallen oder ausbleiben; deren Wirkung und Zeitverhalten sind ein eigener Abnahmepunkt. GBW setzt eine tatsächlich unterstützte verbundene Waage voraus und ist noch nicht am KFM geprüft. Ein leerer GBW-Test kann regulär wegen fehlenden Mahlguts abbrechen.

Keine automatischen Änderungen an Motortuning, P3-Eingangszuordnung oder Alarmrücksetzung. Explizite persistente PA-Schreibbefehle sind im Transport ausgeschlossen. SON/SP1 werden als Steuerregister geschrieben; über deren mögliche nichtflüchtige Wirkung wird keine unbelegte Zusicherung gemacht.

## Gemeinsame Prüfrunde

Alle Bewegungsprüfungen mit leerer geschlossener Mühle, Bediener am erreichbaren Main Power Switch. Nur spannungsfrei umstecken. Jetzt während der Offline-Vorbereitung bleibt der Servo getrennt. Es wird nichts automatisch geflasht oder gestartet.

| Schritt | Aktion in der gemeinsamen Runde | Nachweis vor dem Weitergehen |
| --- | --- | --- |
| 1 | Main Power aus; Controller per USB prüfen und ausschließlich das Kandidaten-App-Image aktualisieren. | MAC, Build-Umgebung und Version .9 stimmen. Vorhandene Partitionierung/NVS bleiben erhalten. Kein Start ohne Freigabe. |
| 2 | Bereits verdrahteten Servo einschalten, keinen Start drücken. Bildschirm auf 100 U/min. | KFM erkannt, vollständige Werkseinstellungsprüfung, Alarm 0, PA25=0, SON=0, Auswahl 2, Ist-Drehzahl 0. `tse_run_candidate.stopReadbackVerified=true`. |
| 3 | Nach Bereitschaft am Schalter USB-Freigabe, Bildschirm Start und nach etwa 2 Sekunden Stop. Button-Purge dafür zunächst aus. | Tatsächliche Bewegung und Stop beobachtet; PA25 und SON sowie Ist-Drehzahl wieder 0. Schreibantwort allein zählt nicht. |
| 4 | Jeweils gezielt 100 → 200 → 500 U/min, dann Stop; gespeicherter Sollwert über 500 nur als bewusste Begrenzungsprüfung. | Soll-/Ist-Werte, Vorzeichen und softwareseitige Begrenzung passen. Keine Werte über 500 angefordert. |
| 5 | Manuell ausgelösten Purge mit kurzen niedrigen Vor-/Rückwärtsphasen prüfen. | Richtungswechsel erst nach gemessenem Stillstand; Rückkehr in den Ruhezustand. Bei einer Nullpause bleibt derselbe begrenzte Ablauf möglich. |
| 6 | Neue Funkverlust-Prüfung bei 100 U/min: Bildschirm direkt nach bestätigtem Start vollständig stromlos lassen. | Stop-Grund `remote_link_lost` vor Host-Aufräumbefehl; Motorstillstand und Rücklesewerte stimmen. Nach Wiedereinschalten kein Neustart. |
| 7 | Automatisches Ende bei 30 Sekunden separat prüfen; anschließend Neustart mit ausgeschaltetem Servo und später kontrollierter Versorgung. | Freigabe verworfen, kein unaufgeforderter Wiederanlauf. Optional GBW nur mit Waage gesondert prüfen. |

Bei unbestätigtem Stop, unerwarteter Bewegung/Richtung oder fehlender Rückmeldung: Main Power aus und Ergebnis sichern. Nicht einfach erneut freigeben. Nach einem Controller-Neustart bei zuvor laufendem Servo kann eine nichtnullige PA25-Vorgabe bestehen bleiben; der Kandidat soll dann blockieren. Eine neue Prüfung beginnt erst nach sicherem Ausschalten und bestätigtem Nullzustand.

Zum Aufzeichnen `tools/kfm_capture.py` mit frisch ermitteltem USB-Port und erwarteter MAC verwenden. Das Skript liest ausschließlich; Freigaben und menschliche Aktionen getrennt mit Zeitstempel festhalten. `api device`, `api tse` und `api status` enthalten Kennung, Kandidatengrenzen, Freigabe und Stop-Rücklesestatus. `api tse disarm` fordert Stop an, sein allgemeines ACK allein ist kein Stop-Nachweis. Vor Konsolenende unbedingt den ursprünglichen Stop-Grund sichern, anschließend frische Rücklesewerte.

## Noch keine Freigabe für unbeaufsichtigten Alltag

Hardwarebestätigt aus .7/.8 sind begrenzte 100-U/min-Bewegung, Bildschirm-Start/Stop, nominales 5-Sekunden-Ende sowie Stop bei gezielt ausgeschalteter Funk-Schnittstelle mit anschließender Funkrückkehr ohne Neustart. Das ist kein Nachweis eines echten fehlenden-Paket-Timeouts bei aktivem Empfänger. Der neue variable PA25-Betrieb ist bisher offline geprüft. Die ergänzenden Bedienabläufe und Änderungen aus .10 beschreibt [der Offline-Prüfbericht](TSE-KFM-Offline-Bedienablaeufe-DE.md).

Ein ausgeschalteter Controller kann keinen Stop senden; über eine unterbrochene RS485-Verbindung kommt kein Stop an. Ein wirksamer treiberseitiger Kommunikations-Watchdog oder eine unabhängige Freigabe-/Stop-Kette ist für diese Ausfälle noch nicht nachgewiesen. Solche Fehler nicht durch Abziehen im laufenden Betrieb improvisieren. Höhere Drehzahlen, mechanische Drehrichtung unter allen Abläufen, Lastbetrieb und Alltagstauglichkeit bleiben gesonderte Abnahmepunkte.
