> Aktueller getesteter Stand: [28.10.2026, Version2026.10.28.6](TSE-KFM-Stand-20261028-DE.md). Der folgende Text dokumentiert eine frühere Phase.

> Historischer .4-Prüfplan. Für die nächste Runde gilt [die .9-Funktionsprüfung](TSE-KFM-Funktionspruefung-DE.md).

# KFM: nächste gemeinsame Prüfrunde

Stand: 27.10.2026. Vorbereitet ist Version **2026.10.27.4**. Diese Version ist auf dem allein per USB angeschlossenen ESP32 installiert; Start, USB-Diagnose und Schreibsperren wurden ohne Mühle geprüft. Die letzte Messung mit angeschlossenem Servotreiber erfolgte mit **2026.10.27.3**. Der neue Null-Preset-Test wurde noch nicht gestartet. Die bisherigen Hardwareprotokolle bleiben unter `outputs/kfm-startup-20261027` erhalten.

## Was bereits feststeht

Die KFM-Erkennung, Initialisierung und normale RS485-Telemetrie funktionieren. Die Eingangsbelegung des Originalcontrollers bleibt erhalten. PA24 ist nach dem Einschalten **−1000 U/min**, PA25 ist **0**, P3-15 ist **0** und P3-16 ist **2**. Der Treiber stand bei allen bisherigen Messungen. Ein bestätigter FC06-Schreibbefehl `PA24=0` änderte diesen Einschaltwert im anschließenden Kaltstarttest nicht dauerhaft. Diesen Speicherbefehl wiederholen wir nicht auf Verdacht.

Der nichtnullige PA24-Wert allein belegt keine Bewegung. Für den Wiederanlauf ist entscheidend, ob die Freigabe und die Auswahl des Null-Presets nach einer Unterbrechung zurückgesetzt werden. Das wird als Nächstes gezielt beobachtet.

## Die beiden vorbereiteten Images

| Image / Build-Umgebung | Zulässiger Zweck |
| --- | --- |
| `controller-kfm-commissioning` | Diagnose mit Startsperre; erlaubt die bekannten Stop-Werte und den ausdrücklich aufgerufenen PA24-Nullschreibversuch. SON=1 bleibt gesperrt. |
| `controller-kfm-zero-probe` | Zusätzlich genau die manuell gestartete SON-Prüfung am vorher verifizierten Null-Preset. Alle Drehzahl-Schreibwerte ungleich 0 sowie die Auswahl des Lauf-Presets bleiben im Transport gesperrt. |

Der Null-Preset-Test kann den Motor im Stillstand **bestromen**. Er gehört in die gemeinsame Runde mit anwesendem Bediener, leerer geschlossener Mühle und erreichbarem Main Power Switch. Er ist kein unbeaufsichtigter Test. Eine Softwarefrist oder ein RS485-Stop ersetzt keine unabhängige Abschaltung.

## Ablauf für ungefähr eine Stunde

