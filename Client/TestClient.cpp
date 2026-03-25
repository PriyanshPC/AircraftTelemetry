/*
 * TestClient.cpp
 * Temporary test tool for Module 1 verification.
 * NOT the real client — delete this before final submission.
 *
 * What it does:
 *   - Connects to the server
 *   - Sends 10 fake TelemetryPackets with realistic fuel values
 *   - Disconnects cleanly
 *   - Run multiple instances simultaneously to test threading
 *
 * Usage:
 *   TestClient.exe <server_ip> <port>
 *   TestClient.exe 127.0.0.1 5000
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

#pragma comment(lib, "ws2_32.lib")

#include "../shared/telemetry_packet.h"

int send_all(SOCKET sock, void* data, int len) {
    int total = 0;
    char* ptr = (char*)data;
    while (total < len) {
        int sent = send(sock, ptr + total, len - total, 0);
        if (sent <= 0) return -1;
        total += sent;
    }
    return total;
}

int main(int argc, char* argv[]) {

    if (argc < 3) {
        fprintf(stderr, "Usage: TestClient.exe <server_ip> <port>\n");
        return 1;
    }

    char* server_ip = argv[1];
    int   server_port = atoi(argv[2]);

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    /* Generate a fake aircraft ID */
    uint32_t aircraft_id = (uint32_t)(GetCurrentProcessId() ^ (uint32_t)time(NULL));
    printf("[TestClient] Aircraft ID: %u\n", aircraft_id);

    /* Create socket */
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "[TestClient] socket() failed\n");
        WSACleanup();
        return 1;
    }

    /* Connect */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)server_port);
    inet_pton(AF_INET, server_ip, &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[TestClient] connect() failed: %d\n",
            WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }
    printf("[TestClient] Connected to %s:%d\n", server_ip, server_port);

    /* Send 10 fake packets simulating a flight */
    float initial_fuel = 19000.0f;
    float burn_rate = 0.111f; /* ~400 gal/hr */

    for (int i = 0; i < 10; i++) {
        TelemetryPacket pkt;
        pkt.aircraft_id = aircraft_id;
        pkt.elapsed_time_sec = (float)(i * 60);          /* every 60 seconds */
        pkt.fuel_remaining = initial_fuel - (burn_rate * pkt.elapsed_time_sec);

        printf("[TestClient] Sending packet %d | "
            "Elapsed: %.0fs | Fuel: %.1f gal\n",
            i + 1, pkt.elapsed_time_sec, pkt.fuel_remaining);

        if (send_all(sock, &pkt, sizeof(TelemetryPacket)) < 0) {
            fprintf(stderr, "[TestClient] send() failed\n");
            break;
        }

        Sleep(500); /* wait 500ms between packets so server output is readable */
    }

    printf("[TestClient] All packets sent — closing connection.\n");
    closesocket(sock);
    WSACleanup();
    return 0;
}