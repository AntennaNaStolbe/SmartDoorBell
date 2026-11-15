# SmartDoorBell на ESP32-C6

Умный дверной звонок: в простое адресная лента светится настраиваемым цветом, при касании сенсорной кнопки диоды последовательно загораются контрастным цветом, а из динамика играет мелодия звонка. После отпускания кнопки текущий цикл доигрывается, затем лента возвращается к подсветке простоя.

## Возможности

- Подсветка простоя: включение, яркость и цвет задаются из Home Assistant через Zigbee.
- Настройки подсветки сохраняются в NVS и переживают перезагрузку.
- Сенсорная кнопка TTP223, антидребезг 40 мс.
- Последовательная анимация: 9 диодов загораются по одному каждые 130 мс; цвет контрастен подсветке простоя.
- Удержание кнопки повторяет звонок; после отпускания текущий цикл доигрывается.
- Мелодия дверного звонка через MAX98357A и динамик.
- Анимация и звук работают параллельно; мелодия играет в отдельной задаче FreeRTOS.
- Мелодия хранится в LittleFS и легко заменяется без пересборки прошивки.
- Zigbee-роутер для Zigbee2MQTT: light, кнопка «Звонок», событие касания и OTA.
- Датчик `last_press` с временем последнего нажатия или удалённого звонка по данным Zigbee2MQTT.

## Аппаратура

| Компонент | Назначение |
|---|---|
| ESP32-C6 SuperMini | Основной контроллер |
| Адресная лента WS2812B, 9 светодиодов | Подсветка и анимация |
| TTP223 | Сенсорная кнопка |
| MAX98357A | I2S-усилитель с ЦАП |
| Динамик 4–8 Ом, 3 Вт и больше | Звук |
| Блок питания 5 В, 1–3 А | Питание всей системы |
| Резистор 220–470 Ом, 0.25 Вт | Последовательно в линию данных ленты |

## Схема подключения

### Адресная лента

| Лента | ESP32-C6 | Примечание |
|---|---|---|
| VCC / 5V | 5V | Питание 5 В |
| GND | GND | Общий провод |
| DIN | GPIO6 | Резистор 220–470 Ом последовательно в линию данных |

### Сенсорная кнопка TTP223

| TTP223 | ESP32-C6 | Примечание |
|---|---|---|
| VCC | 3V3 | Питать от 3.3 В, чтобы выход не подал 5 В на GPIO |
| GND | GND | Общий провод |
| OUT | GPIO7 | Активный уровень HIGH |

По умолчанию TTP223 работает в режиме momentary: касание — HIGH, отпущено — LOW. Если на модуле перепаяны перемычки и логика инвертирована, поменяйте `kTouchActiveLevel` в `include/config.h`.

### Аудиомодуль MAX98357A

| MAX98357A | ESP32-C6 | Примечание |
|---|---|---|
| VIN | 5V | Питание усилителя |
| GND | GND | Общий провод |
| BCLK | GPIO1 | Тактирование I2S |
| LRC | GPIO2 | LRC / WS |
| DIN | GPIO0 | Данные I2S |
| SD | не подключать | Оставить в воздухе |
| GAIN | Подключить к GND | Усиление по умолчанию около 9 дБ |

Динамик подключается только к выходным контактам усилителя.

Пин SD оставлен в воздухе. По даташиту состояние SD лучше задавать фиксированным уровнем; если усилитель не запускается или канал нестабилен, подключите SD к 3V3 (левый канал) или GND (правый).


## Корпус и 3D-модель

Устройство собрано в корпусе, напечатанном на 3D-принтере. Ниже — внешний вид готового устройства.

| Корпус | Кнопка |
|---|---|
| ![Готовый корпус](pictures/mainBox.png) | ![Кнопка](pictures/button.png) |

Файлы модели лежат в папке `3D models`:

