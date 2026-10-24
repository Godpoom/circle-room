"""Updates the website's safety section and the README for a release.

Pending (just released, not scanned yet):
  python tools/set_scan_status.py pending <ver> <exe_sha> <portable_sha> <lite_sha> <prev_ver> <prev_exe_sha> <prev_date_th> <prev_date_en>
Scanned (after the VirusTotal result is in):
  python tools/set_scan_status.py scanned <ver> <exe_sha> <portable_sha> <lite_sha> <flagged> <engines> <date_th> <date_en> [<note_th> <note_en>]
  The optional note explains a detection (e.g. a machine-learning false positive).
"""
import re
import sys

mode, ver, exe, full, lite = sys.argv[1:6]
rest = sys.argv[6:]
SITE, README = "docs/index.html", "README.md"
s = open(SITE, encoding="utf-8").read()

a = s.index('<section class="safe" id="safety">')
b = s.index("</section>", a)
sec = s[a:b]

# hashes always describe the current release
sec = re.sub(r'(<span class="k">CircleRoom\.exe</span><code>)[0-9a-f]{64}(</code><button class="copy" type="button" data-copy=")[0-9a-f]{64}',
             rf"\g<1>{exe}\g<2>{exe}", sec)
sec = re.sub(r'(<span class="k">Portable\.zip</span><code>)[0-9a-f]{64}(</code><button class="copy" type="button" data-copy=")[0-9a-f]{64}',
             rf"\g<1>{full}\g<2>{full}", sec)
sec = re.sub(r'(<span class="k">Lite\.zip</span><code>)[0-9a-f]{64}(</code><button class="copy" type="button" data-copy=")[0-9a-f]{64}',
             rf"\g<1>{lite}\g<2>{lite}", sec)

head_start = sec.index('      <h2>')
head_end = sec.index('      </a>', head_start) + len('      </a>')
if mode == "pending":
    prev_ver, prev_exe, prev_th, prev_en = rest
    head = f'''      <p class="pending"><span data-th>เวอร์ชัน {ver} · กำลังรอผลสแกน</span><span data-en>Version {ver} · scan pending</span></p>
      <h2><span data-th>เวอร์ชันก่อนหน้า ไม่มีแอนตี้ไวรัสตัวไหนแจ้ง</span><span data-en>The previous version: nothing flagged</span></h2>
      <p><span data-th>ผล 0/71 นี้เป็นของ <b>CircleRoom.exe {prev_ver}</b> (สแกนวันที่ {prev_th}) ส่วนเวอร์ชันล่าสุด {ver} เพิ่งออก กำลังรอผลสแกน ระหว่างนี้แนะนำให้สแกนไฟล์ที่คุณโหลดมาเองตามขั้นตอนด้านขวา</span>
         <span data-en>This 0/71 is for <b>CircleRoom.exe {prev_ver}</b> (scanned {prev_en}). Version {ver} has just shipped and its scan is pending; until then, please scan your download yourself using the steps on the right.</span></p>
      <a class="vt" href="https://www.virustotal.com/gui/file/{prev_exe}" target="_blank" rel="noopener">
        <span data-th>ดูรายงานของ {prev_ver} บน VirusTotal</span><span data-en>Open the {prev_ver} report on VirusTotal</span>
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><path d="M7 17 17 7M8 7h9v9"/></svg>
      </a>'''
    # The ring shows the previous version's clean result, matching the text beside it.
    sec = re.sub(r'<b>\d+</b><span>/ \d+</span>', "<b>0</b><span>/ 71</span>", sec)
    sec = re.sub(r'aria-label="\d+ of \d+ antivirus', 'aria-label="0 of 71 antivirus', sec)
    sec = re.sub(r'data-flagged="\d+"', 'data-flagged="0"', sec)
    sec = re.sub(r'(<span data-en>flagged</span> )\d+', r'\g<1>0', sec)
    step1 = ('<span data-th>เช็คว่าไฟล์ที่โหลดมาตรงกับที่เราปล่อย</span><span data-en>Check your file matches our release</span>')
    readme = (f"> **ตรวจความปลอดภัย:** เวอร์ชัน {ver} กำลังรอผลสแกน VirusTotal (เวอร์ชัน {prev_ver} ได้ 0/71 "
              f"[รายงาน](https://www.virustotal.com/gui/file/{prev_exe}))\n"
              f"> SHA-256 ของ `CircleRoom.exe` {ver}: `{exe}` ตรวจเองได้ด้วย `Get-FileHash .\\CircleRoom.exe` "
              f"และแนะนำให้สแกนไฟล์ที่โหลดมาอีกครั้งเพื่อความปลอดภัย\n")
