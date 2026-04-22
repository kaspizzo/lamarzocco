# TODO - Priorisiertes Backlog

Dieses Dokument uebersetzt die Senior-Review-Findings in ein umsetzbares Backlog
fuer `firmware/esp32/`.

Ziel:
- Jeder Eintrag soll als GitHub-Issue oder klar abgegrenzter PR-Scope taugen.
- `P0` blockiert einen breiteren oeffentlichen Release.
- Grosse Refactor-Tickets duerfen als ein Issue geplant werden, sollten in der
  Umsetzung aber bei Bedarf in mehrere PRs geschnitten werden.

## Legende

- `Prio`
  `P0` = Release-Blocker, `P1` = naechste Iterationen, `P2` = Follow-up
- `Aufwand`
  `S` < 1 Tag, `M` = 1-2 Tage, `L` = 3-5 Tage, `XL` = mehr als 1 Woche
- `Status`
  Initial fuer alle Eintraege: `open`

## Empfohlene Reihenfolge

1. Track A komplett abarbeiten.
2. Aus Track B zuerst Stabilitaet, CI und Repo-Hygiene ziehen.
3. Track C nur parallel starten, wenn keine `P0`- oder Betriebsrisiken offen sind.
4. Track D spaetestens vor einem breiteren Release nachziehen.

---

## Track A - Release-Blocker

### [ ] RB-01 OTA-Partitionslayout und Rollback einziehen
Prio: `P0` | Aufwand: `L` | Status: `open`
Warum: Ohne OTA-Slot gibt es fuer ein netzverbundenes Geraet keinen sauberen Weg,
Sicherheitsfixes auszurollen.
Abhaengigkeiten: Entscheidung zum neuen Flash-Layout.
Abnahme:
- `firmware/esp32/partitions.csv` enthaelt `otadata` und zwei `ota_*` Slots.
- OTA-Update und Rollback sind im Firmware-Pfad verdrahtet und verifiziert.
- Flash-/Release-Doku beschreibt den Migrationspfad fuer bestehende Geraete.
Dateien: `firmware/esp32/partitions.csv`, OTA-Init/-Boot-Code, relevante Doku

### [x] RB-05 Harte Groessenbegrenzung fuer Cloud-HTTP-Antworten
Prio: `P0` | Aufwand: `S` | Status: `done`
Warum: Unbegrenztes `realloc` im Response-Buffer kann Speicher erschopfen oder
Controller instabil machen.
Abnahme:
- `http_buffer_append` bricht oberhalb eines festen Caps sauber ab.
- Fehlerpfad liefert ein nachvollziehbares `ESP_ERR_NO_MEM` oder aequivalent.
- Ein Test oder reproduzierbarer manueller Nachweis deckt Oversize-Responses ab.
Dateien: `firmware/esp32/main/cloud_api.c`, ggf. `firmware/esp32/tests/host/src/test_cloud_api.c`

---

## Track B - Stabilitaet, Tests und Betrieb

### [x] OPS-01 Wi-Fi-Reconnect mit Backoff und AP-Fallback
Prio: `P1` | Aufwand: `M` | Status: `done`
Warum: Sofortiges `esp_wifi_connect()` ohne Backoff fuehrt bei Stoerungen zu
unruhigem Verhalten und schlechter Recoverability.
Abnahme:
- `STA_DISCONNECTED` nutzt exponentielles Backoff mit Zaehler.
- Nach definierter Fehlerschwelle wird der Setup-AP wieder verfuegbar gemacht.
- Das Verhalten ist fuer Boot, WLAN-Verlust und Recovery nachvollziehbar dokumentiert.
Dateien: Wi-Fi Event-Handling in `firmware/esp32/main/wifi_setup.c` und Umfeld

### [x] OPS-02 HTTPD-Stack-Budget explizit absichern
Prio: `P1` | Aufwand: `M` | Status: `done`
Warum: Mehrere grosse Stack-Puffer in Handlern sind auf Embedded-Targets eine
direkte Absturzquelle.
Abnahme:
- Der Setup-Portal-HTTP-Server nutzt ein explizites Stack-Budget im Runtime-Config.
- Puffer > 512 B in HTTPD-Handlern liegen auf dem Heap oder sind sonst reduziert.
- Relevante Portal-Pfade laufen weiterhin stabil.
Dateien: `firmware/esp32/main/setup_portal_routes.c`, `firmware/esp32/sdkconfig.defaults`

### [x] OPS-03 Lock-Ordering dokumentieren oder Entkopplung einziehen
Prio: `P1` | Aufwand: `M` | Status: `done`
Warum: Die aktuelle Lock-Reihenfolge ist konsistent, aber fragil und schlecht
gegen spaetere Aenderungen abgesichert.
Abnahme:
- Lock-Order ist an zentraler Stelle explizit dokumentiert.
- Offensichtliche geschachtelte Pfade sind reduziert oder auf apply-then-notify
  umgestellt.
