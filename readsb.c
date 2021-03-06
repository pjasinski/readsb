// Part of readsb, a Mode-S/ADSB/TIS message decoder.
//
// readsb.c: main program & miscellany
//
// Copyright (c) 2019 Michael Wolf <michael@mictronics.de>
//
// This code is based on a detached fork of dump1090-fa.
//
// Copyright (c) 2014-2016 Oliver Jowett <oliver@mutability.co.uk>
//
// This file is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.
//
// This file is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// This file incorporates work covered by the following copyright and
// license:
//
// Copyright (C) 2012 by Salvatore Sanfilippo <antirez@gmail.com>
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//  *  Redistributions of source code must retain the above copyright
//     notice, this list of conditions and the following disclaimer.
//
//  *  Redistributions in binary form must reproduce the above copyright
//     notice, this list of conditions and the following disclaimer in the
//     documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#define READSB
#include "readsb.h"
#include "help.h"
#include "geomag.h"

#include <stdarg.h>

struct _Modes Modes;

static void backgroundTasks(void);
//
// ============================= Program options help ==========================
//
// This is a little silly, but that's how the preprocessor works..
#define _stringize(x) #x

static error_t parse_opt(int key, char *arg, struct argp_state *state);
const char *argp_program_version = VERSION_STRING;
const char doc[] = "readsb Mode-S/ADSB/TIS Receiver   "
        VERSION_STRING
        "\nBuild options: "
#ifdef ENABLE_RTLSDR
        "ENABLE_RTLSDR "
#endif
#ifdef ENABLE_BLADERF
        "ENABLE_BLADERF "
#endif
#ifdef ENABLE_PLUTOSDR
        "ENABLE_PLUTOSDR "
#endif
#ifdef SC16Q11_TABLE_BITS
#define stringize(x) _stringize(x)
        "SC16Q11_TABLE_BITS=" stringize(SC16Q11_TABLE_BITS)
#undef stringize
#endif
"\v"
"Debug mode flags: d = Log frames decoded with errors\n"
"                  D = Log frames decoded with zero errors\n"
"                  c = Log frames with bad CRC\n"
"                  C = Log frames with good CRC\n"
"                  p = Log frames with bad preamble\n"
"                  n = Log network debugging info\n"
"                  j = Log frames to frames.js, loadable by debug.html\n";

#undef _stringize
#undef verstring

const char args_doc[] = "";
static struct argp argp = {options, parse_opt, args_doc, doc, NULL, NULL, NULL};

//
// ============================= Utility functions ==========================
//
static void log_with_timestamp(const char *format, ...) __attribute__ ((format(printf, 1, 2)));
static void cleanup_and_exit(int code);

static void log_with_timestamp(const char *format, ...) {
    char timebuf[128];
    char msg[1024];
    time_t now;
    struct tm local;
    va_list ap;

    now = time(NULL);
    localtime_r(&now, &local);
    strftime(timebuf, 128, "%c %Z", &local);
    timebuf[127] = 0;

    va_start(ap, format);
    vsnprintf(msg, 1024, format, ap);
    va_end(ap);
    msg[1023] = 0;

    fprintf(stderr, "%s  %s\n", timebuf, msg);
}

static void cond_broadcast_all() {

    if (Modes.jsonThread)
        pthread_cond_broadcast(&Modes.jsonThreadCond);

    if (Modes.jsonGlobeThread)
        pthread_cond_broadcast(&Modes.jsonGlobeThreadCond);

    for (int i = 0; i < TRACE_THREADS; i++) {
        if (Modes.jsonTraceThread[i])
            pthread_cond_broadcast(&Modes.jsonTraceThreadCond[i]);
    }

    if (Modes.decodeThread) {
        pthread_cond_broadcast(&Modes.decodeThreadCond);
        pthread_cond_broadcast(&Modes.data_cond);
    }

    pthread_cond_broadcast(&Modes.mainThreadCond);
}

static void sigintHandler(int dummy) {
    MODES_NOTUSED(dummy);
    Modes.exit = 1; // Signal to threads that we are done

    cond_broadcast_all();

    signal(SIGINT, SIG_DFL); // reset signal handler - bit extra safety
    log_with_timestamp("Caught SIGINT, shutting down..\n");
}

static void sigtermHandler(int dummy) {
    MODES_NOTUSED(dummy);
    Modes.exit = 1; // Signal to threads that we are done

    cond_broadcast_all();

    signal(SIGTERM, SIG_DFL); // reset signal handler - bit extra safety
    log_with_timestamp("Caught SIGTERM, shutting down..\n");
}

void receiverPositionChanged(float lat, float lon, float alt) {
    log_with_timestamp("Autodetected receiver location: %.5f, %.5f at %.0fm AMSL", lat, lon, alt);
    writeJsonToFile(Modes.json_dir, "receiver.json", generateReceiverJson()); // location changed
}