else:
    flagged, engines, date_th, date_en = rest[:4]
    note_th, note_en = (rest[4], rest[5]) if len(rest) >= 6 else ("", "")
    note = (f'''
      <p class="scannote"><span data-th>{note_th}</span><span data-en>{note_en}</span></p>''' if note_th else "")
    head = f'''      <h2><span data-th>{"ไม่มีแอนตี้ไวรัสตัวไหนแจ้งว่าอันตราย" if flagged == "0" else f"แอนตี้ไวรัส {flagged} ตัวแจ้งเตือน"}</span><span data-en>{"No antivirus engine flagged it" if flagged == "0" else (f"{flagged} engine flagged it" if flagged == "1" else f"{flagged} engines flagged it")}</span></h2>
      <p><span data-th>เราส่ง <b>CircleRoom.exe</b> เวอร์ชัน {ver} ให้ VirusTotal สแกนด้วยแอนตี้ไวรัส {engines} ตัว ผลคือพบ {flagged} (สแกนวันที่ {date_th})</span>
         <span data-en>We sent <b>CircleRoom.exe</b> {ver} to VirusTotal, which ran it past {engines} antivirus engines: {flagged} flagged it (scanned {date_en}).</span></p>{note}
      <a class="vt" href="https://www.virustotal.com/gui/file/{exe}" target="_blank" rel="noopener">
        <span data-th>ดูรายงานบน VirusTotal</span><span data-en>Open the VirusTotal report</span>
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><path d="M7 17 17 7M8 7h9v9"/></svg>
      </a>'''
    sec = re.sub(r'<b>\d+</b><span>/ \d+</span>', f"<b>{flagged}</b><span>/ {engines}</span>", sec)
    sec = re.sub(r'aria-label="\d+ of \d+ antivirus', f'aria-label="{flagged} of {engines} antivirus', sec)
    sec = re.sub(r'data-flagged="\d+"', f'data-flagged="{flagged}"', sec)
    sec = re.sub(r'(<span data-en>flagged</span> )\d+', rf'\g<1>{flagged}', sec)
    step1 = ('<span data-th>เช็คว่าไฟล์ที่โหลดมาเป็นไฟล์เดียวกับที่สแกน</span><span data-en>Check your file is the one we scanned</span>')
    readme = (f"> **ตรวจความปลอดภัย:** VirusTotal สแกน `CircleRoom.exe` {ver} แล้ว **{flagged}/{engines}** "
              f"([รายงาน](https://www.virustotal.com/gui/file/{exe})){(' ' + note_th) if note_th else ''}\n"
              f"> SHA-256 ของ `CircleRoom.exe`: `{exe}` ตรวจเองได้ด้วย `Get-FileHash .\\CircleRoom.exe` "
              f"และแนะนำให้สแกนไฟล์ที่โหลดมาอีกครั้งเพื่อความปลอดภัย\n")
pend_start = sec.find('      <p class="pending">')
if 0 <= pend_start < head_start:
    head_start = pend_start
sec = sec[:head_start] + head + sec[head_end:]
sec = re.sub(r'<h3><span data-th>เช็คว่าไฟล์ที่โหลดมา[^<]*</span><span data-en>[^<]*</span></h3>', f"<h3>{step1}</h3>", sec)
s = s[:a] + sec + s[b:]

if ".pending{" not in s:
    s = s.replace("/* ---------- before / after ---------- */",
                  ".pending{display:inline-block;margin:0 0 var(--s3);padding:4px 10px;border:1px solid var(--red);border-radius:2px;"
                  "font-family:var(--f-mono);font-size:12.5px;letter-spacing:.08em;color:var(--red)}\n"
                  "/* ---------- before / after ---------- */", 1)
s = re.sub(r'<span class="js-ver">v[\d.]+</span>', f'<span class="js-ver">v{ver}</span>', s)
open(SITE, "w", encoding="utf-8").write(s)

r = open(README, encoding="utf-8").read()
r = re.sub(r"> \*\*ตรวจความปลอดภัย:\*\*.*\n> SHA-256 ของ `CircleRoom\.exe`.*\n", readme.replace("\\", "\\\\"), r)
open(README, "w", encoding="utf-8").write(r)
print("ok", mode, ver)
