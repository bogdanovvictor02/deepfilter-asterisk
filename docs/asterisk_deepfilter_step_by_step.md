# Пошаговая инструкция: Asterisk + DeepFilterNet на macOS

> **Рекомендация:** Нативная сборка Asterisk на macOS исторически проблемная и хрупкая.
> Мы используем **Docker** — это надёжнее, воспроизводимее, и ближе к продакшену (Linux).
> Ваш Mac будет хостом с SIP-клиентом, а Asterisk + DeepFilterNet будут внутри контейнера.

---

## ОБЗОР АРХИТЕКТУРЫ

```
┌──────────────────────────────────────────────────┐
│  macOS (ваш Mac)                                 │
│                                                  │
│  ┌──────────────┐     ┌──────────────┐           │
│  │  Telephone 1 │     │  Telephone 2 │           │
│  │  (Oперо /    │     │  (Oперо /    │           │
│  │   Zoiper)    │     │   Zoiper)    │           │
│  │  ext: 6001   │     │  ext: 6002   │           │
│  └──────┬───────┘     └──────┬───────┘           │
│         │    SIP/RTP         │                   │
│         └────────┬───────────┘                   │
│                  │                               │
│    ┌─────────────▼───────────────────────┐       │
│    │  Docker Container (Ubuntu/Debian)   │       │
│    │                                     │       │
│    │  Asterisk PBX                       │       │
│    │    ├── PJSIP channel driver         │       │
│    │    ├── func_speex.so (штатный)      │       │
│    │    └── func_deepfilter.so (НАШ)     │       │
│    │                                     │       │
│    │  DeepFilterNet (libdf.so + модель)  │       │
│    │  speexdsp (resampler)               │       │
│    │  Rust toolchain (для сборки)        │       │
│    └─────────────────────────────────────┘       │
│         порты: 5060/udp, 10000-10020/udp         │
└──────────────────────────────────────────────────┘
```

---

## ШАГ 0: Подготовка Mac

### 0.1 Установить Docker Desktop

```bash
# Если ещё нет — скачать и установить Docker Desktop для Mac:
# https://www.docker.com/products/docker-desktop/
# Или через Homebrew:
brew install --cask docker
```

Запустить Docker Desktop и убедиться что работает:
```bash
docker --version
docker run hello-world
```

### 0.2 Установить SIP-клиент (софтфон)

Вам нужны **два** SIP-клиента для тестирования звонков друг другу.

**Вариант A — Zoiper (рекомендуется):**
- Скачать бесплатную версию: https://www.zoiper.com/en/voip-softphone/download/current
- Установить на Mac

**Вариант B — Telephone.app (встроенный в macOS):**
- Открыть App Store → найти "Telephone" (бесплатный SIP-клиент)
- Или: `brew install --cask telephone`

**Вариант C — Linphone:**
- https://www.linphone.org/technical-corner/linphone

> Для полного теста нужны 2 клиента. Можно поставить Zoiper + Telephone,
> или использовать телефон (Android/iOS) как второй софтфон.

---

## ШАГ 1: Создать проект

```bash
mkdir -p ~/asterisk-deepfilter
cd ~/asterisk-deepfilter
```

### 1.1 Создать Dockerfile

