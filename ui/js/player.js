/* https://stackoverflow.com/questions/3144711/find-the-time-left-in-a-settimeout/36389263#36389263 */
var getTimeout = (function() {
	var _setTimeout = setTimeout,
	map = {};

	setTimeout = function(callback, delay) {
		var id = _setTimeout(callback, delay);
		map[id] = [Date.now(), delay];
		return id;
	};

	return function(id) {
		var m = map[id];
		return m ? Math.max(m[1] - Date.now() + m[0], 0) : NaN;
	};
})();

var player = function(audioFile, containerElem, events) {
	var Player = {};

	var init = function(audioFile, containerElem, events) {
		Player.container = $(containerElem);

		Player.termContainer = $('<div id="term"></div>')
		    .appendTo(Player.container);

		Player.controls = $('<div id="controls"></div>')
		    .appendTo(Player.container);

		Player.term = new Terminal({
			rows: events.height,
			cols: events.width,
			fontFamily: 'courier-new, courier, monospace',
			fontSize: 16,
			cursorStyle: 'block'
		});
		Player.term.open(Player.termContainer[0]);

		var screenEl = Player.term.element ?
		    Player.term.element.querySelector('.xterm-screen') : null;
		Player.termWidth = (screenEl ?
		    Math.ceil(screenEl.getBoundingClientRect().width) : 0) + 'px';

		Player.termContainer.css({
		    margin: '0 auto',
		    width: Player.termWidth
		});

		Player.controls.css({
		    margin: '0 auto',
		    width: Player.termWidth
		});

		/* Copy terminal selection to clipboard when mouse selection completes */
		$(Player.termContainer).on('mouseup', function() {
			if (!Player.term.hasSelection()) {
				return;
			}
			var sel = Player.term.getSelection();
			if (!sel) {
				return;
			}
			if (navigator.clipboard && navigator.clipboard.writeText) {
				navigator.clipboard.writeText(sel).catch(function() {});
			} else if (document.execCommand) {
				try {
					document.execCommand('copy');
				} catch (e) {}
			}
		});

		Player.termEvents = events.stdout;
		Player.eventOff = 0;
		Player.rem = 0;
		Player.timerHandle = undefined;
		Player.seekerHandle = undefined;
		Player.timersRunning = 0;

		Player.playTime = 0;
		Player.seekPos = 0;
		Player.baseSeekPos = 0;

		var startTimers = function() {
			if (Player.timersRunning || Player.paused) {
				return;
			}
			Player.timersRunning = 1;
			Player.start = Date.now();
			Player.baseSeekPos = Player.seekPos;
			Player.timerHandle = setTimeout(Player.nextEvent, Player.rem);
			Player.seekerHandle = setTimeout(Player.updateSeeker, 100);
		};

		var stopTimers = function(saveRemaining) {
			if (Player.seekerHandle) {
				clearTimeout(Player.seekerHandle);
				Player.seekerHandle = undefined;
			}
			if (Player.timerHandle) {
				if (saveRemaining) {
					var rem = getTimeout(Player.timerHandle);
					Player.rem = isNaN(rem) ? 0 : rem;
				}
				clearTimeout(Player.timerHandle);
				Player.timerHandle = undefined;
			}
			Player.timersRunning = 0;
		};

		var finishPlayback = function() {
			if (Player.eventOff < Player.termEvents.length) {
				var str = "";
				while (Player.eventOff < Player.termEvents.length) {
					str += Player.termEvents[Player.eventOff][1];
					Player.pos += Player.termEvents[Player.eventOff][0];
					Player.eventOff++;
				}
				if (str.length > 0) {
					Player.term.write(str);
				}
			}

			stopTimers(false);

			Player.seekUpdate = 1;
			Player.seeker.val(Player.duration).change();
			Player.seekUpdate = 0;

			Player.ended = 1;
			Player.paused = 1;
			Player.rem = 0;
			Player.eventOff = 0;
			Player.pos = 0;
			Player.seekPos = 0;
			Player.baseSeekPos = 0;
			Player.toggle.html('<i class="material-icons">&#xE042;</i>');
		};

		Player.seekTo = function(t) {
			var back = Player.eventOff &&
			    Player.pos + Player.termEvents[Player.eventOff - 1][0] > (t / 1000);
			var prefix = "";

			if (back) {
				/* Send RIS (ESC c) in-band so WriteBuffer resets in FIFO order */
				prefix = "\x1bc";
				Player.pos = 0;
			}

			var i = back ? 0 : Player.eventOff;
			var str = "";
			while (i < Player.termEvents.length &&
			    Player.pos + Player.termEvents[i][0] <= (t / 1000)) {
				str += Player.termEvents[i][1];
				Player.pos += Player.termEvents[i++][0];
			}

			i = i == Player.termEvents.length ? i - 1 : i;

			Player.term.write(prefix + str);
			Player.eventOff = i;

			return Player.termEvents[i][0] - t;
		};

		Player.behind = function() {
			return Player.pos + Player.termEvents[Player.eventOff][0] <
			    Player.audio.currentTime;
		};

		Player.nextEvent = function() {
			Player.timerHandle = undefined;
			Player.rem = 0;
			if (Player.paused || Player.eventOff >= Player.termEvents.length) {
				return;
			}

			var str = "";
			if (audioFile) {
				while (Player.eventOff < Player.termEvents.length &&
				    Player.behind()) {
					str += Player.termEvents[Player.eventOff][1];
					Player.pos += Player.termEvents[Player.eventOff][0];
					Player.eventOff++;
				}
			} else {
				str = Player.termEvents[Player.eventOff][1];
				Player.pos += Player.termEvents[Player.eventOff++][0];
			}

			if (str.length > 0) {
				Player.term.write(str);
			}

			if (Player.eventOff < Player.termEvents.length) {
				var delay;
				if (audioFile) {
					delay = ((Player.pos + Player.termEvents[Player.eventOff][0]) -
					    Player.audio.currentTime) * 1000;
				} else {
					delay = Player.termEvents[Player.eventOff][0] * 1000;
				}
				Player.timerHandle = setTimeout(Player.nextEvent, Math.max(delay, 0));
			}
		};

		Player.updateSeeker = function() {
			Player.seekerHandle = undefined;
			if (Player.paused) {
				return;
			}

			Player.seekUpdate = 1;
			if (audioFile) {
				var curMs = Player.audio.currentTime * 1000;
				Player.seeker.val(Math.min(curMs, Player.duration)).change();
			} else {
				Player.seekPos = Math.min(
				    Player.baseSeekPos + (Date.now() - Player.start),
				    Player.duration);
				Player.seeker.val(Player.seekPos).change();
			}
			Player.seekUpdate = 0;

			if (!audioFile && Player.seekPos >= Player.duration &&
			    Player.eventOff >= Player.termEvents.length) {
				finishPlayback();
				return;
			}

			Player.seekerHandle = setTimeout(Player.updateSeeker, 100);
		};

		if (audioFile) {
			Player.toggle = $('<button id="playToggle" disabled><i class="material-icons">&#xE88B;</i></button>')
			    .appendTo(Player.controls);
		} else {
			Player.toggle = $('<button id="playToggle"><i class="material-icons">&#xE037;</i></button>')
			    .appendTo(Player.controls);
			Player.startable = 1;
		}

		Player.paused = 1;
		Player.ended = 0;
		Player.pos = 0;
		Player.toggle.click(function() {
			if (Player.startable) {
				if (Player.ended) {
					Player.term.write("\x1bc");
					Player.seekUpdate = 1;
					Player.seeker.val(0).change();
					Player.seekUpdate = 0;
					Player.ended = 0;
					Player.paused = 1;
					Player.restarted = 1;
					Player.pos = 0;
					Player.eventOff = 0;
					Player.rem = 0;
					Player.seekPos = 0;
					Player.baseSeekPos = 0;
				}

				if (Player.paused) {
					Player.paused = 0;
					Player.toggle.html('<i class="material-icons">&#xE034;</i>');

					if (audioFile) {
						Player.audio.play();
					}

					startTimers();
				} else {
					if (audioFile) {
						Player.audio.pause();
					} else if (Player.timersRunning) {
						Player.seekPos = Math.min(
						    Player.baseSeekPos + (Date.now() - Player.start),
						    Player.duration);
					}

					stopTimers(true);
					Player.toggle.html('<i class="material-icons">&#xE037;</i>');
					Player.paused = 1;
				}
			}
		});

		Player.duration = events.duration * 1000;
		var step = Player.duration > 0 ? Player.duration / 1000 : 1;
		Player.seeker = $('<input type="range" min="0" max="' +
		    Player.duration + '" step="' + step +
		    '" value="0">').appendTo(Player.controls);

		Player.maxSeek = 0;
		Player.seeking = 0;
		Player.seekUnpause = 0;
		Player.seeker.rangeslider({
			polyfill : false,

			onSlide: function(pos, val) {
				if (Player.seekUpdate) {
					return;
				}

				if (Player.seeking == 0) {
					Player.seeking = 1;
					if (val > Player.maxSeek) {
						Player.seeker.val(val).change();
					}
					if (Player.paused == 0) {
						Player.toggle.trigger('click');
					}
				}
			},

			onSlideEnd: function(pos, val) {
				if (Player.seekUpdate) {
					return;
				}

				if (val > Player.maxSeek) {
					Player.seeker.val(val).change();
				}

				stopTimers(false);
				Player.rem = Player.seekTo(val);
				if (audioFile) {
					Player.audio.currentTime = val / 1000;
				} else {
					Player.seekPos = val;
					Player.baseSeekPos = val;
				}
				Player.seeking = 0;
			}
		});

		if (audioFile) {
			Player.audio = new Audio(audioFile);
			$(Player.audio).on('ended', function () {
				finishPlayback();
			});

			$(Player.audio).on('durationchange', function() {
				Player.maxSeek = Player.audio.duration * 1000;
			});

			$(Player.audio).on('canplaythrough', function() {
				Player.startable = 1;
				if (!Player.restarted) {
					Player.toggle.html('<i class="material-icons">&#xE037;</i>');
				} else {
					Player.restarted = 0;
				}
				Player.toggle.prop('disabled', false);
			});

			Player.audio.load();
		}

		return Player;
	};

	return init(audioFile, containerElem, events);
};
