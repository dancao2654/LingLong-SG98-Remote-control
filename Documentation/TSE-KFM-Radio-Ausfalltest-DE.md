# Gezielter Ausfall des Controller-Funks, Firmware .8 bleibt unverändert

Die realen Touchscreen-Start-/Stop- und automatischen Zeitstop-Tests bei100U/min sind bestätigt. Zwei Versuche mit manuellem Ausschalten des Touchscreens waren für den Stop unter Funkverlust nicht eindeutig: Runde1 endete am Zeitlimit; in Runde2 wurde die Freigabe nach Funkverlust schon vor einem neuen Start zurückgenommen. Der alte Pulsdatensatz wurde ausdrücklich nicht als neuer Lauf gewertet. Kein Servo-Alarm wurde dabei gemeldet.

## Anderer Prüfaufbau ohne neue Firmware

Das reine Mac-Werkzeug `tools/kfm_radio_off_test.py` vermeidet den engen Handablauf. Es prüft die exakte .8-Identität, gibt einen einzigen Test frei und wartet auf den echten Start am Touchscreen. Erst wenn ein NEUER Puls mit aktivem Lauf, gültiger Drehzahl zwischen−50 und−150, passender Auswahl/PA25/SON und ohne Alarm gemeldet wird, sendet es `dbg espnow off` und protokolliert den tatsächlichen Funkstatus. Es sendet selbst keinen Motor-Start, keinen Drehzahlwert und keinen Rohschreibbefehl.

Danach muss vor jedem Host-Disarm ein neuer passender Laufdatensatz mit `remote_link_lost`, abgeschalteter Freigabe, PA25zero, Auswahl2, Drehzahl0, gültigen Stop-Antworten UND Rücklesung und normalem IDLE/Fehler0 vorliegen. Ein alter Puls, ein normaler Zeitstop oder ein vom Host angeforderter Abbruch zählt nicht als Funkverlust-Erfolg.

Erst nach dieser Nullzustandsbestätigung schaltet das Werkzeug ESP-NOW wieder ein. Es verlangt danach mindestens drei passende Beobachtungen über mindestens zwei Sekunden: Funk zurück, kein Arm, kein neuer Lauf, Drehzahl0. Ein ausbleibender Stop oder eine fehlende Rücklesung ist ein Abbruch und verlangt Main Power Switch aus. Die Rücksetzung wird versucht; Funk wird nur nach einem passenden Nullzustand wiederhergestellt. Kein automatischer zweiter Start.

**Prüfumfang:** Dies ist ein gezielter Ausfall der Controller-Funkschnittstelle, keine direkte Vermessung der Drei-Sekunden-Frist bei bloß ausbleibenden Paketen und weiterhin aktivem Empfänger. Controller-CPU, RS485 und Stromversorgung bleiben bei diesem Test verfügbar. Daraus folgt keine nachgewiesene Abschaltung bei Controller-Stromverlust, unterbrochener RS485-Leitung oder beliebigen Netzunterbrechungen.

## Bedienung

```sh
python3 tools/kfm_radio_off_test.py --port /dev/cu.usbmodemXXXX --expected-mac B8:1F:3F:C8:F5:30 --operator-present
```

Nur `arm`, `disarm`, `quit`. Eine Freigabe pro Konsolensitzung. Der Bediener bleibt mit leerer geschlossener Mühle am erreichbaren Hauptschalter. Der Touchscreen bleibt dieses Mal AN. Auf Ansage Start am Touchscreen drücken, sonst nichts. Das Programm schaltet den Controller-Funk nach der gemessenen Bewegung selbst ab und stellt ihn nach dem Stop wieder her. Bei weiterlaufendem Motor oder unerwartetem Verhalten sofort Main Power Switch aus.

Offlinetests prüfen alte/neue Lauf-IDs, echte gemeldete Bewegung, Zeitstop-Abgrenzung, Stop-/Null-Rücklesung, sichere Link-Rückkehr und den Ausschluss von Host-Start-/Drehzahlbefehlen. Das Werkzeug selbst ist noch nicht an einem echten Lauf eingesetzt worden; die bekannten Firmware-Sperren und100-U/min/5-Sekunden-Prüfgrenzen bleiben unverändert und ersetzen keine unabhängige Abschaltung.