```bash
cat > Dockerfile << 'DOCKERFILE_END'
# ============================================================
# Asterisk + DeepFilterNet — всё в одном контейнере
# ============================================================
FROM debian:bookworm-slim AS builder

ENV DEBIAN_FRONTEND=noninteractive

# ── Системные зависимости ──
RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    curl \
    wget \
    pkg-config \
    # Asterisk build deps
    libjansson-dev \
    libxml2-dev \
    libsqlite3-dev \
    libssl-dev \
    libedit-dev \
    uuid-dev \
    libspeex-dev \
    libspeexdsp-dev \
    libcurl4-openssl-dev \
    # Для PJSIP
    libpjproject-dev \
    # Утилиты
    autoconf \
    automake \
    libtool \
    && rm -rf /var/lib/apt/lists/*

# ── Установить Rust (для сборки DeepFilterNet) ──
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | \
    sh -s -- -y --default-toolchain stable
ENV PATH="/root/.cargo/bin:${PATH}"

# ── Собрать Asterisk из исходников ──
WORKDIR /usr/src
RUN wget -q https://downloads.asterisk.org/pub/telephony/asterisk/asterisk-22-current.tar.gz \
    && tar xzf asterisk-22-current.tar.gz \
    && rm asterisk-22-current.tar.gz \
    && mv asterisk-22.* asterisk

WORKDIR /usr/src/asterisk
RUN ./configure --with-jansson-bundled \
    && make menuselect.makeopts \
    && menuselect/menuselect \
        --enable func_speex \
        --enable codec_speex \
        --enable res_rtp_asterisk \
        menuselect.makeopts \
    && make -j$(nproc) \
    && make install \
    && make samples

# ── Клонировать и собрать DeepFilterNet (C API) ──
WORKDIR /usr/src
RUN git clone --depth 1 https://github.com/Rikorose/DeepFilterNet.git

WORKDIR /usr/src/DeepFilterNet
# Собираем libdf с поддержкой C API
RUN cargo build --release -p deep-filter --features capi \
    || cargo build --release -p df --features capi \
    || echo "NOTE: capi feature build attempted"

# Копируем собранную библиотеку
RUN find target/release -name "libdf*" -o -name "libdeep_filter*" | head -20 && \
    cp target/release/libdf.so /usr/local/lib/ 2>/dev/null || \
    cp target/release/libdeep_filter.so /usr/local/lib/ 2>/dev/null || \
    echo "WARNING: libdf not found, will need manual build"
RUN ldconfig

# Скачиваем предобученную модель DeepFilterNet3
RUN mkdir -p /usr/share/asterisk/deepfilter && \
    wget -q -O /usr/share/asterisk/deepfilter/DeepFilterNet3.tar.gz \
    "https://github.com/Rikorose/DeepFilterNet/releases/download/v0.5.6/DeepFilterNet3.tar.gz" \
    || echo "WARNING: model download may need manual step"

# ── Финальный образ ──
FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    libjansson4 \
    libxml2 \
    libsqlite3-0 \
    libssl3 \
    libedit2 \
    uuid-runtime \
    libspeex1 \
    libspeexdsp1 \
    libcurl4 \
    libpjproject-dev \
    tcpdump \
    net-tools \
    iputils-ping \
    sngrep \
    && rm -rf /var/lib/apt/lists/*

# Копируем Asterisk
COPY --from=builder /usr/sbin/asterisk /usr/sbin/
COPY --from=builder /usr/lib/asterisk/ /usr/lib/asterisk/
COPY --from=builder /var/lib/asterisk/ /var/lib/asterisk/
COPY --from=builder /var/spool/asterisk/ /var/spool/asterisk/
COPY --from=builder /var/log/asterisk/ /var/log/asterisk/
COPY --from=builder /var/run/asterisk/ /var/run/asterisk/
COPY --from=builder /etc/asterisk/ /etc/asterisk/

# Копируем DeepFilterNet
COPY --from=builder /usr/local/lib/libdf* /usr/local/lib/
COPY --from=builder /usr/local/lib/libdeep_filter* /usr/local/lib/
COPY --from=builder /usr/share/asterisk/deepfilter/ /usr/share/asterisk/deepfilter/
RUN ldconfig

# Создаём пользователя asterisk
RUN groupadd -r asterisk && useradd -r -g asterisk asterisk && \
    chown -R asterisk:asterisk /var/lib/asterisk /var/spool/asterisk \
    /var/log/asterisk /var/run/asterisk /etc/asterisk

EXPOSE 5060/udp 5060/tcp
EXPOSE 10000-10020/udp

CMD ["asterisk", "-f", "-vvv"]
DOCKERFILE_END
```

### 1.2 Создать конфигурации Asterisk

```bash
mkdir -p config
```

#### pjsip.conf — SIP аккаунты
```bash
cat > config/pjsip.conf << 'EOF'
; ════════════════════════════════════════
; PJSIP Transport
; ════════════════════════════════════════

[transport-udp]
type=transport
protocol=udp
bind=0.0.0.0:5060

; ════════════════════════════════════════
; Шаблон для телефонов
; ════════════════════════════════════════

[endpoint-template](!)
type=endpoint
transport=transport-udp
context=internal
disallow=all
allow=ulaw
allow=alaw
allow=g722
direct_media=no
rtp_symmetric=yes
force_rport=yes
rewrite_contact=yes

[auth-template](!)
type=auth
auth_type=userpass

[aor-template](!)
type=aor
max_contacts=5
remove_existing=yes

; ════════════════════════════════════════
; Телефон 1: extension 6001
; ════════════════════════════════════════

[6001](endpoint-template)
auth=6001-auth
aors=6001-aor
callerid="User One" <6001>

[6001-auth](auth-template)
username=6001
password=secret6001

[6001-aor](aor-template)

; ════════════════════════════════════════
; Телефон 2: extension 6002
; ════════════════════════════════════════

[6002](endpoint-template)
auth=6002-auth
aors=6002-aor
callerid="User Two" <6002>

[6002-auth](auth-template)
username=6002
password=secret6002

[6002-aor](aor-template)
EOF
```

