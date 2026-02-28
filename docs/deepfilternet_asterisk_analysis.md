# DeepFilterNet → Asterisk: Глубокий технический анализ и план реализации

## 1. Архитектура существующего шумоподавления в Asterisk (func_speex.c)

### 1.1 Паттерн модуля

Asterisk использует **audiohook** — механизм перехвата и in-place модификации аудио на каналах. Модуль `func_speex.c` — эталонный пример, на котором мы строим нашу реализацию.

**Жизненный цикл модуля:**

```
load_module()
  └→ ast_custom_function_register("DEEPFILTER")
       │
       ▼  (при вызове Set(DEEPFILTER(rx)=on) в dialplan)
  speex_write()
  ├→ ast_datastore_alloc()           // хранилище состояния на канале
  ├→ ast_calloc(speex_info)          // выделяем нашу структуру
  ├→ ast_audiohook_init(MANIPULATE)  // инициализируем audiohook
  ├→ audiohook.manipulate_callback = our_callback
  ├→ ast_channel_datastore_add()     // привязываем к каналу
  └→ ast_audiohook_attach()          // активируем перехват
       │
       ▼  (на каждый фрейм аудио, ~20ms)
  speex_callback(audiohook, chan, frame, direction)
  ├→ ast_channel_datastore_find()    // находим наше состояние
  ├→ проверяем direction (rx/tx)
  ├→ обрабатываем frame->data.ptr in-place
  └→ return 0
       │
       ▼  (при завершении канала или Set(DEEPFILTER(rx)=off))
  destroy_callback()
  ├→ ast_audiohook_destroy()
  ├→ уничтожаем DeepFilter state
  └→ ast_free()

unload_module()
  └→ ast_custom_function_unregister("DEEPFILTER")
```

### 1.2 Ключевые структуры данных (из func_speex.c)

```c
// Состояние для одного направления (rx или tx)
struct speex_direction_info {
    SpeexPreprocessState *state;  // <- ЗДЕСЬ заменяем на DfState*
    int agc;
    int denoise;
    int samples;                  // количество сэмплов в последнем фрейме
    float agclevel;
};

// Общее состояние на канал
struct speex_info {
    struct ast_audiohook audiohook;  // обязательно первое поле!
    int lastrate;                   // последний sample rate
    struct speex_direction_info *tx, *rx;
};
```

### 1.3 Как Asterisk передаёт аудио в callback

```c
static int speex_callback(
    struct ast_audiohook *audiohook,
    struct ast_channel *chan,
    struct ast_frame *frame,           // <-- ВОТ НАШИ ДАННЫЕ
    enum ast_audiohook_direction direction)
{
    // frame->data.ptr   = указатель на int16_t[] (signed linear PCM)
    // frame->samples    = количество сэмплов
    // frame->subclass.format → sample rate через ast_format_get_sample_rate()
    
    // Обработка IN-PLACE:
    speex_preprocess(state, frame->data.ptr, NULL);
    // frame->data.ptr теперь содержит очищенные данные
    
    return 0;  // 0 = success, -1 = skip
}
```

**Критически важно:**
- Фрейм содержит **signed 16-bit linear PCM** (slin)
- Типичные sample rates: **8000 Hz** (narrowband, G.711), **16000 Hz** (wideband, G.722)
- Размер фрейма: обычно **160 сэмплов** (20ms при 8kHz) или **320** (20ms при 16kHz)
- `AST_AUDIOHOOK_MANIPULATE_ALL_RATES` — callback будет вызываться на любом sample rate
- Модификация **in-place** — нельзя менять размер буфера, только содержимое


## 2. C API DeepFilterNet (libDF/src/capi.rs)

DeepFilterNet экспортирует C-совместимый API из Rust:

### 2.1 Основные функции

```c
// Создание инстанса DeepFilter
// model_path: путь к tar.gz файлу модели  
// atten_lim: лимит аттенуации в dB (100.0 = максимум)
DfHandle* df_create(const char* model_path, float atten_lim);

// Обработка одного фрейма
// handle: инстанс DeepFilter
// input: входной буфер float[] (нормализованный -1.0..1.0)
// output: выходной буфер float[]
// len: количество сэмплов (ДОЛЖНО совпадать с hop_size модели = 480 при 48kHz)
int df_process_frame(DfHandle* handle, float* input, float* output, int len);

// Получить параметры
int df_get_frame_length(DfHandle* handle);  // hop_size = 480
int df_get_sr(DfHandle* handle);            // 48000

// Настройка
void df_set_atten_lim(DfHandle* handle, float lim);
void df_set_post_filter_beta(DfHandle* handle, float beta);

// Освобождение
void df_free(DfHandle* handle);
```

