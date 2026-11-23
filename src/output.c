#include <sys/time.h>
#include <sys/wait.h>

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "audio.h"
#include "castty.h"
#include "json.h"
#include "record.h"
#include "tui/hud.h"
#include "tui/screen.h"
#include "utf8.h"

static int audio_enabled, paused, start_paused;
static int first = 1;
static int v1_need_comma = 1;
static int need_append_resize, append_cols, append_rows;
static int audio_sync_done;
static struct timeval prevtv, nowtv;
static double aprev, anow, dur, pending_cast_offset_ms;
static FILE *evout;
static int master;
static struct tui_vterm *vt_comp;
static struct tui_surface *sf_comp;
static struct hud_layout hud_lay;
static struct hud_state hud_st;
static int cast_child_cols, cast_child_rows;

static double next_delta_ms(int start_audio);

static void
on_vt_reply(const char *bytes, size_t len, void *user)
{
	int *fdp = user;

	if (fdp != NULL && *fdp >= 0 && len > 0) {
		xwrite(*fdp, (void *)bytes, len);
	}
}

static double
live_elapsed_ms(void)
{
	if (paused || first) {
		return dur;
	}
	if (audio_enabled) {
		double d = audio_clock_ms() - aprev;

		return dur + (d > 0.0 ? d : 0.0);
	} else {
		struct timeval tv;
		double pms, nms;

		gettimeofday(&tv, NULL);
		pms = (double)(prevtv.tv_sec * 1000) +
		    ((double)prevtv.tv_usec / 1000.);
		nms = (double)(tv.tv_sec * 1000) + ((double)tv.tv_usec / 1000.);
		return dur + ((nms > pms) ? (nms - pms) : 0.0);
	}
}

static void
render_compositor(int draw_hud)
{
	int cr = 0, cc = 0, cvis = 1, cshape = 0;
	char title[256];

	if (vt_comp == NULL || sf_comp == NULL) {
		return;
	}
	(void)tui_vterm_take_dirty(vt_comp);
	if (tui_vterm_take_title(vt_comp, title, sizeof title)) {
		char osc[300];
		int n = snprintf(osc, sizeof osc, "\x1b]0;%s\x07", title);

		if (n > 0 && (size_t)n < sizeof osc) {
			(void)tui_surface_write_raw(sf_comp, osc, (size_t)n);
		}
	}
	tui_surface_begin(sf_comp);
	tui_vterm_blit(vt_comp, sf_comp, 0, 0);
	if (draw_hud &&
	    (hud_lay.active_mode != HUD_OFF || hud_st.prompt_active)) {
		hud_render(&hud_st, &hud_lay, sf_comp);
	}
	tui_vterm_get_cursor(vt_comp, &cr, &cc, &cvis, &cshape);
	tui_surface_flush(sf_comp, cr, cc, cvis, cshape);
}

static void
apply_compositor_layout(const struct control_msg *msg, int format_version)
{
	int dim_changed = (msg->cols != (unsigned short)cast_child_cols ||
	    msg->rows != (unsigned short)cast_child_rows);

	if (vt_comp != NULL) {
		tui_vterm_resize(vt_comp, msg->rows, msg->cols);
	}
	if (sf_comp != NULL) {
		if (msg->outer_rows != hud_lay.outer_rows ||
		    msg->outer_cols != hud_lay.outer_cols) {
			tui_surface_resize(sf_comp, msg->outer_rows,
			    msg->outer_cols);
		} else {
			tui_surface_invalidate(sf_comp);
		}
	}

	hud_compute_layout(msg->hud_mode, msg->outer_rows, msg->outer_cols,
	    msg->rows, msg->cols, msg->hud_mode == HUD_OFF, &hud_lay);

	if (dim_changed && !paused &&
	    (format_version == 2 || format_version == 3)) {
		double rdelta = next_delta_ms(1);
		double rtime = (format_version == 3) ? rdelta : dur;

		need_append_resize = 0;
		fprintf(evout, "[%0.4f,\"r\",\"%hux%hu\"]\n",
		    rtime / 1000, msg->cols, msg->rows);
	}
	if (dim_changed) {
		cast_child_cols = msg->cols;
		cast_child_rows = msg->rows;
	}
}

