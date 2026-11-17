"""Builds the ffxiah price snapshot and loads it into bot_market_prices.

    python modules/bots/market/load_snapshot.py import-browse <dump>   # mktExport('browse') output
    python modules/bots/market/load_snapshot.py import-sales <dump>    # mktExport('sales') output
    python modules/bots/market/load_snapshot.py load [--price median|lowest] [--dry-run]

<dump> is a file holding 'BEGIN<base64 gzip>END' (crawl.js), raw or inside a JSON tool result.

Files in data/:
  browse_raw.json   category pages: itemid, stack, median, stock, sold/day
  sales_raw.json    item pages: lowest sale, sales shown, newest/oldest sale time
  snapshot.json     the two merged, with item names and the price the server uses

`load` rebuilds snapshot.json and replaces bot_market_prices. The running map server
(bot_market.cpp) restocks the auction house from it within ~5 minutes; stocked listings
at an old price are removed and relisted at the new one.
"""
import argparse
import base64
import gzip
import json
import pathlib
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
DATA = HERE / "data"
sys.path.insert(0, str(HERE.parent / "dashboard"))


def decode_dump(path):
    text = pathlib.Path(path).read_text(encoding="utf-8")
    if text.lstrip().startswith("["):  # a saved tool result: [{"type": "text", "text": ...}]
        text = "".join(part.get("text", "") for part in json.loads(text))
    blob = text[text.index("BEGIN") + 5 : text.rindex("END")]
    return json.loads(gzip.decompress(base64.b64decode(blob)).decode("utf-8"))


def import_browse(path):
    rows = decode_dump(path)  # [itemid, stack, median, stock, rate]
    out = {
        "source": "https://www.ffxiah.com/browse",
        "taken_at": time.strftime("%Y-%m-%d"),
        "columns": ["itemid", "stack", "median", "stock", "rate"],
        "rows": rows,
    }
    (DATA / "browse_raw.json").write_text(json.dumps(out, separators=(",", ":")), encoding="utf-8")
    print(f"browse_raw.json: {len(rows)} rows")


def import_sales(path):
    res = decode_dump(path)  # {"itemid:stack": [lowest, count, newest, oldest]}
    out = {
        "source": "https://www.ffxiah.com/item/<id>",
        "taken_at": time.strftime("%Y-%m-%d"),
        "columns": ["lowest", "sales", "newest", "oldest"],
        "rows": res,
    }
    (DATA / "sales_raw.json").write_text(json.dumps(out, separators=(",", ":")), encoding="utf-8")
    print(f"sales_raw.json: {len(res)} items")


def load(price_mode, dry_run):
    import dashboard  # noqa: E402  (credentials + connect())

    browse = json.loads((DATA / "browse_raw.json").read_text(encoding="utf-8"))
    sales_path = DATA / "sales_raw.json"
    sales = json.loads(sales_path.read_text(encoding="utf-8")) if sales_path.exists() else {"rows": {}}

    conn = dashboard.connect()
    cur = conn.cursor()
    cur.execute("SELECT itemid, name, stackSize, aH FROM item_basic")
    items = {r[0]: r for r in cur.fetchall()}

    snapshot, skipped = [], 0
    for itemid, stack, median, stock, rate in browse["rows"]:
        info = items.get(itemid)
        if info is None or info[3] == 0 or (stack and info[2] <= 1):
            skipped += 1  # not on this server's auction house
            continue
        lowest, count, newest, oldest = sales["rows"].get(f"{itemid}:{stack}", [0, 0, 0, 0])
        if price_mode == "lowest" and lowest > 0:
            price, source = lowest, "lowest"
        else:
            price, source = median, "median"
        snapshot.append(
            {
                "itemid": itemid,
                "name": info[1],
                "stack": stack,
                "price": price,
                "source": source,
                "lowest": lowest,
                "median": median,
                "stock": stock,
                "rate": rate,
                "sales": count,
                "newest_sale": newest,
                "oldest_sale": oldest,
            }
        )

    meta = {
        "source": "ffxiah.com (server Asura)",
        "browse_taken_at": browse.get("taken_at"),
        "sales_taken_at": sales.get("taken_at"),
        "price_mode": price_mode,
        "items": len(snapshot),
        "from_lowest_sale": sum(1 for s in snapshot if s["source"] == "lowest"),
        "stocked": sum(1 for s in snapshot if s["stock"] > 0),
    }
    print(json.dumps(meta, indent=1), f"\nskipped (not on this server's AH): {skipped}")
    if dry_run:
        return

    (DATA / "snapshot.json").write_text(json.dumps({"meta": meta, "items": snapshot}, indent=0), encoding="utf-8")

    now = int(time.time())
    # Same definition as bot_market.cpp, so loading works before the map server has run the module.
    cur.execute(
        "CREATE TABLE IF NOT EXISTS `bot_market_prices` ("
        "`itemid` SMALLINT(5) UNSIGNED NOT NULL, `stack` TINYINT(1) UNSIGNED NOT NULL,"
        "`price` INT(10) UNSIGNED NOT NULL DEFAULT 0, `lowest` INT(10) UNSIGNED NOT NULL DEFAULT 0,"
        "`median` INT(10) UNSIGNED NOT NULL DEFAULT 0, `stock` SMALLINT(5) UNSIGNED NOT NULL DEFAULT 0,"
        "`rate` FLOAT NOT NULL DEFAULT 0, `sales` SMALLINT(5) UNSIGNED NOT NULL DEFAULT 0,"
        "`source` VARCHAR(16) NOT NULL DEFAULT '', `updated` INT(10) UNSIGNED NOT NULL DEFAULT 0,"
        "PRIMARY KEY (`itemid`, `stack`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
    )
    cur.execute("DELETE FROM bot_market_prices")
    cur.executemany(
        "INSERT INTO bot_market_prices (itemid, stack, price, lowest, median, stock, rate, sales, source, updated) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        [
            (s["itemid"], s["stack"], s["price"], s["lowest"], s["median"], min(s["stock"], 65535), s["rate"], min(s["sales"], 65535), s["source"], now)
            for s in snapshot
        ],
    )
    conn.commit()
    print(f"bot_market_prices: {len(snapshot)} rows")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("import-browse").add_argument("dump")
    sub.add_parser("import-sales").add_argument("dump")
    p = sub.add_parser("load")
    p.add_argument("--price", choices=["lowest", "median"], default="median")
    p.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    if a.cmd == "import-browse":
        import_browse(a.dump)
    elif a.cmd == "import-sales":
        import_sales(a.dump)
    else:
        load(a.price, a.dry_run)


if __name__ == "__main__":
    main()
