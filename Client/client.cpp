/**
 * @file client.cpp
 * @brief Implementation of Module 2 — Aircraft Telemetry Client.
 *
 * @details
 * Standalone executable that reads fuel telemetry from a plain-text data file,
 * constructs fixed-size @c TelemetryPacket records, and streams them over a
 * TCP connection to the Aircraft Telemetry Server.
 *
 * **Usage:**
 * @code
 *   Client.exe <server_ip> <port> <telemetry_file>
 *   Client.exe 192.168.1.10 5000 "..\TelemetryData\Data Files\katl-kefd-B737-700.txt"
 * @endcode
 *
 * **Execution flow:**
 *  1. Parse and validate command-line arguments.
 *  2. Initialise Winsock 2.2.
 *  3. Generate a unique @c aircraft_id as @c PID XOR time(NULL).
 *  4. Open the telemetry data file.
 *  5. Create a TCP socket and connect to the server.
 *  6. Read the file line by line; parse each valid line into a
 *     (@c timestamp, @c fuel_remaining) pair via @c parse_telemetry_line().
 *  7. Compute @c elapsed_time_sec relative to the first valid timestamp.
 *  8. Populate a @c TelemetryPacket and send it with @c send_all().
 *  9. On EOF: close the socket (signals a clean disconnect to the server)
 *     and call @c WSACleanup().
 *
 * **Telemetry file format:**
 * The file may mix two line formats:
 * @code
 *   FUEL TOTAL QUANTITY,3_3_2023 14:53:21,4564.466309,   ← first line (labelled)
 *    3_3_2023 14:53:22,4564.405273,                       ← subsequent lines
 * @endcode
 * Lines that are blank, start with @c '#', or cannot be parsed are silently
 * skipped.
 *
 * @ingroup Client
 *
 * @author  Group 7 — Member 2
 * @date    2025
 * @version 1.0
 */

#ifndef _WIN32_WINNT
/** @brief Minimum Windows version: Windows 7 (required for Winsock 2.2). */
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

/* ═══════════════════════════════════════════════════════════════════════════
 * HELPER FUNCTIONS
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Send all @p len bytes from @p data over @p sock, handling partial sends.
 *
 * @details
 * A single @c send() call is not guaranteed to transmit all requested bytes
 * (the OS may buffer and return a short count).  This wrapper loops until
 * every byte has been sent or an error occurs, ensuring the full
 * @c TelemetryPacket is always delivered atomically from the receiver's
 * perspective.
 *
 * @param[in] sock  Connected TCP socket to send data on.
 * @param[in] data  Pointer to the buffer to send.  Must not be @c NULL.
 * @param[in] len   Number of bytes to send.  Must be > 0.
 *
 * @return Total bytes sent (== @p len) on success.
 * @return @c -1 if @c send() returns an error (@c <= 0) at any iteration.
 */
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

/**
 * @brief Advance a string pointer past any leading space or tab characters.
 *
 * @details
 * Used to normalise telemetry lines that may be indented with leading
 * whitespace (continuation lines in the data file frequently begin with a
 * space character).
 *
 * @param[in] s  Pointer to the start of a null-terminated string.
 *               Must not be @c NULL.
 *
 * @return Pointer to the first character in @p s that is not a space or tab,
 *         or to the null terminator if the entire string is whitespace.
 *
 * @note The original string is not modified; the returned pointer points
 *       into the same buffer.
 */
