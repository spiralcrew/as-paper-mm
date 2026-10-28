# AGENTS.md

Инструкции для ИИ-агентов (Claude Code, Codex, Cursor, OpenCode и др.), работающих с этим репозиторием.
Claude Code подключает этот файл через `CLAUDE.md` (`@AGENTS.md`).

## Назначение проекта

`as-paper-mm` — C++20 маркет-мейкер по модели Avellaneda–Stoikov (2008) на живых данных
Binance Spot BTCUSDT (WebSocket `btcusdt@bookTicker` + `btcusdt@trade`) с **симуляцией
исполнения** (paper trading, без реальных денег и ордеров). Учебный проект курса по C++:
сборка, тесты, бенчмарк и документация автоматизированы в GitHub Actions (CI/CD) и GitHub Pages.

## Структура

| Путь | Что там |
|---|---|
| `include/asmm/*.hpp` | публичный API библиотеки `asmm_core` (Doxygen-комментарии обязательны) |
| `src/*.cpp` | реализация: `avellaneda_stoikov`, `volatility`, `paper_exchange`, `binance_parser`, `engine` |
| `app/main.cpp` | CLI `asmm`: читает JSON Lines из stdin/файла |
| `tests/` | GoogleTest, по файлу `test_<модуль>.cpp` на модуль |
| `bench/` | Google Benchmark (`bench_main.cpp`) |
| `scripts/binance_feed.py` | стример Binance → stdout |
| `data/sample_btcusdt.jsonl` | записанные 90 с реальных данных; используются тестами и бенчмарком |
| `docs/` | описание модели, решений, ограничений |
| `.github/workflows/ci.yml`, `Doxyfile` | CI/CD и документация |

## Команды

```bash
# сборка (Release, с тестами и бенчмарком)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DASMM_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel

# тесты
ctest --test-dir build --output-on-failure

# бенчмарк
./build/bench/asmm_bench

# запуск на записанных данных / на живых данных
./build/asmm --input data/sample_btcusdt.jsonl
python scripts/binance_feed.py | ./build/asmm

# документация
mkdir -p build/doxygen && doxygen Doxyfile   # → build/doxygen/html
```

## Правила оформления кода

- C++20, без расширений компилятора; сборка без предупреждений с `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`.
- Форматирование — `.clang-format` (Google, отступ 4, ширина 100): `clang-format -i <файлы>`.
- Пространство имён `asmm`; типы `PascalCase`, функции и переменные `snake_case`,
  члены класса с суффиксом `_`, константы `kPascalCase`.
- Каждый публичный тип и функция в `include/` — с Doxygen-комментарием (`@brief`, единицы измерения, исключения).
- Единицы измерения явно: цены в USDT, объёмы в BTC, время в мс (`*_ms`) или с (`*_sec`), σ в USDT/√s.
- Ядро (`asmm_core`) без сетевого I/O и без внешних зависимостей; сеть — только в `scripts/`.
- Неверные параметры — `std::invalid_argument`; горячий путь (`parse_binance_message`, `on_book`) без аллокаций.
- Новая логика — с unit-тестом в `tests/`; изменения горячего пути — с бенчмарком в `bench/`.
- Коммиты: `feat:`, `fix:`, `refactor:`, `test:`, `docs:`, `chore:`, `ci:`.

## Ограничения для агента

- **Перед завершением задачи собери проект и запусти тесты** (команды выше); все тесты должны проходить.
- **Не меняй CI/CD** (`.github/workflows/`) и `Doxyfile` без явного запроса.
- Не добавляй торговлю реальными деньгами, приватные эндпоинты Binance, API-ключи и секреты в репозиторий.
- Не перезаписывай `data/sample_btcusdt.jsonl` без запроса: от него зависят тесты и бенчмарк.
- Не добавляй зависимости, кроме подключаемых через CMake `FetchContent` с фиксированной версией.
- Не ослабляй и не удаляй существующие тесты, чтобы «починить» сборку.
- Формулы модели меняй только со ссылкой на источник и обновлением `docs/model.md`.
- При изменении поведения обновляй `README.md` и/или `docs/`.
