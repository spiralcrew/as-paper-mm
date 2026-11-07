# AS Paper MM — Avellaneda–Stoikov market making на живых данных Binance

[![CI](https://github.com/spiralcrew/as-paper-mm/actions/workflows/ci.yml/badge.svg)](https://github.com/spiralcrew/as-paper-mm/actions/workflows/ci.yml)
[![docs](https://img.shields.io/badge/docs-Doxygen-blue)](https://spiralcrew.github.io/as-paper-mm/)

**Документация (Doxygen, GitHub Pages): https://spiralcrew.github.io/as-paper-mm/**

## Что делает программа

`asmm` — маркет-мейкер на C++20, который в реальном времени получает рыночные данные
**BTC/USDT со спота Binance** (лучшие цены, сделки, полный стакан) и котирует по модели
**Avellaneda–Stoikov (2008)**. Исполнение заявок **симулируется** (paper trading): реальных
денег, ключей API и ордеров нет. Симулятор учитывает место в очереди уровня, отмены чужих
заявок, задержку выставления/отмены и проскальзывание рыночных заявок по стакану.

```
Binance Spot ── WebSocket bookTicker + trade + depth@100ms ─┐
             └─ REST /api/v3/depth (снимок) ─────────────────┤
                                                             ▼
            scripts/binance_feed.py  (recv_ms, синхронизация стакана, запись в файлы)
                                                             │ JSON Lines (stdin / файл)
                                                             ▼
 asmm ─ парсер ─► локальная книга L2 ─────────────┐
                ─► EWMA σ                          │
                ─► калибровка λ(δ) = A·e^(−kδ) ─► котировки AS: r, спред, bid/ask
                                                   │  тик, post-only, лимит |q|, гистерезис
                                                   ▼
               виртуальная биржа: очередь FIFO, отмены, задержка, сделки → исполнения,
               рыночная ликвидация с проскальзыванием, позиция, кэш, PnL
```

### Откуда данные

Публичные стримы **Binance Spot**, ключи не нужны:

```
wss://stream.binance.com:9443/stream?streams=btcusdt@bookTicker/btcusdt@trade/btcusdt@depth@100ms
https://api.binance.com/api/v3/depth?symbol=BTCUSDT&limit=1000
```

- `btcusdt@bookTicker` — лучшие бид/аск в реальном времени;
- `btcusdt@trade` — лента сделок (цена, объём, сторона агрессора);
- `btcusdt@depth@100ms` + REST-снимок — полная книга заявок по правилам Binance
  («How to manage a local order book correctly»).

Документация API: <https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams>.
На Binance.com нет спот-пары BTC/USD — долларовая цена BTC берётся из **BTCUSDT**
(самая ликвидная пара). Сборщик поддерживает `--symbol`, `--ws-url`, `--rest-url`.

### Модель (Avellaneda & Stoikov, 2008)

```
r      = s − q·γ·σ²·τ                     резервационная цена      (3.17)
spread = γ·σ²·τ + (2/γ)·ln(1 + γ/k)        оптимальный спред        (3.18)
bid    = r − spread/2,   ask = r + spread/2
λ(δ)   = A·e^(−kδ)                         интенсивность исполнения (3.8)
```

`s` — mid, `q` — инвентарь в лотах, `γ` — неприятие риска, `σ` — волатильность (EWMA, USDT/√s),
`τ` — время до конца сессии, `k`, `A` — калибруются онлайн по ленте сделок.
`asmm_sim` воспроизводит численный эксперимент статьи (§3.3, таблицы 1–3); результаты
совпадают со статьёй в пределах шума Монте-Карло, кроме одной ячейки, — подробно, с
таблицей сравнения, ограничениями и литературой: **[docs/model.md](docs/model.md)**.

### Симуляция исполнения

| Эффект | Как моделируется |
|---|---|
| Очередь | встаём в хвост видимого объёма уровня; сделки по нашей цене сначала съедают очередь перед нами |
| Пробой уровня | сделка хуже нашей цены — уровень исчерпан, мы исполнены полностью |
| Отмены чужих заявок | уменьшение уровня без сделок двигает нас вперёд пропорционально (`proportional`) или не двигает (`pessimistic`) |
| Задержка | заявка встаёт и снимается через `--latency-ms`; до этого старая котировка может быть исполнена |
| Проскальзывание | рыночная ликвидация проходит по уровням стакана: VWAP, число уровней, `|VWAP − mid|·qty`, комиссия тейкера |
| Сохранение очереди | заявка не переставляется, если цена сдвинулась меньше чем на `--requote-ticks` |

## Структура

| Путь | Содержимое |
|---|---|
| `include/asmm/` | публичные заголовки библиотеки `asmm_core` |
| `src/` | реализация: модель AS, волатильность, калибровка λ(δ), книга L2, виртуальная биржа, парсер, движок, эксперимент статьи |
| `app/` | CLI `asmm` (paper trading) и `asmm_sim` (эксперимент статьи) |
| `tests/` | unit-тесты (GoogleTest) |
| `bench/` | бенчмарки (Google Benchmark) |
| `scripts/` | `binance_feed.py` — сборщик данных Binance |
| `data/` | 5 минут реальных данных BTCUSDT со стаканом — для тестов, бенчмарка и реплея |
| `docs/` | описание модели, калибровки, симуляции исполнения и проверки |
| `.github/workflows/ci.yml` | CI/CD (GitHub Actions): build → test → replay → paper reproduction → benchmark → Pages |
| `Doxyfile` | конфигурация Doxygen |
| `AGENTS.md`, `CLAUDE.md` | инструкции для ИИ-агентов |

## Сборка

Требования: CMake ≥ 3.25, компилятор C++20 (GCC 12+, Clang 16+, MSVC 19.34+), интернет при
первой конфигурации (CMake `FetchContent` скачивает GoogleTest и Google Benchmark).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Опции CMake: `-DASMM_BUILD_TESTS=OFF`, `-DASMM_BUILD_BENCHMARKS=OFF`, `-DASMM_WARNINGS_AS_ERRORS=ON`.

## Запуск

Сборщику нужен Python 3.9+:

```bash
pip install -r scripts/requirements.txt
```

Живой paper trading:

```bash
python scripts/binance_feed.py | ./build/asmm --latency-ms 50
```

Живой paper trading с записью данных (файлы ротируются раз в час, каждый начинается со снимка стакана):

```bash
python scripts/binance_feed.py --record data/raw | ./build/asmm --fills-csv fills.csv --state-csv state.csv
```

Только сбор данных и реплей записанного файла:

```bash
python scripts/binance_feed.py --no-stdout --record data/raw --gzip
./build/asmm --input data/sample_btcusdt.jsonl
```

Эксперимент из статьи:

```bash
./build/asmm_sim
```

Основные параметры `asmm` (полный список — `./build/asmm --help`):

| Параметр | По умолчанию | Смысл |
|---|---|---|
| `--gamma` | 0.001 | неприятие риска γ, 1/USDT |
| `--horizon` | 30 | длина сессии T, с |
| `--k`, `--no-calibrate` | 2.0, калибровка вкл. | k до калибровки / зафиксировать k |
| `--qty`, `--max-inv` | 0.001 BTC, 5 лотов | размер лота и лимит инвентаря |
| `--fill-model` | queue | `queue`, `touch`, `through` |
| `--cancel-model` | proportional | `proportional`, `pessimistic` |
| `--latency-ms` | 0 | задержка наших заявок |
| `--requote-ticks` | 1 | гистерезис перестановки |
| `--fee`, `--taker-fee` | 0, 0.001 | комиссии мейкера и тейкера |
| `--liquidate-session` | выкл. | закрывать позицию в конце каждой сессии |

## Тесты

```bash
ctest --test-dir build --output-on-failure
```

Покрывают формулы AS, калибровку λ(δ) на синтетике с известными A и k, синхронизацию книги
(включая разрывы), очередь, отмены, задержку, проскальзывание, движок. На реальных данных
проверяется, что локальная книга совпадает с bookTicker во всех точках с одинаковым `update_id`.
Таблица 1 статьи воспроизводится с допусками.

## Бенчмарк

```bash
./build/bench/asmm_bench
```

Меряет котировки AS, парсинг `bookTicker`/`trade`/`depthUpdate`, применение обновления к книге
из 1000 уровней, обработку сделки моделью очереди, движок на одном обновлении, полный реплей
сэмпла и эксперимент статьи (1000 траекторий).

## Документация

```bash
mkdir -p build/doxygen && doxygen Doxyfile   # → build/doxygen/html/index.html
```

## CI/CD

GitHub Actions ([.github/workflows/ci.yml](.github/workflows/ci.yml)) на каждый push и pull request:

| Job / шаг | Что делает |
|---|---|
| `Build, test, benchmark` → Configure, Build | CMake + GCC, Release, `-Werror` |
| → Unit tests | `ctest` (GoogleTest), JUnit-отчёт в артефактах |
| → Paper trading replay | прогон `asmm` на записанных данных Binance (очередь, задержка 50 мс) |
| → Avellaneda-Stoikov paper reproduction | `asmm_sim`: таблицы 1–3 статьи |
| → Benchmark | Google Benchmark; результат в журнале шага и в Summary запуска, `benchmark.json` в артефактах |
| `Doxygen docs` | генерация HTML-документации |
| `Deploy docs to GitHub Pages` | публикация на GitHub Pages (только ветка `main`) |

## Лицензия

MIT, см. [LICENSE](LICENSE).
