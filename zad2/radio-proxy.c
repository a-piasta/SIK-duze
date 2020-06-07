#include <sys/types.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include "client_protocol.h"
#include "http_connection.h"
#include "utils.h"

#define BUFFER_LEN      0x1000

char *hostname = NULL;
char *resource = NULL;
char *multi = NULL;
char *port = NULL;
char *listen_port = NULL;
bool metadata = false;
unsigned timeout = 5;
unsigned client_timeout = 5;

volatile sig_atomic_t cont = 1;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static void sigint_handler(int signum __attribute__((unused))) {
  cont = 0;
}

static void print_usage(char *prog_name) {
  fprintf(stderr, "Usage: %s -h host -r resource -p port [-m yes/no] [-t timeout]", prog_name);
  fprintf(stderr, " [-P listen_port [-B multi] [-T listen_timeout]]\n");
}

static void parse_parameters(int argc, char *argv[]) {
  int opt;

  while ((opt = getopt(argc, argv, "h:r:p:m:t:P:B:T:")) != -1) {
    switch (opt) {
      case 'h':
        hostname = optarg;
        break;
      case 'r':
        resource = optarg;
        break;
      case 'p':
        port = optarg;
        break;
      case 'm':
        if (strcmp(optarg, "yes") == 0) {
          metadata = true;
        } else {
          if (strcmp(optarg, "no") == 0) {
            metadata = false;
          } else {
            print_usage(argv[0]);
            exit(1);
          }
        }
        break;
      case 't':
        timeout = atoi(optarg);
        break;
      case 'P':
        listen_port = optarg;
        break;
      case 'B':
        multi = optarg;
        break;
      case 'T':
        client_timeout = atoi(optarg);
        break;
      default: /* '?' */
        print_usage(argv[0]);
        exit(1);
    }
  }
}

void *client_communication_routine(void *arg) {
  struct client_routine_data *data = arg;

  int client_sock = data->sock;
  struct client_protocol_dgram *iam_packet = data->iam_packet;
  uint16_t iam_packet_len = data->iam_packet_len;

  struct sockaddr_in client_address;
  socklen_t client_address_len;

  struct client_protocol_dgram *packet = malloc(UDP_BUFFER_LEN);
  if (!packet) goto handle_errors;

  while (cont) {
    client_address_len = (socklen_t) sizeof(client_address);
    ssize_t len = recvfrom(client_sock, packet, UDP_BUFFER_LEN, MSG_DONTWAIT,
                           (struct sockaddr *)&client_address, &client_address_len);
    uint16_t type;
    if (len < 0) {
      if (errno != EAGAIN || errno != EWOULDBLOCK) goto handle_errors;
      type = NONE;
    } else {
      type = ntohs(packet->type);
    }

    switch (type) {
      case NONE:
      case DISCOVER:
      case KEEPALIVE:;
        time_t current_time = time(NULL);
        bool found = false;
        FOR_LIST(c, client_list) {
          if (type != NONE && is_same_address(&c->client_address, &client_address)) {
            c->valid = true;
            c->last_keepalive = current_time;
            found = true;
          } else {
            if (c->last_keepalive != -1 &&
                current_time - c->last_keepalive > client_timeout) {
              c->valid = false;
            }
          }
        }

        if (!found && type == DISCOVER) {
          ssize_t len = sendto(client_sock, iam_packet, iam_packet_len, 0,
                               (struct sockaddr *)&client_address, client_address_len);

          if (len != iam_packet_len) goto handle_errors;
        }

        int err = pthread_mutex_lock(&mutex);
        if (err != 0) goto handle_errors;

        erase_nonvalid_elements(&client_list);

        if (!found && type == DISCOVER) {
          struct client *new_client = malloc(sizeof(struct client));
          if (!new_client) {
            if (pthread_mutex_unlock(&mutex) != 0) exit(1);
            goto handle_errors;
          }
          new_client->last_keepalive = -1;
          new_client->client_address = client_address;
          new_client->valid = true;

          add_client(&client_list, new_client);
        }

        if (pthread_mutex_unlock(&mutex) != 0) exit(1);
      default:; // dziwna wiadomość - skip
    }
  }
  handle_errors:;
  return NULL;
}

