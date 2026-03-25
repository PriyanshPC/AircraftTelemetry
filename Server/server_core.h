#ifndef SERVER_CORE_H
#define SERVER_CORE_H

/*
 * server_core.h
 * Module 1 — Server Core
 * Owner: Priyansh
 *
 * Exposes:
 *   records_mutex  — global mutex used by DataProcessing module
 *   handle_client  — pthread thread function, one per connected client
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <pthread.h>

 /* Global mutex — defined in server_core.cpp, used in data_processing.cpp */
extern pthread_mutex_t records_mutex;

/* Thread function — one instance per connected client */
void* handle_client(void* arg);

#endif /* SERVER_CORE_H */