#!/usr/bin/env python3
"""Builds the recomp's own entries into Wonder's menus as a RomFS mod (docs/MENU.md).

Today: a "Quit Game" button in the pause menu, under "Exit Course". It is the game's own button (a copy of the
L_BtnExit part, same look and animations), its label is in every language, and it reports the new PauseResult value
QuitToDesktop, which the runtime adds to the game's enum and acts on (games/wonder/menu.cpp).

Reads the RomFS from private/wonder/extract/romfs and writes private/wonder/mods/recomp-menu/romfs/:
  Layout/Pause.Nin_NX_NVN.blarc.zs   the pause layout with the L_QuitBtn pane
  Pack/Bootup.Nin_NX_NVN.pack.zs     the pause screen's button table with the QuitToDesktop button
  Mals/<lang>.Product.120.sarc.zs    the button's label (LayoutMsgRef/PauseRef.msbt)
  System/Resource/ResourceSizeTable.Product.120.rsizetable.zs   those files' sizes (they grew)
Run it again after the RomFS changes (a game update). Needs the zstandard module.

Usage: python tools/wonder_menu_mod.py [--romfs <dir>] [--out <mod dir>]
"""
import argparse
import os
import struct
import zlib

import zstandard

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

QUIT_PANE = "L_QuitBtn"
QUIT_ENUM = "QuitToDesktop"  # game::sequence::PauseResult value 8, added by games/wonder/menu.cpp
QUIT_Y = -265.0  # one button step (130) under L_BtnExit
ICON_ROW_Y = -405.0  # the round buttons (Settings, Control Guide...) move down to make room: from -355
ICON_PANES = ("L_PlayerChangeBtn", "L_OptionBtn", "L_HelpBtn", "L_AmiiboBtn")
QUIT_TEXT = {
    "USen": "Quit Game", "EUen": "Quit Game", "USfr": "Quitter le jeu", "EUfr": "Quitter le jeu",
    "USes": "Salir del juego", "EUes": "Salir del juego", "EUde": "Spiel beenden", "EUit": "Esci dal gioco",
    "EUnl": "Spel afsluiten", "EUpt": "Sair do jogo", "USpt": "Sair do jogo", "EUru": "Выйти из игры",
    "EUpl": "Zakończ grę", "JPja": "ゲームを終了", "KRko": "게임 종료", "CNzh": "退出游戏", "TWzh": "結束遊戲",
    "THth": "ออกจากเกม",
}


# ---- zstd + SARC -------------------------------------------------------------------------------------------------

def unzs(path):
    return zstandard.ZstdDecompressor().decompress(open(path, "rb").read(), max_output_size=1 << 31)


def sarc_nodes(d):
    """[(name, node offset, data start, data end)]; data offsets are absolute."""
    assert d[:4] == b"SARC" and d[6:8] == b"\xff\xfe", "not a little-endian SARC"
    data = struct.unpack_from("<I", d, 0xC)[0]
    n = struct.unpack_from("<H", d, 0x1A)[0]
    names = 0x20 + n * 0x10 + 8
    out = []
    for k in range(n):
        node = 0x20 + k * 0x10
        _, attr, start, end = struct.unpack_from("<IIII", d, node)
        o = names + (attr & 0xFFFF) * 4
        out.append((d[o:d.index(b"\0", o)].decode(), node, data + start, data + end))
    return out


def sarc_get(d, name):
    for n, _, s, e in sarc_nodes(d):
        if n == name:
            return d[s:e]
    raise KeyError(name)


def sarc_replace(d, files):
    """Each replaced file's new bytes go at the end (0x2000-aligned); every other file keeps its offset, so
    alignment the game relies on is untouched. The old copies stay as unused bytes."""
    out = bytearray(d)
    data = struct.unpack_from("<I", d, 0xC)[0]
    for name, _, _, _ in sarc_nodes(d):
        if name in files:
            node = next(o for n, o, _, _ in sarc_nodes(bytes(out)) if n == name)
            out += b"\0" * (-len(out) % 0x2000)
            start = len(out)
            out += files[name]
            struct.pack_into("<II", out, node + 8, start - data, len(out) - data)
    missing = set(files) - {n for n, _, _, _ in sarc_nodes(d)}
    assert not missing, f"not in the archive: {missing}"
    struct.pack_into("<I", out, 0x8, len(out))
    return bytes(out)


