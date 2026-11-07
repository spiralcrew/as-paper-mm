#!/usr/bin/env python3
"""Сборщик публичных рыночных данных Binance Spot для `asmm` (JSON Lines).

Подключается к combined stream Binance Spot

    wss://stream.binance.com:9443/stream?streams=
        btcusdt@bookTicker/btcusdt@trade/btcusdt@depth@100ms

и печатает каждое сообщение одной строкой, добавляя поле `recv_ms` — локальное время
получения (у bookTicker в споте своего времени нет).

Локальная книга заявок строится по правилам Binance
(https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams,
«How to manage a local order book correctly»): после подключения обновления стакана
буферизуются, запрашивается снимок REST `GET /api/v3/depth`, в поток пишется снимок
(stream `<symbol>@depthSnapshot`) и затем буферизованные обновления с `u > lastUpdateId`.
При разрыве последовательности (`U > u_prev + 1`), переподключении и ротации файла
снимок запрашивается заново, поэтому каждый файл записи самодостаточен.

Ключи API не нужны: все данные публичные.

Примеры:
    python scripts/binance_feed.py | ./build/asmm                      # живой paper trading
    python scripts/binance_feed.py --record data/raw | ./build/asmm    # + запись на диск
    python scripts/binance_feed.py --duration 60 --no-stdout --record data/raw
"""

from __future__ import annotations

import argparse
import asyncio
import gzip
import json
import sys
import time
import urllib.request
from pathlib import Path
from typing import IO

import websockets

DEFAULT_WS_URL = "wss://stream.binance.com:9443"
DEFAULT_REST_URL = "https://api.binance.com"


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--symbol", default="btcusdt", help="символ (default: btcusdt)")
    p.add_argument("--ws-url", default=DEFAULT_WS_URL, help=f"WebSocket endpoint (default: {DEFAULT_WS_URL})")
    p.add_argument("--rest-url", default=DEFAULT_REST_URL, help=f"REST endpoint (default: {DEFAULT_REST_URL})")
    p.add_argument("--depth-limit", type=int, default=1000, help="уровней в снимке стакана (default: 1000)")
    p.add_argument("--no-depth", action="store_true", help="без стакана: только bookTicker и сделки")
    p.add_argument("--duration", type=float, default=0.0, help="секунд работы, 0 = бесконечно")
    p.add_argument("--record", type=Path, help="каталог для записи файлов <symbol>_YYYYmmdd_HHMMSS.jsonl[.gz] (UTC)")
    p.add_argument("--rotate-min", type=float, default=60.0, help="период ротации файлов, минут (default: 60)")
    p.add_argument("--gzip", action="store_true", help="сжимать файлы записи")
    p.add_argument("--no-stdout", action="store_true", help="не писать в stdout (только запись)")
    return p.parse_args()


def log(msg: str) -> None:
    print(f"[binance_feed] {msg}", file=sys.stderr, flush=True)


class Sink:
    """Вывод в stdout и/или ротируемые файлы."""

    def __init__(self, args: argparse.Namespace) -> None:
        self.stdout = not args.no_stdout
        self.dir: Path | None = args.record
        self.symbol = args.symbol.lower()
        self.rotate_sec = args.rotate_min * 60.0
        self.gzip = args.gzip
        self.file: IO[str] | None = None
        self.opened_at = 0.0
        self.lines = 0
        if self.dir is not None:
            self.dir.mkdir(parents=True, exist_ok=True)

    def need_rotation(self) -> bool:
        """Нужен ли новый файл (вызывающий должен тогда запросить свежий снимок)."""
        if self.dir is None:
            return False
        return self.file is None or time.time() - self.opened_at >= self.rotate_sec

    def rotate(self) -> None:
        if self.dir is None:
            return
        self.close()
        name = f"{self.symbol}_{time.strftime('%Y%m%d_%H%M%S', time.gmtime())}.jsonl"
        path = self.dir / (name + ".gz" if self.gzip else name)
        # Файл живёт до ротации, поэтому без контекстного менеджера; закрывается в close().
        if self.gzip:
            self.file = gzip.open(path, "at", encoding="utf-8")  # noqa: SIM115
        else:
            self.file = open(path, "a", encoding="utf-8")  # noqa: SIM115
        self.opened_at = time.time()
        log(f"recording to {path}")

    def write(self, line: str) -> None:
        self.lines += 1
        if self.stdout:
            sys.stdout.write(line + "\n")
            sys.stdout.flush()
        if self.file is not None:
            self.file.write(line + "\n")

    def close(self) -> None:
        if self.file is not None:
            self.file.close()
            self.file = None


def fetch_snapshot(rest_url: str, symbol: str, limit: int) -> str:
    url = f"{rest_url}/api/v3/depth?symbol={symbol.upper()}&limit={limit}"
    with urllib.request.urlopen(url, timeout=10) as resp:
        return resp.read().decode("utf-8")


class Stats:
    """Счётчики за всё время работы (по всем соединениям)."""

    snapshots = 0
    gaps = 0
    reconnects = 0


