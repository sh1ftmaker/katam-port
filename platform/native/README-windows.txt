katam-port -- Kirby & The Amazing Mirror, from the decompilation
================================================================

There is no game in this folder.

This program is a port of the Kirby & The Amazing Mirror decompilation: it is
the game's code, rebuilt for a PC, with a software Game Boy Advance around it.
It contains none of the game's data -- no graphics, no music, no levels -- and
it has no way to obtain any.  You supply your own copy of the ROM, from a
cartridge you own.

    katam.exe C:\path\to\your-rom.gba

or start katam.exe with no arguments and either pick the file in the dialog it
offers, or drag the .gba file onto its window.

The decompilation matches the US release, game code B8KE.  Another region or a
ROM hack will load and will not work properly, and the program says so.


What is in the folder
---------------------

    katam.exe     the port
    SDL2.dll      the window, the sound and the gamepad, from libsdl.org.
                  It has to sit next to katam.exe; Windows will not find it
                  anywhere else.
    README.txt    this file


The console window
------------------

katam.exe opens a console window alongside the game.  That is deliberate.
Everything the port has to say goes there: which ROM it loaded, where your
save file is, and -- on the one failure it cannot work around -- why it could
not start.  Closing the console closes the game.


Controls
--------

    arrow keys or WASD    move
    J or Z                A
    K or X                B
    Q, E                  L, R
    Enter                 Start
    Backspace or R.Shift  Select

    F11                   full screen (Escape leaves it)
    1 - 6                 window size
    F12                   save a screenshot next to katam.exe
    Ctrl+Q                quit

A gamepad works without setting anything up.


Your save
---------

Saves go to

    %APPDATA%\katam-port\katam-port\

as an ordinary 64 KiB .sav file, named after your ROM.  It is the same format
an emulator reads and writes, so a save can be moved between this port, the
browser build, and an emulator.  The game writes it a second or two after it
saves in-game; do not kill the process during the "SAVING" message.


Licence
-------

SDL2 is distributed under the zlib licence and is not part of this project;
see https://libsdl.org.  The port itself carries the licence in the repository
it was built from.
