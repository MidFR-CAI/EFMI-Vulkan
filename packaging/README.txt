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
F9            mods on or off
Mod keys      every mod's own toggle keys work as in XXMI, and are remembered
Alt+M         debug: every texture a mod replaces turns magenta
Alt+H         debug: hide everything the mods draw (shows what the game draws itself)


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
