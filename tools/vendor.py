#!/usr/bin/env python3
"""Put back what belongs to other people: source trees, and the fonts.

DOOM, MicroPython and the text stack (FreeType, HarfBuzz, SheenBidi,
libunibreak) are clones, not copies: they carry their own history and they are
large, so the repository pins them by commit here instead of holding them.
This restores exactly the commits the port was built and tested against.
Nobody's `main` moving under us is not an upgrade, it is a different porting
job, so the commits are exact and not a branch name.

The fonts are single files, so each is pinned twice over: by the commit in
its URL and by its SHA-256. A file that arrives different is refused.

Nothing of ours lives inside any of them, and that is deliberate: git
records a nested repository as a link and refuses to look inside, so anything
we kept in there would silently never be committed. Our MicroPython port is
third_party/mpy-xyuos and reaches into their tree through MPY_TOP; our DOOM
platform layer is userland/doomgeneric_xyuos.c.

    python3 tools/vendor.py          restore whatever is missing
    python3 tools/vendor.py --check  say what is missing, change nothing
"""
import hashlib, os, shutil, subprocess, sys, urllib.request

HOME = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

VENDOR = [
    {
        "path": "third_party/doomgeneric",
        "url": "https://github.com/ozkl/doomgeneric.git",
        "commit": "dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284",
        "why": "DOOM. Our platform layer is userland/doomgeneric_xyuos.c.",
    },
    {
        "path": "third_party/micropython",
        "url": "https://github.com/micropython/micropython.git",
        "commit": "1c3c201149f37fe8d81246191b3127bb198d6306",
        "why": "MicroPython. Our port is third_party/mpy-xyuos and finds "
               "this tree through MPY_TOP.",
    },
    # The text stack. Built by the Makefile into build/lib*.a; our side of it
    # is libtext/.
    {
        "path": "third_party/freetype",
        "url": "https://github.com/freetype/freetype.git",
        "commit": "0a0221a1347e2f1e07c395263540026e9a0aa7c7",
        "why": "FreeType 2.14.3: turns a glyph outline into pixels.",
    },
    {
        "path": "third_party/harfbuzz",
        "url": "https://github.com/harfbuzz/harfbuzz.git",
        "commit": "b42511e071162fe76102f613a6ccc009726c99af",
        "why": "HarfBuzz 12.3.2: turns characters into positioned glyphs.",
    },
    {
        "path": "third_party/sheenbidi",
        "url": "https://github.com/Tehreer/SheenBidi.git",
        "commit": "cfe430e7375a7845b679adae9d51dac6deaa8858",
        "why": "SheenBidi 3.0.0: the Unicode bidirectional algorithm.",
    },
    {
        "path": "third_party/libunibreak",
        "url": "https://github.com/adah1972/libunibreak.git",
        "commit": "28a2756b864c343f438cd22537d49d394d4666a5",
        "why": "libunibreak 8.0: where a line may break, where a character "
               "ends.",
    },
]

# Noto, under the SIL Open Font License, which asks for the licence to travel
# with the fonts -- so the three licence files go onto the disk beside them.
NOTO = ("https://raw.githubusercontent.com/notofonts/notofonts.github.io/"
        "f145d86c53996717bc4c25d4602eb9294e43dccc/")
CJK = ("https://raw.githubusercontent.com/notofonts/noto-cjk/"
       "f8d157532fbfaeda587e826d4cd5b21a49186f7c/")
GF = ("https://raw.githubusercontent.com/google/fonts/"
      "23e54b51ddffbc7713c583748e3bd86f62b1fa4a/")


def noto(name, sha):
    family = name.split("-")[0]
    return (name + ".ttf", NOTO + "fonts/%s/hinted/ttf/%s.ttf" % (family, name),
            sha)


