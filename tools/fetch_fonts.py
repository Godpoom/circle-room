"""Downloads the fonts bundled with Circle Room into fonts/ (copied to bin/fonts by build.ps1 and
shipped in CircleRoom_Portable.zip). Every family comes from github.com/google/fonts under the SIL
Open Font License 1.1 (or Apache-2.0); each folder keeps its licence file next to the fonts.

  python tools/fetch_fonts.py            # fetch what is missing
For each family it takes the regular and bold upright styles (or the variable font that covers them).
"""
import os
import re
import sys
import urllib.request

RAW = "https://raw.githubusercontent.com/google/fonts/main/"

THAI = ["Sarabun", "Prompt", "Kanit", "Mitr", "Chakra Petch", "Bai Jamjuree", "K2D", "KoHo", "Kodchasan", "Krub",
        "Mali", "Niramit", "Fahkwang", "Charm", "Charmonman", "Srisakdi", "Thasadith", "Taviraj", "Trirong", "Pridi",
        "Athiti", "Chonburi", "Itim", "Pattaya", "Sriracha", "Maitree", "Noto Sans Thai", "Noto Serif Thai",
        "Noto Sans Thai Looped", "IBM Plex Sans Thai", "IBM Plex Sans Thai Looped", "Anuphan"]
LATIN = ["Roboto", "Open Sans", "Lato", "Montserrat", "Poppins", "Raleway", "Oswald", "Merriweather",
         "Playfair Display", "Nunito", "Inter", "Work Sans", "Rubik", "Source Sans 3", "PT Sans", "PT Serif",
         "Noto Sans", "Noto Serif", "Lora", "Libre Baskerville", "Bebas Neue", "Anton", "Archivo Black",
         "Abril Fatface", "Alfa Slab One", "Lobster", "Pacifico", "Dancing Script", "Great Vibes", "Caveat",
         "Shadows Into Light", "Indie Flower", "Permanent Marker", "Satisfy", "Amatic SC", "Kalam", "Patrick Hand",
         "Comfortaa", "Quicksand", "Josefin Sans", "Fira Sans", "Fira Code", "JetBrains Mono", "Space Mono",
         "Space Grotesk", "DM Sans", "DM Serif Display", "Bitter", "Crimson Text", "EB Garamond",
         "Cormorant Garamond", "Cinzel", "Righteous", "Bangers", "Press Start 2P", "Fredoka", "Baloo 2", "Exo 2",
         "Orbitron", "Titillium Web", "Barlow", "Barlow Condensed", "Manrope", "Outfit", "Sora", "Urbanist", "Teko",
         "Zilla Slab", "Arvo", "Black Ops One", "Monoton", "Special Elite", "Courier Prime", "Abel", "Roboto Slab",
         "Roboto Mono", "Source Code Pro", "Libre Franklin", "Karla", "Mulish"]


def get(url):
    with urllib.request.urlopen(url, timeout=60) as r:
        return r.read()


def pick(meta):
    """(filename, style, weight) entries from METADATA.pb; regular + bold upright, or the variable roman."""
    fonts = []
    for block in re.findall(r"fonts\s*\{(.*?)\}", meta, re.S):
        fn = re.search(r'filename:\s*"([^"]+)"', block)
        st = re.search(r'style:\s*"([^"]+)"', block)
        wt = re.search(r"weight:\s*(\d+)", block)
        if fn:
            fonts.append((fn.group(1), st.group(1) if st else "normal", int(wt.group(1)) if wt else 400))
    upright = [f for f in fonts if f[1] == "normal"]
    variable = sorted({f[0] for f in upright if "[" in f[0]})
    if variable:
        return variable[:1]
    out = []
    for want in (400, 700):
        best = min(upright, key=lambda f: abs(f[2] - want), default=None)
        if best and best[0] not in out:
            out.append(best[0])
    return out


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fonts")
    os.makedirs(root, exist_ok=True)
    total = 0
    failed = []
    for fam in THAI + LATIN:
        slug = fam.lower().replace(" ", "")
        dest = os.path.join(root, fam.replace(" ", ""))
        if os.path.isdir(dest) and any(n.endswith((".ttf", ".otf")) for n in os.listdir(dest)):
            total += 1
            continue
        meta, base, lic = None, None, None
        for licdir, licfile in (("ofl", "OFL.txt"), ("apache", "LICENSE.txt")):
            try:
                meta = get(RAW + licdir + "/" + slug + "/METADATA.pb").decode("utf-8", "replace")
                base, lic = RAW + licdir + "/" + slug + "/", licfile
                break
            except Exception:
                continue
        if not meta:
            failed.append(fam)
            print("  missing:", fam)
            continue
        files = pick(meta)
        if not files:
            failed.append(fam)
            continue
        os.makedirs(dest, exist_ok=True)
        try:
            for f in files:
                data = get(base + urllib.request.quote(f))
                open(os.path.join(dest, f.replace("[", "-").replace("]", "").replace(",", "-")), "wb").write(data)
            open(os.path.join(dest, lic), "wb").write(get(base + lic))
            total += 1
            print("  ok:", fam, ", ".join(files))
        except Exception as e:
            failed.append(fam)
            print("  failed:", fam, e)
    size = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(root) for f in fs)
    print(f"{total} families in fonts/ ({size / 2**20:.1f} MB); failed: {failed or 'none'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
