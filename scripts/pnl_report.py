#!/usr/bin/env python3
"""
pnl_report.py — daily P&L report generator.

Reads the binary trade log emitted by TradeLogger::flush_binary() and
produces a compliance-friendly CSV plus a short human summary.

Usage:
    python pnl_report.py <trade_log.bin> [--out pnl.csv] [--summary]
"""
from __future__ import annotations

import argparse
import struct
import sys
from collections import defaultdict
from pathlib import Path

# Mirror of hft::telemetry::TradeEvent layout — keep in sync with
# telemetry/trade_logger.h
#
#   TimestampNs     timestamp;      // uint64
#   TradeEventType  type;           // uint8
#   Side            side;           // uint8
#   InstrumentId    instrument_id;  // uint16
#   OrderId         cl_ord_id;      // uint64
#   OrderId         order_id;       // uint64
#   Price           price;          // int64
#   Quantity        quantity;       // int64
#   Quantity        filled_qty;     // int64
#   Quantity        leaves_qty;     // int64
#   VenueId         venue;          // uint8
#   pad[67]
#
# Total 128 bytes with trailing pad.
RECORD_SIZE = 128
FIELDS_FMT = "<QBBHQQqqqqB"   # 8+1+1+2+8+8+8+8+8+8+1 = 61 bytes
# Remaining 67 bytes are padding.

EVENT_NAMES = {
    1: "ORDER_SENT",
    2: "ORDER_ACK",
    3: "ORDER_FILLED",
    4: "ORDER_PARTIAL",
    5: "ORDER_CANCELLED",
    6: "ORDER_REJECTED",
    7: "ORDER_REPLACED",
}

SIDE_NAMES = {0: "UNKNOWN", 1: "BUY", 2: "SELL"}


def parse(path: Path):
    data = path.read_bytes()
    if len(data) % RECORD_SIZE != 0:
        raise ValueError(
            f"{path}: length {len(data)} not a multiple of {RECORD_SIZE}")
    events = []
    for i in range(0, len(data), RECORD_SIZE):
        rec = data[i : i + RECORD_SIZE]
        fields = struct.unpack_from(FIELDS_FMT, rec, 0)
        (ts, etype, side, instr, cl_ord_id, order_id,
         price, qty, filled, leaves, venue) = fields
        events.append({
            "ts": ts,
            "type": EVENT_NAMES.get(etype, str(etype)),
            "side": SIDE_NAMES.get(side, str(side)),
            "instrument": instr,
            "cl_ord_id": cl_ord_id,
            "order_id": order_id,
            "price": price,
            "qty": qty,
            "filled": filled,
            "leaves": leaves,
            "venue": venue,
        })
    return events


def pnl_by_instrument(events):
    """Simplistic mark-to-market P&L: averages fill prices per side."""
    pos = defaultdict(lambda: {"qty": 0, "avg": 0.0, "realized": 0.0, "fills": 0})
    for e in events:
        if e["type"] not in ("ORDER_FILLED", "ORDER_PARTIAL"):
            continue
        p = pos[e["instrument"]]
        sqty = e["filled"] if e["side"] == "BUY" else -e["filled"]
        px = e["price"] / 10000.0  # Fixed-point Price → float
        new_qty = p["qty"] + sqty
        # Close or flip
        if p["qty"] != 0 and ((p["qty"] > 0) != (sqty > 0)):
            closing = min(abs(sqty), abs(p["qty"]))
            sign = 1 if p["qty"] > 0 else -1
            p["realized"] += sign * closing * (px - p["avg"])
        if (p["qty"] >= 0 and sqty > 0) or (p["qty"] <= 0 and sqty < 0):
            total = abs(p["qty"]) + abs(sqty)
            if total:
                p["avg"] = (abs(p["qty"]) * p["avg"] + abs(sqty) * px) / total
        elif p["qty"] == 0 or ((p["qty"] ^ new_qty) < 0):
            p["avg"] = px
        p["qty"] = new_qty
        p["fills"] += 1
    return pos


def write_csv(events, out: Path):
    with out.open("w", encoding="utf-8") as f:
        f.write("timestamp,type,side,instrument,cl_ord_id,order_id,price,qty,filled,leaves,venue\n")
        for e in events:
            f.write(f"{e['ts']},{e['type']},{e['side']},{e['instrument']},"
                    f"{e['cl_ord_id']},{e['order_id']},{e['price']},{e['qty']},"
                    f"{e['filled']},{e['leaves']},{e['venue']}\n")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("trade_log", type=Path)
    ap.add_argument("--out", type=Path, default=Path("pnl.csv"))
    ap.add_argument("--summary", action="store_true")
    args = ap.parse_args(argv)

    events = parse(args.trade_log)
    write_csv(events, args.out)
    print(f"wrote {len(events)} events -> {args.out}")

    if args.summary:
        p = pnl_by_instrument(events)
        print()
        print(f"{'instrument':<12}{'qty':>10}{'avg_px':>12}{'realized':>14}{'fills':>8}")
        total = 0.0
        for instr, info in sorted(p.items()):
            print(f"{instr:<12}{info['qty']:>10}{info['avg']:>12.4f}{info['realized']:>14.2f}{info['fills']:>8}")
            total += info["realized"]
        print(f"{'TOTAL':<12}{'':>10}{'':>12}{total:>14.2f}")


if __name__ == "__main__":
    main(sys.argv[1:])
