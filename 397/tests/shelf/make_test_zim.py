"""A small Kiwix-style archive for the bench: headings, lists, tables, references, letters outside the 397's typeface,
other names (redirects), an image, and 3000 numbered articles so the title search has something to bisect."""
import sys
from libzim.writer import Creator, Item, StringProvider, Hint

class Page(Item):
    def __init__(self, path, title, html, mime="text/html"):
        super().__init__(); self.p, self.t, self.h, self.m = path, title, html, mime
    def get_path(self): return self.p
    def get_title(self): return self.t
    def get_mimetype(self): return self.m
    def get_contentprovider(self): return StringProvider(self.h)
    def get_hints(self): return {Hint.FRONT_ARTICLE: self.m == "text/html"}

def wrap(title, body):
    return f"<html><head><title>{title}</title><style>p{{x:1}}</style></head><body><h1>{title}</h1>{body}</body></html>"

LONG = "".join(f"<p>Paragraph {i}: the quick brown fox jumps over the lazy dog, and the river keeps running past the old mill while the bench counts the lines of this long article.</p>" for i in range(1, 121))
pages = [
    ("A/Berlin", "Berlin", wrap("Berlin", "<p>Berlin is the capital of Germany.<sup class=reference>[1]</sup> It lies on the Spree.</p>"
        "<table class=infobox><tr><td>Population</td><td>3.7 million</td></tr></table>"
        "<details><summary><h2>History</h2></summary><p>First named in 1237.</p><ul><li>Prussia</li><li><p>The Wall</p></li></ul></details>"
        "<h2>References</h2><ol class=references><li>Ref text</li></ol>")),
    ("A/Lodz", "Łódź", wrap("Łódź", "<p>Łódź is a city in Poland. Kraków and Gdańsk too. Őrség is in Hungary.</p><p>Greek: αβγ. Tokyo: 東京.</p>")),
    ("A/Grusse", "Grüße", wrap("Grüße", "<p>A greeting in German, with ß and ü.</p>")),
    ("A/United_States", "United States", wrap("United States", "<p>The United States is a country.</p>")),
    ("A/Long", "Long article", wrap("Long article", LONG)),
    ("A/Tokyo", "東京", wrap("東京", "<p>Only a name the 397 cannot search.</p>")),
    ("A/Empty", "Empty", wrap("Empty", "<table><tr><td>only a table</td></tr></table>")),
]
pages += [(f"A/Item_{i:04d}", f"Item {i:04d}", wrap(f"Item {i:04d}", f"<p>Article number {i}.</p>")) for i in range(3000)]

with Creator(sys.argv[1]).config_indexing(False, "eng") as c:
    c.set_mainpath("A/Berlin")
    for p in pages: c.add_item(Page(*p))
    c.add_item(Page("I/pic.png", "pic", "\x89PNG fake", "image/png"))
    c.add_redirection("A/USA", "USA", "A/United_States", {Hint.FRONT_ARTICLE: True})
    c.add_redirection("A/Lodz_city", "Lodz", "A/Lodz", {Hint.FRONT_ARTICLE: True})
    for k, v in {"Title": "Bench Wikipedia", "Language": "eng", "Creator": "Wikipedia", "Publisher": "Kiwix",
                 "Date": "2026-10-27", "Description": "A small archive for the 397 bench", "Name": "bench_en"}.items():
        c.add_metadata(k, v)