| Файл | Назначение |
|---|---|
| `SmartDoorBell.f3d` | Исходная модель Fusion 360 |
| `MainBox_white.stl` | Корпус |
| `MainBox2_white.stl` | Корпус |
| `button_white.stl` | Кнопка |
| `button_bell_black.stl` | Вставка "колокольчик" кнопки |
| `button_ring_black.stl` | Кольцо кнопки |

## Как это работает

```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> Fill: касание TTP223 или ring из HA
    Fill --> Hold: все 9 диодов горят, 1170 мс
    Hold --> Off: 130 мс
    Off --> On: 650 мс
    On --> Fill: кнопка удерживается, 2600 мс
    On --> Idle: кнопка отпущена, 2600 мс
```

- В состоянии `Idle` все 9 диодов светятся цветом и яркостью из `light`-сущности.
- По фронту касания запускаются анимация и аудио.
- В состоянии `Fill` диоды загораются по одному от первого к последнему: 9 шагов по 130 мс, всего 1170 мс.
- В состоянии `Hold` все диоды горят цветом анимации 130 мс.
- В состоянии `Off` лента выключена 650 мс.
- В состоянии `On` все диоды горят цветом анимации 650 мс.
- Полный цикл занимает 2600 мс и совпадает по длительности с `doorbell.wav`.
- Если кнопка всё ещё удерживается в конце цикла, анимация и звук запускаются снова.
- После отпускания кнопки текущий цикл не обрывается, а доигрывает до конца; затем лента возвращается в `Idle`.
- Мелодия играет в отдельной задаче FreeRTOS, поэтому не задерживает анимацию.

## Аудио

Файл мелодии: `data/doorbell.wav`.

Требования к файлу:

- контейнер WAV (RIFF/WAVE);
- PCM без сжатия;
- 16 бит на сэмпл;
- моно или стерео;
- частота дискретизации 16 кГц (прошивка читает частоту из заголовка, но 16 кГц — оптимальный вариант);
- длительность 2.6 с, чтобы совпадать с циклом анимации;
- короткий fade и 20–50 мс тишины в конце, чтобы не было щелчка: прошивка больше не добавляет тишину после файла.

Мелодия генерируется скриптом `tools/make_doorbell.py`: классический двухнотный «дин-дон» с затуханием, без сторонних аудиофайлов и лицензионных ограничений.

Генерация:

```bash
python3 tools/make_doorbell.py
```

Скрипту нужен `numpy`. Если его нет:

```bash
python3 -m pip install numpy
```

Проверить звук на macOS:

```bash
afplay data/doorbell.wav
```

Заменить мелодию своей:

1. Положите WAV в `data/doorbell.wav` с параметрами выше.
2. При необходимости конвертируйте, например через `afconvert` или `ffmpeg`.
3. Загрузите файловую систему в плату:

```bash
pio run -t uploadfs
```

Если файл отсутствует или формат неверный, лента продолжит работать, а в Serial Monitor появится сообщение об ошибке.

## Zigbee2MQTT и Home Assistant

Устройство работает как Zigbee-роутер. Производитель — `AntennaNS`, модель — `SmartDoorBell`. Основной endpoint — color dimmable light (On/Off, Level, Color, Identify), также используются кластеры Time и OTA client. On/Off client отправляет физическое нажатие в координатор. Внешний конвертер добавляет кнопку, событие и датчик времени.

### Сущности

| Сущность | Тип | Что делает |
|---|---|---|
| `light` | light | Включение, яркость и цвет подсветки простоя |
| `ring` | button | Запускает звонок через `genIdentify.identify` |
| `last_press` | sensor, `device_class: timestamp` | Время приёма в Z2M последнего физического касания или удалённого звонка |
| `action` | event | Событие `single` только при физическом касании |
| `update` | update | OTA-обновление прошивки через Zigbee2MQTT |

Кнопка в Home Assistant публикует в топик `zigbee2mqtt/<friendly_name>/set/ring` полезную нагрузку `ring`. Эквивалентная команда через общий топик:

```json
{"ring": "ring"}
```