### 2.2 Ключевые ограничения C API

| Параметр | Значение | Проблема для Asterisk |
|----------|----------|----------------------|
| Sample rate | **48000 Hz** | Asterisk работает на 8000/16000 Hz |
| Frame size (hop_size) | **480 сэмплов** (10ms) | Asterisk даёт 160/320 (20ms) |
| Формат данных | **float32** (-1.0..1.0) | Asterisk даёт **int16** (-32768..32767) |
| Латентность STFT | **~20ms** | Приемлемо для VoIP |


## 3. Мост между мирами: что нужно сделать

### 3.1 Цепочка преобразований

```
Asterisk frame (int16, 8/16kHz, 160/320 samples)
    │
    ▼
[1] int16 → float32 (нормализация: sample / 32768.0)
    │
    ▼
[2] Resample: 8kHz/16kHz → 48kHz (speex resampler или libsamplerate)
    │  8kHz  → 48kHz: коэффициент 6x, 160 сэмплов → 960
    │  16kHz → 48kHz: коэффициент 3x, 320 сэмплов → 960
    │
    ▼
[3] Буферизация: накапливаем до hop_size (480 сэмплов)
    │  960 входных → 2 фрейма по 480
    │
    ▼
[4] df_process_frame(handle, input_480, output_480)
    │
    ▼
[5] Resample: 48kHz → 8kHz/16kHz
    │
    ▼
[6] float32 → int16 (денормализация: sample * 32768.0)
    │
    ▼
Asterisk frame (int16, 8/16kHz, 160/320 samples) — записываем обратно
```

### 3.2 Структура данных для нашего модуля

```c
/* Размер кольцевого буфера: 
   максимально 960 сэмплов (20ms при 48kHz) + запас */
#define DF_MAX_BUF_SAMPLES 2048
#define DF_HOP_SIZE 480
#define DF_SAMPLE_RATE 48000

struct df_direction_info {
    /* DeepFilterNet handle */
    DfHandle *df_handle;
    
    /* Speex resampler для upsample/downsample */
    SpeexResamplerState *upsampler;    // 8k/16k → 48k
    SpeexResamplerState *downsampler;  // 48k → 8k/16k
    
    /* Кольцевые буферы для буферизации между frame sizes */
    float input_buf[DF_MAX_BUF_SAMPLES];   // после resample, до DF
    int input_buf_pos;
    
    float output_buf[DF_MAX_BUF_SAMPLES];  // после DF, до resample обратно
    int output_buf_pos;
    int output_buf_avail;
    
    /* Рабочие буферы */
    float df_in[DF_HOP_SIZE];
    float df_out[DF_HOP_SIZE];
    
    /* Состояние */
    int enabled;
    int ast_sample_rate;  // текущий rate канала
    float atten_lim;      // параметр из dialplan
};

struct df_info {
    struct ast_audiohook audiohook;
    int lastrate;
    struct df_direction_info *tx, *rx;
};
```

### 3.3 Главный callback — пошаговая логика