#### extensions.conf — логика звонков (dialplan)
```bash
cat > config/extensions.conf << 'EOF'
; ════════════════════════════════════════
; Dialplan для тестирования DeepFilterNet
; ════════════════════════════════════════

[general]
static=yes
writeprotect=yes

[internal]

; --- Обычный звонок (без шумоподавления) ---
; Набрать 6001 или 6002 чтобы позвонить
exten => 6001,1,NoOp(Calling 6001 - NO noise reduction)
 same => n,Dial(PJSIP/6001,30)
 same => n,Hangup()

exten => 6002,1,NoOp(Calling 6002 - NO noise reduction)
 same => n,Dial(PJSIP/6002,30)
 same => n,Hangup()

; --- Звонок С шумоподавлением Speex (для сравнения) ---
; Набрать 7001 или 7002
exten => 7001,1,NoOp(Calling 6001 WITH Speex denoise)
 same => n,Set(DENOISE(rx)=on)
 same => n,Dial(PJSIP/6001,30)
 same => n,Hangup()

exten => 7002,1,NoOp(Calling 6002 WITH Speex denoise)
 same => n,Set(DENOISE(rx)=on)
 same => n,Dial(PJSIP/6002,30)
 same => n,Hangup()

; --- Звонок С шумоподавлением DeepFilterNet ---
; Набрать 8001 или 8002
; (будет работать ПОСЛЕ того как мы соберём и загрузим модуль)
exten => 8001,1,NoOp(Calling 6001 WITH DeepFilterNet)
 same => n,Set(DEEPFILTER(rx)=on)
 same => n,Dial(PJSIP/6001,30)
 same => n,Hangup()

exten => 8002,1,NoOp(Calling 6002 WITH DeepFilterNet)
 same => n,Set(DEEPFILTER(rx)=on)
 same => n,Dial(PJSIP/6002,30)
 same => n,Hangup()

; --- Echo test (слышите себя) ---
; Набрать 9999
exten => 9999,1,NoOp(Echo test)
 same => n,Answer()
 same => n,Echo()
 same => n,Hangup()

; --- Echo test с DeepFilterNet ---
; Набрать 9998
exten => 9998,1,NoOp(Echo test WITH DeepFilterNet)
 same => n,Answer()
 same => n,Set(DEEPFILTER(rx)=on)
 same => n,Echo()
 same => n,Hangup()
EOF
```

#### modules.conf
```bash
cat > config/modules.conf << 'EOF'
[modules]
autoload=yes
noload=chan_alsa.so
noload=chan_oss.so
noload=chan_mgcp.so
noload=chan_skinny.so
noload=chan_unistim.so
noload=chan_sip.so
; загружаем наш модуль когда он будет готов:
; load=func_deepfilter.so
EOF
```

#### rtp.conf
```bash
cat > config/rtp.conf << 'EOF'
[general]
rtpstart=10000
rtpend=10020
EOF
```

#### logger.conf
```bash
cat > config/logger.conf << 'EOF'
[general]

[logfiles]
console => notice,warning,error,verbose
messages => notice,warning,error
full => notice,warning,error,debug,verbose,dtmf,fax
EOF
```

### 1.3 docker-compose.yml

```bash
cat > docker-compose.yml << 'EOF'
version: '3.8'

services:
  asterisk:
    build: .
    container_name: asterisk-df
    # network_mode: host даёт минимум NAT-проблем с RTP
    network_mode: host
    volumes:
      # Конфиги Asterisk
      - ./config/pjsip.conf:/etc/asterisk/pjsip.conf
      - ./config/extensions.conf:/etc/asterisk/extensions.conf
      - ./config/modules.conf:/etc/asterisk/modules.conf
      - ./config/rtp.conf:/etc/asterisk/rtp.conf
      - ./config/logger.conf:/etc/asterisk/logger.conf
      # Директория для нашего модуля
      - ./module:/usr/lib/asterisk/modules/custom
    restart: unless-stopped
    cap_add:
      - NET_ADMIN
EOF
```

