HogsFix - Hogs of War / Frontschweine (PC, 2000) auf Windows 10/11
=====================================================================

(English below)

DEUTSCH
-------
HogsFix ist ein einzelnes Plugin (HogsFix.asi), das beim Start alle Fehler im Speicher behebt. Es wird keine Spieldatei
verändert und nichts in die Registry geschrieben. Kein dgVoodoo, DDrawCompat, GOG-winmm-Proxy oder libvorbis nötig.

Was behoben wird:
  - Absturz/Schwarzbild nach dem Start (DirectDraw-Oberflächenverlust, Bink-Absturz), "Terminating"-Meldung
  - Alt-Tab, Win-Taste, Minimieren: Spiel läuft danach weiter (vorher eingefroren bzw. sehr langsam)
  - GOG-Version: Alt-Tab und Win-Taste waren durch GOGs Kompatibilitätseintrag ("IgnoreAltTab") gesperrt
  - Computer-Schweine bewegen sich nie (KI-Pfadsuche)
  - flackernde/fehlende Geländeteile auf 32-Bit-Desktops
  - Titelbild: "Taste drücken" ohne weiße Kästen
  - Breitbild: Auflösungen bis 2048 Pixel Breite im Startfenster, Kampf in Hor+ (z. B. 1920x1080).
    Mehr schafft DirectX 7 nicht (2560x1440/4K: "Failed to create D3DDevice"); ein 4K-Bildschirm
    zeigt 1920x1080 exakt 2x skaliert.
  - Kampf mit 60 FPS (Original 25) mit Zwischenbildern, CPU-Last ~10 % statt 100 %
  - Menüs flüssig (vorher 10-15 FPS), Menü-Animationen im Originaltempo
  - CD-Musik aus MUSIC\TrackNN.ogg (GOG-Version), Lautstärkeregler im Spiel funktioniert
  - Fenstermodus (Windowed=1): Größe änderbar, Intro-Filme und Menügrafiken korrekt
  - kein CD-Check, läuft aus jedem Ordner (portabel)
  - das Spiel deaktivierte alle anderen Fenster (nach Absturz Taskleiste unbedienbar)
  - Gamepad: Xbox-Controller (und alle XInput-Gamepads) funktionieren ohne Einrichtung, mit der Belegung der
    PlayStation-Version (siehe unten)

Voraussetzung: die GOG-Version des Spiels. Unterstützt werden GOGs warhogs_.exe (unverändert, so wie GOG sie
installiert) und die No-CD-Warhogs.exe der Verkaufsversion vom September 2000 (Datei-Zeitstempel 0x39C903E8,
964.608 Byte). Mit einer anderen exe ändert HogsFix nichts (steht in HogsFix.log, wenn Log=1).

Installation:
  1. Die drei Dateien dinput.dll, HogsFix.asi, HogsFix.ini in den Spielordner kopieren (dort, wo warhogs_.exe liegt).
  2. Das Spiel wie gewohnt starten (GOG-Verknüpfung, GOG Galaxy oder warhogs_.exe). Im Startfenster Auflösung wählen
     (z. B. 1920x1080) und Start.
  Nur wer statt warhogs_.exe eine No-CD-Warhogs.exe startet: die winmm.dll der GOG-Version löschen oder umbenennen
  (mit ihr hängt diese exe schon beim Laden). Für warhogs_.exe wird sie nicht benutzt und darf bleiben.
  Die GOG-Dateien libogg-0.dll, libvorbis-0.dll, libvorbisfile-3.dll werden nicht gebraucht (dürfen bleiben).
  Update von einer älteren HogsFix-Version, die als winmm.dll installiert wurde: diese winmm.dll darf gelöscht werden.

Deinstallation: die drei Dateien löschen (und ggf. die GOG-winmm.dll wiederherstellen, z. B. per GOG Galaxy
"Dateien überprüfen/reparieren").

Hinweise:
  - Beim ersten Start startet sich das Spiel einmal selbst neu (Kompatibilitätsmodus für 16-Bit-Farben und
    hohe DPI, nur für diesen Prozess). Windows trägt den Modus "DWM8And16BitMitigation" eventuell selbst dauerhaft
    für die exe ein (Programmkompatibilitäts-Assistent) - das ist normal und harmlos.
  - Warum dinput.dll: GOGs Kompatibilitätseintrag für warhogs_.exe lässt Windows die System-winmm.dll laden, bevor
    das Spiel seine Dateien lädt - eine winmm.dll im Spielordner wird dort nie geladen. dinput.dll ist der
    Ultimate ASI Loader unter einem anderen Namen und leitet DirectInput unverändert an Windows weiter.
  - dgVoodoo kann zusätzlich benutzt werden (DDraw.dll + D3DImm.dll in den Spielordner), ist aber nicht nötig.
  - Lautstärke der Musik: im Spiel ("Lautstärke gesamt") oder HogsFix.ini [Music] Volume.

