/*
 * Static dashboard HTML — served from /dashboard.
 *
 * Skeleton ports the ESP32 IoTWebConf dashboard: card layout (CSS),
 * empty data tables (id=modemconfig|gsstatus|satdata|lastpacket),
 * a console textarea, and the same JS that polls /cs and /wm.
 *
 * The world map SVG is a placeholder (bordered viewBox + animated red
 * dot at the sat position). The ESP32 builds a per-pixel <rect> map
 * from its 128×64 XBM at request time; we'll do the equivalent in a
 * follow-up using src/worldmap.h.
 *
 * One bug intentionally fixed vs ESP32: the original WORLDMAP_SCRIPT
 * checks `x.status` (the *console* XHR) instead of `wmx.status`. With
 * the console XHR aborted between polls, that read can be stale and
 * skip the table update. We use `wmx.status` here.
 */

#pragma once

static const char DASHBOARD_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>TinyGS nRF52</title>"
"<style>"
"body{font-family:Arial,sans-serif;margin:0;padding:10px;text-align:center;}"
"h1{margin:0 0 10px 0;font-size:1.2em}"
"table{margin:0 auto;}h3{margin:0 0 6px 0;text-align:center;}"
".card{height:12em;margin:10px;text-align:left;font-family:Arial;"
"border:3px groove;border-radius:0.3rem;display:inline-block;"
"padding:10px;min-width:260px;vertical-align:top;}"
"td{padding:0 10px;}td:first-child{color:#666;}"
"textarea{resize:vertical;width:100%;margin:0;height:200px;padding:5px;"
"overflow:auto;font-family:monospace;font-size:0.85em;}"
"#c1{width:98%;padding:5px;}"
".console{display:inline-block;text-align:center;margin:10px 0;width:98%;max-width:1080px;}"
".G{color:green;}.R{color:red;}"
"svg{display:block;margin:0 auto 10px;border:1px solid #aaa;}"
"</style></head><body>"

"<h1>TinyGS nRF52 — <a href='/'>raw status</a> · <a href='/restart' "
"onclick=\"return confirm('Reboot the station?')\">restart</a></h1>"

"<svg width='480' height='270' viewBox='0 0 240 135' "
"xmlns='http://www.w3.org/2000/svg'>"
"<rect x='0' y='0' width='240' height='135' fill='#0a1830' stroke='#444' stroke-width='1'/>"
/* Equator + prime meridian guides — placeholder for proper world map. */
"<line x1='0' y1='67.5' x2='240' y2='67.5' stroke='#234' stroke-width='0.5'/>"
"<line x1='120' y1='0' x2='120' y2='135' stroke='#234' stroke-width='0.5'/>"
"<text x='4' y='12' fill='#456' font-size='6'>worldmap TBD</text>"
"<circle id='wmsatpos' cx='120' cy='67' r='3' stroke='red' fill='none' stroke-width='1.5'>"
"<animate attributeName='r' values='2;5;2' dur='1.5s' repeatCount='indefinite'/>"
"</circle></svg>"

/* Cards. The label column is fixed; values arrive from /wm. */
"<div class='card'><h3>Groundstation</h3><table id='gsstatus'>"
"<tr><td>Name</td><td></td></tr>"
"<tr><td>Version</td><td></td></tr>"
"<tr><td>MQTT</td><td></td></tr>"
"<tr><td>Parent RSSI</td><td></td></tr>"
"<tr><td>Radio</td><td></td></tr>"
"<tr><td>Last RSSI</td><td></td></tr>"
"</table></div>"

"<div class='card'><h3>Modem</h3><table id='modemconfig'>"
"<tr><td>Modulation</td><td></td></tr>"
"<tr><td>Frequency</td><td></td></tr>"
"<tr><td>Freq. Offset</td><td></td></tr>"
"<tr><td>Spreading Factor</td><td></td></tr>"
"<tr><td>Coding Rate</td><td></td></tr>"
"<tr><td>Bandwidth</td><td></td></tr>"
"</table></div>"

