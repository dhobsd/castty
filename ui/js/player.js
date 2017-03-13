/* https://stackoverflow.com/questions/1582534/calculating-text-width/15302051#15302051 */
$.fn.textWidth = function(text, font) {
	if (!$.fn.textWidth.fakeEl) $.fn.textWidth.fakeEl = $('<span>').hide().appendTo(document.body);
	$.fn.textWidth.fakeEl.text(text || this.val() || this.text()).css('font-family', font || this.css('font-family'));
	return $.fn.textWidth.fakeEl.width();
};

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
	}
})();

var player = function(audioFile, containerElem, termEvents, termInfo) {
	var Player = {};
	
	var init = function(audioFile, containerElem, termEvents, termInfo) {
		Player.container = containerElem;

		Player.termContainer = $('<div id="term"></div>')
		    .appendTo(Player.container);

		Player.controls = $('<div id="controls"></div>')
		    .appendTo(Player.container);

		Player.term = new Terminal({
			rows: termInfo.rows,
			cols: termInfo.cols
		});
		Player.term.open(Player.termContainer[0]);
		Player.termWidth = $(Player.termContainer).textWidth('m'.repeat(termInfo.cols),
		    'courier-new,courier,monospace') + 'px';

		Player.termContainer.css({
		    margin: '0 auto',
		    width: Player.termWidth
		});

		Player.controls.css({
		    margin: '0 auto',
		    width: Player.termWidth
		});

		Player.termEvents = termEvents;
		Player.eventOff = 0;
		Player.rem = 0;
		Player.timerHandle = undefined;

		Player.playTime = 0;

		Player.seekTo = function(t) {
			var back = Player.eventOff && t < Player.termEvents[Player.eventOff - 1].s;

			if (back) {
				Player.term.clear();
				Player.term.reset();
			}

			var i = back ? 0 : Player.eventOff;
			var str = "";
			while (i < Player.termEvents.length &&
			    Player.termEvents[i].s <= t) {
				str += Player.termEvents[i++].e;
			}

			i = i == Player.termEvents.length ? i - 1 : i;

			Player.term.write(str);
			Player.eventOff = i;

			return Player.termEvents[i].s - t;
		};

		Player.nextEvent = function() {
			Player.term.write(Player.termEvents[Player.eventOff].e);

			if (Player.termEvents[Player.eventOff + 1]) {
				Player.timerHandle = setTimeout(Player.nextEvent,
				    (Player.termEvents[Player.eventOff + 1].s -
				     Player.audio.currentTime * 1000));
			}

			Player.eventOff++;
		}

		Player.updateSeeker = function() {
			Player.seekUpdate = 1;
			Player.seeker.val(100 + Player.audio.currentTime * 1000 +
			    ((Player.audio.currentTime * 1000) % 100)).change();
			Player.seekUpdate = 0;

			Player.seekerHandle = setTimeout(Player.updateSeeker, 100);
		}

		Player.toggle = $('<button id="playToggle" disabled>(Loading)</button>')
			.appendTo(Player.controls);

		Player.paused = 1;
		Player.ended = 0;
		Player.toggle.click(function() {
			if (Player.startable) {
				if (Player.ended) {
					Player.term.clear();
					Player.seekUpdate = 1;
					Player.seeker.val(0).change();
					Player.seekUpdate = 0;
					Player.ended = 0;
					Player.paused = 1;
					Player.restarted = 1;
				}

				if (Player.paused) {
					Player.paused = 0;
					Player.toggle.html('<i class="material-icons">pause</i>');
					Player.start = Date.now();
					Player.audio.play();
					Player.timerHandle =
					    setTimeout(Player.nextEvent, Player.rem);
					Player.seekerHandle =
					    setTimeout(Player.updateSeeker, 100);
				} else {
					Player.audio.pause();
					if (Player.seekerHandle) {
						clearTimeout(Player.seekerHandle);
					}

					if (Player.timerHandle) {
						Player.rem =
						    getTimeout(Player.timerHandle);
						clearTimeout(Player.timerHandle);
					}
					Player.toggle.html('<i class="material-icons">play_arrow</i>');
					Player.paused = 1;
				}
			}
		});

		Player.duration = Player.termEvents[Player.termEvents.length - 1].s;
		Player.seeker = $('<input type="range" min="0" max="' +
		    Player.duration + '" step="' + Player.duration / 1000 +
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

				clearTimeout(Player.timerHandle);
				Player.rem = Player.seekTo(val);
				Player.audio.currentTime = val / 1000;
				Player.seeking = 0;
			}
		});

		Player.audio = new Audio(audioFile);
		$(Player.audio).on('ended', function () {
			Player.seekUpdate = 1;
			Player.seeker.val(Player.duration).change();
			Player.seekUpdate = 0;

			Player.ended = 1;
			Player.paused = 1;
			Player.rem = 0;
			Player.eventOff = 0;
			Player.toggle.html('<i class="material-icons">replay</i>');

			clearTimeout(Player.timerHandle);
			clearTimeout(Player.seekerHandle);
			Player.timerHandle = undefined;
			Player.seekerHandle = undefined;
		});

		$(Player.audio).on('durationchange', function() {
			Player.maxSeek = Player.audio.duration * 1000;
		});

		$(Player.audio).on('canplaythrough', function() {
			Player.startable = 1;
			if (!Player.restarted) {
				Player.toggle.html('<i class="material-icons">play_arrow</i>');
			} else {
				Player.restarted = 0;
			}
			Player.toggle.prop('disabled', false);
		});

		return Player;
	}

	return init(audioFile, containerElem, termEvents, termInfo);
}
