# Contributing

Thanks for helping. Reports from other store versions are the most useful contribution right now.

## Reporting a problem or a store version

Open an issue with the **Problem / store report** form. It asks for:
- your store (Steam, GOG, Epic, other) and game version;
- the output of `LANMod_Installer.exe status`;
- `modloader\modloader.log` from the game folder, if the mod loader is installed.

The installer never writes anything when something doesn't match what it expects, so a report of an
error message is safe and very helpful.

## Changing the code

1. Fork the repository and create a branch.
2. Build: `sh build.sh` (LLVM-MinGW or MinGW-w64). The output is in `dist/LAN_Mod/`.
3. Test on your copy of the game: install with the built `LANMod_Installer.exe`, start the game, check
   **Co-op Campaign > LAN** and `modloader\modloader.log`, then run `uninstall` and check the game is back
   to normal.
4. Open a pull request and describe what you changed and how you tested it.

Every pull request is built automatically (GitHub Actions). The built mod is attached to the run as
**LAN_Mod-test-build**, so others can try it before it is merged.

## Rules for the release

- **No game files in the repository or the release.** The installer builds everything from the player's
  own copy.
- **No binaries in pull requests.** EXE and DLL files are built from source by `build.sh` and by the
  automatic build, never committed.
- Find things by name, content or byte signature, never by fixed file offsets or addresses, so all store
  versions keep working.
- If the installer can't verify something, it must refuse and change nothing.
- No networking, telemetry or downloads in the installer or the loader.

## Ideas

- Confirm or fix GOG and Epic versions.
- A small window (GUI) for the installer.
- Translations of the player readme.