# ---- BYML (version 7, what Wonder's gyml use) -------------------------------------------------------------------

class U32(int):
    pass


def byml_read(d):
    assert d[:2] == b"YB", "not a little-endian BYML"
    kt, st, root = struct.unpack_from("<III", d, 4)

    def table(o):
        if not o:
            return []
        n = struct.unpack_from("<I", d, o)[0] >> 8
        offs = struct.unpack_from(f"<{n + 1}I", d, o + 4)
        return [d[o + x:d.index(b"\0", o + x)].decode() for x in offs[:-1]]

    keys, strs = table(kt), table(st)

    def val(t, v):
        if t == 0xA0:
            return strs[v]
        if t in (0xC0, 0xC1):
            return node(v)
        if t == 0xD0:
            return bool(v)
        if t == 0xD1:
            return struct.unpack("<i", struct.pack("<I", v))[0]
        if t == 0xD2:
            return struct.unpack("<f", struct.pack("<I", v))[0]
        if t == 0xD3:
            return U32(v)
        raise ValueError(f"BYML node type {t:#x} not handled")

    def node(o):
        t, n = d[o], struct.unpack_from("<I", d, o)[0] >> 8
        if t == 0xC0:
            types = d[o + 4:o + 4 + n]
            vals = o + 4 + ((n + 3) & ~3)
            return [val(types[i], struct.unpack_from("<I", d, vals + 4 * i)[0]) for i in range(n)]
        if t == 0xC1:
            r = {}
            for i in range(n):
                e = o + 4 + 8 * i
                k = struct.unpack_from("<I", d, e)[0] & 0xFFFFFF
                r[keys[k]] = val(d[e + 3], struct.unpack_from("<I", d, e + 4)[0])
            return r
        raise ValueError(f"BYML node type {t:#x} not handled")

    return struct.unpack_from("<H", d, 2)[0], node(root)


def byml_write(version, root):
    keys, strs = set(), set()

    def collect(v):
        if isinstance(v, dict):
            keys.update(v)
            for x in v.values():
                collect(x)
        elif isinstance(v, list):
            for x in v:
                collect(x)
        elif isinstance(v, str):
            strs.add(v)

    collect(root)
    keys, strs = sorted(keys, key=str.encode), sorted(strs, key=str.encode)
    kidx, sidx = {k: i for i, k in enumerate(keys)}, {s: i for i, s in enumerate(strs)}
    out = bytearray(16)

    def align():
        out.extend(b"\0" * (-len(out) % 4))

    def table(items):
        align()
        start = len(out)
        blob = [s.encode() + b"\0" for s in items]
        offs, o = [], 4 + 4 * (len(items) + 1)
        for b in blob:
            offs.append(o)
            o += len(b)
        offs.append(o)
        out.extend(struct.pack("<I", 0xC2 | len(items) << 8) + struct.pack(f"<{len(offs)}I", *offs) + b"".join(blob))
        return start

    def typed(v):
        if isinstance(v, dict):
            return 0xC1
        if isinstance(v, list):
            return 0xC0
        if isinstance(v, str):
            return 0xA0
        if isinstance(v, bool):
            return 0xD0
        if isinstance(v, U32):
            return 0xD3
        if isinstance(v, int):
            return 0xD1
        return 0xD2

    def scalar(t, v):
        if t == 0xA0:
            return sidx[v]
        if t == 0xD2:
            return struct.unpack("<I", struct.pack("<f", v))[0]
        return int(v) & 0xFFFFFFFF

    def node(v):
        # Children first, so this node's slots can hold their offsets.
        items = sorted(v.items(), key=lambda kv: kidx[kv[0]]) if isinstance(v, dict) else list(enumerate(v))
        sub = {k: node(x) for k, x in items if isinstance(x, (dict, list))}
        align()
        start = len(out)
        if isinstance(v, dict):
            out.extend(struct.pack("<I", 0xC1 | len(items) << 8))
            for k, x in items:
                t = typed(x)
                out.extend(struct.pack("<II", kidx[k] | t << 24, sub[k] if k in sub else scalar(t, x)))
        else:
            out.extend(struct.pack("<I", 0xC0 | len(items) << 8) + bytes(typed(x) for _, x in items))
            align()
            for k, x in items:
                out.extend(struct.pack("<I", sub[k] if k in sub else scalar(typed(x), x)))
        return start

    kt = table(keys) if keys else 0
    st = table(strs) if strs else 0
    root_off = node(root)
    struct.pack_into("<2sHIII", out, 0, b"YB", version, kt, st, root_off)
    return bytes(out)


