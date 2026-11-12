# Argos PID Plus / ESPHome bridge

Lokalna integracja sterownika **Argos PID Plus / Pellet** z **Home Assistant** przez **ESPHome** i **WT32-ETH01**.

Projekt przechwytuje komunikację Ethernet sterownika, emuluje lokalnie endpoint używany przez Argosa i wystawia telemetrię, konfigurację oraz alarmy bez konieczności korzystania z chmury e-kotlownia.

> Projekt nieoficjalny, oparty na reverse engineeringu własnego urządzenia i rzeczywistych testach.

## Wersja

**v1.2**

Najważniejsze elementy obecnej wersji:

- lokalna praca Argos ↔ WT32-ETH01 ↔ Home Assistant,
- odczyt bieżących parametrów kotła,
- edycja nastaw z buforem zmian i ręcznym przyciskiem **Zapisz**,
- zapis konfiguracji w formacie FULL CONFIG zgodnym z zachowaniem chmury,
- wymagane lowercase HEX w odpowiedzi konfiguracyjnej,
- opcjonalny proxy do e-kotlowni do diagnostyki i dalszego reverse engineeringu,
- obsługa alarmów z pola **AB**,
- odczyt stanów trzech pomp (CO, CWU, cyrkulacyjnej),
- zezwolenie na pracę wentylatora, wysterowanie zadane i wynikowe,
- pełne logi 103 pól POST dzielone na krótkie fragmenty, z maskowaniem ID,
- dashboard Home Assistant,
- dodatkowy czujnik poziomu opału JSN-SR04T.

## Topologia

```text
Argos PID Plus
    |
    | Ethernet 10.10.15.x
    |
WT32-ETH01
    |
    | Wi-Fi
    |
Home Assistant
```

W przykładowej konfiguracji:

- ESP Wi-Fi: `10.10.14.103`
- ESP Ethernet: `10.10.15.1`
- Argos: `10.10.15.11`

Adresy należy dopasować do własnej sieci. Są to przykładowe prywatne adresy z instalacji testowej; repozytorium nie zawiera hasła Wi-Fi ani `secrets.yaml`.

## Struktura

```text
components/argos_pid/
  __init__.py
  argos_pid.cpp
  argos_pid.h

home_assistant/
  argos_pid_dashboard.yaml
  README_dashboard.txt

esp32-argospid.yaml
MAPA_PROTOKOLU_v1.2.txt
CHANGELOG_v1.2.txt
VERSION.txt
NAZEWNICTWO_v06a.txt
```

## Instalacja ESPHome

1. Skopiuj katalog `components/argos_pid` do katalogu konfiguracji ESPHome.
2. Skopiuj `esp32-argospid.yaml`.
3. Skopiuj `secrets.example.yaml` jako `secrets.yaml` i uzupełnij dane Wi-Fi:

```yaml
wifi_ssid: "..."
wifi_password: "..."
```

4. Sprawdź adresację IP w YAML.
5. Skompiluj i wgraj firmware na WT32-ETH01.
6. Po pierwszym poprawnym POST fizycznego sterownika Argos komponent wczyta stan live.

`secrets.yaml` jest ignorowany przez repozytorium i nie powinien być publikowany.

## Zapis nastaw

Zmiana encji `number` lub `select` w Home Assistant **nie jest od razu wysyłana do sterownika**.

Zmiany trafiają do bufora RAM. Można zmienić kilka parametrów, a następnie użyć:

- **Argos Zapisz nastawy**
- **Argos Anuluj zmiany**

Przy zapisie komponent buduje pełną odpowiedź konfiguracyjną i oczekuje potwierdzenia w kolejnym POST sterownika.

### Ważne: lowercase HEX

Argos wymaga, aby cyfry szesnastkowe `a..f` w odpowiedzi konfiguracyjnej były zapisane **małymi literami**.

Przykładowo uppercase potrafi zostać błędnie zinterpretowany:

```text
D  -> 0
2C -> 2
E  -> 0
2F -> 2
```

v1.2 normalizuje wysyłany payload do lowercase.

## Alarmy

Pole regularnego POST:

```text
AB = numer błędu
```

Kod jest przesyłany jako ASCII HEX.

Przykłady:

| RAW AB | Kod | Znaczenie |
|---|---:|---|
| `0` | 0 | OK |
| `8` | 8 | Piec przegrzany |
| `A` | 10 | STB – zadziałało zabezpieczenie |
| `B` | 11 | Kocioł wygaszony |
| `C` | 12 | Kończy się opał w zasobniku |
| `E` | 14 | Otwarta klapa zasobnika |

Pole `AB` zostało potwierdzone fizycznym testem otwarcia klapy zasobnika:

```text
RAW CHANGE: AB 0 -> E
```

W Home Assistant dostępne są:

- **Argos Alarm aktywny**
- **Argos Alarm kod**
- **Argos Alarm komunikat**