char* ltrim(char* s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

/**
 * @brief Parse a timestamp string in @c d_m_yyyy @c hh:mm:ss format into a @c struct @c tm.
 *
 * @details
 * Expected input format: @c "3_3_2023 14:53:21" (day_month_year hour:min:sec).
 * Uses @c sscanf() to extract six integer components, then populates @p out_tm
 * according to the @c struct @c tm convention (@c tm_mon is zero-based,
 * @c tm_year is years since 1900).
 *
 * Sets @c out_tm->tm_isdst = -1 to ask @c mktime() to determine DST automatically.
 *
 * @param[in]  datetime_str  Null-terminated string containing the timestamp.
 *                           Must not be @c NULL.
 * @param[out] out_tm        Caller-allocated @c struct @c tm that is zeroed and
 *                           populated on success.  Must not be @c NULL.
 *
 * @return @c 1 if exactly six fields were successfully parsed.
 * @return @c 0 if @c sscanf() did not match all six fields (malformed input).
 */
int parse_datetime(const char* datetime_str, struct tm* out_tm) {
    int day, month, year, hour, minute, second;

    if (sscanf(datetime_str, "%d_%d_%d %d:%d:%d",
        &day, &month, &year, &hour, &minute, &second) != 6) {
        return 0;
    }

    memset(out_tm, 0, sizeof(struct tm));
    out_tm->tm_mday = day;
    out_tm->tm_mon  = month - 1;
    out_tm->tm_year = year - 1900;
    out_tm->tm_hour = hour;
    out_tm->tm_min  = minute;
    out_tm->tm_sec  = second;
    out_tm->tm_isdst = -1;

    return 1;
}

/**
 * @brief Parse one line from the telemetry data file into a timestamp and fuel value.
 *
 * @details
 * Handles two line formats that appear in the telemetry files:
 *
 * **Format 1 — labelled first line:**
 * @code
 *   FUEL TOTAL QUANTITY,3_3_2023 14:53:21,4564.466309,
 * @endcode
 *
 * **Format 2 — continuation lines:**
 * @code
 *    3_3_2023 14:53:22,4564.405273,
 * @endcode
 *
 * Steps:
 *  1. Copy the line into a local buffer (avoids modifying the caller's data).
 *  2. @c ltrim() the leading whitespace.
 *  3. Skip blank lines, newline-only lines, and comment lines (@c '#').
 *  4. Branch on the @c "FUEL TOTAL QUANTITY," prefix to select the sscanf pattern.
 *  5. Call @c parse_datetime() on the extracted timestamp string.
 *  6. Convert to @c time_t via @c mktime().
 *
 * @param[in]  line           Null-terminated line read from the file.
 *                            Must not be @c NULL.
 * @param[out] timestamp_out  Receives the @c time_t corresponding to the
 *                            line's timestamp field on success.
 * @param[out] fuel_out       Receives the fuel quantity (gallons) on success.
 *
 * @return @c 1 if the line was parsed successfully and both output values
 *         were populated.
 * @return @c 0 if the line is blank, a comment, or does not match either
 *         expected format (caller should skip this line silently).
 */
int parse_telemetry_line(const char* line, time_t* timestamp_out, float* fuel_out) {
    char buffer[256];
    strncpy(buffer, line, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    char* p = ltrim(buffer);

    if (*p == '\0' || *p == '\n' || *p == '#') {
        return 0;
    }

    char  datetime_str[64] = { 0 };
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
    *fuel_out      = fuel;
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN ENTRY POINT
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Client entry point — connect to the server and stream the telemetry file.
 *
 * @details
 * **Usage:** @code Client.exe <server_ip> <port> <telemetry_file> @endcode
 *
 * Startup:
 *  1. Validate that exactly three arguments were provided.
 *  2. Call @c WSAStartup(MAKEWORD(2,2)).
 *  3. Generate a unique @c aircraft_id = @c GetCurrentProcessId() XOR @c time(NULL).
 *     This is statistically unique across concurrent client instances even on
 *     the same machine because process IDs differ and the XOR scrambles them
 *     further.
 *  4. Open the telemetry file; abort if it cannot be read.
 *  5. Create a TCP socket, resolve the server address with @c inet_pton(),
 *     and call @c connect().
 *
 * Main loop (one iteration per line in the telemetry file):
 *  - Skip unparseable lines silently.
 *  - Record the timestamp of the first valid line as @c first_timestamp.
 *  - Compute @c elapsed_time_sec = @c difftime(current_timestamp, first_timestamp).
 *    Clamp to 0.0 if negative (can occur on malformed timestamps).
 *  - Populate a @c TelemetryPacket and call @c send_all().
 *  - Log progress every 100 packets.
 *
 * Teardown:
 *  - @c fclose() the telemetry file.
 *  - @c closesocket() — this sends a FIN to the server, which interprets the
 *    resulting zero-byte @c recv() as a clean flight end and calls
 *    @c finalize_flight().
 *  - @c WSACleanup().
 *
 * @param[in] argc  Argument count; must be >= 4.
 * @param[in] argv  @c argv[1] = server IP string, @c argv[2] = port string,
 *                  @c argv[3] = path to the telemetry data file.
 *
 * @return @c 0 on successful completion (all packets sent, clean disconnect).
 * @return @c 1 on any startup error (bad arguments, Winsock, file, socket, connect).
 */
int main(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage  : Client.exe <server_ip> <port> <telemetry_file>\n");
        fprintf(stderr, "Example: Client.exe 192.168.1.10 5000 \"..\\TelemetryData\\Data Files\\katl-kefd-B737-700.txt\"\n");
        return 1;
    }

    const char* server_ip      = argv[1];
    int         server_port    = atoi(argv[2]);
    const char* telemetry_file = argv[3];

    if (server_port <= 0 || server_port > 65535) {
        fprintf(stderr, "[Error] Invalid server port.\n");
        return 1;
    }

    /* ── Winsock initialization ── */
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "[Error] WSAStartup failed.\n");
        return 1;
    }

    /*
     * Generate a unique aircraft ID.
     * PID XOR time(NULL) gives a different value for each concurrent client
     * instance even when launched from the same batch script on the same machine.
     */
    uint32_t aircraft_id = (uint32_t)(GetCurrentProcessId() ^ (uint32_t)time(NULL));

    printf("=============================================\n");
    printf(" Aircraft Telemetry Client\n");
    printf(" Aircraft ID : %u\n", aircraft_id);
    printf(" Server      : %s:%d\n", server_ip, server_port);
    printf(" File        : %s\n", telemetry_file);
    printf("=============================================\n");

    /* ── Open telemetry file ── */
    FILE* fp = fopen(telemetry_file, "r");
    if (!fp) {
        fprintf(stderr, "[Error] Cannot open telemetry file: %s\n", telemetry_file);
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

    /* ── Resolve server address and connect ── */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons((u_short)server_port);

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

    /* ── Telemetry streaming loop ── */
    char   line[256];
    int    packet_count      = 0;
    time_t first_timestamp   = 0;
    int    first_valid_record = 1;

    while (fgets(line, sizeof(line), fp)) {
        time_t current_timestamp;
        float  fuel_remaining;

        /* Skip lines that cannot be parsed */
        if (!parse_telemetry_line(line, &current_timestamp, &fuel_remaining)) {
            continue;
        }

        /* Capture the first valid timestamp as the flight start reference */
        if (first_valid_record) {
            first_timestamp   = current_timestamp;
            first_valid_record = 0;
        }

        /* Elapsed seconds since the first packet (always >= 0) */
        float elapsed_time_sec = (float)difftime(current_timestamp, first_timestamp);
        if (elapsed_time_sec < 0) {
            elapsed_time_sec = 0.0f;
        }

        /* Build and send the packet */
        TelemetryPacket pkt;
        pkt.aircraft_id      = aircraft_id;
        pkt.elapsed_time_sec = elapsed_time_sec;
        pkt.fuel_remaining   = fuel_remaining;

        if (send_all(sock, &pkt, sizeof(TelemetryPacket)) < 0) {
            fprintf(stderr, "[Error] send() failed on packet %d\n", packet_count + 1);
            break;
        }

        packet_count++;

        /* Progress logging every 100 packets */
        if (packet_count % 100 == 0) {
            printf("[Client] Sent %d packets | Elapsed: %.0f sec | Fuel: %.3f\n",
                packet_count, elapsed_time_sec, fuel_remaining);
        }
    }

    printf("[Client] EOF reached. Total packets sent: %d\n", packet_count);
    printf("[Client] Closing connection.\n");

    /* ── Teardown ── */
    fclose(fp);
    closesocket(sock);   /* FIN → server interprets as clean disconnect */
    WSACleanup();

    return 0;
}
