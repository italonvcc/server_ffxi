#!/usr/bin/python3
"""
Bot Players dashboard.

A local web page to create, inspect and manage bot players (modules/bots/cpp/bot_players.cpp).

It only talks to the database:
  - reads characters, gear and the live status the map server writes to `bot_players`
  - queues actions in `bot_commands`, which the map server runs every 2.4 seconds

Usage (from the repository root or anywhere):
    python modules/bots/dashboard/dashboard.py [--port 8088]

Then open http://127.0.0.1:8088

NOTE: There is no login. It only listens on 127.0.0.1, keep it that way.
"""

import argparse
import json
import re
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

import mariadb

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
SQL_FILE = HERE.parent / "sql" / "bot_players.sql"
INDEX_FILE = HERE / "index.html"

# Map server writes a heartbeat every ~4.8s; consider it down after this many seconds.
HEARTBEAT_STALE_SECONDS = 15

JOBS = [
    "NON", "WAR", "MNK", "WHM", "BLM", "RDM", "THF", "PLD", "DRK", "BST", "BRD", "RNG",
    "SAM", "NIN", "DRG", "SMN", "BLU", "COR", "PUP", "DNC", "SCH", "GEO", "RUN", "MON",
]
RACES = {1: "Hume (M)", 2: "Hume (F)", 3: "Elvaan (M)", 4: "Elvaan (F)",
         5: "Tarutaru (M)", 6: "Tarutaru (F)", 7: "Mithra", 8: "Galka"}
SIZES = {0: "Small", 1: "Medium", 2: "Large"}
NATIONS = {0: "San d'Oria", 1: "Bastok", 2: "Windurst"}
EQUIP_SLOTS = ["Main", "Sub", "Ranged", "Ammo", "Head", "Body", "Hands", "Legs", "Feet",
               "Neck", "Waist", "Ear 1", "Ear 2", "Ring 1", "Ring 2", "Back"]
GIL_ITEM_ID = 65535
LOC_INVENTORY = 0

# char_jobs column per job id
JOB_COLUMNS = {i: name.lower() for i, name in enumerate(JOBS) if i > 0}

NAME_RE = re.compile(r"^[A-Z][a-z]{2,14}$")
COMMANDS = {"create", "spawn", "despawn", "summon", "autospawn", "delete"}


def read_credentials():
    """Reads SQL_* from settings/network.lua, falling back to settings/default/network.lua."""
    creds = {}
    for path in (REPO_ROOT / "settings" / "default" / "network.lua", REPO_ROOT / "settings" / "network.lua"):
        if not path.exists():
            continue
        for line in path.read_text(encoding="utf-8").splitlines():
            match = re.search(r"(SQL_\w+)\s*=\s*(?:['\"](.*?)['\"]|([^'\s,]+)),", line)
            if match:
                creds[match.group(1)] = match.group(3) if match.group(3) else match.group(2)
    return creds


CREDS = read_credentials()


def connect():
    return mariadb.connect(
        host=CREDS.get("SQL_HOST", "127.0.0.1"),
        port=int(CREDS.get("SQL_PORT", 3306)),
        user=CREDS.get("SQL_LOGIN", "root"),
        password=CREDS.get("SQL_PASSWORD", ""),
        database=CREDS.get("SQL_DATABASE", "xidb"),
        autocommit=True,
    )


def query(conn, sql, params=()):
    cur = conn.cursor(dictionary=True)
    cur.execute(sql, params)
    rows = cur.fetchall() if cur.description else []
    cur.close()
    return rows


def ensure_tables():
    conn = connect()
    try:
        cur = conn.cursor()
        for statement in SQL_FILE.read_text(encoding="utf-8").split(";"):
            body = "\n".join(l for l in statement.splitlines() if not l.strip().startswith("--")).strip()
            if body:
                cur.execute(body)
        cur.close()
    finally:
        conn.close()


def pretty_item(name):
    return name.replace("_", " ").title() if name else ""


