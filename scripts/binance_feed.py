#!/usr/bin/env python3
"""Стример публичных рыночных данных Binance Spot в stdout (JSON Lines) для `asmm`.

Подключается к combined stream Binance Spot:
    wss://stream.binance.com:9443/stream?streams=btcusdt@bookTicker/btcusdt@trade
и печатает каждое сообщение одной строкой, добавляя поле верхнего уровня
`recv_ms` — локальное время получения (у bookTicker в споте своего времени нет).

Ключи API не нужны: стримы публичные. Документация:
https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams

Примеры:
    python scripts/binance_feed.py | ./build/asmm
    python scripts/binance_feed.py --duration 60 > data/sample_btcusdt.jsonl
"""

from __future__ import annotations

import argparse
import asyncio
import sys
import time

import websockets

DEFAULT_BASE_URL = "wss://stream.binance.com:9443"


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--symbol", default="btcusdt", help="символ в нижнем регистре (default: btcusdt)")
    p.add_argument("--base-url", default=DEFAULT_BASE_URL, help=f"WebSocket endpoint (default: {DEFAULT_BASE_URL})")
    p.add_argument("--duration", type=float, default=0.0, help="секунд записи, 0 = бесконечно")
    return p.parse_args()


async def stream(args: argparse.Namespace) -> None:
    sym = args.symbol.lower()
    url = f"{args.base_url}/stream?streams={sym}@bookTicker/{sym}@trade"
    deadline = time.monotonic() + args.duration if args.duration > 0 else None
    out = sys.stdout
    while True:
        try:
            async with websockets.connect(url, ping_interval=20, max_queue=None) as ws:
                print(f"connected: {url}", file=sys.stderr)
                async for raw in ws:
                    recv_ms = time.time_ns() // 1_000_000
                    # Вставляем recv_ms первым полем, не перепарсивая JSON.
                    out.write('{"recv_ms":%d,%s\n' % (recv_ms, raw[1:]))
                    out.flush()
                    if deadline is not None and time.monotonic() >= deadline:
                        return
        except (OSError, websockets.ConnectionClosed) as exc:
            # Binance рвёт соединение раз в 24 часа и при сетевых сбоях — переподключаемся.
            print(f"connection lost ({exc}), reconnecting in 1s", file=sys.stderr)
            await asyncio.sleep(1)


def main() -> int:
    args = parse_args()
    try:
        asyncio.run(stream(args))
    except (KeyboardInterrupt, BrokenPipeError):
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
