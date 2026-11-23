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
	var ICON_PLAY = '<i class="material-icons" aria-hidden="true">&#xE037;</i>';
	var ICON_PAUSE = '<i class="material-icons" aria-hidden="true">&#xE034;</i>';
	var ICON_REPLAY = '<i class="material-icons" aria-hidden="true">&#xE042;</i>';
	var ICON_LOADING = '<i class="material-icons" aria-hidden="true">&#xE88B;</i>';

	var setToggleIcon = function(icon, label) {
		Player.toggle.html(icon).attr('aria-label', label);
	};

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
		Player.pendingSeek = null;

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
			setToggleIcon(ICON_REPLAY, 'Replay');
		};

		Player.seekTo = function(t) {
			var targetSec = t / 1000;
			var back = Player.ended || targetSec < Player.pos;
			var prefix = "";

			if (back) {
				/* Send RIS (ESC c) in-band so WriteBuffer resets in FIFO order */
				prefix = "\x1bc";
				Player.pos = 0;
				Player.eventOff = 0;
				Player.ended = 0;
			}

			var i = Player.eventOff;
			var str = "";
			while (i < Player.termEvents.length &&
			    Player.pos + Player.termEvents[i][0] <= targetSec) {
				str += Player.termEvents[i][1];
				Player.pos += Player.termEvents[i++][0];
			}

			if (prefix.length > 0 || str.length > 0) {
				Player.term.write(prefix + str);
			}
			Player.eventOff = i;

			if (i < Player.termEvents.length) {
				return Math.max(((Player.pos + Player.termEvents[i][0]) - targetSec) * 1000, 0);
			}
			return 0;
		};

		Player.behind = function() {
			return Player.pos + Player.termEvents[Player.eventOff][0] <=
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

		/*
		 * Enable Play immediately rather than waiting for canplaythrough:
		 * mobile browsers defer audio buffering until audio.play() is called
		 * synchronously inside a direct user tap handler.
		 */
		Player.toggle = $('<button id="playToggle" type="button" aria-label="Play">' +
		    ICON_PLAY + '</button>').appendTo(Player.controls);
		Player.startable = 1;

		Player.paused = 1;
		Player.ended = 0;
		Player.pos = 0;
		Player.toggle.click(function() {
			if (!Player.startable) {
				return;
			}

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
				if (audioFile && Player.audio.readyState > 0) {
					try {
						Player.audio.currentTime = 0;
					} catch (e) {}
				}
			}

			if (Player.paused) {
				Player.paused = 0;

				if (audioFile) {
					if (Player.audio.readyState < 3) {
						setToggleIcon(ICON_LOADING, 'Loading');
					} else {
						setToggleIcon(ICON_PAUSE, 'Pause');
					}

					var playPromise = Player.audio.play();
					if (!Player.audio.paused && Player.audio.readyState >= 3) {
						startTimers();
					}

					if (playPromise && typeof playPromise.then === 'function') {
						playPromise.then(function() {
							if (!Player.paused) {
								setToggleIcon(ICON_PAUSE, 'Pause');
								startTimers();
							}
						}, function() {
							if (!Player.paused) {
								stopTimers(true);
								Player.paused = 1;
								setToggleIcon(ICON_PLAY, 'Play');
							}
						});
					}
				} else {
					setToggleIcon(ICON_PAUSE, 'Pause');
					startTimers();
				}
			} else {
				if (audioFile) {
					Player.audio.pause();
				} else if (Player.timersRunning) {
					Player.seekPos = Math.min(
					    Player.baseSeekPos + (Date.now() - Player.start),
					    Player.duration);
				}

				stopTimers(true);
				setToggleIcon(ICON_PLAY, 'Play');
				Player.paused = 1;
			}
		});

		Player.duration = events.duration * 1000;
		var step = Player.duration > 0 ? Player.duration / 1000 : 1;
		Player.seeker = $('<input type="range" min="0" max="' +
		    Player.duration + '" step="' + step +
		    '" value="0">').appendTo(Player.controls);

		Player.maxSeek = Player.duration;
		Player.seeking = 0;
		Player.seekUnpause = 0;
		Player.seeker.rangeslider({
			polyfill: false,

			onSlide: function(pos, val) {
				if (Player.seekUpdate) {
					return;
				}

				if (Player.maxSeek > 0 && val > Player.maxSeek) {
					val = Player.maxSeek;
					Player.seekUpdate = 1;
					Player.seeker.val(val).change();
					Player.seekUpdate = 0;
				}

				if (Player.seeking == 0) {
					Player.seeking = 1;
					if (Player.paused == 0) {
						Player.seekUnpause = 1;
						Player.toggle.trigger('click');
					} else {
						Player.seekUnpause = 0;
					}
				}
			},

			onSlideEnd: function(pos, val) {
				if (Player.seekUpdate) {
					return;
				}

				if (Player.maxSeek > 0 && val > Player.maxSeek) {
					val = Player.maxSeek;
					Player.seekUpdate = 1;
					Player.seeker.val(val).change();
					Player.seekUpdate = 0;
				}

				var wasEnded = Player.ended;
				stopTimers(false);
				Player.rem = Player.seekTo(val);
				if (audioFile) {
					if (Player.audio.readyState > 0) {
						try {
							Player.audio.currentTime = val / 1000;
							Player.pendingSeek = null;
						} catch (e) {
							Player.pendingSeek = val / 1000;
						}
					} else {
						Player.pendingSeek = val / 1000;
					}
				} else {
					Player.seekPos = val;
					Player.baseSeekPos = val;
				}
				if (wasEnded && !Player.ended && Player.paused) {
					setToggleIcon(ICON_PLAY, 'Play');
				}
				Player.seeking = 0;
				if (Player.seekUnpause) {
					Player.seekUnpause = 0;
					if (Player.paused) {
						Player.toggle.trigger('click');
					}
				}
			}
		});

		if (audioFile) {
			Player.audio = new Audio(audioFile);
			Player.audio.preload = 'auto';
			Player.audio.setAttribute('playsinline', '');

			$(Player.audio).on('loadedmetadata durationchange', function() {
				if (isFinite(Player.audio.duration) && Player.audio.duration > 0) {
					Player.maxSeek = Player.audio.duration * 1000;
				}
				if (Player.pendingSeek !== null && Player.audio.readyState > 0) {
					try {
						Player.audio.currentTime = Player.pendingSeek;
						Player.pendingSeek = null;
					} catch (e) {}
				}
			});

			$(Player.audio).on('playing', function() {
				if (!Player.paused) {
					if (Player.pendingSeek !== null && Player.audio.readyState > 0) {
						try {
							Player.audio.currentTime = Player.pendingSeek;
							Player.pendingSeek = null;
						} catch (e) {}
					}
					setToggleIcon(ICON_PAUSE, 'Pause');
					startTimers();
				}
			});

			$(Player.audio).on('waiting', function() {
				if (!Player.paused) {
					stopTimers(true);
					setToggleIcon(ICON_LOADING, 'Loading');
				}
			});

			$(Player.audio).on('error', function() {
				if (!Player.paused) {
					stopTimers(true);
					Player.paused = 1;
					setToggleIcon(ICON_PLAY, 'Play');
				}
			});

			$(Player.audio).on('ended', function() {
				finishPlayback();
			});
		}

		return Player;
	};

	return init(audioFile, containerElem, events);
};
