#include "client_protocol.h"
#include "utils.h"
#include "telnet.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PROXY           20
#define METADATA_BUFFER_LEN 80

char *hostaddr = NULL;
char *proxy_port = NULL;
char *telnet_port = NULL;
unsigned timeout = 5;

struct addrinfo addr_hints, *addr_result;

struct proxy {
  struct sockaddr_in address;
  char *name;
  size_t name_len;
};

unsigned chosen_proxy = 0;
unsigned active_proxy = 0;
_Atomic unsigned marked_line = 0;
time_t last_data;
struct proxy proxy[MAX_PROXY + 1];
size_t metadata_len = 0;
char metadata[METADATA_BUFFER_LEN];

pthread_mutex_t telnet_mutex = PTHREAD_MUTEX_INITIALIZER;

bool cont = true;

static void print_usage(char *prog_name) {
  fprintf(stderr, "Usage: %s -H hostaddr -P proxy_port -p telnet_port [-T timeout]\n", prog_name);
}

static void parse_parameters(int argc, char *argv[]) {
  int opt;

  while ((opt = getopt(argc, argv, "H:P:p:T:")) != -1) {
    switch (opt) {
      case 'H':
        hostaddr = optarg;
        break;
      case 'P':
        proxy_port = optarg;
        break;
      case 'p':
        telnet_port = optarg;
        break;
      case 'T':
        timeout = atoi(optarg);
        break;
      default: /* '?' */
        print_usage(argv[0]);
        exit(1);
    }
  }
}

static int tcp_errno_retry[] = {
  ENETDOWN,
  EPROTO,
  ENOPROTOOPT,
  EHOSTDOWN,
  ENONET,
  EHOSTUNREACH,
  EOPNOTSUPP,
  ENETUNREACH
};

#define TELNET_BUFFER_SIZE 50

static bool is_good(int err_num) {
  for (size_t i = 0; i < SIZE(tcp_errno_retry); ++i) {
    if (err_num == tcp_errno_retry[i])
      return true;
  }
  return false;
}

static void *send_keepalive(void *arg) {
  int sock = *(int *)arg;
  struct client_protocol_dgram dgram;
  dgram.type = htons(KEEPALIVE);
  dgram.length = htons(0);
  while (cont) {
    if (chosen_proxy > 0) {
      ssize_t ret = sendto(sock, &dgram, CLIENT_PROTO_DGRAM_HEADER_LEN, 0,
                           (struct sockaddr *) &proxy[chosen_proxy].address,
                           (socklen_t) sizeof(proxy[chosen_proxy].address));
      if (ret != CLIENT_PROTO_DGRAM_HEADER_LEN)
        perror("sendto");
    }
    if (cont) {
      sleep(3);
      usleep(500000);
    }
  }
  return NULL;
}

static void update(int sock) {
  int err;
  if ((err = pthread_mutex_lock(&telnet_mutex)) != 0) {
    errno = err;
    perror("pthread_mutex_lock");
    return;
  }
  ssize_t ret;
  ret = write(sock, CLRSCR, CLRSCR_LEN);
  if (ret != CLRSCR_LEN) goto end;

  ret = write(sock, MOVE_LEFT_UP, MOVE_LEFT_UP_LEN);
  if (ret != MOVE_LEFT_UP_LEN) goto end;
  for (unsigned i = 0; i < active_proxy + 3; ++i) {
    if (i == marked_line) {
      ret = write(sock, UNDERSCORE, UNDERSCORE_LEN);
      if (ret != UNDERSCORE_LEN) goto end;
    }
    if (i == 0) {
      ret = write(sock, szukaj, SZUKAJ_LEN);
      if (ret != SZUKAJ_LEN) goto end;
    }
    if (i > 0 && i <= active_proxy) {
      ret = write(sock, posrednik, POSREDNIK_LEN);
      if (ret != POSREDNIK_LEN) goto end;

      ret = write(sock, proxy[i].name, proxy[i].name_len);
      if (ret < 0 || (size_t) ret != proxy[i].name_len) goto end;

      if (i == chosen_proxy) {
        ret = write(sock, " *", 2);
        if (ret != 2) goto end;
      }
    }
    if (i == active_proxy + 1) {
      ret = write(sock, koniec, KONIEC_LEN);
      if (ret != KONIEC_LEN) goto end;
    }
    if (i == active_proxy + 2) {
      ret = write(sock, metadata, metadata_len);
      if (ret < 0 || (size_t) ret != metadata_len) goto end;
    }
    ret = write(sock, ENDL, ENDL_LEN);
    if (ret != ENDL_LEN) goto end;
    if (i == marked_line) {
      ret = write(sock, NO_ATTR, NO_ATTR_LEN);
      if (ret != NO_ATTR_LEN) goto end;
    }
  }
  end:
  if ((err = pthread_mutex_unlock(&telnet_mutex)) != 0) {
    errno = err;
    perror("pthread_mutex_unlock");
    return;
  }
}