Событие и время приходят в состояние устройства:

```json
{
  "action": "single",
  "last_press": "2026-11-03T18:42:10.000Z"
}
```

`last_press` обновляется и при физическом касании, и при удалённом звонке. `action` появляется только при физическом касании. Повторные срабатывания при удержании не обновляют `last_press`.

`action` становится event-сущностью Home Assistant только при включённом `experimental_event_entities: true` в конфигурации Zigbee2MQTT. Без этого событие всё равно доступно в MQTT и через автоматизации Z2M.

### Установка конвертера

1. В `configuration.yaml` Zigbee2MQTT включите внешние JS-конвертеры и event-сущности:

```yaml
advanced:
  enable_external_js: true

homeassistant:
  experimental_event_entities: true
```

Если блоки `advanced:` или `homeassistant:` уже существуют, добавьте ключи внутрь них. Не создавайте второй такой же блок: Z2M выдаст предупреждение о дублирующемся ключе.

2. Скопируйте `zigbee2mqtt/smart-doorbell.mjs` в папку `external_converters` каталога данных Z2M:

- HAOS add-on: `/addon_configs/45df7312_zigbee2mqtt/external_converters/smart-doorbell.mjs`;
- standalone/Docker: `<data_dir>/external_converters/smart-doorbell.mjs`, где `<data_dir>` содержит `configuration.yaml`.

Папку `external_converters` создайте, если её нет. Не копируйте конвертер в папку `zigbee2mqtt` репозитория: Z2M читает файлы только из своего каталога данных.

3. Перезапустите add-on Zigbee2MQTT и проверьте в логе строку `Loaded external converter 'smart-doorbell.mjs'`. Если строки нет, проверьте путь, расширение `.mjs`, `advanced.enable_external_js` и права на файл.

4. Включите `Permit join` и подайте питание на звонок. Устройство подключится как роутер.

5. После сопряжения нажмите на TTP223. В MQTT появятся `action` и `last_press`; в Home Assistant — `light`, `button`, `event`, `sensor` и `update`.

6. После OTA или смены производителя откройте устройство в Z2M и нажмите `Re-interview`. Проверьте `manufacturerName = AntennaNS`, `vendor = AntennaNS`, версию прошивки `0x00010004` и `update = off`.

### Настройка OTA в Zigbee2MQTT

В `configuration.yaml` добавьте постоянный URL индекса OTA:

```yaml
ota:
  zigbee_ota_override_index_location: https://github.com/AntennaNaStolbe/SmartDoorBell/releases/latest/download/index.json
```

Если блок `ota:` уже есть, добавьте ключ внутрь него. URL должен быть публично доступен из контейнера Z2M. Если вы используете форк, замените `AntennaNaStolbe` на свой GitHub-аккаунт и передайте свой `--base-url` при сборке OTA.

После изменения конфигурации перезапустите Z2M. Затем откройте устройство → OTA → `Check for updates`. В Home Assistant должна появиться сущность `update`.

Для появления OTA должны выполняться условия:

- во внешнем конвертере указано `ota: true`;
- `fileVersion` в `index.json` больше установленной версии;
- `manufacturerCode = 0x1A1A`, `imageType = 0x0001` и hardware version `0x0001` совпадают с прошивкой;
- релиз опубликован, а не сохранён как draft/prerelease;
- в релизе есть и `index.json`, и `AntennaNS-SmartDoorBell.zigbee`.

### Чек-лист нового пользователя

- [ ] MQTT и координатор настроены.
- [ ] В `configuration.yaml` включён `advanced.enable_external_js`.
- [ ] `smart-doorbell.mjs` лежит в `external_converters`.
- [ ] В логе есть `Loaded external converter 'smart-doorbell.mjs'`.
- [ ] Включён `homeassistant.experimental_event_entities`.
- [ ] Указан `ota.zigbee_ota_override_index_location`.
- [ ] Z2M перезапущен.
- [ ] Устройство сопряжено и при необходимости переинтервьюировано.
- [ ] Появились `light`, `button`, `event`, `sensor.last_press` и `update`.
- [ ] OTA находит версию `0x00010004`.

