#include "hal/nwy_hal_socket.h"
#include "hal/nwy_hal_os.h"
#include "hal/nwy_hal_uart.h" // Utilizing professional standard logging headers
#include "nwy_osi_api.h"
#include "nwy_socket_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HAL_SOCKET_RX_BUFFER_LIMIT 1536
#define HAL_SOCKET_MAX_RETRIES 5 // Added missing definition
#define TAG "HAL_SOCKET"

typedef struct {
  int socket_fd;
  bool is_connected;
  nwy_hal_socket_recv_cb_t recv_cb;
  nwy_osi_thread_t rx_task_handle;
  volatile bool rx_loop_active;
} nwy_hal_socket_manager_t;

static nwy_hal_socket_manager_t g_hal_sock_ctx = {-1, false, NULL, NULL, false};

static const char *get_socket_state_str(nwy_tcp_state_e state) { //
  switch (state) {
  case NWY_CLOSED:
    return "CLOSED"; //
  case NWY_LISTEN:
    return "LISTEN"; //
  case NWY_SYN_SENT:
    return "SYN_SENT"; //
  case NWY_SYN_RCVD:
    return "SYN_RCVD"; //
  case NWY_ESTABLISHED:
    return "ESTABLISHED"; //
  default:
    return "UNKNOWN"; //
  }
}

static void nwy_hal_socket_rx_task_worker(void *param) {
  int target_fd = (int)(intptr_t)param;
  uint8_t *heap_rx_buffer = (uint8_t *)malloc(HAL_SOCKET_RX_BUFFER_LIMIT);

  if (!heap_rx_buffer) {
    g_hal_sock_ctx.rx_loop_active = false;
    return;
  }

  while (g_hal_sock_ctx.rx_loop_active &&
         g_hal_sock_ctx.socket_fd == target_fd) {
    int read_bytes = nwy_socket_recv(target_fd, heap_rx_buffer,
                                     HAL_SOCKET_RX_BUFFER_LIMIT - 1, 0); //

    if (read_bytes > 0) {
      heap_rx_buffer[read_bytes] = '\0';
      if (g_hal_sock_ctx.recv_cb) {
        g_hal_sock_ctx.recv_cb(target_fd, heap_rx_buffer, (uint32_t)read_bytes);
      }
    } else if (read_bytes == 0) {
      nwy_hal_uart_log_i(TAG, "Connection closed cleanly by remote endpoint.");
      break;
    } else {
      int current_err = nwy_socket_errno();        //
      if (current_err == 11 || current_err == 4) { // EAGAIN / EINTR
        nwy_hal_os_thread_sleep(50);
        continue;
      }
      nwy_hal_uart_log_e(TAG, "Socket read exception. Error sequence code: %d",
                         current_err);
      break;
    }
  }

  free(heap_rx_buffer);
  g_hal_sock_ctx.rx_loop_active = false;
  g_hal_sock_ctx.is_connected = false;
}

/**
 * @brief Establishes a highly reliable TCP client connection.
 * @return Active socket file descriptor on success, -1 on failure.
 */
