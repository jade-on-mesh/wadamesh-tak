# Models the standalones download

| File | What | Size | SHA-256 |
|---|---|---|---|
| `vox-restorer-small.bin` | The AI voice restorer (VOICE_RESTORER.md): MeshCore Open TAK's voice training kit, option 4, small restorer, 91,962 steps. 128 channels, 4 ConvNeXt blocks, fp16 ("VRS1"). | 1,001,348 bytes | `4ead0284f9c450252f8811b3477b5f017656e16b62165fd4593d87718afafda7` |

The T-Deck, T-Deck Max, T-Lora Pager, T-Display P4 and Tanmatsu fetch it over
Wi-Fi from their own release's tree (else `main`) when the AI voice restorer is
on and it isn't on the standalone yet, check it loads, and keep it on the SD
card (else internal storage). Replace it here to give every standalone a new
model; a copy on a standalone's SD card (`/models/vox-restorer-small.bin`) is
used first.