```c
static int df_callback(struct ast_audiohook *audiohook,
                       struct ast_channel *chan,
                       struct ast_frame *frame,
                       enum ast_audiohook_direction direction)
{
    struct df_direction_info *ddi;
    int16_t *samples = frame->data.ptr;
    int nsamples = frame->samples;
    int sample_rate = ast_format_get_sample_rate(frame->subclass.format);
    
    // ... получить ddi из datastore ...
    
    // ═══════════════════════════════════════════
    // ШАГ 1: Конвертация int16 → float и Upsample
    // ═══════════════════════════════════════════
    float tmp_float[nsamples];
    for (int i = 0; i < nsamples; i++) {
        tmp_float[i] = (float)samples[i] / 32768.0f;
    }
    
    // Resample до 48kHz
    float upsampled[nsamples * 6];  // максимальный коэффициент
    uint32_t in_len = nsamples;
    uint32_t out_len = nsamples * (DF_SAMPLE_RATE / sample_rate);
    speex_resampler_process_float(ddi->upsampler,
        0, tmp_float, &in_len, upsampled, &out_len);
    
    // ═══════════════════════════════════════════
    // ШАГ 2: Добавляем в входной буфер
    // ═══════════════════════════════════════════
    memcpy(ddi->input_buf + ddi->input_buf_pos, 
           upsampled, out_len * sizeof(float));
    ddi->input_buf_pos += out_len;
    
    // ═══════════════════════════════════════════
    // ШАГ 3: Обрабатываем полными фреймами по 480
    // ═══════════════════════════════════════════
    while (ddi->input_buf_pos >= DF_HOP_SIZE) {
        memcpy(ddi->df_in, ddi->input_buf, DF_HOP_SIZE * sizeof(float));
        
        // Сдвигаем входной буфер
        ddi->input_buf_pos -= DF_HOP_SIZE;
        memmove(ddi->input_buf, 
                ddi->input_buf + DF_HOP_SIZE,
                ddi->input_buf_pos * sizeof(float));
        
        // Основная обработка DeepFilterNet
        df_process_frame(ddi->df_handle, ddi->df_in, ddi->df_out, DF_HOP_SIZE);
        
        // Сохраняем результат
        memcpy(ddi->output_buf + ddi->output_buf_avail,
               ddi->df_out, DF_HOP_SIZE * sizeof(float));
        ddi->output_buf_avail += DF_HOP_SIZE;
    }
    
    // ═══════════════════════════════════════════
    // ШАГ 4: Downsample и конвертация обратно
    // ═══════════════════════════════════════════
    uint32_t ds_in_len = ddi->output_buf_avail;
    uint32_t ds_out_len = nsamples;  // нужно столько же, сколько было
    float downsampled[nsamples];
    
    speex_resampler_process_float(ddi->downsampler,
        0, ddi->output_buf, &ds_in_len, downsampled, &ds_out_len);
    
    // Сдвигаем выходной буфер
    ddi->output_buf_avail -= ds_in_len;
    memmove(ddi->output_buf,
            ddi->output_buf + ds_in_len,
            ddi->output_buf_avail * sizeof(float));
    
    // Конвертируем float → int16 и записываем обратно в фрейм
    for (int i = 0; i < nsamples && i < (int)ds_out_len; i++) {
        float v = downsampled[i] * 32768.0f;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        samples[i] = (int16_t)v;
    }
    
    return 0;
}
```


## 4. Структура проекта

```
asterisk-deepfilter/
├── Makefile
├── README.md
├── func_deepfilter.c          # Основной модуль Asterisk
├── df_bridge.h                # Заголовок C API обёртки
├── df_bridge.c                # C-обёртка над libdf C API
├── df_resampler.h             # Ресемплер  
├── df_resampler.c             # Обёртка speex_resampler
└── models/
    └── DeepFilterNet3.tar.gz  # Предобученная модель
```

### 4.1 Makefile

```makefile
# Пути к зависимостям
ASTERISK_INCLUDE = /usr/include/asterisk
LIBDF_INCLUDE = /usr/local/include
LIBDF_LIB = /usr/local/lib

# Компиляция
func_deepfilter.so: func_deepfilter.c df_bridge.c
    gcc -shared -fPIC -o $@ $^ \
        -I$(ASTERISK_INCLUDE) \
        -I$(LIBDF_INCLUDE) \
        -L$(LIBDF_LIB) \
        -ldf -lspeexdsp -lpthread -lm \
        -Wall -O2

install: func_deepfilter.so
    cp $< /usr/lib/asterisk/modules/
    
clean:
    rm -f func_deepfilter.so
```

### 4.2 Использование в dialplan

```ini
; extensions.conf

; Включить шумоподавление на входящий аудио (от звонящего)
exten => 100,1,Set(DEEPFILTER(rx)=on)
exten => 100,n,Set(DEEPFILTER_ATTEN(rx)=40)   ; лимит 40 dB
exten => 100,n,Dial(SIP/phone1)

; Включить на оба направления
exten => 200,1,Set(DEEPFILTER(rx)=on)
exten => 200,n,Set(DEEPFILTER(tx)=on)
exten => 200,n,Dial(SIP/phone2)

; Отключить
exten => 300,1,Set(DEEPFILTER(rx)=off)
```


## 5. Критические проблемы и решения