FONT_DIR = "assets/fonts"
FONTS = [
    noto("NotoSans-Regular", "478c558ea716033cd60c03438f628dfa75694dcf6b5f6d505a2f05fd2b4f3823"),
    noto("NotoSans-Bold", "1df075a380fc7cb898acf64c1f7b3b4dd780de3caa860178bf929de35817a913"),
    noto("NotoSans-Italic", "467e3f89eeca4108bb8710a2b9e0cf2281ac56d5b0609211a83776d0505eecb5"),
    noto("NotoSans-BoldItalic", "1b602a9d6353be42c91df097a4857b69fa2696f26703d7a33b54a15d87c2622c"),
    noto("NotoSerif-Regular", "19e72cd8d595fae5bd74a5206f5d938512e1183d4fed7abb1ec1be1d7efa5f88"),
    noto("NotoSerif-Bold", "96656aa5cec8f1d6fd0e804c1fad397e1a1cfa082e6642124e0bda68cd8363ce"),
    noto("NotoSerif-Italic", "749e80e313ef711f9373c6cce17c72297ef05490b3dcda7967d1d5d90bf1183f"),
    noto("NotoSerif-BoldItalic", "c710c5b9cf354ae46e7a10472a08019b28220fafeb5887a482a76856f8f6fc0b"),
    noto("NotoSansMono-Regular", "65b5e2b2c4a1fba9ae8be1f026cb35b03dcb8886d9b2a4147054fde12f7e767d"),
    noto("NotoSansMono-Bold", "a21ea0ba6ea49fda7b34ca39a504b487f1130885d36e1a4f9f4255b3ba6994bc"),
    noto("NotoSansArabic-Regular", "bdff3e5659d67e67def05b33f749683b9376ae819d65d3dd62ac4640b3aaef48"),
    noto("NotoSansArabic-Bold", "4e5462d2e8be880317b9f49b5b2da109ddb6a3563d91cc604b67f3535832a555"),
    noto("NotoSansHebrew-Regular", "cdefaf8efd47045f6820928eba84db5bed7557539328952b5f828315485e02ee"),
    noto("NotoSansHebrew-Bold", "da9226e886c245a7e11673c24dec82bded64d8574c1e1f03983bf89297d2aaa8"),
    noto("NotoSansDevanagari-Regular", "4e3c66638958c3e2ab5d37f47a8deb89fffeb7be9985c665a519bbc7ba762313"),
    noto("NotoSansDevanagari-Bold", "6a09c8d797cfc803d32cdc731e809424d74cbaff59f503de34ade421a08e5bc2"),
    noto("NotoSansThai-Regular", "61cf814eec46b294d6ea4401ac295d0cecd5207bd2331dcc5a15e7301d30ee44"),
    noto("NotoSansThai-Bold", "2ac6c6e8a478e23b15f76e4894af1fa2210f8f350e4e6e54aad530bec03efbfb"),
    noto("NotoSansArmenian-Regular", "720df88c332417a235b4d6209d14ec2e2bf4bfe2a954b7453d869ea593bfce1e"),
    noto("NotoSansGeorgian-Regular", "d3e33254b09e7bb2c5cf0f17e554b80462056c5a107097f258d495168c3a9346"),
    noto("NotoSansMath-Regular", "d51afd5739c7ba6c44fcab35a88160e25dfb69a2d4ad0bd99533f8d894af1f96"),
    noto("NotoSansSymbols-Regular", "d0e98e9a2c046594c5021437273943be7e79e0fd980fde125279e22302212595"),
    noto("NotoSansSymbols2-Regular", "c4a0a80f0041ce4be81e2478faad22776d23edb98ae3f0d19bd37044820ecf9d"),
    # The rest of the scripts a page in the world's big languages is written
    # in, one regular face each: South and South-East Asia, Ethiopia, and the
    # ones Wikipedia's own list of editions turned up as boxes.
    noto("NotoSansBengali-Regular", "b55c62ee531e3214da6c0701daecea89a52ba42db7d8206b92e6b51f397a3193"),
    noto("NotoSansGurmukhi-Regular", "658d0207da305a1411c539a8b0bbeda64d4146e54fb4827facddb890b6b90d74"),
    noto("NotoSansGujarati-Regular", "9b5a7aaeeb649a2e75a49d8b006a1f87db1b61c0df3b001609f4e0725d88dbf6"),
    noto("NotoSansOriya-Regular", "a16645d056017927406546aa78e4ce15e782fd8783467267b75450453d007415"),
    noto("NotoSansTamil-Regular", "3c0a186feb3c63c7f6d63e1511dcdc144e745ae09b98e217c83f3e317974f6f9"),
    noto("NotoSansTelugu-Regular", "b274780b69d1d23fe84b55e809a152cb2ac5306d33864b1f87622f6971871aae"),
    noto("NotoSansKannada-Regular", "9ad74dc64838c6855b96f671fc08e425a58921b9d0c71712ea79c328a27e6e38"),
    noto("NotoSansMalayalam-Regular", "c08de7fa8d032a5d6a4d120fb82c78cec60b362a4e73fa26360d89759ff2a7f9"),
    noto("NotoSansSinhala-Regular", "9e32612d47004552f3125e78648a9e2e7899a216ccd3cefbb93a9b5f4c809feb"),
    noto("NotoSansMyanmar-Regular", "fafce4db400bc0b214907ccdbfb0ad2f18a57bfefd08c8a571830b84088cf2fc"),
    noto("NotoSansKhmer-Regular", "e66675f2082788f0511a714bef5a1748928294b38c8e286a96ea73a864b5e605"),
    noto("NotoSansLao-Regular", "0a86e5e1ccfe34ca78c43fac6829dc751b42bcc469272a9a55325aae587bfbe7"),
    noto("NotoSerifTibetan-Regular", "ee97bf3dc56e813651db734c9f35f8f1d41e7e31acf5f7d893e64ad22b292446"),
    noto("NotoSansEthiopic-Regular", "f6f7fc379db9438959a2b0527e7a2cf36ea9c84626d56ec444fff37fc24c3c10"),
    noto("NotoSansThaana-Regular", "7543935bdcef770c9d3dd54222651b29040e5ec82f3bc58542392b2c7a9cbd3c"),
    noto("NotoSansSyriac-Regular", "4440929bf1a47bb50179e8d8495641d208c63d607355e451f29ea2d3e5343290"),
    noto("NotoSansMongolian-Regular", "a28ba3cde3de22de7ddc934bd5d5babe54e6ce28c073a288cd978ffcf26b295b"),
    noto("NotoSansCherokee-Regular", "c052352137ae8d283840a0e2991a675d47859d8fdbae5726d373d4f0d97a8c87"),
    noto("NotoSansTifinagh-Regular", "8058054786bc572007193988654d1ded342bc2538c2d91c50d1a7238ea1a99cc"),
    noto("NotoSansNKo-Regular", "c756efb2c40f754107d76fa4e401fc3b8b7edec5cc65db549d3d0236ac6d08a1"),
    noto("NotoSansOlChiki-Regular", "4b1ad6ec4a30277c75fd66465b0b9ae489e19f8fdf44ef3179b6fda7f5ceb32d"),
    noto("NotoSansJavanese-Regular", "81fdea70d379989bafea65eae5a6a96144991b437415744716a49a56f09f747a"),
    noto("NotoSansCanadianAboriginal-Regular", "6f489ba696faff1d96f8ace395c0d22fe3b82621f69bf50b337112fb47ff1f17"),
    ("NotoSansCJKsc-Regular.otf",
     CJK + "Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf",
     "2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b"),
    ("NotoEmoji-Regular.ttf", GF + "ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf",
     "de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551"),
    ("LICENSE-Noto.txt", NOTO + "fonts/LICENSE",
     "f2095b08bed08b23a6fe26112fcd679a2bee3f002eef077eb05d215ed1051bd8"),
    ("LICENSE-NotoCJK.txt", CJK + "Sans/LICENSE",
     "6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2"),
    ("LICENSE-NotoEmoji.txt", GF + "ofl/notoemoji/OFL.txt",
     "500bb1ccf43df7bbb522112f9133a52b16e1c35e809632f5d8609b179152de5b"),
]

