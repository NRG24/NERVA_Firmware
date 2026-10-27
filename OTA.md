# Over-the-air firmware updates (v0.9.0+)

From v0.9.0 the ring runs MCUboot in front of the app and takes new
firmware over BLE. The SWD jig is needed exactly once per ring: to put the
bootloader on. Every update after that goes over the air.

**NOT YET RUN ON HARDWARE** as of 2026-10-27. Built and host-tested only.
The first-flash and first-update checklist is at the end.

## Flash layout

| Region | Address | Size | Contents |
|---|---|---|---|
| `boot_partition` | 0x00000 | 48 kB | MCUboot, overwrite-only, LZMA2 decompressor (uses ~36.6 kB) |
| `slot0_partition` | 0x0C000 | 272 kB | running app, 512-byte MCUboot header first (~239 kB) |
| `slot1_partition` = `history_partition` | 0x50000 | 176 kB | **on-ring history log**, and the landing slot for an update |
| `storage_partition` | 0x7C000 | 16 kB | NVS settings: bonds, profile, history seq floor |

Two full image slots don't fit. 2 × 245 kB + bootloader + bonds already
fills 512 kB and leaves nothing for history. So:

- **The update is compressed.** imgtool LZMA2-compresses the signed image
  to ~144 kB (79% of slot 1). The bootloader decompresses it over slot 0.
  `SB_CONFIG_MCUBOOT_COMPRESSED_IMAGE_SUPPORT` requires overwrite-only mode.
- **Slot 1 is the history partition.** An update erases the on-ring
  history, so the app must sync it first.
- **There is no revert.** MCUboot checks the signature and hash before it
  copies anything, so a corrupt, truncated or foreign upload is refused and
  the old firmware keeps running. A correctly signed image that is broken
  in itself stays installed, though, and recovering from that needs the
  jig. **Run every release on an unsealed board before it goes out over
  the air.**

The history partition is still at 0x50000 with the same size as v0.6–v0.8.
A v0.8 ring reflashed over SWD to v0.9 keeps its log, as long as the
reflash doesn't use `--chip` erase.

## How the history is protected (`src/ota.c`, `src/history.c`)

- **Upload starts.** The first chunk of an upload (offset 0), or an image
  erase command, calls `history_suspend()` before any byte is written. That
  saves the next record seq to settings (`hist/seq`) and stops every read
  and write of the partition. While suspended, appends return `-EBUSY` and
  a history sync returns no records, though its END packet still carries
  the newest seq.
- **Update installed.** On the first boot after the update the partition
  holds no log header, so `history_init()` formats it (~4 s) and starts
  numbering at the saved seq, not at 1. An app syncing "everything after
  seq N" keeps working with no special case.
- **Upload abandoned.** After 10 minutes with no image traffic,
  `history_resume()` formats the partition and the log carries on, same
  boot and same seq. If the phone later tries to continue that upload
  mid-image, the chunk is refused with SMP rc 6 (EBADSTATE), and the upload
  must restart from 0.
- **Uploaded and marked, but no reset came.** The ring reboots by itself
  after 60 s and installs the image.
- **Battery below 3.6 V.** The ring refuses the *start* of an upload with
  SMP rc 10 (EBUSY). An upload already under way is not stopped.

`history_suspend()` before `history_init()` (a phone faster than a
microsecond-scale window at boot) is handled safely, but the numbering may
then repeat. See the comment in `history.c`.

## Building

`.\build.ps1` (any variant) builds MCUboot and the app together through
sysbuild. It needs the signing key:

```
keys/ring-mcuboot-ecdsa-p256.pem     ECDSA P-256, made with imgtool keygen
```

`keys/*.pem` is gitignored. **Back the key up somewhere safe.** Without it,
no ring that has this bootloader can ever be updated over the air again,
only reflashed over SWD. Anyone who has the key can sign firmware the ring
will accept, so they would still need to bond with the ring to upload it.
To make a new key:

