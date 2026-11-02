# The shelf: offline archives on the E!nkster 397

`shelf_convert.py` turns a Kiwix archive (a `.zim` file, for example a Wikipedia) into plain text files on a microSD card. The E!nkster 397 reads them with no network and no database: you type a title on the keyboard, and the page appears on the paper.

Version 1.0.0. Python 3.10 or newer, on Windows, macOS or Linux.

## Use it
1. Install Kiwix's own library: `python -m pip install libzim` (on Windows: `py -m pip install libzim`).
2. Download an archive from the Kiwix library (library.kiwix.org), for example a Wikipedia "mini" or "maxi".
3. Convert it straight onto the card:
   ```
   python shelf_convert.py wikipedia_en_computer_maxi.zim E:\
   ```
   The archive lands in `E:\shelf\<name>\`. Options:
   - `--name NAME`: the folder name on the card (default: from the file name).
   - `--no-redirects`: leave out the other names of articles (a smaller index, less to search).
   - `--limit N`: stop after N articles, for a quick try.
   - `--jobs N`: processor cores to use (default: all but one).

At the end, the converter reports what it left out (images, empty pages) and how many letters it folded to a plain letter or left as they are.

## Worked example
The bench script `397/tests/shelf/make_test_zim.py` writes a small archive with headings, lists, a table, references, letters outside the 397's typeface, a redirect, an image and 3,000 numbered articles:
```
python 397/tests/shelf/make_test_zim.py small.zim
python 397/shelf/shelf_convert.py small.zim card --name small
```
The result: `3,006 articles, 2 other names, 3,007 titles to search, 1 text file(s), 0.2 MB on the card`. The same output, cut down, is in `397/tests/host/fixtures/shelf/bench_en/`. The 397's bench reads it there.

On a real archive (Wikipedia's computer science "maxi", 91,847 articles), the conversion took 19 seconds on a 23-core desktop and wrote 302 MB of text.

## The format (version 1)
One folder per archive: `<card>/shelf/<name>/`.

**`about.txt`**: `key=value` lines. `format=1`, `title`, `description`, `language`, `source`, `publisher`, `date`, `licence`, `zim`, `articles`, `titles`, `files`, `converted`, `converter`. The 397 refuses a folder whose `format` it does not know, and shows the licence with every article.

**`titles.txt`**: one line per title, UTF-8, sorted by the search key (byte order):
```
key<TAB>title<TAB>file<TAB>offset<TAB>length<TAB>target
```
- `key`: the title folded for searching. Lower case, `ß`→`ss`, `æ`→`ae`, `œ`→`oe`, `ø`→`o`, `ł`→`l`, `đ`/`ð`→`d`, `þ`→`th`, `ı`→`i`, accents removed (Unicode NFKD), only `a`–`z` and `0`–`9` kept, any other run of characters becomes one space.
- `file`, `offset`, `length`: which `text-NNN.txt` holds the article, and where, in bytes.
- `target`: empty for an article. For another name of an article (a redirect), the key of the article it points to.

**`text-000.txt`, `text-001.txt`…**: the articles as plain text, each file under 1 GB (FAT32 allows 4 GB). Each article is its title line, then the body. `## ` starts a heading and `• ` a list item. Images, tables, references, infoboxes and links are dropped.

**How the 397 finds a title:** it folds what you type with the same rule, then halves `titles.txt` by jumping into the middle of the file until it lands on the first key that starts with what you typed. Nothing is loaded into memory. A 13 GB archive searches as fast as a small one.

## Licences
- **The script is MIT** (see `LICENSE` at the root). It imports **python-libzim** (openZIM / Kiwix, GPL-3.0-or-later). The script itself is MIT; anyone who ships it bundled with libzim ships that bundle under the GPL.
- **The archives** are made by Kiwix and openZIM, from their sources. Wikipedia's text is under **CC BY-SA 4.0**. The converter writes the archive's licence into `about.txt`, and the 397 shows it with every article.

## Related work
The shelf was developed independently. A different take on the same idea, offline Wikipedia on an ESP32 board with a colour screen and images, is Alun Morris's ESP32 Offline Wikipedia Reader (reported by Open Source For You, September 2026). Nothing was taken from it.

Written by Julien and Claude the AI
