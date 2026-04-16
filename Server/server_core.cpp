/**
 * @file server_core.cpp
 * @brief Implementation of Module 1 — Server Core (Connection & Thread Management).
 *
 * @details
 * Entry point for the Aircraft Telemetry Server executable.  Responsible for:
 *  - Initialising Winsock and the global POSIX mutex.
 *  - Spawning a fixed-size thread pool of @c THREAD_POOL_SIZE worker threads.
 *  - Creating and binding a TCP socket, then entering the accept loop.
 *  - Pushing each accepted connection into a circular work queue.
 *  - Worker threads dequeue sockets and call @c handle_client_socket() to
 *    receive all packets for that aircraft until it disconnects.
 *
 * **Startup sequence:**
 *  1. @c WSAStartup()       — initialise Winsock 2.2
 *  2. @c pthread_mutex_init() — initialise @c records_mutex
 *  3. Thread pool init      — zero work queue, init its mutex/cond, spawn workers
 *  4. @c socket()           — create TCP socket
 *  5. @c setsockopt()       — set @c SO_REUSEADDR for fast post-crash restart
 *  6. @c bind()             — attach to specified port on all interfaces
 *  7. @c listen()           — mark socket as passive
 *  8. Accept loop           — @c accept() → @c enqueue_client() → worker wakes
 *
 * **Thread pool optimization (Project 6 — endurance test finding):**
 *
 * The original design spawned one @c pthread per accepted connection.
 * Under a 1,000-client load test this created up to 1,000 simultaneous threads;
 * each thread on Windows carries a default stack of ~1 MB, totalling ~1 GB of
 * committed virtual memory at peak.  Over the 71-minute endurance test, threads
 * accumulated faster than the OS reclaimed their stacks between waves, producing
 * the 81 MB/hr memory growth that violated SRS requirement PR-S-004
 * (memory growth ≤ 5%/hr).
 *
 * The thread pool solution:
 *  - @c THREAD_POOL_SIZE persistent workers are created once at startup.
 *  - Each worker blocks on @c work_queue.cond (zero CPU while idle).
 *  - Accepted sockets are pushed into a circular FIFO queue (capacity
 *    @c WORK_QUEUE_CAPACITY) — no per-connection @c malloc() or
 *    @c pthread_create() needed.
 *  - Memory is bounded to @c THREAD_POOL_SIZE stacks regardless of burst size.
 *
 * @ingroup ServerCore
 *
 * @author  Group 7 — Priyansh Chaudhary
 * @date    2025
 * @version 2.0  (Project 6 — thread pool)
 */

#ifndef _WIN32_WINNT
/** @brief Minimum Windows version: Windows 7 (required for Winsock 2.2). */
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
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Global mutex protecting the shared @c flight_records map.
 *
 * @details
 * Defined here and referenced by @c data_processing.cpp via @c extern
 * (declared in @c server_core.h).  Must be held whenever the
 * @c flight_records unordered_map is read or modified.
 *
 * Initialised by @c pthread_mutex_init() at the start of @c main();
 * destroyed by @c pthread_mutex_destroy() on shutdown.
 */
pthread_mutex_t records_mutex;

/* ═══════════════════════════════════════════════════════════════════════════
 * THREAD POOL CONFIGURATION
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Number of persistent worker threads in the thread pool.
 *
 * @details
 * 128 workers can handle I/O for 128 concurrent aircraft simultaneously
 * while keeping total stack memory bounded to 128 × ~1 MB = ~128 MB.
 * Tune upward if profiling shows all workers are consistently busy while
 * the work queue is non-empty.
 */
#define THREAD_POOL_SIZE     128

/**
 * @brief Maximum number of accepted sockets that can queue while all workers are busy.
 *
 * @details
 * A value of 2,048 comfortably absorbs a burst of 1,000 simultaneous clients
 * (the load-test scenario) with room to spare.  Connections beyond this limit
 * are dropped immediately with a warning logged to @c stderr.
 */
#define WORK_QUEUE_CAPACITY  2048

/**
 * @brief Circular FIFO queue used to pass accepted sockets to worker threads.
 *
 * @details
 * The main accept loop calls @c enqueue_client() to push sockets in;
 * @c worker_thread() instances call @c pthread_cond_wait() and then dequeue
 * one socket at a time.  All access to this struct is serialised by its
 * embedded @c mutex.
 */
typedef struct {
    SOCKET          sockets[WORK_QUEUE_CAPACITY]; /**< Circular socket buffer.       */
    int             head;                         /**< Next dequeue position (0-based). */
    int             tail;                         /**< Next enqueue position (0-based). */
    int             count;                        /**< Number of items currently queued. */
    int             shutdown;                     /**< Set to 1 to signal workers to exit. */
    pthread_mutex_t mutex;                        /**< Guards all fields of this struct. */
    pthread_cond_t  cond;                         /**< Signalled when an item is enqueued. */
} WorkQueue;

/** @brief The single global work queue instance shared by all worker threads. */
static WorkQueue work_queue;

