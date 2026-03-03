# DeepFilter-Asterisk: сборка и установка на Linux

Документация по сборке модуля `func_deepfilter.so` и его интеграции в Asterisk на Linux.

---

## Содержание

1. [Обзор](#обзор)
2. [Зависимости](#зависимости)
3. [Сборка через Docker](#сборка-через-docker)
4. [Сборка на хосте (native)](#сборка-на-хосте-native)
5. [Подключение модуля в Asterisk](#подключение-модуля-в-asterisk)
6. [Проверка и отладка](#проверка-и-отладка)
7. [Архитектура и диалплан](#архитектура-и-диалплан)

---

## Обзор

Модуль `func_deepfilter.so` — это **Asterisk Custom Function**, реализующая шумоподавление на базе DeepFilterNet. Модуль регистрирует функции диалплана:

- `DEEPFILTER(rx)` / `DEEPFILTER(tx)` — включение/выключение обработки
- `DEEPFILTER_ATTEN(rx)` / `DEEPFILTER_ATTEN(tx)` — лимит ослабления шума (dB)

Структура проекта:

```
deepfilter-asterisk/
├── module/
│   ├── Makefile           # Сборка модуля
│   ├── func_deepfilter.c  # Исходный код
│   └── df_bridge.h        # C API для libdeepfilter
├── config/                # Конфигурация Asterisk
├── Dockerfile             # Полная сборка в контейнере
└── docker-compose.yml
```

---

## Зависимости

| Зависимость | Назначение |
|-------------|------------|
| **Asterisk 22** | PBX, заголовки для компиляции |
| **libdeepfilter** | C API DeepFilterNet (Rust crate `deep_filter` с feature `capi`) |
| **libspeexdsp** | Ресемплинг 8/16 kHz ↔ 48 kHz |
| **ONNX модель DeepFilterNet3** | Предобученная модель |

---

## Сборка через Docker

Рекомендуемый способ — полная сборка в Docker (Asterisk + DeepFilterNet + модуль).

### Требования

- Docker и Docker Compose
- Достаточно RAM (рекомендуется ≥4 GB для сборки Rust)

### Шаги

```bash
# Клонировать репозиторий (если ещё не сделано)
git clone <repo-url> deepfilter-asterisk
cd deepfilter-asterisk

# Собрать образ (первый раз ~15–30 мин)
docker compose build

# Запустить
docker compose up -d

# Проверить
docker exec -it asterisk-df asterisk -rx "core show version"
docker exec -it asterisk-df asterisk -rx "module show like deepfilter"
```

### Важно по архитектуре

Dockerfile в репозитории настроен под **aarch64 (ARM64)**. На x86_64 необходимо поправить пути к библиотеке:

1. В `Dockerfile` заменить строки с `aarch64-linux-gnu` на `x86_64-linux-gnu` или на `/usr/local/lib`, в зависимости от того, куда `cargo cinstall` устанавливает библиотеку на вашей платформе.

2. Узнать фактический путь после сборки DeepFilterNet:

```bash
# Внутри builder-контейнера после сборки DeepFilterNet:
find /usr/local -name "*libdeepfilter*" -o -name "*libdeep_filter*"
```

Типичные варианты:

- **x86_64:** `/usr/local/lib/libdeepfilter.so` или `/usr/local/lib/x86_64-linux-gnu/`
- **aarch64:** `/usr/local/lib/aarch64-linux-gnu/libdeepfilter.so`

---

## Сборка на хосте (native)

Сборка модуля на Linux без Docker.

### 1. Установить Asterisk с заголовками

**Вариант A — пакет (Debian/Ubuntu):**

```bash
sudo apt update
sudo apt install -y asterisk asterisk-dev
```

**Вариант B — из исходников:**

```bash
cd /usr/src
wget https://downloads.asterisk.org/pub/telephony/asterisk/asterisk-22-current.tar.gz
tar xzf asterisk-22-current.tar.gz
cd asterisk-22.*

./configure --with-jansson-bundled
make menuselect.makeopts
menuselect/menuselect --enable func_speex --enable codec_speex --enable res_rtp_asterisk menuselect.makeopts
make -j$(nproc)
sudo make install
sudo make install-headers
```

### 2. Установить libspeexdsp

```bash
sudo apt install -y libspeexdsp-dev
```

### 3. Собрать и установить libdeepfilter

```bash
# Rust toolchain
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable
source ~/.cargo/env
cargo install cargo-c

# DeepFilterNet
cd /tmp
git clone --depth 1 https://github.com/Rikorose/DeepFilterNet.git
cd DeepFilterNet

cargo cinstall --package deep_filter --release --features capi --prefix /usr/local
sudo ldconfig
```

### 4. Скачать модель

```bash
sudo mkdir -p /usr/share/asterisk/deepfilter
sudo wget -O /usr/share/asterisk/deepfilter/DeepFilterNet3.tar.gz \
  "https://github.com/Rikorose/DeepFilterNet/raw/refs/heads/main/models/DeepFilterNet3_onnx.tar.gz"
```

### 5. Собрать модуль

```bash
cd /path/to/deepfilter-asterisk/module

# Сборка
make

# Установка (по умолчанию в /usr/lib/asterisk/modules)
sudo make install

# Опционально — другой каталог модулей
make install MODULES_DIR=/usr/lib/asterisk/modules
```

### 6. Проверка pkg-config (опционально)

Если `pkg-config libdeepfilter` не срабатывает, Makefile использует fallback:

- `-I/usr/local/include`
- `-L/usr/local/lib`
- `-ldeep_filter` (или `-ldeepfilter` в зависимости от имени библиотеки)

При расхождении имён библиотек (`libdeep_filter` vs `libdeepfilter`) отредактируйте `module/Makefile`, строка `LDFLAGS`:

```makefile
# Вариант 1 (обычно cargo cinstall)
LDFLAGS = -ldeep_filter -lspeexdsp -lpthread -lm

# Вариант 2 (если библиотека называется libdeepfilter)
LDFLAGS = -ldeepfilter -lspeexdsp -lpthread -lm
```

---

## Подключение модуля в Asterisk

### 1. Конфигурация modules.conf

Добавьте или проверьте загрузку модуля:

```ini
; /etc/asterisk/modules.conf

[modules]
autoload=yes

; Загрузить модуль DeepFilter
load=func_deepfilter.so
```

### 2. Убедиться в наличии модели

Модуль по умолчанию ищет модель по пути:

```
/usr/share/asterisk/deepfilter/DeepFilterNet3.tar.gz
```

Проверка:

```bash
ls -la /usr/share/asterisk/deepfilter/DeepFilterNet3.tar.gz
```

### 3. Загрузка модуля

**При запуске Asterisk:** модуль загрузится автоматически (autoload + load).

**Вручную (если Asterisk уже запущен):**

```bash
asterisk -rx "module load func_deepfilter.so"
```

**Перезагрузка после изменений:**

```bash
asterisk -rx "module unload func_deepfilter.so"
asterisk -rx "module load func_deepfilter.so"
```

Или через `make reload` из `module/`:

```bash
cd module
make reload
```

---

## Проверка и отладка

### Проверка загрузки модуля

```bash
asterisk -rx "module show like deepfilter"
```

Ожидаемый вывод:

```
Module                         Description                    Use Count  Status
func_deepfilter.so             AI Noise Reduction using Deep  0          Running
```

### Проверка функций диалплана

```bash
asterisk -rx "core show function DEEPFILTER"
asterisk -rx "core show function DEEPFILTER_ATTEN"
```

### Тестовые номера (dialplan)

| Номер | Описание |
|-------|----------|
| 6001, 6002 | Обычный звонок без шумоподавления |
| 7001, 7002 | Звонок с Speex |
| 8001, 8002 | Звонок с DeepFilterNet |
| 8801, 8802 | DeepFilterNet в обоих направлениях (rx+tx) |
| 9999 | Echo без обработки |
| 9998 | Echo с DeepFilterNet |
| 9997 | Echo с Speex |

### Типичные ошибки

| Симптом | Возможная причина |
|--------|-------------------|
| `Module load failed` | Нет libdeepfilter или libspeexdsp |
| `Failed to open model` | Модель не найдена в `/usr/share/asterisk/deepfilter/` |
| `symbol not found` | Версия libdeepfilter несовместима с df_bridge.h |
| Нет заголовков Asterisk | Не установлен asterisk-dev или не выполнен `make install-headers` |

### Проверка зависимостей

```bash
ldd /usr/lib/asterisk/modules/func_deepfilter.so
```

Все зависимости должны резолвиться (без `not found`).

---

## Архитектура и диалплан

### Использование в диалплане

```ini
; Включить DeepFilter на входящем аудио
exten => 8001,1,Set(DEEPFILTER(rx)=on)
 same => n,Dial(PJSIP/6001,30)
 same => n,Hangup()

; Включить на обоих направлениях
exten => 8801,1,Set(DEEPFILTER(rx)=on)
 same => n,Set(DEEPFILTER(tx)=on)
 same => n,Dial(PJSIP/6001,30)
 same => n,Hangup()

; Ограничить ослабление (dB)
exten => 8001,1,Set(DEEPFILTER_ATTEN(rx)=40)
 same => n,Set(DEEPFILTER(rx)=on)
 same => n,Dial(PJSIP/6001,30)
```

### Конвейер обработки

1. int16 → float32 (нормализация −1.0..1.0)
2. Ресемплинг до 48 kHz (Speex)
3. Буферизация и обработка фреймами 480 сэмплов
4. Ресемплинг обратно к частоте канала Asterisk
5. float32 → int16

### Зависимость от Speex

Модуль объявляет зависимость `speexdsp` в MODULEINFO. Убедитесь, что `func_speex.so` и `codec_speex` включены в сборку Asterisk (для сравнения и совместимости).

---

## Быстрая шпаргалка

```bash
# Docker
docker compose build && docker compose up -d
docker exec -it asterisk-df asterisk -rx "module show like deepfilter"

# Native build
cd module && make && sudo make install
asterisk -rx "module load func_deepfilter.so"

# Проверка
asterisk -rx "core show function DEEPFILTER"
asterisk -rx "dialplan reload"
```

---

*Документ создан на основе анализа кодовой базы deepfilter-asterisk.*