//
// =============================== Initialization ===========================
//
static void modesInitConfig(void) {
    // Default everything to zero/NULL
    memset(&Modes, 0, sizeof (Modes));

    for (int i = 0; i < 256; i++) {
        Modes.threadNumber[i] = i;
    }

    // Now initialise things that should not be 0/NULL to their defaults
    Modes.gain = MODES_MAX_GAIN;
    Modes.freq = MODES_DEFAULT_FREQ;
    Modes.check_crc = 1;
    Modes.net_heartbeat_interval = MODES_NET_HEARTBEAT_INTERVAL;
    Modes.db_file = strdup("/usr/local/share/tar1090/git-db/aircraft.csv.gz");
    Modes.net_input_raw_ports = strdup("0");
    Modes.net_output_raw_ports = strdup("0");
    Modes.net_output_sbs_ports = strdup("0");
    Modes.net_input_sbs_ports = strdup("0");
    Modes.net_input_beast_ports = strdup("0");
    Modes.net_output_beast_ports = strdup("0");
    Modes.net_output_beast_reduce_ports = strdup("0");
    Modes.net_output_beast_reduce_interval = 125;
    Modes.net_output_vrs_ports = strdup("0");
    Modes.net_output_vrs_interval = 5 * SECONDS;
    Modes.net_output_json_ports = strdup("0");
    Modes.net_output_api_ports = strdup("0");
    Modes.net_connector_delay = 30 * 1000;
    Modes.interactive_display_ttl = MODES_INTERACTIVE_DISPLAY_TTL;
    Modes.json_interval = 1000;
    Modes.json_location_accuracy = 1;
    Modes.maxRange = 1852 * 300; // 300NM default max range
    Modes.mode_ac_auto = 0;
    Modes.nfix_crc = 1;
    Modes.biastee = 0;
    Modes.filter_persistence = 8;
    Modes.net_sndbuf_size = 2; // Default to 256 kB network write buffers
    Modes.net_output_flush_size = 1280; // Default to 1280 Bytes
    Modes.net_output_flush_interval = 50; // Default to 50 ms
    Modes.netReceiverId = 0;
    Modes.netIngest = 0;
    Modes.uuidFile = strdup("/boot/adsbx-uuid");
    Modes.json_trace_interval = 30 * 1000;
    Modes.heatmap_current_interval = -1;
    Modes.heatmap_interval = 60 * SECONDS;
    Modes.json_reliable = -13;

    Modes.cpr_focus = 0xc0ffeeba;
    //Modes.cpr_focus = 0x43BF95;
    //
    //Modes.receiver_focus = 0x1aa14156975948af;

    sdrInitConfig();

    reset_stats(&Modes.stats_current);
    for (int i = 0; i < 90; ++i) {
        reset_stats(&Modes.stats_10[i]);
    }
    //receiverTest();
    Modes.scratch = malloc(sizeof(struct aircraft));
}
//
//=========================================================================
//
static void modesInit(void) {
    int i;

    Modes.startup_time = mstime();

    if (Modes.json_reliable == -13) {
        if (Modes.json_globe_index || Modes.globe_history_dir)
            Modes.json_reliable = 2;
        else if (Modes.bUserFlags & MODES_USER_LATLON_VALID)
            Modes.json_reliable = 1;
        else
            Modes.json_reliable = 2;
    }
    if (Modes.net_output_flush_interval < 5)
        Modes.net_output_flush_interval = 5;
    if (Modes.net_output_flush_interval > 1000)
        Modes.net_output_flush_interval = 1000;

    Modes.filter_persistence += Modes.json_reliable - 1;

    uint64_t now = mstime();
    Modes.next_stats_update = now + 10 * SECONDS;
    Modes.next_stats_display = now + Modes.stats;

    pthread_mutex_init(&Modes.mainThreadMutex, NULL);
    pthread_cond_init(&Modes.mainThreadCond, NULL);

    pthread_mutex_init(&Modes.data_mutex, NULL);
    pthread_cond_init(&Modes.data_cond, NULL);

    pthread_mutex_init(&Modes.decodeThreadMutex, NULL);
    pthread_mutex_init(&Modes.jsonThreadMutex, NULL);
    pthread_mutex_init(&Modes.jsonGlobeThreadMutex, NULL);

    pthread_cond_init(&Modes.decodeThreadCond, NULL);
    pthread_cond_init(&Modes.jsonThreadCond, NULL);
    pthread_cond_init(&Modes.jsonGlobeThreadCond, NULL);

    for (int i = 0; i < TRACE_THREADS; i++) {
        pthread_mutex_init(&Modes.jsonTraceThreadMutex[i], NULL);
        pthread_cond_init(&Modes.jsonTraceThreadCond[i], NULL);
    }

    geomag_init();

    Modes.sample_rate = (double)2400000.0;

    // Allocate the various buffers used by Modes
    Modes.trailing_samples = (MODES_PREAMBLE_US + MODES_LONG_MSG_BITS + 16) * 1e-6 * Modes.sample_rate;

    if (Modes.sdr_type == SDR_NONE) {
        if (Modes.net)
            Modes.net_only = 1;
        if (!Modes.net_only) {
            fprintf(stderr, "No networking or SDR input selected, exiting!\n");
            cleanup_and_exit(1);
        }
    } else if (Modes.sdr_type == SDR_MODESBEAST || Modes.sdr_type == SDR_GNS) {
        Modes.net_only = 1;
    } else {
        Modes.net_only = 0;
    }

    if (!Modes.net_only) {
        for (i = 0; i < MODES_MAG_BUFFERS; ++i) {
            if ((Modes.mag_buffers[i].data = calloc(MODES_MAG_BUF_SAMPLES + Modes.trailing_samples, sizeof (uint16_t))) == NULL) {
                fprintf(stderr, "Out of memory allocating magnitude buffer.\n");
                exit(1);
            }

            Modes.mag_buffers[i].length = 0;
            Modes.mag_buffers[i].dropped = 0;
            Modes.mag_buffers[i].sampleTimestamp = 0;
        }
    }

    // Validate the users Lat/Lon home location inputs
    if ((Modes.fUserLat > 90.0) // Latitude must be -90 to +90
            || (Modes.fUserLat < -90.0) // and
            || (Modes.fUserLon > 360.0) // Longitude must be -180 to +360
            || (Modes.fUserLon < -180.0)) {
        Modes.fUserLat = Modes.fUserLon = 0.0;
    } else if (Modes.fUserLon > 180.0) { // If Longitude is +180 to +360, make it -180 to 0
        Modes.fUserLon -= 360.0;
    }
    // If both Lat and Lon are 0.0 then the users location is either invalid/not-set, or (s)he's in the
    // Atlantic ocean off the west coast of Africa. This is unlikely to be correct.
    // Set the user LatLon valid flag only if either Lat or Lon are non zero. Note the Greenwich meridian
    // is at 0.0 Lon,so we must check for either fLat or fLon being non zero not both.
    // Testing the flag at runtime will be much quicker than ((fLon != 0.0) || (fLat != 0.0))
    Modes.bUserFlags &= ~MODES_USER_LATLON_VALID;
    if ((Modes.fUserLat != 0.0) || (Modes.fUserLon != 0.0) || Modes.bUserFlags & MODES_USER_LATLON_VALID) {
        Modes.bUserFlags |= MODES_USER_LATLON_VALID;
        fprintf(stderr, "Using lat: %9.4f, lon: %9.4f\n", Modes.fUserLat, Modes.fUserLon);
    }

    // Limit the maximum requested raw output size to less than one Ethernet Block
    // Set to default if 0
    if (Modes.net_output_flush_size > (MODES_OUT_FLUSH_SIZE) || Modes.net_output_flush_size == 0) {
        Modes.net_output_flush_size = MODES_OUT_FLUSH_SIZE;
    }
    if (Modes.net_output_flush_interval > (MODES_OUT_FLUSH_INTERVAL)) {
        Modes.net_output_flush_interval = MODES_OUT_FLUSH_INTERVAL;
    }
    if (Modes.net_sndbuf_size > (MODES_NET_SNDBUF_MAX)) {
        Modes.net_sndbuf_size = MODES_NET_SNDBUF_MAX;
    }

    if((Modes.net_connector_delay <= 0) || (Modes.net_connector_delay > 86400 * 1000)) {
        Modes.net_connector_delay = 30 * 1000;
    }

    if (Modes.api) {
        Modes.byLat = malloc(API_INDEX_MAX * sizeof(struct av));
        Modes.byLon = malloc(API_INDEX_MAX * sizeof(struct av));
    }

    // Prepare error correction tables
    modesChecksumInit(Modes.nfix_crc);
    icaoFilterInit();
    modeACInit();

    if (Modes.show_only)
        icaoFilterAdd(Modes.show_only);

    Modes.json_globe_special_tiles = calloc(GLOBE_SPECIAL_INDEX, sizeof(struct tile));
    init_globe_index(Modes.json_globe_special_tiles);
}

