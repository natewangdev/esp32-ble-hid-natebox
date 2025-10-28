/**
 * Wi-Fi + HTTP server helper for handling HID actions over REST.
 */

#ifndef NETWORK_SERVER_H
#define NETWORK_SERVER_H

#include <stdint.h>
#include "esp_err.h"

esp_err_t network_server_start(void);
void network_server_set_hid_conn_id(uint16_t conn_id);

#endif /* NETWORK_SERVER_H */

