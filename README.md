CasTTY is a fork of [ttyrec](http://0xcc.net/ttyrec/) that makes it easy to
create terminal-based screencasts for publication on webpages. It depends
on [PortAudio](http://portaudio.com/) (for recording audio) and
[Concurrency Kit](http://concurrencykit.org/). These dependencies are assumed
to be installed in `/usr/local`. If they live elsewhere, edit the Makefile.

This software isn't bug-free, and the bugs haven't annoyed me enough yet to
fix. If you'd like to contribute, see the `TODO.`

[You probably want to see what you get after you use this.](https://9vx.org/~dho/term/index.html)

To build, simply run `make`. 

To use:

    % castty -r audio.raw

CasTTY outputs 16-bit LPCM audio at 44.1kHz. Utilities like
[sox](http://sox.sourceforge.net/) may be used to convert the audio into more
useful formats for web publication.

    % sox -D -r 44100 -e signed -b 16 -c 2 -L audio.raw audio.wav

You may of course wish to compress further. There are numerous utilities to
help here.

Apart from audio, CasTTY outputs a Javascript file containing an array of
events to play back in the browser. By default, this file is called
`events.js`, but its name can be changed by calling e.g.
`castty -r audio.raw myevents.js`.

The `ui` directory of the repository is a self-contained implementation of a
CasTTY player. Utilities used include:

 * [jQuery](https://jquery.com/)
 * [rangeslider.js](https://github.com/andreruffert/rangeslider.js)
 * [xterm.js](https://github.com/sourcelair/xterm.js)

Licenses for all these utilities can be found in their respective repositories
and in the LICENSE file in this repository.

To create a cast, simply modify `ui/index.html` to point to the correct audio
file and `events.js` output from `castty`.
