"""
Client patch for the custom spell changes (modules/custom/lua/custom_spells.lua).

The client has its own copy of each spell's targets, job levels and MP cost, and of each
scroll's description, and uses them for the target cursor, the spell menu, MP checks and
what it shows. This patches them to match the server toggles:

  ENSPELL_PARTY_TARGET  En-spells: Phalanx II's targets (self or party), range and cursor
  RDM_HASTE_LEVEL_2     Haste: RDM level 2, and the scroll text
  HASTE_MP_10           Haste: 10 MP

Files (FTABLE index -> ROM path on a standard install):
  0x51 spell table  (ROM/118/114.DAT) 0x64-byte records, each byte rotated right by a key
                    from the bit counts of bytes 2, 11, 12 (stored as is).
                    Windower/ResourceExtractor ResourceParser.ParseSpell.
  0x4A usable items (ROM/118/107.DAT) 0x1400-byte records at (id & 0xFFF), rotated right 5.
                    Description = string 4. Windower/POLUtils Things/Item.cs.

Every run starts from the original files (backed up as <file>.orig the first time), so it
can be re-run after changing this script. Needs an Administrator terminal (Program Files).

    python patch_client.py            # patch
    python patch_client.py --check    # show what would change, write nothing
    python patch_client.py --restore  # put the originals back

Restart the game after patching. Use the patched client only on this server. A client
update may overwrite the files: run this again afterwards.
"""
import argparse
import os
import shutil
import struct
import sys

DEFAULT_ROOT = r"C:\Program Files (x86)\PlayOnline\SquareEnix\FINAL FANTASY XI"

RDM = 5
LEVEL_MIN_SLOT = 23  # Holds the lowest level of any job (Cure 1, Haste 40, Enstone 18)

ENSPELLS = {
    100: "Enfire", 101: "Enblizzard", 102: "Enaero", 103: "Enstone", 104: "Enthunder", 105: "Enwater",
    312: "Enfire II", 313: "Enblizzard II", 314: "Enaero II", 315: "Enstone II", 316: "Enthunder II",
    317: "Enwater II", 310: "Enlight",
}
PHALANX_II = 107  # The self-or-party version of a self-only enhancing spell: the template
HASTE = 57

# Scroll item id -> (old text, new text), same length
SCROLL_TEXT = {
    4665: ("WHM Lv.40 / RDM Lv.48 ", "WHM Lv.40 / RDM Lv.2  "),  # Scroll of Haste
}

# --- spell table ------------------------------------------------------------------------------
SPELL_RECORD = 0x64
KEYS = {0: 7, 1: 1, 2: 6, 3: 2, 4: 5}
PLAIN = (2, 11, 12)


def popcount(b):
    return bin(b).count("1")


def spell_key(raw):
    return KEYS[abs(popcount(raw[2]) - popcount(raw[11]) + popcount(raw[12])) % 5]


def rotr(data, k):
    return bytearray(((b >> k) | (b << (8 - k))) & 0xFF for b in data)


def rotl(data, k):
    return bytearray(((b << k) | (b >> (8 - k))) & 0xFF for b in data)


def spell_decode(raw):
    out = rotr(raw, spell_key(raw))
    for i in PLAIN:
        out[i] = raw[i]
    return out


def spell_encode(rec, key):
    out = rotl(rec, key)
    for i in PLAIN:
        out[i] = rec[i]
    return out