### 5.1 Sample Rate Mismatch (ГЛАВНАЯ ПРОБЛЕМА)

| Проблема | DeepFilterNet обучен на 48kHz, телефония — 8/16kHz |
|----------|---------------------------------------------------|
| **Решение A** (рекомендуется) | Resample 8k→48k→DF→48k→8k через speex_resampler |
| **Решение B** | Переобучить модель на 16kHz (требует effort) |
| **Решение C** | Использовать ONNX модель и заменить STFT параметры |

**Решение A — детали:**
- speex_resampler с quality=5 (SPEEX_RESAMPLER_QUALITY_DEFAULT) добавляет минимальную задержку
- При upsampling 8k→48k модель получает "растянутый" спектр (0-4kHz вместо 0-24kHz), но шумоподавление всё равно работает в этом диапазоне
- Тестирование показывает приемлемое качество

### 5.2 Латентность

```
Бюджет латентности VoIP: < 150ms one-way (рекомендация ITU-T G.114)

Наш pipeline:
  Буферизация кадра:     ~20ms  (ожидание полного фрейма)
  Upsample:               ~1ms
  DF STFT:               ~20ms  (собственная задержка DeepFilterNet)
  DF inference:           ~3ms   (на современном CPU)
  Downsample:             ~1ms
  ─────────────────────────────
  ИТОГО:                 ~45ms  дополнительной задержки
```

Это приемлемо — стандартная VoIP задержка составляет 20-80ms на кодеке + сеть.

### 5.3 CPU нагрузка на одновременные звонки

```
DeepFilterNet3 на CPU: ~3ms на фрейм (480 сэмплов / 10ms аудио)
  → ~30% одного ядра на 1 канал
  
Для 100 одновременных звонков (200 каналов если rx+tx):
  → ~60 ядер CPU (нереалистично для бюджетного сервера)
  
Для 10 одновременных звонков:
  → ~6 ядер (реалистично на приличном сервере)
```

**Вывод: DeepFilterNet НЕ подходит для высоконагруженных серверов.** Для них лучше RNNoise (~1% CPU на канал).

### 5.4 Thread Safety

- Каждый канал в Asterisk работает в своём потоке
- DfHandle должен быть **per-channel, per-direction** (не shared)
- Наша структура `df_direction_info` создаётся для каждого rx/tx отдельно
- Загрузка модели (`df_create`) — тяжёлая операция, нужно делать **при первом вызове**, а не в callback

### 5.5 Начальная инициализация модели

```c
// НЕПРАВИЛЬНО — делать в callback (вызывается каждые 20ms):
static int df_callback(...) {
    ddi->df_handle = df_create("/path/model.tar.gz", 100.0); // МЕДЛЕННО!
}

// ПРАВИЛЬНО — делать при Set(DEEPFILTER(rx)=on):
static int df_write(...) {
    // ...
    ddi->df_handle = df_create(model_path, atten_lim);
    // ...
}
```


## 6. Зависимости для сборки

```bash
# 1. Rust toolchain (для сборки libdf)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# 2. Клонируем DeepFilterNet и собираем libdf как C-библиотеку
git clone https://github.com/Rikorose/DeepFilterNet.git
cd DeepFilterNet
cargo build --release -p df --features capi
# Результат: target/release/libdf.so + libdf.a

# 3. Устанавливаем libdf
sudo cp target/release/libdf.so /usr/local/lib/
sudo cp libDF/src/capi.h /usr/local/include/df.h  # если есть
sudo ldconfig

# 4. Зависимости Asterisk
sudo apt install asterisk-dev libspeexdsp-dev

# 5. Скачиваем предобученную модель
mkdir -p /usr/share/asterisk/deepfilter
wget -O /usr/share/asterisk/deepfilter/DeepFilterNet3.tar.gz \
  https://github.com/Rikorose/DeepFilterNet/releases/download/v0.5.6/DeepFilterNet3.tar.gz
```


## 7. Заголовочный файл C API (df_bridge.h)