# ---- BFLYT ------------------------------------------------------------------------------------------------------

def flyt_sections(d):
    o, n = struct.unpack_from("<H", d, 6)[0], struct.unpack_from("<H", d, 0x10)[0]
    for _ in range(n):
        size = struct.unpack_from("<I", d, o + 4)[0]
        yield o, d[o:o + 4], size
        o += size


def pane_name(d, o):
    return d[o + 0xC:o + 0xC + 24].split(b"\0")[0].decode("latin1")


def patch_pause_layout(d):
    """L_QuitBtn: a copy of L_BtnExit (same part, same look) right after it, one step lower; the icon row moves down."""
    out = bytearray(d)
    src = None
    for o, magic, size in flyt_sections(d):
        if magic == b"prt1" and pane_name(d, o) == "L_BtnExit":
            src = (o, size)
        if magic == b"prt1" and pane_name(d, o) in ICON_PANES:
            struct.pack_into("<f", out, o + 0x30, ICON_ROW_Y)  # translate.y
        if magic == b"prt1" and pane_name(d, o) == QUIT_PANE:
            raise SystemExit("the pause layout already has " + QUIT_PANE)
    assert src, "L_BtnExit not found in Pause.bflyt"
    o, size = src
    clone = bytearray(d[o:o + size])
    clone[0xC:0xC + 24] = QUIT_PANE.encode().ljust(24, b"\0")
    struct.pack_into("<f", clone, 0x30, QUIT_Y)
    out[o + size:o + size] = clone
    struct.pack_into("<I", out, 0xC, len(out))  # file size
    struct.pack_into("<H", out, 0x10, struct.unpack_from("<H", d, 0x10)[0] + 1)  # section count
    return bytes(out)


# ---- MSBT -------------------------------------------------------------------------------------------------------

def msbt_add(d, label, text):
    """Adds one message: LBL1 rebuilt (hash buckets), ATR1 count + 1 (Wonder's entries are 0 bytes), TXT2 appended."""
    assert d[:8] == b"MsgStdBn" and d[8:10] == b"\xff\xfe" and d[0xC] == 1, "not a little-endian UTF-16 MSBT"
    sections, o = [], 0x20
    while o < len(d):
        size = struct.unpack_from("<I", d, o + 4)[0]
        sections.append([d[o:o + 4], d[o + 16:o + 16 + size]])
        o += 16 + ((size + 15) & ~15)
    body = dict((bytes(m), b) for m, b in sections)
    lbl, txt = body[b"LBL1"], body[b"TXT2"]
    buckets = struct.unpack_from("<I", lbl)[0]
    labels = []
    for i in range(buckets):
        count, off = struct.unpack_from("<II", lbl, 4 + 8 * i)
        for _ in range(count):
            n = lbl[off]
            labels.append((lbl[off + 1:off + 1 + n], struct.unpack_from("<I", lbl, off + 1 + n)[0]))
            off += 5 + n
    assert label.encode() not in [l for l, _ in labels], label + " already in the MSBT"
    count = struct.unpack_from("<I", txt)[0]
    offs = list(struct.unpack_from(f"<{count}I", txt, 4)) + [len(txt)]
    texts = [txt[offs[i]:offs[i + 1]] for i in range(count)]
    labels.append((label.encode(), count))
    texts.append(text.encode("utf-16-le") + b"\0\0")

    def h(s):
        v = 0
        for c in s:
            v = (v * 0x492 + c) & 0xFFFFFFFF
        return v % buckets

    table, blob = bytearray(struct.pack("<I", buckets)), bytearray()
    base = 4 + 8 * buckets
    for b in range(buckets):
        mine = [(l, i) for l, i in labels if h(l) == b]
        table += struct.pack("<II", len(mine), base + len(blob))
        for l, i in mine:
            blob += bytes([len(l)]) + l + struct.pack("<I", i)
    body[b"LBL1"] = bytes(table + blob)
    t, o = bytearray(struct.pack("<I", count + 1)), 4 + 4 * (count + 1)
    for x in texts:
        t += struct.pack("<I", o)
        o += len(x)
    body[b"TXT2"] = bytes(t + b"".join(texts))
    if b"ATR1" in body:
        n, size = struct.unpack_from("<II", body[b"ATR1"])
        assert size == 0, "ATR1 entries with data are not handled"
        body[b"ATR1"] = struct.pack("<II", n + 1, 0) + body[b"ATR1"][8:]
    out = bytearray(d[:0x20])
    for m, _ in sections:
        b = body[bytes(m)]
        out += m + struct.pack("<I", len(b)) + b"\0" * 8 + b + b"\xab" * (-len(b) % 16)
    struct.pack_into("<I", out, 0x12, len(out))
    return bytes(out)


