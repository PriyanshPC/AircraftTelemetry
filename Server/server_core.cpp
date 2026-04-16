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
 *   6. Thread pool init  — pre-spawn THREAD_POOL_SIZE worker threads
 *   7. accept() loop     — push accepted sockets into work queue
 *   8. worker_thread()   — dequeues sockets and calls handle_client_socket()
 *
 * Optimization applied (Project 6 endurance test finding):
 *
 *   THREAD POOL — replaces unbounded pthread_create() per connection.
 *
 *   Original design: one new pthread per accepted connection.
 *   Under the 1000-client load test this spawned up to 1000 simultaneous
 *   threads. Each pthread on Windows carries a default stack of ~1 MB,
 *   meaning up to ~1 GB of committed virtual memory at peak. This was the
 *   primary driver of the memory growth seen in the endurance test
 *   (0 → 96 MB over 71 minutes as threads accumulated faster than the OS
 *   reclaimed their stacks between cycles).
 *
 *   New design: THREAD_POOL_SIZE worker threads are created once at startup.
 *   Accepted sockets are pushed into a circular work queue. Workers block on
 *   a condition variable until a socket is available, handle it, then loop
 *   back to wait. Memory is bounded to THREAD_POOL_SIZE stacks regardless of
 *   client burst size. Excess connections queue up (up to WORK_QUEUE_CAPACITY)
 *   and are served as workers become free.
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
 * THREAD POOL CONFIGURATION
 *
 * THREAD_POOL_SIZE   — number of persistent worker threads.
 *                      128 workers can overlap I/O across 128 concurrent
 *                      aircraft while keeping stack memory bounded.
 *                      Tune upward if profiling shows workers are the
 *                      bottleneck (all busy while queue is non-empty).
 *
 * WORK_QUEUE_CAPACITY — max sockets that can queue while all workers are busy.
 *                       2048 comfortably absorbs a 1000-client burst.
 * ═══════════════════════════════════════════════════════════════════════════ */
#define THREAD_POOL_SIZE     128
#define WORK_QUEUE_CAPACITY  2048

typedef struct {
    SOCKET          sockets[WORK_QUEUE_CAPACITY]; /* circular buffer of sockets */
    int             head;                         /* dequeue index              */
    int             tail;                         /* enqueue index              */
    int             count;                        /* items currently queued     */
    int             shutdown;                     /* set to 1 to stop workers   */
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
} WorkQueue;

static WorkQueue work_queue;

/* ═══════════════════════════════════════════════════════════════════════════
 * handle_client_socket()
 *
 * Core client-handling logic — identical in behaviour to the original
 * handle_client(), but takes a plain SOCKET instead of a heap-allocated
 * void* so worker threads can call it directly without malloc/free.
 *
 * Flow:
 *   1. Loop: recv() exactly sizeof(TelemetryPacket) bytes (MSG_WAITALL)
 *   2. recv() == 0  → clean disconnect → finalize_flight()
 *   3. recv() <  0  → abrupt disconnect → log and exit
 *   4. First packet → init_flight_record()
 *   5. Subsequent  → update_flight_record()
 *   6. closesocket() before returning
 * ═══════════════════════════════════════════════════════════════════════════ */
