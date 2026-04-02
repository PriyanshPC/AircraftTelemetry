#ifndef FLIGHT_RECORD_H
#define FLIGHT_RECORD_H

#include <stdint.h>
#include <time.h>

/*
 * FlightRecord
 * ------------
 * Per-aircraft state maintained in server memory during an active flight.
 * Stored in a shared std::map keyed by aircraft_id.
 * ALL access to this map must be protected by records_mutex (server_core.cpp).
 */
typedef struct {
    uint32_t aircraft_id;       /* Unique aircraft identifier */
    float initial_fuel;         /* Fuel reading captured from the very first packet */
    float latest_fuel;          /* Most recently received fuel value */

    float first_elapsed;        /* First telemetry elapsed time seen for this flight */
    float latest_elapsed;       /* Most recently received elapsed time (seconds) */

    float avg_consumption;      /* Running average fuel consumption (gallons/hour) */
    int packet_count;           /* Total number of packets received this flight */

    time_t session_start;       /* Wall-clock time when the first packet arrived */
} FlightRecord;

#endif /* FLIGHT_RECORD_H */