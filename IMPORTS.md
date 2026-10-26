# Importing from other firmware and apps

**Settings > Backups > Import** brings a standalone's identity, channels and
contacts over from what you used before.

| From | How | What comes over |
| --- | --- | --- |
| The MeshCore app (official) | Its export, `meshcore-backup.json`, on the SD card: **Import backup** | Name and key, radio settings, channels, contacts |
| Squatch Mesh (T-Lora Pager) | Its export, `meshcore-backup.json`, the same way; or its SD card's `/inw` folder: **From another firmware** | Key (and name), channels, contacts |
| MeshPunk (T-Deck) | Its SD card's `/meshpunk` folder: **From another firmware** | Key, channels (Public too, unless you deleted it there), contacts |
| MeshCore Open | Its contacts export (`meshcore_discovered_contacts.json`) on the SD card: **Import backup** | Contacts, added to the ones you have |

**From another firmware** looks for those folders on the SD card, turns what
it finds into a MeshCore backup (`/wadamesh-import-<name>.json`, left on the
card), and imports that. Like any backup import it replaces the identity,
channels and contacts here, then restarts. Messages, maps and each
firmware's own settings stay behind.

Not yet: MeshCore One, Ripple and MeshOS. Their files aren't documented; a
sample export (with a throwaway identity) is what's needed to add them.

Formats: `src/helpers/ForeignImport.h`; host test:
`test/test_foreign_import.cpp`.
