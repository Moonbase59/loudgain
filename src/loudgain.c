/*
 * Loudness normalizer based on the EBU R128 standard
 *
 * Copyright (c) 2014, Alessandro Ghedini
 * All rights reserved.
 *
 * 2019-06-30 - v0.2.1 - Matthias C. Hormann
 *  - Added version
 *  - Added writing tags to Ogg Vorbis files (now supports MP3, FLAC, Ogg Vorbis)
 *  - Always remove REPLAYGAIN_REFERENCE_LOUDNESS, wrong value might confuse players
 *  - Added notice in help on which file types can be written
 *  - Added album summary
 * 2019-07-07 - v0.2.2 - Matthias C. Hormann
 *  - Fixed album peak calculation.
 *  - Write REPLAYGAIN_ALBUM_* tags only if in album mode
 *  - Better versioning (CMakeLists.txt → config.h)
 *  - TODO: clipping calculation still wrong
 * 2019-07-08 - v0.2.4 - Matthias C. Hormann
 *  - add "-s e" mode, writes extra tags (REPLAYGAIN_REFERENCE_LOUDNESS,
 *    REPLAYGAIN_TRACK_RANGE and REPLAYGAIN_ALBUM_RANGE)
 *  - add "-s l" mode (like "-s e" but uses LU/LUFS instead of dB)
 * 2019-07-08 - v0.2.5 - Matthias C. Hormann
 *  - Clipping warning & prevention (-k) now works correctly, both track & album
 * 2019-07-09 - v0.2.6 - Matthias C. Hormann
 *  - Add "-L" mode to force lowercase tags in MP3/ID3v2.
 * 2019-07-10 - v0.2.7 - Matthias C. Hormann
 *  - Add "-S" mode to strip ID3v1/APEv2 tags from MP3 files.
 *  - Add "-I 3"/"-I 4" modes to select ID3v2 version to write.
 *  - First step to implement a new tab-delimited list format: "-O" mode.
 * 2019-07-13 - v0.2.8 - Matthias C. Hormann
 *  - new -O output format: re-ordered, now shows peak before/after gain applied
 *  - -k now defaults to clipping prevention at -1 dBTP (as EBU recommends)
 *  - New -K: Allows clippping prevention with settable dBTP level,
 *     i.e. -K 0 (old-style) or -K -2 (to compensate for post-processing losses)
 * 2019-08-06 - v0.5.3 - Matthias C. Hormann
 *  - Add support for Opus (.opus) files.
 * 2019-08-16 - v0.6.0 - Matthias C. Hormann
 *  - Rework for new FFmpeg API (get rid of deprecated calls)
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
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <math.h>
#include <getopt.h>

#include <ebur128.h>
#include <libavcodec/avcodec.h>
#include <libavutil/common.h>
#include <libswresample/swresample.h>
#include <libavformat/avformat.h>


#include "config.h"
#include "scan.h"
#include "tag.h"
#include "printf.h"

const char *short_opts = "rackK:d:oOqs:LSI:h?v";

static struct option long_opts[] = {
	{ "track",        no_argument,       NULL, 'r' },
	{ "album",        no_argument,       NULL, 'a' },

	{ "clip",         no_argument,       NULL, 'c' },
	{ "noclip",       no_argument,       NULL, 'k' },
	{ "maxtpl",       required_argument, NULL, 'K' },

	{ "pregain",      required_argument, NULL, 'd' },

	{ "output",       no_argument,       NULL, 'o' },
	{ "output-new",   no_argument,       NULL, 'O' },
	{ "quiet",        no_argument,       NULL, 'q' },

	{ "tagmode",      required_argument, NULL, 's' },
	{ "lowercase",    no_argument,       NULL, 'L' },
	{ "striptags",    no_argument,       NULL, 'S' },
	{ "id3v2version", required_argument, NULL, 'I' },

	{ "help",         no_argument,       NULL, 'h' },
	{ "version",      no_argument,       NULL, 'v' },
	{ 0, 0, 0, 0 }
};

enum AV_CONTAINER_ID {
    AV_CONTAINER_ID_MP3,
		AV_CONTAINER_ID_FLAC,
		AV_CONTAINER_ID_OGG,
		AV_CONTAINER_ID_MP4,
		AV_CONTAINER_ID_ASF,
		AV_CONTAINER_ID_WAV,
		AV_CONTAINER_ID_WV,
		AV_CONTAINER_ID_AIFF,
		AV_CONTAINER_ID_APE
};

// FFmpeg container short names
static const char *AV_CONTAINER_NAME[] = {
    "mp3",
    "flac",
    "ogg",
    "mov,mp4,m4a,3gp,3g2,mj2",
    "asf",
    "wav",
		"wv",
		"aiff",
		"ape"
};

int name_to_id(const char *str) {
	int i;
	for (i = 0;  i < sizeof(AV_CONTAINER_NAME) / sizeof(AV_CONTAINER_NAME[0]);  i++) {
		if (!strcmp (str, AV_CONTAINER_NAME[i])) {
			return i;
		}
	}
	return -1; // error
}


bool warn_ebu            = false;
int  ebur128_v_major     = 0;
int  ebur128_v_minor     = 0;
int  ebur128_v_patch     = 0;
char ebur128_version[15] = "";
int  tag_v_major     = 0;
int  tag_v_minor     = 0;
int  tag_v_patch     = 0;
char tag_version[15] = "";
unsigned swr_ver         = 0;
char     swr_version[15] = "";
unsigned lavf_ver         = 0;
char     lavf_version[15] = "";

static inline void help(void);
static inline void version(void);

int main(int argc, char *argv[]) {
	int rc, i;

	char mode           = 's';
	char unit[3]        = "dB";

	unsigned nb_files   = 0;

	double pre_gain     = 0.f;
	double max_true_peak_level = -1.0; // dBTP; default for -k, as per EBU Tech 3343

	bool no_clip        = false;
	bool warn_clip      = true;
	bool do_album       = false;
	bool tab_output     = false;
	bool tab_output_new = false;
	bool lowercase      = false; // force MP3 ID3v2 tags to lowercase?
	bool strip          = false; // MP3 ID3v2: strip other tag types?
	int  id3v2version   = 4;     // MP3 ID3v2 version to write; can be 3 or 4

	// taglib version 
	tag_get_version(&tag_v_major, &tag_v_minor, &tag_v_patch);
	snprintf(tag_version, sizeof(tag_version), "%d.%d.%d",
		tag_v_major, tag_v_minor, tag_v_patch);

	// libebur128 version check -- versions before 1.2.4 aren’t recommended
	ebur128_get_version(&ebur128_v_major, &ebur128_v_minor, &ebur128_v_patch);
	snprintf(ebur128_version, sizeof(ebur128_version), "%d.%d.%d",
		ebur128_v_major, ebur128_v_minor, ebur128_v_patch);
	if (ebur128_v_major <= 1 && ebur128_v_minor <= 2 && ebur128_v_patch < 4)
		warn_ebu = true;

	// libavformat version
	lavf_ver = avformat_version();
	snprintf(lavf_version, sizeof(lavf_version), "%u.%u.%u",
		lavf_ver>>16, lavf_ver>>8&0xff, lavf_ver&0xff);

	// libswresample version
	swr_ver = swresample_version();
	snprintf(swr_version, sizeof(swr_version), "%u.%u.%u",
		swr_ver>>16, swr_ver>>8&0xff, swr_ver&0xff);

	while ((rc = getopt_long(argc, argv, short_opts, long_opts, &i)) !=-1) {
		switch (rc) {
			case 'r':
				/* noop */
				break;

			case 'a':
				do_album = true;
				break;

			case 'c':
				warn_clip = false;
				break;

			case 'k':
				// old-style, no argument, now defaults to -1 dBTP max. true peak level
				no_clip = true;
				break;

			case 'K': {
				// new style, argument in dBTP, sets max. true peak level
				no_clip = true;

				char *rest = NULL;
				max_true_peak_level = strtod(optarg, &rest);

				if (!rest ||
				    (rest == optarg) ||
				    !isfinite(pre_gain))
					fail_printf("Invalid max. true peak level (dBTP)");
				break;
			}

			case 'd': {
				char *rest = NULL;
				pre_gain = strtod(optarg, &rest);

				if (!rest ||
				    (rest == optarg) ||
				    !isfinite(pre_gain))
					fail_printf("Invalid pregain value (dB/LU)");
				break;
			}

			case 'o':
				tab_output = true;
				break;

			case 'O':
				tab_output_new = true;
				break;

			case 'q':
				quiet = 1;
				break;

			case 's': {
				// for mp3gain compatibilty, include modes that do nothing
				char *valid_modes = "cdielavsr";
				mode = optarg[0];
				if (strchr(valid_modes, mode) == NULL)
					fail_printf("Invalid tag mode: '%c'", mode);
				if (mode == 'l') {
					strcpy(unit, "LU");
				}
				break;
			}

			case 'L':
				lowercase = true;
				break;

			case 'S':
				strip = true;
				break;

			case 'I':
				id3v2version = atoi(optarg);
				if (!(id3v2version == 3) && !(id3v2version == 4))
					fail_printf("Invalid ID3v2 version; only 3 and 4 are supported.");
				break;

			case '?':
				if (optopt == 0) {
					// actual option '-?'
					help();
					return 0;
				} else {
					// getopt error, message already printed
					return 1;	// error
				}
			case 'h':
				help();
				return 0;

			case 'v':
				version();
				return 0;
		}
	}

	nb_files = argc - optind;

	scan_init(nb_files);

	for (i = optind; i < argc; i++) {
		ok_printf("Scanning '%s' ...", argv[i]);

		scan_file(argv[i], i - optind);
	}

	// check for different file (codec) types in an album and warn
	// (including Opus might mess up album gain)
	if (do_album) {
		if (scan_album_has_different_containers() || scan_album_has_different_codecs()) {
			warn_printf("You have different file types in the same album!");
			if (scan_album_has_opus())
				fail_printf("Cannot calculate correct album gain when mixing Opus and non-Opus files!");
		}
	}

	if (tab_output)
		printf("File\tMP3 gain\tdB gain\tMax Amplitude\tMax global_gain\tMin global_gain\n");

	if (tab_output_new)
		printf("File\tLoudness\tRange\tTrue_Peak\tTrue_Peak_dBTP\tReference\tWill_clip\tClip_prevent\tGain\tNew_Peak\tNew_Peak_dBTP\n");

	for (i = 0; i < nb_files; i++) {
		bool will_clip = false;
		double tgain = 1.0; // "gained" track peak
		double tnew;
		double tpeak = pow(10.0, max_true_peak_level / 20.0); // track peak limit
		double again = 1.0; // "gained" album peak
		double anew;
		double apeak = pow(10.0, max_true_peak_level / 20.0); // album peak limit
		bool tclip = false;
		bool aclip = false;

		scan_result *scan = scan_get_track_result(i, pre_gain);

		if (scan == NULL)
			continue;

		if (do_album)
			scan_set_album_result(scan, pre_gain);

		// Check if track or album will clip, and correct if so requested (-k/-K)

		// track peak after gain
		tgain = pow(10.0, scan -> track_gain / 20.0) * scan -> track_peak;
		tnew = tgain;
		if (do_album) {
			// album peak after gain
			again = pow(10.0, scan -> album_gain / 20.0) * scan -> album_peak;
			anew = again;
		}

		if ((tgain > tpeak) || (do_album && (again > apeak)))
			will_clip = true;

		// printf("\ntrack: %.2f LU, peak %.6f; album: %.2f LU, peak %.6f\ntrack: %.6f, %.6f; album: %.6f, %.6f; Clip: %s\n",
		// 	scan -> track_gain, scan -> track_peak, scan -> album_gain, scan -> album_peak,
		// 	tgain, tpeak, again, apeak, will_clip ? "Yes" : "No");

		if (will_clip && no_clip) {
			if (tgain > tpeak) {
				// set new track peak = minimum of peak after gain and peak limit
				tnew = FFMIN(tgain, tpeak);
				scan -> track_gain = scan -> track_gain - (log10(tgain/tnew) * 20.0);
				tclip = true;
			}

			if (do_album && (again > apeak)) {
				anew = FFMIN(again, apeak);
				scan -> album_gain = scan -> album_gain - (log10(again/anew) * 20.0);
				aclip = true;
			}

			will_clip = false;

			// printf("\nAfter clipping prevention:\ntrack: %.2f LU, peak %.6f; album: %.2f LU, peak %.6f\ntrack: %.6f, %.6f; album: %.6f, %.6f; Clip: %s\n",
			// 	scan -> track_gain, scan -> track_peak, scan -> album_gain, scan -> album_peak,
			// 	tgain, tpeak, again, apeak, will_clip ? "Yes" : "No");
		}

		switch (mode) {
			case 'c': /* check tags */
				break;

			case 'd': /* delete tags */
				switch (name_to_id(scan -> container)) {

					case AV_CONTAINER_ID_MP3:
						if (!tag_clear_mp3(scan, strip, id3v2version))
							err_printf("Couldn't write to: %s", scan -> file);
						break;

					case AV_CONTAINER_ID_FLAC:
						if (!tag_clear_flac(scan))
							err_printf("Couldn't write to: %s", scan -> file);
						break;

					case AV_CONTAINER_ID_OGG:
						// must separate because TagLib uses fifferent File classes
						switch (scan->codec_id) {
							// Opus needs special handling (different RG tags, -23 LUFS ref.)
							case AV_CODEC_ID_OPUS:
								if (!tag_clear_ogg_opus(scan))
									err_printf("Couldn't write to: %s", scan -> file);
								break;

							case AV_CODEC_ID_VORBIS:
								if (!tag_clear_ogg_vorbis(scan))
									err_printf("Couldn't write to: %s", scan -> file);
								break;

							case AV_CODEC_ID_FLAC:
								if (!tag_clear_ogg_flac(scan))
									err_printf("Couldn't write to: %s", scan -> file);
								break;

							case AV_CODEC_ID_SPEEX:
								if (!tag_clear_ogg_speex(scan))
									err_printf("Couldn't write to: %s", scan -> file);
								break;

							default:
								err_printf("Codec 0x%x in %s container not supported",
									scan->codec_id, scan->container);
								break;
						}
						break;

					case AV_CONTAINER_ID_MP4:
						if (!tag_clear_mp4(scan))
							err_printf("Couldn't write to: %s", scan -> file);
						break;

					case AV_CONTAINER_ID_ASF:
						if (!tag_clear_asf(scan))
							err_printf("Couldn't write to: %s", scan -> file);
						break;

					case AV_CONTAINER_ID_WAV:
						if (!tag_clear_wav(scan, strip, id3v2version))
							err_printf("Couldn't write to: %s", scan -> file);
						break;

					case AV_CONTAINER_ID_AIFF:
						if (!tag_clear_aiff(scan, strip, id3v2version))
							err_printf("Couldn't write to: %s", scan -> file);
						break;

					case AV_CONTAINER_ID_WV:
						if (!tag_clear_wavpack(scan, strip))
							err_printf("Couldn't write to: %s", scan -> file);
						break;

					case AV_CONTAINER_ID_APE:
						if (!tag_clear_ape(scan, strip))
							err_printf("Couldn't write to: %s", scan -> file);
						break;

					default:
						err_printf("File type not supported: %s", scan->container);
						break;
				}
				break;

			case 'i': /* ID3v2 tags */
			case 'e': /* same as 'i' plus extra tags */
			case 'l': /* same as 'e' but in LU/LUFS units (instead of 'dB')*/
				switch (name_to_id(scan -> container)) {

					case AV_CONTAINER_ID_MP3:
						if (!tag_write_mp3(scan, do_album, mode, unit, lowercase, strip, id3v2version))
							err_printf("Couldn't write to: %s", scan -> file);
						break;

					case AV_CONTAINER_ID_FLAC:
						if (!tag_write_flac(scan, do_album, mode, unit))
							err_printf("Couldn't write to: %s", scan -> file);
						break;

					case AV_CONTAINER_ID_OGG:
						// must separate because TagLib uses fifferent File classes
						switch (scan->codec_id) {
							// Opus needs special handling (different RG tags, -23 LUFS ref.)
							case AV_CODEC_ID_OPUS:
								if (!tag_write_ogg_opus(scan, do_album, mode, unit))
									err_printf("Couldn't write to: %s", scan -> file);
								break;

							case AV_CODEC_ID_VORBIS:
								if (!tag_write_ogg_vorbis(scan, do_album, mode, unit))
									err_printf("Couldn't write to: %s", scan -> file);
								break;

							case AV_CODEC_ID_FLAC:
								if (!tag_write_ogg_flac(scan, do_album, mode, unit))
									err_printf("Couldn't write to: %s", scan -> file);
								break;

							case AV_CODEC_ID_SPEEX:
								if (!tag_write_ogg_speex(scan, do_album, mode, unit))
									err_printf("Couldn't write to: %s", scan -> file);
								break;

							default:
								err_printf("Codec 0x%x in %s container not supported",
									scan->codec_id, scan->container);
								break;
						}
						break;

					case AV_CONTAINER_ID_MP4:
						if (!tag_write_mp4(scan, do_album, mode, unit, lowercase))
							err_printf("Couldn't write to: %s", scan -> file);
						break;

					case AV_CONTAINER_ID_ASF:
						if (!tag_write_asf(scan, do_album, mode, unit, lowercase))
							err_printf("Couldn't write to: %s", scan -> file);
						break;

					case AV_CONTAINER_ID_WAV:
						if (!tag_write_wav(scan, do_album, mode, unit, lowercase, strip, id3v2version))
							err_printf("Couldn't write to: %s", scan -> file);
						break;

					case AV_CONTAINER_ID_AIFF:
						if (!tag_write_aiff(scan, do_album, mode, unit, lowercase, strip, id3v2version))
							err_printf("Couldn't write to: %s", scan -> file);
						break;

					case AV_CONTAINER_ID_WV:
						if (!tag_write_wavpack(scan, do_album, mode, unit, lowercase, strip))
							err_printf("Couldn't write to: %s", scan -> file);
						break;

					case AV_CONTAINER_ID_APE:
						if (!tag_write_ape(scan, do_album, mode, unit, lowercase, strip))
							err_printf("Couldn't write to: %s", scan -> file);
						break;

					default:
						err_printf("File type not supported: %s", scan->container);
						break;
				}
				break;

			case 'a': /* APEv2 tags */
				err_printf("APEv2 tags are not supported");
				break;

			case 'v': /* Vorbis Comments tags */
				err_printf("Vorbis Comment tags are not supported");
				break;

			case 's': /* skip tags */
				break;

			case 'r': /* force re-calculation */
				break;

			default:
				err_printf("Invalid tag mode");
				break;
		}

		if (tab_output) {
			// output old-style mp3gain-compatible list
			printf("%s\t", scan -> file);
			printf("%d\t", 0);
			printf("%.2f\t", scan -> track_gain);
			printf("%.6f\t", scan -> track_peak * 32768.0);
			printf("%d\t", 0);
			printf("%d\n", 0);

			if (warn_clip && will_clip)
				err_printf("The track will clip");

			if ((i == (nb_files - 1)) && do_album) {
				printf("%s\t", "Album");
				printf("%d\t", 0);
				printf("%.2f\t", scan -> album_gain);
				printf("%.6f\t", scan -> album_peak * 32768.0);
				printf("%d\t", 0);
				printf("%d\n", 0);
			}
		} else if (tab_output_new) {
			// output new style list: File;Loudness;Range;Gain;Reference;Peak;Peak dBTP;Clipping;Clip-prevent
			printf("%s\t", scan -> file);
			printf("%.2f LUFS\t", scan -> track_loudness);
			printf("%.2f %s\t", scan -> track_loudness_range, unit);
			printf("%.6f\t", scan -> track_peak);
			printf("%.2f dBTP\t", 20.0 * log10(scan -> track_peak));
			printf("%.2f LUFS\t", scan -> loudness_reference);
			printf("%s\t", will_clip ? "Y" : "N");
			printf("%s\t", tclip ? "Y" : "N");
			printf("%.2f %s\t", scan -> track_gain, unit);
			printf("%.6f\t", tnew);
			printf("%.2f dBTP\n", 20.0 * log10(tnew));

			if ((i == (nb_files - 1)) && do_album) {
				printf("%s\t", "Album");
				printf("%.2f LUFS\t", scan -> album_loudness);
				printf("%.2f %s\t", scan -> album_loudness_range, unit);
				printf("%.6f\t", scan -> album_peak);
				printf("%.2f dBTP\t", 20.0 * log10(scan -> album_peak));
				printf("%.2f LUFS\t", scan -> loudness_reference);
				printf("%s\t", (!aclip && (again > apeak)) ? "Y" : "N");
				printf("%s\t", aclip ? "Y" : "N");
				printf("%.2f %s\t", scan -> album_gain, unit);
				printf("%.6f\t", anew);
				printf("%.2f dBTP\n", 20.0 * log10(anew));
			}
		} else {
			// output something human-readable
			printf("\nTrack: %s\n", scan -> file);

			printf(" Loudness: %8.2f LUFS\n", scan -> track_loudness);
			printf(" Range:    %8.2f %s\n", scan -> track_loudness_range, unit);
			printf(" Peak:     %8.6f (%.2f dBTP)\n", scan -> track_peak, 20.0 * log10(scan -> track_peak));
			if (scan -> codec_id == AV_CODEC_ID_OPUS) {
				// also show the Q7.8 number that goes into R128_TRACK_GAIN
				printf(" Gain:     %8.2f %s (%d)%s\n", scan -> track_gain, unit,
				 gain_to_q78num(scan -> track_gain),
				 tclip ? " (corrected to prevent clipping)" : "");
			} else {
				printf(" Gain:     %8.2f %s%s\n", scan -> track_gain, unit,
				 tclip ? " (corrected to prevent clipping)" : "");
			}

			if (warn_clip && will_clip)
				err_printf("The track will clip");

			if ((i == (nb_files - 1)) && do_album) {
				printf("\nAlbum:\n");

				printf(" Loudness: %8.2f LUFS\n", scan -> album_loudness);
				printf(" Range:    %8.2f %s\n", scan -> album_loudness_range, unit);
				printf(" Peak:     %8.6f (%.2f dBTP)\n", scan -> album_peak, 20.0 * log10(scan -> album_peak));
				if (scan -> codec_id == AV_CODEC_ID_OPUS) {
					// also show the Q7.8 number that goes into R128_ALBUM_GAIN
					printf(" Gain:     %8.2f %s (%d)%s\n", scan -> album_gain, unit,
					gain_to_q78num(scan -> album_gain),
						aclip ? " (corrected to prevent clipping)" : "");
				} else {
					printf(" Gain:     %8.2f %s%s\n", scan -> album_gain, unit,
						aclip ? " (corrected to prevent clipping)" : "");
				}
			}
		}

		free(scan);
	}

	scan_deinit();

	return 0;
}

