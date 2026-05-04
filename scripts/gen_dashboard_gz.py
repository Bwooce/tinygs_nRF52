#!/usr/bin/env python3
"""Generate src/web/dashboard_html_gz.h — the dashboard HTML, with the
worldmap SVG inlined and the whole thing gzipped, then embedded as a
C uint8_t[] for the static-resource handler to serve with
`Content-Encoding: gzip`.

Why gzip: /dashboard is ~24 KB plain. Thread MTD throughput to a LAN
host is ~50-60 kbps, so plain transfer takes ~3.5 s. After gzip the
HTML drops to ~6 KB (heavy repetition in the SVG <rect> strips
compresses extremely well), giving ~0.8 s.

Run once after editing the dashboard template or src/worldmap.h:
    python3 scripts/gen_dashboard_gz.py
"""

from __future__ import annotations
import gzip
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WM_H = ROOT / "src" / "worldmap.h"
OUT = ROOT / "src" / "web" / "dashboard_html_gz.h"

DASHBOARD_PRE = """\
<!doctype html><html><head><meta charset='utf-8'>\
<meta name='viewport' content='width=device-width,initial-scale=1'>\
<title>TinyGS nRF52</title>\
<style>\
body{font-family:Arial,sans-serif;margin:0;padding:10px;text-align:center;}\
h1{margin:0 0 10px 0;font-size:1.2em}\
table{margin:0 auto;}h3{margin:0 0 6px 0;text-align:center;}\
.card{height:12em;margin:10px;text-align:left;font-family:Arial;\
border:3px groove;border-radius:0.3rem;display:inline-block;\
padding:10px;min-width:260px;vertical-align:top;}\
td{padding:0 10px;}td:first-child{color:#666;}\
textarea{resize:vertical;width:100%;margin:0;height:200px;padding:5px;\
overflow:auto;font-family:monospace;font-size:0.85em;}\
#c1{width:98%;padding:5px;}\
.console{display:inline-block;text-align:center;margin:10px 0;width:98%;max-width:1080px;}\
.G{color:green;}.R{color:red;}\
svg{display:block;margin:0 auto 10px;border:1px solid #aaa;}\
</style></head><body>\
<h1>TinyGS nRF52 - <a href='/'>home</a> &middot; <a href='/restart' \
onclick=\"return confirm('Reboot the station?')\">restart</a></h1>\
<svg width='480' height='270' viewBox='0 0 240 135' \
xmlns='http://www.w3.org/2000/svg'>\
<rect x='0' y='0' width='240' height='135' fill='#0a1830' stroke='#444' stroke-width='1'/>\
<line x1='0' y1='67.5' x2='240' y2='67.5' stroke='#234' stroke-width='0.5'/>\
<line x1='120' y1='0' x2='120' y2='135' stroke='#234' stroke-width='0.5'/>\
<g fill='#3d6' stroke='none'>\
"""

