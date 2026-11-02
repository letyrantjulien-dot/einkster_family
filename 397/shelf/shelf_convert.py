#!/usr/bin/env python3
"""E!nkster shelf converter: a Kiwix archive (.zim) into plain text files the E!nkster 397 reads from its microSD.

    py shelf_convert.py <archive.zim> <card or folder> [--name NAME] [--no-redirects]

Writes <card>/shelf/<NAME>/ with:
    about.txt      the archive's name, language, source, date, licence, counts (key=value lines)
    titles.txt     every title, sorted by its search key: key<TAB>title<TAB>file<TAB>offset<TAB>length<TAB>redirect target
    text-000.txt   the articles as plain text, each file under 1 GB (FAT32 allows 4 GB): title line, then the body
    text-001.txt   ...

The 397 searches titles.txt by jumping into the middle of it (no database), then reads one article by its offset.
Images, tables, references and links are dropped. Paragraphs, headings ("## "), and list items ("• ") are kept.
Letters the 397's typeface cannot draw are folded to their plain letter where Unicode allows it (ł -> l, ő -> o);
the rest stay as they are and show as a small box on the paper. The report at the end counts them.

Needs Python 3.10 or newer and Kiwix's own library: py -m pip install libzim
Libraries: python-libzim and libzim by openZIM / Kiwix (GPL-3.0-or-later); Python's standard library.
The archive's content keeps its own licence (Wikipedia's text: CC BY-SA 4.0), written into about.txt and shown
on the 397 with every article.

Part of E!nkster, InvisibleChaos. Open source, MIT. Written by Julien and Claude the AI.
"""
import argparse
import html.parser
import multiprocessing
import os
import re
import sys
import time
import unicodedata

VERSION = "1.0.0"
FILE_CAP = 1 << 30                     # 1 GB per text file
FORMAT = 1                             # the shelf format; the 397 checks it

# ---------------------------------------------------------------- the characters the 397 can draw (fonts.h, shared with the 154)
EXTRAS = [
    0x0152, 0x0153, 0x0178, *range(0x0391, 0x03A2), *range(0x03A3, 0x03AA), *range(0x03B1, 0x03CA),
    0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2022, 0x2026, 0x2030, 0x2032, 0x2033,
    0x2039, 0x203A, 0x2070, *range(0x2074, 0x207C), *range(0x2080, 0x208C), 0x20AC, 0x2122,
    0x2190, 0x2191, 0x2192, 0x2193, 0x2194, 0x2202, 0x2206, 0x220F, 0x2211, 0x2212, 0x2218, 0x221A, 0x221E,
    0x222B, 0x2248, 0x2260, 0x2264, 0x2265, 0x25A0, 0x25A1, 0x25CB, 0x25CF, 0x2713, 0x2717,
]
DRAWABLE = set(range(32, 127)) | set(range(160, 256)) | set(EXTRAS) | {10}
SPECIAL = {"ł": "l", "Ł": "L", "đ": "d", "Đ": "D", "ı": "i", "ŋ": "n", "ħ": "h", "Ħ": "H", "ſ": "s",
           "\u2010": "-", "\u2011": "-", "\u2012": "-", "\u2015": "\u2014", "\u2212": "\u2212",
           "\u00ad": "", "\u200b": "", "\u200c": "", "\u200d": "", "\u2060": "", "\ufeff": "",
           "\u2002": " ", "\u2003": " ", "\u2009": " ", "\u200a": " ", "\u202f": "\u00a0", "\u2007": " ",
           "\u02bb": "'", "\u02bc": "'", "\u2032": "\u2032", "\u201b": "'", "\u201f": "\"", "\u2044": "/"}
_fold_cache = {}


def fold_char(c):
    """One character as the 397 can draw it, or the character itself if nothing better exists."""
    if ord(c) in DRAWABLE:
        return c
    r = _fold_cache.get(c)
    if r is not None:
        return r
    if c in SPECIAL:
        r = SPECIAL[c]
    else:
        d = unicodedata.normalize("NFKD", c)
        base = "".join(x for x in d if not unicodedata.combining(x))
        r = base if base and all(ord(x) in DRAWABLE for x in base) else c
    _fold_cache[c] = r
    return r