static inline void help(void) {
	#define CMD_HELP(CMDL, CMDS, MSG) printf("  %s%-5s %-16s%s  %s.\n", COLOR_YELLOW, CMDS ",", CMDL, COLOR_OFF, MSG);
	#define CMD_CONT(MSG) printf("  %s%-5s %-16s%s  %s.\n", COLOR_YELLOW, "", "", COLOR_OFF, MSG);

	printf(COLOR_RED "Usage: " COLOR_OFF);
	printf("%s%s%s ", COLOR_GREEN, PROJECT_NAME, COLOR_OFF);
	puts("[OPTIONS] FILES...\n");

	printf("%s %s supports writing tags to the following file types:\n", PROJECT_NAME, PROJECT_VER);
	puts("  FLAC (.flac), Ogg (.ogg, .oga, .spx, .opus), MP2 (.mp2), MP3 (.mp3),");
	puts("  MP4 (.mp4, .m4a), ASF/WMA (.asf, .wma), WavPack (.wv), APE (.ape).");
	puts("  Experimental: WAV (.wav), AIFF (.aiff, .aif, .snd).\n");

	if (warn_ebu) {
		printf("%sWarning:%s Your EBU R128 library (libebur128) is version %s.\n", COLOR_RED, COLOR_OFF, ebur128_version);
		puts("This is an old version and might cause problems.\n");
	}

	puts(COLOR_RED "Options:" COLOR_OFF);

	CMD_HELP("--help",     "-h", "Show this help");
	CMD_HELP("--version",  "-v", "Show version number");

	puts("");

	CMD_HELP("--track",  "-r", "Calculate track gain only (default)");
	CMD_HELP("--album",  "-a", "Calculate album gain (and track gain)");

	puts("");

	CMD_HELP("--clip",   "-c", "Ignore clipping warning");
	CMD_HELP("--noclip", "-k", "Lower track/album gain to avoid clipping (<= -1 dBTP)");
	CMD_HELP("--maxtpl=n", "-K n", "Avoid clipping; max. true peak level = n dBTP");

	CMD_HELP("--pregain=n",  "-d n",  "Apply n dB/LU pre-gain value (-5 for -23 LUFS target)");

	puts("");

	CMD_HELP("--tagmode=d", "-s d",  "Delete ReplayGain tags from files");
	CMD_HELP("--tagmode=i", "-s i",  "Write ReplayGain 2.0 tags to files");
	CMD_HELP("--tagmode=e", "-s e",  "like '-s i', plus extra tags (reference, ranges)");
	CMD_HELP("--tagmode=l", "-s l",  "like '-s e', but LU units instead of dB");

	CMD_HELP("--tagmode=s", "-s s",  "Don't write ReplayGain tags (default)");

	puts("");

	CMD_HELP("--lowercase", "-L", "Force lowercase tags (MP2/MP3/MP4/WMA/WAV/AIFF)");
	CMD_CONT("This is non-standard but sometimes needed");
	CMD_HELP("--striptags", "-S", "Strip tag types other than ID3v2 from MP2/MP3");
	CMD_CONT("Strip tag types other than APEv2 from WavPack/APE");
	CMD_HELP("--id3v2version=3", "-I 3", "Write ID3v2.3 tags to MP2/MP3/WAV/AIFF");
	CMD_HELP("--id3v2version=4", "-I 4", "Write ID3v2.4 tags to MP2/MP3/WAV/AIFF (default)");

	puts("");

	CMD_HELP("--output",     "-o",  "Database-friendly tab-delimited list output");
	CMD_HELP("--output-new", "-O",  "New format tab-delimited list output");
	CMD_HELP("--quiet",      "-q",  "Don't print scanning status messages");

	puts("");
	// puts("Mandatory arguments to long options are also mandatory for any corresponding short options.");
	// puts("");
	puts("Please report any issues to https://github.com/Moonbase59/loudgain/issues.");
	puts("");
}

static inline void version(void) {
	printf("%s %s - using:\n", PROJECT_NAME, PROJECT_VER);
	printf("  %s %s\n", "libtag", tag_version);
	printf("  %s %s\n", "libebur128", ebur128_version);
	printf("  %s %s\n", "libavformat", lavf_version);
	printf("  %s %s\n", "libswresample", swr_version);
}