def job_level(jobs_row, job_id):
    column = JOB_COLUMNS.get(job_id)
    return jobs_row.get(column, 0) if jobs_row and column else 0


#
# Read models
#


def get_state(conn):
    heartbeat = query(conn, "SELECT value FROM server_variables WHERE name = 'BotsHeartbeat'")
    heartbeat_age = int(time.time()) - heartbeat[0]["value"] if heartbeat else None

    bots = query(conn, """
        SELECT b.*, c.charname, c.nation, c.pos_zone, l.race, l.face, l.size,
               s.mjob, s.sjob, z.name AS zone_name, j.*
        FROM bot_players b
        JOIN chars c ON c.charid = b.charid
        LEFT JOIN char_look l ON l.charid = b.charid
        LEFT JOIN char_stats s ON s.charid = b.charid
        LEFT JOIN char_jobs j ON j.charid = b.charid
        LEFT JOIN zone_settings z ON z.zoneid = IF(b.online, b.zone, c.pos_zone)
        ORDER BY c.charname
    """)

    bot_list = []
    for b in bots:
        bot_list.append({
            "charid": b["charid"],
            "name": b["charname"],
            "race": RACES.get(b["race"], "?"),
            "job": JOBS[b["mjob"]] if b["mjob"] is not None and b["mjob"] < len(JOBS) else "?",
            "level": job_level(b, b["mjob"]),
            "zone": (b["zone_name"] or "").replace("_", " "),
            "online": bool(b["online"]),
            "autospawn": bool(b["autospawn"]),
            "hp": b["hp"], "max_hp": b["max_hp"], "mp": b["mp"], "max_mp": b["max_mp"],
            "pos": [round(b["pos_x"], 2), round(b["pos_y"], 2), round(b["pos_z"], 2)],
        })

    players = query(conn, """
        SELECT c.charid, c.charname, z.name AS zone_name
        FROM accounts_sessions s
        JOIN chars c ON c.charid = s.charid
        LEFT JOIN zone_settings z ON z.zoneid = c.pos_zone
        WHERE s.charid NOT IN (SELECT charid FROM bot_players)
        ORDER BY c.charname
    """)

    commands = query(conn, """
        SELECT q.id, q.command, q.charid, COALESCE(q.name, c.charname) AS name, q.status, q.result,
               DATE_FORMAT(q.created, '%H:%i:%s') AS created
        FROM bot_commands q
        LEFT JOIN chars c ON c.charid = q.charid
        ORDER BY q.id DESC LIMIT 15
    """)

    return {
        "server": {
            "online": heartbeat_age is not None and heartbeat_age <= HEARTBEAT_STALE_SECONDS,
            "heartbeat_age": heartbeat_age,
        },
        "bots": bot_list,
        "players": [{"charid": p["charid"], "name": p["charname"],
                     "zone": (p["zone_name"] or "").replace("_", " ")} for p in players],
        "commands": commands,
        "pending": sum(1 for c in commands if c["status"] == "pending"),
    }


