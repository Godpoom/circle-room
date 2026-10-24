"""Builds the two release zips and prints their SHA-256 (and the exe's).

  CircleRoom_Portable.zip  exe + ai/ models + VC++ runtime + LICENSES/ + notices + อ่านก่อน.txt
  CircleRoom_Lite.zip      exe + LICENSES/ + notices + README.txt (no AI)

The VC++ runtime DLLs are taken from a Visual Studio redist folder (Distributable Code).
Usage, from the project root after build.ps1:
  python tools/package_release.py <version> <out_dir> [<vc_redist_crt_dir>]
"""
import glob
import hashlib
import os
import sys
import zipfile

VC = ["msvcp140.dll", "msvcp140_1.dll", "vcruntime140.dll", "vcruntime140_1.dll"]


def default_vc_dir():
    hits = sorted(glob.glob(os.path.join("C:/", "Program Files", "Microsoft Visual Studio", "*", "*", "VC", "Redist",
                                         "MSVC", "*", "x64", "Microsoft.VC14*.CRT")))
    return hits[-1] if hits else None


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    version, out_dir = sys.argv[1], sys.argv[2]
    vc_dir = sys.argv[3] if len(sys.argv) > 3 else default_vc_dir()
    assert vc_dir and all(os.path.isfile(os.path.join(vc_dir, f)) for f in VC), f"VC redist not found: {vc_dir}"
    root = os.getcwd()
    exe = os.path.join(root, "bin", "CircleRoom.exe")
    ai_dir = os.path.join(root, "bin", "ai")
    ai_files = sorted(f for f in os.listdir(ai_dir) if not f.lower().startswith("segformer") and f.lower() not in VC)
    lic = os.path.join(root, "LICENSES")
    readme_th = open(os.path.join(root, "CircleRoom_Portable", "อ่านก่อน.txt"), encoding="utf-8").read()
    os.makedirs(out_dir, exist_ok=True)

    def common(z):
        z.write(exe, "CircleRoom/CircleRoom.exe")
        z.write(os.path.join(root, "THIRD_PARTY_NOTICES.md"), "CircleRoom/THIRD_PARTY_NOTICES.md")
        for f in sorted(os.listdir(lic)):
            z.write(os.path.join(lic, f), "CircleRoom/LICENSES/" + f)

    full = os.path.join(out_dir, "CircleRoom_Portable.zip")
    with zipfile.ZipFile(full, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        common(z)
        z.writestr("CircleRoom/อ่านก่อน.txt", readme_th.encode("utf-8"))
        for f in ai_files:
            z.write(os.path.join(ai_dir, f), "CircleRoom/ai/" + f)
        for f in VC:
            z.write(os.path.join(vc_dir, f), "CircleRoom/ai/" + f)

    lite_readme = (f"Circle Room Lite {version}\r\n\r\n"
                   "รุ่นเล็ก: เครื่องมือแต่งภาพครบทุกตัว แต่ไม่มีระบบ AI (คัดภาพ AI, แต่งอัตโนมัติ, ปรับเฉพาะส่วน, กู้ส่วนที่สว่างเกิน)\r\n"
                   "ถ้าต้องการ AI ให้ดาวน์โหลด CircleRoom_Portable.zip แทน\r\n\r\n"
                   "วิธีใช้: แตกไฟล์ zip ก่อน แล้วดับเบิลคลิก CircleRoom.exe\r\n"
                   "สัญญาอนุญาตของส่วนประกอบทั้งหมดอยู่ในโฟลเดอร์ LICENSES\r\n\r\n"
                   "Lite build: every editing tool, no AI features. For AI, download CircleRoom_Portable.zip.\r\n"
                   "Licences for all components are in the LICENSES folder.\r\n")
    lite = os.path.join(out_dir, "CircleRoom_Lite.zip")
    with zipfile.ZipFile(lite, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        common(z)
        z.writestr("CircleRoom/README.txt", lite_readme.encode("utf-8-sig"))

    for p in (full, lite):
        with zipfile.ZipFile(p) as z:
            assert z.testzip() is None
            names = z.namelist()
        assert not any("segformer" in n.lower() or "settings.ini" in n.lower() for n in names)
        print(f"{os.path.basename(p):24s} {os.path.getsize(p) / 2**20:7.1f} MB  {len(names):3d} files")
    print("SHA-256")
    print("  CircleRoom.exe          ", sha256(exe))
    print("  CircleRoom_Portable.zip ", sha256(full))
    print("  CircleRoom_Lite.zip     ", sha256(lite))
    # SHA256SUMS.txt (attach to the release): every shipped binary and model, so anyone can
    # check that ai\ DLLs and models were not swapped (Get-FileHash <file>).
    sums = [(sha256(exe), "CircleRoom.exe"), (sha256(full), "CircleRoom_Portable.zip"), (sha256(lite), "CircleRoom_Lite.zip")]
    sums += [(sha256(os.path.join(ai_dir, f)), "ai/" + f) for f in ai_files]
    sums += [(sha256(os.path.join(vc_dir, f)), "ai/" + f) for f in VC]
    with open(os.path.join(out_dir, "SHA256SUMS.txt"), "w", encoding="utf-8", newline="\n") as fh:
        for h, name in sums:
            fh.write(f"{h}  {name}\n")
    print("  wrote", os.path.join(out_dir, "SHA256SUMS.txt"), f"({len(sums)} files)")


if __name__ == "__main__":
    main()