void pass_metadata(int sock, const struct client_protocol_dgram *dgram) {
  int err;
  if ((err = pthread_mutex_lock(&telnet_mutex)) != 0) {
    errno = err;
    perror("pthread_mutex_lock");
    return;
  }

  metadata_len = MIN(ntohs(dgram->length), METADATA_BUFFER_LEN);
  memcpy(metadata, dgram->data, metadata_len);

  if ((err = pthread_mutex_unlock(&telnet_mutex)) != 0) {
    errno = err;
    perror("pthread_mutex_unlock");
  }
  update(sock);
}

static int telnet_communication_routine(int telnet_sock, int proxy_sock) {
  marked_line = 0;
  ssize_t ret = write(telnet_sock, CHANGE_MODE, CHANGE_MODE_LEN);
  if (ret != CHANGE_MODE_LEN) {
    perror("write");
    close(telnet_sock);
    return 1;
  }
  update(telnet_sock);
  /* Na moim komputerze (na studentsie też) telnet na prośbę o zmianę trybu
   * odpowiada w następujący sposób: (trzy pierwsze ready z odpowiedzią)
   * "\xff\xfd\x3\xff\xfb\x22\xff\xfa\x22\x3\x1\x0\x0\x3\x62\x3\x4\x2\xf\x5",
   * "\x0\x0\x7\x62\x1c\x8\x2\x4\x9\x42\x1a\xa\x2\x7f\xb\x2\x15\xf\x2\x11",
   * "\x10\x2\x13\x11\x0\x0\x12\x0\x0\xff\xf0\xff\xfd\x1"
   * Nie wiem, co z tym zrobić (nawet nie wiem, czy mogę założyć, że zawsze
   * zachowa się w ten sposób), więc ignoruję.
   */
  char buffer[TELNET_BUFFER_SIZE];
  int retval = 0;
  while (1) {
    ssize_t c = read(telnet_sock, buffer, TELNET_BUFFER_SIZE);
    if (c == 0) break; // end of connection
    if (c < 0) {
      perror("read");
      continue;
    }
    if (c == msg_len[UP] && strncmp(buffer, message[UP], c) == 0) {
      if (marked_line > 0) {
        marked_line--;
        update(telnet_sock);
      }
    }
    if (c == msg_len[DOWN] && strncmp(buffer, message[DOWN], c) == 0) {
      if (marked_line < active_proxy + 1) {
        marked_line++;
        update(telnet_sock);
      }
    }
    if (c == msg_len[CRLF] && strncmp(buffer, message[CRLF], c) == 0) {
      if (marked_line <= active_proxy) {
        struct client_protocol_dgram dgram;
        dgram.type = htons(DISCOVER);
        dgram.length = htons(0);
        ssize_t ret;
        if (marked_line == 0) {
          ret = sendto(proxy_sock, &dgram, CLIENT_PROTO_DGRAM_HEADER_LEN, 0,
                       addr_result->ai_addr, addr_result->ai_addrlen);
        } else {
          ret = sendto(proxy_sock, &dgram, CLIENT_PROTO_DGRAM_HEADER_LEN, 0,
                       (struct sockaddr *) &proxy[marked_line].address,
                       (socklen_t) sizeof(proxy[marked_line].address));
          chosen_proxy = marked_line;
          update(telnet_sock);
        }
        if (ret != CLIENT_PROTO_DGRAM_HEADER_LEN) {
          perror("sendto");
          continue;
        }
      } else { // koniec
        break;
      }
    }
  }
  close(telnet_sock);
  return(retval);
}

static char udp_buffer[UDP_BUFFER_LEN] __attribute__((aligned(_Alignof(struct client_protocol_dgram))));