/* ═══════════════════════════════════════════════════════════════════════════
 * INTERNAL FUNCTIONS
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Core client-handling logic: receive telemetry packets until disconnect.
 *
 * @details
 * This is where all per-aircraft I/O happens.  Called directly by
 * @c worker_thread() for each dequeued socket.
 *
 * **Receive loop logic:**
 *  1. Call @c recv() with @c MSG_WAITALL to block until exactly
 *     @c sizeof(TelemetryPacket) bytes are available (guarantees atomic
 *     delivery of a complete packet even if the OS segments the TCP stream).
 *  2. @c recv() returns **0**  → clean EOF: client called @c closesocket().
 *     Call @c finalize_flight() then break.
 *  3. @c recv() returns **< 0** → abrupt disconnect or network error.
 *     Log the Winsock error code and break (do not call @c finalize_flight()
 *     because the flight data is incomplete).
 *  4. **First packet** → @c init_flight_record().
 *  5. **Subsequent packets** → @c update_flight_record().
 *  6. Call @c closesocket() before returning so the worker is ready for the
 *     next socket immediately.
 *
 * @param[in] client_sock  The accepted TCP socket for this aircraft connection.
 *                         Ownership transfers to this function; it is closed
 *                         before the function returns.
 */
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

/**
 * @brief Legacy @c pthread_create() wrapper for API compatibility.
 *
 * @details
 * Unmarshals the heap-allocated @c SOCKET* passed as @p arg, frees the
 * allocation (ownership transfer), and delegates to @c handle_client_socket().
 * All receive-loop logic lives in @c handle_client_socket() so there is a
 * single implementation regardless of how the call reaches it.
 *
 * @param[in] arg  Heap-allocated @c SOCKET* cast to @c void*.
 *                 Freed by this function.
 *
 * @return Always @c NULL (required by @c pthread_create() start-routine ABI).
 *
 * @deprecated The server no longer calls this function.  New connections are
 *             dispatched through @c worker_thread() which calls
 *             @c handle_client_socket() directly.
 */
void* handle_client(void* arg) {
    SOCKET client_sock = *(SOCKET*)arg;
    free(arg);
    handle_client_socket(client_sock);
    return NULL;
}

/**
 * @brief Thread pool worker — dequeues sockets and handles them one at a time.
 *
 * @details
 * Each instance runs an infinite loop:
 *  1. Acquire @c work_queue.mutex.
 *  2. While the queue is empty and @c shutdown is not set, call
 *     @c pthread_cond_wait() — this releases the mutex and suspends the
 *     thread at zero CPU cost until @c enqueue_client() signals the condvar.
 *  3. On wakeup: if @c shutdown is set and the queue is empty, unlock and
 *     break (graceful exit).
 *  4. Dequeue one socket (FIFO: read from @c head, advance @c head mod capacity).
 *  5. Release @c work_queue.mutex.
 *  6. Call @c handle_client_socket() — this blocks until the aircraft
 *     disconnects, then returns.
 *  7. Loop back to step 1.
 *
 * @param[in] arg  Unused (satisfies @c pthread_create() signature).
 *
 * @return Always @c NULL.
 */
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

/**
 * @brief Enqueue an accepted client socket into the work queue.
 *
 * @details
 * Acquires @c work_queue.mutex, checks for capacity, writes the socket into
 * the circular buffer at @c tail, advances @c tail, increments @c count,
 * signals @c work_queue.cond to wake one idle worker, then releases the mutex.
 *
 * If the queue is already at capacity (@c count >= @c WORK_QUEUE_CAPACITY)
 * a warning is logged to @c stderr and the function returns 0 without
 * enqueuing.  The caller is responsible for closing the socket in that case.
 *
 * @param[in] sock  An accepted @c SOCKET returned by @c accept().
 *
 * @return @c 1 if the socket was successfully enqueued.
 * @return @c 0 if the queue was full; the socket was NOT enqueued and the
 *              caller must close it.
 */
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
 * MAIN ENTRY POINT
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Server entry point — initialise, bind, and run the accept loop.
 *
 * @details
 * **Usage:** @code Server.exe <port> @endcode
 *
 * Startup sequence:
 *  1. Parse command-line port argument.
 *  2. Call @c WSAStartup(MAKEWORD(2,2)) to initialise Winsock.
 *  3. Initialise @c records_mutex via @c pthread_mutex_init().
 *  4. Zero-initialise the @c WorkQueue, init its mutex and condvar, and
 *     spawn @c THREAD_POOL_SIZE worker threads (all immediately block on
 *     @c work_queue.cond, consuming zero CPU).
 *  5. Create a TCP stream socket.
 *  6. Set @c SO_REUSEADDR so the server can restart immediately after a
 *     crash without waiting for the OS @c TIME_WAIT timeout to expire.
 *  7. Bind to @c INADDR_ANY on the specified port.
 *  8. Call @c listen() with @c SOMAXCONN.
 *  9. Enter the accept loop: @c accept() → log → @c enqueue_client().
 *     If the queue is full, close the socket immediately.
 * 10. On loop exit (currently unreachable): clean up mutexes, condvar,
 *     server socket, and Winsock.
 *
 * @param[in] argc  Argument count; must be >= 2.
 * @param[in] argv  @c argv[1] is the TCP port number as a decimal string.
 *
 * @return @c 0 on clean exit (unreachable in normal operation).
 * @return @c 1 on startup error (bad arguments, Winsock failure, socket/bind/listen error).
 */
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
