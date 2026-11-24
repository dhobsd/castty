# CasTTY

CasTTY is a program similar to [ttyrec](http://0xcc.net/ttyrec/) and
[asciinema](https://github.com/asciinema/asciinema) that makes it easy to
create audio-enabled, terminal-based screencasts for publication on webpages.
It was originally a fork of `ttyrec`, but has since effectively been
rewritten from scratch.

There may be bugs. Feel free to contribute patches or file an issue!

[You probably want to see what you get after you use this.](https://9vx.org/~dho/term/index.html)

## Compatibility

I've tested CasTTY on macOS Sierra and Linux. It should work on other systems,
but if not, please feel free to send a patch. It probably can't record if you
don't have a microphone, unless you have some weird audio mixing setup.

## Building

### Dependencies 

It depends on [libsoundio](http://libsound.io/). Ubuntu 16.04 seems to ship a
broken version of `libsoundio` that depends on a version of JACK that doesn't
work. This is fixed in the [libsoundio repo](https://github.com/andrewrk/libsoundio),
but you will have to build it yourself.

CasTTY also depends on [LAME](http://lame.sourceforge.net/) (`libmp3lame`) for
on-the-fly MP3 encoding and decoding,
[libsndfile](https://libsndfile.github.io/libsndfile/) for WAV, FLAC, Ogg Opus,
and Ogg Vorbis audio support, [FFmpeg](https://ffmpeg.org/) (`libavformat`,
`libavcodec`, `libavutil`) for `.m4a` (AAC) encoding and decoding, and
[libvterm](https://www.leonerd.org.uk/code/libvterm/) for live terminal
compositing in the recording HUD as well as clean terminal scrubbing and cut
snapshots in `castty edit`.

There are no UI build dependencies because I find that idea a little silly.

### Make

To build, simply run `make` once dependencies have been installed. If they were
installed to a location other than `/usr/local/`, you will have to edit the
`Makefile`. 

## Usage

    usage: castty record [-123AacDdFHehlmNprtR] [out.cast]
     -1             Output asciicast v1 format.
     -2             Output asciicast v2 format.
     -3             Output asciicast v3 format (default).
     -A             Append to an existing recording instead of overwriting.
     -a <outfile>   Output audio to <outfile>. Must be specified with -d.
     -c <cols>      Use <cols> columns in the recorded shell session.
     -D <outfile>   Send debugging information into <outfile>.
     -d <device>    Use audio device <device> for input.
     -e <cmd>       Execute <cmd> from the recorded shell session.
     -F <format>    Audio output format: raw, mp3, wav, flac, opus, ogg, m4a.
     -H <mode>      Recording HUD layout: auto, side, bottom, off.
     -h             Show this help.
     -l             List available audio input devices and exit.
     -m             Encode audio to mp3 before writing (shorthand for -F mp3).
     -N             Disable terminal color/version probing.
     -p             Begin the recording in paused mode.
     -r <rows>      Use <rows> rows in the recorded shell session.
     -R             Use a raw sound device.
     -t <title>     Title of the cast.
    
     [out.cast]     Optional output filename of recorded events. If not specified,
                    a file "events.cast" will be created.

To list usable input devices for recording, just run `castty record -l`. Output will
look something like this:

    Available input devices:
       0: Built-in Microphone 44100Hz
          castty -d 'AppleHDAEngineInput:1B,0,1,0:1' -a audio.raw

The `-d 'AppleHDAEngineInput:1B,0,1,0:1'` argument can be pasted directly to
CasTTY to choose that device for recording. The audio format and sample rate
CasTTY will use is also provided.

CasTTY encodes audio directly to MP3 (`-m` / `-F mp3` / `.mp3`), WAV (`-F wav`
/ `.wav`), FLAC (`-F flac` / `.flac`), Ogg Opus (`-F opus` / `.opus`), Ogg
Vorbis (`-F ogg` / `.ogg`), or AAC in MP4/M4A (`-F m4a` / `.m4a`), inferred from
the `-a` filename extension unless overridden by `-F` or `-m`. Otherwise, CasTTY
outputs raw interleaved PCM audio (duplicating mono inputs to stereo for raw PCM;
writing 1-channel mono for encoded formats on mono devices unless appending to an
existing stereo file).

Utilities like [sox](http://sox.sourceforge.net/) may be used to convert the
audio into more useful formats for web publication.

    % sox -D -r 44100 -e signed -b 16 -c 2 -L audio.raw audio.wav

By default, CasTTY does _not_ record audio and sends its terminal event output
to a file called `events.cast`.

### Runtime Commands

CasTTY contains a runtime command interface. Commands are entered with the
sequence `^a` (`C-a`, `Control-a`), followed by the command character.
Currently supported commands are:

 * `^a`: Send a literal `^a` to the recorded session.
 * `a`: Send a literal `^a` to the recorded session.
 * `m`: Mute or unmute the recording. Recording will continue, but without any
   audio until unmuted.
 * `k`: Drop a timeline chapter marker (`"m"` event) at the current timestamp.
 * `K`: Prompt in the HUD for a chapter title and drop a named `"m"` marker
   (`Enter` to save, `Esc` to cancel) without sending input to the shell.
 * `h`: Toggle the recording HUD (timer, timeline strip, markers, and live VU
   meter) on or off when running interactively (`-H auto`/`side`/`bottom`).
 * `p`: Pause or unpause the recording. Neither terminal nor audio will be
   recorded during the paused period. When unpausing, CasTTY requests the
   screen to be redrawn. This may cause your terminal buffer to clear.

### Interactive Editor (`castty edit`)

    usage: castty edit [-a <audio>] [-d <in_dev>] [-F <format>] [-l] [-o <out_dev>] [-R] <file.cast>
     -a <audio>     Accompanying audio file to edit/save alongside the cast.
     -d <in_dev>    Audio input device ID for voiceover/punch-in dubbing.
     -F <format>    Audio format: raw, mp3, wav, flac, opus, ogg, m4a.
     -h             Show this help.
     -l             List available audio input and output devices and exit.
     -o <out_dev>   Audio output device ID for playback.
     -R             Use raw audio devices.

`castty edit` loads asciicast v1, v2, and v3 files together with optional audio
for synchronized playback, timeline/audio editing, and live voiceover dubbing
inside the terminal:

 * **Navigation & Playback**: `Space` play/pause, `h`/`l` (`Left`/`Right`)
   scrub ±0.5s, `H`/`L` (`[`, `]`, `PgUp`/`PgDn`) scrub ±5s, `,`/`.` step by
   terminal event, `0`/`$` jump to start/end, `g` (`:`) jump to timestamp,
   `/` search terminal output/markers, `n`/`p` jump between markers, `+`/`-`
   change playback speed, `M` toggle monitor mute, `Up`/`Down` pan viewport,
   `Tab` toggle the side HUD.
 * **Timeline & Audio Edits**: `i`/`o` mark In/Out selection (`c` or `Esc`
   clears), `x` cut `[In, Out)` with synthesized terminal state continuity and
   10 ms half-cosine audio crossfade, `m` mute audio selection, `s` compress idle
   gaps, `P` insert pause/silence at playhead, `(`/`)` nudge A/V sync ±50 ms,
   `k` add unnamed marker, `K` add or rename marker at playhead, `d` delete
   marker near playhead, `R` redact matching text, `T` edit title.
 * **Voiceover & Pause-to-Extend Dubbing**: `V` start replace voiceover or `O`
   start overdub mix (ducks background track to 25%). While dubbing, press
   `Space` to freeze the terminal frame and continuously extend pause duration
   while narration continues, press `.` to pull the next terminal event to the
   playhead (step-dubbing), `Enter` (`V`/`O`) to commit, or `Esc` to cancel.
 * **History & Saving**: `u` undo (`Ctrl-Z`), `U` redo (`Ctrl-R`), `w` save
   atomically, `W` save/export audio to a new path or format, `q` quit (with
   dirty-check prompt; `Q` quits unconditionally), `?` toggle keybinding help.

### Miscellaneous

CasTTY does support window resizing. However, because the size of the player
is automatically calculated based on the size of the original window, the
recorded window size can only ever be as large or smaller than the original
window size.

CasTTY supports UTF-8 input.

When recording v2 or v3 casts on a TTY, CasTTY queries the outer terminal
over the TTY (`XTVERSION`, `OSC 10`/`11`/`4`) to record its version and color
palette in the header (disable with `-N`). The web player applies recorded
colors automatically, falls back to built-in palettes by `$TERM`/`COLORFGBG`,
and honors `prefers-contrast: more` alongside its theme switcher.

CasTTY outputs in
[asciicast v3](https://docs.asciinema.org/manual/asciicast/v3/)
format by default, and also supports
[asciicast v1](https://github.com/asciinema/asciinema/blob/master/doc/asciicast-v1.md)
(`-1`) and
[asciicast v2](https://github.com/asciinema/asciinema/blob/master/doc/asciicast-v2.md)
(`-2`). Its output files should be compatible with the asciinema player
(though that player does not support audio).

You can upgrade older asciicast v1/v2 recordings or legacy CasTTY `_tre`/`_ti`
files to asciicast v3 with `tools/castty-upgrade`:

    % ./tools/castty-upgrade old.cast -o upgraded.cast

## Web Interface

The `ui` directory of the repository is a self-contained implementation of a
CasTTY player. Third-party dependencies are stored unmodified in `ui/vendor/`
and bundled into `ui/css/min.css` and `ui/js/min.js` via `make -C ui` (using
`make -C ui fetch-vendor` to re-fetch and verify upstream archives):

 * [jQuery](https://jquery.com/) (`3.7.1`)
 * [rangeslider.js](https://github.com/andreruffert/rangeslider.js) (`2.3.3`)
 * [@xterm/xterm](https://github.com/xtermjs/xterm.js) (`6.0.0`)

CasTTY's own styles and player code live unminified in `ui/css/player.css` and
`ui/js/player.js`. Asciicast v2 and v3 `"m"` marker events recorded via `^a k`
or `^a K` render automatically in the web player as scrubber chapter ticks,
hover previews, a chapters menu, and `[` / `]` previous/next chapter shortcuts
(and are also compatible with the asciinema player).

Licenses for all these utilities can be found in their respective repositories
and in the LICENSE file in this repository.

To create a cast, simply modify `ui/index.html` to point to the correct audio
file and `events.cast` output from `castty`.
