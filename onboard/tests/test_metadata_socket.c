/* Offline loopback-only integration test. Uses PSDK public types, but neither
 * links libpayloadsdk nor initializes PSDK. All peers are 127.0.0.1. */
#define _POSIX_C_SOURCE 200809L
#include "metadata_socket.h"
#include "dji_error.h"

#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)
#define OK DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS

static const T_DjiSocketHandler *api;

static struct sockaddr_in loopback(uint16_t port)
{
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    CHECK(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    return address;
}

static int local_peer(int type, uint16_t *port)
{
    int fd = socket(AF_INET, type, 0);
    CHECK(fd >= 0);
    struct sockaddr_in address = loopback(0);
    CHECK(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    CHECK(getsockname(fd, (struct sockaddr *)&address, &size) == 0);
    *port = ntohs(address.sin_port);
    return fd;
}

static void ready(int fd, short event)
{
    struct pollfd item = { .fd = fd, .events = event, .revents = 0 };
    CHECK(poll(&item, 1, 2000) == 1);
    CHECK(item.revents & event);
}

static double monotonic_seconds(void)
{
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void test_udp(void)
{
    uint16_t port;
    int peer = local_peer(SOCK_DGRAM, &port);
    T_DjiSocketHandle handle = NULL;
    CHECK(api->Socket(DJI_SOCKET_MODE_UDP, &handle) == OK);
    CHECK(api->Bind(handle, "127.0.0.1", 0) == OK);
    CHECK(api->Bind(handle, "not-an-ip", 1000) == DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER);

    const uint8_t outbound[] = {0, 17, 128, 255};
    uint32_t transferred = 999;
    CHECK(api->UdpSendData(handle, "127.0.0.1", port, outbound, sizeof(outbound), &transferred) == OK);
    CHECK(transferred == sizeof(outbound));
    ready(peer, POLLIN);
    struct sockaddr_in client;
    socklen_t client_size = sizeof(client);
    uint8_t bytes[128];
    CHECK(recvfrom(peer, bytes, sizeof(bytes), 0, (struct sockaddr *)&client, &client_size) == sizeof(outbound));
    CHECK(memcmp(bytes, outbound, sizeof(outbound)) == 0);
    CHECK(client.sin_addr.s_addr == htonl(INADDR_LOOPBACK));

    const uint8_t reply[] = {2, 4, 6, 8, 10};
    CHECK(sendto(peer, reply, sizeof(reply), MSG_NOSIGNAL,
                 (struct sockaddr *)&client, client_size) == sizeof(reply));
    char address[INET_ADDRSTRLEN];
    uint32_t source_port = 0;
    CHECK(api->UdpRecvData(handle, address, &source_port, bytes, sizeof(bytes), &transferred) == OK);
    CHECK(transferred == sizeof(reply));
    CHECK(memcmp(bytes, reply, sizeof(reply)) == 0);
    CHECK(strcmp(address, "127.0.0.1") == 0 && source_port == port);

    uint8_t oversized[40];
    memset(oversized, 0x6a, sizeof(oversized));
    CHECK(sendto(peer, oversized, sizeof(oversized), MSG_NOSIGNAL,
                 (struct sockaddr *)&client, client_size) == sizeof(oversized));
    transferred = 999;
    CHECK(api->UdpRecvData(handle, address, &source_port, bytes, 4, &transferred) ==
          DJI_ERROR_SYSTEM_MODULE_CODE_OUT_OF_RANGE);
    CHECK(transferred == 0);

    double before = monotonic_seconds();
    CHECK(api->UdpRecvData(handle, address, &source_port, bytes, sizeof(bytes), &transferred) ==
          DJI_ERROR_SYSTEM_MODULE_CODE_TIMEOUT);
    double elapsed = monotonic_seconds() - before;
    CHECK(elapsed >= 0.20 && elapsed < 2.0);
    CHECK(transferred == 0);
    CHECK(api->Close(handle) == OK);
    CHECK(close(peer) == 0);
    puts("PASS UDP binary data, peer address, truncation rejection, bounded empty receive");
}

static void test_tcp_client(void)
{
    uint16_t port;
    int listener = local_peer(SOCK_STREAM, &port);
    CHECK(listen(listener, 1) == 0);
    T_DjiSocketHandle client = NULL;
    CHECK(api->Socket(DJI_SOCKET_MODE_TCP, &client) == OK);
    CHECK(api->TcpConnect(client, "127.0.0.1", port) == OK);
    ready(listener, POLLIN);
    int server = accept(listener, NULL, NULL);
    CHECK(server >= 0);

    const uint8_t payload[] = "metadata socket client";
    uint32_t transferred = 0;
    CHECK(api->TcpSendData(client, payload, sizeof(payload), &transferred) == OK);
    CHECK(transferred == sizeof(payload));
    ready(server, POLLIN);
    uint8_t bytes[128];
    CHECK(recv(server, bytes, sizeof(bytes), 0) == sizeof(payload));
    CHECK(memcmp(bytes, payload, sizeof(payload)) == 0);

    const uint8_t reply[] = "not SDK data";
    CHECK(send(server, reply, sizeof(reply), MSG_NOSIGNAL) == sizeof(reply));
    CHECK(api->TcpRecvData(client, bytes, sizeof(bytes), &transferred) == OK);
    CHECK(transferred == sizeof(reply));
    CHECK(memcmp(bytes, reply, sizeof(reply)) == 0);
    CHECK(shutdown(server, SHUT_WR) == 0);
    transferred = 999;
    CHECK(api->TcpRecvData(client, bytes, sizeof(bytes), &transferred) == OK);
    CHECK(transferred == 0);
    CHECK(api->Close(client) == OK);
    CHECK(close(server) == 0 && close(listener) == 0);
    puts("PASS TCP connect, bidirectional bytes, orderly EOF");
}

static void test_tcp_server(void)
{
    /* Reserve an OS-chosen free loopback port, then bind the opaque API handle.
     * A rare port collision is reported as a test failure, never worked around
     * by contacting a different host. */
    uint16_t port;
    int reservation = local_peer(SOCK_STREAM, &port);
    CHECK(close(reservation) == 0);
    T_DjiSocketHandle listener = NULL, accepted = NULL;
    CHECK(api->Socket(DJI_SOCKET_MODE_TCP, &listener) == OK);
    CHECK(api->Bind(listener, "127.0.0.1", port) == OK);
    CHECK(api->TcpListen(listener) == OK);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(client >= 0);
    struct sockaddr_in address = loopback(port);
    CHECK(connect(client, (struct sockaddr *)&address, sizeof(address)) == 0);
    struct sockaddr_in local;
    socklen_t local_size = sizeof(local);
    CHECK(getsockname(client, (struct sockaddr *)&local, &local_size) == 0);
    char peer_ip[INET_ADDRSTRLEN];
    uint32_t peer_port = 0;
    CHECK(api->TcpAccept(listener, peer_ip, &peer_port, &accepted) == OK);
    CHECK(accepted != NULL);
    CHECK(strcmp(peer_ip, "127.0.0.1") == 0 && peer_port == ntohs(local.sin_port));
    const uint8_t message[] = "server side";
    CHECK(send(client, message, sizeof(message), MSG_NOSIGNAL) == sizeof(message));
    uint8_t bytes[32];
    uint32_t received = 0;
    CHECK(api->TcpRecvData(accepted, bytes, sizeof(bytes), &received) == OK);
    CHECK(received == sizeof(message) && memcmp(bytes, message, sizeof(message)) == 0);
    CHECK(api->Close(accepted) == OK && api->Close(listener) == OK);
    CHECK(close(client) == 0);
    puts("PASS TCP listen/accept and accepted-peer fields");
}

int main(void)
{
    api = L3Metadata_GetSocketHandler();
    CHECK(api != NULL);
    T_DjiSocketHandle invalid = (T_DjiSocketHandle)api;
    CHECK(api->Socket((E_DjiSocketMode)99, &invalid) == DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER);
    CHECK(invalid == NULL);
    CHECK(api->Socket(DJI_SOCKET_MODE_UDP, NULL) == DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER);
    test_udp();
    test_tcp_client();
    test_tcp_server();
    puts("PASS all tests used loopback only; no PSDK runtime or aircraft accessed");
    return 0;
}