### Время

`last_press` формируется в Zigbee2MQTT по времени приёма `commandOn` (физическое нажатие) или `ring` (удалённый звонок). Синхронизация времени устройства на него не влияет.

Прошивка по-прежнему запрашивает время у координатора через кластер `genTime` при старте и повторно раз в 6 часов. Это отдельный механизм для времени устройства; он не блокирует звонок и анимацию. В прошивке учтён баг библиотеки Zigbee 3.3.8: `getTime()` возвращает секунды эпохи Zigbee (с 2000 года) как Unix-время, поэтому к результату добавляется поправка 946684800 секунд.

### OTA-обновление

В прошивке реализован Zigbee OTA client: Z2M скачивает образ на устройство, устройство записывает его во второй app-раздел, переключает загрузку и перезагружается. Настройки света в NVS сохраняются.

OTA-образы публикуются в GitHub Releases репозитория `AntennaNaStolbe/SmartDoorBell`. Первый раз прошивку с OTA-клиентом нужно залить по USB, дальше обновления идут по Zigbee.

Обновление обрабатывает встроенный OTA-клиент библиотеки Zigbee: стек сам отбрасывает заголовок OTA-файла и передаёт прошивку в `esp_ota_*`. В Z2M обновление отображается на вкладке OTA устройства и в сущности `update` Home Assistant, если во внешнем конвертере указано `ota: true`, настроен `zigbee_ota_override_index_location`, а `fileVersion` в индексе больше установленной версии.

#### Первичная прошивка

```bash
pio run
pio run -t upload
pio run -t uploadfs
```

После первичной прошивки не прошивайте OTA-версию повторно по USB: версия на устройстве станет равна версии в индексе, и Z2M не предложит обновление.

#### Выпуск обновления

1. Увеличьте `kFirmwareVersion` в `include/config.h`, например с `0x00010003` до `0x00010004`.
2. Соберите прошивку: `pio run`.
3. Сгенерируйте OTA-файл и индекс с абсолютным URL GitHub:

```bash
python3 tools/make_ota.py \
  --base-url https://github.com/AntennaNaStolbe/SmartDoorBell/releases/latest/download
```

`make_ota.py` читает `kFirmwareVersion`, `kOtaManufacturer`, `kOtaImageType` и `kOtaHardwareVersion` из `include/config.h`. По умолчанию создаётся файл `zigbee2mqtt/ota/AntennaNS-SmartDoorBell.zigbee`.

4. Создайте тег и релиз. Сначала загрузите образ, затем индекс:

```bash
git tag v1.0.4
git push origin v1.0.4

gh release create v1.0.4 \
  zigbee2mqtt/ota/AntennaNS-SmartDoorBell.zigbee \
  --title "v1.0.4" \
  --notes "Zigbee OTA firmware 0x00010004, manufacturer AntennaNS"

gh release upload v1.0.4 zigbee2mqtt/ota/index.json
```

5. Если OTA-индекс ещё не настроен, добавьте в `configuration.yaml` Zigbee2MQTT:

```yaml
ota:
  zigbee_ota_override_index_location: https://github.com/AntennaNaStolbe/SmartDoorBell/releases/latest/download/index.json
```

6. Перезапустите Z2M, откройте устройство → OTA → `Check for updates`. В Home Assistant у устройства появится сущность `update`.

7. После установки OTA откройте устройство → `Re-interview`, чтобы Z2M обновил `manufacturerName` на `AntennaNS`.

Важно: `fileVersion` должен быть больше версии, сейчас запущенной на устройстве. `manufacturerCode` (`0x1A1A`), `imageType` (`0x0001`) и hardware version (`0x0001`) должны совпадать с прошивкой. Релиз должен быть опубликован, а не сохранён как draft/prerelease, иначе `latest/download/index.json` может указывать на старую версию.

