/**
 * @file server_core.h
 * @brief Public interface for Module 1 — Server Core (Connection & Thread Management).
 *
 * @details
 * Exposes two symbols that other translation units need to reference:
 *
 *  - **@c records_mutex** — global POSIX mutex protecting the shared
 *    @c flight_records map maintained by @c data_processing.cpp.
 *    Defined in @c server_core.cpp; declared @c extern here so that
 *    @c data_processing.cpp can acquire and release it without requiring
 *    an additional header dependency.
 *
 *  - **@c handle_client()** — legacy @c pthread_create() entry-point wrapper,
 *    retained for API compatibility.  The server now uses a bounded thread
 *    pool; accepted sockets are pushed into a circular work queue and handled
 *    by persistent @c worker_thread() instances rather than by spawning a new
 *    thread per connection.
 *
 * **Thread pool design (Project 6 optimization):**
 * The original design created one @c pthread per accepted connection.  Under
 * a 1,000-client load test this caused up to 1,000 simultaneous threads, each
 * carrying a ~1 MB default stack on Windows — ~1 GB committed virtual memory
 * at peak — and drove the 81 MB/hr memory growth observed in the endurance
 * test.  The thread pool caps memory to @c THREAD_POOL_SIZE stacks regardless
 * of client burst size.
 *
 * @ingroup ServerCore
 *
 * @author  Group 7 — Priyansh Chaudhary
 * @date    2025
 * @version 2.0  (Project 6 — thread pool)
 */

#ifndef SERVER_CORE_H
#define SERVER_CORE_H

#ifndef _WIN32_WINNT
/** @brief Minimum Windows version: Windows 7 (required for Winsock 2.2). */
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <pthread.h>

/**
 * @brief Global mutex protecting the shared @c flight_records map.
 *
 * @details
 * **Defined** in @c server_core.cpp; declared @c extern here so that
 * @c data_processing.cpp can reference it without a circular dependency.
 *
 * This mutex must be held whenever the @c flight_records
 * (@c std::unordered_map<uint32_t, FlightRecord>) is read or written.
 * All three Data Processing functions (@c init_flight_record(),
 * @c update_flight_record(), @c finalize_flight()) acquire and release
 * it internally — callers do not need to lock it explicitly.
 *
 * Initialised by @c pthread_mutex_init() early in @c main().
 */
extern pthread_mutex_t records_mutex;

/**
 * @brief Legacy @c pthread_create() entry-point wrapper (retained for API compatibility).
 *
 * @details
 * Accepts a heap-allocated @c SOCKET* passed by the caller (malloc'd before
 * @c pthread_create() was the accepted pattern), frees that pointer, and
 * delegates to the internal @c handle_client_socket() function which contains
 * all actual receive-loop logic.
 *
 * **This function is no longer called by the server's main loop.**
 * The current architecture enqueues accepted sockets via @c enqueue_client()
 * and dispatches them to @c worker_thread() workers which call
 * @c handle_client_socket() directly (no heap allocation required).
 * @c handle_client() is kept so that any external code or test harness that
 * relied on the original @c pthread_create(handle_client, socket_ptr) pattern
 * continues to compile and run correctly.
 *
 * @param[in] arg  Heap-allocated @c SOCKET* cast to @c void*.
 *                 Freed by this function before returning.
 *
 * @return Always @c NULL (satisfies the @c pthread_create() start-routine
 *         signature; the return value is never used).
 *
 * @note Ownership of the memory pointed to by @p arg transfers to this
 *       function; the caller must not free it after passing it here.
 */
void* handle_client(void* arg);

#endif /* SERVER_CORE_H */