```bash
python C:\ncs\v3.4.0\bootloader\mcuboot\scripts\imgtool.py keygen -k keys\ring-mcuboot-ecdsa-p256.pem -t ecdsa-p256
```

Run it from the NCS toolchain shell. A new key only works on rings whose
bootloader was built with it.

Outputs, per build directory:

| File | Use |
|---|---|
| `ring-fw-merged.hex` | MCUboot + signed app: **SWD flashing** |
| `ring-fw/zephyr/zephyr.signed.bin` | signed, compressed update: **OTA** |
| `dfu_application.zip` | the same update packaged for nRF Connect Device Manager |
| `ring-fw/zephyr/zephyr.hex` | the app WITHOUT its MCUboot header. Do not flash it on its own: it won't boot |

The image version in the MCUboot header is
`CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION` in `prj.conf`. Keep it equal to
`CONFIG_BT_DIS_FW_REV_STR`.

## Updating

From the PC:

```bash
python tools/history_sync.py
```

```bash
python tools/ota_update.py build/ring-fw/zephyr/zephyr.signed.bin --yes
```

`ota_update.py --info` lists the ring's image slots. The upload takes
about a minute at a 15–30 ms connection interval. MCUboot then takes
roughly 10–20 s to decompress and boot, with no radio, before the ring
advertises again.

From the phone app: use Nordic's nRF Connect Device Manager library
(`iOSMcuManagerLibrary`, `Android-nRF-Connect-Device-Manager`, or the
`mcumgr_flutter` plugin) with the `.bin` or the `dfu_application.zip`. The
standard "test and confirm" or "confirm only" modes both work; in
overwrite-only mode they are the same. Required order:

1. Sync history (opcode `0x0E`) to the end.
2. Run the DFU. The library uploads, marks the image and resets.
3. Reconnect, check the DIS firmware revision, set the clock (`0x0D`),
   resend the profile if the app keeps it, and carry on syncing from the
   same seq.

The SMP characteristic needs an encrypted link, like the Ring Service. The
app's existing bond covers it.

## SWD flashing with the bootloader

```bash
python -m pyocd flash -t nrf52833 -f 1000000 build/ring-fw-merged.hex
```

`.\build.ps1 -Flash` does the same. A plain `pyocd flash` erases only the
sectors the hex covers, so the history log (0x50000) and the bonds
(0x7C000) survive. `pyocd erase --chip` wipes everything; that's only
needed for a module that still has APPROTECT on (FLASHING.md).

## First hardware run: checklist

On an unsealed board on the jig, with RTT:

1. Flash `ring-fw-merged.hex`. RTT should show the app boot as before, plus
   `ota: running image confirmed`. Boot time grows by MCUboot's slot-0
   signature check, which should be well under a second.
2. `python tools/prod_check.py` should pass check 7: slot 0 active,
   confirmed, 0.9.x.
3. `python tools/ota_update.py --info`.
4. Build again with a bumped version (`0.9.1`) and run
   `python tools/ota_update.py ... --yes`. Watch RTT for these, in order:
   `ota: upload started`, `history: suspended`, `ota: upload complete`.
   Then the reset. The ring should come back as 0.9.1, with
   `history: no log in this layout` / `formatting` / `next seq N` on boot,
   where N is the old seq.
5. Abandon test: start an upload and kill the tool halfway. After 10 min,
   RTT should show `upload abandoned, history takes slot 1 back` and
   `history: resumed`, and the next epoch should be stored.
6. Power during MCUboot's copy: the watchdog stays fed (MCUboot feeds every
   channel). Note that BOOST_EN floats while the bootloader runs (see
   HARDWARE_NOTES), so the 5 V boost may sit on for those ~10–20 s.

## Not done

- A bad but signed image can't be reverted. Fixing that needs swap mode,
  which needs two full slots, which needs external flash or a smaller app.
- The bootloader itself can't be updated over the air.
- There's no downgrade protection: any correctly signed image installs,
  older ones included.
