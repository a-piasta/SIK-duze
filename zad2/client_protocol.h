#ifndef _RADIO_CLIENT_PROTOCOL_H_
#define _RADIO_CLIENT_PROTOCOL_H_

#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define STDOUT_STDERR  0
#define RADIO_CLIENT   1

#define NONE        0
#define DISCOVER    1
#define IAM         2
#define KEEPALIVE   3
#define AUDIO       4
#define METADATA    6

#define UDP_BUFFER_LEN  0x10000

#define CLIENT_PROTO_DGRAM_HEADER_LEN (2 * sizeof(uint16_t))

struct client_protocol_dgram {
  uint16_t type;
  uint16_t length;
  char data[];
};

struct client_routine_data {
  struct client_protocol_dgram *iam_packet;
  uint16_t iam_packet_len;
  int sock;
};

struct client {
  time_t last_keepalive;
  struct sockaddr_in client_address;
  struct client *next;
  bool valid;
};

typedef struct client * client_list_t;

// for manipulating clients list
extern pthread_mutex_t mutex;

extern client_list_t client_list;

#define FOR_LIST(c, list) \
  for (struct client *c = list; c != NULL; c = c->next)

void add_client(client_list_t *client_list, struct client *client);

void erase_nonvalid_elements(client_list_t *client_list);

bool is_same_address(const struct sockaddr_in *first,
                     const struct sockaddr_in *second);

void clear_list(client_list_t *client_list);

#endif  // _RADIO_CLIENT_PROTOCOL_H_