def fold_text(s, stats):
    out = []
    for c in s:
        f = fold_char(c)
        if f != c:
            stats["folded"] += 1
        elif ord(c) not in DRAWABLE:
            stats["boxes"] += 1
        out.append(f)
    return "".join(out)


# ---------------------------------------------------------------- the search key (the 397 folds what is typed the same way)
KEY_SPECIAL = {"ß": "ss", "æ": "ae", "œ": "oe", "ø": "o", "ł": "l", "đ": "d", "ð": "d", "þ": "th", "ı": "i"}


def search_key(title):
    s = title.lower()
    s = "".join(KEY_SPECIAL.get(c, c) for c in s)
    s = unicodedata.normalize("NFKD", s)
    out, space = [], False
    for c in s:
        if unicodedata.combining(c):
            continue
        if "a" <= c <= "z" or "0" <= c <= "9":
            out.append(c); space = False
        elif not space and out:
            out.append(" "); space = True
    return "".join(out).strip()


# ---------------------------------------------------------------- HTML to plain text
SKIP_TAGS = {"script", "style", "noscript", "table", "figure", "figcaption", "sup", "math", "svg", "nav",
             "footer", "header", "button", "form", "select", "audio", "video", "iframe", "object", "map", "h1",
             "head", "title", "template"}
SKIP_CLASSES = ("mw-ref", "reference", "references", "reflist", "navbox", "noprint", "mw-editsection", "metadata",
                "mw-empty-elt", "thumb", "gallery", "infobox", "hatnote-ignore", "mwe-math", "mw-cite-backlink")
BLOCK_TAGS = {"p", "div", "section", "details", "summary", "blockquote", "pre", "ul", "ol", "dl", "dd", "dt",
              "li", "h2", "h3", "h4", "h5", "h6", "br", "hr", "tr", "center"}
VOID = {"br", "hr", "img", "meta", "link", "input", "wbr", "source", "track", "area", "col", "embed", "param", "base"}


