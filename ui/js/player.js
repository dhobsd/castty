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

var player = function(audioFile, containerElem, events, termInfoOrOpts, maybeOpts) {
	var Player = {};
	var ICON_PLAY = '<i class="material-icons" aria-hidden="true">&#xE037;</i>';
	var ICON_PAUSE = '<i class="material-icons" aria-hidden="true">&#xE034;</i>';
	var ICON_REPLAY = '<i class="material-icons" aria-hidden="true">&#xE042;</i>';
	var ICON_LOADING = '<i class="material-icons" aria-hidden="true">&#xE88B;</i>';
	var ICON_VOLUME = '<i class="material-icons" aria-hidden="true">&#xE050;</i>';
	var ICON_MUTED = '<i class="material-icons" aria-hidden="true">&#xE04F;</i>';
	var ICON_THEME = '<i class="material-icons" aria-hidden="true">&#xE891;</i>';
	var ICON_FULLSCREEN = '<i class="material-icons" aria-hidden="true">&#xE5D0;</i>';
	var ICON_FULLSCREEN_EXIT = '<i class="material-icons" aria-hidden="true">&#xE5D1;</i>';

	var THEME_ORDER = ['default', 'high-contrast', 'light', 'vga'];
	var THEMES = {
		'default': {
			label: 'Default Dark',
			minContrast: 1,
			colors: {
				background: '#000000',
				foreground: '#ffffff',
				cursor: '#ffffff',
				cursorAccent: '#000000'
			}
		},
		'high-contrast': {
			label: 'High Contrast',
			minContrast: 7,
			colors: {
				background: '#000000',
				foreground: '#ffffff',
				cursor: '#00ff66',
				cursorAccent: '#000000',
				selectionBackground: 'rgba(255, 255, 0, 0.4)',
				black: '#000000',
				red: '#ff5555',
				green: '#55ff55',
				yellow: '#ffff55',
				blue: '#66b3ff',
				magenta: '#ff66ff',
				cyan: '#55ffff',
				white: '#ffffff',
				brightBlack: '#b3b3b3',
				brightRed: '#ff8080',
				brightGreen: '#80ff80',
				brightYellow: '#ffff80',
				brightBlue: '#99ccff',
				brightMagenta: '#ff99ff',
				brightCyan: '#80ffff',
				brightWhite: '#ffffff'
			}
		},
		'light': {
			label: 'High Contrast Light',
			minContrast: 4.5,
			colors: {
				background: '#ffffff',
				foreground: '#111111',
				cursor: '#111111',
				cursorAccent: '#ffffff',
				selectionBackground: 'rgba(0, 102, 204, 0.25)',
				black: '#111111',
				red: '#b91c1c',
				green: '#15803d',
				yellow: '#854d0e',
				blue: '#1d4ed8',
				magenta: '#7e22ce',
				cyan: '#0e7490',
				white: '#525252',
				brightBlack: '#404040',
				brightRed: '#991b1b',
				brightGreen: '#166534',
				brightYellow: '#713f12',
				brightBlue: '#1e40af',
				brightMagenta: '#6b21a8',
				brightCyan: '#155e75',
				brightWhite: '#111111'
			}
		},
		'vga': {
			label: 'Classic VGA (16-color)',
			minContrast: 1,
			colors: {
				background: '#000000',
				foreground: '#aaaaaa',
				cursor: '#aaaaaa',
				cursorAccent: '#000000',
				black: '#000000',
				red: '#aa0000',
				green: '#00aa00',
				yellow: '#aa5500',
				blue: '#0000aa',
				magenta: '#aa00aa',
				cyan: '#00aaaa',
				white: '#aaaaaa',
				brightBlack: '#555555',
				brightRed: '#ff5555',
				brightGreen: '#55ff55',
				brightYellow: '#ffff55',
				brightBlue: '#5555ff',
				brightMagenta: '#ff55ff',
				brightCyan: '#55ffff',
				brightWhite: '#ffffff'
			}
		}
	};

	var formatTime = function(ms) {
		var totalSec = Math.max(Math.floor((ms || 0) / 1000), 0);
		var hrs = Math.floor(totalSec / 3600);
		var mins = Math.floor((totalSec % 3600) / 60);
		var secs = totalSec % 60;
		var secStr = secs < 10 ? '0' + secs : String(secs);
		if (hrs > 0) {
			var minStr = mins < 10 ? '0' + mins : String(mins);
			return hrs + ':' + minStr + ':' + secStr;
		}
		return mins + ':' + secStr;
	};

	var setToggleIcon = function(icon, label) {
		Player.toggle.html(icon).attr('aria-label', label);
	};

	var ANSI_COLOR_NAMES = [
		'black', 'red', 'green', 'yellow',
		'blue', 'magenta', 'cyan', 'white',
		'brightBlack', 'brightRed', 'brightGreen', 'brightYellow',
		'brightBlue', 'brightMagenta', 'brightCyan', 'brightWhite'
	];

	var parseCastTheme = function(rawTheme) {
		if (!rawTheme || typeof rawTheme !== 'object') {
			return null;
		}
		var colors = {};
		if (rawTheme.fg) {
			colors.foreground = rawTheme.fg;
			colors.cursor = rawTheme.fg;
		}
		if (rawTheme.bg) {
			colors.background = rawTheme.bg;
			colors.cursorAccent = rawTheme.bg;
		}
		if (typeof rawTheme.palette === 'string' && rawTheme.palette) {
			var parts = rawTheme.palette.split(':');
			for (var i = 0; i < parts.length && i < ANSI_COLOR_NAMES.length; i++) {
				if (parts[i]) {
					colors[ANSI_COLOR_NAMES[i]] = parts[i];
				}
			}
		}
		if ($.isEmptyObject(colors)) {
			return null;
		}
		return {
			label: 'Recorded',
			minContrast: 1,
			colors: colors
		};
	};

	/* Normalize asciicast v1, v2/v3 (NDJSON), and legacy ttyrec (_tre/_ti) */
	var normalizeEvents = function(rawEvents, info, hasAudio) {
		if (typeof rawEvents === 'string') {
			var lines = rawEvents.split(/\r?\n/);
			var header = null;
			var normEvents = [];
			var markers = [];
			var prevSec = 0;
			var totalSec = 0;
			var pendingDelta = 0;
			var idleLimit = 0;
			var isV3 = false;
			var hasResize = false;

			for (var i = 0; i < lines.length; i++) {
				var line = $.trim(lines[i]);
				if (!line || line.charAt(0) === '#') {
					continue;
				}
				var parsed = JSON.parse(line);
				if (!$.isArray(parsed)) {
					if (!header) {
						if (parsed && parsed.version === 1 && $.isArray(parsed.stdout)) {
							return parsed;
						}
						header = parsed || {};
						isV3 = header.version === 3;
						idleLimit = Number(header.idle_time_limit) || 0;
					}
					continue;
				}

				var rawT = Number(parsed[0]) || 0;
				var rawDt;
				if (isV3) {
					rawDt = Math.max(rawT, 0);
				} else {
					rawDt = Math.max(rawT - prevSec, 0);
					prevSec = rawT;
				}
				var dt = (!hasAudio && idleLimit > 0) ?
				    Math.min(rawDt, idleLimit) : rawDt;
				totalSec += dt;
				pendingDelta += dt;

				var code = parsed[1];
				var payload = parsed[2];
				if (code === 'o') {
					normEvents.push([pendingDelta, payload || '']);
					pendingDelta = 0;
				} else if (code === 'r' && typeof payload === 'string') {
					var dims = payload.split('x');
					var rCols = parseInt(dims[0], 10);
					var rRows = parseInt(dims[1], 10);
					if (rCols > 0 && rRows > 0) {
						normEvents.push([pendingDelta, '', { resize: [rCols, rRows] }]);
						pendingDelta = 0;
						hasResize = true;
					}
				} else if (code === 'm') {
					markers.push({ time: totalSec, label: payload || '' });
				}
			}

			header = header || {};
			var termMeta = (header.term && typeof header.term === 'object') ?
			    header.term : {};
			var envMeta = header.env;
			if (!envMeta && termMeta.type) {
				envMeta = { TERM: termMeta.type };
			}
			return {
				version: header.version || 2,
				width: termMeta.cols || header.width || (info && info.cols) || 80,
				height: termMeta.rows || header.height || (info && info.rows) || 24,
				duration: header.duration || totalSec,
				env: envMeta,
				term: header.term,
				theme: termMeta.theme || header.theme,
				markers: markers,
				hasResize: hasResize,
				stdout: normEvents
			};
		}

		if ($.isArray(rawEvents)) {
			var ttyStdout = [];
			var prevMs = 0;
			for (var j = 0; j < rawEvents.length; j++) {
				var ev = rawEvents[j];
				var ms = Number(ev.s) || 0;
				ttyStdout.push([Math.max((ms - prevMs) / 1000, 0), ev.e]);
				prevMs = ms;
			}
			return {
				width: (info && info.cols) || 80,
				height: (info && info.rows) || 24,
				duration: prevMs / 1000,
				stdout: ttyStdout
			};
		}

		return rawEvents;
	};

	var init = function(audioFile, containerElem, events, termInfo, opts) {
		events = normalizeEvents(events, termInfo, !!audioFile);
		opts = opts || {};

		var baseFontSize = 16;

		var themes = $.extend({}, THEMES);
		var themeOrder = THEME_ORDER.slice();
		var recordedTheme = parseCastTheme(events.theme);
		if (recordedTheme) {
			themes['recorded'] = recordedTheme;
			themeOrder.unshift('recorded');
		}

		Player.themes = themes;
		Player.markers = events.markers || [];
		Player.metadata = {
			version: events.version || 1,
			term: events.term || null,
			env: events.env || null
		};
		Player.container = $(containerElem);
		Player.container.css({
			maxWidth: '100%',
			boxSizing: 'border-box'
		});

		Player.root = $('<div class="castty-player" role="region" aria-label="Terminal recording player"></div>')
		    .appendTo(Player.container);

		Player.termContainer = $('<div id="term"></div>')
		    .appendTo(Player.root);

		Player.controls = $('<div id="controls"></div>')
		    .appendTo(Player.root);

		Player.term = new Terminal({
			rows: events.height,
			cols: events.width,
			fontFamily: 'courier-new, courier, monospace',
			fontSize: baseFontSize,
			cursorStyle: 'block',
			cursorInactiveStyle: 'block',
			disableStdin: true
		});
		Player.term.open(Player.termContainer[0]);

		/* Prevent hidden xterm textarea from summoning mobile virtual keyboards */
		if (Player.term.textarea) {
			Player.term.textarea.setAttribute('readonly', 'readonly');
			Player.term.textarea.setAttribute('inputmode', 'none');
			Player.term.textarea.setAttribute('tabindex', '-1');
			Player.term.textarea.setAttribute('aria-hidden', 'true');
			Player.term.textarea.disabled = true;
		}

		Player.themeName = 'default';
		Player.setTheme = function(themeOrName) {
			var spec = typeof themeOrName === 'string' ? themes[themeOrName] : null;
			if (spec) {
				Player.themeName = themeOrName;
				Player.term.options.minimumContrastRatio = spec.minContrast;
				Player.term.options.theme = spec.colors;
				Player.root.css('background', spec.colors.background || '#000');
				if (Player.themeToggle) {
					Player.themeToggle.attr('aria-label',
					    'Theme: ' + spec.label + '. Click to switch');
				}
			} else if (themeOrName && typeof themeOrName === 'object') {
				Player.themeName = 'custom';
				Player.term.options.theme = themeOrName;
				Player.root.css('background', themeOrName.background || '#000');
			}
		};

		var initialTheme = opts.theme;
		if (!initialTheme && window.matchMedia &&
		    window.matchMedia('(prefers-contrast: more)').matches) {
			initialTheme = 'high-contrast';
		}
		if (!initialTheme && recordedTheme) {
			initialTheme = 'recorded';
		}
		Player.setTheme(initialTheme || 'default');

		var screenEl = Player.term.element ?
		    Player.term.element.querySelector('.xterm-screen') : null;
		var baseRect = screenEl ? screenEl.getBoundingClientRect() : null;
		var baseWidth = baseRect ? baseRect.width : 0;
		var baseHeight = baseRect ? baseRect.height : 0;

		Player.isFullscreen = false;

		var isNativeFullscreen = function() {
			var fsEl = document.fullscreenElement || document.webkitFullscreenElement;
			return fsEl === Player.root[0];
		};

		Player.layout = function() {
			var fs = Player.isFullscreen || isNativeFullscreen();
			var availWidth;
			var availHeight = 0;

			if (fs) {
				availWidth = window.innerWidth || document.documentElement.clientWidth || 0;
				availHeight = window.innerHeight || document.documentElement.clientHeight || 0;
			} else {
				var docWidth = document.documentElement.clientWidth || window.innerWidth || 0;
				var rect = Player.container[0].getBoundingClientRect();
				availWidth = Player.container.innerWidth();
				if (docWidth > 0) {
					var maxFromViewport = Math.max(docWidth - Math.max(rect.left, 0) * 2, 0);
					if (!availWidth || availWidth > docWidth) {
						availWidth = maxFromViewport || docWidth;
					}
				}
			}

			if ((baseWidth <= 0 || baseHeight <= 0) && screenEl) {
				Player.term.options.fontSize = baseFontSize;
				baseRect = screenEl.getBoundingClientRect();
				baseWidth = baseRect.width;
				baseHeight = baseRect.height;
			}

			var fontSize = baseFontSize;
			if (availWidth > 0 && baseWidth > 0 && (fs || baseWidth > availWidth)) {
				fontSize = baseFontSize * (availWidth / baseWidth);
			}
			if (fs && availHeight > 0 && baseHeight > 0) {
				var controlsH = Player.controls.outerHeight() || 44;
				var maxTermH = Math.max(availHeight - controlsH, 100);
				fontSize = Math.min(fontSize, baseFontSize * (maxTermH / baseHeight));
			}
			fontSize = Math.max(4, Math.min(fontSize, fs ? 48 : baseFontSize));

			Player.term.options.fontSize = fontSize;

			var renderedRect = screenEl ? screenEl.getBoundingClientRect() : null;
			var renderedWidth = renderedRect ? renderedRect.width : baseWidth;
			if (availWidth > 0 && renderedWidth > availWidth && renderedWidth > 0) {
				fontSize = Math.max(4, fontSize * ((availWidth - 1) / renderedWidth));
				Player.term.options.fontSize = fontSize;
				renderedRect = screenEl.getBoundingClientRect();
				renderedWidth = renderedRect.width;
			}
			if (fs && availHeight > 0 && renderedRect) {
				var fsControlsH = Player.controls.outerHeight() || 44;
				var maxFsTermH = Math.max(availHeight - fsControlsH, 100);
				if (renderedRect.height > maxFsTermH && renderedRect.height > 0) {
					fontSize = Math.max(4, fontSize * (maxFsTermH / renderedRect.height));
					Player.term.options.fontSize = fontSize;
					renderedRect = screenEl.getBoundingClientRect();
					renderedWidth = renderedRect.width;
				}
			}

			var boxWidth = availWidth > 0 ?
			    Math.min(Math.ceil(renderedWidth), availWidth) :
			    Math.ceil(renderedWidth);
			Player.termWidth = boxWidth + 'px';

			Player.root.css({
				width: fs ? '100%' : Player.termWidth
			});

			Player.termContainer.css({
				margin: '0 auto',
				width: Player.termWidth,
				maxWidth: '100%',
				overflowX: renderedWidth > boxWidth + 1 ? 'auto' : 'hidden'
			});

			Player.controls.css({
				margin: '0 auto',
				width: Player.termWidth,
				maxWidth: '100%'
			});

			if (Player.seeker && Player.seeker.data('plugin_rangeslider')) {
				Player.seeker.rangeslider('update', false, false);
			}
		};

		$(window).on('resize orientationchange', function() {
			Player.layout();
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
		Player.duration = (events.duration || 0) * 1000;

		var updateTimeDisplay = function(curMs) {
			if (!Player.timeElapsed) {
				return;
			}
			var clamped = Math.min(Math.max(curMs || 0, 0), Player.duration || curMs || 0);
			var curStr = formatTime(clamped);
			var durStr = formatTime(Player.duration);
			Player.timeElapsed.text(curStr);
			Player.timeTotal.text(durStr);
			if (Player.seeker) {
				Player.seeker.attr('aria-valuetext', curStr + ' of ' + durStr);
			}
		};

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

		var initialCols = events.width || 80;
		var initialRows = events.height || 24;
		var hasResizeEvents = !!events.hasResize;

		var applyTermSize = function(cols, rows) {
			if (!cols || !rows || (cols === Player.term.cols && rows === Player.term.rows)) {
				return;
			}
			Player.term.resize(cols, rows);
			baseWidth = 0;
			baseHeight = 0;
			Player.layout();
		};

		var writeSegment = function(str, resizeDims) {
			if (resizeDims) {
				var targetCols = resizeDims[0];
				var targetRows = resizeDims[1];
				Player.term.write(str, function() {
					applyTermSize(targetCols, targetRows);
				});
			} else if (str.length > 0) {
				Player.term.write(str);
			}
		};

		var finishPlayback = function() {
			if (Player.eventOff < Player.termEvents.length) {
				var str = "";
				while (Player.eventOff < Player.termEvents.length) {
					var ev = Player.termEvents[Player.eventOff];
					if (ev[2] && ev[2].resize) {
						writeSegment(str, ev[2].resize);
						str = "";
					} else {
						str += ev[1];
					}
					Player.pos += ev[0];
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
			updateTimeDisplay(Player.duration);

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
			var str = "";

			if (back) {
				/* Send RIS (ESC c) in-band so WriteBuffer resets in FIFO order */
				if (hasResizeEvents) {
					writeSegment("\x1bc", [initialCols, initialRows]);
				} else {
					str = "\x1bc";
				}
				Player.pos = 0;
				Player.eventOff = 0;
				Player.ended = 0;
			}

			var i = Player.eventOff;
			while (i < Player.termEvents.length &&
			    Player.pos + Player.termEvents[i][0] <= targetSec) {
				var ev = Player.termEvents[i];
				if (ev[2] && ev[2].resize) {
					writeSegment(str, ev[2].resize);
					str = "";
				} else {
					str += ev[1];
				}
				Player.pos += ev[0];
				i++;
			}

			if (str.length > 0) {
				Player.term.write(str);
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
					var ev = Player.termEvents[Player.eventOff];
					if (ev[2] && ev[2].resize) {
						writeSegment(str, ev[2].resize);
						str = "";
					} else {
						str += ev[1];
					}
					Player.pos += ev[0];
					Player.eventOff++;
				}
			} else {
				var singleEv = Player.termEvents[Player.eventOff++];
				if (singleEv[2] && singleEv[2].resize) {
					writeSegment("", singleEv[2].resize);
				} else {
					str = singleEv[1];
				}
				Player.pos += singleEv[0];
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
				var curMs = Math.min(Player.audio.currentTime * 1000, Player.duration);
				Player.seeker.val(curMs).change();
				updateTimeDisplay(curMs);
			} else {
				Player.seekPos = Math.min(
				    Player.baseSeekPos + (Date.now() - Player.start),
				    Player.duration);
				Player.seeker.val(Player.seekPos).change();
				updateTimeDisplay(Player.seekPos);
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
				if (hasResizeEvents) {
					writeSegment("\x1bc", [initialCols, initialRows]);
				} else {
					Player.term.write("\x1bc");
				}
				Player.seekUpdate = 1;
				Player.seeker.val(0).change();
				Player.seekUpdate = 0;
				updateTimeDisplay(0);
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

		var step = Player.duration > 0 ? Player.duration / 1000 : 1;
		Player.seeker = $('<input type="range" aria-label="Seek" min="0" max="' +
		    Player.duration + '" step="' + step +
		    '" value="0">').appendTo(Player.controls);

		Player.maxSeek = Player.duration;
		Player.seeking = 0;
		Player.seekUnpause = 0;
		var pointerScrubbing = false;

		var commitSeek = function(val) {
			val = Math.max(0, Math.min(val, Player.maxSeek || Player.duration || val));
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
			updateTimeDisplay(val);
			if (wasEnded && !Player.ended && Player.paused) {
				setToggleIcon(ICON_PLAY, 'Play');
			}
		};

		var rangeEvents = ('PointerEvent' in window) ? {
			startEvent: ['pointerdown'],
			moveEvent: ['pointermove'],
			endEvent: ['pointerup', 'pointercancel']
		} : {
			startEvent: ['touchstart', 'mousedown'],
			moveEvent: ['touchmove', 'mousemove'],
			endEvent: ['touchend', 'touchcancel', 'mouseup']
		};

		$(Player.controls).on(rangeEvents.startEvent.join(' '), '.rangeslider', function() {
			pointerScrubbing = true;
		});

		Player.seeker.rangeslider({
			polyfill: false,
			startEvent: rangeEvents.startEvent,
			moveEvent: rangeEvents.moveEvent,
			endEvent: rangeEvents.endEvent,

			onSlide: function(pos, val) {
				if (Player.seekUpdate || !pointerScrubbing) {
					return;
				}

				if (Player.maxSeek > 0 && val > Player.maxSeek) {
					val = Player.maxSeek;
					Player.seekUpdate = 1;
					Player.seeker.val(val).change();
					Player.seekUpdate = 0;
				}

				updateTimeDisplay(val);

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
					pointerScrubbing = false;
					return;
				}

				if (Player.maxSeek > 0 && val > Player.maxSeek) {
					val = Player.maxSeek;
					Player.seekUpdate = 1;
					Player.seeker.val(val).change();
					Player.seekUpdate = 0;
				}

				commitSeek(val);
				Player.seeking = 0;
				pointerScrubbing = false;
				if (Player.seekUnpause) {
					Player.seekUnpause = 0;
					if (Player.paused) {
						Player.toggle.trigger('click');
					}
				}
			}
		});

		/* Sync keyboard arrow changes on the native range input */
		Player.seeker.on('input change', function(e, data) {
			if (Player.seekUpdate || pointerScrubbing || Player.seeking || (data && data.origin)) {
				return;
			}
			var val = parseFloat(Player.seeker.val()) || 0;
			if (e.type === 'input') {
				Player.seekUpdate = 1;
				Player.seeker.change();
				Player.seekUpdate = 0;
			}
			commitSeek(val);
		});

		Player.timeDisplay = $('<span class="castty-time" aria-label="Playback time"></span>')
		    .appendTo(Player.controls);
		Player.timeElapsed = $('<span class="castty-time-elapsed"></span>')
		    .appendTo(Player.timeDisplay);
		$('<span class="castty-time-sep" aria-hidden="true"> / </span>')
		    .appendTo(Player.timeDisplay);
		Player.timeTotal = $('<span class="castty-time-total"></span>')
		    .appendTo(Player.timeDisplay);
		updateTimeDisplay(0);

		if (audioFile) {
			Player.muteToggle = $('<button id="muteToggle" type="button" aria-label="Mute" aria-pressed="false">' +
			    ICON_VOLUME + '</button>').appendTo(Player.controls);
			Player.muteToggle.on('click', function() {
				if (!Player.audio) {
					return;
				}
				Player.audio.muted = !Player.audio.muted;
				Player.muteToggle
				    .html(Player.audio.muted ? ICON_MUTED : ICON_VOLUME)
				    .attr('aria-label', Player.audio.muted ? 'Unmute' : 'Mute')
				    .attr('aria-pressed', Player.audio.muted ? 'true' : 'false');
			});
		}

		var initSpec = themes[Player.themeName] || themes['default'];
		Player.themeToggle = $('<button id="themeToggle" type="button" aria-label="Theme: ' +
		    initSpec.label + '. Click to switch">' + ICON_THEME + '</button>')
		    .appendTo(Player.controls);
		Player.themeToggle.on('click', function() {
			var idx = $.inArray(Player.themeName, themeOrder);
			var nextName = themeOrder[(idx + 1) % themeOrder.length];
			Player.setTheme(nextName);
		});

		var fsFallbackTimer = null;
		var clearFsTimer = function() {
			if (fsFallbackTimer) {
				clearTimeout(fsFallbackTimer);
				fsFallbackTimer = null;
			}
		};

		var syncFullscreenUi = function(active) {
			Player.isFullscreen = !!active;
			Player.root.toggleClass('castty-fullscreen', Player.isFullscreen && !isNativeFullscreen());
			Player.fullscreenToggle
			    .html(Player.isFullscreen ? ICON_FULLSCREEN_EXIT : ICON_FULLSCREEN)
			    .attr('aria-label', Player.isFullscreen ? 'Exit fullscreen' : 'Enter fullscreen')
			    .attr('aria-pressed', Player.isFullscreen ? 'true' : 'false');
			Player.layout();
		};

		Player.fullscreenToggle = $('<button id="fullscreenToggle" type="button" aria-label="Enter fullscreen" aria-pressed="false">' +
		    ICON_FULLSCREEN + '</button>').appendTo(Player.controls);
		Player.fullscreenToggle.on('click', function() {
			var rootEl = Player.root[0];
			clearFsTimer();
			if (Player.isFullscreen || isNativeFullscreen()) {
				if (isNativeFullscreen()) {
					var exitFn = document.exitFullscreen || document.webkitExitFullscreen;
					if (exitFn) {
						try {
							var exitRes = exitFn.call(document);
							if (exitRes && typeof exitRes.catch === 'function') {
								exitRes.catch(function() { syncFullscreenUi(false); });
							}
							return;
						} catch (e) {}
					}
				}
				syncFullscreenUi(false);
				return;
			}

			var fsEnabled = document.fullscreenEnabled !== undefined ?
			    document.fullscreenEnabled : document.webkitFullscreenEnabled;
			var reqFn = fsEnabled !== false ?
			    (rootEl.requestFullscreen || rootEl.webkitRequestFullscreen) : null;
			if (reqFn) {
				try {
					var reqRes = reqFn.call(rootEl);
					if (reqRes && typeof reqRes.then === 'function') {
						var settled = false;
						fsFallbackTimer = setTimeout(function() {
							if (!settled) {
								settled = true;
								fsFallbackTimer = null;
								syncFullscreenUi(true);
							}
						}, 150);
						reqRes.then(function() {
							if (!settled) {
								settled = true;
								clearFsTimer();
								syncFullscreenUi(true);
							}
						}, function() {
							if (!settled) {
								settled = true;
								clearFsTimer();
								syncFullscreenUi(true);
							}
						});
						return;
					}
					syncFullscreenUi(true);
					return;
				} catch (e) {}
			}
			syncFullscreenUi(true);
		});

		$(document).on('fullscreenchange webkitfullscreenchange', function() {
			clearFsTimer();
			syncFullscreenUi(isNativeFullscreen() || Player.root.hasClass('castty-fullscreen'));
		});

		$(document).on('keydown', function(e) {
			if (e.metaKey || e.ctrlKey || e.altKey) {
				return;
			}
			var target = e.target;
			var tag = target && target.tagName ? target.tagName.toLowerCase() : '';
			var insidePlayer = $.contains(Player.root[0], target) || target === Player.root[0];
			if (!insidePlayer && target !== document.body && target !== document.documentElement) {
				return;
			}

			if (e.key === 'Escape' && Player.isFullscreen && !isNativeFullscreen()) {
				e.preventDefault();
				syncFullscreenUi(false);
			} else if (e.key === ' ' || e.key === 'k' || e.key === 'K') {
				if (e.key === ' ' && (tag === 'button' || tag === 'input')) {
					return;
				}
				e.preventDefault();
				Player.toggle.trigger('click');
			} else if ((e.key === 'ArrowLeft' || e.key === 'j' || e.key === 'J') && tag !== 'input') {
				e.preventDefault();
				var curBack = audioFile ? Player.audio.currentTime * 1000 : Player.seekPos;
				var nextBack = Math.max(curBack - 5000, 0);
				Player.seekUpdate = 1;
				Player.seeker.val(nextBack).change();
				Player.seekUpdate = 0;
				commitSeek(nextBack);
			} else if ((e.key === 'ArrowRight' || e.key === 'l' || e.key === 'L') && tag !== 'input') {
				e.preventDefault();
				var curFwd = audioFile ? Player.audio.currentTime * 1000 : Player.seekPos;
				var nextFwd = Math.min(curFwd + 5000, Player.maxSeek || Player.duration);
				Player.seekUpdate = 1;
				Player.seeker.val(nextFwd).change();
				Player.seekUpdate = 0;
				commitSeek(nextFwd);
			} else if ((e.key === 'm' || e.key === 'M') && Player.muteToggle) {
				e.preventDefault();
				Player.muteToggle.trigger('click');
			} else if (e.key === 'f' || e.key === 'F') {
				e.preventDefault();
				Player.fullscreenToggle.trigger('click');
			}
		});

		if (audioFile) {
			Player.audio = new Audio(audioFile);
			Player.audio.preload = 'auto';
			Player.audio.setAttribute('playsinline', '');

			$(Player.audio).on('loadedmetadata durationchange', function() {
				if (isFinite(Player.audio.duration) && Player.audio.duration > 0) {
					Player.maxSeek = Player.audio.duration * 1000;
					updateTimeDisplay(Player.audio.currentTime * 1000);
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

		Player.layout();
		return Player;
	};

	var termInfo = null;
	var opts = maybeOpts || {};
	if (termInfoOrOpts && typeof termInfoOrOpts === 'object') {
		if ('rows' in termInfoOrOpts || 'cols' in termInfoOrOpts) {
			termInfo = termInfoOrOpts;
		} else {
			opts = termInfoOrOpts;
		}
	}

	return init(audioFile, containerElem, events, termInfo, opts);
};
