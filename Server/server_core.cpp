/*
 * server_core.cpp
 * Module 1 — Server Core: Connection & Thread Management
 * Owner: Priyansh
 *
 * Responsibilities:
 *   - TCP socket: socket(), bind(), listen(), accept() loop
 *   - Spawn one pthread per accepted client connection
 *   - pthread_detach() so threads self-clean on exit
 *   - pthread_mutex_t initialization and destruction
 *   - Graceful handling of abrupt client disconnects (in handle_client)
 *
 * Windows notes:
 *   - Use closesocket() not close()
 *   - WSAStartup() required before any socket call
 *   - SOCKET type instead of int for socket descriptors
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "ws2_32.lib")

#include "server_core.h"
#include "data_processing.h"
#include "../shared/telemetry_packet.h"
#include "../shared/flight_record.h"

 /* ── Global mutex ────────────────────────────────────────────────────────── */
pthread_mutex_t records_mutex;

/* ── Client thread handler ───────────────────────────────────────────────── */
void* handle_client(void* arg) {
    /*
     * This function runs in its own pthread.
     * Each connected aircraft gets one dedicated instance of this function.
     *
     * TODO (Priyansh):
     *   1. Cast arg back to SOCKET
     *   2. Loop: recv() exactly sizeof(TelemetryPacket) bytes each call
     *   3. If recv() returns 0  → clean disconnect → call finalize_flight()
     *   4. If recv() returns <0 → abrupt disconnect → log error, do NOT crash
     *   5. On either disconnect: closesocket(), return NULL
     *   6. For each valid packet received: call update_flight_record()
     */

    SOCKET client_sock = *(SOCKET*)arg;
    free(arg); /* arg was malloc'd in main — free it here */

    TelemetryPacket pkt;
    int first_packet = 1;

    while (1) {
        int bytes = recv(client_sock, (char*)&pkt, sizeof(TelemetryPacket), MSG_WAITALL);

        if (bytes == 0) {
            /* Client disconnected cleanly — end of flight */
            printf("[Server] Aircraft %u flight ended cleanly.\n", pkt.aircraft_id);
            finalize_flight(pkt.aircraft_id);
            break;
        }
        if (bytes < 0) {
            /* Client crashed or network lost — handle gracefully */
            printf("[Server] Aircraft connection lost (abrupt disconnect).\n");
            break;
        }

        /* First packet from this aircraft — initialise its record */
        if (first_packet) {
            init_flight_record(&pkt);
            first_packet = 0;
        }
        else {
            update_flight_record(&pkt);
        }
    }

    closesocket(client_sock);
    return NULL;
}

/* ── main() ──────────────────────────────────────────────────────────────── */
int main(int argc, char* argv[]) {

    if (argc < 2) {
        fprintf(stderr, "Usage: Server.exe <port>\n");
        fprintf(stderr, "Example: Server.exe 5000\n");
        return 1;
    }

    /* ── Winsock init ── */
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "[Error] WSAStartup failed.\n");
        return 1;
    }

    /* ── Mutex init ── */
    pthread_mutex_init(&records_mutex, NULL);

    int port = atoi(argv[1]);
    printf("╔══════════════════════════════════════════╗\n");
    printf("║  Aircraft Telemetry Server               ║\n");
    printf("║  Listening on port %-5d                 ║\n", port);
    printf("╚══════════════════════════════════════════╝\n");

    /*
     * TODO (Priyansh):
     *   1. Create server socket:    socket(AF_INET, SOCK_STREAM, 0)
     *   2. Set SO_REUSEADDR option: setsockopt()
     *   3. Bind to port:            bind()
     *   4. Start listening:         listen()
     *   5. Accept loop:
     *        SOCKET client = accept(...)
     *        SOCKET* arg   = malloc(sizeof(SOCKET))  ← heap allocate so thread gets its own copy
     *        *arg          = client
     *        pthread_create(&thread, NULL, handle_client, arg)
     *        pthread_detach(thread)
     */

     /* ── Cleanup ── */
    pthread_mutex_destroy(&records_mutex);
    WSACleanup();
    return 0;
}