Gamepad (Xbox 360/One/Series und andere XInput-Gamepads; einfach anschließen, auch während das Spiel läuft):
  Belegung wie auf der PlayStation (Taste an derselben Stelle), Tastatur funktioniert weiter:
    Steuerkreuz / linker Stick   Schwein bewegen, Menüs, Waffenmenü, Zielen          (Pfeiltasten)
    A  (PS: X)                   Menü: auswählen. Kampf: Feuer (halten = Stärke), Waffe wählen  (Eingabe / Leertaste)
    B  (PS: Kreis)               Kampf: Waffenmenü. Menü: zurück                    (Eingabe / Rücktaste)
    X  (PS: Quadrat)             Springen                                           (Umschalt rechts)
    Y  (PS: Dreieck)             Fahrzeug/Geschütz ein/aus, Waffe abwählen, Menü zurück   (Rücktaste)
    LB (L1, halten)              Kamera / genaues Zielen mit den Richtungen         (Strg rechts)
    LT (L2)                                                                         (Pos1)
    RB (R1) / rechter Stick hoch    Flugbahn höher, Schwein wechseln                (Bild auf)
    RT (R2) / rechter Stick runter  Flugbahn tiefer, Schwein wechseln               (Bild ab)
    Start                        Pause (Menü: auswählen)                            (Esc)
    Back/View (Select)                                                              (Entf)
  Auswertung nach dem Kampf: A = Weiter (Leertaste), Y = Wiederholen (Esc). Intro-Filme: A, B oder Start.
  Im Kampf drückt das Gamepad die Tasten, die unter Optionen -> Steuerung eingestellt sind; eine geänderte
  Tastenbelegung gilt also auch für das Gamepad (Tasten werden dort mit der Tastatur zugewiesen).
  Gamepads ohne XInput (ältere DirectInput-Gamepads) benutzt das Spiel wie bisher selbst (Optionen -> Steuerung).

HogsFix.ini (1 = an, 0 = aus):
  [General]    Log         HogsFix.log schreiben (für Fehlerberichte)
  [Fixes]      NoCD        CD-Prüfung überspringen
               Portable    Spielordner = Ordner der exe (statt Registry-Pfad)
               AIFix       Computer-Schweine bewegen sich
               WinFix      andere Fenster nicht mehr deaktivieren
               ResumeFix   Spiel läuft nach Minimieren/Alt-Tab weiter
               AltTab      GOG-warhogs_.exe: Alt-Tab und Win-Taste wieder erlauben
               NviewFix    kein Gelände-Flackern
               TitleText   "Taste drücken" auf dem Titelbild ohne weiße Kästen
  [Widescreen] Enable      Breitbild-Auflösungen (bis 2048 breit), Hor+-Sichtfeld
               NviewWiden  Sichtbereich des Geländes um N Blöcke erweitern (0-4)
  [Battle]     Enable      neue Kampfschleife (keine 100 % CPU)
               FrameHz     Bildrate im Kampf (0 = Original 25)
               Interp      Zwischenbilder berechnen (0 = Bilder wiederholen)
  [Display]    Fix         DirectDraw-Korrekturen (Abstürze, Alt-Tab, Filme, Grafik)
               Windowed    im Fenster statt Vollbild
               Shim16Bit   16-Bit-Kompatibilitätsmodus per Neustart setzen
               DpiAware    DPI-Modus per Neustart setzen (sonst Bild größer als der Bildschirm)
               FastBlit    schnelle Menügrafik
               MenuFPS     Bildrate der Menüs (25 = Originaltempo der Animationen, 0 = unbegrenzt)
  [Music]      Enable      CD-Musik aus MUSIC\TrackNN.ogg
               Volume      Musiklautstärke in Prozent
  [Gamepad]    Enable      XInput-Gamepad mit PlayStation-Belegung
               Deadzone    Totzone des linken Sticks in Prozent (Standard 30)

Mitgeliefert: dinput.dll = Ultimate ASI Loader 9.7.4 (x86) von ThirteenAG, MIT-Lizenz,
https://github.com/ThirteenAG/Ultimate-ASI-Loader . HogsFix enthält stb_vorbis (Sean Barrett, Public Domain).


ENGLISH
-------
HogsFix is a single plugin (HogsFix.asi) that fixes everything in memory at startup. No game file is modified and
nothing is written to the registry. No dgVoodoo, DDrawCompat, GOG winmm proxy or libvorbis needed.