---

## ШАГ 2: Сборка и запуск базового Asterisk

```bash
cd ~/asterisk-deepfilter

# Собрать образ (первый раз — 15-30 минут)
docker compose build

# Запустить
docker compose up -d

# Проверить что работает
docker exec -it asterisk-df asterisk -rx "core show version"

# Посмотреть логи
docker logs -f asterisk-df
```

**Проверки в CLI Asterisk:**
```bash
# Подключиться к CLI Asterisk
docker exec -it asterisk-df asterisk -rvvv

# В CLI Asterisk:
core show version
pjsip show endpoints
module show like speex
dialplan show internal
```

Вы должны увидеть:
- Asterisk запущен
- Endpoints 6001 и 6002 зарегистрированы (после подключения софтфонов)
- func_speex.so загружен

---

## ШАГ 3: Настройка SIP-клиентов

### 3.1 Узнать IP вашего Mac

```bash
# Для Docker с network_mode: host — это IP вашего Mac
ifconfig | grep "inet " | grep -v 127.0.0.1
# Запомните адрес, например: 192.168.1.100
```

> **Примечание для Docker Desktop на Mac:**
> `network_mode: host` на macOS работает не так как на Linux.
> Если не подключается, замените в docker-compose.yml на:
> ```yaml
>     ports:
>       - "5060:5060/udp"
>       - "5060:5060/tcp"
>       - "10000-10020:10000-10020/udp"
> ```
> И используйте `127.0.0.1` как адрес сервера.

### 3.2 Настроить Zoiper (Телефон 1 — 6001)

1. Открыть Zoiper
2. Settings → Accounts → Add account
3. Заполнить:
   - **Username:** `6001`
   - **Password:** `secret6001`
   - **Domain/Host:** `127.0.0.1` (или IP вашего Mac)
   - **Port:** `5060`
   - **Transport:** UDP
4. Save → должен появиться зелёный статус "Registered"

### 3.3 Настроить Telephone.app (Телефон 2 — 6002)

1. Открыть Telephone
2. Telephone → Preferences → Accounts → "+"
3. Заполнить:
   - **Full Name:** User Two
   - **Domain:** `127.0.0.1`
   - **User Name:** `6002`
   - **Password:** `secret6002`
4. Сохранить

### 3.4 Первый тестовый звонок (БЕЗ шумоподавления)

1. В Zoiper (6001) набрать: **6002**
2. В Telephone (6002) должен прийти звонок
3. Ответить → проверить двустороннюю связь
4. Повесить трубку

**Если звонок прошёл — базовая настройка работает!**

### 3.5 Тест Echo (слышите себя)

1. В Zoiper набрать: **9999**
2. Вы должны слышать собственный голос с задержкой
3. Это самый простой способ тестировать шумоподавление

---

## ШАГ 4: Тест штатного шумоподавления Speex (для сравнения)

```bash
# Убедитесь что func_speex загружен
docker exec -it asterisk-df asterisk -rx "module show like speex"
```

Должны увидеть: `func_speex.so`

**Тест:**
1. В Zoiper набрать: **7002** (вместо 6002)
2. Этот вызов проходит через `Set(DENOISE(rx)=on)` — Speex denoise
3. Попробуйте говорить с фоновым шумом и сравните с обычным звонком на 6002

---

## ШАГ 5: Сборка модуля func_deepfilter.so

### 5.1 Войти в контейнер

```bash
docker exec -it asterisk-df bash
```

### 5.2 Установить зависимости для сборки (внутри контейнера)

```bash
apt-get update && apt-get install -y \
    build-essential \
    git \
    curl \
    wget \
    pkg-config \
    libspeexdsp-dev

# Установить Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source ~/.cargo/env
```

### 5.3 Собрать DeepFilterNet с C API

```bash
cd /usr/src
git clone --depth 1 https://github.com/Rikorose/DeepFilterNet.git
cd DeepFilterNet

# Собрать библиотеку
cargo build --release -p deep-filter --features capi

# Найти собранную библиотеку
find target/release -name "*.so" -o -name "*.dylib" | head -10

# Установить
cp target/release/libdf.so /usr/local/lib/ 2>/dev/null || \
cp target/release/libdeep_filter.so /usr/local/lib/ 2>/dev/null
ldconfig

# Скачать модель
mkdir -p /usr/share/asterisk/deepfilter
cd /usr/share/asterisk/deepfilter
wget "https://github.com/Rikorose/DeepFilterNet/releases/download/v0.5.6/DeepFilterNet3.tar.gz"
```