def get_bot(conn, charid):
    rows = query(conn, """
        SELECT b.*, c.charname, c.nation, c.pos_zone, c.pos_x AS saved_x, c.pos_y AS saved_y, c.pos_z AS saved_z,
               c.playtime, DATE_FORMAT(b.created, '%Y-%m-%d %H:%i') AS created_at,
               l.race, l.face, l.size, s.mjob, s.sjob, z.name AS zone_name
        FROM bot_players b
        JOIN chars c ON c.charid = b.charid
        LEFT JOIN char_look l ON l.charid = b.charid
        LEFT JOIN char_stats s ON s.charid = b.charid
        LEFT JOIN zone_settings z ON z.zoneid = c.pos_zone
        WHERE b.charid = ?
    """, (charid,))
    if not rows:
        return None
    b = rows[0]

    jobs_rows = query(conn, "SELECT * FROM char_jobs WHERE charid = ?", (charid,))
    jobs_row = jobs_rows[0] if jobs_rows else {}
    job_levels = [{"job": JOBS[i], "level": jobs_row.get(col, 0)}
                  for i, col in JOB_COLUMNS.items() if jobs_row.get(col, 0)]

    equipment = {r["equipslotid"]: r for r in query(conn, """
        SELECT e.equipslotid, i.itemId, ib.name
        FROM char_equip e
        JOIN char_inventory i ON i.charid = e.charid AND i.location = e.containerid AND i.slot = e.slotid
        LEFT JOIN item_basic ib ON ib.itemid = i.itemId
        WHERE e.charid = ?
    """, (charid,))}

    inventory = query(conn, """
        SELECT i.slot, i.itemId, i.quantity, ib.name
        FROM char_inventory i
        LEFT JOIN item_basic ib ON ib.itemid = i.itemId
        WHERE i.charid = ? AND i.location = ? AND i.itemId != ?
        ORDER BY i.slot
    """, (charid, LOC_INVENTORY, GIL_ITEM_ID))

    gil = query(conn, "SELECT quantity FROM char_inventory WHERE charid = ? AND itemId = ? LIMIT 1",
                (charid, GIL_ITEM_ID))

    equipped_slots = {(LOC_INVENTORY, r["itemId"]) for r in equipment.values()}

    return {
        "charid": b["charid"],
        "name": b["charname"],
        "race": RACES.get(b["race"], "?"),
        "face": f"{b['face'] // 2 + 1}{'AB'[b['face'] % 2]}" if b["face"] is not None else "?",
        "size": SIZES.get(b["size"], "?"),
        "nation": NATIONS.get(b["nation"], "?"),
        "job": JOBS[b["mjob"]] if b["mjob"] is not None and b["mjob"] < len(JOBS) else "?",
        "sub_job": JOBS[b["sjob"]] if b["sjob"] else None,
        "level": job_level(jobs_row, b["mjob"]),
        "job_levels": job_levels,
        "online": bool(b["online"]),
        "autospawn": bool(b["autospawn"]),
        "zone": (b["zone_name"] or "").replace("_", " "),
        "pos": [round(b["saved_x"], 2), round(b["saved_y"], 2), round(b["saved_z"], 2)],
        "hp": b["hp"], "max_hp": b["max_hp"], "mp": b["mp"], "max_mp": b["max_mp"],
        "gil": gil[0]["quantity"] if gil else 0,
        "created": b["created_at"],
        "equipment": [{"slot": name, "item": pretty_item(equipment[i]["name"]) if i in equipment else None,
                       "item_id": equipment[i]["itemId"] if i in equipment else None}
                      for i, name in enumerate(EQUIP_SLOTS)],
        "inventory": [{"slot": r["slot"], "item": pretty_item(r["name"]) or f"Item {r['itemId']}",
                       "item_id": r["itemId"], "quantity": r["quantity"],
                       "equipped": (LOC_INVENTORY, r["itemId"]) in equipped_slots} for r in inventory],
    }


def get_zones(conn):
    return [{"id": z["zoneid"], "name": z["name"].replace("_", " ")}
            for z in query(conn, "SELECT zoneid, name FROM zone_settings WHERE zoneip != '' ORDER BY name")]


#
# Commands
#


class BadRequest(Exception):
    pass