Pełna aktualnie zaimplementowana tabela Er0..Er17 znajduje się w pliku [MAPA_PROTOKOLU_v1.2.txt](MAPA_PROTOKOLU_v1.2.txt).

## Pompy i wentylator (v1.2)

Nowe stany zostały potwierdzone kontrolowanymi przełączeniami na fizycznym sterowniku:

| RAW POST | Funkcja | `0` | `1` |
|---|---|---|---|
| `AC` | Pompa CO | ON | OFF |
| `AD` | Pompa CWU | ON | OFF |
| `AE` | Pompa cyrkulacyjna | ON | OFF |
| `C9` | Zezwolenie wentylatora | OFF | ON |

Pole `AF` przechowuje procentowe **wysterowanie zadane** wentylatora także wtedy, gdy regulator wyłączył jego wyjście.
Dlatego nowa encja **Argos Wentylator wysterowanie wynikowe** pokazuje `AF` tylko przy `C9=1`, a w przeciwnym przypadku `0%`.
Nie jest to niezależny pomiar obrotów wentylatora ani potwierdzenie elektryczne pracy silnika.

Stany odświeżają się wraz z kolejnymi ramkami POST (~27 s). Przy braku komunikacji ostatnia wartość może być nieaktualna; sprawdzaj również **Argos Stan online**.

**Podajnik:** w kontrolowanych testach nie udało się znaleźć jego stanu w 103 polach regularnego POST. Nie publikujemy niepotwierdzonej encji.

## Diagnostyka — logi RAW na żądanie

W aktualnym YAML jest przełącznik **Argos ESP logi RAW** (w Home Assistant w kategorii diagnostyki).

- **OFF** (domyślnie, również po restarcie) — standardowe logi DEBUG.
- **ON** — komunikaty VERY_VERBOSE tylko z tagu `argos_pid`, w tym całe 103 pola regularnego POST, dzielone na krótkie, numerowane komunikaty `RAW POST`.
- Po wyłączeniu wraca DEBUG bez restartu i bez ponownego wgrywania firmware.

`logger.level: VERY_VERBOSE` określa, które komunikaty są **kompilowane** do firmware; `logger.initial_level: INFO` oraz osobny poziom tagu `argos_pid` ograniczają rzeczywiście wypisywane logi przy codziennym działaniu. Włączenie RAW służy do krótkich sesji diagnostycznych i może spowalniać sterownik.

**Uwaga o prywatności:** pole `A0` jest maskowane w komunikatach `RAW POST`; przed publikacją logu nadal sprawdź pozostałe dane.

## Proxy chmury

Switch **Argos Chmura Proxy** służy wyłącznie do diagnostyki i dalszego reverse engineeringu.

- po restarcie jest OFF,
- OFF = lokalna praca,
- ON = transparentny proxy do e-kotlowni,
- podczas proxy lokalny zapis konfiguracji jest blokowany.

Do normalnej pracy projektu chmura nie jest potrzebna.

## Home Assistant

Przykładowy dashboard znajduje się w:

`home_assistant/argos_pid_dashboard.yaml`

Encje nastaw są oznaczone jako konfiguracja, aby nie zaśmiecały głównego panelu urządzenia.

## Sprzęt użyty podczas testów

- WT32-ETH01 v1.4
- Argos PID Plus / Pellet
- JSN-SR04T
  - TRIG: GPIO14
  - ECHO: GPIO39 przez dzielnik 10k/20k
- 1-Wire: GPIO33 + rezystor 4.7k do 3.3 V

## Dokumentacja protokołu

Aktualna mapa rozpoznanych pól:

[MAPA_PROTOKOLU_v1.2.txt](MAPA_PROTOKOLU_v1.2.txt)

Projekt nadal zawiera monitor zmian nieznanych pól RAW, dzięki czemu można dalej rozszerzać mapę protokołu.

## Ograniczenia

- harmonogramy J* nie są jeszcze edytowane lokalnie,
- część pól protokołu nadal nie ma potwierdzonego znaczenia,
- wersja z odczytem pomp i wentylatora została wgrana i potwierdzona jako działająca na urządzeniu testowym,
- konfiguracja była testowana na konkretnym sterowniku i wersji firmware, więc inne rewizje mogą zachowywać się inaczej.

## Changelog

Zobacz [CHANGELOG_v1.2.txt](CHANGELOG_v1.2.txt).

## Licencja

Ten projekt jest udostępniany na licencji **GNU General Public License v3.0 (GPL-3.0-only)**.

Możesz używać, modyfikować i rozpowszechniać kod zgodnie z warunkami GPLv3. Jeżeli rozpowszechniasz zmodyfikowaną wersję projektu, jej kod źródłowy również musi pozostać dostępny na warunkach GPLv3.

Pełny tekst licencji znajduje się w pliku [LICENSE](LICENSE).
