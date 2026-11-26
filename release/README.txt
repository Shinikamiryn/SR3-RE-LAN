LAN Mod for Saints Row: The Third Remastered
=============================================

Play the co-op campaign over your local network (LAN), without Steam lobbies or online services.
The PC version still contains the game's LAN ("System Link") code, but as shipped the LAN menu entry
is missing and joining a LAN game fails. This mod restores the menu entry and makes LAN joining work.

Works with any PC copy of the game (Steam, GOG, Epic, ...). Every player needs the mod installed.


INSTALL
-------
1. Close the game.
2. Unzip this folder anywhere.
3. Double-click LANMod_Installer.exe.
   It finds the game automatically (Steam, GOG or Epic). If it can't, it asks you to paste the game
   folder (the folder that contains SRTTR.exe).

That's it. No game files are included with the mod: the installer edits the menu in your own copy
and keeps a backup of the original bytes.


PLAY
----
Host:   Main menu > Co-op Campaign > LAN > New Game (or Load Game). Wait in the lobby.
Others: Main menu > Co-op Campaign > LAN > Find Game, pick the host, Load or Create character.
The host accepts the join request, then chooses Start Co-op Game.

All PCs must be on the same local network. Windows Firewall may ask once to allow the game on
private networks; allow it. The game uses UDP port 4200.
Two players, as in the original co-op.


UNINSTALL
---------
Double-click "Uninstall LAN Mod.bat" (or run: LANMod_Installer.exe uninstall).
It restores the original menu from the backup and removes the mod loader.
If you ever lose the backup, use your store's "Verify integrity of game files" (Steam) /
"Verify" (Epic) / "Verify / Repair" (GOG) instead.


WHAT IT CHANGES
---------------
- cache\interface.vpp_pc: the Co-op menu gets a "LAN" entry. Backup:
  cache\interface.vpp_pc.lanmod_backup
- dinput8.dll + modloader\ in the game folder: a small mod loader that applies
  modloader\patches\lan_join.patch in memory each time the game starts, so LAN joining works.
  The game's .exe file is never modified.
  modloader\modloader.log shows "patch lan_join_host_addr: APPLIED" when it is active.


OTHER COMMANDS
--------------
LANMod_Installer.exe status               show what is installed
LANMod_Installer.exe install "<folder>"   install into a specific game folder
LANMod_Installer.exe uninstall "<folder>"


TROUBLESHOOTING
---------------
- "another dinput8.dll is already in the game folder": another mod or tool (for example an overlay
  or ReShade) uses that file name. Remove it or install it differently, then run the installer again.
- "the menu script is not the one this mod expects": your game version differs from the one the mod
  was made for. Nothing was changed. Please report your store and game version.
- LAN entry missing after a game update: updates can replace interface.vpp_pc. Run the installer again.
- Host not listed under Find Game: check that both PCs are on the same network and that the firewall
  allows the game.


Source code, technical notes and issue reports: see the project's GitHub page.
License: MIT (the mod's own code). Saints Row: The Third Remastered belongs to its owners; no game
files are distributed with this mod.
