# BMS Hub & Controller

Система моніторингу та керування акумуляторними батареями з BLE BMS (Battery Management System) через ESP32.

## Компоненти

### 🔴 Hub (ESP32 + Display)
- Збирає дані з контролерів через ESP-NOW
- Відображає агреговану інформацію
- WiFi AP для налаштування
- REST API для доступу до даних

### 🟢 Controller (ESP32 + Display + RS485)
- Підключається до BMS по BLE
- Відправляє дані на Hub через ESP-NOW
- Має власний WiFi для прямого доступу
- Керування MOSFET через веб-інтерфейс

## Функціональність

### Hub
- ✅ Прийом даних від Controller через ESP-NOW
- ✅ Агрегація даних з декількох контролерів
- ✅ REST API (`/api/data`)
- ✅ Веб-інтерфейс для налаштування
- ✅ WiFi конфігурація через AP
- ✅ Відображення на OLED дисплеї:
  - IP адреса, SSID, канал WiFi
  - Кількість контролерів
  - Загальна напруга, струм, SOC

### Controller
- ✅ Підключення до BLE BMS (JK BMS та інші)
- ✅ Відправка даних на Hub через ESP-NOW
- ✅ Пряме WiFi підключення для моніторингу
- ✅ REST API:
  - `GET /data` - дані BMS
  - `GET /control?cmd=charge_on/off` - керування MOSFET
  - `GET /api/wifi/scan` - сканування WiFi
- ✅ Веб-інтерфейс для налаштування
- ✅ Відображення на OLED дисплеї:
  - IP адреса, SSID, канал WiFi
  - Напруга, струм, SOC батареї
  - Напруга кожної комірки (до 16)
  - Температури

## WiFi та ESP-NOW

### Конфігурація WiFi
1. Hub створює AP `BMS-Hub-XXXX` (пароль: `bms12345`)
2. Controller може підключитись до існуючої WiFi мережі
3. Важливо: всі пристрої мають бути на одному WiFi каналі для ESP-NOW

### ESP-NOW
- Використовується для зв'язку Hub ↔ Controller
- Не потребує підключення до роутера
- Робота на каналі WiFi (1-13)
- Hub може бути в режимі broadcast (FF:FF:FF:FF:FF:FF)

## API Endpoints

### Hub
```
GET /api/data              # Агреговані дані
GET /api/config           # Конфігурація
POST /api/hub/discover    # Пошук контролерів
```

### Controller
```
GET /                     # Веб-інтерфейс
GET /data                 # Дані BMS (JSON)
GET /control?cmd=X        # Керування MOSFET
  cmds: charge_on, charge_off, discharge_on, discharge_off
GET /scan                 # BLE сканування
GET /connect?id=X         # Підключення до BMS
GET /disconnect           # Відключення від BMS
GET /api/wifi/scan        # WiFi сканування
GET /api/wifi?ssid=X&pass=Y # Підключення до WiFi
GET /hubconfig?mac=XX..   # Налаштування Hub MAC
```

## Збірка

### Вимоги
- PlatformIO Core або VS Code з PlatformIO extension
- Python 3.x

### Команди
```bash
# Збірка Hub
pio run -e hub

# Збірка Controller
pio run -e controller

# Завантаження Hub
pio run -e hub --target upload --upload-port COM3

# Завантаження Controller
pio run -e controller --target upload --upload-port COM4
```


## Дисплей

Обидва пристрої використовують GMT020-02-7P TFT 320x240 (ST7789).

Інформація на екрані:
- Рамка з назвою пристрою
- WiFi: SSID, IP, канал
- Дані BMS (для Controller)
- Агреговані дані (для Hub)

## Android App

Дивись `android-app/README.md` для інструкцій по встановленню PWA додатку для моніторингу.

## Типові проблеми

### Hub не бачить Controller
- Перевір що обидва на одному WiFi каналі
- На дисплеях має показувати однаковий канал
- Перезавантаж обидва пристрої
- Перевір MAC адресу Hub в налаштуваннях Controller

### Не підключається до WiFi
- Перевір SSID і пароль
- Максимальна довжина пароля 63 символи
- Спробуй спочатку через веб-інтерфейс Controller

### CORS помилки в додатку
- Перевір що CORS headers додані в прошивку
- Онови прошивку Controller і Hub

## Структура проекту

```
hub/
├── src/
│   ├── main.cpp              # Точка входу Hub
│   ├── hub.h                 # Логіка Hub
│   ├── controller.h          # Логіка Controller
│   ├── display_hub.h         # Дисплей Hub
│   ├── display_controller.h  # Дисплей Controller
│   └── bms_parser.h          # Парсер BMS протоколу
├── platformio.ini            # Конфігурація PlatformIO
└── README.md                 # Цей файл
```

## Ліцензія
MIT License