def queue_command(conn, body):
    command = body.get("command")
    if command not in COMMANDS:
        raise BadRequest(f"Unknown command: {command}")

    def as_int(key, lo, hi, required=False):
        value = body.get(key)
        if value in (None, ""):
            if required:
                raise BadRequest(f"{key} is required")
            return None
        try:
            value = int(value)
        except (TypeError, ValueError):
            raise BadRequest(f"{key} must be a number")
        if not lo <= value <= hi:
            raise BadRequest(f"{key} must be {lo}-{hi}")
        return value

    def as_float(key):
        value = body.get(key)
        if value in (None, ""):
            return None
        try:
            return float(value)
        except (TypeError, ValueError):
            raise BadRequest(f"{key} must be a number")

    row = {"command": command}

    if command == "create":
        name = str(body.get("name", "")).strip()
        name = name[:1].upper() + name[1:].lower()
        if not NAME_RE.match(name):
            raise BadRequest("Names are 3-15 letters only.")
        if query(conn, "SELECT charid FROM chars WHERE charname = ?", (name,)):
            raise BadRequest(f"The name {name} is taken.")
        row.update(name=name,
                   race=as_int("race", 1, 8, True), face=as_int("face", 0, 15, True),
                   size=as_int("size", 0, 2, True), job=as_int("job", 1, 6, True),
                   level=as_int("level", 1, 99) or 1, nation=as_int("nation", 0, 2),
                   autospawn=1 if body.get("autospawn", True) else 0)
    else:
        row["charid"] = as_int("charid", 1, 2**32 - 1, True)
        if not query(conn, "SELECT charid FROM bot_players WHERE charid = ?", (row["charid"],)):
            raise BadRequest("That character is not a bot.")
        if command == "autospawn":
            row["autospawn"] = 1 if body.get("autospawn") else 0

    if command in ("create", "summon", "spawn"):
        anchor = as_int("anchor_charid", 1, 2**32 - 1)
        zone = as_int("zone", 0, 65535)
        if anchor:
            row["anchor_charid"] = anchor
        elif zone is not None:
            row.update(zone=zone, pos_x=as_float("pos_x") or 0.0,
                       pos_y=as_float("pos_y") or 0.0, pos_z=as_float("pos_z") or 0.0)
        elif command != "spawn":
            raise BadRequest("Pick a player to stand next to, or a zone and position.")

    columns = ", ".join(row)
    placeholders = ", ".join("?" for _ in row)
    cur = conn.cursor()
    cur.execute(f"INSERT INTO bot_commands ({columns}) VALUES ({placeholders})", tuple(row.values()))
    command_id = cur.lastrowid
    cur.close()
    return {"id": command_id}


def get_command(conn, command_id):
    rows = query(conn, "SELECT id, command, status, result FROM bot_commands WHERE id = ?", (command_id,))
    return rows[0] if rows else None


#
# HTTP
#


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # keep the console quiet; errors are still printed below

    def send_json(self, status, payload):
        data = json.dumps(payload, default=str).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def handle_api(self, method):
        path = urlparse(self.path).path
        conn = connect()
        try:
            if method == "GET" and path == "/api/state":
                return self.send_json(200, get_state(conn))
            if method == "GET" and path == "/api/zones":
                return self.send_json(200, get_zones(conn))
            if method == "GET" and (m := re.fullmatch(r"/api/bots/(\d+)", path)):
                bot = get_bot(conn, int(m.group(1)))
                return self.send_json(200 if bot else 404, bot or {"error": "Not found"})
            if method == "GET" and (m := re.fullmatch(r"/api/commands/(\d+)", path)):
                cmd = get_command(conn, int(m.group(1)))
                return self.send_json(200 if cmd else 404, cmd or {"error": "Not found"})
            if method == "POST" and path == "/api/commands":
                length = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(length) or b"{}")
                return self.send_json(200, queue_command(conn, body))
            return self.send_json(404, {"error": "Not found"})
        except BadRequest as err:
            return self.send_json(400, {"error": str(err)})
        except (mariadb.Error, json.JSONDecodeError) as err:
            print(f"dashboard: {err}")
            return self.send_json(500, {"error": str(err)})
        finally:
            conn.close()

    def do_GET(self):
        if urlparse(self.path).path in ("/", "/index.html"):
            data = INDEX_FILE.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)
            return
        self.handle_api("GET")

    def do_POST(self):
        self.handle_api("POST")


def main():
    parser = argparse.ArgumentParser(description="Bot Players dashboard")
    parser.add_argument("--port", type=int, default=8088)
    args = parser.parse_args()

    ensure_tables()

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"Bot dashboard: http://127.0.0.1:{args.port}  (Ctrl+C to stop)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
