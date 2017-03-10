# CasTTY To Do Items

## Misc

Convert these TODO items to issues in the GH repo.

## Bugs

I stripped down ttyrec heavily to make CasTTY, and have broken some terminal
handling stuff. That needs fixing.

Appending to a recording doesn't really work (audio is fine, but the JS output
breaks).

There are probably better ways to handle seeking in the web UI.

The web UI is fugly.

The web UI probably doesn't work well on mobile devices.

## Features

Pause recording. This would ignore any events on the TTY and stop recording
audio until recording was resumed.

Input volume support.

More robust input setup (verify levels, etc).

Transcode LPCM to multiple output formats with widest browser coverage either
on exit or through another tool.

Curses-style control UI with `screen` or `tmux` style keybindings.

Editing functionality (simple cut, move, sync)

Add livecasting functionality to the player and recorder. (This will almost
definitely require some kind of real-time transcoding support.)
