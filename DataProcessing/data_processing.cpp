/**
 * @file data_processing.cpp
 * @brief Implementation of Module 3 — Data Processing (Fuel Consumption & Flight Records).
 *
 * @details
 * Compiled as a **static library** (.lib) that is linked into the Server project.
 * Provides the three public lifecycle functions declared in @c data_processing.h:
 *  - @c init_flight_record()   — called on the first packet from each aircraft
 *  - @c update_flight_record() — called on every subsequent packet
 *  - @c finalize_flight()      — called on clean client disconnect
 *
 * **Optimizations applied (Project 6 — load/endurance test findings):**
 *
 * 1. **std::unordered_map replaces std::map**
 *    O(1) average lookup vs O(log n); eliminates the
 *    @c std::_Tree::_Find_lower_bound overhead that appeared in the
 *    load-test CPU profiler at 1.15% of total CPU.
 *
 * 2. **Throttled printf in update_flight_record()**
 *    Under a 1,000-client load test, @c printf() consumed 70.85% of total
 *    CPU (confirmed by @c _vfprintf_l at 70.64% in the profiler hot path).
 *    Now logs once every @c LOG_INTERVAL packets, reducing console I/O by ~99%.
 *
 * 3. **Single-lock update in update_flight_record()**
 *    Original: lock → read @c initial_fuel → unlock → calculate → lock →
 *    write → unlock (two lock/unlock cycles, two @c find() calls).
 *    Optimized: lock → find iterator → calculate inside lock → write →
 *    unlock (one cycle, one lookup; arithmetic cost inside the lock is
 *    negligible compared to a second lock round-trip).
 *
 * 4. **localtime_s() in finalize_flight()**
 *    @c localtime() returns a pointer to a shared static buffer — calling it
 *    from two threads simultaneously corrupts both timestamps.
 *    @c localtime_s() writes into a caller-supplied @c struct @c tm, making
 *    it fully thread-safe.
 *
 * 5. **file_mutex protecting CSV writes**
 *    Multiple worker threads may call @c finalize_flight() simultaneously
 *    (one per disconnecting aircraft).  @c file_mutex ensures only one
 *    thread writes to the CSV at a time, preventing interleaved rows and
 *    duplicate headers.
 *
 * 6. **Removed redundant fseek() before ftell()**
 *    Files opened in append mode (@c "a") are already positioned at EOF on
 *    open; the original @c fseek(f, 0, SEEK_END) was a no-op.
 *
 * @ingroup DataProcessing
 *
 * @author  Group 7 — Member 3
 * @date    2025
 * @version 2.0  (Project 6 — optimized)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unordered_map>   /**< O(1) average lookup — replaces std::map */

#include <pthread.h>

#include "data_processing.h"
#include "../shared/telemetry_packet.h"
#include "../shared/flight_record.h"

/** @brief Global mutex protecting @c flight_records.
 *         Defined in @c server_core.cpp; referenced here via @c extern.
 *         Must be held whenever @c flight_records is read or modified. */
extern pthread_mutex_t records_mutex;

/** @brief Mutex that serialises concurrent CSV appends in @c finalize_flight().
 *
 *  @details
 *  Multiple worker threads may call @c finalize_flight() at the same time
 *  (one per disconnecting aircraft).  This mutex guarantees that only one
 *  thread opens, writes to, and closes the CSV file at a time, preventing
 *  interleaved rows and duplicate header lines.
 *
 *  Initialised statically so no explicit @c pthread_mutex_init() call is
 *  required at startup. */
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

/** @brief In-memory store of all active flight sessions, keyed by aircraft_id.
 *
 *  @details
 *  Uses @c std::unordered_map for O(1) average-case lookup, replacing the
 *  original @c std::map which gave O(log n) and produced visible profiler
 *  overhead (@c std::_Tree::_Find_lower_bound at 1.15% CPU in load test).
 *
 *  All reads and writes must be performed while holding @c records_mutex. */
static std::unordered_map<uint32_t, FlightRecord> flight_records;

/** @brief Path (relative to the server working directory) of the CSV output file. */
#define OUTPUT_FILE     "flight_records.csv"

