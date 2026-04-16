/**
 * @file telemetry_packet.h
 * @brief Fixed-size binary packet transmitted from Client to Server over TCP.
 *
 * @details
 * TelemetryPacket is the sole data interchange format between the Aircraft
 * Telemetry Client (Module 2) and the Server Core (Module 1).  Using a packed
 * fixed-size struct eliminates delimiter parsing overhead and allows the server
 * to read exactly one complete record per @c recv() call using @c MSG_WAITALL.
 *
 * **Wire format — 12 bytes, host byte order:**
 * | Byte offset | Size (bytes) | Field              |
 * |-------------|--------------|---------------------|
 * | 0           | 4            | @c aircraft_id      |
 * | 4           | 4            | @c elapsed_time_sec |
 * | 8           | 4            | @c fuel_remaining   |
 *
 * @warning Do **not** modify this struct after distributing to the team.
 *          Any field addition, removal, or reorder silently breaks the
 *          client–server binary contract without a compile-time error.
 *
 * @note Both Client and Server must be compiled for the same platform
 *       so that @c sizeof(TelemetryPacket) == 12 on both sides.
 *
 * @ingroup Shared
 *
 * @author  Group 7
 * @date    2025
 * @version 1.0
 */

#ifndef TELEMETRY_PACKET_H
#define TELEMETRY_PACKET_H

#include <stdint.h>

/**
 * @brief Fixed-size telemetry packet sent from an aircraft client to the server.
 *
 * @details
 * Transmitted over a TCP stream as a raw binary blob.  The server reads
 * exactly @c sizeof(TelemetryPacket) bytes per @c recv() invocation using
 * the @c MSG_WAITALL flag to guarantee atomic delivery.
 *
 * Fuel consumption is derived on the server by comparing @c fuel_remaining
 * across successive packets:
 * @code
 *   avg_gal_hr = (initial_fuel - fuel_remaining) / elapsed_time_sec * 3600.0f;
 * @endcode
 */
typedef struct {
    uint32_t  aircraft_id;       /**< Unique 32-bit identifier assigned to this
                                  *   aircraft instance on startup.
                                  *   Generated as: PID XOR current Unix timestamp. */
    float     elapsed_time_sec;  /**< Seconds elapsed since the flight began
                                  *   (i.e. since the first telemetry line was read).
                                  *   Always 0.0 on the first packet. */
    float     fuel_remaining;    /**< Gallons of fuel remaining onboard at the
                                  *   moment this packet was constructed.
                                  *   Parsed directly from the telemetry data file. */
} TelemetryPacket;

#endif /* TELEMETRY_PACKET_H */
