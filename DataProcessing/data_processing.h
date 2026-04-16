/**
 * @file data_processing.h
 * @brief Public API for Module 3 — Data Processing (Fuel Consumption & Flight Records).
 *
 * @details
 * This header exposes the three lifecycle functions that make up Module 3.
 * The module is compiled as a **static library** (.lib) and linked into the
 * Server project.  All three functions are called exclusively from
 * @c handle_client_socket() in @c server_core.cpp.
 *
 * **Module responsibilities:**
 *  - Maintain an in-memory map of active FlightRecord objects.
 *  - Compute running average fuel consumption (gallons/hour) as packets arrive.
 *  - Persist final flight statistics to @c flight_records.csv on disconnect.
 *
 * **Thread safety:**
 * All map accesses are protected by @c records_mutex (defined in
 * @c server_core.cpp, declared @c extern here via the implementation file).
 * CSV writes are serialised by an additional @c file_mutex defined inside
 * @c data_processing.cpp.  Callers do not need to hold any lock before
 * calling these functions.
 *
 * @ingroup DataProcessing
 *
 * @author  Group 7 — Member 3
 * @date    2025
 * @version 2.0  (Project 6 — optimized)
 */

#ifndef DATA_PROCESSING_H
#define DATA_PROCESSING_H

#include <stdint.h>
#include "../shared/telemetry_packet.h"
#include "../shared/flight_record.h"

/**
 * @brief Initialise a new FlightRecord for an aircraft on its first packet.
 *
 * @details
 * Creates a zeroed @c FlightRecord, populates it with data from @p pkt
 * (using @c pkt->fuel_remaining as the @c initial_fuel baseline), records
 * the current wall-clock time as @c session_start, and inserts it into the
 * shared @c flight_records map under the key @c pkt->aircraft_id.
 *
 * Acquires and releases @c records_mutex internally.
 *
 * @param[in] pkt  Pointer to the first TelemetryPacket received from this
 *                 aircraft.  Must not be @c NULL.  The struct is read but
 *                 not modified.
 *
 * @pre  No entry for @c pkt->aircraft_id exists in @c flight_records.
 * @post A new @c FlightRecord is present in @c flight_records with
 *       @c packet_count == 1 and @c avg_consumption == 0.0f.
 *
 * @note Called exactly once per aircraft session, before any call to
 *       @c update_flight_record() for the same @c aircraft_id.
 */
void init_flight_record(TelemetryPacket* pkt);

/**
 * @brief Update an existing FlightRecord with data from a subsequent packet.
 *
 * @details
 * Looks up the FlightRecord for @c pkt->aircraft_id and recalculates the
 * running average fuel consumption using the formula:
 * @code
 *   avg_gal_hr = (initial_fuel - pkt->fuel_remaining) / pkt->elapsed_time_sec * 3600.0f;
 * @endcode
 * Updates @c latest_fuel, @c latest_elapsed, @c avg_consumption, and
 * @c packet_count under a single lock section (one lock/unlock cycle,
 * one map lookup via iterator).
 *
 * Throttled console logging: prints a status line every @c LOG_INTERVAL
 * packets to avoid the printf I/O bottleneck identified during load testing
 * (70.85% of total CPU at 1,000 concurrent clients).
 *
 * Acquires and releases @c records_mutex internally.
 *
 * @param[in] pkt  Pointer to the incoming TelemetryPacket.  Must not be
 *                 @c NULL.  The struct is read but not modified.
 *
 * @pre  @c init_flight_record() has been called for @c pkt->aircraft_id.
 * @post @c FlightRecord::packet_count is incremented by 1;
 *       @c avg_consumption, @c latest_fuel, and @c latest_elapsed reflect
 *       values from @p pkt.
 *
 * @note If no record is found for @c pkt->aircraft_id the function returns
 *       silently (safety guard against out-of-order disconnect events).
 */
void update_flight_record(TelemetryPacket* pkt);

/**
 * @brief Finalise and persist a completed flight session, then remove it from memory.
 *
 * @details
 * Retrieves the FlightRecord for @p aircraft_id, erases it from the map
 * (both operations under @c records_mutex), computes the total flight
 * duration, formats a thread-safe timestamp via @c localtime_s(), prints a
 * console summary, and appends one CSV row to @c flight_records.csv.
 *
 * CSV writes are serialised by an internal @c file_mutex; if the file is
 * brand-new (size == 0) a header row is written first.
 *
 * @param[in] aircraft_id  The 32-bit identifier of the aircraft that has
 *                         disconnected.
 *
 * @pre  A FlightRecord for @p aircraft_id exists in @c flight_records
 *       (i.e. at least one packet was received).
 * @post The FlightRecord for @p aircraft_id is removed from memory and a
 *       row is appended to @c flight_records.csv.
 *
 * @note If no record exists for @p aircraft_id (e.g. abrupt disconnect
 *       before the first packet) the function returns silently.
 *
 * @warning Must only be called after all calls to @c update_flight_record()
 *          for the same @p aircraft_id have completed (i.e. after the
 *          receive loop exits).
 */
void finalize_flight(uint32_t aircraft_id);

#endif /* DATA_PROCESSING_H */
