/*
 * data_processing.cpp
 * Module 3 — Data Processing: Fuel Consumption & Flight Records
 * Owner: Member 3
 *
 * Compiled as a Static Library linked by the Server project.
 *
 * Optimizations applied (Project 6 load/endurance test findings):
 *
 *   1. std::unordered_map replaces std::map
 *      — O(1) average lookup vs O(log n); eliminates _Find_lower_bound
 *        overhead visible in the load-test CPU profiler.
 *
 *   2. Throttled printf in update_flight_record()
 *      — printf was consuming 70.85 % of total CPU under load (profiler hot
 *        path). Now logs once per LOG_INTERVAL packets instead of every packet.
 *
 *   3. Single-lock update in update_flight_record()
 *      — Was: lock → read initial_fuel → unlock → calculate → lock → write
 *        → unlock  (two lock cycles, two map lookups via find()).
 *      — Now: lock → find iterator → calculate → write → unlock
 *        (one lock cycle, one lookup, iterator reused for write).
 *        The arithmetic (subtraction + division) is negligible inside the lock.
 *
 *   4. localtime_s() in finalize_flight()
 *      — localtime() returns a pointer to a shared static buffer — not
 *        thread-safe when multiple threads finalize flights simultaneously.
 *        localtime_s() writes into a caller-supplied struct tm.
 *
 *   5. file_mutex protecting CSV writes
 *      — Multiple threads can call finalize_flight() simultaneously.
 *        file_mutex ensures only one thread writes to the CSV at a time,
 *        preventing interleaved output or duplicate headers.
 *
 *   6. Removed redundant fseek() before ftell()
 *      — Files opened in append mode ("a") already position at EOF;
 *        the fseek(f, 0, SEEK_END) was a no-op.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unordered_map>   /* O(1) avg lookup — replaces std::map */

#include <pthread.h>

#include "data_processing.h"
#include "../shared/telemetry_packet.h"
#include "../shared/flight_record.h"

/* ── Extern mutex — defined in Server/server_core.cpp ───────────────────── */
extern pthread_mutex_t records_mutex;

/* ── File mutex — serialises CSV writes across concurrent finalize calls ── */
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Shared in-memory flight records (O(1) average lookup) ─────────────── */
static std::unordered_map<uint32_t, FlightRecord> flight_records;

/* ── Output CSV filename ────────────────────────────────────────────────── */
#define OUTPUT_FILE     "flight_records.csv"

/*
 * LOG_INTERVAL — print a status line every N packets per aircraft.
 * At 1 packet per telemetry line and ~400 lines per file, setting this to
 * 100 gives ~4 log lines per flight instead of 400.
 * Raise or lower to taste; 0 disables per-packet logging entirely.
 */
#define LOG_INTERVAL    100

/* ══════════════════════════════════════════════════════════════════════════
 * init_flight_record()
 * Called once per aircraft on its very first packet.
 * ══════════════════════════════════════════════════════════════════════════ */
void init_flight_record(TelemetryPacket* pkt) {

    FlightRecord record;
    memset(&record, 0, sizeof(FlightRecord));

    record.aircraft_id     = pkt->aircraft_id;
    record.initial_fuel    = pkt->fuel_remaining;
    record.latest_fuel     = pkt->fuel_remaining;
    record.latest_elapsed  = pkt->elapsed_time_sec;
    record.avg_consumption = 0.0f;
    record.packet_count    = 1;
    record.session_start   = time(NULL);

    pthread_mutex_lock(&records_mutex);
    flight_records[pkt->aircraft_id] = record;
    pthread_mutex_unlock(&records_mutex);

    printf("[DataProcessing] Aircraft %u connected | "
           "Initial fuel: %.1f gal\n",
           pkt->aircraft_id, pkt->fuel_remaining);
}

/* ══════════════════════════════════════════════════════════════════════════
 * update_flight_record()
 *
 * Optimized vs original:
 *   - One lock section instead of two (was: read lock → unlock → write lock)
 *   - One map lookup via iterator instead of two find() calls
 *   - Throttled printf: logs every LOG_INTERVAL packets, not every packet
 * ══════════════════════════════════════════════════════════════════════════ */
