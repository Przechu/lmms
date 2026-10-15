# LMMS on Haiku: MIDI

This fork adds native MIDI support for [Haiku](https://www.haiku-os.org) to LMMS.
Upstream LMMS has no MIDI client for Haiku (it has ALSA, OSS, sndio, WinMM and
Apple clients, none of which exist there), so on Haiku LMMS falls back to the
dummy client and no MIDI device can be chosen.

The change is one new MIDI client, `MidiHaiku`, built on Haiku's MIDI Kit
(`libmidi2`). Everything else is the upstream code.

## What it does

- The MIDI sources and sinks Haiku knows about (hardware ports, MidiSynth,
  other applications) are listed as readable and writable ports. They appear in
  the usual places: the instrument's MIDI tab, the MIDI controller dialog, and
  **Settings > MIDI > MIDI interface > Haiku MIDI**.
- Subscribing a track to a port connects a small LMMS endpoint to it. These show
  up in Haiku's MIDI patchbay as `LMMS <- name` (input) and `LMMS -> name`
  (output), so the connections can also be seen and changed there.
- Note on/off, key pressure, controllers, program change, channel pressure and
  pitch bend are passed in both directions. SysEx and clock are not.
- Ports follow the roster: a device or application that appears while LMMS is
  running is added to the lists, and a subscription to one that went away is
  meant to reconnect when it returns.
- Tested so far: output to MidiSynth, input from MidiSynth, hot-plugged sources
  appearing in the list, and pitch bend. Not yet tested:
  controller and program-change traffic, hardware keyboards, reconnecting after a
  device goes away, and 32-bit Haiku.
- Haiku MIDI is the default client on Haiku, so nothing needs to be configured.

## The patch

| File | Change |
| --- | --- |
| `include/MidiHaiku.h` | New. The client's interface; it does not include any Haiku header. |
| `src/core/midi/MidiHaiku.cpp` | New. The client. |
| `src/core/AudioEngine.cpp` | Try the Haiku client first on Haiku; accept its name in the settings. |
| `src/gui/modals/SetupDialog.cpp` | Offer "Haiku MIDI" in the MIDI settings. |
| `src/core/CMakeLists.txt` | Build `MidiHaiku.cpp`. |
| `src/CMakeLists.txt` | Link `libbe` and `libmidi2` on Haiku. |
| `src/core/AudioEngineWorkerThread.cpp` | Not MIDI: stop the audio thread from starving its workers (see below). |
| `src/gui/MainApplication.cpp`, `include/MainApplication.h` | Not MIDI: make dragging with the mouse and drag and drop work (see below). |
| `plugins/ZynAddSubFx/CMakeLists.txt` | Not MIDI: build ZynAddSubFx's GUI on Haiku (it took the Windows branch) and link `libbsd`. Needs the `fltk` package. |

All of it is guarded by `LMMS_BUILD_HAIKU`, so no other platform is affected.
The submodules `src/3rdparty/hiir` and `plugins/MidiImport/portsmf` are needed
by this LMMS version; the HaikuPorts recipe has to provide them.

## Audio thread and its workers

On Haiku the audio thread runs at a real-time priority, set by the media
system, while `QThread::setPriority()` does nothing, so the render workers stay
at the normal priority. At the end of every period the audio thread used to spin
until all jobs were done. A worker that was ready to run but had not been given a
CPU could then wait for ever behind the spinning audio thread, and LMMS stopped
playing with the audio thread at full load. This showed up when the ZynAddSubFx
GUI was opened, because the plugin then waits for another process, but it can
happen with any plugin.

The wait loop now spins briefly and then sleeps in short steps, and the workers
get the audio thread's priority.

## Dragging with the mouse

Qt on Haiku sends the moves of the mouse without the buttons that are held down,
although it knows that one is down. Qt gives such moves only to widgets that track
the mouse, so the windows of LMMS could not be dragged by their title bars, and
other widgets that rely on the buttons in a move may have been affected too.
`MainApplication::notify()` now gives those moves the buttons before the widgets
see them. The cause is in Qt's Haiku platform plugin; this works around it.

## Drag and drop

Qt on Haiku has only its basic drag and drop within one application. It takes a
widget's acceptance of a drag when the drag enters as the answer for the moves
that follow, but the last move before the button was released, which repeats
the cursor's position, came back as not accepted. Qt then cancelled the drop, so
dragging a preset, a sample or a clip into the Song Editor did nothing.

`MainApplication::notify()` now treats a move that the widget has not handled as
accepted when the same widget accepted the drag on entering. LMMS's widgets all
decide on entering, so what may be dropped where is unchanged.

## Building

Use the usual LMMS build, on Haiku. Qt 5 is what the HaikuPorts package uses:

    cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DWANT_QT5=ON \
        -DWANT_JACK=OFF -DWANT_WEAKJACK=OFF -DWANT_VST=OFF
    cmake --build build

The submodules (`git submodule update --init --recursive`) are needed as for any
LMMS build. HaikuPorts has a recipe for LMMS (`media-sound/lmms`); the patch
applies on top of it as an ordinary patchset.

## Trying it

1. Start MidiSynth (it is in the `midisynth` package).
2. In LMMS, open an instrument, go to its MIDI tab and turn on **MIDI output**.
   Choose *MidiSynth* from the list that appears.
3. Play a key in the instrument's piano: it sounds in MidiSynth.
4. The other direction works the same way with **MIDI input**.

## Releases

Releases of this fork for Haiku are tagged `haiku-<LMMS version>-<n>`, for
example `haiku-1.3.0-1`. A tag marks the LMMS commit it is based on plus this
patch.

## Status and licence

Made for Haiku by ablyss, kept in this fork only; it is not proposed upstream.
LMMS is GPL-2.0-or-later and so is this addition.
