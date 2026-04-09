/*
 * server_core.cpp
 * Module 1 — Server Core: Connection & Thread Management
 * Owner: Priyansh
 *
 * Build order:
 *   1. WSAStartup        — initialize Winsock
 *   2. socket()          — create TCP socket
 *   3. setsockopt()      — allow port reuse after restart
 *   4. bind()            — attach socket to port
 *   5. listen()          — mark socket as passive (waiting for connections)
 *   6. accept() loop     — block until client connects, spawn thread
 *   7. handle_client()   — thread function: recv packets, call Module 3
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#pragma comment(lib, "ws2_32.lib")

#include "server_core.h"
#include "../DataProcessing/data_processing.h"
#include "../shared/telemetry_packet.h"

 /* ═══════════════════════════════════════════════════════════════════════════
  * GLOBAL MUTEX
  * Defined here. Used in data_processing.cpp via extern.
  * Protects the shared flight_records map from race conditions.
  * ═══════════════════════════════════════════════════════════════════════════ */
pthread_mutex_t records_mutex;

/* ═══════════════════════════════════════════════════════════════════════════
 * handle_client()
 * Runs in its own pthread — one instance per connected aircraft.
 *
 * arg: heap-allocated SOCKET pointer (malloc'd in main, freed here)
 *
 * Flow:
 *   1. Cast arg → SOCKET, free the heap allocation
 *   2. Loop: recv() exactly sizeof(TelemetryPacket) bytes
 *   3. recv() == 0  → clean disconnect (EOF) → finalize_flight()
 *   4. recv() <  0  → abrupt disconnect (crash/power-off) → log, exit thread
 *   5. First packet → init_flight_record()
 *   6. All other packets → update_flight_record()
 *   7. Close socket, thread exits
 * ═══════════════════════════════════════════════════════════════════════════ */