"<div class='card'><h3>Satellite</h3><table id='satdata'>"
"<tr><td>Listening to</td><td></td></tr>"
"<tr><td>Lat / Lon</td><td></td></tr>"
"<tr><td>Az / El</td><td></td></tr>"
"<tr><td>Doppler</td><td></td></tr>"
"<tr><td>UTC Time</td><td></td></tr>"
"<tr><td>Local Time</td><td></td></tr>"
"</table></div>"

"<div class='card'><h3>Last Packet</h3><table id='lastpacket'>"
"<tr><td>Received</td><td></td></tr>"
"<tr><td>RSSI</td><td></td></tr>"
"<tr><td>SNR</td><td></td></tr>"
"<tr><td>Freq err</td><td></td></tr>"
"<tr><td colspan='2' style='text-align:center;'></td></tr>"
"</table></div>"

/* Console — same protocol as ESP32 /cs, our backend already supports it. */
"<div class='console'>"
"<textarea readonly id='t1' wrap='off'></textarea>"
"<form method='get' onsubmit='return f(1);'>"
"<input id='c1' placeholder='(commands disabled — read-only console)' disabled>"
"</form></div>"

"<script>"
/* /cs short-poll. Adapted from ESP32 IOTWEBCONF_CONSOLE_SCRIPT. */
"var x=null,lt,sn=0,id=0;"
"function f(p){var c,o='',t;clearTimeout(lt);"
"t=document.getElementById('t1');"
"if(t.scrollTop>=sn){"
"if(x!=null){x.abort();}"
"x=new XMLHttpRequest();"
"x.onreadystatechange=function(){"
"if(x.readyState==4&&x.status==200){"
"var a=x.responseText;"
"id=a.substr(0,a.indexOf('\\n')).replace(/^seq:\\s*/,'');"
"var z=a.substr(a.indexOf('\\n')+1);"
"if(z.length>0){t.value+=z;}"
"t.scrollTop=99999;sn=t.scrollTop;}};"
"x.open('GET','cs?c2='+id,true);x.send();}"
"lt=setTimeout(f,2345);return false;}"

/* /wm dashboard poll. Adapted; bug fix: wmx.status not x.status. */
"var wmx=null,wmt;"
"function wmf(){var sp,mc,gs,sd,lp;clearTimeout(wmt);"
"wmx=new XMLHttpRequest();"
"wmx.onreadystatechange=function(){"
"if(wmx.readyState==4&&wmx.status==200){"
"var wmp=wmx.responseText.split(',');"
"sp=document.getElementById('wmsatpos');"
"sp.setAttribute('cx',wmp[0]);sp.setAttribute('cy',wmp[1]);"
"mc=document.getElementById('modemconfig');"
"for(let r=0;r<6;r++)mc.rows[r].cells[1].innerHTML=wmp[r+2];"
"if(wmp[2]=='LoRa'){mc.rows[3].cells[0].innerHTML='Spreading Factor';"
"mc.rows[4].cells[0].innerHTML='Coding Rate';}"
"else{mc.rows[3].cells[0].innerHTML='Bitrate';"
"mc.rows[4].cells[0].innerHTML='Frequency dev';}"
"gs=document.getElementById('gsstatus');"
"for(let r=0;r<6;r++)gs.rows[r].cells[1].innerHTML=wmp[r+8];"
"sd=document.getElementById('satdata');"
"for(let r=0;r<6;r++)sd.rows[r].cells[1].innerHTML=wmp[r+14];"
"lp=document.getElementById('lastpacket');"
"for(let r=0;r<4;r++)lp.rows[r].cells[1].innerHTML=wmp[r+20];"
"lp.rows[4].cells[0].innerHTML=wmp[24];}};"
"wmx.open('GET','wm',true);wmx.send();"
"wmt=setTimeout(wmf,5000);return false;}"

"window.addEventListener('load',function(){f();wmf();});"
"</script></body></html>";
