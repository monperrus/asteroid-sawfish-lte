# asteroid-sawfish-lte

Working LTE — data, SMS and a registered modem — on the Huawei Watch 2 4G
(`sawshark`, sold as `sawfish`) running
[AsteroidOS](https://asteroidos.org/), where the modem does not boot at all
out of the box and the one documented attempt at booting it put the watch
into a reboot loop.

This repository contains the step-by-step procedure, the three systemd units
that package it, and the tools they need. Upstream tracks cellular on this
device as "bad" in
[meta-smartwatch#340](https://github.com/AsteroidOS/meta-smartwatch/issues/340).

What works today, verified on a live Tele2 SE SIM:

- The MSM8909 modem boots, keeps its EFS, reports its IMEI and registers on
  LTE (`+COPS: 0,0,"Tele2 Tele2",7`).
- A routed `rmnet_data0` carries ICMP, carrier DNS, HTTP and TLS.
- SMS sends and receives over the AT control plane, with no data bearer.

What does not:

- **Nothing is enabled at boot.** The battery cost has not been measured, and
  this device has a history of a firmware-triggered reboot loop. See
  [The battery soak](#the-battery-soak).
- ofono and ConnMan do not know LTE exists, so the AsteroidOS network
  settings UI does not either. The bearer is owned by `lte-bearer.service`.
- IPv6 is untested. Voice calls need the audio path, see
  [asteroid-sawfish-audio](https://github.com/monperrus/asteroid-sawfish-audio).

Everything below assumes `adb` access to the watch as root, which is the
default for `adb shell`.

## Licensing

MIT (see `LICENSE`), except where a file says otherwise: the `rmtfs` and
`libqipcrtr4msmipc` fixes are patches against GPL/BSD upstreams and carry
those projects' licences.

---

## How this works, in one picture

```
lte-rmtfs.service     serves EFS to the modem       (must be up FIRST)
      |
lte-modem.service     arms firmware, holds the subsystem powered
      |
lte-bearer.service    DPM + WDA + rmnet + the WDS call, applies IP config
```

`BindsTo=`/`After=` chain them, so `systemctl start lte-bearer` starts all
three in order and `systemctl stop lte-rmtfs` tears all three down.

The modem has no local flash. It keeps its NV items, RF calibration and IMEI
on the `modemst1`/`modemst2` partitions and asks the AP for them over the IPC
router. AsteroidOS runs neither the vendor `rmt_storage` nor mainline
`rmtfs`, so the modem stalls during initialisation and its watchdog fires —
the signature in
[meta-smartwatch#185](https://github.com/AsteroidOS/meta-smartwatch/issues/185).
Because `restart_level` defaults to `SYSTEM`, that modem crash escalates to a
full SoC reset. That is the whole reboot loop: **run an EFS server first, and
demote `restart_level` before booting the modem.**

The kernel here is a downstream Android 3.18 with `AF_MSM_IPC` and no `qrtr`,
so mainline QMI userspace only runs under an `LD_PRELOAD` shim that emulates
`AF_QIPCRTR` on top of it.

---

## Step 0 — Safety, once per watch

The EFS partitions hold the IMEI and the RF calibration. Corrupting them
bricks the modem permanently and there is no way back. Do not skip this.

### 0.1 Back up every modem-related partition

```sh
for p in modemst1 modemst2 fsg fsc nvbin nvdata modem; do
  adb exec-out "dd if=/dev/disk/by-partlabel/$p bs=1M" > backup/$p.img
done
sha256sum backup/*.img | tee backup/SHA256SUMS
```

Nothing here ever writes to those partitions: `rmtfs` runs read-only (`-r`),
and firmware is copied *out* of a read-only mount of `modem`.

### 0.2 Know the recovery path

Hold power during the Huawei boot logo until the vibration ends, release,
press again quickly, and you are in fastboot. Keep a known-good
`asteroid-sawfish-boot.img` and rootfs to hand before arming anything.

### 0.3 Keep the disarm safety net

`install-lte.sh` installs and **enables** `lte-firmware-disarm.service`,
which empties `/lib/firmware/{mba.mbn,modem.*}` early on every boot. A reboot
therefore always returns the watch to the state where the modem physically
cannot boot, whatever else went wrong. `lte-modem.service` re-arms after it
has run.

Remove it only once the battery soak is done and you actually want LTE at
boot.

---

## Step 1 — Install

From a host with `adb` access:

```sh
./install-lte.sh [adb-serial]
```

That pushes everything in `bin/`, the helper scripts, the five units, the udev
rules, the default config and this README (to `/usr/share/doc/lte/`), reloads
udev and systemd, and enables the disarm service. It is idempotent — re-run it
after editing anything.

The udev rules matter: they create `/dev/modem` (→ `smdcntl0`),
`/dev/modem-at` (→ `smd8`) and `/dev/qcom_rmtfs_uio1` (→ the `uio` device
named `rmtfs`). `lte-rmtfs.service` has `ConditionPathExists=` on the last
one and will silently not start without it.

Set your carrier's APN in `/etc/default/lte-bearer`:

| Setting | Default | Meaning |
|---|---|---|
| `APN` | `internet.tele2.se` | data APN — *not* the `4g.tele2.se` attach APN |
| `IP_TYPE` | `4` | IPv6 is untested on this modem |
| `MUX_ID` | `1` | QMAP mux ID, must match the rmnet endpoint |
| `PREFER` | `wifi` | which path wins when both are up |
| `MANAGE_RESOLV` | `yes` | take over `/etc/resolv.conf` while up |

**The units are installed but not enabled at boot, on purpose** — see
[The battery soak](#the-battery-soak).

---

## Step 2 — Bring LTE up

As root on the watch:

```sh
systemctl start lte-bearer.service      # pulls in lte-modem and lte-rmtfs
systemctl status lte-bearer.service
```

Expect this to take a while: the modem has to boot, load EFS, register its
QMI services, attach to the network, and only then can the bearer start. A
cold start to a routed interface is a minute or two.

Verify:

```sh
ip addr show rmnet_data0
ping -c 4 -I rmnet_data0 1.1.1.1
```

Live proof from the packaging session (Tele2 SE):

```text
address=10.145.82.17/30 gateway=10.145.82.18
dns=130.244.127.161,130.244.127.169 mtu=1500
ping -I rmnet_data0 1.1.1.1: 4 transmitted, 4 received, 0% loss
```

`PREFER=wifi` holds: with the bearer up, `ip route get 1.1.1.1` still
resolves via `wlan0`. Stopping leaves no address, no route, no `qmicli`
process and the original `/etc/resolv.conf` restored. A stop takes roughly
40 s, most of it the network stopping gracefully.

Healthy modem signature:

```sh
cat /sys/bus/msm_subsys/devices/subsys2/state        # ONLINE
cat /sys/bus/msm_subsys/devices/subsys2/crash_count  # 1, then flat
```

One contained `RELATED` restart during EFS bring-up is normal. A climbing
`crash_count` is not.

---

## Step 3 — SMS

SMS needs the modem but **not** the bearer, so `systemctl start
lte-modem.service` is enough. It runs over AT on `/dev/modem-at`
(= `/dev/smd8`), and everything touching that node — including the
`cat`/`printf` inside a shell script — needs the smdpkt shim preloaded:

```sh
export LD_PRELOAD=/usr/local/lib/lte/libsmdpkt_wrapper.so
at_cmd.sh 8 'AT+CREG?' 'AT+CSQ'      # +CREG: 0,1 means registered
lte-send-sms '+46701234567' 'hello'
```

`send_sms.sh` automates the two-part `AT+CMGS` exchange, which is easy to get
wrong by hand: text mode (`AT+CMGF=1`, the default is PDU), the command line
terminated with a **bare `\r`**, wait for the `> ` prompt, then the body
terminated by **Ctrl-Z (`\032`)** — `\033` aborts. Success returns
`+CMGS: <mr>`.

### Receiving: arm `+CNMI` or messages are silently dropped

The modem's default is `+CNMI: 0,0,0,0,0`. With `<mt>=0` it has nowhere to
route an arriving message and does not tell the terminal about it. **An SMS
can arrive and simply be discarded, and an empty `AT+CMGL="ALL"` will
honestly report nothing.** An empty inbox before arming `+CNMI` proves
nothing.

```sh
AT+CMGF=1
AT+CPMS="ME","ME","ME"     # ME = modem, 23 slots; SM = SIM, 25 slots
AT+CNMI=2,1,0,0,0          # buffer+deliver, store + notify
AT+CSDH=1                  # detailed headers
AT+CMGL="ALL"              # lists the *currently selected* store only
```

Two traps: `+CPMS?` reports per-store counts and `+CMGL` only lists the
selected one, so `+CPMS: "ME",0,23,"ME",0,23,"SM",1,25` means there is a
message on the SIM you will not see until you switch stores. And arming
`+CNMI` can emit immediate `+CMTI` lines for already-queued messages,
including a bogus-looking `+CMTI: ,-1`; a `+CMGL` fired straight after can
race the store commit and come back empty. Wait and re-list before deciding
the indications were spurious — they usually are not.

Only one reader can usefully hold `/dev/modem-at`. Do not run a long-lived
`cat` listener and `at_cmd.sh` at the same time; poll instead.

MT latency is seconds, not minutes. If a reply seems slow, suspect `+CNMI`
before suspecting the network.

---

## Why each step is load-bearing

Every one of these was a separate failure during bring-up. None is optional.

### rmtfs must be up before the modem, and must never be restarted under it

`Restart=no` on `lte-rmtfs.service` is deliberate. Restarting `rmtfs` under a
live modem forces a contained modem crash, and the replacement instance
misses the EFS requests the modem makes *while restarting* — you get a modem
stuck in a restart cycle. Recovery is to stop everything and replay the
sequence, which the `BindsTo=` chain does for you.

Upstream `rmtfs` also needed a fix to work here at all: this MSM8909 firmware
sends `phys_offset` in RMTFS iovec requests **relative to the allocated
buffer**, while upstream interprets it as an absolute physical address and
rejects every transfer. Submitted as
[linux-msm/rmtfs#41](https://github.com/linux-msm/rmtfs/pull/41).

### `restart_level` before firmware, always

`lte-modem-arm` writes `RELATED` to the subsystem's `restart_level` and
**refuses to continue if the write did not take**. The default is `SYSTEM`,
under which an ordinary modem crash resets the entire SoC. Containment comes
before firmware.

### The subsystem holder must be a service, not a scope

Opening `/dev/subsys_modem` *is* the power-on (`subsystem_get`), so something
must hold the fd. `systemd-run --scope ... 'exec 3< /dev/subsys_modem' &` is
reaped when the invoking shell exits; the fd closes, the modem powers down,
and the *next* symptom you see is `/dev/modem-at` failing to open with
`Connection timed out` — which reads like an unrelated SMD fault and is
thoroughly misleading.

### `ONLINE` is not readiness — gate on QMI, and set operating mode

`lte-modem-hold` is `Type=notify` and signals ready only after three things,
in order, because each is ready well before the next:

1. `subsys2/state` reads `ONLINE`. This flips almost immediately, long before
   the modem has registered its QMI services; starting the bearer there fails
   with `node with id 0 not found in QRTR bus`.
2. `qmicli --dms-get-ids` answers over QRTR. That is what the bearer actually
   depends on, so it is what is worth waiting for.
3. `--dms-set-operating-mode=online`. A freshly booted modem sits in
   `shutting-down` and will never attach. Nothing surfaces this usefully: the
   only symptom is the bearer failing 120 s later with `generic-no-service`,
   which reads like a config fault but is not.

### DPM opens the control port — that is what creates `rmnet0`

The vendor `/system/etc/data/netmgr_config.xml` identifies this SoC as the
`feero` profile: control port `DATA5_CNTL`, hardware endpoint `bam-dmux`
interface 0, RX/TX pipe pair **0/0**. Read the pipes rather than guessing:

```sh
msm-rmnet-ctl get-endpoints rmnet0
# interface=rmnet0 endpoint=0 consumer=0 producer=0
```

Stock `qmicli` treats endpoint number 0 as both a legitimate value and an
"unset" sentinel, so it cannot express this device's configuration; the fix
is a separate presence flag, on the `qmicli-dpm-zero-pipe-ids` branch.

### `rmnet0` must be switched to raw-IP

BAM-DMUX delivers raw QMAP frames, but the interface comes up claiming
`link/ether`. While it does, the network core strips 14 bytes off every
downlink frame as an Ethernet header before rmnet's ingress handler sees it,
and every reply vanishes. Ftrace showed the packet socket seeing 92 bytes and
`rmnet_ingress_handler` seeing 78. `msm-rmnet-ctl set-raw-ip rmnet0` issues
`RMNET_IOCTL_SET_LLP_IP` (`0x89f2`); `get-llp` should then read
`0x2 (raw-ip)`.

This also stops ConnMan from mistaking `rmnet0` for a wired Ethernet service.

### WDA must agree with DPM

`ep-type=bam-dmux,ep-iface-number=0`, raw-IP, QMAP up and down, DL max size
2048, DL max datagrams 1. An earlier `ep-type=embedded` here was the single
reason no downlink arrived.

> `qmicli --help-wda` lists `ep-type (undefined|hsusb|pcie|embedded)` and
> omits `bam-dmux`. The help string is stale; the parser accepts `bam-dmux`.
> Do not conclude from the help text that this is impossible.

### Wait for registration before starting the call

The modem answers QMI a good while before it attaches. Starting too early
returns QMI error 14 `CallFailed` with call end reason 3
`generic-no-service`. The bearer polls `--nas-get-serving-system` for
`Registration state: 'registered'`.

### rmnet mux/demux, flags 10 both ways

Flags `10` = MAP + muxing with no aggregation. Do **not** enable checksum-v4
because a capture shows bytes past the declared QMAP payload — this kernel
returns `ERR_BAD_BUFFER` for checksum processing, and with flags 10 the
driver trims correctly anyway.

`rmnetcli` here is the stock ROM's `/system/bin/rmnetcli`, an Android binary
that runs on this image because the bionic linker is present.

> **It must be run with `env -u LD_PRELOAD -u LD_LIBRARY_PATH`.** The bearer
> exports a glibc `LD_PRELOAD` for `qmicli`; inheriting that into a bionic
> binary aborts it instantly — `SIGABRT`, "core dumped", no diagnostic. The
> only visible symptom is `rmnet_data0` never being created.

### The mux bind and the WDS call must be in one process

This is the constraint that shapes the whole bearer script:

- A second process asking `--wds-get-current-settings` gets QMI error 15,
  `OutOfCall` — the call belongs to another client.
- Reusing the CID via `--client-cid=N` after the first process exited gets
  `Unknown client N for service wds`. QRTR drops the client with its socket,
  so `--client-no-release-cid` does not survive process exit for WDS.
- Stock `qmicli` refuses `--wds-bind-mux-data-port` plus
  `--wds-get-current-settings` with "too many WDS actions requested".

So `lte-bearer` starts **one** `qmicli-lte` with bind + `--wds-start-network`
+ `--wds-follow-network`, keeps it as the call's owner for the unit's
lifetime, and reads the settings out of that process's own output. When it
exits, the bearer is genuinely gone and systemd restarts the unit.

### Keep IP off `rmnet0`

`rmnet0` is a raw QMAP transport and must never carry IP. ConnMan and avahi
see the new link appear and give it a `169.254/16` address plus a
`default dev rmnet0 scope link` route — **at metric 0**, which beats every
real default route and black-holes all traffic onto an interface that cannot
route it. The bearer strips both at start-up and re-strips them every 30 s,
because the daemons put them back.

### Address, routes and DNS are applied by hand

There is no DHCP on a raw-IP QMI bearer. The call reports address, netmask,
gateway, two DNS servers and MTU; the bearer converts the netmask to a
prefix, puts the address on `rmnet_data0`, adds a default route via the
gateway, pins both DNS servers to that gateway, and writes
`/run/lte-resolv.conf` over `/etc/resolv.conf`. ConnMan and
`systemd-resolved` both want that file on this image and conflict; the bearer
sidesteps that rather than joining it.

Two details that are easy to get wrong:

- **The route metric cannot be a sensible-looking constant.** ConnMan gives
  `wlan0` a near-maximum metric (4294961151 on this image), so a "high"
  number like 500 silently makes LTE the *primary* path. The bearer reads
  `wlan0`'s actual metric and derives its own from it, so `PREFER=wifi` means
  what it says.
- **`/etc/resolv.conf` is a symlink on this image.** Copying it to save the
  original saves a *pointer*, and once the bearer has run once that pointer
  is to the bearer's own file — the original is then gone. The bearer records
  the link *target* and restores the symlink on stop.

### Stopping cleanly

`qmicli` stops the call on `SIGINT` ("Ctrl+C will stop the network") and
ignores `SIGTERM`. So the bearer sends `SIGINT` to its child and waits up to
15 s before `SIGKILL` — without this a `systemctl stop` hangs in
`deactivating` — and the unit uses `KillMode=mixed`, so systemd signals only
the script. Under the default `control-group` the teardown itself is killed
halfway through, leaving routes and `/etc/resolv.conf` behind.

---

## Troubleshooting

| Symptom | Cause |
|---|---|
| `/dev/modem-at`: `Connection timed out` | the modem is not powered — the subsys holder died. `systemctl status lte-modem` |
| `lte-rmtfs.service` never starts | `/dev/qcom_rmtfs_uio1` missing; the udev rules are not applied |
| TX counters rise, RX stays 0 | `rmnet0` is still Ethernet-mode, or WDA disagrees with DPM |
| `OutOfCall` from `--wds-get-current-settings` | you are asking from a process that does not own the call; read `journalctl -u lte-bearer` instead |
| `Unknown client N for service wds` | a CID from an exited process; QRTR already dropped it |
| `crash_count` climbing | check `restart_level` is `RELATED`, and that `rmtfs` was up *before* the modem |
| modem restarts after touching rmtfs | never restart `rmtfs` under a live modem; stop the whole chain and replay |
| `node with id 0 not found in QRTR bus` | started before QMI registered; the readiness gate in `lte-modem-hold` exists for this |
| `CallFailed` / `generic-no-service` | not registered yet, or the modem is not in `online` operating mode |
| `rmnetcli` "Aborted (core dumped)" | a glibc `LD_PRELOAD` leaked into a bionic binary |
| all traffic dies when the bearer comes up | the metric-0 `default dev rmnet0` route is back |
| `systemctl stop` hangs in `deactivating` | `qmicli` needs `SIGINT`, and `KillMode` must be `mixed` |
| SMS never arrives | `+CNMI` is not armed, or you are listing the wrong `+CPMS` store |
| nothing survives a reboot | by design — `lte-firmware-disarm.service` |

---

## Three things that must never be done

- **Never leave `restart_level` at `SYSTEM` with firmware armed.** A routine
  modem crash then resets the whole watch, and since the firmware is still on
  disk it does it again. That is the reboot loop.
- **Never restart `rmtfs` while the modem is up.** Stop the whole chain and
  replay it.
- **Never hold `/dev/subsys_modem` from a shell, a scope, or `nohup`.** All
  three get reaped and power the modem down at a moment you will not connect
  to the symptom.

---

## Files

| File | What it does |
| --- | --- |
| `install-lte.sh` | Step 1. Pushes everything to the watch over `adb`. Idempotent. |
| `lte-rmtfs.service` | Serves EFS to the modem. Must be up before the modem boots. |
| `lte-modem.service` | Arms firmware and holds the modem powered; readiness = QMI answers. |
| `lte-bearer.service` | The data bearer: DPM, WDA, rmnet, the WDS call, IP config. |
| `lte-firmware-disarm.service` | The safety net. Empties `/lib/firmware` at every boot. |
| `lte-modem-arm` / `lte-modem-disarm` | `restart_level`, then copy firmware out of a read-only mount of the stock `modem` partition. |
| `lte-modem-hold` | Holds `/dev/subsys_modem` open and gates readiness on QMI. |
| `lte-bearer` | The bearer itself, configured by `/etc/default/lte-bearer`. |
| `lte-bearer.default` | Shipped `/etc/default/lte-bearer`. APN goes here. |
| `99-lte-modem.rules` | `/dev/modem`, `/dev/modem-at`, `/dev/qcom_rmtfs_uio1`. |
| `at_cmd.sh` | Sends AT commands to `/dev/modem-at` and prints what comes back. |
| `send_sms.sh` | One SMS in text mode, including the `>`-prompt/Ctrl-Z dance. |
| `lte-soak` / `lte-soak@.service` / `lte-soak-report` | The battery A/B measurement. |
| `bin/rmtfs` | Mainline `rmtfs`, patched. Serves EFS over the IPC router. |
| `bin/libqipcrtr4msmipc.so` | `AF_QIPCRTR`→`AF_MSM_IPC` shim, patched. This is what lets mainline QMI userspace run on a downstream 3.18 kernel. |
| `bin/libsmdpkt_wrapper.so` | Unmodified upstream shim. Anything touching `/dev/modem-at` needs it preloaded. |
| `bin/qmicli-lte` + `bin/libqmi-glib.so.5`, `bin/libqrtr-glib.so.0` | Patched `qmicli`. Stock `qmicli` cannot do what the bearer needs. |
| `bin/msm-rmnet-ctl` | Reads the BAM pipe pair and switches `rmnet0` to raw-IP. Static. |
| `src/` | The patched sources and the upstream submissions, see below. |

Everything under `bin/` is cross-built for `armv7vehf-neon`
(`-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard`) and stripped. The build
tree they come from is large and regenerable, so it is not checked in — this
repository's `.gitignore` is deny-by-default for exactly that reason.

### The patches behind the binaries

| Patch | Why | Source |
|---|---|---|
| `rmtfs`: accept a shared-memory *offset*, not only an absolute address ([PR #41](https://github.com/linux-msm/rmtfs/pull/41)) | this firmware sends `phys_offset=0x200` for sector 1; upstream read it as an absolute address and rejected every EFS transfer | `src/0003-*.patch`, `src/sharedmem.c` |
| `libqipcrtr4msmipc`: answer `QRTR_TYPE_NEW_LOOKUP`, including wildcard enumeration and the all-zero end-of-dump marker | the original shim supports a service *registering* itself (what `rmtfs` needs) but not a client *looking one up* (what `qrtr-glib` needs) | `src/0004-*.patch`, `src/libqipcrtr4msmipc.c` |
| `libqipcrtr4msmipc`: `ppoll()` readiness plus a one-datagram peek cache | target GLib waits with `ppoll()` and probes with `MSG_PEEK`; AF_MSM_IPC's `MSG_PEEK` is destructive, so `qmicli` lost every immediate response | `src/0004-*.patch`, `src/libqipcrtr4msmipc.c` |
| `libqmi`: separate presence flag for DPM endpoint number 0 | this kernel reports `consumer=0, producer=0`, which stock `qmicli` cannot distinguish from "unset" | `src/0001-*.patch` |
| `libqmi`: allow `--wds-bind-mux-data-port` and `--wds-start-network` in one invocation | the bind and the call must share one WDS client; this is a local diagnostic patch, not upstream material | `src/0002-*.patch` |

The numbered `src/*.patch` files are the versions proposed upstream;
`src/sharedmem.c` and `src/libqipcrtr4msmipc.c` are the working sources the
shipped binaries were actually built from, which say the same thing less
tidily. `src/msm-rmnet-ctl.c` and `src/Makefile.msm-rmnet-ctl` build
`bin/msm-rmnet-ctl`:

```sh
make -f Makefile.msm-rmnet-ctl CC=arm-linux-gnueabihf-gcc LDFLAGS=-static
```

`src/set-rmnet-llp.c` is the one-ioctl precursor to that tool, kept because
it is the smallest possible reproduction of the raw-IP fix.

Once QMI works over QRTR, this modem registers its services natively on the
IPC router (all on node 0): `WDS`→port 42, `DMS`→port 50, `NAS`→port 36,
`WMS`→port 38, `UIM`→port 41. That is the durable finding — the mainline
QMI-over-QRTR approach is architecturally viable on a downstream MSM kernel,
independently of the `smdcntl0`/`qmuxd` dead end. `/dev/smdcntl0` is *not* a
usable path here: on Android that channel is only opened once the vendor's
`qmuxd` negotiates it, and no vendor daemon is running.

---

## The battery soak

This is the gate on enabling any of it at boot. Upstream disabled modem boot
on this device over exactly this
([asteroid#101](https://github.com/AsteroidOS/asteroid/issues/101#issuecomment-840657432)),
so it must be measured, not assumed — and note that mainline `rmtfs` is not
the vendor `rmt_storage` that caused the historic drain, so it may simply not
reproduce.

Two 2-hour idle runs, modem down then modem up, compared on drain per hour
and on `/sys/kernel/debug/wakeup_sources`:

```sh
# on the watch, as root, OFF the charger:
systemctl start lte-soak@modem-down.service     # run 1, modem stopped
systemctl start lte-soak@modem-up.service       # run 2, bearer running

# on the host, afterwards:
adb pull /home/ceres/soak ./soak
./lte-soak-report soak/soak-modem-down-*.csv soak/soak-modem-up-*.csv
```

`lte-soak` **refuses to start while the watch is on the charger** and aborts
if the charger is connected mid-run — a soak on charge measures nothing but
produces a plausible-looking file, which is worse than no data. `SOAK_FORCE=1`
overrides for dry runs only.

**Both runs must be identical apart from the modem.** Same Wi-Fi state, same
screen behaviour, nobody touching the watch, and no SSH polling during the
run — the sampler is deliberately local because an SSH loop would keep the
device awake and measure itself.

A wake-lock named `ipc000000e9_qmicli-lte` is already visible in
`wakeup_sources` while the bearer runs. That is the thing to watch.

### Result so far: worn baseline, modem down (2026-08-31)

The control half of the A/B, and on its own already the more important
number:

```text
96% -> 5% in 6.30 h        =  14.45 %/h
projected runtime          =  ~6.9 h from full
current_avg                =  68 mA mean, 64 mA median
voltage                    =  4.25 V -> 3.25 V
```

**The watch lasts under 7 hours worn with the modem off.** Whatever the modem
costs is a delta on top of a platform that already drains this fast, so "does
LTE hurt battery life" is a smaller question than it looked.

Two methodology findings from that run:

- **`SOAK_INTERVAL` is a lower bound, not a cadence.** At 120 s the median gap
  was ~16 min and only 16% of the expected samples appeared: the device
  suspends and the timer does not fire until something else wakes it. That is
  the right behaviour — sampling on a forced schedule would keep the device
  awake and measure the sampler — and rates come from the recorded
  timestamps, so sparse data is still sound. Do not "fix" it with a wake
  alarm.
- **Runs end abruptly.** This one stopped at 5% battery and never wrote its
  closing `wakeup_sources` snapshot. The sampler now writes it from a signal
  handler and refreshes a `.wakeup.last` every ~10 samples; the report falls
  back to that.

The modem-up half has not been run. Until it has, the units stay disabled at
boot and `lte-firmware-disarm.service` stays enabled.

---

## Still open

- **The modem-up battery soak.** The reason nothing is enabled at boot.
- **ofono/ConnMan ownership.** ConnMan does not know LTE exists, so neither
  does the AsteroidOS network settings UI. ofono's `gobi` driver auto-detects
  USB/`usbmisc` QMI devices only and will not find this modem's `smdpkt`
  device without patching `udevng.c`; point it at a QRTR node instead
  (`qmi-endpoint-qrtr.c` already exists upstream) before going near that.
- **IPv6.** Untested.
- **Voice calls.** They need the speaker and microphone path, which is
  separate work; the modem bring-up here is the shared prerequisite.
- **A proper Yocto layer.** This installs over `adb` into `/usr/local`; it is
  not a meta-smartwatch recipe yet.