Файл OTA собирается в формате ZCL: 56-байтный обязательный заголовок + 4 байта hardware versions, 6-байтный заголовок элемента и сама прошивка. `index.json` содержит SHA-512 и размер файла. Локальный вариант с `zigbee_ota_override_index_location: ota/index.json` остаётся доступен, если файлы из `zigbee2mqtt/ota` скопированы в каталог данных Z2M.

#### Быстрая OTA

По умолчанию Z2M передаёт OTA блоками по 50 байт с паузой 250 мс. Для образа ~887 КБ это ~74 минуты только на передачу; на практике обновление занимает около 77 минут.

Для одной OTA можно передать параметры прямо в запросе, не меняя глобальный конфиг Z2M. Скрипт `tools/fast_ota.py` использует 64 байта и 50 мс — примерно 12 минут теоретически и 15–20 минут с учётом накладных расходов:

```bash
export HA_URL="https://ha.example.com"
export HA_TOKEN="..."

python3 tools/fast_ota.py --version v1.0.5 --yes
```

Проверить запрос без запуска:

```bash
python3 tools/fast_ota.py --version v1.0.5 --dry-run
```

Если URL образа нестандартный, передайте его через `--firmware-url`.

Эквивалентный MQTT-запрос:

```json
{
  "id": "0x9888e0fffe71bca8",
  "url": "https://github.com/AntennaNaStolbe/SmartDoorBell/releases/download/v1.0.5/AntennaNS-SmartDoorBell.zigbee",
  "default_maximum_data_size": 64,
  "image_block_response_delay": 50
}
```

Топик: `zigbee2mqtt/bridge/request/device/ota_update/update`.

Не используйте для быстрой OTA кнопку `update.install` в Home Assistant: она отправляет только `id` и не передаёт параметры чанков. Запускайте обновление скриптом или напрямую через MQTT.

`64/50` — компромисс между скоростью и стабильностью. `100/50` быстрее (~7–8 минут теоретически), но выше риск повторов и сбоев. Если OTA проходит нестабильно, повторите с `50/100` или верните дефолтные `50/250`. Не прерывайте питание во время прошивки и обновляйте только одно устройство за раз.

Если нужно ускорить все будущие OTA, можно задать глобально в `configuration.yaml`:

```yaml
ota:
  default_maximum_data_size: 64
  image_block_response_delay: 50
```

После изменения глобальных настроек перезапустите Z2M. Для разового обновления предпочтительнее параметры в запросе: они не влияют на другие устройства.

### Сброс Zigbee

Удерживайте кнопку BOOT (GPIO9) на плате 5 секунд во время работы устройства. Прошивка выполнит сброс Zigbee-настроек и перезагрузится. Не удерживайте BOOT при включении питания: ESP32-C6 войдёт в режим загрузчика.

### Примечания по сборке

Для роутера используется раздел `zigbee_zczr.csv`, но в проекте лежит совместимый `partitions_zigbee_zczr.csv`. Он нужен из-за особенности PlatformIO: стандартный файл содержит дополнительные FAT-разделы Zigbee, и сборщик LittleFS ошибочно выбирает `zb_fct` вместо `spiffs`. В пользовательской таблице разделы Zigbee помечены числовыми subtype `0x81`/`0x82` (те же `fat`/`spiffs`), поэтому `spiffs` остаётся единственным разделом, который видит сборщик файловой системы. Смещения и размеры разделов совпадают со стандартной таблицей.

## Сборка и прошивка

Проект собран на PlatformIO. Плата определяется как `esp32-c6-devkitc-1`, потому что это совместимый board definition для ESP32-C6 SuperMini.

```bash
pio run
pio run -t upload
pio run -t uploadfs
pio device monitor -b 115200
```

После сборки OTA-образ и индекс можно обновить командой:

```bash
python3 tools/make_ota.py
```

