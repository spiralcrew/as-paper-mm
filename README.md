# AS Paper MM — Avellaneda–Stoikov market making на живых данных Binance

[![CI](https://github.com/spiralcrew/as-paper-mm/actions/workflows/ci.yml/badge.svg)](https://github.com/spiralcrew/as-paper-mm/actions/workflows/ci.yml)
[![docs](https://img.shields.io/badge/docs-Doxygen-blue)](https://spiralcrew.github.io/as-paper-mm/)

**Документация (Doxygen, GitHub Pages): https://spiralcrew.github.io/as-paper-mm/**

## Что делает программа

`asmm` — маркет-мейкер на C++20, который в реальном времени получает рыночные данные
**BTC/USDT со спота Binance** и котирует по модели **Avellaneda–Stoikov (2008)**.
Исполнение заявок **симулируется** (paper trading): реальных денег, ключей API и ордеров нет.

Конвейер на каждом сообщении:

```
Binance WebSocket ──► scripts/binance_feed.py ──stdin──► asmm
 (bookTicker+trade)    (+ recv_ms, JSON Lines)            │
                                                         ├─ парсер Binance JSON
                                                         ├─ EWMA-волатильность σ
                                                         ├─ котировки AS: r, спред, bid/ask
                                                         ├─ округление к тику, post-only, лимит |q|
                                                         └─ виртуальная биржа: исполнение по ленте
                                                            сделок, позиция, кэш, PnL
```

### Откуда данные

Публичный combined WebSocket-стрим **Binance Spot** (ключи не нужны):

```
wss://stream.binance.com:9443/stream?streams=btcusdt@bookTicker/btcusdt@trade
```

- `btcusdt@bookTicker` — лучшие бид/аск в реальном времени;
- `btcusdt@trade` — лента сделок (цена, объём, сторона агрессора).

Документация API: <https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams>.
На Binance.com нет спот-пары BTC/USD — долларовая цена BTC берётся из **BTCUSDT**
(самая ликвидная пара). Фидер поддерживает `--symbol`/`--base-url` (например, Binance.US `btcusd`).

### Модель

```
r      = s − q·γ·σ²·τ                       резервационная цена
spread = γ·σ²·τ + (2/γ)·ln(1 + γ/k)          оптимальный спред
bid    = r − spread/2,   ask = r + spread/2
```

`s` — mid, `q` — инвентарь в лотах, `γ` — неприятие риска, `σ` — волатильность (USDT/√s),
`τ` — время до конца сессии, `k` — затухание интенсивности исполнения.
Симуляция исполнения: наш бид исполняется сделкой агрессора-продавца по цене ≤ бида
(`touch`) или < бида (`through`), аск — симметрично. Подробно, с ограничениями и
литературой: [docs/model.md](docs/model.md).

## Структура

| Путь | Содержимое |
|---|---|
| `include/asmm/` | публичные заголовки библиотеки `asmm_core` |
| `src/` | реализация: модель AS, волатильность, виртуальная биржа, парсер, движок |
| `app/` | CLI `asmm` (`main.cpp`) |
| `tests/` | unit-тесты (GoogleTest) |
| `bench/` | бенчмарки (Google Benchmark) |
| `scripts/` | `binance_feed.py` — стример Binance → stdout |
| `data/` | записанные 90 с BTCUSDT для тестов, бенчмарка и реплея |
| `docs/` | описание модели и решений |
| `.github/workflows/ci.yml` | CI/CD (GitHub Actions): build → test → benchmark → Pages |
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

Живые данные (нужен Python 3.9+):

```bash
pip install -r scripts/requirements.txt
python scripts/binance_feed.py | ./build/asmm
```

Реплей записанных данных:

```bash
./build/asmm --input data/sample_btcusdt.jsonl
```

Параметры: `./build/asmm --help` (`--gamma`, `--k`, `--horizon`, `--qty`, `--max-inv`,
`--fill-model touch|through`, `--fee`, `--report`, ...). Пример вывода:

```
FILL SELL 0.001000 BTC @ 80568.56 (fee 0.000000) | q=-1.21 lots
[1793198736194] mid=80596.01 sigma=2.432 tau=14.6s r=80596.15 bid=80595.60 ask=80596.70 q=-1.67 lots pnl=-0.0275 fills=17
=== Paper trading summary (no real funds) ===
```

Записать свой сэмпл: `python scripts/binance_feed.py --duration 90 > data/sample_btcusdt.jsonl`.

## Тесты

```bash
ctest --test-dir build --output-on-failure
```

## Бенчмарк

```bash
./build/bench/asmm_bench
```

Меряет `compute_quotes`, округление котировок, парсинг `bookTicker`/`trade`, обработку
одного обновления стакана движком и полный реплей записанного сэмпла.

## Документация

```bash
mkdir -p build/doxygen && mkdir -p build/doxygen && doxygen Doxyfile   # → build/doxygen/html/index.html
```

## CI/CD

GitHub Actions ([.github/workflows/ci.yml](.github/workflows/ci.yml)) на каждый push и pull request:

| Job / шаг | Что делает |
|---|---|
| `Build, test, benchmark` → Configure, Build | CMake + GCC, Release, `-Werror` |
| → Unit tests | `ctest` (GoogleTest), JUnit-отчёт в артефактах |
| → Replay smoke run | прогон `asmm` на записанных данных Binance |
| → Benchmark | Google Benchmark; результат в журнале шага и в Summary запуска, `benchmark.json` в артефактах |
| `Doxygen docs` | генерация HTML-документации |
| `Deploy docs to GitHub Pages` | публикация на GitHub Pages (только ветка `main`) |

## Лицензия

MIT, см. [LICENSE](LICENSE).
