/*
 * client.cpp
 * Module 2 — Client Core: Telemetry Transmission
 * Owner: Member 2
 *
 * Responsibilities:
 *   - Parse server IP, port, telemetry file from command-line arguments
 *   - Generate unique aircraft ID (PID XOR timestamp)
 *   - Connect to server via TCP
 *   - Read telemetry file line by line
 *   - Pack each line into TelemetryPacket and transmit via send_all()
 *   - Close socket cleanly on EOF (signals end-of-flight to server)
 *
 * Usage:
 *   Client.exe <server_ip> <port> <telemetry_file>
 *   Client.exe 192.168.1.10 5000 katl-kefd-B737-700.txt
 *
 * Windows notes:
 *   - Use closesocket() not close()
 *   - GetCurrentProcessId() instead of getpid()
 *   - WSAStartup() required before any socket call
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#include "../shared/telemetry_packet.h"
#include "../shared/flight_record.h"

 /* ── send_all ────────────────────────────────────────────────────────────── */
 /*
  * Guarantees all bytes are sent even if send() does a partial write.
  * Returns total bytes sent, or -1 on error.
  */
int send_all(SOCKET sock, void* data, int len) {
    int   total = 0;
    char* ptr = (char*)data;
    while (total < len) {
        int sent = send(sock, ptr + total, len - total, 0);
        if (sent <= 0) return -1;
        total += sent;
    }
    return total;
}

/* ── main() ──────────────────────────────────────────────────────────────── */
int main(int argc, char* argv[]) {

    if (argc < 4) {
        fprintf(stderr, "Usage  : Client.exe <server_ip> <port> <telemetry_file>\n");
        fprintf(stderr, "Example: Client.exe 192.168.1.10 5000 katl-kefd-B737-700.txt\n");
        return 1;
    }

    char* server_ip = argv[1];
    int   server_port = atoi(argv[2]);
    char* telem_file = argv[3];

    /* ── Winsock init ── */
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "[Error] WSAStartup failed.\n");
        return 1;
    }

    /* ── Generate unique aircraft ID ── */
    uint32_t aircraft_id = (uint32_t)(GetCurrentProcessId() ^ (uint32_t)time(NULL));

    printf("╔══════════════════════════════════════════╗\n");
    printf("║  Aircraft Telemetry Client               ║\n");
    printf("║  Aircraft ID : %-10u               ║\n", aircraft_id);
    printf("║  Server      : %s:%-5d           ║\n", server_ip, server_port);
    printf("║  Data file   : %-26s║\n", telem_file);
    printf("╚══════════════════════════════════════════╝\n");

    /* ── Open telemetry file ── */
    FILE* fp = fopen(telem_file, "r");
    if (!fp) {
        fprintf(stderr, "[Error] Cannot open telemetry file: %s\n", telem_file);
        WSACleanup();
        return 1;
    }

    /* ── Create TCP socket ── */
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "[Error] socket() failed: %d\n", WSAGetLastError());
        fclose(fp);
        WSACleanup();
        return 1;
    }

    /* ── Connect to server ── */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((u_short)server_port);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        fprintf(stderr, "[Error] connect() failed: %d\n", WSAGetLastError());
        closesocket(sock);
        fclose(fp);
        WSACleanup();
        return 1;
    }

    printf("[Client] Connected to server. Streaming telemetry...\n");

    /* ── Transmission loop ── */
    char   line[128];
    int    packet_count = 0;
    float  elapsed, fuel;

    while (fgets(line, sizeof(line), fp)) {

        /* Skip empty lines or comment lines */
        if (line[0] == '\n' || line[0] == '#') continue;

        /* Parse CSV line: elapsed_time,fuel_remaining */
        if (sscanf(line, "%f,%f", &elapsed, &fuel) != 2) continue;

        /* Build packet */
        TelemetryPacket pkt;
        pkt.aircraft_id = aircraft_id;
        pkt.elapsed_time_sec = elapsed;
        pkt.fuel_remaining = fuel;

        /* Transmit */
        if (send_all(sock, &pkt, sizeof(TelemetryPacket)) < 0) {
            fprintf(stderr, "[Error] send() failed on packet %d\n", packet_count);
            break;
        }

        packet_count++;

        /* Progress update every 100 packets */
        if (packet_count % 100 == 0) {
            printf("[Client] Aircraft %u | Sent %d packets | "
                "Elapsed: %.0fs | Fuel: %.1f gal\n",
                aircraft_id, packet_count, elapsed, fuel);
        }
    }

    printf("[Client] Aircraft %u | EOF reached | Total packets sent: %d\n",
        aircraft_id, packet_count);
    printf("[Client] Closing connection — end of flight.\n");

    /* ── Clean shutdown ── */
    fclose(fp);
    closesocket(sock);
    WSACleanup();
    return 0;
}
