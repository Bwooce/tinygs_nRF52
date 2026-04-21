# Home Assistant OpenThread Border Router — Setup for This Station

This firmware talks IPv6 over Thread. To reach `mqtt.tinygs.com` (IPv4), it
depends on a specific kind of Thread Border Router: one that runs a **DNS64
resolver, advertises a NAT64 prefix, and exposes `ot-ctl` for commissioning**.

The only off-the-shelf BR that provides all three is the Home Assistant
OpenThread Border Router add-on (running on HA Yellow, HA Green, or on a
Raspberry Pi with a Silicon Labs SkyConnect / Nabu Casa SkyConnect dongle).

This document covers why other BRs fall short, how to set up the HA OTBR, and
the two persistence tweaks that bite after a container restart.

## Why Apple / Google BRs are not enough

Apple (Apple TV 4K, HomePod mini, HomePod 2nd gen) and Google (Nest Hub 2nd
gen, Nest Wifi Pro) devices implement Thread Border Routing, but only the
slice needed to tunnel Matter and HomeKit traffic between Wi-Fi and Thread:

| Capability                                 | HA OTBR | Apple BR | Google BR |
|--------------------------------------------|:-------:|:--------:|:---------:|
| Forwards Thread ↔ Wi-Fi (basic routing)    | ✓       | ✓        | ✓         |
| Advertises NAT64 prefix to the mesh        | ✓       | ✗        | ✗         |
| Runs a DNS64 resolver (`AAAA` synthesis)   | ✓       | ✗        | ✗         |
| Exposes `ot-ctl` for commissioning         | ✓       | ✗        | ✗         |
| Configurable `ip6tables` firewall          | ✓       | N/A      | N/A       |

This station sends *outbound* TLS/MQTT to an **IPv4-only** host
(`mqtt.tinygs.com`). Without a NAT64 prefix in the mesh netdata, the station
has no way to synthesise an IPv6 address that the BR will translate back to
IPv4. Apple and Google BRs advertise Thread routes but intentionally leave
NAT64/DNS64 out, because Matter/HomeKit devices don't need them.

So: **Apple and Google BRs can coexist with this station on the same Thread
mesh, but they cannot be the one that carries its MQTT traffic.** If the mesh
picks one of them as the station's egress, DNS for `mqtt.tinygs.com` returns
nothing usable and the connection silently fails.

### Mixed-BR networks — force the route preference

On a typical household network it's normal to have an Apple or Google BR
*already* running (they auto-enable when you pair a HomeKit/Matter device)
**and** also add the HA OTBR for this station. All BRs co-advertise Thread
routes into the mesh netdata with a preference.

The default preference is `med`. If multiple BRs all run at `med`, the mesh
picks deterministically but not necessarily in your favour — and our packets
might be routed through the Apple or Google BR that has no NAT64.

The fix is to promote the HA OTBR to `high` preference:

```sh
docker exec addon_core_openthread_border_router ot-ctl br routeprf high
```

Now every device on the mesh (including ours) prefers the HA OTBR for
external routing. Apple/Google BRs stay on the mesh to serve their own
Matter devices, but they stop being the egress path for this station.

Verify on the device side after the change:

```
ot-ctl br prefix   # should list the /96 NAT64 prefix advertised by HA OTBR
ot-ctl netdata show
```

## 1. Prerequisites

- Home Assistant Yellow, HA Green, or Raspberry Pi + HA OS / Supervised
- A Thread-capable 802.15.4 radio:
  - Silicon Labs SkyConnect / Nabu Casa SkyConnect (USB dongle)
  - HA Yellow's built-in Silicon Labs MGM210P module