//
//=========================================================================
//
// We read data using a thread, so the main thread only handles decoding
// without caring about data acquisition
//
static void *readerThreadEntryPoint(void *arg) {
    MODES_NOTUSED(arg);
    srandom(get_seed());

    sdrRun();

    // Wake the main thread (if it's still waiting)
    pthread_mutex_lock(&Modes.data_mutex);
    if (!Modes.exit)
        Modes.exit = 2; // unexpected exit
    pthread_cond_signal(&Modes.data_cond);
    pthread_mutex_unlock(&Modes.data_mutex);

#ifndef _WIN32
    pthread_exit(NULL);
#else
    return NULL;
#endif
}

static void *jsonThreadEntryPoint(void *arg) {
    MODES_NOTUSED(arg);
    srandom(get_seed());

    uint64_t sleep_ms = Modes.json_interval;

    pthread_mutex_lock(&Modes.jsonThreadMutex);

    uint64_t next_history = mstime();

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    while (!Modes.exit) {

        incTimedwait(&ts, sleep_ms);

        int res = 0;
        while (!Modes.exit && res == 0) {
            res = pthread_cond_timedwait(&Modes.jsonThreadCond, &Modes.jsonThreadMutex, &ts);
        }
        if (Modes.exit)
            break;

        struct timespec start_time;
        start_cpu_timing(&start_time);

        uint64_t now = mstime();

        struct char_buffer cb = generateAircraftJson();
        if (Modes.json_gzip)
            writeJsonToGzip(Modes.json_dir, "aircraft.json.gz", cb, 3);
        writeJsonToFile(Modes.json_dir, "aircraft.json", cb);

        if ((ALL_JSON) && now >= next_history) {
            char filebuf[PATH_MAX];

            snprintf(filebuf, PATH_MAX, "history_%d.json", Modes.json_aircraft_history_next);
            writeJsonToFile(Modes.json_dir, filebuf, generateAircraftJson());

            if (!Modes.json_aircraft_history_full) {
                writeJsonToFile(Modes.json_dir, "receiver.json", generateReceiverJson()); // number of history entries changed
                if (Modes.json_aircraft_history_next == HISTORY_SIZE - 1)
                    Modes.json_aircraft_history_full = 1;
            }

            Modes.json_aircraft_history_next = (Modes.json_aircraft_history_next + 1) % HISTORY_SIZE;
            next_history = now + HISTORY_INTERVAL;
        }

        end_cpu_timing(&start_time, &Modes.stats_current.aircraft_json_cpu);
    }

    pthread_mutex_unlock(&Modes.jsonThreadMutex);

#ifndef _WIN32
    pthread_exit(NULL);
#else
    return NULL;
#endif
}

static void *jsonGlobeThreadEntryPoint(void *arg) {
    MODES_NOTUSED(arg);
    srandom(get_seed());

    static int part;
    int n_parts = 4; // power of 2

    uint64_t sleep_ms = Modes.json_interval / n_parts;

    pthread_mutex_lock(&Modes.jsonGlobeThreadMutex);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    while (!Modes.exit) {
        char filename[32];

        incTimedwait(&ts, sleep_ms);

        int res = 0;
        while (!Modes.exit && res == 0) {
            res = pthread_cond_timedwait(&Modes.jsonGlobeThreadCond, &Modes.jsonGlobeThreadMutex, &ts);
        }
        if (Modes.exit)
            break;

        struct timespec start_time;
        start_cpu_timing(&start_time);

        for (int i = 0; i <= GLOBE_MAX_INDEX; i++) {
            if (i == GLOBE_SPECIAL_INDEX)
                i = GLOBE_MIN_INDEX;

            if (i % n_parts != part)
                continue;

            if (i >= GLOBE_MIN_INDEX && globe_index_index(i) < GLOBE_MIN_INDEX)
                continue;

            snprintf(filename, 31, "globe_%04d.binCraft", i);
            struct char_buffer cb2 = generateGlobeBin(i, 0);
            writeJsonToGzip(Modes.json_dir, filename, cb2, 5);
            free(cb2.buffer);

            snprintf(filename, 31, "globeMil_%04d.binCraft", i);
            struct char_buffer cb3 = generateGlobeBin(i, 1);
            writeJsonToGzip(Modes.json_dir, filename, cb3, 5);
            free(cb3.buffer);

            if (!Modes.jsonBinCraft) {
                snprintf(filename, 31, "globe_%04d.json", i);
                struct char_buffer cb = generateGlobeJson(i);
                writeJsonToGzip(Modes.json_dir, filename, cb, 3);
                free(cb.buffer);
            }
        }

        part++;
        part %= n_parts;
        end_cpu_timing(&start_time, &Modes.stats_current.globe_json_cpu);
    }

    pthread_mutex_unlock(&Modes.jsonGlobeThreadMutex);

#ifndef _WIN32
    pthread_exit(NULL);
#else
    return NULL;
#endif
}

