# Radio emulator tests

Wadamesh's radio code running on a computer, against a modelled radio: the real
RadioLib SX126x driver, meshcomod's `CustomSX1262Wrapper` and Wadamesh's
`MediaMeshRadio`, each node on its own SX1262 model, all joined by a simulated
LoRa air. Packets take their real airtime, collide, and are only heard on the
frequency, bandwidth and SF they were sent on, so the MediaMesh scan, its
timing and its receive origins are tested end to end without a radio in hand.

```
emu/build.sh          # fetch the pinned deps, build, run every test
emu/build.sh build    # build only (emu/build/)
```

Needs g++ and git. `emu/fetch-deps.sh` puts RadioLib (7.8.1, what the
PlatformIO builds resolve `^7.6.0` to) and meshcomod (`core-v1.18.0`, as
platformio.ini pins) in `emu/third_party/` (gitignored). CI runs it in the
host-tests job.

## Tests

| Test | What it checks |
| --- | --- |
| `test_mediamesh_radio` | A MediaMesh sender alternates 20 packets on the main mesh (910.525 MHz, 62.5 kHz, SF7) and 20 on radio2 (911.150 MHz, 250 kHz, SF7). A scanning receiver must hear at least 90% of each, each credited to the profile it came on; a receiver with radio2 off must hear the main mesh only. Each node is its own process, as the wrapper's buffered receive is one per process. |

## Where it comes from

`emu/include`, `emu/core`, `emu/devices/sx1262.*` and `emu/air` are from
[MatterMesh OS](https://github.com/mattermeshos/mattermesh-os)'s T-Deck Plus PC
emulator (`tdeck-plus/emu`, GPL-3.0; see NOTICE). Changes for Wadamesh, marked
in the source:

- **FreeRTOS shim:** task notifications (`ulTaskNotifyTake`,
  `xTaskNotifyGive`), recursive mutexes and `xSemaphoreGiveFromISR`, which the
  MediaMesh scan and the wrapper's receive drain use.
- **Thread-safe chip:** the SX1262 model is driven from the scan task, the
  drain task and the main loop; an SPI transaction or a pump holds the chip.
- **Mid-preamble acquisition:** a receiver that tunes in while a preamble is
  on the air acquires it after 4 of its symbols (if the preamble lasts that
  long), and one that leaves sooner doesn't. A scan lives or dies by this.
- **No hardware we don't model:** the SD card, NVS, I2S and inflate pieces are
  left out, and LEDC calls are no-ops.

The air broker (`emu-air`) takes a topology file: lines of
`A B rssi snr [loss%]` (both ways), `A > B ...` (one way), or
`default rssi snr loss`.