What it fixes:
  - crash/black screen after starting (DirectDraw surface loss, Bink crash), "Terminating" message
  - alt-tab, Windows key, minimize: the game continues afterwards (used to freeze or run very slowly)
  - GOG version: Alt+Tab and the Windows key were blocked by GOG's compatibility entry ("IgnoreAltTab")
  - computer pigs never move (AI path request)
  - flickering/missing terrain blocks on 32-bit desktops
  - title screen: "press any key" text without white boxes
  - widescreen: resolutions up to 2048 pixels wide in the launcher, Hor+ battle view (e.g. 1920x1080).
    DirectX 7 cannot go higher (2560x1440/4K: "Failed to create D3DDevice"); a 4K screen shows
    1920x1080 at exactly 2x.
  - battles at 60 FPS (originally 25) with interpolation, ~10 % CPU instead of 100 %
  - smooth menus (were 10-15 FPS), menu animations at their original speed
  - CD music from MUSIC\TrackNN.ogg (GOG version), in-game volume works
  - windowed mode (Windowed=1): resizable, intro movies and menu art correct
  - no CD check, runs from any folder (portable)
  - the game disabled every other window (taskbar unusable after a crash)
  - gamepad: Xbox controllers (and any XInput gamepad) work without setup, with the PlayStation version's layout
    (see below)

Requirements: the GOG version of the game. Supported exes: GOG's warhogs_.exe (unmodified, as GOG installs it) and the
no-CD Warhogs.exe of the September 2000 retail build (timestamp 0x39C903E8, 964,608 bytes). With any other exe
HogsFix changes nothing (HogsFix.log says so when Log=1).

Install:
  1. Copy dinput.dll, HogsFix.asi and HogsFix.ini into the game folder (next to warhogs_.exe).
  2. Start the game as usual (GOG shortcut, GOG Galaxy or warhogs_.exe), pick a resolution in the launcher
     (e.g. 1920x1080), Start.
  Only if you run a no-CD Warhogs.exe instead of warhogs_.exe: delete or rename GOG's winmm.dll (that exe hangs while
  loading with it). warhogs_.exe never uses it, so it can stay.
  GOG's libogg-0.dll, libvorbis-0.dll and libvorbisfile-3.dll are not used (can stay).
  Updating from an older HogsFix that was installed as winmm.dll: that winmm.dll can be deleted.

Uninstall: delete the three files (and restore GOG's winmm.dll if you removed it, e.g. with GOG Galaxy's
verify/repair).

Notes:
  - On the first start the game restarts itself once (compatibility layers for 16-bit colour and high DPI, for this
    process only). Windows may add the "DWM8And16BitMitigation" layer for the exe permanently by itself (Program
    Compatibility Assistant) - normal and harmless.
  - Why dinput.dll: GOG's compatibility entry for warhogs_.exe makes Windows load the system winmm.dll before the game
    loads its own files, so a winmm.dll in the game folder is never loaded there. dinput.dll is the Ultimate ASI
    Loader under another name and passes DirectInput through to Windows unchanged.
  - dgVoodoo can be used on top (DDraw.dll + D3DImm.dll in the game folder) but is not needed.
  - Music volume: in game ("Lautstärke gesamt" / master volume) or HogsFix.ini [Music] Volume.

Gamepad (Xbox 360/One/Series and other XInput gamepads; just plug it in, also while the game is running):
  Same layout as on the PlayStation (button in the same place), the keyboard keeps working:
    D-pad / left stick           move the pig, menus, weapon menu, aiming           (arrow keys)
    A  (PS: cross)               menu: select. Battle: fire (hold = power), pick weapon  (Return / Space)
    B  (PS: circle)              battle: weapon menu. Menu: back                    (Return / Backspace)
    X  (PS: square)              jump                                               (right Shift)
    Y  (PS: triangle)            enter/leave vehicles and guns, deselect weapon, menu back  (Backspace)
    LB (L1, hold)                camera / precise aiming with the directions        (right Ctrl)
    LT (L2)                                                                         (Home)
    RB (R1) / right stick up     trajectory up, cycle pigs                          (Page Up)
    RT (R2) / right stick down   trajectory down, cycle pigs                        (Page Down)
    Start                        pause (menus: select)                              (Esc)
    Back/View (Select)                                                              (Delete)
  Results after a battle: A = continue (Space), Y = retry (Esc). Intro movies: A, B or Start.
  In battle the gamepad presses the keys set in Optionen -> Steuerung (controls), so changed key bindings apply to the
  gamepad too (keys are assigned there with the keyboard).
  Gamepads without XInput (older DirectInput pads) are used by the game itself as before (Optionen -> Steuerung).

HogsFix.ini (1 = on, 0 = off): see the comments in the file; the keys are listed in the German section above.

Included: dinput.dll = Ultimate ASI Loader 9.7.4 (x86) by ThirteenAG, MIT license,
https://github.com/ThirteenAG/Ultimate-ASI-Loader . HogsFix contains stb_vorbis (Sean Barrett, public domain).
