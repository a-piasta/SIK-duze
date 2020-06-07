#include "http_connection.h"

#include "client_protocol.h"
#include "utils.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_UDP_MSG_SIZE 0x400
#define MAX_UDP_DATA_LEN (0x400 - CLIENT_PROTO_DGRAM_HEADER_LEN)

static char udp_buffer[MAX_UDP_MSG_SIZE] __attribute__((aligned(_Alignof(struct client_protocol_dgram))));

extern volatile sig_atomic_t cont;
extern pthread_mutex_t client_mutex;

static const char *ok_answer[] = {
  "ICY 200 OK\r\n",
  "HTTP/1.0 200 OK\r\n",
  "HTTP/1.1 200 OK\r\n"
};

static const char *prefixes[] = {
  "icy-metaint:",
  "icy-name:"
};

static const int prefixes_size[] = {12, 9};

static bool parse_first_line(const char *buffer, size_t size) {
  for (size_t i = 0; i < SIZE(ok_answer); ++i) {
    if (size < strlen(ok_answer[i])) continue;
    if (strncmp(buffer, ok_answer[i], strlen(ok_answer[i])) == 0)
      return true;
  }
  return false;
}

static ssize_t write_exact(int fd, const void *buffer, size_t size) {
  size_t pos = 0;
  while (pos < size) {
    ssize_t ret = write(fd, (void *)((char *)buffer + pos), size - pos);
    if (ret < 0) {
      if (errno == ENOSPC || errno == EDQUOT) continue;
      return -1;
    }
    pos += ret;
  }
  return pos;
}

void send_http_request(int sock, const char *resource, bool metadata) {
  size_t len = strlen(resource) + 100;
  char buffer[len];
  size_t bytes = 0;
  int ret;
  if ((ret = snprintf(buffer, len, "GET %s HTTP/1.0\r\n", resource)) < 0) goto handle_error;
  bytes += ret;
  if (metadata) {
    if ((ret = snprintf(buffer + bytes, len - bytes, "Icy-Metadata:1\r\n")) < 0)
      goto handle_error;
    bytes += ret;
  }
  if ((ret = snprintf(buffer + bytes, len - bytes, "\r\n")) < 0) goto handle_error;
  bytes += ret;

  size_t pos = 0;
  do {
    ssize_t r = write(sock, buffer + pos, bytes - pos);
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
      goto handle_error;
    }
    pos += r;
  } while (pos < bytes);

  return;

  handle_error:

  close(sock);
  exit(-1);
}

int receive_http_header(int sock, long *icy_metaint, char **icy_name,
                        size_t *icy_name_len, char *buffer, size_t buffer_len,
                        size_t *received_data) {
  ssize_t len;
  unsigned line = 0;
  size_t line_pos = 0;
  size_t pos = 0;
  size_t last_CRLF_pos = 0;
  bool is_name = false, is_current_name = false, cont = true;
  *icy_name_len = 0;

  while (cont) {
    len = read(sock, buffer + pos, buffer_len - pos);
    if (len < 0) {
      break;
    }
    if (len == 0) cont = false;
    pos += len;
    if (pos < buffer_len / 2) continue;
    if (line == 0 && line_pos == 0 && !parse_first_line(buffer, pos))
      return -1;

    /* iterujemy się do pos - 30, aby nie mieć problemu z wyjściem poza bufor
     * przy sprawdzaniu prefiksów linii */
    for (size_t i = 0; i < pos - 30; ++i) {
      if (line_pos == 0) {
        if (strncasecmp(prefixes[0], buffer + i, prefixes_size[0]) == 0) {
          *icy_metaint = strtol(buffer + i + prefixes_size[0], NULL, 0);
        } else {
          if (strncasecmp(prefixes[1], buffer + i, prefixes_size[1]) == 0) {
            //printf("found name\n");
            is_name = true;
            is_current_name = false;
          }
        }
      }

      if (buffer[i] == '\r' && buffer[i + 1] == '\n') {
        if (line_pos == 0) { // only CRLF in line
          *received_data = pos - i - 2;
          goto correct_end;
        }

        last_CRLF_pos = i + 2;
        line++;

        if (is_name) {
          if (line_pos > *icy_name_len) {
            char *name = realloc(*icy_name, line_pos);
            if (name == NULL) return -1;
            *icy_name = name;
          }

          if (is_current_name) {
            memcpy(*icy_name + *icy_name_len, buffer, i);
          } else {
            memcpy(*icy_name, buffer + i - line_pos, line_pos);
          }

          *icy_name_len = line_pos;
        }

        i++;
        line_pos = 0;
        is_name = false;
        continue;
      }
      line_pos++;
    }

    if (last_CRLF_pos > 0) {
      pos -= last_CRLF_pos;
      memmove(buffer, buffer + last_CRLF_pos, pos);
      last_CRLF_pos = 0;
    } else {
      if (is_name) {
        if (line_pos < *icy_name_len) {
          char *name = realloc(*icy_name, line_pos);
          if (name == NULL) return -1;
          *icy_name = name;
        }

        if (is_current_name) {
          memcpy(*icy_name + *icy_name_len, buffer, pos);
        } else {
          memcpy(*icy_name, buffer + pos - line_pos, line_pos);
        }

        *icy_name_len = line_pos;
      }
      is_current_name = true;
      pos = 0;
    }
  }

  return -1;

  correct_end:

  memmove(buffer, buffer + pos - *received_data, *received_data);
  return 0;
}

