"""Store listings for a release: winget, Scoop, the MCP Registry and an MCP Bundle (.mcpb).

Run from the project root after tools/package_release.py, with the folder it wrote:
  python tools/make_store_manifests.py <version> <release_dir> [<release_date YYYY-MM-DD>]

Writes (and prints what to submit):
  <release_dir>/CircleRoom-mcp.mcpb       one-click install for Claude Desktop and MCP clients
                                          (the Lite build: every editing, text and drawing tool;
                                          AI cull / auto edit need the full download)
  packaging/winget/<version>/*.yaml       for a pull request to microsoft/winget-pkgs
  bucket/circleroom.json                  Scoop: scoop bucket add circleroom https://github.com/Godpoom/circle-room
  server.json                             MCP Registry: mcp-publisher publish
The version must be numeric (1.3.3): stores and the in-app updater compare numbers.
"""
import datetime
import hashlib
import json
import os
import re
import sys
import zipfile

REPO = "https://github.com/Godpoom/circle-room"
SITE = "https://godpoom.github.io/circle-room/"
RAW_ICON = "docs/assets/icon-180.png"

SHORT = "Free, light photo editor for Windows with on-device AI and an MCP server for AI agents."
LONG = ("Circle Room is a free photo editor for Windows 10/11: Lightroom-style light, colour, tone curve, HSL, "
        "colour grading, crop and looks; camera RAW; AI that runs on your computer to pick the best shots in a "
        "folder, auto-edit many photos, erase objects and rebuild blown highlights; layers with text (Thai and "
        "Latin fonts), vector drawing and Make your font; optional image generation through OpenRouter with your "
        "own key. It is also an MCP server, so Claude, Codex, Cursor, Gemini CLI and other agents can edit, draw "
        "and export photos and see every step. A ~3 MB program that uses about 15 MB of RAM. English and Thai.")
TAGS = ["photo-editor", "image-editor", "raw", "lightroom-alternative", "ai", "photo-culling", "batch-editing",
        "magic-eraser", "layers", "mcp", "mcp-server", "thai", "photography", "free"]

