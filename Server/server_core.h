#ifndef SERVER_CORE_H
#define SERVER_CORE_H

/*
 * server_core.h
 * Module 1 — Server Core
 * Owner: Priyansh
 *
 * Exposes the global mutex used by data_processing.cpp
 * and the client thread handler function signature.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <pthread.h>

 /* Global mutex — defined in server_core.cpp, used in data_processing.cpp */
extern pthread_mutex_t records_mutex;

/* Thread function — one instance runs per connected client */
void* handle_client(void* arg);

#endif /* SERVER_CORE_H */