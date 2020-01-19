/*
 * Loudness normalizer based on the EBU R128 standard
 *
 * Copyright (c) 2014, Alessandro Ghedini
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <limits.h>

// Include libutil
#include "config.h"
#ifdef HAVE_PTY_H
	#include <pty.h>
#endif
#ifdef HAVE_UTIL_H
	#include <util.h>
#endif
#ifdef HAVE_LIBUTIL_H
	#include <libutil.h>
#endif

#include <sys/ioctl.h>

#include "lg-util.h"

int use_syslog = 0;
int quiet = 0;

static void do_log(const char *prefix, const char *fmt, va_list args);
static void get_screen_size(int fd, unsigned *w, unsigned *h);

void ok_printf(const char *fmt, ...) {
	va_list args;

	if (quiet) return;

	va_start(args, fmt);
	do_log("[" COLOR_GREEN "✔" COLOR_OFF "] ", fmt, args);
	va_end(args);
}

void debug_printf(const char *fmt, ...) {
	va_list args;

	if (quiet) return;

	va_start(args, fmt);
	do_log("[" COLOR_YELLOW "¡" COLOR_OFF "] ", fmt, args);
	va_end(args);
}

void warn_printf(const char *fmt, ...) {
	va_list args;

	va_start(args, fmt);
	do_log("[" COLOR_YELLOW "!" COLOR_OFF "] ", fmt, args);
	va_end(args);
}

void err_printf(const char *fmt, ...) {
	va_list args;

	va_start(args, fmt);
	do_log("[" COLOR_RED "✘" COLOR_OFF "] ", fmt, args);
	va_end(args);
}

void fail_printf(const char *fmt, ...) {
	va_list args;

	va_start(args, fmt);
	do_log("[" COLOR_RED "✘" COLOR_OFF "] ", fmt, args);
	va_end(args);

	_exit(EXIT_FAILURE);
}

void sysf_printf(const char *fmt, ...) {
	int rc;
	va_list args;

	_free_ char *format = NULL;

	rc = asprintf(&format, "%s: %s", fmt, strerror(errno));
	if (rc < 0) fail_printf("OOM");

	va_start(args, fmt);
	do_log("[" COLOR_RED "✘" COLOR_OFF "] ", format, args);
	va_end(args);

	_exit(EXIT_FAILURE);
}

void progress_bar(unsigned ctrl, unsigned long x, unsigned long n, unsigned w) {
	int i;
	FILE *stream = stdout;
	static int show_bar = 0;

	switch (ctrl) {
		case 0: /* init */
			if (quiet)
				break;

			if (!isatty(fileno(stream)))
				break;

			show_bar = 1;

			break;

		case 1: /* draw */
			if (show_bar != 1)
				return;

			if ((x != n) && (x % (n / 100 + 1) != 0))
				return;

			if (w == 0) {
				get_screen_size(fileno(stream), &w, NULL);
				w -= 8;
			}

			double ratio = x / (double) n;
			int   c     = ratio * w;

			fprintf(stream, " %3.0f%% [", ratio * 100);

			for (i = 0; i < c; i++)
				fputc('=', stream);

			for (i = c; i < w; i++)
				fputc(' ', stream);

			fprintf(stream, "]\r");
			fflush(stream);

			break;

		case 2: /* end */
			if (show_bar == 1)
				fputc('\n', stream);

			break;
	}
}

static void do_log(const char *pre, const char *fmt, va_list args) {
	int rc;
	static char format[LINE_MAX];

	rc = snprintf(format, LINE_MAX, "%s%s\n", use_syslog ? "" : pre, fmt);
	if (rc < 0) fail_printf("EIO");

	if (use_syslog == 1)
		vsyslog(LOG_CRIT, format, args);
	else
		vfprintf(stderr, format, args);
}

static void get_screen_size(int fd, unsigned *w, unsigned *h) {
	struct winsize ws;

	if (ioctl(fd, TIOCGWINSZ, &ws) < 0 || !ws.ws_row || !ws.ws_col)
		return;

	if (w != NULL)
		*w = ws.ws_col;

	if (h != NULL)
		*h = ws.ws_row;
}