- Neue oder angepasste Kommentare machen Deadlock-Risiko und Reihenfolge klar.
Dateien: `firmware/esp32/main/wifi_setup_internal.h`, betroffene Call-Sites

### [x] OPS-04 `app_main`-Mainloop auf event-getriebenen Takt umstellen
Prio: `P1` | Aufwand: `L` | Status: `done`
Warum: Polling mit 200 ms Blockzeit verschlechtert Timing-Genauigkeit und UI-
Reaktionsfaehigkeit.
Abnahme:
- Periodische Arbeit laeuft ueber Timer oder explizite Events statt ueber grobes Polling.
- Shot-Timer und UI-Responsivitaet regressieren nicht.
- Architekturkommentar beschreibt den neuen Scheduling-Pfad.
Dateien: `app_main` und zugehoerige Queue-/Timer-Logik

### [x] OPS-05 `s_preset_version` konsequent atomar behandeln
Prio: `P2` | Aufwand: `S` | Status: `done`
Warum: Gemischte atomare und nicht-atomare Zugriffe sind ein unnötiger
Concurrency-Fussfehler.
Abnahme:
- Der Typ ist `_Atomic uint32_t` oder aequivalent.
- Reads und Writes nutzen einheitlich `atomic_*` oder eine klar definierte Abstraktion.
- Build laeuft ohne neue Warnings.
Dateien: betroffene Preset-/State-Dateien

### [x] OPS-06 NVS-Druck durch Logo und Keys aufloesen
Prio: `P1` | Aufwand: `M` | Status: `done`
Warum: 24 KB NVS fuer Logo-Blob, Key-Material und Strings ist eng und macht
Silent-Failures wahrscheinlich.
Abhaengigkeiten: kann mit `RB-01` kombiniert werden.
Abnahme:
- Es gibt eine klare Entscheidung: groessere NVS-Partition statt Asset-Auslagerung.
- Upload- und Persistenzpfade behandeln Grenzfaelle sauber.
- Die gewaehlte Richtung ist in Partitionslayout oder Storage-Code sichtbar umgesetzt.
Dateien: `firmware/esp32/partitions.csv`, `firmware/esp32/main/controller_settings.c`,
`firmware/esp32/main/storage_security.c`

### [x] OPS-07 PSA-Key-Lifecycles in Fehlerpfaden sauber schliessen
Prio: `P2` | Aufwand: `S` | Status: `done`
Warum: Vorzeitige Returns nach temporaerer PSA-Key-Erzeugung koennen Key-Leaks oder
uneinheitliches Cleanup hinterlassen.
Abnahme:
- Alle betroffenen Fehlerpfade laufen ueber einen gemeinsamen Cleanup-Pfad.
- `psa_destroy_key` wird fuer temporaere Keys verlässlich aufgerufen.
- Ein Code-Kommentar oder Test deckt die Intention ab.
Dateien: `firmware/esp32/main/cloud_api.c`, zugehoerige Host-Tests

### [x] OPS-08 Lokalen Host-Test-Workflow vor `git push` aktivieren
Prio: `P1` | Aufwand: `M` | Status: `done`
Warum: Vorhandene Host-Tests ohne Push-Gate werden mittelfristig wertlos.
Abnahme:
- Repo-lokaler `pre-push` Hook fuehrt `./firmware/esp32/dev.sh test` aus.
- `firmware/esp32/dev.sh` kann die Hooks installieren und beschreibt den Ablauf konsistent.
- Doku nennt den lokalen Migrationspfad statt GitHub-Hosting.
Dateien: `.githooks/pre-push`, `firmware/esp32/dev.sh`, relevante Doku

### [x] OPS-09 Repo-Hygiene fuer Build-Artefakte und `sdkconfig`
Prio: `P1` | Aufwand: `M` | Status: `done`
Warum: Eingecheckte Build-Ausgaben und generierte Configs machen Diffs laut und
erhoehen Merge-Risiken.
Abnahme:
- `build/`, `managed_components/`, Renderer-Binaries und aehnliche Artefakte sind ignoriert.
- Generiertes `sdkconfig` ist nicht mehr Teil des normalen Repos.
- `sdkconfig.defaults` bleibt die einzige gepflegte Default-Quelle.
Dateien: `.gitignore`, Repo-Inhalt, `firmware/esp32/sdkconfig.defaults`

### [x] OPS-10 Kleine Repo-Hygiene nachziehen
Prio: `P2` | Aufwand: `S` | Status: `done`
Warum: `.DS_Store`, zu offene Berechtigungen und gemischte CLI-Sprache kosten
unnuetige Wartungszeit.
Abnahme:
- `.DS_Store` ist ignoriert.
- Ueberoffene Permissions im Repo sind normalisiert.
- `firmware/esp32/dev.sh` spricht konsistent Englisch.
Dateien: `.gitignore`, betroffene Assets/Ordner, `firmware/esp32/dev.sh`

