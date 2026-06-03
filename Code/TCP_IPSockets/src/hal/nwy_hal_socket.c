#include "hal/nwy_hal_socket.h"
#include "hal/nwy_hal_os.h"
#include "hal/nwy_hal_uart.h" // Utilizing professional standard logging headers
#include "nwy_osi_api.h"
#include "nwy_socket_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define HAL_SOCKET_RX_BUFFER_LIMIT 1536
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

int nwy_hal_socket_tcp_connect(int cid, const char *host_ip, uint16_t port,
                               nwy_hal_socket_recv_cb_t recv_cb) {
  if (!host_ip || port == 0 || cid < 1 || cid > 6) { //
    return -1;
  }

  if (g_hal_sock_ctx.socket_fd >= 0) {
    nwy_hal_socket_close(g_hal_sock_ctx.socket_fd);
  }

  // DYNAMIC DNS RESOLUTION: Automatically resolves endpoints dynamically
  nwy_hal_uart_log_i(TAG, "Querying DNS tables for host identifier: %s",
                     host_ip);
  int isipv6 = 0;
  char *resolved_ip_str = nwy_socket_gethost_by_name(host_ip, &isipv6, cid); //

  if (resolved_ip_str == NULL || strlen(resolved_ip_str) == 0) {
    nwy_hal_uart_log_e(TAG, "DNS resolution failed to match network target: %s",
                       host_ip); //
    return -1;
  }
  nwy_hal_uart_log_i(TAG,
                     "DNS lookup resolution success. Assigned destination: %s",
                     resolved_ip_str);

  // Allocation constraints matching standard protocol family properties
  int created_fd =
      nwy_socket_open(2, 1, 6, cid); // AF_INET=2, SOCK_STREAM=1, IPPROTO_TCP=6
  if (created_fd < 0) {
    nwy_hal_uart_log_e(TAG, "Socket descriptor allocation error.");
    return -1;
  }

  struct sockaddr_in server_target_addr;
  memset(&server_target_addr, 0, sizeof(server_target_addr));

  // MANDATORY INDUSTRIAL FIX: Explicit assignment of structure lengths prevents
  // instant core engine drop
  server_target_addr.sin_len = sizeof(struct sockaddr_in);       //
  server_target_addr.sin_family = 2;                             // AF_INET
  server_target_addr.sin_port = nwy_socket_htons((int16_t)port); //

  if (nwy_socket_inet_pton(2, resolved_ip_str, &server_target_addr.sin_addr) !=
      NWY_SUCCESS) { //
    nwy_hal_uart_log_e(TAG, "Binary address field transformation failed.");
    nwy_socket_close(created_fd); //
    return -1;
  }

  nwy_hal_uart_log_i(TAG,
                     "Dispatching network connect request over descriptor: %d",
                     created_fd);
  int dial_result = nwy_socket_connect(
      created_fd, (const struct sockaddr *)&server_target_addr,
      sizeof(struct sockaddr_in)); //

  // PROGRESSIVE HOOK LOOP: Handle cellular routing latency synchronously safely
  int polling_cycles = 0;
  bool connection_clear = false;

  while (polling_cycles <
         15) { // Maximum 7.5 second connection gate window limits
    nwy_tcp_state_e current_state = nwy_socket_get_state(created_fd); //
    int current_errno = nwy_socket_errno();                           //

    if (current_state == NWY_ESTABLISHED) { //
      nwy_hal_uart_log_i(TAG, "TCP socket fully established and active.");
      connection_clear = true;
      break;
    } else if (current_state == NWY_CLOSED ||
               (dial_result != 0 && current_errno != 115 &&
                current_errno != 114)) {
      // 115 = EINPROGRESS, 114 = EALREADY. Exit if hard fault returned
      nwy_hal_uart_log_e(TAG,
                         "Base stack handshake rejected. State: %s, Errno: %d",
                         get_socket_state_str(current_state), current_errno);
      break;
    }

    nwy_hal_uart_log_w(
        TAG,
        "Handshake link in progress... State: %s, Errno: %d. Waiting 500ms.",
        get_socket_state_str(current_state), current_errno);
    nwy_hal_os_thread_sleep(500);
    polling_cycles++;

    // Re-verify network layer state map
    dial_result = nwy_socket_connect(
        created_fd, (const struct sockaddr *)&server_target_addr,
        sizeof(struct sockaddr_in)); //
  }

  if (!connection_clear) {
    nwy_socket_close(created_fd); //
    return -1;
  }

  // Bind references to global tracking records
  g_hal_sock_ctx.socket_fd = created_fd;
  g_hal_sock_ctx.is_connected = true;
  g_hal_sock_ctx.recv_cb = recv_cb;
  g_hal_sock_ctx.rx_loop_active = true;

  bool thread_spawning_clear = nwy_hal_os_thread_create(
      &g_hal_sock_ctx.rx_task_handle, "nwy_sock_rx_worker",
      nwy_hal_socket_rx_task_worker, (void *)(intptr_t)created_fd,
      NWY_OSI_PRIORITY_NORMAL, 1024 * 4);

  if (!thread_spawning_clear) {
    nwy_hal_uart_log_e(TAG, "Failed to instantiate context receiver listener.");
    nwy_socket_close(created_fd); //
    g_hal_sock_ctx.socket_fd = -1;
    g_hal_sock_ctx.is_connected = false;
    g_hal_sock_ctx.rx_loop_active = false;
    return -1;
  }

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