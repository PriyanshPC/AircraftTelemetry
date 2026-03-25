#ifndef DATA_PROCESSING_H
#define DATA_PROCESSING_H

/*
 * data_processing.h
 * Module 3 — Data Processing: Fuel Consumption & Flight Records
 * Owner: Member 3
 *
 * Compiled as a Static Library (.lib) linked by the Server project.
 * Public API called by handle_client() in server_core.cpp.
 */

#include <stdint.h>
#include "../shared/telemetry_packet.h"
#include "../shared/flight_record.h"

 /*
  * init_flight_record()
  * Called on the FIRST packet received from a new aircraft.
  * Creates a new FlightRecord entry in the shared map.
  */
void init_flight_record(TelemetryPacket* pkt);

/*
 * update_flight_record()
 * Called on every subsequent packet from an active aircraft.
 * Recalculates running average fuel consumption and updates the record.
 */
void update_flight_record(TelemetryPacket* pkt);

/*
 * finalize_flight()
 * Called when a client disconnects cleanly (recv returns 0).
 * Computes final average, writes to flight_records.csv, removes from map.
 */
void finalize_flight(uint32_t aircraft_id);

#endif /* DATA_PROCESSING_H */