class DepthSync:
    """Буферизация обновлений стакана до снимка и контроль непрерывности `U`/`u`."""

    def __init__(self, args: argparse.Namespace, sink: Sink) -> None:
        self.args = args
        self.sink = sink
        self.symbol = args.symbol.lower()
        self.buffer: list[tuple[int, int, str]] = []
        self.pending: asyncio.Future | None = None
        self.synced = False
        self.last_u = 0

    def request_snapshot(self) -> None:
        self.synced = False
        if self.pending is None:
            loop = asyncio.get_running_loop()
            self.pending = loop.run_in_executor(
                None, fetch_snapshot, self.args.rest_url, self.symbol, self.args.depth_limit
            )

    def on_update(self, first_u: int, final_u: int, line: str) -> None:
        if self.synced:
            if first_u > self.last_u + 1:
                Stats.gaps += 1
                log(f"depth gap: U={first_u} > last u+1={self.last_u + 1}, resnapshot")
                self.buffer = [(first_u, final_u, line)]
                self.request_snapshot()
                return
            if final_u <= self.last_u:
                return
            self.last_u = final_u
            self.sink.write(line)
            return
        self.buffer.append((first_u, final_u, line))
        if self.pending is None:
            self.request_snapshot()

    def poll(self) -> None:
        """Если снимок готов — записать его и буфер."""
        if self.pending is None or not self.pending.done():
            return
        future, self.pending = self.pending, None
        try:
            raw = future.result()
        except Exception as exc:  # noqa: BLE001 - сеть: повторяем
            log(f"snapshot failed: {exc}; retrying")
            self.request_snapshot()
            return
        last_id = json.loads(raw)["lastUpdateId"]
        if self.buffer and self.buffer[0][0] > last_id + 1:
            log("snapshot older than buffered updates, refetching")
            self.request_snapshot()
            return
        recv_ms = time.time_ns() // 1_000_000
        self.sink.write(f'{{"recv_ms":{recv_ms},"stream":"{self.symbol}@depthSnapshot","data":{raw.strip()}}}')
        Stats.snapshots += 1
        self.last_u = last_id
        for first_u, final_u, line in self.buffer:
            if final_u <= self.last_u:
                continue
            if first_u > self.last_u + 1:
                log("gap inside buffer, refetching snapshot")
                self.buffer = [(f, u, l) for f, u, l in self.buffer if u > self.last_u]
                self.request_snapshot()
                return
            self.sink.write(line)
            self.last_u = final_u
        self.buffer.clear()
        self.synced = True


async def stream(args: argparse.Namespace, sink: Sink) -> None:
    sym = args.symbol.lower()
    streams = [f"{sym}@bookTicker", f"{sym}@trade"] + ([] if args.no_depth else [f"{sym}@depth@100ms"])
    url = f"{args.ws_url}/stream?streams={'/'.join(streams)}"
    deadline = time.monotonic() + args.duration if args.duration > 0 else None
    depth_suffix = f"{sym}@depth@100ms"
    backoff = 1.0
    while True:
        depth = DepthSync(args, sink)
        try:
            async with websockets.connect(url, ping_interval=20, max_queue=None) as ws:
                log(f"connected: {url}")
                backoff = 1.0
                async for raw in ws:
                    recv_ms = time.time_ns() // 1_000_000
                    if sink.need_rotation():
                        sink.rotate()
                        if not args.no_depth:
                            depth.request_snapshot()
                    line = f'{{"recv_ms":{recv_ms},{raw[1:]}'
                    if not args.no_depth and f'"stream":"{depth_suffix}"' in raw[:64]:
                        data = json.loads(raw)["data"]
                        depth.on_update(int(data["U"]), int(data["u"]), line)
                    else:
                        sink.write(line)
                    if not args.no_depth:
                        depth.poll()
                    if deadline is not None and time.monotonic() >= deadline:
                        log(
                            f"done: {sink.lines} lines, {Stats.snapshots} snapshots, "
                            f"{Stats.gaps} gaps, {Stats.reconnects} reconnects"
                        )
                        return
        except (OSError, websockets.ConnectionClosed) as exc:
            # Binance рвёт соединение раз в 24 часа и при сетевых сбоях — переподключаемся
            # с экспоненциальной паузой (1, 2, 4 ... 30 с). Новое соединение начинается с
            # нового снимка стакана (DepthSync создаётся заново).
            Stats.reconnects += 1
            log(f"connection lost ({exc}), reconnecting in {backoff:.0f}s")
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2.0, 30.0)
            if deadline is not None and time.monotonic() >= deadline:
                return


def main() -> int:
    args = parse_args()
    if args.no_stdout and args.record is None:
        log("nothing to do: --no-stdout without --record")
        return 2
    sink = Sink(args)
    try:
        asyncio.run(stream(args, sink))
    except (KeyboardInterrupt, BrokenPipeError):
        pass
    finally:
        sink.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