- HA Supervisor with add-on support (HA OS or Supervised; Docker-only HA Core
  can't run add-ons)

## 2. Install the Open Thread Border Router add-on

Settings → Add-ons → Add-on Store → **OpenThread Border Router** → Install →
Start.

If the radio is also serving Zigbee via ZHA, migrate Zigbee off that radio
first (or use Silicon Labs Multi-Protocol — not recommended unless you
already have it working). Running OTBR on a radio that's also doing Zigbee
via the same dongle is supported but adds a class of issues that aren't
worth debugging on day one.

## 3. Configure the add-on

Open the OTBR add-on → Configuration. For this station:

| Option | Value | Why |
|--------|-------|-----|
| `firewall` | `false` | Keeping the HA-OTBR `ip6tables` firewall off during setup means LAN → Thread-ULA ingress works, which you need for the station's web UI / debug. Turning it on later is covered in PLAN.md §21. |
| `autoflash_firmware` | `true` | Fine to leave on |
| `device` | auto-detected | Don't override unless the OTBR picks the wrong radio |

Restart the add-on after saving.

## 4. Apply the post-start tweaks

Two things that **don't survive a container restart** and have to be applied
each time the add-on / HA OS reboots:

1. **`br routeprf high`** — make HA OTBR the preferred egress (§ above)
2. **`ip6tables` MASQUERADE** — NAT the station's Thread ULA source to the
   host's public IPv6 when packets leave the `end0` interface. Without this,
   upstream routers drop Thread ULA (`fd..::/8`) packets that escape the mesh
   — DNS works (BR does it for us), but TLS handshakes to NAT64-synthesised
   addresses hang.

### Quick one-shot (after OTBR restart, manual)

```sh
docker exec addon_core_openthread_border_router sh -c "\
  ot-ctl br routeprf high; \
  ip6tables -t nat -C POSTROUTING -s fc00::/7 -o end0 -j MASQUERADE 2>/dev/null || \
  ip6tables -t nat -A POSTROUTING -s fc00::/7 -o end0 -j MASQUERADE"
```

The `-C … 2>/dev/null || -A` idiom means "add the rule if it isn't already
there" — safe to re-run. `fc00::/7` covers every ULA prefix, which matters
because the OMR prefix the BR picks can change across restarts (e.g.
`fd9f:…/64` → `fdd7:…/64`).

### Persistent version via HA automation

Add to `configuration.yaml`:

```yaml
shell_command:
  otbr_nat64_setup: >
    docker exec addon_core_openthread_border_router sh -c "
    ot-ctl br routeprf high;
    ip6tables -t nat -C POSTROUTING -s fc00::/7 -o end0 -j MASQUERADE 2>/dev/null ||
    ip6tables -t nat -A POSTROUTING -s fc00::/7 -o end0 -j MASQUERADE"

automation:
  - alias: "OTBR NAT64 Setup"
    trigger:
      - platform: homeassistant
        event: start
    action:
      - delay: "00:00:30"   # let the add-on finish starting
      - service: shell_command.otbr_nat64_setup
```

The 30 s delay gives the OTBR container time to open its socket; without it
the `docker exec` races the add-on startup.

## 5. Verify NAT64 and DNS64 are advertised

```sh
docker exec addon_core_openthread_border_router ot-ctl netdata show
```

Look for lines like:

```
Prefixes:
fdd7:b8a4:2e0a:1::/64 paros med e000
Routes:
fc00::/7 s med e000
Services:
44970 01 01 s e000      <-- DNS server
fd d7 b8 a4 2e 0a 1 00 00 00 00 00 01 01 01 00  <-- NAT64 prefix
```

The NAT64 prefix is a `/96` route the mesh advertises; the station synthesises
outbound IPv6 by concatenating this `/96` with the last 32 bits of the
server's IPv4.

If you don't see the NAT64 prefix, the firewall option is blocking it or
IPv6 isn't routable from the host — check `ip -6 route` on the HA host first,
then the add-on's Kconfig.

## 6. Commission the station

With the OTBR running and confirmed healthy:

```sh
docker exec addon_core_openthread_border_router ot-ctl commissioner start
docker exec addon_core_openthread_border_router ot-ctl commissioner joiner add '*' TNYGS2026NRF
```

The `'*'` wildcard accepts any joiner EUI64 — fine for a home network;
narrow to the specific EUI64 for a public/shared mesh.

Then plug the station in (the firmware is preconfigured with PSKd
`TNYGS2026NRF` in `prj.conf`; change it if you changed it on the
commissioner side). Within ~30 s you should see on the station's USB
console:

```
Thread role: Detached
Thread role: Child
Thread re-attached as Child after 1s detached
Thread credentials saved
Resolving mqtt.tinygs.com via NAT64-synthesised DNS [...]:53
MQTT CONNECTED to mqtt.tinygs.com:8883
```

## 7. Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| `DNS resolution timed out` on device | `br routeprf` not set to `high`, mesh picked Apple/Google BR for egress — re-run the quick-fix command |
| `MQTT DISCONNECTED after 1s: result=-5 (EIO)` right after handshake | `ip6tables MASQUERADE` rule missing — upstream drops the ULA source |
| `No /96 NAT64 route in Thread netdata` | OTBR firewall option left `true`, or add-on didn't finish starting before the automation ran (extend the 30 s delay) |
| Joiner stuck at "Detached" forever | Commissioner not running, or PSKd mismatch — `ot-ctl commissioner state` should say `active` |
| Device joins but can't reach `mqtt.tinygs.com` | DNS64 not being served — usually a sign the egress is going through the wrong BR; confirm with `ot-ctl netdata show` that the HA OTBR's DNS server is listed |

## Related reading

- PLAN.md §21 — Web UI transport decisions and how they interact with the
  `firewall` setting (plain HTTP vs port-scoped `ip6tables` allow vs mTLS).
- `docs/TINYGS_MQTT_PROTOCOL.md` — what traffic this station actually sends
  once the BR plumbing works.
