/**
 * @file stream_sender.h
 * @brief UDP stream sender for CSI frames.
 */

#ifndef STREAM_SENDER_H
#define STREAM_SENDER_H

#include <stdint.h>
#include <stddef.h>

/**
 * Initialize the UDP sender.
 * Creates a UDP socket targeting the configured aggregator.
 *
 * @return 0 on success, -1 on error.
 */
int stream_sender_init(void);

/**
 * Initialize the UDP sender with explicit IP and port.
 * Used when configuration is loaded from NVS at runtime.
 *
 * @param ip   Aggregator IP address string (e.g. "192.168.1.20").
 * @param port Aggregator UDP port.
 * @return 0 on success, -1 on error.
 */
int stream_sender_init_with(const char *ip, uint16_t port);

/**
 * Send a serialized CSI frame over UDP.
 *
 * @param data Frame data buffer.
 * @param len  Length of data to send.
 * @return Number of bytes sent, or -1 on error.
 */
int stream_sender_send(const uint8_t *data, size_t len);

/**
 * Send a UDP packet to an arbitrary IP/port using the existing socket.
 * Used by the audio raw-stream debug endpoint to ship samples to a
 * caller-specified listener without disturbing the CSI/feature destination.
 *
 * Best-effort: drops silently on ENOMEM (does not engage the global backoff
 * used by the CSI path).
 *
 * @param data Frame data buffer.
 * @param len  Length of data to send.
 * @param ip   Destination IPv4 dotted-quad (e.g. "192.168.1.20"), non-NULL.
 * @param port Destination UDP port, must be >0.
 * @return Number of bytes sent, or -1 on error.
 */
int stream_sender_send_to(const uint8_t *data, size_t len,
                          const char *ip, uint16_t port);

/**
 * Close the UDP sender socket.
 */
void stream_sender_deinit(void);

#endif /* STREAM_SENDER_H */
