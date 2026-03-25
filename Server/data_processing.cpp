/*
 * data_processing.cpp
 * Module 3 — Data Processing: Fuel Consumption & Flight Records
 * Owner: Member 3
 *
 * Compiled as a Static Library linked by the Server project.
 *
 * Threading note:
 *   records_mutex is DEFINED in server_core.cpp (Server project).
 *   It is declared extern here so this library can use it.
 *   Lock it BEFORE every read/write to the flight_records map.
 *   Do all calculations BEFORE acquiring the lock to keep
 *   the critical section as short as possible.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <map>

#include <pthread.h>

#include "data_processing.h"
#include "../shared/telemetry_packet.h"
#include "../shared/flight_record.h"

 /* ── Extern mutex — defined in Server/server_core.cpp ───────────────────── */
extern pthread_mutex_t records_mutex;

/* ── Shared in-memory flight records map ────────────────────────────────── */
static std::map<uint32_t, FlightRecord> flight_records;

/* ── Output CSV filename ────────────────────────────────────────────────── */
#define OUTPUT_FILE "flight_records.csv"

/* ══════════════════════════════════════════════════════════════════════════
 * init_flight_record()
 * ══════════════════════════════════════════════════════════════════════════ */
void init_flight_record(TelemetryPacket* pkt) {

    FlightRecord record;
    memset(&record, 0, sizeof(FlightRecord));

    record.aircraft_id = pkt->aircraft_id;
    record.initial_fuel = pkt->fuel_remaining;
    record.latest_fuel = pkt->fuel_remaining;
    record.latest_elapsed = pkt->elapsed_time_sec;
    record.avg_consumption = 0.0f;
    record.packet_count = 1;
    record.session_start = time(NULL);

    pthread_mutex_lock(&records_mutex);
    flight_records[pkt->aircraft_id] = record;
    pthread_mutex_unlock(&records_mutex);

    printf("[DataProcessing] Aircraft %u connected | "
        "Initial fuel: %.1f gal\n",
        pkt->aircraft_id, pkt->fuel_remaining);
}

/* ══════════════════════════════════════════════════════════════════════════
 * update_flight_record()
 * ══════════════════════════════════════════════════════════════════════════ */
void update_flight_record(TelemetryPacket* pkt) {

    /* ── Do math BEFORE locking ── */
    float initial_fuel = 0.0f;

    pthread_mutex_lock(&records_mutex);
    if (flight_records.find(pkt->aircraft_id) == flight_records.end()) {
        pthread_mutex_unlock(&records_mutex);
        return; /* safety check */
    }
    initial_fuel = flight_records[pkt->aircraft_id].initial_fuel;
    pthread_mutex_unlock(&records_mutex);

    /* ── Calculate outside the lock ── */
    float fuel_consumed = initial_fuel - pkt->fuel_remaining;
    float avg = 0.0f;
    if (pkt->elapsed_time_sec > 0.0f) {
        avg = (fuel_consumed / pkt->elapsed_time_sec) * 3600.0f; /* gal/hr */
    }

    /* ── Lock only for the write ── */
    pthread_mutex_lock(&records_mutex);
    if (flight_records.find(pkt->aircraft_id) != flight_records.end()) {
        FlightRecord& rec = flight_records[pkt->aircraft_id];
        rec.latest_fuel = pkt->fuel_remaining;
        rec.latest_elapsed = pkt->elapsed_time_sec;
        rec.avg_consumption = avg;
        rec.packet_count += 1;
    }
    pthread_mutex_unlock(&records_mutex);

    printf("[DataProcessing] Aircraft %-10u | "
        "Fuel: %8.1f gal | "
        "Avg: %.1f gal/hr\n",
        pkt->aircraft_id,
        pkt->fuel_remaining,
        avg);
}

/* ══════════════════════════════════════════════════════════════════════════
 * finalize_flight()
 * ══════════════════════════════════════════════════════════════════════════ */
void finalize_flight(uint32_t aircraft_id) {

    /* ── Retrieve and remove record under lock ── */
    pthread_mutex_lock(&records_mutex);
    if (flight_records.find(aircraft_id) == flight_records.end()) {
        pthread_mutex_unlock(&records_mutex);
        return;
    }
    FlightRecord rec = flight_records[aircraft_id]; /* copy before erasing */
    flight_records.erase(aircraft_id);
    pthread_mutex_unlock(&records_mutex);

    /* ── Calculate flight duration ── */
    double duration = difftime(time(NULL), rec.session_start);

    /* ── Format timestamp ── */
    char timestamp[32];
    struct tm* t = localtime(&rec.session_start);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t);

    /* ── Print final summary ── */
    printf("\n[DataProcessing] ++-------- Flight Complete --------++\n");
    printf("[DataProcessing]   Aircraft ID   : %u\n", rec.aircraft_id);
    printf("[DataProcessing]   Avg Fuel Burn : %.1f gal/hr\n", rec.avg_consumption);
    printf("[DataProcessing]   Duration      : %.0f sec\n", duration);
    printf("[DataProcessing]   Packets recvd : %d\n", rec.packet_count);
    printf("[DataProcessing] ++-----------------------------------++\n");

    /* ── Append to CSV ── */
    FILE* f = fopen(OUTPUT_FILE, "a");
    if (f) {
        /* Write header if file is empty */
        fseek(f, 0, SEEK_END);
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
}