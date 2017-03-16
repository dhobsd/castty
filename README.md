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

CasTTY also depends on [LAME](http://lame.sourceforge.net/) (and in particular,
libmp3lame) for on-the-fly MP3 encoding.

There are no UI build dependencies because I find that idea a little silly.

### Make

To build, simply run `make` once dependencies have been installed. If they were
installed to a location other than `/usr/local/`, you will have to edit the
`Makefile`. 

## Usage

    usage: castty [-acdelrt] [out.json]
     -a <outfile>   Output audio to <outfile>. Must be specified with -d.
     -c <cols>      Use <cols> columns in the recorded shell session.
     -D <outfile>   Send debugging information into <outfile>
     -d <device>    Use audio device <device> for input.
     -e <cmd>       Execute <cmd> from the recorded shell session.
     -l             List available audio input devices and exit.
     -m             Encode audio to mp3 before writing.
     -r <rows>      Use <rows> rows in the recorded shell session.
     -t <title>     Title of the cast.
    
     [out.json]     Optional output filename of recorded events. If not specified,
                    a file "events.json" will be created.

To list usable input devices for recording, just run `castty -l`. Output will
look something like this:

    Available input devices:
       0: Built-in Microphone 44100Hz
          castty -d 'AppleHDAEngineInput:1B,0,1,0:1' -a audio.f32le

The `-d 'AppleHDAEngineInput:1B,0,1,0:1'` argument can be pasted directly to
CasTTY to choose that device for recording. The audio format and sample rate
CasTTY will use is also provided.

CasTTY supports MP3 output by default, but other encodings may be desirable.
Without the `-m` flag, CasTTY outputs interleaved PCM audio. (CasTTY upgrades
mono audio to stereo.)

Utilities like [sox](http://sox.sourceforge.net/) may be used to convert the
audio into more useful formats for web publication.

    % sox -D -r 44100 -e signed -b 16 -c 2 -L audio.raw audio.wav

By default, CasTTY does _not_ record audio and sends its terminal event output
to a file called `events.js`.

### Runtime Commands

CasTTY contains a runtime command interface. Commands are entered with the
sequence `^a` (`C-a`, `Control-a`), followed by the command character.
Currently supported commands are:

 * `^a`: Send a literal `^a` to the recorded session.
 * `a`: Send a literal `^a` to the recorded session.
 * `m`: Mute or unmute the recording. Recording will continue, but without any
   audio until unmuted.
 * `p`: Pause or unpause the recording. Neither terminal nor audio will be
   recorded during the paused period. When unpausing, CasTTY requests the
   screen to be redrawn. This may cause your terminal buffer to clear.

### Miscellaneous

CasTTY does support window resizing. However, because the size of the player
is automatically calculated based on the size of the original window, the
recorded window size can only ever be as large or smaller than the original
window size.

CasTTY supports UTF-8 input.

CasTTY outputs in
[asciicast v1](https://github.com/asciinema/asciinema/blob/master/doc/asciicast-v1.md)
format. Its output files should be compatible with the asciinema player
(though that player does not support audio).

## Web Interface

The `ui` directory of the repository is a self-contained implementation of a
CasTTY player. Utilities used include:

 * [jQuery](https://jquery.com/)
 * [rangeslider.js](https://github.com/andreruffert/rangeslider.js)
 * [xterm.js](https://github.com/sourcelair/xterm.js)

Licenses for all these utilities can be found in their respective repositories
and in the LICENSE file in this repository.

To create a cast, simply modify `ui/index.html` to point to the correct audio
file and `events.js` output from `castty`.