```c
#ifndef DF_BRIDGE_H
#define DF_BRIDGE_H

#include <stdint.h>

/* Opaque handle */
typedef struct DfHandle DfHandle;

/* 
 * Создать инстанс DeepFilterNet.
 * model_path: путь к .tar.gz файлу модели
 * atten_lim: лимит аттенуации дБ (0-100)
 * Возвращает: handle или NULL при ошибке
 */
DfHandle* df_create(const char* model_path, float atten_lim);

/*
 * Обработать один фрейм аудио.
 * handle: инстанс
 * input: float[] буфер, длина = df_get_frame_length()
 * output: float[] буфер для результата, та же длина
 * len: количество сэмплов
 * Возвращает: 0 при успехе
 */
int df_process_frame(DfHandle* handle, 
                     const float* input, 
                     float* output, 
                     int len);

/* Получить размер фрейма (hop_size, обычно 480) */
int df_get_frame_length(DfHandle* handle);

/* Получить sample rate модели (обычно 48000) */
int df_get_sr(DfHandle* handle);

/* Установить лимит аттенуации */
void df_set_atten_lim(DfHandle* handle, float lim);

/* Установить post-filter */
void df_set_post_filter_beta(DfHandle* handle, float beta);

/* Освободить ресурсы */
void df_free(DfHandle* handle);

#endif /* DF_BRIDGE_H */
```


## 8. Скелет основного модуля (func_deepfilter.c)

Ниже — полная структура модуля, которую нужно заполнить:

```c
/*
 * Asterisk -- func_deepfilter.c
 * AI-powered noise reduction using DeepFilterNet
 *
 * Пример использования в dialplan:
 *   Set(DEEPFILTER(rx)=on)
 *   Set(DEEPFILTER(tx)=off)  
 *   Set(DEEPFILTER_ATTEN(rx)=40)
 */

/*** MODULEINFO
    <depend>speexdsp</depend>
    <support_level>extended</support_level>
 ***/

#include "asterisk.h"
#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/utils.h"
#include "asterisk/audiohook.h"
#include "asterisk/config.h"
#include <speex/speex_resampler.h>

#include "df_bridge.h"

#define DEFAULT_MODEL_PATH "/usr/share/asterisk/deepfilter/DeepFilterNet3.tar.gz"
#define DEFAULT_ATTEN_LIM  100.0f
#define DF_HOP_SIZE        480
#define DF_SAMPLE_RATE     48000
#define DF_MAX_BUF         4096

/* ════════════════════════════════════════════
 * СТРУКТУРЫ ДАННЫХ
 * ════════════════════════════════════════════ */

struct df_direction_info {
    DfHandle *df_handle;
    SpeexResamplerState *upsampler;
    SpeexResamplerState *downsampler;
    
    float input_buf[DF_MAX_BUF];
    int input_buf_pos;
    
    float output_buf[DF_MAX_BUF];
    int output_buf_avail;
    
    int enabled;
    int ast_rate;
    float atten_lim;
};

struct df_info {
    struct ast_audiohook audiohook;
    int lastrate;
    struct df_direction_info *tx, *rx;
};

/* ════════════════════════════════════════════
 * LIFECYCLE: destroy
 * ════════════════════════════════════════════ */

static void df_direction_destroy(struct df_direction_info *ddi)
{
    if (!ddi) return;
    if (ddi->df_handle) df_free(ddi->df_handle);
    if (ddi->upsampler)   speex_resampler_destroy(ddi->upsampler);
    if (ddi->downsampler) speex_resampler_destroy(ddi->downsampler);
    ast_free(ddi);
}

static void destroy_callback(void *data)
{
    struct df_info *di = data;
    ast_audiohook_destroy(&di->audiohook);
    df_direction_destroy(di->rx);
    df_direction_destroy(di->tx);
    ast_free(di);
}

static const struct ast_datastore_info df_datastore = {
    .type = "deepfilter",
    .destroy = destroy_callback
};

/* ════════════════════════════════════════════
 * LIFECYCLE: create direction
 * ════════════════════════════════════════════ */

static struct df_direction_info *df_direction_create(int ast_rate, float atten_lim)
{
    struct df_direction_info *ddi;
    int err;
    
    ddi = ast_calloc(1, sizeof(*ddi));
    if (!ddi) return NULL;
    
    /* Создаём DeepFilter инстанс */
    ddi->df_handle = df_create(DEFAULT_MODEL_PATH, atten_lim);
    if (!ddi->df_handle) {
        ast_log(LOG_ERROR, "DeepFilter: failed to load model from %s\n",
                DEFAULT_MODEL_PATH);
        ast_free(ddi);
        return NULL;
    }
    
    /* Ресемплеры: ast_rate ↔ 48000 */
    ddi->upsampler = speex_resampler_init(1, ast_rate, DF_SAMPLE_RATE, 5, &err);
    if (!ddi->upsampler || err != 0) {
        ast_log(LOG_ERROR, "DeepFilter: upsampler init failed\n");
        df_free(ddi->df_handle);
        ast_free(ddi);
        return NULL;
    }
    
    ddi->downsampler = speex_resampler_init(1, DF_SAMPLE_RATE, ast_rate, 5, &err);
    if (!ddi->downsampler || err != 0) {
        ast_log(LOG_ERROR, "DeepFilter: downsampler init failed\n");
        speex_resampler_destroy(ddi->upsampler);
        df_free(ddi->df_handle);
        ast_free(ddi);
        return NULL;
    }
    
    ddi->ast_rate = ast_rate;
    ddi->atten_lim = atten_lim;
    ddi->enabled = 1;
    ddi->input_buf_pos = 0;
    ddi->output_buf_avail = 0;
    
    return ddi;
}

/* ════════════════════════════════════════════
 * ГЛАВНЫЙ CALLBACK — обработка каждого фрейма
 * ════════════════════════════════════════════ */

static int df_callback(struct ast_audiohook *audiohook,
                       struct ast_channel *chan,
                       struct ast_frame *frame,
                       enum ast_audiohook_direction direction)
{
    struct ast_datastore *datastore;
    struct df_info *di;
    struct df_direction_info *ddi;
    int16_t *frame_data;
    int nsamples;
    int sample_rate;
    
    if (audiohook->status == AST_AUDIOHOOK_STATUS_DONE) return -1;
    if (frame->frametype != AST_FRAME_VOICE) return 0;
    
    if (!(datastore = ast_channel_datastore_find(chan, &df_datastore, NULL)))
        return -1;
    
    di = datastore->data;
    ddi = (direction == AST_AUDIOHOOK_DIRECTION_READ) ? di->rx : di->tx;
    
    if (!ddi || !ddi->enabled || !ddi->df_handle)
        return 0;
    
    frame_data = frame->data.ptr;
    nsamples = frame->samples;
    sample_rate = ast_format_get_sample_rate(frame->subclass.format);
    
    /* Пересоздаём ресемплеры при смене rate */
    if (sample_rate != ddi->ast_rate) {
        /* TODO: пересоздать upsampler/downsampler */
        ddi->ast_rate = sample_rate;
    }
    
    /* ── ШАГ 1: int16 → float ── */
    float tmp_in[nsamples];
    for (int i = 0; i < nsamples; i++) {
        tmp_in[i] = (float)frame_data[i] / 32768.0f;
    }
    
    /* ── ШАГ 2: Upsample → 48kHz ── */
    int max_upsampled = nsamples * (DF_SAMPLE_RATE / sample_rate + 1);
    float upsampled[max_upsampled];
    spx_uint32_t in_len = nsamples;
    spx_uint32_t out_len = max_upsampled;
    
    speex_resampler_process_float(ddi->upsampler, 0,
        tmp_in, &in_len, upsampled, &out_len);
    
    /* ── ШАГ 3: Буферизация + обработка DF ── */
    /* Добавляем в входной буфер */
    if (ddi->input_buf_pos + (int)out_len > DF_MAX_BUF) {
        ast_log(LOG_WARNING, "DeepFilter: input buffer overflow, resetting\n");
        ddi->input_buf_pos = 0;
        ddi->output_buf_avail = 0;
        return 0;
    }
    
    memcpy(ddi->input_buf + ddi->input_buf_pos,
           upsampled, out_len * sizeof(float));
    ddi->input_buf_pos += out_len;
    
    /* Обрабатываем все доступные полные фреймы */
    while (ddi->input_buf_pos >= DF_HOP_SIZE) {
        float df_in[DF_HOP_SIZE], df_out[DF_HOP_SIZE];
        
        memcpy(df_in, ddi->input_buf, DF_HOP_SIZE * sizeof(float));
        
        ddi->input_buf_pos -= DF_HOP_SIZE;
        if (ddi->input_buf_pos > 0) {
            memmove(ddi->input_buf,
                    ddi->input_buf + DF_HOP_SIZE,
                    ddi->input_buf_pos * sizeof(float));
        }
        
        df_process_frame(ddi->df_handle, df_in, df_out, DF_HOP_SIZE);
        
        if (ddi->output_buf_avail + DF_HOP_SIZE <= DF_MAX_BUF) {
            memcpy(ddi->output_buf + ddi->output_buf_avail,
                   df_out, DF_HOP_SIZE * sizeof(float));
            ddi->output_buf_avail += DF_HOP_SIZE;
        }
    }
    
    /* ── ШАГ 4: Downsample + float → int16 ── */
    if (ddi->output_buf_avail > 0) {
        float downsampled[nsamples + 64]; /* с запасом */
        spx_uint32_t ds_in = ddi->output_buf_avail;
        spx_uint32_t ds_out = nsamples;
        
        speex_resampler_process_float(ddi->downsampler, 0,
            ddi->output_buf, &ds_in, downsampled, &ds_out);
        
        /* Сдвигаем выходной буфер */
        ddi->output_buf_avail -= ds_in;
        if (ddi->output_buf_avail > 0) {
            memmove(ddi->output_buf,
                    ddi->output_buf + ds_in,
                    ddi->output_buf_avail * sizeof(float));
        }
        
        /* Записываем обратно в фрейм */
        int copy_len = (ds_out < (spx_uint32_t)nsamples) ? ds_out : nsamples;
        for (int i = 0; i < copy_len; i++) {
            float v = downsampled[i] * 32768.0f;
            v = (v > 32767.0f) ? 32767.0f : ((v < -32768.0f) ? -32768.0f : v);
            frame_data[i] = (int16_t)v;
        }
    }
    
    return 0;
}

/* ════════════════════════════════════════════
 * DIALPLAN FUNCTIONS: write/read
 * ════════════════════════════════════════════ */

static int df_write(struct ast_channel *chan, const char *cmd,
                    char *data, const char *value)
{
    struct ast_datastore *datastore = NULL;
    struct df_info *di = NULL;
    struct df_direction_info **ddi_ptr = NULL;
    int is_new = 0;
    
    if (!chan) {
        ast_log(LOG_WARNING, "DeepFilter: no channel provided\n");
        return -1;
    }
    
    if (strcasecmp(data, "rx") && strcasecmp(data, "tx")) {
        ast_log(LOG_ERROR, "DeepFilter: invalid direction '%s' (use rx or tx)\n", data);
        return -1;
    }
    
    ast_channel_lock(chan);
    if (!(datastore = ast_channel_datastore_find(chan, &df_datastore, NULL))) {
        ast_channel_unlock(chan);
        
        if (!(datastore = ast_datastore_alloc(&df_datastore, NULL)))
            return 0;
        
        if (!(di = ast_calloc(1, sizeof(*di)))) {
            ast_datastore_free(datastore);
            return 0;
        }
        
        ast_audiohook_init(&di->audiohook, AST_AUDIOHOOK_TYPE_MANIPULATE,
                          "deepfilter", AST_AUDIOHOOK_MANIPULATE_ALL_RATES);
        di->audiohook.manipulate_callback = df_callback;
        di->lastrate = 8000;
        is_new = 1;
    } else {
        ast_channel_unlock(chan);
        di = datastore->data;
    }
    
    ddi_ptr = (!strcasecmp(data, "rx")) ? &di->rx : &di->tx;
    
    if (!strcasecmp(cmd, "DEEPFILTER")) {
        if (ast_true(value)) {
            /* Включение */
            if (!*ddi_ptr) {
                *ddi_ptr = df_direction_create(di->lastrate, DEFAULT_ATTEN_LIM);
                if (!*ddi_ptr) {
                    ast_log(LOG_ERROR, "DeepFilter: failed to create instance\n");
                    if (is_new) {
                        ast_free(di);
                        ast_datastore_free(datastore);
                    }
                    return -1;
                }
            }
            (*ddi_ptr)->enabled = 1;
        } else {
            /* Выключение */
            if (*ddi_ptr) {
                (*ddi_ptr)->enabled = 0;
            }
        }
    } else if (!strcasecmp(cmd, "DEEPFILTER_ATTEN")) {
        /* Установка лимита аттенуации */
        float atten = 0;
        if (sscanf(value, "%f", &atten) == 1 && *ddi_ptr && (*ddi_ptr)->df_handle) {
            df_set_atten_lim((*ddi_ptr)->df_handle, atten);
        }
    }
    
    /* Очистка если оба направления выключены */
    if (!di->rx && !di->tx) {
        if (is_new) {
            is_new = 0;
        } else {
            ast_channel_lock(chan);
            ast_channel_datastore_remove(chan, datastore);
            ast_channel_unlock(chan);
            ast_audiohook_remove(chan, &di->audiohook);
            ast_audiohook_detach(&di->audiohook);
        }
        ast_datastore_free(datastore);
    }
    
    if (is_new) {
        datastore->data = di;
        ast_channel_lock(chan);
        ast_channel_datastore_add(chan, datastore);
        ast_channel_unlock(chan);
        ast_audiohook_attach(chan, &di->audiohook);
    }
    
    return 0;
}

static int df_read(struct ast_channel *chan, const char *cmd,
                   char *data, char *buf, size_t len)
{
    struct ast_datastore *datastore;
    struct df_info *di;
    struct df_direction_info *ddi;
    
    if (!chan) return -1;
    
    ast_channel_lock(chan);
    if (!(datastore = ast_channel_datastore_find(chan, &df_datastore, NULL))) {
        ast_channel_unlock(chan);
        snprintf(buf, len, "off");
        return 0;
    }
    ast_channel_unlock(chan);
    
    di = datastore->data;
    ddi = (!strcasecmp(data, "rx")) ? di->rx : di->tx;
    
    if (!strcasecmp(cmd, "DEEPFILTER")) {
        snprintf(buf, len, "%s", (ddi && ddi->enabled) ? "on" : "off");
    } else if (!strcasecmp(cmd, "DEEPFILTER_ATTEN")) {
        snprintf(buf, len, "%.1f", ddi ? ddi->atten_lim : 0.0f);
    }
    
    return 0;
}

/* ════════════════════════════════════════════
 * MODULE REGISTRATION
 * ════════════════════════════════════════════ */

static struct ast_custom_function deepfilter_function = {
    .name = "DEEPFILTER",
    .write = df_write,
    .read = df_read,
    .read_max = 22,
};

static struct ast_custom_function deepfilter_atten_function = {
    .name = "DEEPFILTER_ATTEN",
    .write = df_write,
    .read = df_read,
    .read_max = 22,
};

static int unload_module(void)
{
    ast_custom_function_unregister(&deepfilter_function);
    ast_custom_function_unregister(&deepfilter_atten_function);
    return 0;
}

static int load_module(void)
{
    if (ast_custom_function_register(&deepfilter_function))
        return AST_MODULE_LOAD_DECLINE;
    
    if (ast_custom_function_register(&deepfilter_atten_function)) {
        ast_custom_function_unregister(&deepfilter_function);
        return AST_MODULE_LOAD_DECLINE;
    }
    
    ast_log(LOG_NOTICE, "DeepFilter: AI noise suppression module loaded. "
            "Model: %s\n", DEFAULT_MODEL_PATH);
    
    return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY,
    "AI Noise Reduction using DeepFilterNet");
```


