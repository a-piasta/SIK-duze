#ifndef _RADIO_HTTP_CONNECTION_H_
#define _RADIO_HTTP_CONNECTION_H_

#include <stdbool.h>
#include <stdio.h>

void send_http_request(int sock, const char *resource, bool metadata);

/* additional data (after CRLF that ends a header is being stored in buffer
 * and its size is being stored in *received_data       */
int receive_http_header(int sock, long *icy_metaint, char **icy_name,
                        size_t *icy_name_len, char *buffer, size_t buffer_len,
                        size_t *received_data);

/* set client_sock to -1 if audio/metadata should be passed to stdout/stderr
 * instead of clients        */
int receive_http_data(int sock, long icy_metaint, char *buffer,
                      size_t buffer_len, size_t data_len, int client_sock);

#endif  // _RADIO_HTTP_CONNECTION_H_
