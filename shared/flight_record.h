/**
 * @file flight_record.h
 * @brief Per-aircraft in-memory state maintained by the server during an active flight.
 *
 * @details
 * A @c FlightRecord is created in the shared @c flight_records map when the server
 * receives the very first TelemetryPacket from a new aircraft, updated on every
 * subsequent packet, and destroyed (after being written to CSV) when the client
 * disconnects cleanly.
 *
 * **Thread safety:** All access to the @c flight_records map that holds these
 * records is serialised by @c records_mutex (defined in @c server_core.cpp).
 * No @c FlightRecord should ever be read or written outside that lock.
 *
 * @warning Do **not** modify this struct after distributing to the team.
 *          The Data Processing module reads every field by name; renaming
 *          or removing a member will cause compilation failures across modules.
 *
 * @ingroup Shared
 *
 * @author  Group 7
 * @date    2025
 * @version 1.0
 */

#ifndef FLIGHT_RECORD_H
#define FLIGHT_RECORD_H

#include <stdint.h>
#include <time.h>

/**
 * @brief Holds all runtime state for one aircraft's active flight session.
 *
 * @details
 * Stored in @c std::unordered_map<uint32_t, FlightRecord> inside
 * @c data_processing.cpp, keyed by @c aircraft_id.
 *
 * Lifecycle:
 *  - **Created**  by @c init_flight_record()  on the first received packet.
 *  - **Updated**  by @c update_flight_record() on every subsequent packet.
 *  - **Finalized** by @c finalize_flight()     on clean client disconnect;
 *    the record is copied out, erased from the map, and written to CSV.
 *
 * Average fuel consumption formula:
 * @code
 *   avg_consumption = (initial_fuel - latest_fuel) / latest_elapsed * 3600.0f;
 * @endcode
 */
typedef struct {
    uint32_t  aircraft_id;      /**< Unique 32-bit aircraft identifier.
                                 *   Matches @c TelemetryPacket::aircraft_id for
                                 *   every packet belonging to this session. */

    float     initial_fuel;     /**< Fuel level (gallons) captured from the very
                                 *   first packet.  Used as the baseline to compute
                                 *   total fuel consumed over the flight. */

    float     latest_fuel;      /**< Most recently received fuel value (gallons).
                                 *   Overwritten on every call to
                                 *   @c update_flight_record(). */

    float     latest_elapsed;   /**< Most recently received elapsed time (seconds).
                                 *   Overwritten on every call to
                                 *   @c update_flight_record(). */

    float     avg_consumption;  /**< Running average fuel burn rate in gallons per
                                 *   hour, recalculated on each packet update.
                                 *   0.0 until the second packet is received
                                 *   (elapsed_time_sec must be > 0). */

    int       packet_count;     /**< Total number of TelemetryPackets received for
                                 *   this aircraft during the current session.
                                 *   Incremented by @c update_flight_record(). */

    time_t    session_start;    /**< Wall-clock time (Unix epoch) when the very
                                 *   first packet from this aircraft arrived.
                                 *   Used to compute total flight duration on
                                 *   disconnect. */
} FlightRecord;

#endif /* FLIGHT_RECORD_H */