static void
sync_audio_clock(void)
{
	if (!audio_sync_done) {
		double gap = audio_clock_ms() - dur;

		if (gap > pending_cast_offset_ms) {
			pending_cast_offset_ms = gap;
		}
		audio_sync_done = 1;
	}
}

/*
 * Always drive timing from the audio sample clock when recording audio so
 * terminal events stay locked to the audio track without drift. Muting (^a m)
 * still records zeroed silence frames and keeps advancing audio_clock_ms(),
 * so we never switch clocks mid-recording.
 */
static double
next_delta_ms(int start_audio)
{
	double delta;

	if (first) {
		if (audio_enabled) {
			if (start_audio && !start_paused) {
				audio_start();
				sync_audio_clock();
			}
			anow = aprev = audio_clock_ms();
		} else {
			gettimeofday(&prevtv, NULL);
			nowtv = prevtv;
		}

		first = 0;
	} else {
		if (audio_enabled) {
			anow = audio_clock_ms();
		} else {
			gettimeofday(&nowtv, NULL);
		}
	}

	if (audio_enabled) {
		delta = anow - aprev;
		aprev = anow;
	} else {
		double pms, nms;

		pms = (double)(prevtv.tv_sec * 1000) + ((double)prevtv.tv_usec / 1000.);
		nms = (double)(nowtv.tv_sec * 1000) + ((double)nowtv.tv_usec / 1000.);

		delta = nms - pms;
		prevtv = nowtv;
	}

	if (pending_cast_offset_ms > 0.0) {
		delta += pending_cast_offset_ms;
		pending_cast_offset_ms = 0.0;
	}
	dur += delta;
	return delta;
}

static void
handle_command(const struct control_msg *msg, int format_version)
{
	static unsigned char c_l = 0x0c;

	switch (msg->cmd) {
	case CMD_MUTE:
		if (audio_enabled) {
			audio_toggle_mute();
		}
		break;

	case CMD_PAUSE:
		paused = !paused;
		hud_note_pause(&hud_st, paused);
		if (!paused) {
			/* Redraw screen */
			xwrite(master, &c_l, 1);
			if (audio_enabled) {
				audio_start();
				sync_audio_clock();
				anow = aprev = audio_clock_ms();
			} else {
				gettimeofday(&prevtv, NULL);
				nowtv = prevtv;
			}
		} else {
			if (audio_enabled) {
				audio_stop();
			}
		}
		break;

	case CMD_MARKER_PROMPT:
		hud_set_prompt(&hud_st, msg->prompt_active, msg->label);
		break;

	case CMD_MARKER: {
		char *esc = json_escape(msg->label);

		hud_set_prompt(&hud_st, 0, NULL);
		if (!paused) {
			double mdelta = next_delta_ms(1);
			double mtime = (format_version == 3) ? mdelta : dur;

			if (format_version == 2 || format_version == 3) {
				fprintf(evout, "[%0.4f,\"m\",\"%s\"]\n",
				    mtime / 1000, esc);
			}
			hud_note_marker_label(&hud_st, dur, msg->label);
		} else {
			if (format_version == 2 || format_version == 3) {
				double mtime = (format_version == 3) ? 0.0 : dur;

				fprintf(evout, "[%0.4f,\"m\",\"%s\"]\n",
				    mtime / 1000, esc);
			}
			hud_note_marker_label(&hud_st, dur, msg->label);
		}
		free(esc);
		break;
	}

	case CMD_RESIZE:
	case CMD_HUD_TOGGLE:
		apply_compositor_layout(msg, format_version);
		break;

	default:
		abort();
	}
}