Порядок важен: сначала прошивка, затем файловая система. `uploadfs` не стирает прошивку, но перезаписывает раздел LittleFS.

Если порт не появляется:

1. Используйте USB-C кабель с линиями данных, а не charge-only.
2. Зажмите BOOT, нажмите RESET, отпустите RESET, затем отпустите BOOT.
3. Проверьте другой USB-порт.

## Настройки

Все основные параметры находятся в `include/config.h`:

| Параметр | Значение по умолчанию | Описание |
|---|---|---|
| `kLedPin` | 6 | Пин данных ленты |
| `kLedCount` | 9 | Количество светодиодов |
| `kMaxBrightness` | 255 | Яркость анимации звонка |
| `kDefaultIdleLevel` | 102 | Яркость простоя по умолчанию (шкала Zigbee) |
| `kDefaultIdleRed/Green/Blue` | 255 | Цвет простоя по умолчанию (белый) |
| `kTouchPin` | 7 | Вход TTP223 |
| `kTouchActiveLevel` | HIGH | Активный уровень касания |
| `kTouchDebounceMs` | 40 | Антидребезг |
| `kFactoryResetPin` | 9 | Кнопка BOOT для сброса Zigbee |
| `kFactoryResetHoldMs` | 5000 | Удержание для сброса |
| `kZigbeeEndpoint` | 1 | Endpoint Zigbee |
| `kTimeSyncRetryMs` | 10000 | Повтор синхронизации времени при ошибке |
| `kTimeResyncMs` | 6 часов | Период повторной синхронизации |
| `kFillStepMs` | 130 | Шаг заполнения: один LED |
| `kFillMs` | 1170 | Длительность заполнения |
| `kHoldMs` | 130 | Удержание цвета анимации |
| `kOffMs` | 650 | Пауза с выключенной лентой |
| `kOnMs` | 650 | Финальная вспышка |
| `kCycleMs` | 2600 | Полный цикл анимации и звука |
| `kAudioRestartTimeoutMs` | 300 | Ожидание остановки аудио перед повтором |
| `kSettingsSaveDelayMs` | 500 | Задержка сохранения настроек в NVS |
| `kSettingsNamespace` | `doorbell` | Namespace NVS |
| `kManufacturerName` | `AntennaNS` | Имя производителя в Zigbee |
| `kModelName` | `SmartDoorBell` | Имя модели в Zigbee |
| `kFirmwareVersion` | `0x00010004` | Версия прошивки для OTA |
| `kOtaManufacturer` | `0x1A1A` | Manufacturer code OTA |
| `kOtaImageType` | `0x0001` | Image type OTA |
| `kOtaHardwareVersion` | `0x0001` | Hardware version OTA |
| `kI2sBclkPin` | 1 | I2S BCLK |
| `kI2sLrcPin` | 2 | I2S LRC |
| `kI2sDinPin` | 0 | I2S DIN |
| `kDoorbellPath` | `/doorbell.wav` | Путь к мелодии в LittleFS |

Настройки света меняются из HA и сохраняются автоматически. Сброс Zigbee кнопкой BOOT также очищает настройки света и возвращает белый цвет с яркостью 40%.

## Питание и безопасность

- Используйте общий GND для ESP32, ленты, TTP223 и усилителя.
- Для 9 светодиодов и усилителя рекомендуется блок питания 5 В на 1-2 А.
- Не подавайте 5 В на пин `5V` одновременно с подключённым USB.
- Если лента мерцает, проверьте общий GND, питание и резистор в линии данных.

## Диагностика