void* handle_client(void* arg) {

    /* ── Unpack the socket from the heap allocation ── */
    SOCKET client_sock = *(SOCKET*)arg;
    free(arg);

    TelemetryPacket pkt;
    int first_packet = 1;
    uint32_t aircraft_id = 0;

    printf("[Server] Thread started for new connection.\n");

    /* ── Receive loop ── */
    while (1) {

        /*
         * MSG_WAITALL tells recv() to block until ALL sizeof(TelemetryPacket)
         * bytes have arrived. Without this, recv() might return a partial packet
         * on a busy network.
         */
        int bytes = recv(client_sock,
            (char*)&pkt,
            sizeof(TelemetryPacket),
            MSG_WAITALL);

        /* ── Client disconnected cleanly (EOF) ── */
        if (bytes == 0) {
            printf("\n[Server] Aircraft %u disconnected cleanly - flight ended.\n",
                aircraft_id);
            finalize_flight(aircraft_id);
            break;
        }

        /* ── Client crashed or network lost (abrupt disconnect) ── */
        if (bytes < 0) {
            int err = WSAGetLastError();
            printf("[Server] Aircraft %u lost connection abruptly "
                "(WSA error: %d) — thread cleaning up.\n",
                aircraft_id, err);
            /*
             * Do NOT call finalize_flight() here — the flight data is
             * incomplete. Just exit the thread cleanly.
             * The server keeps running. Other threads are unaffected.
             */
            break;
        }

        /* ── Valid packet received ── */
        aircraft_id = pkt.aircraft_id; /* track ID for disconnect messages */

        if (first_packet) {
            /*
             * First packet from this aircraft.
             * Initialize its FlightRecord in the shared map.
             */
            init_flight_record(&pkt);
            first_packet = 0;
        }
        else {
            /*
             * Subsequent packets.
             * Recalculate running average fuel consumption.
             */
            update_flight_record(&pkt);
        }
    }

    /* ── Always close the socket before the thread exits ── */
    closesocket(client_sock);
    printf("[Server] Thread for aircraft %u exited cleanly.\n", aircraft_id);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main()
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(int argc, char* argv[]) {

    /* ── Argument check ── */
    if (argc < 2) {
        fprintf(stderr, "Usage  : Server.exe <port>\n");
        fprintf(stderr, "Example: Server.exe 5000\n");
        return 1;
    }
    int port = atoi(argv[1]);

    /* ── Winsock initialization ── */
    WSADATA wsaData;
    int wsa_result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsa_result != 0) {
        fprintf(stderr, "[Error] WSAStartup failed: %d\n", wsa_result);
        return 1;
    }

    /* ── Mutex initialization ── */
    pthread_mutex_init(&records_mutex, NULL);

    /* ── Create TCP socket ── */
    /*
     * AF_INET   = IPv4
     * SOCK_STREAM = TCP (reliable, ordered, connection-based)
     * 0         = default protocol for SOCK_STREAM = TCP
     */
    SOCKET server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock == INVALID_SOCKET) {
        fprintf(stderr, "[Error] socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    /* ── Set SO_REUSEADDR ── */
    /*
     * Without this, if the server crashes and you restart it immediately,
     * Windows keeps the port "in use" for ~60 seconds (TIME_WAIT state).
     * SO_REUSEADDR lets us bind to the port again immediately.
     * Critical for testing — you will restart the server constantly.
     */
    int opt = 1;
    setsockopt(server_sock,
        SOL_SOCKET,
        SO_REUSEADDR,
        (char*)&opt,
        sizeof(opt));

    /* ── Bind to port ── */
    /*
     * INADDR_ANY = accept connections on ANY network interface
     * (localhost, LAN IP, etc.) — important for multi-PC testing
     */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons((u_short)port);

    if (bind(server_sock,
        (struct sockaddr*)&server_addr,
        sizeof(server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[Error] bind() failed: %d\n", WSAGetLastError());
        closesocket(server_sock);
        WSACleanup();
        return 1;
    }

    /* ── Start listening ── */
    /*
     * SOMAXCONN = maximum connection backlog (system maximum, typically 128+)
     * This is the queue of connections waiting to be accept()'d.
     * Use SOMAXCONN not a small number like 5 — during load testing
     * hundreds of clients connect nearly simultaneously.
     */
    if (listen(server_sock, SOMAXCONN) == SOCKET_ERROR) {
        fprintf(stderr, "[Error] listen() failed: %d\n", WSAGetLastError());
        closesocket(server_sock);
        WSACleanup();
        return 1;
    }

    /* ── Ready ── */
    printf("++--------------------------------------------++\n");
    printf("||  Aircraft Telemetry Server                 ||\n");
    printf("||  Listening on port %-5d                   ||\n", port);
    printf("||  Waiting for aircraft connections...       ||\n");
    printf("++--------------------------------------------++\n\n");

    /* ═══════════════════════════════════════════════════════════════════════
     * MAIN ACCEPT LOOP
     *
     * This loop runs forever. Every time a client connects:
     *   1. accept() returns a new socket for that specific client
     *   2. We heap-allocate the socket so the thread gets its own copy
     *      (if we passed a stack variable, it could be overwritten before
     *       the thread reads it — a classic threading bug)
     *   3. pthread_create() spawns the thread
     *   4. pthread_detach() tells the OS to auto-cleanup when thread finishes
     *      (we never call pthread_join() — that would block the loop)
     *   5. Loop immediately back to accept() — never stops accepting
     * ═══════════════════════════════════════════════════════════════════════ */
    while (1) {

        struct sockaddr_in client_addr;
        int client_addr_len = sizeof(client_addr);

        /* Blocks here until a client connects */
        SOCKET client_sock = accept(server_sock,
            (struct sockaddr*)&client_addr,
            &client_addr_len);

        if (client_sock == INVALID_SOCKET) {
            fprintf(stderr, "[Warning] accept() failed: %d - continuing.\n",
                WSAGetLastError());
            continue; /* Don't crash — just wait for next connection */
        }

        /* Get client IP for logging */
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET,
            &client_addr.sin_addr,
            client_ip,
            INET_ADDRSTRLEN);
        printf("[Server] New connection from %s - spawning thread.\n",
            client_ip);

        /*
         * IMPORTANT: heap-allocate the socket before passing to thread.
         *
         * Why malloc? If we passed &client_sock directly, the next iteration
         * of the loop would overwrite client_sock with the next accepted socket
         * BEFORE the new thread had a chance to read it.
         *
         * Each thread gets its own malloc'd copy → no race condition.
         * The thread is responsible for free()'ing it (done in handle_client).
         */
        SOCKET* sock_ptr = (SOCKET*)malloc(sizeof(SOCKET));
        if (sock_ptr == NULL) {
            fprintf(stderr, "[Error] malloc failed - closing connection.\n");
            closesocket(client_sock);
            continue;
        }
        *sock_ptr = client_sock;

        /* Spawn thread */
        pthread_t thread;
        int result = pthread_create(&thread, NULL, handle_client, sock_ptr);
        if (result != 0) {
            fprintf(stderr, "[Error] pthread_create failed: %d\n", result);
            free(sock_ptr);
            closesocket(client_sock);
            continue;
        }

        /*
         * Detach the thread — it cleans itself up when handle_client returns.
         * We never call pthread_join() because that would block the accept loop.
         */
        pthread_detach(thread);

        printf("[Server] Thread spawned successfully.\n\n");
    }

    /* ── Cleanup (only reached if the loop is ever broken) ── */
    closesocket(server_sock);
    pthread_mutex_destroy(&records_mutex);
    WSACleanup();
    return 0;
}