def spell_records(data):
    found, off = {}, 0
    while off + 16 <= len(data):
        size = struct.unpack_from("<i", data, off + 4)[0]
        btype, plen = size & 0x7F, ((size >> 3) & ~0xF) - 0x10
        if btype in (0x00, 0x01):
            off += 16
            continue
        if btype == 0x49:
            for i in range(plen // SPELL_RECORD):
                ro = off + 16 + i * SPELL_RECORD
                sid = struct.unpack_from("<h", spell_decode(data[ro:ro + SPELL_RECORD]), 0)[0]
                if sid:
                    found[sid] = ro
        off += 16 + max(plen, 0)
    return found


def spell_summary(rec):
    levels = struct.unpack_from("<24h", rec, 0x0E)
    jobs = " ".join(f"{i}:{lv}" for i, lv in enumerate(levels) if lv != -1)
    return (f"targets=0x{struct.unpack_from('<H', rec, 6)[0]:02x} range={rec[0x45]} cursor={rec[0x48]} "
            f"mp={struct.unpack_from('<h', rec, 0x0A)[0]} levels[{jobs}]")


def set_level(rec, job, level):
    struct.pack_into("<h", rec, 0x0E + 2 * job, level)
    levels = [lv for i, lv in enumerate(struct.unpack_from("<24h", rec, 0x0E)) if lv != -1 and i != LEVEL_MIN_SLOT]
    struct.pack_into("<h", rec, 0x0E + 2 * LEVEL_MIN_SLOT, min(levels))


def patch_spells(data, log):
    recs = spell_records(data)
    missing = [s for s in [PHALANX_II, HASTE, *ENSPELLS] if s not in recs]
    if missing:
        sys.exit(f"Spell records not found: {missing}. Is this the spell table?")

    def edit(sid, name, fn):
        ro = recs[sid]
        raw = data[ro:ro + SPELL_RECORD]
        rec = spell_decode(raw)
        if rec[SPELL_RECORD - 1] != 0xFF:
            sys.exit(f"{name}: record does not end in 0xFF, not patching anything")
        before = spell_summary(rec)
        fn(rec)
        after = spell_summary(rec)
        log(f"  {name:14} {before}" + ("" if before == after else f"\n  {'':14} -> {after}"))
        new = spell_encode(rec, spell_key(raw))
        assert spell_decode(new) == rec
        data[ro:ro + SPELL_RECORD] = new

    template = spell_decode(data[recs[PHALANX_II]:recs[PHALANX_II] + SPELL_RECORD])
    t_targets, t_range, t_cursor = struct.unpack_from("<H", template, 6)[0], template[0x45], template[0x48]

    def party_target(rec):
        struct.pack_into("<H", rec, 6, t_targets)
        rec[0x45], rec[0x48] = t_range, t_cursor

    for sid, name in ENSPELLS.items():
        edit(sid, name, party_target)

    def haste(rec):
        set_level(rec, RDM, 2)
        struct.pack_into("<h", rec, 0x0A, 10)

    edit(HASTE, "Haste", haste)


# --- usable items -----------------------------------------------------------------------------
ITEM_RECORD = 0x1400


def patch_items(data, log):
    for item_id, (old, new) in SCROLL_TEXT.items():
        assert len(old) == len(new)
        ro = (item_id & 0xFFF) * ITEM_RECORD
        rec = rotr(data[ro:ro + ITEM_RECORD], 5)
        if struct.unpack_from("<I", rec, 0)[0] != item_id:
            sys.exit(f"Item {item_id} not where expected, not patching anything")
        if struct.unpack_from("<I", rec, 0x1C)[0] < 5:
            sys.exit(f"Item {item_id}: no description string")
        off = struct.unpack_from("<I", rec, 0x20 + 8 * 4)[0]
        start = 0x1C + off + 28
        end = rec.index(b"\0", start)
        text = bytes(rec[start:end])
        log(f"  item {item_id}: {text!r}")
        if new.encode() in text:
            continue
        if old.encode() not in text:
            sys.exit(f"Item {item_id}: description does not contain {old!r}")
        text = text.replace(old.encode(), new.encode(), 1)
        rec[start:end] = text
        log(f"  {'':10} -> {text!r}")
        data[ro:ro + ITEM_RECORD] = rotl(rec, 5)


# --- files ------------------------------------------------------------------------------------
def dat_path(root, index):
    with open(os.path.join(root, "FTABLE.DAT"), "rb") as f:
        f.seek(index * 2)
        v = struct.unpack("<H", f.read(2))[0]
    return os.path.join(root, "ROM", str(v >> 7), f"{v & 0x7F}.DAT")


def original(path):
    """The untouched file: its backup if there is one (also the en-spell patch's older one)."""
    for backup in (path + ".orig", path + ".enspell.bak"):
        if os.path.exists(backup):
            return backup
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=DEFAULT_ROOT, help="FINAL FANTASY XI install folder")
    ap.add_argument("--check", action="store_true", help="show what would change, write nothing")
    ap.add_argument("--restore", action="store_true", help="put the original files back")
    args = ap.parse_args()

    files = [(dat_path(args.root, 0x51), "spell table", patch_spells),
             (dat_path(args.root, 0x4A), "usable items", patch_items)]

    if args.restore:
        for path, label, _ in files:
            src = original(path)
            if src == path:
                print(f"{label}: no backup, left as is ({path})")
            else:
                shutil.copy2(src, path)
                print(f"{label}: restored {path}")
        return

    out = []
    for path, label, patch in files:
        src = original(path)
        print(f"{label}: {path}" + ("" if src == path else f"\n  from {os.path.basename(src)}"))
        data = bytearray(open(src, "rb").read())
        patch(data, print)
        out.append((path, src, data))

    if args.check:
        print("\nNothing written (--check).")
        return

    for path, src, data in out:
        if src == path:
            shutil.copy2(path, path + ".orig")
            print(f"Backup: {path}.orig")
        with open(path, "wb") as f:
            f.write(data)
        print(f"Written: {path}")
    print("Done. Restart the game to load it.")


if __name__ == "__main__":
    main()