DASHBOARD_POST = """\
</g>\
<circle id='wmsatpos' cx='120' cy='67' r='3' stroke='red' fill='none' stroke-width='1.5'>\
<animate attributeName='r' values='2;5;2' dur='1.5s' repeatCount='indefinite'/>\
</circle></svg>\
<div class='card'><h3>Groundstation</h3><table id='gsstatus'>\
<tr><td>Name</td><td></td></tr>\
<tr><td>Version</td><td></td></tr>\
<tr><td>MQTT</td><td></td></tr>\
<tr><td>Parent RSSI</td><td></td></tr>\
<tr><td>Radio</td><td></td></tr>\
<tr><td>Last RSSI</td><td></td></tr>\
</table></div>\
<div class='card'><h3>Modem</h3><table id='modemconfig'>\
<tr><td>Modulation</td><td></td></tr>\
<tr><td>Frequency</td><td></td></tr>\
<tr><td>Freq. Offset</td><td></td></tr>\
<tr><td>Spreading Factor</td><td></td></tr>\
<tr><td>Coding Rate</td><td></td></tr>\
<tr><td>Bandwidth</td><td></td></tr>\
</table></div>\
<div class='card'><h3>Satellite</h3><table id='satdata'>\
<tr><td>Listening to</td><td></td></tr>\
<tr><td>Lat / Lon</td><td></td></tr>\
<tr><td>Az / El</td><td></td></tr>\
<tr><td>Doppler</td><td></td></tr>\
<tr><td>UTC Time</td><td></td></tr>\
<tr><td>Local Time</td><td></td></tr>\
</table></div>\
<div class='card'><h3>Last Packet</h3><table id='lastpacket'>\
<tr><td>Received</td><td></td></tr>\
<tr><td>RSSI</td><td></td></tr>\
<tr><td>SNR</td><td></td></tr>\
<tr><td>Freq err</td><td></td></tr>\
<tr><td colspan='2' style='text-align:center;'></td></tr>\
</table></div>\
<div class='console'>\
<textarea readonly id='t1' wrap='off'></textarea>\
<form method='get' onsubmit='return f(1);'>\
<input id='c1' placeholder='(commands disabled - read-only console)' disabled>\
</form></div>\
<script>\
var x=null,lt,sn=0,id=0;\
function f(p){var c,o='',t;clearTimeout(lt);\
t=document.getElementById('t1');\
if(t.scrollTop>=sn){\
if(x!=null){x.abort();}\
x=new XMLHttpRequest();\
x.onreadystatechange=function(){\
if(x.readyState==4&&x.status==200){\
var a=x.responseText;\
id=a.substr(0,a.indexOf('\\n')).replace(/^seq:\\s*/,'');\
var z=a.substr(a.indexOf('\\n')+1);\
if(z.length>0){t.value+=z;}\
t.scrollTop=99999;sn=t.scrollTop;}};\
x.open('GET','cs?c2='+id,true);x.send();}\
lt=setTimeout(f,2345);return false;}\
var wmx=null,wmt;\
function wmf(){var sp,mc,gs,sd,lp;clearTimeout(wmt);\
wmx=new XMLHttpRequest();\
wmx.onreadystatechange=function(){\
if(wmx.readyState==4&&wmx.status==200){\
var wmp=wmx.responseText.split(',');\
sp=document.getElementById('wmsatpos');\
sp.setAttribute('cx',wmp[0]);sp.setAttribute('cy',wmp[1]);\
mc=document.getElementById('modemconfig');\
for(let r=0;r<6;r++)mc.rows[r].cells[1].innerHTML=wmp[r+2];\
if(wmp[2]=='LoRa'){mc.rows[3].cells[0].innerHTML='Spreading Factor';\
mc.rows[4].cells[0].innerHTML='Coding Rate';}\
else{mc.rows[3].cells[0].innerHTML='Bitrate';\
mc.rows[4].cells[0].innerHTML='Frequency dev';}\
gs=document.getElementById('gsstatus');\
for(let r=0;r<6;r++)gs.rows[r].cells[1].innerHTML=wmp[r+8];\
sd=document.getElementById('satdata');\
for(let r=0;r<6;r++)sd.rows[r].cells[1].innerHTML=wmp[r+14];\
lp=document.getElementById('lastpacket');\
for(let r=0;r<4;r++)lp.rows[r].cells[1].innerHTML=wmp[r+20];\
lp.rows[4].cells[0].innerHTML=wmp[24];}};\
wmx.open('GET','wm',true);wmx.send();\
wmt=setTimeout(wmf,5000);return false;}\
window.addEventListener('load',function(){f();wmf();});\
</script></body></html>\
"""


def parse_worldmap_h(text: str):
    width = int(re.search(r"#define\s+WORLDMAP_W\s+(\d+)", text).group(1))
    height = int(re.search(r"#define\s+WORLDMAP_H\s+(\d+)", text).group(1))
    body = re.search(r"\{([^}]+)\}", text, re.DOTALL).group(1)
    bytes_ = bytes(int(b, 0) for b in re.findall(r"0x[0-9A-Fa-f]+", body))
    return width, height, bytes_


def land_runs(width: int, height: int, bits: bytes):
    for y in range(height):
        x = 0
        while x < width:
            while x < width:
                idx = y * width + x
                if (bits[idx >> 3] >> (idx & 7)) & 1:
                    break
                x += 1
            if x >= width:
                break
            run_start = x
            while x < width:
                idx = y * width + x
                if not ((bits[idx >> 3] >> (idx & 7)) & 1):
                    break
                x += 1
            yield (y, run_start, x - run_start)


def build_svg_runs(width: int, height: int, bits: bytes) -> str:
    parts = []
    for y, x, n in land_runs(width, height, bits):
        parts.append(f"<rect x='{x}' y='{y}' width='{n}' height='1'/>")
    return "".join(parts)


def main():
    text = WM_H.read_text()
    width, height, bits = parse_worldmap_h(text)
    svg = build_svg_runs(width, height, bits)
    html = DASHBOARD_PRE + svg + DASHBOARD_POST
    raw_size = len(html.encode("utf-8"))

    # gzip with maximum compression (compresslevel=9, mtime=0 for reproducible builds).
    gz = gzip.compress(html.encode("utf-8"), compresslevel=9, mtime=0)
    gz_size = len(gz)

    print(f"raw HTML: {raw_size} bytes")
    print(f"gzipped:  {gz_size} bytes ({100 * gz_size / raw_size:.1f}%)")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("w") as f:
        f.write("/*\n")
        f.write(" * Auto-generated by scripts/gen_dashboard_gz.py.\n")
        f.write(f" * Raw HTML: {raw_size} bytes; gzipped: {gz_size} bytes.\n")
        f.write(" * Re-run the generator if dashboard template or worldmap.h changes.\n")
        f.write(" */\n\n")
        f.write("#pragma once\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"static const uint8_t DASHBOARD_HTML_GZ[{gz_size}] = {{\n")
        for i in range(0, gz_size, 16):
            chunk = gz[i:i+16]
            f.write("    " + ", ".join(f"0x{b:02x}" for b in chunk))
            if i + 16 < gz_size:
                f.write(",")
            f.write("\n")
        f.write("};\n")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