# Short tool list for the bundle manifest (what an MCP client shows before installing).
TOOLS = [
    ("open_photo", "Open a photo and see it"), ("view", "Render the photo with its edits, or a detail crop"),
    ("get_info", "Size, every edit, regions and layers"), ("list_adjustments", "Every adjustment and its range"),
    ("set_adjustments", "Light, colour, curves, HSL, grading, detail, effects"), ("auto_edit", "AI edit for the scene"),
    ("apply_look", "Apply a ready-made look"), ("crop_rotate", "Crop, aspect and straighten"),
    ("find_regions", "Find people, faces, sky, water... in the photo"), ("adjust_region", "Edit one area only"),
    ("recover_highlights", "Rebuild blown-out whites"), ("magic_erase", "Remove people, wires, litter"),
    ("layer_add", "Add a photo layer, with AI cut-out"), ("new_canvas", "Start a blank canvas"),
    ("add_text", "Text layer in any font, Thai included"), ("draw_svg", "Vector drawing from SVG"),
    ("list_fonts", "Fonts usable for text"), ("layer_update", "Move, resize, restyle a layer"),
    ("layer_remove", "Remove a layer"), ("undo", "Step back"), ("export", "Full-size JPEG or PNG"),
    ("cull_folder", "Pick the best photos in a folder"), ("search_photos", "Search a folder by meaning (EN/TH)"),
    ("batch_auto_edit", "AI-edit many photos at once"),
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def build_mcpb(version, rel_dir, root):
    manifest = {
        "manifest_version": "0.3",
        "name": "circle-room",
        "display_name": "Circle Room",
        "version": version,
        "description": "Photo editing, text, drawing and export for AI agents, on Windows.",
        "long_description": LONG + " This bundle carries the Lite build: every editing, text and drawing tool. "
                                   "For AI cull, auto edit and the other on-device AI tools, use the full download "
                                   "from the website and its Connect AI button.",
        "author": {"name": "IAMPOOM", "url": "https://github.com/Godpoom"},
        "homepage": SITE,
        "repository": {"type": "git", "url": REPO + ".git"},
        "icon": "icon.png",
        "license": "MIT",
        "keywords": ["photo", "image", "editor", "raw", "text", "svg", "thai", "windows"],
        "server": {
            "type": "binary",
            "entry_point": "server/CircleRoom.exe",
            "mcp_config": {"command": "${__dirname}/server/CircleRoom.exe", "args": ["--mcp"], "env": {}},
        },
        "compatibility": {"platforms": ["win32"]},
        "tools": [{"name": n, "description": d} for n, d in TOOLS],
    }
    out = os.path.join(rel_dir, "CircleRoom-mcp.mcpb")
    if os.path.isfile(out):  # already built (maybe published): rebuilding would change its hash
        return out
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr("manifest.json", json.dumps(manifest, indent=2, ensure_ascii=False))
        z.write(os.path.join(root, RAW_ICON), "icon.png")
        z.write(os.path.join(root, "bin", "CircleRoom.exe"), "server/CircleRoom.exe")
        z.write(os.path.join(root, "LICENSE"), "server/LICENSE.txt")
        z.write(os.path.join(root, "THIRD_PARTY_NOTICES.md"), "server/THIRD_PARTY_NOTICES.md")
        lic = os.path.join(root, "LICENSES")
        for f in sorted(os.listdir(lic)):
            z.write(os.path.join(lic, f), "server/LICENSES/" + f)
    return out


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    version, rel_dir = sys.argv[1], sys.argv[2]
    date = sys.argv[3] if len(sys.argv) > 3 else datetime.date.today().isoformat()
    assert re.fullmatch(r"\d+\.\d+\.\d+", version), "numeric version only, e.g. 1.3.3"
    root = os.getcwd()
    portable = os.path.join(rel_dir, "CircleRoom_Portable.zip")
    assert os.path.isfile(portable), "run tools/package_release.py first"
    tag = "v" + version
    dl = f"{REPO}/releases/download/{tag}/"
    portable_sha = sha256(portable)

    mcpb = build_mcpb(version, rel_dir, root)
    mcpb_sha = sha256(mcpb)

    # winget (manifest schema 1.28.0): the Portable zip, extracted whole; "circleroom" on PATH.
    wdir = os.path.join(root, "packaging", "winget", version)
    os.makedirs(wdir, exist_ok=True)
    pid = "Godpoom.CircleRoom"
    head = "# yaml-language-server: $schema=https://aka.ms/winget-manifest.{kind}.1.28.0.schema.json\n\n"
    files = {
        f"{pid}.yaml": head.format(kind="version") +
        f"PackageIdentifier: {pid}\nPackageVersion: {version}\nDefaultLocale: en-US\nManifestType: version\nManifestVersion: 1.28.0\n",
        f"{pid}.installer.yaml": head.format(kind="installer") +
        f"PackageIdentifier: {pid}\nPackageVersion: {version}\nInstallerType: zip\nNestedInstallerType: portable\n"
        "NestedInstallerFiles:\n- RelativeFilePath: CircleRoom\\CircleRoom.exe\n  PortableCommandAlias: circleroom\n"
        f"MinimumOSVersion: 10.0.17763.0\nReleaseDate: {date}\nInstallers:\n- Architecture: x64\n"
        f"  InstallerUrl: {dl}CircleRoom_Portable.zip\n  InstallerSha256: {portable_sha.upper()}\n"
        "ManifestType: installer\nManifestVersion: 1.28.0\n",
        f"{pid}.locale.en-US.yaml": head.format(kind="defaultLocale") +
        f"PackageIdentifier: {pid}\nPackageVersion: {version}\nPackageLocale: en-US\nPublisher: IAMPOOM\n"
        f"PublisherUrl: https://github.com/Godpoom\nPublisherSupportUrl: {REPO}/issues\nPackageName: Circle Room\n"
        f"PackageUrl: {SITE}\nLicense: MIT\nLicenseUrl: {REPO}/blob/main/LICENSE\nCopyright: (c) 2026 IAMPOOM\nShortDescription: {json.dumps(SHORT)}\n"
        f"Description: {json.dumps(LONG)}\nMoniker: circleroom\nTags:\n"  # quoted: the text contains ": " + "".join(f"- {t}\n" for t in TAGS) +
        f"ReleaseNotesUrl: {REPO}/releases/tag/{tag}\nManifestType: defaultLocale\nManifestVersion: 1.28.0\n",
    }
    for name, text in files.items():
        with open(os.path.join(wdir, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    # Scoop: this repo is the bucket.
    os.makedirs(os.path.join(root, "bucket"), exist_ok=True)
    scoop = {
        "version": version,
        "description": SHORT,
        "homepage": SITE,
        "license": "MIT",
        "architecture": {"64bit": {"url": dl + "CircleRoom_Portable.zip", "hash": portable_sha}},
        "extract_dir": "CircleRoom",
        "bin": [["CircleRoom.exe", "circleroom"]],
        "shortcuts": [["CircleRoom.exe", "Circle Room"]],
        "persist": ["presets", "styles"],
        "checkver": {"github": REPO},
        "autoupdate": {
            "architecture": {"64bit": {"url": REPO + "/releases/download/v$version/CircleRoom_Portable.zip"}},
            "hash": {"url": "$baseurl/SHA256SUMS.txt"},
        },
    }
    with open(os.path.join(root, "bucket", "circleroom.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(scoop, f, indent=4)
        f.write("\n")

    # MCP Registry
    server = {
        "$schema": "https://static.modelcontextprotocol.io/schemas/2025-12-11/server.schema.json",
        "name": "io.github.Godpoom/circle-room",
        "title": "Circle Room",
        "description": "Windows photo editor for agents: edit, cull, erase, text, SVG drawing, export.",
        "websiteUrl": SITE,
        "repository": {"url": REPO, "source": "github"},
        "version": version,
        "packages": [{"registryType": "mcpb", "identifier": dl + "CircleRoom-mcp.mcpb", "fileSha256": mcpb_sha,
                      "transport": {"type": "stdio"}}],
    }
    assert len(server["description"]) <= 100
    with open(os.path.join(root, "server.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(server, f, indent=2)
        f.write("\n")

    print(f"Portable {portable_sha}\nmcpb     {mcpb_sha}  {os.path.getsize(mcpb) / 1e6:.1f} MB  ({mcpb})")
    print(f"winget   {wdir}\nscoop    bucket/circleroom.json\nregistry server.json")
    print(f"Upload CircleRoom-mcp.mcpb to the {tag} release before publishing server.json.")


if __name__ == "__main__":
    main()
