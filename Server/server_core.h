#ifndef SERVER_CORE_H
#define SERVER_CORE_H

/*
 * server_core.h
 * Module 1 — Server Core
 * Owner: Priyansh
 *
 * Exposes:
 *   records_mutex  — global mutex protecting the flight_records map
 *                    (defined here, used in data_processing.cpp via extern)
 *
 *   handle_client  — legacy pthread wrapper retained for API compatibility.
 *                    Internally delegates to handle_client_socket().
 *                    The server now uses a fixed thread pool; new connections
 *                    are queued and handled by worker_thread() workers rather
 *                    than by spawning a new thread per connection.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <pthread.h>

/* Global mutex — defined in server_core.cpp, used in data_processing.cpp */
extern pthread_mutex_t records_mutex;

/*
 * handle_client()
 * Legacy pthread wrapper. Takes a heap-allocated SOCKET* (malloc'd by
 * caller, freed here) and delegates to handle_client_socket().
 * Kept for backward compatibility; the thread pool calls
 * handle_client_socket() directly.
 */
void* handle_client(void* arg);

#endif /* SERVER_CORE_H */