static void
handle_input(unsigned char *buf, size_t buflen, int format_version)
{
	assert(format_version == 1 || format_version == 2 ||
	    format_version == 3);

	if (need_append_resize) {
		double rdelta = next_delta_ms(1);
		double rtime = (format_version == 3) ? rdelta : dur;

		fprintf(evout, "[%0.4f,\"r\",\"%dx%d\"]\n",
		    rtime / 1000, append_cols, append_rows);
		need_append_resize = 0;
	}

	double delta = next_delta_ms(1);

	if (format_version == 3) {
		fprintf(evout, "[%0.4f,\"o\",\"", delta / 1000);
	} else if (format_version == 2) {
		fprintf(evout, "[%0.4f,\"o\",\"", dur / 1000);
	} else if (format_version == 1) {
		fprintf(evout, "%s[%0.4f,\"", v1_need_comma ? "," : "",
		    delta / 1000);
		v1_need_comma = 1;
	}

	uint32_t state, cp;
	state = 0;
	for (size_t j = 0; j < buflen; j++) {
		if (!u8_decode(&state, &cp, buf[j])) {
			if ((cp < 128 && !isprint(cp)) ||
			    cp > 128) {
				if (state == UTF8_ACCEPT) {
					if (cp > 0xffff) {
						uint32_t h, l;
						h = ((cp - 0x10000) >> 10) + 0xd800;
						l = ((cp - 0x10000) & 0x3ff) + 0xdc00;
						fprintf(evout, "\\u%04" PRIx32 "\\u%04" PRIx32, h, l);
					} else {
						fprintf(evout, "\\u%04" PRIx32, cp);
					}
				} else {
					fputs("\\ud83d\\udca9", evout);
				}
			} else {
				switch (buf[j]) {
				case '"':
				case '\\':
					fputc('\\', evout); // output backslash for escaping
					fputc(buf[j], evout); // print the character itself
					break;
				default:
					fputc(buf[j], evout);
					break;
				}
			}
		}
	}

	fputs("\"]\n", evout);
}