int send_udp_data(int sock, uint16_t type, char *buffer, size_t len) {
  size_t pos = 0;
  while (pos < len) {
    struct client_protocol_dgram *dgram = (struct client_protocol_dgram *) udp_buffer;
    dgram->type = htons(type);
    uint16_t length = MIN(len - pos, MAX_UDP_DATA_LEN);
    dgram->length = htons(length);
    memcpy(dgram->data, buffer + pos, length);

    if (pthread_mutex_lock(&mutex) != 0) exit(1);

    FOR_LIST(c, client_list) {
      ssize_t len = sendto(sock, dgram, length + CLIENT_PROTO_DGRAM_HEADER_LEN, 0,
                           (struct sockaddr *)&c->client_address,
                           (socklen_t) sizeof(c->client_address));

      /* if an error occurred, it's probably a strange bug on our side
       * and we don't even know what is a state of a program */
      if (len != (ssize_t)(length + CLIENT_PROTO_DGRAM_HEADER_LEN)) {
        if (pthread_mutex_unlock(&mutex) != 0) exit(1);
      }
    }

    if (pthread_mutex_unlock(&mutex) != 0) exit(1);
    pos += length;
  }
  return 0;
}

int receive_http_data(int sock, long icy_metaint, char *buffer,
                      size_t buffer_len, size_t data_len, int client_sock) {
  if (icy_metaint < 0) { // no metadata
    for (;;) {
      if (client_sock == -1) {
        if (write_exact(STDOUT_FILENO, buffer, data_len) < 0) return -1; // failed write to stdout
      } else {
        if (send_udp_data(client_sock, AUDIO, buffer, data_len) < 0) return -1;
      }
      if (!cont) break;
      ssize_t len = read(sock, buffer, buffer_len);
      if (len < 0) return -1; // strange error or timeout
      data_len = len;
    }
  } else {
    bool metadata = false;
    size_t chunk = icy_metaint;
    size_t pos = 0;

    while (pos < data_len) {
      if (metadata) {
        chunk = (ssize_t)(unsigned char)buffer[pos++];
        chunk *= 16;
      }

      size_t bytes = MIN(chunk, data_len - pos);

      if (bytes > 0) {
        if (client_sock == -1) {
          if (write_exact(metadata ? STDERR_FILENO : STDOUT_FILENO, buffer + pos, bytes) < 0)
            return -1;
        } else {
          if (send_udp_data(client_sock, metadata ? METADATA : AUDIO, buffer + pos, bytes) < 0)
            return -1;
        }
        chunk -= bytes;
        pos += bytes;
      }

      if (chunk == 0) {
        metadata = !metadata;
        if (!metadata) chunk = icy_metaint;
      }
    }

    while (cont) {
      if (metadata) {
        unsigned char chunk_size;
        ssize_t len = read(sock, &chunk_size, 1);
        if (len <= 0) return -1;
        chunk = chunk_size;
        chunk *= 16;

        if (chunk == 0) {
          metadata = false;
          chunk = icy_metaint;
        }
      }

      do {
        ssize_t bytes = read(sock, buffer, MIN(chunk, buffer_len));
        if (bytes < 0) return -1;
        if (client_sock == -1) {
          if (write_exact(metadata ? STDERR_FILENO : STDOUT_FILENO, buffer, bytes) < 0)
            return -1;
        } else {
          if (send_udp_data(client_sock, metadata ? METADATA : AUDIO, buffer, bytes) < 0)
            return -1;
        }
        chunk -= bytes;
      } while (chunk > 0);

      metadata = !metadata;
      if (!metadata) chunk = icy_metaint;
    }
  }

  return 0;
}