static void *decodeThreadEntryPoint(void *arg) {
    MODES_NOTUSED(arg);
    srandom(get_seed());

    pthread_mutex_lock(&Modes.decodeThreadMutex);

    /* If the user specifies --net-only, just run in order to serve network
     * clients without reading data from the RTL device.
     * This rules also in case a local Mode-S Beast is connected via USB.
     */

    if (Modes.net_only) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        while (!Modes.exit) {
            struct timespec start_time;
            struct timespec watch;

            startWatch(&watch);
            start_cpu_timing(&start_time);

            backgroundTasks();

            end_cpu_timing(&start_time, &Modes.stats_current.background_cpu);
            int64_t elapsed = stopWatch(&watch);

            if (elapsed > 80) {
                static int antiSpam;
                if (--antiSpam <= 0) {
                    fprintf(stderr, "<3>High load: net work took %"PRId64" ms, suppressing for 300 loops!\n", elapsed);
                    antiSpam = 300;
                }
            }
            //fprintf(stderr, "net work took %"PRId64" ms\n", elapsed);

            incTimedwait(&ts, Modes.net_output_flush_interval);

            //startWatch(&watch);
            int res = 0;
            while (!Modes.exit && res == 0) {
                res = pthread_cond_timedwait(&Modes.decodeThreadCond, &Modes.decodeThreadMutex, &ts);
            }
            //elapsed = stopWatch(&watch);
            //fprintf(stderr, "slept for %"PRId64" ms\n", elapsed);

            if (Modes.exit)
                break;
        }
    } else {
        int watchdogCounter = 50; // about 5 seconds

        // Create the thread that will read the data from the device.
        pthread_mutex_lock(&Modes.data_mutex);
        pthread_create(&Modes.reader_thread, NULL, readerThreadEntryPoint, NULL);

        while (!Modes.exit) {
            struct timespec start_time;

            if (Modes.first_free_buffer == Modes.first_filled_buffer) {
                /* wait for more data.
                 * we should be getting data every 50-60ms. wait for max 100ms before we give up and do some background work.
                 * this is fairly aggressive as all our network I/O runs out of the background work!
                 */

                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 100000000;
                normalize_timespec(&ts);

                pthread_mutex_unlock(&Modes.decodeThreadMutex);

                pthread_cond_timedwait(&Modes.data_cond, &Modes.data_mutex, &ts); // This unlocks Modes.data_mutex, and waits for Modes.data_cond

                pthread_mutex_lock(&Modes.decodeThreadMutex);
            }

            // Modes.data_mutex is locked, and possibly we have data.

            // copy out reader CPU time and reset it
            add_timespecs(&Modes.reader_cpu_accumulator, &Modes.stats_current.reader_cpu, &Modes.stats_current.reader_cpu);
            Modes.reader_cpu_accumulator.tv_sec = 0;
            Modes.reader_cpu_accumulator.tv_nsec = 0;

            if (Modes.first_free_buffer != Modes.first_filled_buffer) {
                // FIFO is not empty, process one buffer.

                struct mag_buf *buf;

                start_cpu_timing(&start_time);
                buf = &Modes.mag_buffers[Modes.first_filled_buffer];

                // Process data after releasing the lock, so that the capturing
                // thread can read data while we perform computationally expensive
                // stuff at the same time.
                pthread_mutex_unlock(&Modes.data_mutex);

                demodulate2400(buf);
                if (Modes.mode_ac) {
                    demodulate2400AC(buf);
                }

                Modes.stats_current.samples_processed += buf->length;
                Modes.stats_current.samples_dropped += buf->dropped;
                end_cpu_timing(&start_time, &Modes.stats_current.demod_cpu);

                // Mark the buffer we just processed as completed.
                pthread_mutex_lock(&Modes.data_mutex);
                Modes.first_filled_buffer = (Modes.first_filled_buffer + 1) % MODES_MAG_BUFFERS;
                pthread_cond_signal(&Modes.data_cond);
                pthread_mutex_unlock(&Modes.data_mutex);
                watchdogCounter = 50;
            } else {
                // Nothing to process this time around.
                pthread_mutex_unlock(&Modes.data_mutex);
                if (--watchdogCounter <= 0) {
                    log_with_timestamp("No data received from the SDR for a long time, it may have wedged, exiting!");
                    Modes.exit = 1;
                    sdrCancel();
                }
            }

            start_cpu_timing(&start_time);
            backgroundTasks();
            end_cpu_timing(&start_time, &Modes.stats_current.background_cpu);
            pthread_mutex_lock(&Modes.data_mutex);
        }

        pthread_mutex_unlock(&Modes.data_mutex);

        log_with_timestamp("Waiting for receive thread termination");
        int res;
        int count = 100;
        // Wait on reader thread exit
        while (count-- > 0 && (res = pthread_tryjoin_np(Modes.reader_thread, NULL))) {
            struct timespec slp = {0, 100 * 1000 * 1000};
            nanosleep(&slp, NULL);
        }
        if (res) {
            log_with_timestamp("Receive thread termination failed, will raise SIGKILL on exit!");
            Modes.exit = SIGKILL;
        } else {
            pthread_cond_destroy(&Modes.data_cond); // Thread cleanup - only after the reader thread is dead!
            pthread_mutex_destroy(&Modes.data_mutex);
        }
    }

    pthread_mutex_unlock(&Modes.decodeThreadMutex);

#ifndef _WIN32
    pthread_exit(NULL);
#else
    return NULL;
#endif
}
//
// ============================== Snip mode =================================
//
// Get raw IQ samples and filter everything is < than the specified level
// for more than 256 samples in order to reduce example file size
//
static void snipMode(int level) {
    int i, q;
    uint64_t c = 0;

    while ((i = getchar()) != EOF && (q = getchar()) != EOF) {
        if (abs(i - 127) < level && abs(q - 127) < level) {
            c++;
            if (c > MODES_PREAMBLE_SIZE) continue;
        } else {
            c = 0;
        }
        putchar(i);
        putchar(q);
    }
}

static void display_total_stats(void) {
    struct stats added;
    add_stats(&Modes.stats_alltime, &Modes.stats_current, &added);
    display_stats(&added);
}

//
//=========================================================================
//
// This function is called a few times every second by main in order to
// perform tasks we need to do continuously, like accepting new clients
// from the net, refreshing the screen in interactive mode, and so forth
//
static void backgroundTasks(void) {
    static uint64_t next_second;

    icaoFilterExpire();

    if (Modes.net) {
        modesNetPeriodicWork();
    }

    uint64_t now = mstime();
    if (now > next_second) {
        next_second = now + 1000;

        if (Modes.net)
            modesNetSecondWork();
    }

    // Refresh screen when in interactive mode
    if (Modes.interactive) {
        interactiveShowData();
    }

}

