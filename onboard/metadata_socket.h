#ifndef L3_METADATA_SOCKET_H
#define L3_METADATA_SOCKET_H

#include "dji_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Linux/POSIX IPv4 callbacks for the bounded metadata probe. No shell commands,
 * sysctl writes, interface configuration, DNS, or application logging. Buffers
 * requested on each socket are 256 KiB, subject to normal kernel limits.
 *
 * Each I/O/accept call waits at most 250 ms; connect waits at most 1500 ms.
 * TIMEOUT and partial transfers must be handled by the SDK. These limits bound
 * the callbacks, not DjiCore_Init or the entire SDK; use a process watchdog too.
 *
 * SDK callers of UdpRecvData/TcpAccept must supply an IPv4 address output of at
 * least INET_ADDRSTRLEN (16) bytes. Close requires all other users of that handle
 * to have stopped. This is not a measured high-throughput video transport. */
const T_DjiSocketHandler *L3Metadata_GetSocketHandler(void);

#ifdef __cplusplus
}
#endif

#endif