int main(int argc, char *argv[]) {
  parse_parameters(argc, argv);

  if (!hostname || !resource || !port) {
    print_usage(argv[0]);
    return 1;
  }

  struct addrinfo addr_hints, *addr_result;
  memset(&addr_hints, 0, sizeof(struct addrinfo));
  addr_hints.ai_family = AF_INET;
  addr_hints.ai_socktype = SOCK_STREAM;
  addr_hints.ai_protocol = IPPROTO_TCP;

  if (signal(SIGINT, &sigint_handler) == SIG_ERR) exit(1);

  int err = getaddrinfo(hostname, port, &addr_hints, &addr_result);
  if (err != 0) exit(1);

  int sock = socket(addr_result->ai_family, addr_result->ai_socktype, addr_result->ai_protocol);
  if (sock < 0) exit(1);

  // setting timeout for TCP connection
  struct timeval tcp_timeout;
  tcp_timeout.tv_sec = timeout;
  tcp_timeout.tv_usec = 0;

  if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tcp_timeout, sizeof(tcp_timeout)) < 0) exit(1);
  if (connect(sock, addr_result->ai_addr, addr_result->ai_addrlen) < 0) exit(1);

  freeaddrinfo(addr_result);

  send_http_request(sock, resource, metadata);

  long icy_metaint = -1;
  char *icy_name = NULL;
  size_t icy_name_len = 0;
  char buffer[BUFFER_LEN];
  size_t received_data;

  int client_sock = -1;
  struct sockaddr_in server_address;
  uint16_t lport;
  pthread_t client_communication;

  if (receive_http_header(sock, &icy_metaint, &icy_name, &icy_name_len,
                          buffer, BUFFER_LEN, &received_data) < 0)
    goto handle_errors;

  if (icy_name) {
    size_t tmp = strlen("icy-name:");
    if (icy_name_len > tmp && strncasecmp(icy_name, "icy-name:", tmp) == 0) {
      for (size_t i = 0; i + tmp < icy_name_len; ++i)
        icy_name[i] = icy_name[i + tmp];
      icy_name_len -= tmp;
    }
  }

  struct ip_mreq ip_mreq; // for multicast

  if (listen_port != NULL) {
    if (icy_name_len > UINT16_MAX) goto handle_errors;

    lport = convert(listen_port);
    if (errno == EINVAL || errno == ERANGE) goto handle_errors;

    struct client_protocol_dgram *iam_packet = malloc(icy_name_len + CLIENT_PROTO_DGRAM_HEADER_LEN);
    if (iam_packet == NULL) goto handle_errors;

    iam_packet->type = htons(IAM);
    iam_packet->length = htons((uint16_t)(icy_name_len));
    if (icy_name_len > 0) memcpy(iam_packet->data, icy_name, icy_name_len);

    client_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (client_sock < 0) {
      free(iam_packet);
      goto handle_errors;
    }

    if (multi) {
      ip_mreq.imr_interface.s_addr = htonl(INADDR_ANY);
      if (inet_aton(multi, &ip_mreq.imr_multiaddr) == 0)
        goto handle_errors_client;

      if (setsockopt(client_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &ip_mreq, sizeof ip_mreq) < 0)
        goto handle_errors_client;
    }

    int optval = 1;
    if (setsockopt(client_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
      goto handle_errors_client;

    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(lport);

    if (bind(client_sock, (struct sockaddr *) &server_address,
            (socklen_t) sizeof(server_address)) < 0)
      goto handle_errors_client;

    struct client_routine_data cr_data;
    cr_data.iam_packet = iam_packet;
    cr_data.iam_packet_len = icy_name_len + CLIENT_PROTO_DGRAM_HEADER_LEN;
    cr_data.sock = client_sock;
    if (pthread_create(&client_communication, NULL, &client_communication_routine, &cr_data) != 0)
      goto handle_errors_client;

    if (0) { // we can only get here while handling errors
      handle_errors_client:
      free(iam_packet);
      if (close(client_sock) < 0) return 1;
      goto handle_errors;
    }
  }

  receive_http_data(sock, icy_metaint, buffer, BUFFER_LEN, received_data, client_sock);

  if (listen_port) {
    if (pthread_join(client_communication, NULL) != 0) exit(1);
    if (multi) {
      if (setsockopt(client_sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &ip_mreq, sizeof ip_mreq) < 0)
        exit(1);
    }
    if (close(client_sock) < 0) exit(1);
  }

  free(icy_name);
  if (close(sock) < 0) exit(1);
  clear_list(&client_list);
  pthread_mutex_destroy(&mutex);
  exit(0);

  handle_errors:

  free(icy_name);
  close(sock);
  exit(1);
}
