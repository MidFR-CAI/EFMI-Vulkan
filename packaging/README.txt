EFMIVulkan
==========

Runs EFMI (XXMI) mods for Arknights: Endfield on the game's Vulkan renderer, so you do not
have to switch the game to DirectX 11.

It is a Vulkan layer that follows EFMI's own rules (the files in XXMI's Core\EFMI folder):
the same mods, toggles and keys, no changes to the mods.


Requirements
------------
- Windows 10 or 11, 64-bit.
- Arknights: Endfield, installed with the official GRYPHLINK launcher.
- Your EFMI mods, in XXMI Launcher's EFMI\Mods folder (install them as usual with XXMI).
  XXMI itself does not need to run.


How to use
----------
1. Unzip this folder anywhere and keep its files together.
2. Close the game. Run EFMIVulkan.exe and accept the admin prompt
   (the game runs as administrator, so the layer has to be registered for all users).
3. The first time, it asks for two folders, and remembers them in config.ini:
     - your EFMI Mods folder (XXMI Launcher\EFMI\Mods)
     - the GRYPHLINK folder (the one with Launcher.exe)
4. The official launcher opens. Start the game there as usual, on Vulkan
   ("Launch with DirectX 11" unticked). Leave the EFMIVulkan window open while you play.
5. When the game closes, EFMIVulkan removes itself again. It is only active while you play
   through it, and only inside Endfield: other Vulkan programs ignore it.


Keys (in game)
--------------
Shift+F8      fix mods broken by a game update (also: F in the EFMIVulkan window)
Shift+F9      mods on or off
Shift+F10     reload every mod, without restarting the game (also: R in the EFMIVulkan window)
Mod keys      every mod's own toggle keys work as in XXMI, and are remembered

Debug keys (F10 frame capture, Ctrl+F10 / Ctrl+Shift+F10 auto-fix in two steps, Alt+M
magenta textures, Alt+H hide the mods, Alt+N hide one draw, Alt+B bone modes) are off. To
turn them on, for looking into a mod that does not draw right, create an empty file named
"debugkeys" in %USERPROFILE%\efmivulkan and start the game again.


Fix mods (mods broken by a game update)
---------------------------------------
Have some of the broken characters on screen, close enough to see clearly, and press
Shift+F8 in game (or F in the EFMIVulkan window). The mods go off for a moment while
EFMIVulkan looks at the game's own characters, and a few seconds later you hear a beep: the
mods it could fix are fixed and reloaded. Each press is kept for the session and adds to the
ones before, so swap in the next broken characters and press it again:
  - A part whose ID (hash) the update changed gets the new one: the game's part of the same
    size on the same character. A part the update re-made (another size) cannot be fixed:
    the mod needs a new version from its author.
  - A texture whose ID changed gets the one game texture of its size and format there.
  - Texture slots: mods that put a texture into a numbered DX11 slot (ps-t14 = ...) need to
    know where that slot is on Vulkan. Each capture adds what it sees to slots.ini, and a
    slot is used once that is clear.
Every changed mod ini is backed up first, next to it ("mod.ini <date> <time>.BAK"); to undo,
delete the ini and rename the .BAK back. What was changed, and what could not be and why,
is in %USERPROFILE%\efmivulkan\autofix.log. The fixed mods are reloaded straight away.


Reload
------
Shift+F10 (or R in the EFMIVulkan window) reads your Mods folder again: new, edited and
removed mods take effect without restarting the game.


Logs and problems
-----------------
Logs are in %USERPROFILE%\efmivulkan (efmivulkan.log, launch.log).

If a mod does not look right, open an issue on the GitHub page and attach efmivulkan.log.
The log names each mod component it could not draw, and why. Please also say whether the
mod works on DirectX 11 with XXMI: a mod broken by a game update is broken there too.

Known gaps: shape keys, and draws of several copies of one mesh at once, are not done yet.

To remove EFMIVulkan, delete this folder. If the game ever crashed while it was running, run
EFMIVulkan.exe --unregister once first.

Changing folders: edit config.ini, or delete it to be asked again.