| Проблема | Что проверить |
|---|---|
| Плата не определяется | Кабель data, BOOT+RESET, другой USB-порт, драйверы не нужны |
| Нет звука | Питание MAX98357A, файл `doorbell.wav` загружен через `uploadfs`, динамик подключён к выходам усилителя; при необходимости задайте уровень SD |
| Лента не горит | Питание 5 В, общий GND, GPIO6, направление DIN |
| Лента мерцает | Общий GND, резистор 330–470 Ом, короткие провода |
| Ложные касания | Питание TTP223 3.3 В, общий GND, калибровка после включения, расстояние до металла |
| Плата перезагружается при звуке | Слишком слабый блок питания, используйте 5 В 3 А |
| Не появляется в Zigbee2MQTT | `Permit join`, установленный конвертер, `enable_external_js: true`, сброс Zigbee кнопкой BOOT |
| Конвертер не загрузился | Путь `external_converters/smart-doorbell.mjs`, расширение `.mjs`, `advanced.enable_external_js: true`, перезапуск add-on Z2M, строка `Loaded external converter` в логе |
| Нет `last_press` | Проверьте загрузку конвертера и события `commandOn`/`ring` в MQTT; синхронизация времени Zigbee для `last_press` не нужна |
| Кнопка `ring` не работает | Конвертер загружен, модель `SmartDoorBell`, MQTT-топик `/set`, статус сопряжения |
| Нет сущности `action` | Включите `homeassistant.experimental_event_entities: true` и перезапустите Z2M |
| Нет сущности `update` | Проверьте `ota.zigbee_ota_override_index_location`, публичную доступность `index.json`, `ota: true` в конвертере, совпадение manufacturer/image type/hardware version и версию выше текущей |
| OTA 404 или не видит файл | Проверьте имя `AntennaNS-SmartDoorBell.zigbee` в `index.json` и в релизе; релиз должен быть опубликован и содержать оба файла |
| OTA не начинается | Увеличьте `kFirmwareVersion`, пересоберите образ и `index.json`; проверьте лог `Zigbee - OTA` в Serial Monitor |
| OTA идёт около часа | Это дефолтные 50 B / 250 мс; используйте `tools/fast_ota.py` с 64/50 |
| OTA обрывается | Проверьте питание, размер образа не больше `0x140000`, стабильность Zigbee-сети |
| После OTA остался `DIY` | Сделайте `Re-interview` устройства в Z2M, чтобы обновить `manufacturerName` на `AntennaNS` |
| `buildfs` падает с `LFS_ERR_NOSPC` | Используется `partitions_zigbee_zczr.csv`, а не стандартный `zigbee_zczr.csv` |

## Структура проекта

```text
SmartDoorBell/
├── include/
│   ├── config.h
│   ├── audio_player.h
│   ├── doorbell_zigbee.h
│   ├── light_settings.h
│   └── ...
├── src/
│   ├── main.cpp
│   ├── audio_player.cpp
│   └── doorbell_zigbee.cpp
├── data/
│   └── doorbell.wav
├── 3D models/
│   ├── SmartDoorBell.f3d
│   ├── MainBox_white.stl
│   ├── MainBox2_white.stl
│   ├── button_white.stl
│   ├── button_bell_black.stl
│   └── button_ring_black.stl
├── pictures/
│   ├── mainBox.png
│   └── button.png
├── tools/
│   ├── fast_ota.py
│   ├── make_doorbell.py
│   └── make_ota.py
├── zigbee2mqtt/
│   ├── smart-doorbell.mjs
│   └── ota/
│       ├── AntennaNS-SmartDoorBell.zigbee
│       └── index.json
├── partitions_zigbee_zczr.csv
├── platformio.ini
├── README.md
└── .gitignore
```

## Примечания

- Мелодия синтезируется локально, сторонние аудиофайлы не используются.
- Прошивка использует Adafruit NeoPixel, встроенные ESP_I2S и Zigbee.
- Анимация и звук синхронизированы по циклу 2600 мс; мелодия играет в отдельной задаче FreeRTOS.
- Zigbee-стек работает как роутер; для прошивки нужен раздел `zigbee_zczr` и флаги `ZIGBEE_MODE_ZCZR`.
- Внешний конвертер Z2M использует стандартные кластеры, без кастомных cluster-команд.
- Zigbee-производитель — `AntennaNS`; после смены производителя требуется `Re-interview` в Z2M.