void *proxy_routine(void *arg) {
  int *sockets = (int *)arg;
  int sock = sockets[0];
  int telnet_sock = sockets[1];
  struct sockaddr_in proxy_address;
  socklen_t proxy_addrlen;

  while (cont) {
    proxy_addrlen = (socklen_t) sizeof(proxy_address);
    ssize_t len = recvfrom(sock, udp_buffer, UDP_BUFFER_LEN, MSG_DONTWAIT,
                           (struct sockaddr *)&proxy_address, &proxy_addrlen);
    struct client_protocol_dgram *dgram = (struct client_protocol_dgram *) udp_buffer;
    uint16_t type;
    if (len >= 0) type = ntohs(dgram->type);
    else type = NONE;

    if ((size_t) len < CLIENT_PROTO_DGRAM_HEADER_LEN) continue; // strange message - ignore

    uint16_t length = ntohs(dgram->length);
    if (len - CLIENT_PROTO_DGRAM_HEADER_LEN != length) continue;
    switch (type) {
      case AUDIO:
      case METADATA:
        if (chosen_proxy > 0 && is_same_address(&proxy_address, &proxy[chosen_proxy].address)) {
          last_data = time(NULL);
          if (type == AUDIO)
            fwrite(udp_buffer + CLIENT_PROTO_DGRAM_HEADER_LEN, 1, length, stdout);
          else
            pass_metadata(telnet_sock, dgram);
        }
        break;
      case IAM:;
        bool found = false;
        for (unsigned i = 1; i <= active_proxy; ++i) {
          if (is_same_address(&proxy_address, &proxy[i].address)) {
            found = true;
            if (proxy[i].name_len != length || strncmp(proxy[i].name, dgram->data, length) != 0) {
              char *new_name = realloc(proxy[i].name, length);
              if (!new_name) {
                perror("malloc");
                break;
              }
              proxy[i].name = new_name;
              memcpy(proxy[i].name, dgram->data, length);
              proxy[i].name_len = length;
              update(telnet_sock);
            }
          }
        }
        if (!found && active_proxy < MAX_PROXY) {
          char *name_buf = malloc(sizeof(length));
          if (name_buf) {
            active_proxy++;
            proxy[active_proxy].address = proxy_address;
            proxy[active_proxy].name = name_buf;
            proxy[active_proxy].name_len = length;
            memcpy(proxy[active_proxy].name, dgram->data, length);
            update(telnet_sock);
          }
        } // no break here (intended)
      default:
        if (chosen_proxy > 0) {
          // check if chosen radio-proxy timeouted
          time_t current_time = time(NULL);
          if (current_time - last_data > timeout) {
            if (chosen_proxy < active_proxy) {
              free(proxy[chosen_proxy].name);
              proxy[chosen_proxy] = proxy[active_proxy];
              proxy[active_proxy].name = NULL;
            } else {
              free(proxy[active_proxy].name);
              proxy[active_proxy].name = NULL;
            }
            chosen_proxy = 0;
            active_proxy--;
            update(telnet_sock);
          }
        }
    }
  }
  return NULL;
}

int main(int argc, char *argv[]) {
  parse_parameters(argc, argv);
  if (!hostaddr || !proxy_port || !telnet_port) {
    print_usage(argv[0]);
    exit(1);
  }

  addr_hints.ai_family = AF_INET;
  addr_hints.ai_socktype = SOCK_DGRAM;

  int err;
  if ((err = getaddrinfo(hostaddr, proxy_port, &addr_hints, &addr_result)) != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
    exit(1);
  }

  uint16_t telnet_port_num = convert(telnet_port);

  int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_sock < 0) {
    perror("socket");
    exit(1);
  }

  int proxy_sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (proxy_sock < 0) {
    perror("socket");
    close(listen_sock);
    exit(1);
  }

  int optval = 1;
  if (setsockopt(proxy_sock, SOL_SOCKET, SO_BROADCAST, &optval, sizeof(optval)) < 0) {
    perror("setsockopt");
    close(listen_sock);
    exit(1);
  }

  if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
    perror("setsockopt - can't reuse addr");
  }

  struct sockaddr_in local_address;
  local_address.sin_family = AF_INET;
  local_address.sin_addr.s_addr = htonl(INADDR_ANY);
  local_address.sin_port = htons(telnet_port_num);

  if (bind(listen_sock, (struct sockaddr *) &local_address,
           (socklen_t) sizeof(local_address)) < 0)
    goto handle_errors;

  if (listen(listen_sock, 5) < 0) goto handle_errors;

  pthread_t keepalive_thread, proxy_thread;
  err = pthread_create(&keepalive_thread, NULL, &send_keepalive, &proxy_sock);
  if (err != 0) goto handle_errors;

  while (1) {
    struct sockaddr telnet_addr;
    socklen_t telnet_addrlen;
    int telnet_sock = accept(listen_sock, &telnet_addr, &telnet_addrlen);
    if (telnet_sock < 0) {
      if (is_good(errno)) {
        errno = 0;
        continue;
      } else {
        goto handle_errors;
      }
    }
    int sockets[2] = {proxy_sock, telnet_sock};
    err = pthread_create(&proxy_thread, NULL, &proxy_routine, sockets);
    if (err != 0) {
      cont = false;
      perror("pthread_create");
      if ((err = pthread_join(keepalive_thread, NULL)) != 0) {
        errno = err;
        perror("pthread_join");
      }
      if (close(telnet_sock < 0)) perror("close");
      goto handle_errors;
    }
    if (telnet_communication_routine(telnet_sock, proxy_sock) == 0) {
      cont = false;
      break;
    }
  }

  int r = 0;

  if ((err = pthread_join(proxy_thread, NULL)) != 0) {
    errno = err;
    perror("pthread_join");
    r = 1;
  }
  if ((err = pthread_join(keepalive_thread, NULL)) != 0) {
    errno = err;
    perror("pthread_join");
    r = 1;
  }
  if (close(proxy_sock) < 0) {
    r = 1;
    perror("close");
  }
  if (close(listen_sock) < 0) {
    r = 1;
    perror("close");
  }
  exit(r);

  handle_errors:
  close(proxy_sock);
  close(listen_sock);
  if ((err = pthread_mutex_destroy(&telnet_mutex)) != 0) {
    errno = err;
    perror("pthread_mutex_destroy");
  }
}
