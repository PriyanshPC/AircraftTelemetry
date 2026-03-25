#ifndef TELEMETRY_PACKET_H
#define TELEMETRY_PACKET_H

#include <stdint.h>

/*
 * TelemetryPacket
 * ---------------
 * Fixed-size binary struct transmitted from Client to Server over TCP.
 * Total size: 12 bytes.
 *
 * The server always reads exactly sizeof(TelemetryPacket) bytes per recv() call.
 * Using a fixed binary struct eliminates delimiter parsing and maximises throughput.
 *
 * !! DO NOT MODIFY THIS STRUCT AFTER DISTRIBUTING TO THE TEAM !!
 * Any change silently breaks the client-server data contract.
 */
typedef struct {
    uint32_t  aircraft_id;        /* Unique ID assigned to this aircraft on startup  */
    float     elapsed_time_sec;   /* Seconds elapsed since the flight began           */
    float     fuel_remaining;     /* Gallons of fuel remaining onboard right now      */
} TelemetryPacket;

#endif /* TELEMETRY_PACKET_H */