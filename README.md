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

There are no UI build dependencies because I find that idea a little silly.

### Make

To build, simply run `make` once dependencies have been installed. If they were
installed to a location other than `/usr/local/`, you will have to edit the
`Makefile`. 

## Usage

    usage: castty [-adlcre] [out.js]
     -a <outfile>   Output audio to <outfile>. Must be specified with -d.
     -c <cols>      Use <cols> columns in the recorded shell session.
     -d <device>    Use audio device <device> for input.
     -e <cmd>       Execute <cmd> from the recorded shell session.
     -l             List available audio input devices and exit.
     -r <rows>      Use <rows> rows in the recorded shell session.
    
     [out.js]       Optional output filename of recorded events. If not specified,
                    a file "events.js" will be created.

To list usable input devices for recording, just run `castty -l`. Output will
look something like this:

    Available input devices:
       0: Built-in Microphone 44100Hz
          castty -d 'AppleHDAEngineInput:1B,0,1,0:1' -a audio.f32le

The `-d 'AppleHDAEngineInput:1B,0,1,0:1'` argument can be pasted directly to
castty to choose that device for recording. The audio format and sample rate
castty will use is also provided.

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