void update_flight_record(TelemetryPacket* pkt) {

    float avg   = 0.0f;
    int   count = 0;

    pthread_mutex_lock(&records_mutex);

    auto it = flight_records.find(pkt->aircraft_id);
    if (it == flight_records.end()) {
        pthread_mutex_unlock(&records_mutex);
        return; /* safety: no record for this ID */
    }

    FlightRecord& rec = it->second;

    /*
     * Calculate inside the lock — the arithmetic is trivially fast and
     * holding the lock slightly longer is far cheaper than a second
     * lock/unlock cycle plus a second map lookup.
     */
    float fuel_consumed = rec.initial_fuel - pkt->fuel_remaining;
    if (pkt->elapsed_time_sec > 0.0f) {
        avg = (fuel_consumed / pkt->elapsed_time_sec) * 3600.0f; /* gal/hr */
    }

    rec.latest_fuel     = pkt->fuel_remaining;
    rec.latest_elapsed  = pkt->elapsed_time_sec;
    rec.avg_consumption = avg;
    rec.packet_count   += 1;
    count               = rec.packet_count;

    pthread_mutex_unlock(&records_mutex);

    /*
     * Throttled logging — was 70.85 % of CPU under 1000-client load test.
     * Printing every LOG_INTERVAL packets cuts console I/O by ~99 %.
     */
    if (LOG_INTERVAL > 0 && count % LOG_INTERVAL == 0) {
        printf("[DataProcessing] Aircraft %-10u | "
               "Fuel: %8.1f gal | "
               "Avg: %.1f gal/hr | "
               "Pkts: %d\n",
               pkt->aircraft_id,
               pkt->fuel_remaining,
               avg,
               count);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * finalize_flight()
 *
 * Optimized vs original:
 *   - Uses iterator directly (avoids separate find() + erase(key) lookups)
 *   - localtime_s() instead of localtime() — thread-safe timestamp
 *   - file_mutex prevents concurrent CSV writes / duplicate headers
 *   - Removed redundant fseek(f, 0, SEEK_END) before ftell()
 * ══════════════════════════════════════════════════════════════════════════ */
void finalize_flight(uint32_t aircraft_id) {

    /* ── Retrieve and erase under lock ── */
    pthread_mutex_lock(&records_mutex);
    auto it = flight_records.find(aircraft_id);
    if (it == flight_records.end()) {
        pthread_mutex_unlock(&records_mutex);
        return;
    }
    FlightRecord rec = it->second; /* copy before erasing */
    flight_records.erase(it);      /* erase by iterator — one lookup total */
    pthread_mutex_unlock(&records_mutex);

    /* ── Calculate flight duration ── */
    double duration = difftime(time(NULL), rec.session_start);

    /* ── Thread-safe timestamp (localtime_s writes into caller's struct tm) ── */
    char     timestamp[32];
    struct tm tm_buf;
    localtime_s(&tm_buf, &rec.session_start);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_buf);

    /* ── Console summary ── */
    printf("\n[DataProcessing] ++-------- Flight Complete --------++\n");
    printf("[DataProcessing]   Aircraft ID   : %u\n",          rec.aircraft_id);
    printf("[DataProcessing]   Avg Fuel Burn : %.1f gal/hr\n", rec.avg_consumption);
    printf("[DataProcessing]   Duration      : %.0f sec\n",    duration);
    printf("[DataProcessing]   Packets recvd : %d\n",          rec.packet_count);
    printf("[DataProcessing] ++-----------------------------------++\n");

    /* ── CSV append — serialised by file_mutex ── */
    pthread_mutex_lock(&file_mutex);

    FILE* f = fopen(OUTPUT_FILE, "a");
    if (f) {
        /*
         * ftell() on a file opened with "a" returns the current file size.
         * Write the header only when the file is brand-new (size == 0).
         * The redundant fseek(f, 0, SEEK_END) from the original code is
         * removed — append mode already positions at EOF on open.
         */
        if (ftell(f) == 0) {
            fprintf(f, "aircraft_id,avg_consumption_gal_hr,"
                       "flight_duration_sec,packets_received,timestamp\n");
        }
        fprintf(f, "%u,%.2f,%.0f,%d,%s\n",
                rec.aircraft_id,
                rec.avg_consumption,
                duration,
                rec.packet_count,
                timestamp);
        fclose(f);
    }

    pthread_mutex_unlock(&file_mutex);
}