# ---- the mod ----------------------------------------------------------------------------------------------------

def rstb_grow(d, growth):
    """System/Resource/ResourceSizeTable: the game sizes each file's load buffer from it, so a file that grew needs
    its entry grown by as much (RESTBL v1: CRC32-of-path entries, then entries by name for collisions)."""
    assert d[:6] == b"RESTBL", "not a RESTBL"
    _, name_len, ncrc, nname = struct.unpack_from("<IIII", d, 6)
    out = bytearray(d)
    left = dict(growth)
    for i in range(ncrc):
        crc, size = struct.unpack_from("<II", d, 0x16 + 8 * i)
        for path in [p for p in left if zlib.crc32(p.encode()) == crc]:
            struct.pack_into("<I", out, 0x16 + 8 * i + 4, size + left.pop(path))
    o = 0x16 + 8 * ncrc
    for i in range(nname):
        name = d[o:o + name_len].split(b"\0")[0].decode()
        if name in left:
            struct.pack_into("<I", out, o + name_len, struct.unpack_from("<I", d, o + name_len)[0] + left.pop(name))
        o += name_len + 4
    assert not left, f"not in the size table: {list(left)}"
    return bytes(out)


def write_zs(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(zstandard.ZstdCompressor(level=19).compress(data))
    print("wrote", os.path.relpath(path, REPO))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--romfs", default=os.path.join(REPO, "private", "wonder", "extract", "romfs"))
    ap.add_argument("--out", default=os.path.join(REPO, "private", "wonder", "mods", "recomp-menu"))
    a = ap.parse_args()
    romfs, out = a.romfs, os.path.join(a.out, "romfs")
    growth = {}

    def mod(name, member, edit, member_heap=0):
        arc = unzs(os.path.join(romfs, name))
        old = sarc_get(arc, member)
        data = edit(old)
        new = sarc_replace(arc, {member: data})
        growth[name[:-3]] = len(new) - len(arc)
        if member_heap:  # a pack member is a resource of its own, whose entry also sizes what it is parsed into
            growth[member] = len(data) - len(old) + member_heap
        write_zs(os.path.join(out, name), new)

    mod("Layout/Pause.Nin_NX_NVN.blarc.zs", "blyt/Pause.bflyt", patch_pause_layout)

    def add_button(data):
        version, screen = byml_read(data)
        assert byml_read(byml_write(version, screen))[1] == screen, "BYML round trip changed the pause screen"
        buttons = screen["ButtonInfoGenerateTable"]
        exit_btn = next(b for b in buttons if b["PaneName"] == "L_BtnExit")
        buttons.insert(buttons.index(exit_btn) + 1, {
            **exit_btn, "PaneName": QUIT_PANE, "UniqueName": "QuitBtn",
            "DecideInfoGenerateTable": {**exit_btn["DecideInfoGenerateTable"], "EnumValue": QUIT_ENUM}})
        return byml_write(version, screen)

    # The pause screen's entry (19136 for a 4660-byte file) is its parsed tables too: room for one more button.
    mod("Pack/Bootup.Nin_NX_NVN.pack.zs", "Gyml/UIExtraScreenParam/Pause.game__UIExtraScreenParam.bgyml", add_button,
        member_heap=0x2000)

    for f in sorted(os.listdir(os.path.join(romfs, "Mals"))):
        lang = f.split(".")[0]
        if lang not in QUIT_TEXT:
            print("skipped", f, "(no translation)")
            continue
        mod("Mals/" + f, "LayoutMsgRef/PauseRef.msbt",
            lambda m: msbt_add(m, QUIT_PANE + "-T_BtnName", QUIT_TEXT[lang]))

    name = "System/Resource/ResourceSizeTable.Product.120.rsizetable.zs"
    write_zs(os.path.join(out, name), rstb_grow(unzs(os.path.join(romfs, name)), growth))


if __name__ == "__main__":
    main()
