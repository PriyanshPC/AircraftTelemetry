#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#include "../shared/telemetry_packet.h"

/* Send all bytes */
int send_all(SOCKET sock, const void* data, int len) {
    int total = 0;
    const char* ptr = (const char*)data;

    while (total < len) {
        int sent = send(sock, ptr + total, len - total, 0);
        if (sent <= 0) {
            return -1;
        }
        total += sent;
    }
    return total;
}

/* Trim leading spaces */
char* ltrim(char* s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

/* Parse timestamp format: d_m_yyyy hh:mm:ss */
int parse_datetime(const char* datetime_str, struct tm* out_tm) {
    int day, month, year, hour, minute, second;

    if (sscanf(datetime_str, "%d_%d_%d %d:%d:%d",
        &day, &month, &year, &hour, &minute, &second) != 6) {
        return 0;
    }

    memset(out_tm, 0, sizeof(struct tm));
    out_tm->tm_mday = day;
    out_tm->tm_mon = month - 1;
    out_tm->tm_year = year - 1900;
    out_tm->tm_hour = hour;
    out_tm->tm_min = minute;
    out_tm->tm_sec = second;
    out_tm->tm_isdst = -1;

    return 1;
}

/* Parse one telemetry line
   Supports:
   "FUEL TOTAL QUANTITY,3_3_2023 14:53:21,4564.466309,"
   " 3_3_2023 14:53:22,4564.405273,"
*/
int parse_telemetry_line(const char* line, time_t* timestamp_out, float* fuel_out) {
    char buffer[256];
    strncpy(buffer, line, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    char* p = ltrim(buffer);

    if (*p == '\0' || *p == '\n' || *p == '#') {
        return 0;
    }

    char datetime_str[64] = { 0 };
    float fuel = 0.0f;

    /* Case 1: first line with label */
    if (strncmp(p, "FUEL TOTAL QUANTITY,", 20) == 0) {
        if (sscanf(p, "FUEL TOTAL QUANTITY,%63[^,],%f", datetime_str, &fuel) != 2) {
            return 0;
        }
    }
    else {
        /* Case 2: normal telemetry line */
        if (sscanf(p, "%63[^,],%f", datetime_str, &fuel) != 2) {
            return 0;
        }
    }

    struct tm tm_value;
    if (!parse_datetime(datetime_str, &tm_value)) {
        return 0;
    }

    time_t ts = mktime(&tm_value);
    if (ts == (time_t)-1) {
        return 0;
    }

    *timestamp_out = ts;
    *fuel_out = fuel;
    return 1;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage  : Client.exe <server_ip> <port> <telemetry_file>\n");
        fprintf(stderr, "Example: Client.exe 192.168.1.10 5000 \"..\\TelemetryData\\Data Files\\katl-kefd-B737-700.txt\"\n");
        return 1;
    }

    const char* server_ip = argv[1];
    int server_port = atoi(argv[2]);
    const char* telemetry_file = argv[3];

    if (server_port <= 0 || server_port > 65535) {
        fprintf(stderr, "[Error] Invalid server port.\n");
        return 1;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "[Error] WSAStartup failed.\n");
        return 1;
    }

    uint32_t aircraft_id = (uint32_t)(GetCurrentProcessId() ^ (uint32_t)time(NULL));

    printf("=============================================\n");
    printf(" Aircraft Telemetry Client\n");
    printf(" Aircraft ID : %u\n", aircraft_id);
    printf(" Server      : %s:%d\n", server_ip, server_port);
    printf(" File        : %s\n", telemetry_file);
    printf("=============================================\n");

    FILE* fp = fopen(telemetry_file, "r");
    if (!fp) {
        fprintf(stderr, "[Error] Cannot open telemetry file: %s\n", telemetry_file);
        WSACleanup();
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "[Error] socket() failed: %d\n", WSAGetLastError());
        fclose(fp);
        WSACleanup();
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((u_short)server_port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "[Error] Invalid server IP address.\n");
        closesocket(sock);
        fclose(fp);
        WSACleanup();
        return 1;
    }

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[Error] connect() failed: %d\n", WSAGetLastError());
        closesocket(sock);
        fclose(fp);
        WSACleanup();
        return 1;
    }

    printf("[Client] Connected to server.\n");
    printf("[Client] Streaming telemetry...\n");

    char line[256];
    int packet_count = 0;
    time_t first_timestamp = 0;
    int first_valid_record = 1;

    while (fgets(line, sizeof(line), fp)) {
        time_t current_timestamp;
        float fuel_remaining;

        if (!parse_telemetry_line(line, &current_timestamp, &fuel_remaining)) {
            continue;
        }

        if (first_valid_record) {
            first_timestamp = current_timestamp;
            first_valid_record = 0;
        }

        float elapsed_time_sec = (float)difftime(current_timestamp, first_timestamp);
        if (elapsed_time_sec < 0) {
            elapsed_time_sec = 0.0f;
        }

        TelemetryPacket pkt;
        pkt.aircraft_id = aircraft_id;
        pkt.elapsed_time_sec = elapsed_time_sec;
        pkt.fuel_remaining = fuel_remaining;

        if (send_all(sock, &pkt, sizeof(TelemetryPacket)) < 0) {
            fprintf(stderr, "[Error] send() failed on packet %d\n", packet_count + 1);
            break;
        }

        packet_count++;

        if (packet_count % 100 == 0) {
            printf("[Client] Sent %d packets | Elapsed: %.0f sec | Fuel: %.3f\n",
                packet_count, elapsed_time_sec, fuel_remaining);
        }
    }

    printf("[Client] EOF reached. Total packets sent: %d\n", packet_count);
    printf("[Client] Closing connection.\n");

    fclose(fp);
    closesocket(sock);
    WSACleanup();

    return 0;
}