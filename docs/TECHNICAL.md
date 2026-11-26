# Technical notes

Addresses are for the Steam build (EXE PE timestamp `0x60ee85f4`, default image base `0x140000000`).
Other builds will differ, which is why the mod finds its targets by name, content and byte signature.

## The game's packfiles (`.vpp_pc`, `.str2_pc`)

Packfile version 6, little-endian, 64-bit fields.

| Offset | Field |
|---|---|
| `0x000` | magic `CE 0A 89 51` |
| `0x004` | version (6) |
| `0x14C` | flags: `0x0000` uncompressed, `0x4801` LZ4-compressed, `0x4803` compressed + condensed (packed back-to-back; used by `.str2_pc`) |
| `0x158` | entry count, file size, directory size, names size, uncompressed data size, compressed data size (6 × u64) |
| `0x1000` | directory: 0x30 bytes per entry (name offset, 0, data offset, size, compressed size or `-1`, flags) |
| next 0x1000 | names (NUL-terminated) |
| next 0x1000 | data |

- **Compressed data:** split into blocks, each with a 16-byte header (`EE DB EE 0F EE DB BA 00`, u32 compressed size, u32 uncompressed size) followed by an LZ4 block.
- **Uncompressed packs:** like `interface.vpp_pc`, entries are 0x1000-aligned, so each one has some slack before the next.

`tools/vpp.py` reads and checks packfiles; `tools/vpp_write.py` writes them. It reproduces original files byte for byte in all three modes.

## The LAN menu entry

The Co-op menu is the UI container `main_menu_coop_top` (`main_menu_coop_top.str2_pc` inside `cache/interface.vpp_pc`).

- **Missing button:** its script `main_menu_coop_top.lua` still contains the LAN handler `main_menu_coop_top_dlc_check_lan`, but no button reaches it.
- **The edit:** the mod adds a button labelled `MAINMENU_SYSLINK_TITLE` (the game's own string, which shows as "LAN") and routes it to that handler.
- **Override archive ignored:** the game ignores `patch_compressed.vpp_pc` for UI screens not listed in `patch_vint_doc_containers.asm_pc`, which ships empty. So the container is replaced **in place**: its slot in `interface.vpp_pc` has 16384 bytes, and the patched container is about 15.9 KB.
- **Manifest sizes:** the game sizes its load buffers from the container manifest `vint_doc_containers.asm_pc`. Its record for `main_menu_coop_top.vint_doc` stores the compressed container size (record + `0x34`) and the script size (record + `0x4C` and `0xAA`). These must be updated, or the menu screen breaks.

## Why LAN joining fails as shipped, and `lan_join.patch`

- **Where:** join step 2 (`0x14046d20a`), which LAN sessions also run. It fills the host address with `GetLobbyOwner()` and marks it as a Steam address (`session+0x38 = 1`, at `0x14046d26c`).
- **What happens:** the host-address check `0x140469c30` requires an IP address (`+0x10 == 0`) for LAN sessions and otherwise returns 2. So a LAN join ends with "the session is no longer available", whoever hosts. The Steam path sets the address type; the LAN path was never adjusted for it.
- **The patch:** RVA `0x469c91`, `74 52` → `EB 52`. Signature `f6 c3 40 48 8b 5c 24 30 74 16 83 7e 10 00 [74] 52 b8 02 00 00 00`. Only the LAN branch is affected; online play is unchanged.
- **Delivery:** the Steam EXE is wrapped by SteamStub, so the patch is applied in memory by the mod loader rather than on disk.

## How LAN discovery and joining work

- **Port:** every peer uses UDP port 4200.
- **Searching:** a searcher broadcasts a connectionless `ping` (`00 00 ff ff` + message id 0). Hosts answer with a `pong` sent to port 4200 of the searcher's IP, not to the source port.
- **The `pong`:** it carries the host name (UTF-16LE) and a session descriptor. The join address comes from the descriptor, not from the pong's sender address. Server list entries expire about 10 s after the last pong.
- **Who answers:** a host answers searches only while its session is joinable (LAN handler around `0x14045eb12`).
- **Connected traffic:** after joining, traffic is sequenced and acknowledged. Message ids include `join_rel` 4, `join_unrel` 5, `synced_sys` 26 and `acks` 31; the message table is at `0x141d6f610`.
- **Firewall:** Windows Firewall must allow the game on private networks.

## The mod loader

- **How it loads:** `dinput8.dll` in the game folder. The game imports `DINPUT8.dll`, so Windows loads this copy first.
- **Forwarding:** it forwards `DirectInput8Create`, `DllGetClassObject`, `DllCanUnloadNow`, `DllRegisterServer` and `DllUnregisterServer` to `System32\dinput8.dll`.
- **Patching:** in `DllMain` it applies `ploader/patches/*.reloc`, then `*.cave`, then `*.patch`, before the game's own startup code runs. Then it loads `ploader/plugins/*.dll` on a worker thread.
- **Log:** `ploader/ploader.log`.