static void handle_client_socket(SOCKET client_sock) {

    TelemetryPacket pkt;
    int      first_packet = 1;
    uint32_t aircraft_id  = 0;

    /* ── Receive loop ── */
    while (1) {

        int bytes = recv(client_sock,
                         (char*)&pkt,
                         sizeof(TelemetryPacket),
                         MSG_WAITALL);

        if (bytes == 0) {
            /* Clean EOF — client called closesocket() */
            printf("\n[Server] Aircraft %u disconnected cleanly - flight ended.\n",
                   aircraft_id);
            finalize_flight(aircraft_id);
            break;
        }

        if (bytes < 0) {
            /* Abrupt loss — don't finalize; data is incomplete */
            int err = WSAGetLastError();
            printf("[Server] Aircraft %u lost connection abruptly "
                   "(WSA error: %d) — worker cleaning up.\n",
                   aircraft_id, err);
            break;
        }

        aircraft_id = pkt.aircraft_id;

        if (first_packet) {
            init_flight_record(&pkt);
            first_packet = 0;
        }
        else {
            update_flight_record(&pkt);
        }
    }

    closesocket(client_sock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * handle_client()
 *
 * Legacy pthread wrapper retained for API compatibility with server_core.h.
 * Unmarshals the heap-allocated socket pointer, then delegates to
 * handle_client_socket() — keeping all logic in one place.
 * ═══════════════════════════════════════════════════════════════════════════ */
void* handle_client(void* arg) {
    SOCKET client_sock = *(SOCKET*)arg;
    free(arg);
    handle_client_socket(client_sock);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * worker_thread()
 *
 * Thread pool worker — runs forever until work_queue.shutdown is set.
 * Blocks on work_queue.cond when the queue is empty (zero CPU cost while
 * idle). When signalled, dequeues one socket and handles it.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void* worker_thread(void* arg) {
    (void)arg; /* unused */

    while (1) {
        pthread_mutex_lock(&work_queue.mutex);

        /* Sleep until there is work or a shutdown has been requested */
        while (work_queue.count == 0 && !work_queue.shutdown) {
            pthread_cond_wait(&work_queue.cond, &work_queue.mutex);
        }

        /* Exit cleanly when shutting down and queue is drained */
        if (work_queue.shutdown && work_queue.count == 0) {
            pthread_mutex_unlock(&work_queue.mutex);
            break;
        }

        /* Dequeue one socket (FIFO) */
        SOCKET sock = work_queue.sockets[work_queue.head];
        work_queue.head = (work_queue.head + 1) % WORK_QUEUE_CAPACITY;
        work_queue.count--;

        pthread_mutex_unlock(&work_queue.mutex);

        /* Handle the client — blocks until the aircraft disconnects */
        handle_client_socket(sock);
    }

    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * enqueue_client()
 *
 * Pushes an accepted socket into the work queue and signals one worker.
 * Returns 1 on success, 0 if the queue is full (caller should close socket).
 * ═══════════════════════════════════════════════════════════════════════════ */
static int enqueue_client(SOCKET sock) {
    pthread_mutex_lock(&work_queue.mutex);

    if (work_queue.count >= WORK_QUEUE_CAPACITY) {
        pthread_mutex_unlock(&work_queue.mutex);
        fprintf(stderr, "[Warning] Work queue full (%d/%d) — dropping connection.\n",
                work_queue.count, WORK_QUEUE_CAPACITY);
        return 0;
    }

    work_queue.sockets[work_queue.tail] = sock;
    work_queue.tail = (work_queue.tail + 1) % WORK_QUEUE_CAPACITY;
    work_queue.count++;

    pthread_cond_signal(&work_queue.cond); /* wake one idle worker */
    pthread_mutex_unlock(&work_queue.mutex);
    return 1;
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
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "[Error] WSAStartup failed\n");
        return 1;
    }

    /* ── Mutex initialization ── */
    pthread_mutex_init(&records_mutex, NULL);

    /* ── Thread pool initialization ──────────────────────────────────────
     *
     * 1. Zero-initialize the work queue struct.
     * 2. Init its mutex and condition variable.
     * 3. Spawn THREAD_POOL_SIZE worker threads.
     *    Workers immediately block on work_queue.cond — zero CPU until
     *    a connection arrives.
     * ──────────────────────────────────────────────────────────────────── */
    work_queue.head     = 0;
    work_queue.tail     = 0;
    work_queue.count    = 0;
    work_queue.shutdown = 0;
    pthread_mutex_init(&work_queue.mutex, NULL);
    pthread_cond_init(&work_queue.cond,  NULL);

    pthread_t pool[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&pool[i], NULL, worker_thread, NULL) != 0) {
            fprintf(stderr, "[Error] Failed to create worker thread %d\n", i);
            WSACleanup();
            return 1;
        }
    }

    /* ── Create TCP socket ── */
    SOCKET server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock == INVALID_SOCKET) {
        fprintf(stderr, "[Error] socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    /* ── SO_REUSEADDR — allows immediate restart without TIME_WAIT delay ── */
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    /* ── Bind ── */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons((u_short)port);

    if (bind(server_sock,
             (struct sockaddr*)&server_addr,
             sizeof(server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[Error] bind() failed: %d\n", WSAGetLastError());
        closesocket(server_sock);
        WSACleanup();
        return 1;
    }

    /* ── Listen ── */
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
    printf("||  Thread pool : %-4d workers               ||\n", THREAD_POOL_SIZE);
    printf("||  Queue cap   : %-4d connections            ||\n", WORK_QUEUE_CAPACITY);
    printf("||  Waiting for aircraft connections...       ||\n");
    printf("++--------------------------------------------++\n\n");

    /* ═══════════════════════════════════════════════════════════════════════
     * MAIN ACCEPT LOOP
     *
     * accept() blocks until a client connects, then immediately pushes the
     * new socket into the work queue and signals a worker — no per-connection
     * pthread_create() or malloc() needed.
     * ═══════════════════════════════════════════════════════════════════════ */
    while (1) {

        struct sockaddr_in client_addr;
        int client_addr_len = sizeof(client_addr);

        SOCKET client_sock = accept(server_sock,
                                    (struct sockaddr*)&client_addr,
                                    &client_addr_len);

        if (client_sock == INVALID_SOCKET) {
            fprintf(stderr, "[Warning] accept() failed: %d - continuing.\n",
                    WSAGetLastError());
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        printf("[Server] New connection from %s — queuing for worker.\n", client_ip);

        if (!enqueue_client(client_sock)) {
            /* Queue was full — drop this connection */
            closesocket(client_sock);
        }
    }

    /* ── Cleanup (only reached if accept loop is ever broken) ── */
    closesocket(server_sock);
    pthread_mutex_destroy(&records_mutex);
    pthread_mutex_destroy(&work_queue.mutex);
    pthread_cond_destroy(&work_queue.cond);
    WSACleanup();
    return 0;
}