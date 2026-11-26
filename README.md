# LAN Mod for Saints Row: The Third Remastered

Play the co-op campaign of **Saints Row: The Third Remastered (PC)** over your local network, without
Steam lobbies or online services.

The mod adds a **LAN** option to the Co-op menu: one player hosts, the others on the same network find
the game and join. Every player needs it installed.

## For players

1. Download `LAN_Mod.zip` from the [Releases](../../releases) page and unzip it.
2. Close the game and double-click `LANMod_Installer.exe`.
   It finds the game automatically (Steam, GOG or Epic) or asks for the game folder.
3. In the game: **Co-op Campaign > LAN**. The host picks New Game or Load Game; the others pick Find
   Game. The host accepts the join request and starts the co-op game.

To remove it, run `Uninstall LAN Mod.bat`. Details and troubleshooting: [release/README.txt](release/README.txt).

No game files are distributed. The installer edits the menu inside your own copy of the game and keeps
a backup of the original bytes.

## How it works

| Part | What it does |
|---|---|
| `installer/lanmod_installer.c` | Finds the game. Reads `main_menu_coop_top.str2_pc` from `cache/interface.vpp_pc` by name, unpacks the menu script (LZ4), adds the LAN button with text edits that must each match exactly once, repacks it and writes it back into its own slot. Then updates the archive directory and the container manifest (`vint_doc_containers.asm_pc`). The original bytes go to `interface.vpp_pc.lanmod_backup`, and uninstall restores and verifies them. |
| `loader/` | A small mod loader: a `dinput8.dll` proxy. Windows loads it from the game folder; it forwards DirectInput to the real DLL and applies `modloader/patches/*.patch` in memory at startup. The game EXE is never modified, which keeps it compatible with DRM-wrapped EXEs. Patches find their target with byte signatures, so they also work when the code moves between store versions. |
| `patches/lan_join.patch` | Makes LAN joining work: one byte (`je` to `jmp`) in the LAN branch of the host-address check, so it accepts the host's address. |
| `tools/vpp.py`, `tools/vpp_write.py` | Python reader/writer for the game's `.vpp_pc` / `.str2_pc` packfiles (v6, LZ4), for modders. |

Technical notes (archive format, how LAN joining works, network protocol): [docs/TECHNICAL.md](docs/TECHNICAL.md).

### Patch file format (for other loader mods)

```
name   = lan_join_host_addr
sig    = f6 c3 40 48 8b 5c 24 30 74 16 83 7e 10 00 74 52 b8 02 00 00 00   # ?? = wildcard
offset = 14     # where the new bytes go, relative to the signature start
bytes  = eb
```

A patch is applied only if its signature matches exactly once. `modloader/modloader.log` reports each
patch as APPLIED, already applied, or NOT applied. The loader also supports `*.reloc` (move a static
table to a bigger buffer) and `*.cave` (replace instructions with a jump to new code); see the comments
in `loader/src/dinput8_proxy.c`.

## Building

You need a 64-bit Windows C compiler (LLVM-MinGW or MinGW-w64) and a POSIX shell (Git Bash works):

```sh
sh build.sh                         # -> dist/LAN_Mod/ and dist/LAN_Mod.zip
CC=x86_64-w64-mingw32-gcc sh build.sh
```

Developer check: `LANMod_Installer.exe build-menu "<game folder>" out.str2` writes the patched menu
container without installing anything.

## Status and help wanted

- Tested on the **Steam** version (EXE timestamp `0x60ee85f4`): menu, hosting, LAN discovery and
  2-player co-op over LAN. **GOG and Epic** should work, since everything is found by name, content or
  signature, but they are untested. Reports are welcome.
- More than 2 players: the game's network layer can be stretched, but the game itself is built around
  exactly two players (for example, only two character-appearance slots exist). It isn't part of this mod.
- Ideas: a small GUI for the installer, checking more store versions.

## License

[MIT](LICENSE) for the code in this repository. Saints Row: The Third Remastered belongs to its owners.
This project is not affiliated with or endorsed by them.