### [x] OPS-11 Timestamp-Semantik fuer Request-Signing festziehen
Prio: `P2` | Aufwand: `S` | Status: `done`
Warum: Uptime-basierte Timestamps wirken wie echte Zeit, springen aber nach Reboot
zurueck und erschweren Diagnose.
Abnahme:
- Es gibt eine explizite Entscheidung zwischen Uptime und Epochenzeit.
- Der Code dokumentiert die Entscheidung unmittelbar am Header-Aufbau.
- Falls umgestellt wird, regressiert das Request-Signing nicht.
Dateien: `firmware/esp32/main/cloud_api.c`

### [x] OPS-12 JWT-Oversize-Fallback sichtbar machen
Prio: `P2` | Aufwand: `S` | Status: `done`
Warum: Stille Fallbacks verstecken Upstream-Aenderungen und erschweren echte
Fehleranalyse.
Abnahme:
- Oversize- oder Parse-Fallbacks loggen mindestens ein `ESP_LOGW`.
- Das Log enthaelt die beobachtete Token-Laenge oder einen gleichwertigen Hinweis.
- Normale Token-Pfade bleiben log-seitig sauber.
Dateien: `firmware/esp32/main/cloud_api.c`

---

## Track C - Wartbarkeit und Architektur

### [x] ARC-01 Feldsynchronisation in `machine_link.c` descriptor-basiert machen
Prio: `P1` | Aufwand: `L` | Status: `done`
Warum: Mehrfach kopierte Feldlogik ist die groesste laufende Quelle fuer
"Feld vergessen"-Bugs.
Abnahme:
- Wiederholte Feldschleifen fuer Completion/In-Flight/Reported-Werte laufen ueber
  gemeinsame Deskriptoren.
- Das Hinzufuegen eines neuen Felds benoetigt weniger manuelle Touchpoints.
- Vorhandene Host-Tests fuer Policy und Feldsync bleiben gruen oder werden erweitert.
Dateien: `firmware/esp32/main/machine_link.c`, zugehoerige Host-Tests

### [x] ARC-02 Settings-Persistenz auf Descriptor-Modell umstellen
Prio: `P1` | Aufwand: `L` | Status: `done`
Warum: Dieselben Settings muessen heute an zu vielen Stellen manuell gepflegt werden.
Abnahme:
- Load/Save-Pfade nutzen eine gemeinsame Feldbeschreibung.
- `storage_security.c` und relevante Migrationspfade greifen auf dasselbe Modell zu.
- Ein neues persistentes Feld kann ohne 4-fache manuelle Verdrahtung hinzugefuegt werden.
Dateien: `firmware/esp32/main/controller_settings.c`,
`firmware/esp32/main/storage_security.c`, relevante Migrationen

### [ ] ARC-03 Setup-Portal-HTML aus C-Stringliteralen auslagern
Prio: `P1` | Aufwand: `L` | Status: `open`
Warum: Grosse HTML-Bloecke in C erschweren Review, Iteration und spaetere
Lokalisierung.
Abhaengigkeiten: Storage-Entscheidung aus `RB-01` und/oder `OPS-06`.
Abnahme:
- Portal-HTML liegt als Asset in SPIFFS, LittleFS oder aequivalenter Ablage.
- HTTP-Handler rendern oder serven diese Assets statt langer Inline-Strings.
- Build- oder Packaging-Schritt fuer die Assets ist dokumentiert.
Dateien: `firmware/esp32/main/setup_portal_routes.c`, Asset-Pfade, Build-/Flash-Doku

### [x] ARC-04 DE/EN-Strings in einen zentralen Katalog ziehen
Prio: `P1` | Aufwand: `M` | Status: `done`
Warum: Doppelt ausformulierte UI-Texte streuen Uebersetzungslogik unnoetig ueber
den Code.
Abnahme:
- Wiederkehrende Strings liegen in einer Tabelle oder einem klaren `tr()`-Pfad.
- Bestehende DE/EN-Texte bleiben funktional identisch.
- Neue Strings koennen ohne weitere Inline-Ternaries hinzugefuegt werden.
Dateien: Status-/UI-Formatierung, Portal-/UI-Strings

### [x] ARC-05a `wifi_setup_internal.h` entkoppeln
Prio: `P2` | Aufwand: `S` | Status: `done`
Warum: Der eigentliche Schmerz lag nicht in `s_state` oder `s_link` selbst,
sondern im God-Header `wifi_setup_internal.h`, der Runtime-State, NVS-Keys,
Cloud-Endpoints und Utility-Helfer vermischt hat.
Abnahme:
- Neue kleine Header fuer Security-/Text-Helfer, Cloud-Endpoints und NVS-Keys
  sind eingefuehrt.