int nwy_hal_socket_tcp_connect(int cid, const char *host_ip, uint16_t port,
                               nwy_hal_socket_recv_cb_t recv_cb) {
  if (!host_ip || port == 0 || cid < 1 || cid > 7) {
    nwy_hal_uart_log_e(TAG, "Invalid parameters: cid=%d, host=%s, port=%d", cid,
                       host_ip, port);
    return -1;
  }

  // 1. Cleanup: Ensure no dangling socket references exist
  if (g_hal_sock_ctx.socket_fd >= 0) {
    nwy_hal_socket_close(g_hal_sock_ctx.socket_fd);
  }

  // 2. DNS Resolution: Dynamic lookup for maximum resilience against IP changes
  int isipv6 = 0;
  nwy_hal_uart_log_i(TAG, "Querying DNS tables for host identifier: %s",
                     host_ip);
  char *resolved_ip_str = nwy_socket_gethost_by_name(host_ip, &isipv6, cid);

  if (!resolved_ip_str || strlen(resolved_ip_str) == 0) {
    nwy_hal_uart_log_e(TAG, "DNS resolution failed for host: %s", host_ip);
    return -1;
  }
  nwy_hal_uart_log_i(TAG, "DNS lookup success. Destination: %s",
                     resolved_ip_str);

  // 3. Socket Open: Create raw transport ring
  int created_fd =
      nwy_socket_open(2, 1, 6, cid); // AF_INET=2, SOCK_STREAM=1, TCP=6
  if (created_fd < 0) {
    nwy_hal_uart_log_e(TAG, "Socket descriptor allocation failed.");
    return -1;
  }

  // 4. Configuration: Prepare network address structures
  struct sockaddr_in server_target_addr;
  memset(&server_target_addr, 0, sizeof(server_target_addr));
  server_target_addr.sin_len =
      sizeof(struct sockaddr_in);    // Essential for Neoway LwIP
  server_target_addr.sin_family = 2; // AF_INET
  server_target_addr.sin_port = nwy_socket_htons((int16_t)port);

  if (nwy_socket_inet_pton(2, resolved_ip_str, &server_target_addr.sin_addr) !=
      NWY_SUCCESS) {
    nwy_hal_uart_log_e(TAG, "Address binary conversion failed.");
    nwy_socket_close(created_fd);
    return -1;
  }

  // 5. Connection Loop: Fail-proof retry logic for cellular baseband
  // propagation
  int retry_count = 0;
  bool connected = false;

  while (retry_count < HAL_SOCKET_MAX_RETRIES) {
    int dial_result = nwy_socket_connect(
        created_fd, (const struct sockaddr *)&server_target_addr,
        sizeof(struct sockaddr_in));
    int err = nwy_socket_errno();
    nwy_tcp_state_e state = nwy_socket_get_state(created_fd);

    if (dial_result == 0 || state == NWY_ESTABLISHED || state == NWY_SYN_SENT) {
      connected = true;
      break;
    }

    nwy_hal_uart_log_w(TAG,
                       "Retry [%d/%d] Connect failed. Errno: %d, State: %s",
                       ++retry_count, HAL_SOCKET_MAX_RETRIES, err,
                       get_socket_state_str(state));

    if (err != 115 && err != 114)
      break; // Hard fail if not "In Progress"
    nwy_hal_os_thread_sleep(1000);
  }

  if (!connected) {
    nwy_hal_uart_log_e(TAG, "Connection establishment aborted.");
    nwy_socket_close(created_fd);
    return -1;
  }

  // 6. Finalization: Spawn listening thread and store context
  g_hal_sock_ctx.socket_fd = created_fd;
  g_hal_sock_ctx.is_connected = true;
  g_hal_sock_ctx.recv_cb = recv_cb;
  g_hal_sock_ctx.rx_loop_active = true;

  if (!nwy_hal_os_thread_create(
          &g_hal_sock_ctx.rx_task_handle, "nwy_sock_rx_worker",
          nwy_hal_socket_rx_task_worker, (void *)(intptr_t)created_fd,
          NWY_OSI_PRIORITY_NORMAL, 2048)) {
    nwy_hal_uart_log_e(TAG, "Failed to initialize background RX worker.");
    nwy_socket_close(created_fd);
    return -1;
  }

  nwy_hal_uart_log_i(TAG, "TCP socket fully established and active.");
  return created_fd;
}

bool nwy_hal_socket_send(int socket_fd, const uint8_t *data,
                         uint32_t data_len) {
  if (socket_fd < 0 || !data || data_len == 0 || !g_hal_sock_ctx.is_connected) {
    return false;
  }
  int feedback_bytes = nwy_socket_send(socket_fd, data, data_len, 0); //
  return (feedback_bytes == (int)data_len);
}

void nwy_hal_socket_close(int socket_fd) {
  if (socket_fd < 0)
    return;

  if (g_hal_sock_ctx.socket_fd == socket_fd) {
    g_hal_sock_ctx.rx_loop_active = false;
    g_hal_sock_ctx.is_connected = false;
    nwy_socket_close(socket_fd); //
    g_hal_sock_ctx.socket_fd = -1;
  } else {
    nwy_socket_close(socket_fd); //
  }
}