void
outputproc(struct outargs *oa)
{
	unsigned char obuf[BUFSIZ];
	struct pollfd pollfds[2];
	int status;

	status = EXIT_SUCCESS;
	master = oa->masterfd;

	assert(oa->format_version == 1 || oa->format_version == 2 ||
	    oa->format_version == 3);

	if (oa->audioout || oa->devid) {
		assert(oa->audioout && oa->devid);
	}

	int appending = (oa->append && oa->append_info.exists);

	if (oa->audioout) {
		audio_enabled = 1;
		audio_init_append(oa->devid, oa->audioout, oa->use_raw,
		    oa->append, appending ? oa->append_info.dur_ms : 0.0);
	}

	start_paused = paused = oa->start_paused;

	if (appending) {
		dur = oa->append_info.dur_ms;
		if (oa->format_version == 1) {
			evout = xfopen(oa->outfn, "r+b");
			if (oa->append_info.v1_rewrite_buf != NULL) {
				rewind(evout);
				if (fwrite(oa->append_info.v1_rewrite_buf, 1,
				    oa->append_info.v1_rewrite_len, evout) !=
				    oa->append_info.v1_rewrite_len) {
					perror("fwrite");
					exit(EXIT_FAILURE);
				}
				fflush(evout);
				if (ftruncate(fileno(evout),
				    (off_t)oa->append_info.v1_rewrite_len) != 0) {
					perror("ftruncate");
					exit(EXIT_FAILURE);
				}
			} else {
				if (fseeko(evout,
				    (off_t)oa->append_info.v1_truncate_off,
				    SEEK_SET) != 0 ||
				    ftruncate(fileno(evout),
				    (off_t)oa->append_info.v1_truncate_off) != 0) {
					perror("ftruncate");
					exit(EXIT_FAILURE);
				}
			}
			v1_need_comma = !oa->append_info.v1_empty;
			if (oa->append_info.dur_ms >
			    oa->append_info.v1_events_ms) {
				pending_cast_offset_ms =
				    oa->append_info.dur_ms -
				    oa->append_info.v1_events_ms;
			}
		} else {
			evout = xfopen(oa->outfn, "ab");
			if (oa->append_info.need_newline) {
				fputc('\n', evout);
			}
			if (oa->cols > 0 && oa->rows > 0 &&
			    (oa->cols != oa->append_info.cols ||
			    oa->rows != oa->append_info.rows)) {
				need_append_resize = 1;
				append_cols = oa->cols;
				append_rows = oa->rows;
			}
		}
	} else {
		evout = xfopen(oa->outfn, "wb");

		/* Write asciicast header and append events. Format defined at
		 * v1 https://github.com/asciinema/asciinema/blob/master/doc/asciicast-v1.md
		 * v2 https://github.com/asciinema/asciinema/blob/master/doc/asciicast-v2.md
		 * v3 https://docs.asciinema.org/manual/asciicast/v3/
		 *
		 * With v1, we insert an empty first record to avoid the hassle of dealing with
		 * ES (still) not supporting trailing commas.
		 */
		if (oa->format_version == 3) {
			fprintf(evout,
			    "{\"version\": 3, \"term\": {\"cols\": %d, \"rows\": %d",
			    oa->cols, oa->rows);
			if (oa->term_type && *oa->term_type) {
				fprintf(evout, ", \"type\": \"%s\"", oa->term_type);
			}
			if (oa->term_version && *oa->term_version) {
				fprintf(evout, ", \"version\": \"%s\"", oa->term_version);
			}
			if (oa->theme_json && *oa->theme_json) {
				fprintf(evout, ", \"theme\": %s", oa->theme_json);
			}
			fprintf(evout, "}, \"timestamp\": %lld",
			    (long long)oa->timestamp);
			if (oa->cmd && *oa->cmd) {
				fprintf(evout, ", \"command\": \"%s\"", oa->cmd);
			}
			if (oa->title && *oa->title) {
				fprintf(evout, ", \"title\": \"%s\"", oa->title);
			}
			if (oa->env && *oa->env) {
				fprintf(evout, ", \"env\": %s", oa->env);
			}
			fprintf(evout, "}\n");
		} else {
			fprintf(evout,
			    "{\"version\": %d, "
			    "\"width\": %d, "
			    "\"height\": %d, "
			    "\"command\": \"%s\", "
			    "\"title\": \"%s\", "
			    "\"env\": %s",
			    oa->format_version,
			    oa->cols, oa->rows,
			    oa->cmd ? oa->cmd : "",
			    oa->title ? oa->title : "",
			    oa->env
			);
			if (oa->format_version == 2 && oa->theme_json &&
			    *oa->theme_json) {
				fprintf(evout, ", \"theme\": %s", oa->theme_json);
			}
			if (oa->format_version == 2) {
				// v2 header finished here, data will be appended in separate lines
				fprintf(evout, "}\n");
			} else if (oa->format_version == 1) {
				// v1 header finished, console data is appended in structure
				fprintf(evout, ",\"stdout\":[[0,\"\"]\n");
			}
		}
	}

	setbuf(evout, NULL);
	setbuf(stdout, NULL);

	xclose(STDIN_FILENO);

	cast_child_cols = oa->cols;
	cast_child_rows = oa->rows;
	if (oa->hud_enabled) {
		hud_compute_layout(oa->hud_mode, oa->outer_rows,
		    oa->outer_cols, oa->rows, oa->cols,
		    oa->hud_mode == HUD_OFF, &hud_lay);
		vt_comp = tui_vterm_create(oa->rows, oa->cols, on_vt_reply,
		    &master);
		sf_comp = tui_surface_create(oa->outer_rows, oa->outer_cols,
		    STDOUT_FILENO);
		hud_init(&hud_st, oa->start_paused, appending, audio_enabled,
		    audio_enabled ? audio_active_format_name() : "none",
		    oa->append_info.existing_markers, dur);
	}

	/* Clear screen */
	printf("\x1b[2J");

	/* Move cursor to top-left */
	printf("\x1b[H");
	if (sf_comp != NULL) {
		render_compositor(hud_lay.active_mode != HUD_OFF);
	}

	int f = fcntl(oa->masterfd, F_GETFL);
	fcntl(oa->masterfd, F_SETFL, f | O_NONBLOCK);

	f = fcntl(oa->controlfd, F_GETFL);
	fcntl(oa->controlfd, F_SETFL, f | O_NONBLOCK);

	/* Control descriptor is highest priority */
	pollfds[0].fd = oa->controlfd;
	pollfds[0].events = POLLIN;
	pollfds[0].revents = 0;

	pollfds[1].fd = oa->masterfd;
	pollfds[1].events = POLLIN;
	pollfds[1].revents = 0;

	int burst = 0;

	for (;;) {
		int nready;
		int timeout_ms = (hud_lay.active_mode != HUD_OFF ||
		    hud_st.prompt_active) ? 50 : -1;

		nready = poll(pollfds, 2, timeout_ms);
		if (nready == -1 && errno == EINTR) {
			continue;
		} else if (nready == -1) {
			perror("poll");
			status = EXIT_FAILURE;
			goto end;
		}

		for (int i = 0; i < 2 && nready > 0; i++) {
			if (pollfds[i].revents & POLLIN) {
				if (pollfds[i].fd == oa->controlfd) {
					struct control_msg msg;
					ssize_t nread;

					nread = read(oa->controlfd, &msg, sizeof msg);
					if (nread == -1 || nread != sizeof msg) {
						perror("read");
						status = EXIT_FAILURE;
						goto end;
					}

					handle_command(&msg, oa->format_version);
				} else if (pollfds[i].fd == oa->masterfd) {
					ssize_t nread;

					nread = read(oa->masterfd, obuf, BUFSIZ);
					if (nread < 0) {
						if (errno == EAGAIN || errno == EWOULDBLOCK) {
							continue;
						}
						status = (errno == EIO) ?
						    EXIT_SUCCESS : EXIT_FAILURE;
						goto end;
					}
					if (nread == 0) {
						status = EXIT_SUCCESS;
						goto end;
					}

					if (vt_comp != NULL) {
						tui_vterm_feed(vt_comp,
						    (const char *)obuf,
						    (size_t)nread,
						    STDOUT_FILENO);
					} else {
						xwrite(STDOUT_FILENO, obuf,
						    nread);
					}

					if (!paused) {
						hud_note_output(&hud_st);
						handle_input(obuf, nread, oa->format_version);
					}
					if (nread == BUFSIZ && ++burst < 8) {
						continue;
					}
				}
			}

			if (pollfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
				status = (pollfds[i].revents & (POLLERR | POLLNVAL)) ?
				    EXIT_FAILURE : EXIT_SUCCESS;
				goto end;
			}
		}

		burst = 0;
		if (vt_comp != NULL && sf_comp != NULL) {
			struct audio_vu vu;

			if (hud_lay.active_mode != HUD_OFF) {
				if (audio_enabled) {
					audio_get_vu(&vu);
				} else {
					memset(&vu, 0, sizeof vu);
				}
				hud_tick(&hud_st, live_elapsed_ms(),
				    audio_enabled ? &vu : NULL);
			}
			render_compositor(hud_lay.active_mode != HUD_OFF);
		}
	}

end:
	if (vt_comp != NULL && sf_comp != NULL) {
		if (hud_lay.active_mode != HUD_OFF) {
			render_compositor(0);
		}
		tui_vterm_destroy(vt_comp);
		tui_surface_destroy(sf_comp);
		vt_comp = NULL;
		sf_comp = NULL;
	}
	if (oa->format_version == 3 && oa->shellpid > 0) {
		int wstatus = 0;
		pid_t r = 0;

		for (int attempt = 0; attempt < 50; attempt++) {
			r = waitpid(oa->shellpid, &wstatus, WNOHANG);
			if (r != 0) {
				break;
			}
			struct timespec ts = { 0, 1000000L }; /* 1 ms */
			nanosleep(&ts, NULL);
		}
		if (r == oa->shellpid) {
			int code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) :
			    (WIFSIGNALED(wstatus) ? 128 + WTERMSIG(wstatus) : 1);
			double xdelta = (first || paused) ? 0.0 : next_delta_ms(0);
			fprintf(evout, "[%0.4f,\"x\",\"%d\"]\n",
			    xdelta / 1000, code);
		}
	} else if ((oa->format_version == 1 || oa->format_version == 2) &&
	    !first && !paused) {
		double edelta = next_delta_ms(0);
		if (oa->format_version == 2 && edelta >= 1.0) {
			fprintf(evout, "[%0.4f,\"o\",\"\"]\n", dur / 1000);
		}
	}
	if (oa->format_version == 1) {
		fprintf(evout, "], \"duration\": %.9g}\n", dur / 1000);
	}

	fflush(evout);

	if (oa->audioout && oa->devid) {
		if (!paused) {
			audio_stop();
		}

		audio_exit();
	}

	xfclose(evout);
	xclose(oa->masterfd);

	exit(status);
}
