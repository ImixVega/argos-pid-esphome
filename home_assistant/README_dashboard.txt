ARGOS PID - DASHBOARD HOME ASSISTANT
===================================

Plik: argos_pid_dashboard.yaml

Dashboard zakłada standardowe entity_id utworzone przez ESPHome.
Jeżeli ręcznie zmieniałeś entity_id w Home Assistant, podmień je w pliku.

Sekcje:
- Stan
- Zapis konfiguracji
- Kocioł
- Palnik
- Podtrzymanie
- Pompa CO
- CWU
- Cyrkulacja
- Zawór
- Lato/Zima
- Diagnostyka

v0.6 zachowuje nazwy istniejących number/select, więc ich entity_id nie powinny
się zmienić względem v0.5c. Znikają tylko zbędne read-only duplikaty nastaw.

v1.1:
- karta "Stan kotla" zawiera teraz:
  binary_sensor.argos_alarm_aktywny
  sensor.argos_alarm_kod
  sensor.argos_alarm_komunikat
- surowy monitor RAW nie jest potrzebny do normalnej obslugi alarmow.