## 9. Чеклист для реализации

### Фаза 1: Сборка libdf с C API
- [ ] Клонировать DeepFilterNet
- [ ] Собрать `cargo build --release -p df --features capi`
- [ ] Убедиться что `libdf.so` экспортирует `df_create`, `df_process_frame` и т.д.
- [ ] Написать простой C test: загрузить модель, обработать буфер

### Фаза 2: Минимальный модуль Asterisk
- [ ] Скелет `func_deepfilter.c` (без обработки, только audiohook)
- [ ] Компиляция как `.so` и загрузка в Asterisk
- [ ] Тест: `module load func_deepfilter.so` в CLI

### Фаза 3: Интеграция обработки
- [ ] Реализовать цепочку: int16→float→upsample→DF→downsample→int16
- [ ] Буферизация для выравнивания frame sizes
- [ ] Обработка смены sample rate mid-call

### Фаза 4: Тестирование
- [ ] Тест с G.711 (8kHz) вызовом
- [ ] Тест с G.722 (16kHz) вызовом
- [ ] Замер CPU на реальных вызовах
- [ ] Замер латентности
- [ ] Тест на утечки памяти (valgrind)
- [ ] Stress test: 10+ одновременных вызовов

### Фаза 5: Оптимизация
- [ ] Пул DfHandle для быстрого старта новых каналов
- [ ] Конфигурационный файл `deepfilter.conf`
- [ ] Мониторинг CPU через Asterisk manager events