CHECK = "--check" in sys.argv


def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("failed: %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))
    return r.stdout.strip()


def head_of(path):
    r = subprocess.run(["git", "-C", path, "rev-parse", "HEAD"],
                       capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


missing = 0
for v in VENDOR:
    full = os.path.join(HOME, v["path"])
    at = head_of(full) if os.path.isdir(os.path.join(full, ".git")) else None

    if at == v["commit"]:
        print("%-30s already at %s" % (v["path"], v["commit"][:12]))
        continue

    if at:
        print("%-30s is at %s, wanted %s" % (v["path"], at[:12],
                                             v["commit"][:12]))
        if CHECK:
            missing += 1
            continue
        run(["git", "-C", full, "fetch", "--depth", "50", "origin",
             v["commit"]])
        run(["git", "-C", full, "checkout", "--detach", v["commit"]])
        print("%-30s moved to %s" % (v["path"], v["commit"][:12]))
        continue

    print("%-30s MISSING -- %s" % (v["path"], v["why"]))
    missing += 1
    if CHECK:
        continue

    if os.path.isdir(full) and not os.listdir(full):
        os.rmdir(full)
    if os.path.isdir(full):
        sys.exit("%s exists and is not a clone; move it aside first" % full)

    print("   cloning %s" % v["url"])
    run(["git", "clone", "--filter=blob:none", v["url"], full])
    run(["git", "-C", full, "checkout", "--detach", v["commit"]])
    print("%-30s restored at %s" % (v["path"], v["commit"][:12]))

def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


os.makedirs(os.path.join(HOME, FONT_DIR), exist_ok=True)
for name, url, sha in FONTS:
    full = os.path.join(HOME, FONT_DIR, name)
    if os.path.exists(full) and sha256_of(full) == sha:
        continue
    print("%-30s MISSING" % (FONT_DIR + "/" + name))
    missing += 1
    if CHECK:
        continue
    tmp = full + ".part"
    try:
        with urllib.request.urlopen(url, timeout=120) as r, open(tmp, "wb") as f:
            shutil.copyfileobj(r, f)
    except OSError as e:
        sys.exit("could not fetch %s: %s" % (url, e))
    got = sha256_of(tmp)
    if got != sha:
        os.remove(tmp)
        sys.exit("%s arrived different from the one pinned:\n  wanted %s\n  got    %s"
                 % (name, sha, got))
    os.replace(tmp, full)
    print("%-30s fetched" % (FONT_DIR + "/" + name))

if CHECK and missing:
    print("\n%d item(s) missing; run without --check to restore them." % missing)
    sys.exit(1)
print("\nvendored trees and fonts are in place")