1. **Anschließen und Firmware prüfen, etwa 10 Minuten.** Main Power Switch aus. Der Originalcontroller bleibt vom RS485-Bus getrennt. Nur den ESP32-RS485-Controller über seinen normalen Programmier-USB-Anschluss mit dem Mac verbinden. Die nachgerüstete Servo-USB-C-Verbindung ist kein normaler USB-Port. MAC des Controllers: `B8:1F:3F:C8:F5:30`. Der Touchscreen hat eine andere MAC und wird dabei nicht geflasht. Aktuellen Port neu ermitteln; die Nummer kann sich nach dem Umstecken ändern. Version .4 ist bereits installiert; bei passender Kennung ist kein erneutes Flashen nötig. Falls ein Update nötig wird: Das passende App-Image wird ausschließlich an `0x10000` mit der vorhandenen kompatiblen 8-MB-OTA-Partitionierung geschrieben. Vorher Gerätekennung und laufende App-Partition prüfen. Kein pauschales Löschen oder Factory-Image über die NVS-Daten schreiben.
2. **Grundzustand, etwa 5–10 Minuten.** Main Power Switch ein. Firmwarekennung, `phase=complete`, KFM-Identität, gültige Telemetrie, Fehler 0 und Ist-Drehzahl 0 prüfen. `NOT_READY` bleibt bei der Startsperre beabsichtigt. Die erste Einschalt-Aufzeichnung muss `captured=true`, `beforeAnyBackendWrite=true` und `validMask=15` enthalten. Bei unbekannten Zuständen keinen Test starten.
3. **Null-Preset-Freigabe vorbereiten, etwa 5 Minuten.** Die besondere Operator-Konsole startet zunächst nur die Protokollierung. Erst der eingegebene Befehl `probe` beantragt den Test. Die Firmware liest vor SON=1 unter anderem PA4, PA22..25, PA53, P3-Eingangsbelegung, erzwungene und virtuelle Eingänge, Ist-Drehzahl und Alarm. Nur bei passender Konfiguration, PA25=0, PA53=0, bestätigter Stop-Auswahl und Stillstand folgt SON=1. Es wird keine Drehzahl ungleich 0 geschrieben. Warten, bis `waiting_for_power_off` und `holdVerified=true` angezeigt werden. Bei unerwarteter Bewegung oder unbestätigter Rücksetzung Main Power Switch aus.
4. **Eine gezielte Aus-/Ein-Runde, etwa 5–10 Minuten.** Erst jetzt Main Power Switch aus; mit `off` in der Konsole bestätigen. Auf `waiting_for_power_on` warten. Innerhalb der insgesamt 120 Sekunden seit Testbeginn wieder einschalten und mit `on` bestätigen. Während der beobachteten Kommunikationslücke liest die Prüfung nur. Nach Rückkehr werden die Werte vor den Rücksetzbefehlen gespeichert und anschließend über mindestens drei vollständige Abfragen und mindestens zwei Sekunden beobachtet. Ein bloßer Kommunikationsausfall ist ohne deine Schaltbestätigung kein Nachweis eines Netzzyklus.
5. **Ergebnis und Rücksetzung, etwa 10 Minuten.** Ersten und späteren SON-Wert, SP1-Auswahl, PA24/PA25, Ist-Drehzahl und Alarm vergleichen. `returnVerified=true`, eine unveränderte SON-Beobachtung und `restoreAcknowledged=true` getrennt bewerten. Die Prüfung versucht anschließend SON=0, SP1=2 und PA24-temporär=0. Zum Beenden `cancel`, dann `quit`. Auch beim Programmende versucht die Konsole einen bestätigten Abbruch. Fehlt die Bestätigung, Main Power Switch aus.
6. **Folgeschritt anhand der Daten, Restzeit.** Wird SON nach einer tatsächlichen Unterbrechung konsistent auf 0 zurückgesetzt, bereiten wir danach eine eigene eng begrenzte Laufprüfung vor. Bleibt SON=1 erhalten oder ändern sich Zustände beim Booten, gibt es noch keine Freigabe für einen Drehzahltest. Dann klären wir die zweite Schutzbedingung – Stop-Auswahl bzw. verlässlich gespeicherte Einschaltwerte – anhand der Messdaten. Ein erster Lauf ist für diese Stunde nicht zugesichert.

## Werkzeuge

Ein Port darf nur von einem Programm gleichzeitig geöffnet sein. CoolTerm oder eine andere serielle Konsole vorher schließen.

Nur lesen und protokollieren:

```sh
python3 tools/kfm_capture.py ports
python3 tools/kfm_capture.py capture --port /dev/cu.usbmodemXXXX --expected-mac B8:1F:3F:C8:F5:30 --duration 60
```

Bediente Null-Preset-Prüfung, ausschließlich mit dem passenden besonderen Image:

```sh
python3 tools/kfm_zero_probe_operator.py --port /dev/cu.usbmodemXXXX --expected-mac B8:1F:3F:C8:F5:30 --operator-present
```

Die Konsole akzeptiert nur `probe`, `cancel`, `off`, `on`, `quit`. `off` und `on` protokollieren deine tatsächliche Schalterbetätigung; sie schalten die Mühle nicht selbst. Beliebige Modbus-Schreibbefehle, Drehzahlbefehle und normale Motorstarts sind dort nicht vorgesehen. Aktuelle Statuswerte müssen vor `probe` frisch und passend sein.

## Vorbereitung und Nachweis

Getestet wird mit Python 3.12, PlatformIO 6.2.0 und esptool 5.6.0. Für die seriellen Werkzeuge genügt `tools/requirements-kfm-debug.txt`; für einen eigenen ESP32-Build gibt es `tools/requirements-kfm-build.txt`.

```sh
python3 tools/run_kfm_checks.py --report outputs/kfm-offline-checks.json
pio run -e controller-kfm-commissioning -e controller-kfm-zero-probe -j 2
```

Die Offlineprüfungen öffnen kein Gerät. Sie enthalten die bisherigen TSE-Tests, KFM-Zustands-/Abbruchtests, die Sendebeschränkungen, echte Modbus-/Probe-/Backend-Quellen am simulierten UART sowie Speicherprüfungen mit Sanitizern. Simulationserfolg bestätigt noch keinen realen Motorlauf, keinen belasteten Stop und keine vollständige Wiederanlaufsicherheit.