/**
 * @brief Packet interval between consecutive console log lines per aircraft.
 *
 * @details
 * Setting this to 100 means one status line is printed every 100 packets.
 * With a typical telemetry file of ~400 lines this produces ~4 log lines per
 * flight, compared to 400 before the optimization.
 *
 * Set to @c 0 to disable per-packet logging entirely.
 *
 * **Why this matters:** Under a 1,000-client load test the original
 * every-packet @c printf() consumed 70.85% of total server CPU due to
 * internal I/O locking inside the C runtime (@c _vfprintf_l).
 */
#define LOG_INTERVAL    100

/* ═══════════════════════════════════════════════════════════════════════════
 * FUNCTION IMPLEMENTATIONS
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Initialise a new FlightRecord for an aircraft on its first packet.
 *
 * @details
 * Allocates (on the stack) and zeroes a @c FlightRecord, then populates it:
 *  - @c aircraft_id      ← @c pkt->aircraft_id
 *  - @c initial_fuel     ← @c pkt->fuel_remaining  (baseline for avg calculation)
 *  - @c latest_fuel      ← @c pkt->fuel_remaining
 *  - @c latest_elapsed   ← @c pkt->elapsed_time_sec
 *  - @c avg_consumption  ← 0.0f  (not enough data yet)
 *  - @c packet_count     ← 1
 *  - @c session_start    ← current wall-clock time via @c time(NULL)
 *
 * The record is then inserted into @c flight_records under @c records_mutex.
 *
 * @param[in] pkt  First packet received from this aircraft.  Must not be NULL.
 */
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

/**
 * @brief Update an existing FlightRecord with data from a subsequent packet.
 *
 * @details
 * Performs all work inside a **single lock section** to avoid the double
 * lock/unlock cycle and redundant @c find() call present in the original code:
 *
 *  1. Acquire @c records_mutex.
 *  2. Find the iterator for @c pkt->aircraft_id (one lookup).
 *  3. Compute average fuel consumption inside the lock:
 *     @code
 *       avg = (initial_fuel - pkt->fuel_remaining) / pkt->elapsed_time_sec * 3600.0f;
 *     @endcode
 *  4. Write updated fields back through the iterator (no second @c find()).
 *  5. Capture @c packet_count, then release @c records_mutex.
 *  6. (Outside lock) Print a throttled status line every @c LOG_INTERVAL packets.
 *
 * @param[in] pkt  Subsequent packet from an active aircraft.  Must not be NULL.
 *
 * @note Returns silently if no record exists for @c pkt->aircraft_id
 *       (defensive guard against race conditions on abrupt disconnect).
 */
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
     * Throttled logging — was 70.85% of CPU under 1000-client load test.
     * Printing every LOG_INTERVAL packets cuts console I/O by ~99%.
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

/**
 * @brief Finalise a completed flight session and append its record to CSV.
 *
 * @details
 * Execution steps:
 *  1. **Lock** @c records_mutex, copy the @c FlightRecord for @p aircraft_id,
 *     erase it from the map (single iterator — one lookup), **unlock**.
 *  2. Compute total flight duration: @c difftime(time(NULL), rec.session_start).
 *  3. Format a thread-safe UTC timestamp using @c localtime_s() into a
 *     caller-supplied @c struct @c tm (avoids the shared static buffer
 *     returned by @c localtime()).
 *  4. Print a flight-complete summary to @c stdout.
 *  5. **Lock** @c file_mutex, open @c flight_records.csv in append mode,
 *     write a header row if the file is brand-new (@c ftell() == 0),
 *     append the data row, close the file, **unlock** @c file_mutex.
 *
 * @param[in] aircraft_id  The 32-bit identifier of the aircraft that has
 *                         disconnected cleanly (recv returned 0).
 *
 * @note Returns silently if no record exists for @p aircraft_id (defensive
 *       guard in case an abrupt disconnect races a clean disconnect path).
 *
 * @warning Must be called only after the receive loop for this aircraft has
 *          fully exited; calling it while @c update_flight_record() may still
 *          be executing for the same ID is undefined behaviour.
 */
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