- `wifi_setup_internal.h` ist auf Runtime-State, Locks, Lock-Kontrakt und
  wenige echte Runtime-Policies reduziert.
- Es gibt keine neue Accessor-Schicht fuer `s_state`, und das Locking-Verhalten
  bleibt unveraendert.
- Host-Tests und Firmware-Build bleiben gruen.
Dateien: `firmware/esp32/main/wifi_setup_internal.h`,
`firmware/esp32/main/lm_ctrl_secure_utils.h`,
`firmware/esp32/main/lm_ctrl_cloud_endpoints.h`,
`firmware/esp32/main/lm_ctrl_nvs_keys.h`, betroffene Call-Sites

### [x] ARC-06 Regressionsvektor fuer Request-Proof-PRF hinterlegen
Prio: `P2` | Aufwand: `S` | Status: `done`
Warum: Eine replizierte Upstream-Transformation ohne Testvektor ist langfristig
fragil.
Abnahme:
- Es existiert mindestens ein bekannt-gutes Input/Output-Testpaar.
- Der Test lebt in den Host-Tests und dokumentiert den erwarteten Wert.
- Optionaler Kommentar verweist auf die Herkunft oder das beobachtete Schema.
Dateien: `firmware/esp32/main/cloud_api.c`, `firmware/esp32/tests/host/src/test_cloud_api.c`

### [x] ARC-07 Edge-Cases fuer `parse_http_date_epoch_ms` testen
Prio: `P2` | Aufwand: `S` | Status: `done`
Warum: Datum-Parsing driftet gerne still weg und zerstoert dann Signaturen erst
im Feld.
Abnahme:
- Host-Tests decken mehrere gueltige und ungueltige HTTP-Date-Varianten ab.
- Sonderfaelle wie Formatabweichung oder Nicht-GMT sind explizit enthalten.
- Das erwartete Verhalten bei ungueltigen Eingaben ist klar festgelegt.
Dateien: `firmware/esp32/tests/host/src/test_cloud_api.c` oder passender Testpfad

### [x] ARC-08 Minimal unterstuetztes Captive-DNS-Verhalten dokumentieren
Prio: `P2` | Aufwand: `S` | Status: `done`
Warum: Der DNS-Server ist bewusst minimal, aber ohne Kommentar sieht das wie ein
versehentlicher Halbfertig-Zustand aus.
Abnahme:
- Kommentar oder kurze Doku nennt die bewusst unterstuetzten Query-Formen.
- Bekannte Einschraenkungen fuer iOS/Android-Captive-Use-Cases sind benannt.
- Die Implementierung bleibt minimal, aber absichtlich.
Dateien: `dns_server_task` und ggf. Portal-/Setup-Doku

---

## Track D - Doku und Threat Model

### [x] DOC-01 Threat-Model fuer LAN-Portal und Setup-Pfade dokumentieren
Prio: `P2` | Aufwand: `M` | Status: `done`
Warum: `SECURITY.md` beschreibt heute vor allem Meldewege, nicht das angenommene
Sicherheitsmodell des Produkts.
Abnahme:
- `SECURITY.md` oder README beschreibt Vertrauen in lokales Netz, Setup-only-Pfade
  und explizit ausgeschlossene Angreifermodelle.
- Das Dokument benennt, welche Annahmen fuer das Erst-Onboarding gelten.
- Formulierungen passen zum tatsaechlichen Firmware-Verhalten.
Dateien: `SECURITY.md`, `README.md`

### [x] DOC-02 Known Limitations mit realen Risiken synchronisieren
Prio: `P2` | Aufwand: `S` | Status: `done`
Warum: Oeffentliche Doku sollte aktuelle Restrisiken konsistent benennen statt
nur Funktionsgrenzen zu wiederholen.
Abnahme:
- README fuehrt offene harte Einschraenkungen wie fehlenden OTA-Pfad oder
  First-Admin-Risiko auf, solange diese nicht behoben sind.
- Doppelte oder verstreute BBW-Hinweise sind konsolidiert.
- Release-Kommunikation und Code-Realitaet driften nicht auseinander.
Dateien: `README.md`, ggf. weitere Release-/Publishing-Doku

---

## Hinweise fuer die Pflege

- Neue Eintraege nur aufnehmen, wenn sie als eigenstaendiges Ticket formulierbar sind.
- Nach jedem erledigten `P0`-Punkt Release-Notizen und Doku auf Folgerisiken pruefen.
- Wenn ein `L`- oder `XL`-Ticket beim Start zu gross wirkt, zuerst entlang von
  Runtime-Risiko und Testbarkeit weiter schneiden, nicht entlang von Dateien.