//=========================================================================
// Clean up memory prior to exit.
static void cleanup_and_exit(int code) {
    Modes.exit = 1;
    // Free any used memory
    geomag_destroy();
    interactiveCleanup();
    free(Modes.scratch);
    free(Modes.dev_name);
    free(Modes.filename);
    /* Free only when pointing to string in heap (strdup allocated when given as run parameter)
     * otherwise points to const string
     */
    free(Modes.byLat);
    free(Modes.byLon);
    free(Modes.prom_file);
    free(Modes.json_dir);
    free(Modes.globe_history_dir);
    free(Modes.heatmap_dir);
    free(Modes.state_dir);
    free(Modes.rssi_table);
    free(Modes.net_bind_address);
    free(Modes.db_file);
    free(Modes.net_input_beast_ports);
    free(Modes.net_output_beast_ports);
    free(Modes.net_output_beast_reduce_ports);
    free(Modes.net_output_vrs_ports);
    free(Modes.net_input_raw_ports);
    free(Modes.net_output_raw_ports);
    free(Modes.net_output_sbs_ports);
    free(Modes.net_input_sbs_ports);
    free(Modes.net_output_json_ports);
    free(Modes.net_output_api_ports);
    free(Modes.beast_serial);
    free(Modes.json_globe_special_tiles);
    free(Modes.uuidFile);
    free(Modes.dbIndex);
    free(Modes.db);
    /* Go through tracked aircraft chain and free up any used memory */
    for (int j = 0; j < AIRCRAFT_BUCKETS; j++) {
        struct aircraft *a = Modes.aircraft[j], *na;
        while (a) {
            na = a->next;
            if (a) {

                if (a->first_message)
                    free(a->first_message);
                if (a->trace) {
                    free(a->trace);
                    free(a->trace_all);
                }

                free(a);
            }
            a = na;
        }
    }

    int i;
    for (i = 0; i < MODES_MAG_BUFFERS; ++i) {
        free(Modes.mag_buffers[i].data);
    }
    crcCleanupTables();

    receiverCleanup();

    for (int i = 0; i <= GLOBE_MAX_INDEX; i++) {
        ca_destroy(&Modes.globeLists[i]);
    }

#ifndef _WIN32
    exit(code);
#else
    return (code);
#endif
}

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
    switch (key) {
        case OptDevice:
            Modes.dev_name = strdup(arg);
            break;
        case OptGain:
            Modes.gain = (int) (atof(arg)*10); // Gain is in tens of DBs
            break;
        case OptFreq:
            Modes.freq = (int) strtoll(arg, NULL, 10);
            break;
        case OptDcFilter:
            Modes.dc_filter = 1;
            break;
        case OptBiasTee:
            Modes.biastee = 1;
            break;
        case OptFix:
            Modes.nfix_crc = 1;
            break;
        case OptNoFix:
            Modes.nfix_crc = 0;
            break;
        case OptRaw:
            Modes.raw = 1;
            break;
        case OptNet:
            Modes.net = 1;
            break;
        case OptModeAc:
            Modes.mode_ac = 1;
            Modes.mode_ac_auto = 0;
            break;
        case OptNoModeAcAuto:
            Modes.mode_ac_auto = 0;
            break;
        case OptNetOnly:
            Modes.net = 1;
            Modes.sdr_type = SDR_NONE;
            Modes.net_only = 1;
            break;
        case OptQuiet:
            Modes.quiet = 1;
            break;
        case OptShowOnly:
            Modes.show_only = (uint32_t) strtoul(arg, NULL, 16);
            break;
        case OptMlat:
            Modes.mlat = 1;
            break;
        case OptForwardMlat:
            Modes.forward_mlat = 1;
            break;
        case OptOnlyAddr:
            Modes.onlyaddr = 1;
            break;
        case OptMetric:
            Modes.metric = 1;
            break;
        case OptGnss:
            Modes.use_gnss = 1;
            break;
        case OptAggressive:
            Modes.nfix_crc = MODES_MAX_BITERRORS;
            break;
        case OptInteractive:
            Modes.interactive = 1;
            break;
        case OptInteractiveTTL:
            Modes.interactive_display_ttl = (uint64_t) (1000 * atof(arg));
            break;
        case OptLat:
            Modes.fUserLat = atof(arg);
            break;
        case OptLon:
            Modes.fUserLon = atof(arg);
            break;
        case OptMaxRange:
            Modes.maxRange = atof(arg) * 1852.0; // convert to metres
            break;
        case OptStats:
            if (!Modes.stats)
                Modes.stats = (uint64_t) 1 << 60; // "never"
            break;
        case OptStatsRange:
            Modes.stats_range_histo = 1;
            break;
        case OptStatsEvery:
            Modes.stats = (uint64_t) (1000 * atof(arg));
            break;
        case OptSnip:
            snipMode(atoi(arg));
            cleanup_and_exit(0);
            break;
#ifndef _WIN32
        case OptPromFile:
            Modes.prom_file = strdup(arg);
            break;
        case OptJsonDir:
            Modes.json_dir = strdup(arg);
            break;
        case OptHeatmap:
            Modes.heatmap = 1;
            if (atof(arg) > 0)
                Modes.heatmap_interval = 1000 * atof(arg);
            break;
        case OptHeatmapDir:
            Modes.heatmap_dir = strdup(arg);
            break;
        case OptGlobeHistoryDir:
            Modes.globe_history_dir = strdup(arg);
            if (!Modes.state_dir) {
                Modes.state_dir = malloc(PATH_MAX);
                snprintf(Modes.state_dir, PATH_MAX, "%s/internal_state", Modes.globe_history_dir);
            }
            break;
        case OptStateDir:
            if (Modes.state_dir)
                free(Modes.state_dir);
            Modes.state_dir = strdup(arg);
            break;
        case OptJsonTime:
            Modes.json_interval = (uint64_t) (1000 * atof(arg));
            if (Modes.json_interval < 100) // 0.1s
                Modes.json_interval = 100;
            break;
        case OptJsonLocAcc:
            Modes.json_location_accuracy = atoi(arg);
            break;
        case OptJsonReliable:
            Modes.json_reliable = atoi(arg);
            if (Modes.json_reliable < -1)
                Modes.json_reliable = -1;
            if (Modes.json_reliable > 4)
                Modes.json_reliable = 4;
            break;
        case OptDbFile:
            free(Modes.db_file);
            Modes.db_file = strdup(arg);
            break;
        case OptJsonGzip:
            Modes.json_gzip = 1;
            break;
        case OptJsonBinCraft:
            Modes.jsonBinCraft = atoi(arg);
            break;
        case OptJsonTraceInt:
            if (atof(arg) > 0)
                Modes.json_trace_interval = 1000 * atof(arg);
            break;
        case OptJsonGlobeIndex:
            Modes.json_globe_index = 1;
            break;
#endif
        case OptNetHeartbeat:
            Modes.net_heartbeat_interval = (uint64_t) (1000 * atof(arg));
            break;
        case OptNetRoSize:
            Modes.net_output_flush_size = atoi(arg);
            break;
        case OptNetRoRate:
            Modes.net_output_flush_interval = 1000 * atoi(arg) / 15; // backwards compatibility
            break;
        case OptNetRoIntervall:
            Modes.net_output_flush_interval = (uint64_t) (1000 * atof(arg));
            break;
        case OptNetRoPorts:
            free(Modes.net_output_raw_ports);
            Modes.net_output_raw_ports = strdup(arg);
            break;
        case OptNetRiPorts:
            free(Modes.net_input_raw_ports);
            Modes.net_input_raw_ports = strdup(arg);
            break;
        case OptNetBoPorts:
            free(Modes.net_output_beast_ports);
            Modes.net_output_beast_ports = strdup(arg);
            break;
        case OptNetBiPorts:
            free(Modes.net_input_beast_ports);
            Modes.net_input_beast_ports = strdup(arg);
            break;
        case OptNetBeastReducePorts:
            free(Modes.net_output_beast_reduce_ports);
            Modes.net_output_beast_reduce_ports = strdup(arg);
            break;
        case OptNetBeastReduceInterval:
            if (atof(arg) >= 0)
                Modes.net_output_beast_reduce_interval = (uint64_t) (1000 * atof(arg));
            if (Modes.net_output_beast_reduce_interval > 15000)
                Modes.net_output_beast_reduce_interval = 15000;
            break;
        case OptNetBindAddr:
            free(Modes.net_bind_address);
            Modes.net_bind_address = strdup(arg);
            break;
        case OptNetSbsPorts:
            free(Modes.net_output_sbs_ports);
            Modes.net_output_sbs_ports = strdup(arg);
            break;
        case OptNetJsonPorts:
            free(Modes.net_output_json_ports);
            Modes.net_output_json_ports = strdup(arg);
            break;
        case OptNetApiPorts:
            free(Modes.net_output_api_ports);
            Modes.net_output_api_ports = strdup(arg);
            Modes.api = 1;
            break;
        case OptNetSbsInPorts:
            free(Modes.net_input_sbs_ports);
            Modes.net_input_sbs_ports = strdup(arg);
            break;
        case OptNetVRSPorts:
            free(Modes.net_output_vrs_ports);
            Modes.net_output_vrs_ports = strdup(arg);
            break;
        case OptNetVRSInterval:
            if (atof(arg) > 0)
                Modes.net_output_vrs_interval = atof(arg) * SECONDS;
            break;
        case OptNetBuffer:
            Modes.net_sndbuf_size = atoi(arg);
            break;
        case OptNetVerbatim:
            Modes.net_verbatim = 1;
            break;
        case OptNetReceiverId:
            Modes.netReceiverId = 1;
            break;
        case OptNetReceiverIdJson:
            Modes.netReceiverIdJson = 1;
            break;
        case OptGarbage:
            Modes.garbage_ports = strdup(arg);
            break;
        case OptNetIngest:
            Modes.netIngest = 1;
            break;
        case OptUuidFile:
            free(Modes.uuidFile);
            Modes.uuidFile = strdup(arg);
            break;
        case OptNetConnector:
            if (!Modes.net_connectors || Modes.net_connectors_count + 1 > Modes.net_connectors_size) {
                Modes.net_connectors_size = Modes.net_connectors_count * 2 + 8;
                Modes.net_connectors = realloc(Modes.net_connectors,
                        sizeof(struct net_connector *) * Modes.net_connectors_size);
                if (!Modes.net_connectors)
                    return 1;
            }
            struct net_connector *con = calloc(1, sizeof(struct net_connector));
            Modes.net_connectors[Modes.net_connectors_count++] = con;
            char *connect_string = strdup(arg);
            con->address = con->address0 = strtok(connect_string, ",");
            con->port = con->port0 = strtok(NULL, ",");
            con->protocol = strtok(NULL, ",");
            con->address1 = strtok(NULL, ",");
            con->port1 = strtok(NULL, ",");

            if (pthread_mutex_init(&con->mutex, NULL)) {
                fprintf(stderr, "Unable to initialize connector mutex!\n");
                exit(1);
            }
            //fprintf(stderr, "%d %s\n", Modes.net_connectors_count, con->protocol);
            if (!con->address || !con->port || !con->protocol) {
                fprintf(stderr, "--net-connector: Wrong format: %s\n", arg);
                fprintf(stderr, "Correct syntax: --net-connector=ip,port,protocol\n");
                return 1;
            }
            if (strcmp(con->protocol, "beast_out") != 0
                    && strcmp(con->protocol, "beast_reduce_out") != 0
                    && strcmp(con->protocol, "beast_in") != 0
                    && strcmp(con->protocol, "raw_out") != 0
                    && strcmp(con->protocol, "raw_in") != 0
                    && strcmp(con->protocol, "vrs_out") != 0
                    && strcmp(con->protocol, "sbs_in") != 0
                    && strcmp(con->protocol, "sbs_in_mlat") != 0
                    && strcmp(con->protocol, "sbs_in_jaero") != 0
                    && strcmp(con->protocol, "sbs_in_prio") != 0
                    && strcmp(con->protocol, "sbs_out") != 0
                    && strcmp(con->protocol, "sbs_out_replay") != 0
                    && strcmp(con->protocol, "sbs_out_mlat") != 0
                    && strcmp(con->protocol, "sbs_out_jaero") != 0
                    && strcmp(con->protocol, "sbs_out_prio") != 0
                    && strcmp(con->protocol, "json_out") != 0
               ) {
                fprintf(stderr, "--net-connector: Unknown protocol: %s\n", con->protocol);
                fprintf(stderr, "Supported protocols: beast_out, beast_in, beast_reduce_out, raw_out, raw_in, \n"
                        "sbs_out, sbs_out_replay, sbs_out_mlat, sbs_out_jaero, \n"
                        "sbs_in, sbs_in_mlat, sbs_in_jaero, \n"
                        "vrs_out, json_out\n");
                return 1;
            }
            if (strcmp(con->address, "") == 0 || strcmp(con->address, "") == 0) {
                fprintf(stderr, "--net-connector: ip and port can't be empty!\n");
                fprintf(stderr, "Correct syntax: --net-connector=ip,port,protocol\n");
                return 1;
            }
            if (atol(con->port) > (1<<16) || atol(con->port) < 1) {
                fprintf(stderr, "--net-connector: port must be in range 1 to 65536\n");
                return 1;
            }
            break;
        case OptNetConnectorDelay:
            Modes.net_connector_delay = (uint64_t) 1000 * atof(arg);
            break;

        case OptCprFocus:
            Modes.cpr_focus = strtol(arg, NULL, 16);
            fprintf(stderr, "cpr_focus = %06x\n", Modes.cpr_focus);
            break;
        case OptReceiverFocus:
            Modes.receiver_focus = strtoull(arg, NULL, 16);
            fprintf(stderr, "receiver_focus = %016"PRIx64"\n", Modes.receiver_focus);
            break;

        case OptDebug:
            while (*arg) {
                switch (*arg) {
                    case 'D': Modes.debug |= MODES_DEBUG_DEMOD;
                        break;
                    case 'd': Modes.debug |= MODES_DEBUG_DEMODERR;
                        break;
                    case 'C': Modes.debug |= MODES_DEBUG_GOODCRC;
                        break;
                    case 'c': Modes.debug |= MODES_DEBUG_BADCRC;
                        break;
                    case 'p': Modes.debug |= MODES_DEBUG_NOPREAMBLE;
                        break;
                    case 'n': Modes.debug |= MODES_DEBUG_NET;
                        break;
                    case 'P': Modes.debug_cpr = 1;
                        break;
                    case 'R': Modes.debug_receiver = 1;
                        break;
                    case 'S': Modes.debug_speed_check = 1;
                        break;
                    case 'G': Modes.debug_garbage = 1;
                        break;
                    case 'T': Modes.debug_traceCount = 1;
                        break;
                    case 'K': Modes.debug_sampleCounter = 1;
                        break;
                    case 'j': Modes.debug |= MODES_DEBUG_JS;
                        break;
                    case 'O': Modes.debug_rough_receiver_location = 1;
                        break;
                    case 'U': Modes.debug_dbJson = 1;
                        break;
                    default:
                        fprintf(stderr, "Unknown debugging flag: %c\n", *arg);
                        return 1;
                        break;
                }
                arg++;
            }
            break;
#ifdef ENABLE_RTLSDR
        case OptRtlSdrEnableAgc:
        case OptRtlSdrPpm:
#endif
        case OptBeastSerial:
        case OptBeastDF1117:
        case OptBeastDF045:
        case OptBeastMlatTimeOff:
        case OptBeastCrcOff:
        case OptBeastFecOff:
        case OptBeastModeAc:
        case OptIfileName:
        case OptIfileFormat:
        case OptIfileThrottle:
#ifdef ENABLE_BLADERF
        case OptBladeFpgaDir:
        case OptBladeDecim:
        case OptBladeBw:
#endif
#ifdef ENABLE_PLUTOSDR
        case OptPlutoUri:
        case OptPlutoNetwork:
#endif
        case OptDeviceType:
            /* Forward interface option to the specific device handler */
            if (sdrHandleOption(key, arg) == false)
                return 1;
            break;
        case ARGP_KEY_END:
            if (state->arg_num > 0)
                /* We use only options but no arguments */
                argp_usage(state);
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

//
//=========================================================================
//

int main(int argc, char **argv) {
    srandom(get_seed());

    int j;

    // Set sane defaults
    modesInitConfig();

    // signal handlers:
    signal(SIGINT, sigintHandler);
    signal(SIGTERM, sigtermHandler);
    signal(SIGUSR1, SIG_IGN);

    // Parse the command line options
    if (argp_parse(&argp, argc, argv, ARGP_NO_EXIT, 0, 0)) {
        fprintf(stderr, "Command line used:\n");
        for (int i = 0; i < argc; i++) {
            fprintf(stderr, "%s ", argv[i]);
        }
        fprintf(stderr, "\n");
        cleanup_and_exit(1);
    }
    if (argc >= 2 && (
                !strcmp(argv[1], "--help")
                || !strcmp(argv[1], "--usage")
                || !strcmp(argv[1], "--version")
                || !strcmp(argv[1], "-V")
                || !strcmp(argv[1], "-?")
                )
       ) {
        exit(0);
    }


#ifdef _WIN32
    // Try to comply with the Copyright license conditions for binary distribution
    if (!Modes.quiet) {
        showCopyright();
    }
#endif

    // Initialization
    log_with_timestamp("%s starting up.", MODES_READSB_VARIANT);
    fprintf(stderr, VERSION_STRING"\n");
    //fprintf(stderr, "%zu\n", sizeof(struct state_flags));
    fprintf(stderr, "struct sizes: %zu, ", sizeof(struct aircraft));
    fprintf(stderr, "%zu, ", sizeof(struct state));
    fprintf(stderr, "%zu, ", sizeof(struct state_all));
    fprintf(stderr, "%zu\n", sizeof(struct binCraft));
    //fprintf(stderr, "%zu\n", sizeof(struct modesMessage));
    //fprintf(stderr, "%zu\n", sizeof(pthread_mutex_t));
    //fprintf(stderr, "%zu\n", 10000 * sizeof(struct aircraft));
    modesInit();

    if (Modes.sdr_type != SDR_NONE && !sdrOpen()) {
        cleanup_and_exit(1);
    }

    if (Modes.net) {
        modesInitNet();
    }

    // init stats:
    Modes.stats_current.start = Modes.stats_current.end =
            Modes.stats_alltime.start = Modes.stats_alltime.end =
            Modes.stats_periodic.start = Modes.stats_periodic.end =
            Modes.stats_1min.start = Modes.stats_1min.end =
            Modes.stats_5min.start = Modes.stats_5min.end =
            Modes.stats_15min.start = Modes.stats_15min.end = mstime();

    for (j = 0; j < STAT_BUCKETS; ++j)
        Modes.stats_10[j].start = Modes.stats_10[j].end = Modes.stats_current.start;

    interactiveInit();

    if (Modes.heatmap) {
        if (!Modes.globe_history_dir && !Modes.heatmap_dir) {
            fprintf(stderr, "Heatmap requires globe history dir or heatmap dir to be set, disabling heatmap!\n");
            Modes.heatmap = 0;
        }
    }

    if (Modes.json_globe_index) {
        Modes.keep_traces = 24 * HOURS + 40 * MINUTES; // include 40 minutes overlap, tar1090 needs at least 30 minutes currently
    } else if (Modes.heatmap) {
        Modes.keep_traces = 35 * MINUTES; // heatmap is written every 30 minutes
    }

    if (Modes.json_dir && Modes.json_globe_index) {
        char pathbuf[PATH_MAX];
        snprintf(pathbuf, PATH_MAX, "%s/traces", Modes.json_dir);
        mkdir(pathbuf, 0755);
        for (int i = 0; i < 256; i++) {
            snprintf(pathbuf, PATH_MAX, "%s/traces/%02x", Modes.json_dir, i);
            mkdir(pathbuf, 0755);
        }
    }

    if (Modes.state_dir) {
        fprintf(stderr, "loading state .....\n");
        pthread_t threads[IO_THREADS];
        int numbers[IO_THREADS];
        for (int i = 0; i < IO_THREADS; i++) {
            numbers[i] = i;
            pthread_create(&threads[i], NULL, load_state, &numbers[i]);
        }
        for (int i = 0; i < IO_THREADS; i++) {
            pthread_join(threads[i], NULL);
        }
        for (int i = 0; i < IO_THREADS; i++) {
            numbers[i] = i;
            pthread_create(&threads[i], NULL, load_blobs, &numbers[i]);
        }
        for (int i = 0; i < IO_THREADS; i++) {
            pthread_join(threads[i], NULL);
        }
        uint32_t count_ac = 0;
        uint64_t now = mstime();
        for (int j = 0; j < AIRCRAFT_BUCKETS; j++) {
            for (struct aircraft *a = Modes.aircraft[j]; a; a = a->next) {
                int new_index = a->globe_index;
                a->globe_index = -5;
                set_globe_index(a, new_index);
                count_ac++;
                updateValidities(a, now);
            }
        }
        fprintf(stderr, " .......... done, loaded %u aircraft!\n", count_ac);
        Modes.aircraftCount = count_ac;
        fprintf(stderr, "aircraft table fill: %0.1f\n", Modes.aircraftCount / (double) AIRCRAFT_BUCKETS );

        char pathbuf[PATH_MAX];
        if (mkdir(Modes.globe_history_dir, 0755) && errno != EEXIST)
            perror(Modes.globe_history_dir);

        if (mkdir(Modes.state_dir, 0755) && errno != EEXIST)
            perror(pathbuf);
    }
    // db update on startup
    dbUpdate();
    dbFinishUpdate();

    if (Modes.json_dir) {
        // write initial json files so they're not missing
        writeJsonToFile(Modes.json_dir, "receiver.json", generateReceiverJson());
        //writeJsonToFile(Modes.json_dir, "stats.json", generateStatsJson()); // rather don't do this.
        writeJsonToFile(Modes.json_dir, "aircraft.json", generateAircraftJson());
    }

    // go over the aircraft list once and do other stuff before starting the threads.
    trackPeriodicUpdate();

    pthread_create(&Modes.decodeThread, NULL, decodeThreadEntryPoint, NULL);

    if (Modes.json_dir) {

        pthread_create(&Modes.jsonThread, NULL, jsonThreadEntryPoint, NULL);

        if (Modes.json_globe_index) {
            // globe_xxxx.json
            pthread_create(&Modes.jsonGlobeThread, NULL, jsonGlobeThreadEntryPoint, NULL);

            // trace_xxxxxxxxx.json
            for (int i = 0; i < TRACE_THREADS; i++) {
                pthread_create(&Modes.jsonTraceThread[i], NULL, jsonTraceThreadEntryPoint, &Modes.threadNumber[i]);
            }
        }
    }

    pthread_mutex_lock(&Modes.mainThreadMutex);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    while (!Modes.exit) {
        trackPeriodicUpdate();

        int64_t sleep_ms = 1000;
        incTimedwait(&ts, sleep_ms);

        int res = 0;
        while (!Modes.exit && res == 0) {
            res = pthread_cond_timedwait(&Modes.mainThreadCond, &Modes.mainThreadMutex, &ts);
        }
    }

    pthread_mutex_unlock(&Modes.mainThreadMutex);

    if (Modes.json_dir) {

        pthread_join(Modes.jsonThread, NULL); // Wait on json writer thread exit

        char pathbuf[PATH_MAX];
        snprintf(pathbuf, PATH_MAX, "%s/receiver.json", Modes.json_dir);
        unlink(pathbuf);

        if (Modes.json_globe_index) {
            pthread_join(Modes.jsonGlobeThread, NULL); // Wait on json writer thread exit

            for (int i = 0; i < TRACE_THREADS; i++) {
                pthread_join(Modes.jsonTraceThread[i], NULL); // Wait on json writer thread exit
            }
        }
    }

    pthread_join(Modes.decodeThread, NULL); // Wait on json writer thread exit

    /* Cleanup network setup */
    cleanupNetwork();

    if (Modes.state_dir) {
        fprintf(stderr, "saving state .....\n");

        pthread_t threads[IO_THREADS];
        int numbers[IO_THREADS];
        for (int i = 0; i < IO_THREADS; i++) {
            numbers[i] = i;
            pthread_create(&threads[i], NULL, save_state, &numbers[i]);
        }
        for (int i = 0; i < IO_THREADS; i++) {
            pthread_join(threads[i], NULL);
        }
        fprintf(stderr, "............. done!\n");
    }


    pthread_mutex_destroy(&Modes.decodeThreadMutex);
    pthread_mutex_destroy(&Modes.jsonThreadMutex);
    pthread_mutex_destroy(&Modes.jsonGlobeThreadMutex);
    pthread_cond_destroy(&Modes.decodeThreadCond);
    pthread_cond_destroy(&Modes.jsonThreadCond);
    pthread_cond_destroy(&Modes.jsonGlobeThreadCond);
    for (int i = 0; i < TRACE_THREADS; i++) {
        pthread_mutex_destroy(&Modes.jsonTraceThreadMutex[i]);
        pthread_cond_destroy(&Modes.jsonTraceThreadCond[i]);
    }

    pthread_mutex_destroy(&Modes.mainThreadMutex);
    pthread_cond_destroy(&Modes.mainThreadCond);

    trackForceStats();

    // If --stats were given, print statistics
    if (Modes.stats) {
        display_total_stats();
    }
    if (Modes.exit == SIGKILL) {
        raise(SIGKILL);
    }
    sdrClose();
    if (Modes.exit != 1) {
        log_with_timestamp("Abnormal exit.");
        cleanup_and_exit(1);
    }

    log_with_timestamp("Normal exit.");
    cleanup_and_exit(0);
}