### 5.4 Скомпилировать модуль Asterisk

```bash
cd /usr/src

# Создать файл func_deepfilter.c
# (здесь вы вставляете код из нашего технического документа)
# Для начала — создадим заглушку для проверки сборки:

cat > func_deepfilter.c << 'CEOF'
/*
 * func_deepfilter.c — AI Noise Reduction for Asterisk
 * Placeholder for build testing
 */

#include "asterisk.h"
#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/utils.h"
#include "asterisk/audiohook.h"
#include <speex/speex_resampler.h>

/* TODO: #include "df_bridge.h" — когда C API будет готов */

static int df_write(struct ast_channel *chan, const char *cmd,
                    char *data, const char *value)
{
    ast_log(LOG_NOTICE, "DeepFilter: %s(%s) = %s (placeholder)\n",
            cmd, data, value);
    return 0;
}

static int df_read(struct ast_channel *chan, const char *cmd,
                   char *data, char *buf, size_t len)
{
    snprintf(buf, len, "off");
    return 0;
}

static struct ast_custom_function deepfilter_function = {
    .name = "DEEPFILTER",
    .write = df_write,
    .read = df_read,
    .read_max = 22,
};

static int unload_module(void)
{
    ast_custom_function_unregister(&deepfilter_function);
    return 0;
}

static int load_module(void)
{
    if (ast_custom_function_register(&deepfilter_function))
        return AST_MODULE_LOAD_DECLINE;

    ast_log(LOG_NOTICE, "DeepFilter: AI noise suppression loaded (placeholder)\n");
    return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY,
    "AI Noise Reduction using DeepFilterNet");
CEOF

# Компилируем
gcc -shared -fPIC -o func_deepfilter.so func_deepfilter.c \
    -I/usr/include \
    -I/usr/include/asterisk \
    -lspeexdsp \
    -Wall -O2 \
    -DAST_MODULE_SELF_SYM=__internal_func_deepfilter_self

# Устанавливаем
cp func_deepfilter.so /usr/lib/asterisk/modules/
```

### 5.5 Загрузить модуль в Asterisk

```bash
# Из CLI Asterisk
asterisk -rx "module load func_deepfilter.so"
asterisk -rx "module show like deepfilter"
```

Вы должны увидеть:
```
Module                         Description                    Use Count  Status
func_deepfilter.so             AI Noise Reduction using Deep  0          Running
```

### 5.6 Проверить что функция DEEPFILTER доступна

```bash
asterisk -rx "core show function DEEPFILTER"
```

---

## ШАГ 6: Тестирование

### 6.1 План тестирования

| Тест | Что набрать | Что проверяем |
|------|-------------|---------------|
| Базовый звонок | 6002 | Двусторонняя связь работает |
| Echo без фильтра | 9999 | Слышите себя без изменений |
| Speex denoise | 7002 | Штатное шумоподавление |
| DeepFilter | 8002 | Наше шумоподавление |
| DeepFilter Echo | 9998 | Слышите себя через DF |

### 6.2 Как тестировать шумоподавление

**Источник шума:**
1. Откройте на втором устройстве YouTube: "office background noise" или "coffee shop ambience"
2. Поставьте рядом с микрофоном
3. Говорите одновременно с фоновым шумом

**Сравнение:**
1. Позвоните на **9999** (echo без фильтра) → запомните уровень шума
2. Позвоните на **9998** (echo с DeepFilter) → сравните

### 6.3 Мониторинг в реальном времени

```bash
# В отдельном терминале — смотрим логи
docker exec -it asterisk-df asterisk -rvvv

# Или только наш модуль
docker exec -it asterisk-df asterisk -rx "core set verbose 5"
```

### 6.4 Отладка проблем

```bash
# SIP регистрация не работает?
docker exec -it asterisk-df asterisk -rx "pjsip show endpoints"
docker exec -it asterisk-df asterisk -rx "pjsip show registrations"

# Нет звука?
docker exec -it asterisk-df asterisk -rx "rtp set debug on"

# Модуль не загружается?
docker exec -it asterisk-df asterisk -rx "module show like deepfilter"

# Просмотр всех загруженных модулей
docker exec -it asterisk-df asterisk -rx "module show"

# Перезагрузить dialplan после изменений
docker exec -it asterisk-df asterisk -rx "dialplan reload"

# sngrep — визуальный анализ SIP трафика
docker exec -it asterisk-df sngrep
```