class Html2Text(html.parser.HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.out = []                  # finished paragraphs
        self.cur = []                  # text of the paragraph being built
        self.prefix = ""
        self.skip = 0                  # depth inside skipped elements
        self.stack = []                # (tag, skipped)

    def flush(self):
        t = re.sub(r"\s+", " ", "".join(self.cur)).strip()
        self.cur = []
        if t:
            self.out.append(self.prefix + t); self.prefix = ""     # an empty block (a <p> inside an <li>) keeps the bullet

    def handle_starttag(self, tag, attrs):
        if tag in VOID:
            if tag in ("br",) and not self.skip:
                self.cur.append(" ")
            if tag == "hr" and not self.skip:
                self.flush()
            return
        a = dict(attrs)
        cls = (a.get("class") or "")
        hidden = "display:none" in (a.get("style") or "").replace(" ", "")
        skip = tag in SKIP_TAGS or hidden or any(k in cls.split() or k == cls for k in SKIP_CLASSES) \
            or a.get("role") == "note"
        self.stack.append((tag, skip))
        if skip:
            self.skip += 1
            return
        if self.skip:
            return
        if tag in BLOCK_TAGS:
            self.flush()
            if tag in ("h2", "h3", "h4", "h5", "h6"):
                self.prefix = "## "
            elif tag == "li":
                self.prefix = "\u2022 "

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        # close up to the matching open tag (HTML is forgiving, so must we be)
        for i in range(len(self.stack) - 1, -1, -1):
            if self.stack[i][0] == tag:
                for t, sk in self.stack[i:]:
                    if sk:
                        self.skip -= 1
                del self.stack[i:]
                break
        else:
            return
        if not self.skip and tag in BLOCK_TAGS:
            self.flush()

    def handle_data(self, data):
        if not self.skip:
            self.cur.append(data)

    def text(self):
        self.flush()
        paras, last = [], None
        for p in self.out:
            if p.startswith("## ") and last is not None and last.startswith("## "):
                paras[-1] = p                                  # a heading with nothing under it gives way to the next
            else:
                paras.append(p)
            last = p
        while paras and paras[-1].startswith("## "):
            paras.pop()                                        # empty sections at the end (references, links)
        return "\n\n".join(paras)


def html_to_text(raw):
    p = Html2Text()
    try:
        p.feed(raw); p.close()
    except Exception:
        pass
    return p.text()


# ---------------------------------------------------------------- the archive
def meta(arch, key):
    try:
        return bytes(arch.get_metadata(key)).decode("utf-8", "replace").strip()
    except Exception:
        return ""


def licence_for(arch, name):
    lic = meta(arch, "License") or meta(arch, "Licence")
    if lic:
        return lic
    creator = meta(arch, "Creator").lower()
    if "wikipedia" in creator or name.lower().startswith("wikipedia"):
        return "CC BY-SA 4.0"
    if any(w in creator for w in ("wiktionary", "wikivoyage", "wikibooks", "wikiquote", "wikisource", "wikiversity", "wikinews")):
        return "CC BY-SA 4.0"
    return "see the source"


def safe_name(s):
    s = re.sub(r"[^A-Za-z0-9._-]+", "_", s).strip("._")
    return s[:60] or "archive"


# ---------------------------------------------------------------- the work, spread over the processor's cores
_arch = None


def _init(zim):
    global _arch
    from libzim.reader import Archive
    _arch = Archive(zim)


def _convert(span):
    """Entries a..b of the archive: redirects, articles as folded text, and what was left out."""
    a, b, keep_redirects = span
    res, st = [], {"folded": 0, "boxes": 0, "other": 0, "empty": 0}
    for i in range(a, b):
        e = _arch._get_entry_by_id(i)
        if e.is_redirect:
            if keep_redirects:
                try:
                    res.append(("R", e.title, e.get_redirect_entry().path))
                except Exception:
                    pass
            continue
        it = e.get_item()
        if not it.mimetype.startswith("text/html"):
            st["other"] += 1
            continue
        body = html_to_text(bytes(it.content).decode("utf-8", "replace"))
        if not body.strip():
            st["empty"] += 1
            continue
        res.append(("A", e.path, e.title or e.path, fold_text(e.title or e.path, st), fold_text(body, st)))
    return res, st


def main():
    ap = argparse.ArgumentParser(description="Kiwix archive (.zim) to the E!nkster 397's shelf (plain text on the microSD).")
    ap.add_argument("zim")
    ap.add_argument("card", help="the microSD (for example E:\\) or any folder; the archive goes into <card>/shelf/<name>/")
    ap.add_argument("--name", help="folder name on the card (default: from the file name)")
    ap.add_argument("--no-redirects", action="store_true", help="leave out the other names of articles (smaller index, less to search)")
    ap.add_argument("--limit", type=int, default=0, help="stop after this many articles (for a quick try)")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1), help="processor cores to use (default: all but one)")
    ap.add_argument("--file-mb", type=float, default=0, help=argparse.SUPPRESS)   # the bench: small text files
    args = ap.parse_args()
    global FILE_CAP
    if args.file_mb:
        FILE_CAP = int(args.file_mb * 1e6)

    try:
        from libzim.reader import Archive
    except ImportError:
        sys.exit("Kiwix's library is missing. Install it with:  py -m pip install libzim")

    arch = Archive(args.zim)
    name = safe_name(args.name or os.path.splitext(os.path.basename(args.zim))[0])
    out = os.path.join(args.card, "shelf", name)
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):                                  # a fresh conversion replaces an older one
        if f.startswith("text-") or f in ("titles.txt", "about.txt", "titles.tmp"):
            os.remove(os.path.join(out, f))

    n_all = arch.entry_count
    print(f"{os.path.basename(args.zim)}: {n_all:,} entries, {arch.article_count:,} articles. Writing to {out} with {args.jobs} core(s)")
    stats = {"folded": 0, "boxes": 0}
    loc = {}                                                   # article path -> (file, offset, length, title)
    titles = []                                                # (key, title, file, offset, length, target)
    redirects = []                                             # (title, target path)
    fno, fh, fpos = 0, None, 0
    n_art = n_empty = n_nokey = n_other = 0
    t0 = time.time(); last = t0

    def open_text(k):
        return open(os.path.join(out, f"text-{k:03d}.txt"), "wb")

    fh = open_text(0)
    CHUNK = 2000
    spans = [(a, min(a + CHUNK, n_all), not args.no_redirects) for a in range(0, n_all, CHUNK)]
    done_entries = 0
    with multiprocessing.Pool(args.jobs, _init, (args.zim,)) as pool:
        for res, st in pool.imap(_convert, spans):             # in order, so the files come out the same every time
            for k in ("folded", "boxes"):
                stats[k] += st[k]
            n_other += st["other"]; n_empty += st["empty"]
            for r in res:
                if r[0] == "R":
                    redirects.append((r[1], r[2])); continue
                _, path, raw_title, title, body = r
                rec = (title + "\n" + body + "\n\n").encode("utf-8")
                if fpos + len(rec) > FILE_CAP and fpos > 0:
                    fh.close(); fno += 1; fh = open_text(fno); fpos = 0
                fh.write(rec)
                length = len(rec) - 2
                loc[path] = (fno, fpos, length, title)
                key = search_key(raw_title)
                if key:
                    titles.append((key, title, fno, fpos, length, ""))
                else:
                    n_nokey += 1
                fpos += len(rec); n_art += 1
                if args.limit and n_art >= args.limit:
                    break
            done_entries += CHUNK
            if args.limit and n_art >= args.limit:
                pool.terminate(); break
            if time.time() - last > 5:
                last = time.time()
                el = last - t0; done = min(done_entries, n_all)
                eta = el / done * (n_all - done) if done else 0
                print(f"  {done:,} / {n_all:,} entries, {n_art:,} articles, {fno + 1} text file(s), about {eta / 60:.0f} min to go", flush=True)
    fh.close()

    n_red = 0
    for t, target in redirects:
        L = loc.get(target)
        if not L:
            continue
        key = search_key(t)
        if not key:
            continue
        ft = fold_text(t, stats)
        if search_key(L[3]) == key and ft.lower() == L[3].lower():
            continue                                           # the same title again, only spelled differently
        titles.append((key, ft, L[0], L[1], L[2], L[3])); n_red += 1

    print(f"  sorting {len(titles):,} titles", flush=True)
    titles.sort(key=lambda r: (r[0], r[1]))
    tmp = os.path.join(out, "titles.tmp")
    with open(tmp, "wb") as f:
        for k, t, a, b, c, d in titles:
            t = t.replace("\t", " ").replace("\n", " "); d = d.replace("\t", " ").replace("\n", " ")
            f.write(f"{k}\t{t}\t{a}\t{b}\t{c}\t{d}\n".encode("utf-8"))
    os.replace(tmp, os.path.join(out, "titles.txt"))

    lang = meta(arch, "Language")
    about = {
        "format": str(FORMAT),
        "title": meta(arch, "Title") or name,
        "description": meta(arch, "Description"),
        "language": lang,
        "source": meta(arch, "Creator") or meta(arch, "Publisher"),
        "publisher": meta(arch, "Publisher"),
        "date": meta(arch, "Date"),
        "licence": licence_for(arch, name),
        "zim": os.path.basename(args.zim),
        "articles": str(n_art),
        "titles": str(len(titles)),
        "files": str(fno + 1),
        "converted": time.strftime("%Y-%m-%d"),
        "converter": f"E!nkster shelf_convert.py {VERSION}, with libzim (openZIM, GPL-3.0-or-later)",
    }
    with open(os.path.join(out, "about.txt"), "w", encoding="utf-8", newline="\n") as f:
        for k, v in about.items():
            f.write(f"{k}={v.replace(chr(10), ' ')}\n")

    size = sum(os.path.getsize(os.path.join(out, f)) for f in os.listdir(out))
    print(f"Done in {time.time() - t0:.0f} s: {n_art:,} articles, {n_red:,} other names, {len(titles):,} titles to search, "
          f"{fno + 1} text file(s), {size / 1e6:,.1f} MB on the card.")
    print(f"  left out: {n_empty:,} empty pages, {n_other:,} images and other files, {n_nokey:,} titles with no letter "
          f"the 397 can search (they stay readable from other names)")
    print(f"  characters: {stats['folded']:,} folded to a plain letter, {stats['boxes']:,} the 397 will show as a box")


if __name__ == "__main__":
    main()
