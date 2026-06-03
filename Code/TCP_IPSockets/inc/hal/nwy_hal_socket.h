#ifndef __NWY_HAL_SOCKET_H__
#define __NWY_HAL_SOCKET_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback configuration invoked during network data read windows.
 * @param socket_fd Associated active socket file descriptor.
 * @param data Pointer to the buffer containing received raw data.
 * @param data_len Length of the data chunk in bytes.
 */
typedef void (*nwy_hal_socket_recv_cb_t)(int socket_fd, const uint8_t *data,
                                         uint32_t data_len);

/**
 * @brief Instantiates an asynchronous, non-blocking persistent TCP client link.
 * @param cid Connection Profile Identifier / Data Channel Index (1 to 6).
 * @param host_ip Target server IPv4 dotted address string (e.g.,
 * "93.184.216.34").
 * @param port Target server communication port (e.g., 80).
 * @param recv_cb Pointer to the callback function triggered on data reception
 * windows.
 * @return Active socket file descriptor integer value on success, or -1 on
 * failure.
 */
int nwy_hal_socket_tcp_connect(int cid, const char *host_ip, uint16_t port,
                               nwy_hal_socket_recv_cb_t recv_cb);

/**
 * @brief Transmits a binary payload safely over an open socket channel.
 * @param socket_fd Active valid socket file descriptor.
 * @param data Pointer to the binary data array to transmit.
 * @param data_len Exact sequence length of the message payload in bytes.
 * @return true if all bytes successfully passed down to the base network ring,
 * false otherwise.
 */
bool nwy_hal_socket_send(int socket_fd, const uint8_t *data, uint32_t data_len);

/**
 * @brief Gracefully deallocates tracking references, shuts down tasks, and
 * closes file descriptors.
 * @param socket_fd Active valid socket file descriptor.
 */
void nwy_hal_socket_close(int socket_fd);

#ifdef __cplusplus
}
#endif

#endif // __NWY_HAL_SOCKET_H__