---

## ШАГ 7: Итерация — от заглушки к рабочему модулю

После того как заглушка работает (Шаг 5), нужно заменить её на полный код:

### 7.1 Цикл разработки

```bash
# 1. Отредактировать func_deepfilter.c на Mac
#    (файл монтируется через volume или копируется в контейнер)

# 2. Скопировать в контейнер
docker cp func_deepfilter.c asterisk-df:/usr/src/

# 3. Скомпилировать внутри контейнера
docker exec -it asterisk-df bash -c "
    cd /usr/src && \
    gcc -shared -fPIC -o func_deepfilter.so func_deepfilter.c \
        -I/usr/include -lspeexdsp -ldf -L/usr/local/lib \
        -Wall -O2 -DAST_MODULE_SELF_SYM=__internal_func_deepfilter_self && \
    cp func_deepfilter.so /usr/lib/asterisk/modules/
"

# 4. Перезагрузить модуль в Asterisk
docker exec -it asterisk-df asterisk -rx "module unload func_deepfilter.so"
docker exec -it asterisk-df asterisk -rx "module load func_deepfilter.so"

# 5. Тестировать — позвонить на 8001/8002/9998
```

### 7.2 Когда будете готовы — дайте мне команду

Скажите мне:
> "Напиши полный рабочий код func_deepfilter.c"

И я создам полный файл с:
- Интеграцией C API DeepFilterNet
- Ресемплингом через speex_resampler
- Буферизацией фреймов
- Обработкой ошибок
- Всеми dialplan-функциями

---

## КРАТКАЯ ШПАРГАЛКА КОМАНД

```bash
# ═══ Docker ═══
docker compose build                  # Собрать образ
docker compose up -d                  # Запустить
docker compose down                   # Остановить
docker compose logs -f                # Логи
docker exec -it asterisk-df bash      # Войти в контейнер

# ═══ Asterisk CLI ═══
docker exec -it asterisk-df asterisk -rvvv    # Интерактивный CLI
docker exec -it asterisk-df asterisk -rx "CMD" # Одна команда

# Полезные команды CLI:
core show version
core show channels                    # Текущие звонки
pjsip show endpoints                  # SIP аккаунты
pjsip show contacts                   # Зарегистрированные телефоны
module show like deepfilter           # Наш модуль
module load func_deepfilter.so        # Загрузить
module unload func_deepfilter.so      # Выгрузить
module reload func_deepfilter.so      # Перезагрузить
dialplan reload                       # Перечитать extensions.conf
core show function DEEPFILTER         # Инфо о нашей функции

# ═══ Отладка ═══
rtp set debug on                      # Дебаг RTP
pjsip set logger on                   # Дебаг SIP
core set verbose 5                    # Максимум логов
```

---

## ЧАСТЫЕ ПРОБЛЕМЫ

### P1: Docker Desktop на Mac — нет связи с SIP

**Причина:** `network_mode: host` на Docker Desktop for Mac работает иначе чем на Linux.

**Решение:** Использовать port mapping:
```yaml
# docker-compose.yml
services:
  asterisk:
    ports:
      - "5060:5060/udp"
      - "5060:5060/tcp"
      - "10000-10020:10000-10020/udp"
```
SIP клиент → сервер: `127.0.0.1:5060`

### P2: Звонок проходит, но нет звука (one-way или no audio)

**Причина:** NAT/RTP проблемы.

**Решение:** Убедитесь что в pjsip.conf:
```ini
direct_media=no
rtp_symmetric=yes
force_rport=yes
rewrite_contact=yes
```

### P3: cargo build не находит "capi" feature

**Причина:** Версия DeepFilterNet может отличаться.

**Решение:** Проверить доступные features:
```bash
cd /usr/src/DeepFilterNet
grep -r "capi" libDF/Cargo.toml
cat libDF/Cargo.toml | grep features -A 20
```

### P4: gcc не находит asterisk headers

**Решение:** Установить asterisk-dev или указать путь:
```bash
# Найти заголовки
find / -name "audiohook.h" 2>/dev/null
# Использовать -I/path/to/include
```
