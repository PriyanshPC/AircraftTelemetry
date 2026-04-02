/*
 * data_processing.cpp
 * Module 3 — Data Processing: Fuel Consumption & Flight Records
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <map>
#include <algorithm>

#include <pthread.h>

#include "data_processing.h"
#include "../shared/telemetry_packet.h"
#include "../shared/flight_record.h"

 /* Extern mutex — defined in server_core.cpp */
extern pthread_mutex_t records_mutex;

/* Shared in-memory flight records map */
static std::map<uint32_t, FlightRecord> flight_records;

/* Output CSV filename */
#define OUTPUT_FILE "flight_records.csv"

void init_flight_record(TelemetryPacket* pkt)
{
    FlightRecord record;
    memset(&record, 0, sizeof(FlightRecord));

    record.aircraft_id = pkt->aircraft_id;
    record.initial_fuel = pkt->fuel_remaining;
    record.latest_fuel = pkt->fuel_remaining;

    record.first_elapsed = pkt->elapsed_time_sec;
    record.latest_elapsed = pkt->elapsed_time_sec;

    record.avg_consumption = 0.0f;
    record.packet_count = 1;
    record.session_start = time(NULL);

    pthread_mutex_lock(&records_mutex);
    flight_records[pkt->aircraft_id] = record;
    pthread_mutex_unlock(&records_mutex);

    printf("[DataProcessing] Aircraft %u connected | Initial fuel: %.1f gal | Start elapsed: %.0f sec\n",
        pkt->aircraft_id,
        pkt->fuel_remaining,
        pkt->elapsed_time_sec);
}

void update_flight_record(TelemetryPacket* pkt)
{
    float initial_fuel = 0.0f;

    pthread_mutex_lock(&records_mutex);
    auto it = flight_records.find(pkt->aircraft_id);
    if (it == flight_records.end()) {
        pthread_mutex_unlock(&records_mutex);
        return;
    }

    initial_fuel = it->second.initial_fuel;
    pthread_mutex_unlock(&records_mutex);

    float fuel_consumed = initial_fuel - pkt->fuel_remaining;
    float avg = 0.0f;

    if (pkt->elapsed_time_sec > 0.0f) {
        avg = (fuel_consumed / pkt->elapsed_time_sec) * 3600.0f; /* gal/hr */
    }

    pthread_mutex_lock(&records_mutex);
    it = flight_records.find(pkt->aircraft_id);
    if (it != flight_records.end()) {
        FlightRecord& rec = it->second;

        rec.latest_fuel = pkt->fuel_remaining;

        /* keep the smallest first timestamp and latest current timestamp */
        if (rec.packet_count == 1) {
            rec.first_elapsed = pkt->elapsed_time_sec < rec.first_elapsed
                ? pkt->elapsed_time_sec
                : rec.first_elapsed;
        }
        else {
            rec.first_elapsed = std::min(rec.first_elapsed, pkt->elapsed_time_sec);
        }

        rec.latest_elapsed = pkt->elapsed_time_sec;
        rec.avg_consumption = avg;
        rec.packet_count += 1;
    }
    pthread_mutex_unlock(&records_mutex);

    printf("[DataProcessing] Aircraft %-10u | Fuel: %8.1f gal | Avg: %.1f gal/hr\n",
        pkt->aircraft_id,
        pkt->fuel_remaining,
        avg);
}

void finalize_flight(uint32_t aircraft_id)
{
    pthread_mutex_lock(&records_mutex);
    auto it = flight_records.find(aircraft_id);
    if (it == flight_records.end()) {
        pthread_mutex_unlock(&records_mutex);
        return;
    }

    FlightRecord rec = it->second; /* copy before erase */
    flight_records.erase(it);
    pthread_mutex_unlock(&records_mutex);

    /* Use telemetry timestamps, not wall-clock receive time */
    double telemetry_duration = rec.latest_elapsed - rec.first_elapsed;
    if (telemetry_duration < 0.0) {
        telemetry_duration = 0.0;
    }

    char timestamp[32];
    struct tm* t = localtime(&rec.session_start);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t);

    printf("\n[DataProcessing] ++-------- Flight Complete --------++\n");
    printf("[DataProcessing] Aircraft ID    : %u\n", rec.aircraft_id);
    printf("[DataProcessing] Avg Fuel Burn  : %.1f gal/hr\n", rec.avg_consumption);
    printf("[DataProcessing] Duration       : %.0f sec\n", telemetry_duration);
    printf("[DataProcessing] Packets recvd  : %d\n", rec.packet_count);
    printf("[DataProcessing] ++-----------------------------------++\n");

    FILE* f = fopen(OUTPUT_FILE, "a");
    if (f) {
        fseek(f, 0, SEEK_END);
        if (ftell(f) == 0) {
            fprintf(f, "aircraft_id,avg_consumption_gal_hr,flight_duration_sec,packets_received,timestamp\n");
        }

        fprintf(f, "%u,%.2f,%.0f,%d,%s\n",
            rec.aircraft_id,
            rec.avg_consumption,
            telemetry_duration,
            rec.packet_count,
            timestamp);

        fclose(f);
    }
}