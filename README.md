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

It depends on [PortAudio](http://portaudio.com/) (for recording audio) and
[Concurrency Kit](http://concurrencykit.org/).

There are no UI build dependencies because I find that idea a little silly.

### Make

To build, simply run `make` once dependencies have been installed. If they were
installed to a location other than `/usr/local/`, you will have to edit the
`Makefile`. 

## Usage

To use:

    % castty

or

    % castty -r audio.raw

or

    % castty -r audio.raw termevents.js

CasTTY chooses the system's default audio input device. If you wish to use a
different device, set it to the default. outputs 16-bit LPCM audio at 44.1kHz.
Utilities like [sox](http://sox.sourceforge.net/) may be used to convert the
audio into more useful formats for web publication.

    % sox -D -r 44100 -e signed -b 16 -c 2 -L audio.raw audio.wav

You may of course wish to compress further. There are numerous utilities to
help here, but that's a bit out of scope.

By default, castty does _not_ record audio and sends its terminal event output
to a file called `events.js`.

### Runtime Commands

CasTTY contains a runtime command interface. Commands are entered by first
pressing `^a` and then typing the command. All commands begin with a colon. To
send a literal `^a` to your shell, you can either type `^a^a` or `^aa` (in
case you're using screen or emacs or tmux with screen keybindings). Currently
supported commands include:

 * `:mute`: A toggle to mute or unmute audio input. This literally writes empty
   audio to the output; it does not stop recording.

 * `:pause`: A toggle to pause or unpause recording entirely. This stops audio
   recording and recording events in the shell. You may continue to use the
   shell until you unpause, but if you unpause with the terminal in a
   different state, things will probably get wacky.

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
