#include "client_protocol.h"

#include <errno.h>
#include <stdlib.h>

client_list_t client_list = NULL;

void add_client(client_list_t *client_list, struct client *client) {
  client->next = *client_list;
  *client_list = client;
}

void erase_nonvalid_elements(client_list_t *client_list) {
  struct client *previous = NULL;
  struct client *current = *client_list;
  while (current) {
    if (!current->valid) {
      struct client *tmp = current;
      if (previous) previous->next = current->next;
      current = current->next;
      if (*client_list == tmp) *client_list = tmp->next;
      free(tmp);
    } else {
      previous = current;
      current = current->next;
    }
  }
}

bool is_same_address(const struct sockaddr_in *first,
                     const struct sockaddr_in *second) {
  return first->sin_addr.s_addr == second->sin_addr.s_addr
      && first->sin_port == second->sin_port;
}

void clear_list(client_list_t *client_list) {
  while (*client_list) {
    struct client *client = *client_list;
    *client_list = client->next;
    free(client);
  }
}
