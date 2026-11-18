/*
 * M400/L3 explicit metadata, H.264, or exclusive short point-recording probe.
 * Point recording/Stop require their own explicit command and operator gate.
 * Optional video-control-stdin permits explicit preview-source changes only.
 * No gimbal or flight-control API.
 * SDK initialization requires an explicit mode, never the default.
 * Build only with the standalone CMakeLists.txt in this directory.
 *
 * SDK calls, including Init/DeInit, can block. An external GNU timeout with
 * --kill-after is required; our SIGTERM handler cannot cancel an SDK call.
 */
#include <errno.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "dji_aircraft_info.h"
#include "dji_camera_manager.h"
#include "dji_core.h"
#include "dji_error.h"
#include "dji_liveview.h"
#include "dji_logger.h"
#include "hal_usb_bulk.h"
#include "metadata_socket.h"
#include "l3_rgb_probe.h"
#include "l3_point_probe_controller.h"
#include "l3p1_tcp_sink.h"
#include "l3_stream.h"
#include "l3_video_control.h"
#include "l3_usb_padding.h"
#include "l3_osal.h"
#include "osal/osal.h"
#include "osal/osal_fs.h"
#if L3_USE_OFFICIAL_SOCKET
#include "osal/osal_socket.h"
#endif

#define CONFIG_LIMIT 4096U
#define SDK_LOG_LIMIT (4U * 1024U * 1024U)
#define RGB_BUFFER_CAPACITY (32U * 1024U * 1024U)

/* Default closed even when compiled outside this CMake project. Enable only
 * after a replacement SDK has a verified fix for the runtime MOP memory error.
 */
#ifndef L3_ENABLE_POINT_RECORDING_PROBE
#define L3_ENABLE_POINT_RECORDING_PROBE 0
#endif

static volatile sig_atomic_t stop_requested;
static int sdk_log_fd = -1;
static size_t sdk_log_bytes;
static pthread_mutex_t sdk_log_mutex = PTHREAD_MUTEX_INITIALIZER;

#if L3_USE_OFFICIAL_SOCKET
/* Only the official socket translation unit redirects system() here. Its two
 * global sysctl writes are irrelevant to the blocking-I/O comparison. */
int L3Diagnostic_BlockedSystem(const char *command)
{
    (void)command;
    return -1;
}
#endif

static void erase(void *data, size_t size)
{
    volatile unsigned char *p = data;
    while (size-- != 0U) {
        *p++ = 0;
    }
}

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void usage(void)
{
    puts("Usage: l3_metadata_inspect --help\n"
         "       l3_metadata_inspect --config PATH --check-config\n"
         "       l3_metadata_inspect --config PATH --inspect\n"
         "       l3_metadata_inspect --config PATH --camera-firmware --mount 1\n"
         "       l3_metadata_inspect --config PATH --video-info --mount 1\n"
         "       l3_metadata_inspect --config PATH --sdk-diagnostic --seconds 60\n"
         "         [--diagnostic-video --mount 1]\n"
         "       l3_metadata_inspect --config PATH --rgb-probe --mount 1 --seconds 8\n"
         "       l3_metadata_inspect --config PATH --point-probe --mount 1 --seconds 20 \\\n"
         "         --tcp-host 127.0.0.1 --tcp-port 9010 --exclusive-test\n"
         "       l3_metadata_inspect --config PATH --stop-pointcloud-only --mount 1 --exclusive-test\n"
         "       l3_metadata_inspect --config PATH --stream --mount 1 --seconds 60\n"
         "         --streams points|both|video --bind 127.0.0.1\n"
         "         --point-port 9101 --video-port 9102 --exclusive-test\n"
         "         [--expected-source ID (video-only; no source writes)]\n"
         "         [--video-control-stdin (video-only; explicit source writes)]\n"
         "         [--fast-switch (video control; keep the SDK session open)]\n"
         "\n"
         "No arguments/--help: no configuration read or SDK initialization.\n"
         "--check-config: validate owner-only regular 0600 file; no SDK calls.\n"
         "--inspect: initialize PSDK, require M400, query camera TYPE at the\n"
         "three documented PSDK payload mounts, then deinitialize and exit.\n"
         "--camera-firmware: require exact L3 at --mount 1..3 and read its firmware.\n"
         "--video-info: require exact L3, read supported/current video sources.\n"
         "Raw source IDs are metadata, not proof of point-cloud video support.\n"
         "--sdk-diagnostic: Core/ApplicationStart/aircraft metadata and a bounded\n"
         "1..300 second hold; no CameraManager, Liveview, MOP or recording calls.\n"
         "--diagnostic-video additionally verifies L3 at --mount, then counts\n"
         "and discards DEFAULT H.264 callbacks; it does not save or record.\n"
         "--rgb-probe: require M400 and exact L3 at --mount 1..3; probe DEFAULT\n"
         "H.264 for --seconds 1..10. 32 MiB RAM sink; save only after stop/drain.\n"
         "The saved bytes are UNVERIFIED H.264, not confirmed RGB or frames.\n"
         "Metadata/RGB modes do not record, scan, change source or use MOP.\n"
         "No mode operates motors, flight control or the gimbal. --point-probe\n"
         "EXPLICITLY starts L3 point recording once and requests independent\n"
         "Stop by --seconds 1..20, including with zero received packets.\n"
         "It reads at most 30 packets/60 attempts; TCP sink budget is 200 ms.\n"
         "--exclusive-test asserts no preexisting recording and no concurrent\n"
         "SDK/recording controller. Close Pilot 2 for point subscription.\n"
         "--stop-pointcloud-only performs exact-L3 Stop, with NO Start/MOP;\n"
         "use only after the previous SDK process exited, for your own test.\n"
         "Recording duration is not guaranteed: SDK Start/Stop can block.\n"
         "A missing Stop ACK requires manual L3 stop; process exit is not stop.\n"
         "--stream: 1..300 seconds. --streams defaults to both. points skips all\n"
         "Liveview calls and publishes no video. video subscribes and forwards\n"
         "Liveview H.264 only, never starts point recording and is not gated by\n"
         "the SDK-fix build gate. points/both explicitly start point recording\n"
         "and use the same SDK-fix build gate as --point-probe.\n"
         "--expected-source: optional CameraManager source ID, video-only. Reads\n"
         "the exact L3 range/current source and refuses a mismatch; no writes.\n"
         "--video-control-stdin accepts status/visible/point/stop lines. EOF stops\n"
         "the session. visible/point explicitly change preview source only,\n"
         "with exact-L3/range/readback checks; no scan or recording commands.\n"
         "\n"
         "Config: UTF-8, unquoted KEY=VALUE; split at first '='; all required:\n"
         "APP_NAME APP_ID APP_KEY APP_LICENSE DEVELOPER_ACCOUNT BAUD_RATE\n"
         "Blank lines and lines beginning '#' are accepted. No trimming of\n"
         "keys/values. UTF-8 BOM and CRLF accepted. BAUD_RATE must be 460800.\n"
         "\n"
         "All SDK modes require a fresh, owned 0700 writable working directory.\n"
         "The probe creates l3-rgb.private.h264 (0600, no overwrite).\n"
         "Raw SDK diagnostics go only to new 0600 sdk.private.log (4 MiB cap);\n"
         "they can contain private data and must be redacted before sharing.\n"
         "SDK calls are blocking and have no guaranteed cancellation. Use:\n"
         "timeout --signal=TERM --kill-after=3s 45s /path/l3_metadata_inspect \\\n"
         "  --config /private/psdk.conf --inspect\n"
         "A forced kill may prevent SDK cleanup; do not start another SDK app.");
#if !L3_ENABLE_POINT_RECORDING_PROBE
    puts("BUILD GATE: --point-probe and --stream points|both are DISABLED because the\n"
         "PSDK MOP memory error has no verified fix. --stream video is available\n"
         "(no recording). Refusal occurs before config/TCP/SDK access.\n"
         "--stop-pointcloud-only remains available; it never starts recording.");
#else
    puts("BUILD GATE: --point-probe was explicitly enabled at build time.\n"
         "Use only with a verified SDK MOP memory-error fix and exclusive test.");
#endif
}

/* Reject overlong UTF-8, surrogates, invalid continuation bytes and controls. */
static bool valid_utf8_name(const unsigned char *p)
{
    while (*p != 0U) {
        unsigned char a = *p++;
        size_t following;
        if (a < 0x80U) {
            if (a < 0x20U || a == 0x7fU) return false;
            continue;
        }
        if (a >= 0xc2U && a <= 0xdfU) following = 1U;
        else if (a >= 0xe0U && a <= 0xefU) following = 2U;
        else if (a >= 0xf0U && a <= 0xf4U) following = 3U;
        else return false;
        if ((a == 0xe0U && *p < 0xa0U) ||
            (a == 0xedU && *p >= 0xa0U) ||
            (a == 0xf0U && *p < 0x90U) ||
            (a == 0xf4U && *p >= 0x90U)) return false;
        while (following-- != 0U) {
            if (*p < 0x80U || *p > 0xbfU) return false;
            ++p;
        }
    }
    return true;
}

static bool visible_ascii(const char *p)
{
    for (; *p != '\0'; ++p) {
        unsigned char a = (unsigned char)*p;
        if (a < 0x21U || a > 0x7eU) return false;
    }
    return true;
}

static bool decimal_digits(const char *p)
{
    for (; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

static int read_private_config(const char *path, T_DjiUserInfo *info)
{
    char buffer[CONFIG_LIMIT + 1U];
    struct stat st;
    ssize_t got;
    size_t used = 0U, line_number = 0U;
    unsigned int seen = 0U;
    int result = -1, fd = -1;
    struct field {
        const char *name;
        char *destination;
        size_t capacity;
        bool terminated;
    } fields[] = {
        {"APP_NAME", info->appName, sizeof(info->appName), true},
        {"APP_ID", info->appId, sizeof(info->appId), false},
        {"APP_KEY", info->appKey, sizeof(info->appKey), false},
        {"APP_LICENSE", info->appLicense, sizeof(info->appLicense), false},
        {"DEVELOPER_ACCOUNT", info->developerAccount, sizeof(info->developerAccount), true},
        {"BAUD_RATE", info->baudRate, sizeof(info->baudRate), true}
    };
    memset(buffer, 0, sizeof(buffer));
    memset(info, 0, sizeof(*info));
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 07777) != 0600 ||
        st.st_nlink != 1 || st.st_size <= 0 || st.st_size > CONFIG_LIMIT) {
        fputs("config rejected: require owned, non-symlink, single-link regular 0600 file, 1..4096 bytes\n", stderr);
        goto done;
    }
    while (used < CONFIG_LIMIT) {
        got = read(fd, buffer + used, CONFIG_LIMIT - used);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) goto invalid;
        if (got == 0) break;
        used += (size_t)got;
    }
    /* Reject files grown beyond the original bounded size while reading. */
    if (used == CONFIG_LIMIT) {
        char extra;
        do { got = read(fd, &extra, 1U); } while (got < 0 && errno == EINTR);
        if (got != 0) goto invalid;
    }
    if (used == 0U || memchr(buffer, '\0', used) != NULL) goto invalid;
    buffer[used] = '\0';
    char *line = buffer;
    if (used >= 3U && (unsigned char)line[0] == 0xefU &&
        (unsigned char)line[1] == 0xbbU && (unsigned char)line[2] == 0xbfU) line += 3;
    while (*line != '\0') {
        char *next = strchr(line, '\n');
        if (next != NULL) *next++ = '\0';
        ++line_number;
        size_t length = strlen(line);
        if (length != 0U && line[length - 1U] == '\r') line[--length] = '\0';
        if (length != 0U && line[0] != '#') {
            char *equals = strchr(line, '=');
            if (equals == NULL) goto invalid;
            *equals++ = '\0';
            size_t i, value_length = strlen(equals);
            for (i = 0U; i < sizeof(fields) / sizeof(fields[0]); ++i) {
                if (strcmp(fields[i].name, line) == 0) break;
            }
            if (i == sizeof(fields) / sizeof(fields[0]) ||
                (seen & (1U << i)) != 0U || value_length == 0U ||
                value_length > fields[i].capacity - (fields[i].terminated ? 1U : 0U) ||
                equals[0] == '\'' || equals[0] == '"' ||
                equals[value_length - 1U] == '\'' || equals[value_length - 1U] == '"') goto invalid;
            if (i == 0U) {
                if (!valid_utf8_name((const unsigned char *)equals)) goto invalid;
            } else if (!visible_ascii(equals)) goto invalid;
            if (i == 1U && !decimal_digits(equals)) goto invalid;
            if (i == 4U) {
                char *at = strchr(equals, '@');
                if (at == NULL || at == equals || at[1] == '\0' || strchr(at + 1, '@') != NULL) goto invalid;
            }
            if (i == 5U && strcmp(equals, "460800") != 0) goto invalid;
            memcpy(fields[i].destination, equals, value_length);
            seen |= 1U << i;
        }
        if (next == NULL) break;
        line = next;
    }
    if (seen != (1U << (sizeof(fields) / sizeof(fields[0]))) - 1U) goto invalid;
    result = 0;
    goto done;
invalid:
    fprintf(stderr, "config rejected: invalid format, duplicate/unknown/missing field, encoding or byte capacity (line %zu); values suppressed\n", line_number);
done:
    if (fd >= 0) close(fd);
    erase(buffer, sizeof(buffer));
    if (result != 0) erase(info, sizeof(*info));
    return result;
}

static T_DjiReturnCode private_sdk_log(const uint8_t *data, uint16_t size)
{
    pthread_mutex_lock(&sdk_log_mutex);
    size_t remaining = (size_t)size;
    const uint8_t *p = data;
    if (remaining > SDK_LOG_LIMIT - sdk_log_bytes) remaining = SDK_LOG_LIMIT - sdk_log_bytes;
    while (remaining != 0U && sdk_log_fd >= 0) {
        ssize_t count = write(sdk_log_fd, p, remaining);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        p += count;
        remaining -= (size_t)count;
        sdk_log_bytes += (size_t)count;
    }
    pthread_mutex_unlock(&sdk_log_mutex);
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static bool sdk_result(const char *phase, T_DjiReturnCode result)
{
    printf("event=phase_result phase=%s code=0x%016" PRIx64 "\n", phase, (uint64_t)result);
    return result == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

/* The official HAL silently succeeds for host mode without LIBUSB_INSTALLED,
 * and for unknown interfaces. Guard these paths without modifying vendor code.
 * FunctionFS reads/writes may still block; the external guardian is required.
 */
static T_DjiReturnCode device_usb_init(T_DjiHalUsbBulkInfo info, T_DjiUsbBulkHandle *handle)
{
    if (handle == NULL) return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    T_DjiUsbBulkHandle aligned = NULL;
    memcpy(handle, &aligned, sizeof(aligned));
    if (info.isUsbHost) return DJI_ERROR_SYSTEM_MODULE_CODE_NONSUPPORT;
    if (info.channelInfo.interfaceNum != LINUX_USB_BULK1_INTERFACE_NUM &&
        info.channelInfo.interfaceNum != LINUX_USB_BULK2_INTERFACE_NUM &&
        info.channelInfo.interfaceNum != LINUX_USB_BULK3_INTERFACE_NUM)
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    T_DjiReturnCode result = HalUsbBulk_Init(info, &aligned);
    /* Vendor Init can allocate before an endpoint-open failure. Do not pass the
     * partially initialized opaque handle onward; process exit reclaims it.
     */
    if (result != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) aligned = NULL;
    memcpy(handle, &aligned, sizeof(aligned));
    return result;
}

static T_DjiReturnCode device_usb_write(T_DjiUsbBulkHandle handle, const uint8_t *data,
                                      uint32_t length, uint32_t *actual)
{
    if (actual == NULL || handle == NULL || data == NULL || length == 0U)
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    *actual = 0U;
    return HalUsbBulk_WriteData(handle, data, length, actual);
}

static T_DjiReturnCode device_usb_read(T_DjiUsbBulkHandle handle, uint8_t *data,
                                     uint32_t length, uint32_t *actual)
{
    if (actual == NULL || handle == NULL || data == NULL || length == 0U)
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    *actual = 0U;
    return HalUsbBulk_ReadData(handle, data, length, actual);
}

static int prepare_environment(void)
{
#if L3_FIX_MOP_USB_PADDING
    puts("event=platform usb_copy_fix=pinned_3_18_arm64 source_allocations=unchanged");
#else
    puts("event=platform usb_copy_fix=off");
#endif
    T_DjiOsalHandler osal = {
        .TaskCreate = L3Osal_TaskCreate, .TaskDestroy = L3Osal_TaskDestroy,
        .TaskSleepMs = Osal_TaskSleepMs, .MutexCreate = L3Osal_MutexCreate,
        .MutexDestroy = Osal_MutexDestroy, .MutexLock = Osal_MutexLock,
        .MutexUnlock = Osal_MutexUnlock, .SemaphoreCreate = L3Osal_SemaphoreCreate,
        .SemaphoreDestroy = L3Osal_SemaphoreDestroy, .SemaphoreWait = Osal_SemaphoreWait,
        .SemaphoreTimedWait = Osal_SemaphoreTimedWait, .SemaphorePost = Osal_SemaphorePost,
        .Malloc = Osal_Malloc, .Free = Osal_Free, .GetRandomNum = Osal_GetRandomNum,
        .GetTimeMs = Osal_GetTimeMs, .GetTimeUs = Osal_GetTimeUs
    };
    T_DjiHalUsbBulkHandler usb = {
        .UsbBulkInit = device_usb_init, .UsbBulkDeInit = HalUsbBulk_DeInit,
        .UsbBulkWriteData = device_usb_write, .UsbBulkReadData = device_usb_read,
        .UsbBulkGetDeviceInfo = HalUsbBulk_GetDeviceInfo
    };
    T_DjiFileSystemHandler fs = {
        .FileOpen = Osal_FileOpen, .FileClose = Osal_FileClose, .FileWrite = Osal_FileWrite,
        .FileRead = Osal_FileRead, .FileSync = Osal_FileSync, .FileSeek = Osal_FileSeek,
        .DirOpen = Osal_DirOpen, .DirClose = Osal_DirClose, .DirRead = Osal_DirRead,
        .Mkdir = Osal_Mkdir, .Unlink = Osal_Unlink, .Rename = Osal_Rename, .Stat = Osal_Stat
    };
    T_DjiLoggerConsole console = {
        .func = private_sdk_log, .consoleLevel = DJI_LOGGER_CONSOLE_LOG_LEVEL_DEBUG,
        .isSupportColor = false
    };
#if L3_USE_OFFICIAL_SOCKET
    T_DjiSocketHandler socket_handler = {
        .Socket = Osal_Socket, .Bind = Osal_Bind, .Close = Osal_Close,
        .UdpSendData = Osal_UdpSendData, .UdpRecvData = Osal_UdpRecvData,
        .TcpListen = Osal_TcpListen, .TcpAccept = Osal_TcpAccept,
        .TcpConnect = Osal_TcpConnect, .TcpSendData = Osal_TcpSendData,
        .TcpRecvData = Osal_TcpRecvData
    };
    puts("event=platform socket_adapter=official_blocking sysctl_writes=blocked");
#else
    T_DjiSocketHandler socket_handler = *L3Metadata_GetSocketHandler();
    puts("event=platform socket_adapter=bounded_nonblocking sysctl_writes=none");
#endif
    if (!sdk_result("register_osal", DjiPlatform_RegOsalHandler(&osal))) return -1;
    if (!sdk_result("register_private_log", DjiLogger_AddConsole(&console))) return -1;
    if (!sdk_result("register_usb_bulk", DjiPlatform_RegHalUsbBulkHandler(&usb))) return -1;
    if (!sdk_result("register_socket", DjiPlatform_RegSocketHandler(&socket_handler))) return -1;
    if (!sdk_result("register_filesystem", DjiPlatform_RegFileSystemHandler(&fs))) return -1;
    return 0;
}

static int prepare_private_runtime(void)
{
    struct stat st;
    struct rlimit no_core = {0, 0};
    struct sigaction action;
    if (stat(".", &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 07777) != 0700 || access(".", W_OK | X_OK) != 0) {
        fputs("inspect rejected: run inside a fresh owned writable 0700 directory\n", stderr);
        return -1;
    }
    if (setrlimit(RLIMIT_CORE, &no_core) != 0) {
        fputs("inspect rejected: cannot disable core dumps\n", stderr);
        return -1;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) return -1;
    umask(0077);
    sdk_log_fd = open("sdk.private.log", O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (sdk_log_fd < 0) {
        fputs("inspect rejected: cannot create new private SDK log (use a fresh directory)\n", stderr);
        return -1;
    }
    return 0;
}

typedef struct {
    pthread_mutex_t lock;
    uint8_t *data;
    size_t bytes;
    uint64_t callbacks;
    uint64_t dropped_bytes;
    uint32_t largest_chunk;
    bool accepting;
    bool overflow;
} RgbCapture;

static uint64_t saturated_add(uint64_t a, uint64_t b)
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

/* No SDK call, allocation, clock, log, disk or network operation in this sink.
 * Keep a continuous prefix: once any entire callback cannot fit, never append
 * a later smaller callback. One callback is not a decoded frame or access unit.
 */
static void capture_h264(const uint8_t *data, uint32_t size, void *user)
{
    RgbCapture *capture = user;
    if (data == NULL || size == 0U) return;
    pthread_mutex_lock(&capture->lock);
    capture->callbacks = saturated_add(capture->callbacks, 1U);
    if (size > capture->largest_chunk) capture->largest_chunk = size;
    if (!capture->accepting) {
        capture->dropped_bytes = saturated_add(capture->dropped_bytes, size);
    } else if ((size_t)size > RGB_BUFFER_CAPACITY - capture->bytes) {
        capture->overflow = true;
        capture->accepting = false;
        capture->dropped_bytes = saturated_add(capture->dropped_bytes, size);
    } else {
        memcpy(capture->data + capture->bytes, data, size);
        capture->bytes += size;
        if (capture->bytes == RGB_BUFFER_CAPACITY) {
            capture->overflow = true;
            capture->accepting = false;
        }
    }
    pthread_mutex_unlock(&capture->lock);
}

static bool time_before(const struct timespec *a, const struct timespec *b)
{
    return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}

static int save_h264(const RgbCapture *capture)
{
    size_t written = 0U;
    int error = 0;
    int fd = open("l3-rgb.private.h264", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        printf("event=rgb_probe phase=save complete=no bytes=0 expected=%zu errno=%d\n", capture->bytes, errno);
        return -1;
    }
    while (written < capture->bytes) {
        ssize_t count = write(fd, capture->data + written, capture->bytes - written);
        if (count < 0 && errno == EINTR && !stop_requested) continue;
        if (count <= 0) { error = count < 0 ? errno : EIO; break; }
        written += (size_t)count;
        if (stop_requested) { error = EINTR; break; }
    }
    if (error == 0 && fsync(fd) != 0) error = errno;
    if (close(fd) != 0 && error == 0) error = errno;
    printf("event=rgb_probe phase=save complete=%s bytes=%zu expected=%zu errno=%d visible_confirmed=no\n",
           error == 0 ? "yes" : "no", written, capture->bytes, error);
    /* A failed/forced save may leave a private partial file. It is never
     * labelled complete; the caller must decode and verify any saved prefix.
     */
    return error == 0 ? 0 : -1;
}

static int run_rgb_probe(E_DjiMountPosition mount, unsigned int seconds)
{
    RgbCapture *capture = calloc(1U, sizeof(*capture));
    if (capture == NULL) {
        puts("event=rgb_probe phase=buffer_ready ready=no reason=allocation_failed");
        return 1;
    }
    if (pthread_mutex_init(&capture->lock, NULL) != 0) {
        free(capture);
        puts("event=rgb_probe phase=buffer_ready ready=no reason=mutex_failed");
        return 1;
    }
    capture->data = malloc(RGB_BUFFER_CAPACITY);
    if (capture->data == NULL) {
        pthread_mutex_destroy(&capture->lock);
        free(capture);
        puts("event=rgb_probe phase=buffer_ready ready=no reason=allocation_failed");
        return 1;
    }
    /* Touch all allocated pages before exposing the sink to SDK callbacks. */
    erase(capture->data, RGB_BUFFER_CAPACITY);
    capture->accepting = true;
    printf("event=rgb_probe phase=buffer_ready ready=yes capacity=%u psdk_port=%u seconds=%u source=DEFAULT visible_confirmed=no\n",
           RGB_BUFFER_CAPACITY, (unsigned int)mount, seconds);

    bool liveview_started = false, start_attempted = false, delivery_drained = true;
    bool clock_error = false, saved = false, stop_attempted = false;
    T_DjiReturnCode start_code = DJI_ERROR_SYSTEM_MODULE_CODE_NONSUPPORT_IN_CURRENT_STATE;
    T_DjiReturnCode stop_code = DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
    T_DjiReturnCode deinit_code = DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
    struct timespec start_time = {0, 0}, finish_time = {0, 0};
    bool elapsed_valid = false;
    const char *reason = "not_started";
    puts("event=rgb_probe phase=liveview_init_begin blocking=yes");
    T_DjiReturnCode init_code = DjiLiveview_Init();
    printf("event=rgb_probe phase=liveview_init code=0x%016" PRIx64 "\n", (uint64_t)init_code);
    if (init_code != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) goto summarize;
    liveview_started = true;
    if (stop_requested) { reason = "interrupted"; goto deinit; }
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0) {
        clock_error = true;
        reason = "clock_error";
        goto deinit;
    }
    struct timespec deadline = start_time;
    deadline.tv_sec += (time_t)seconds;
    start_attempted = true;
    delivery_drained = false;
    puts("event=rgb_probe phase=start_begin blocking=yes");
    start_code = l3_rgb_probe_start(mount, capture_h264, capture);
    printf("event=rgb_probe phase=start code=0x%016" PRIx64 "\n", (uint64_t)start_code);
    reason = "start_error";
    if (start_code == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        reason = "deadline";
        while (!stop_requested) {
            pthread_mutex_lock(&capture->lock);
            bool overflow = capture->overflow;
            pthread_mutex_unlock(&capture->lock);
            if (overflow) { reason = "overflow"; break; }
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
                clock_error = true;
                reason = "clock_error";
                break;
            }
            if (!time_before(&now, &deadline)) break;
            struct timespec wake = now;
            wake.tv_nsec += 20L * 1000L * 1000L;
            if (wake.tv_nsec >= 1000L * 1000L * 1000L) {
                ++wake.tv_sec;
                wake.tv_nsec -= 1000L * 1000L * 1000L;
            }
            if (time_before(&deadline, &wake)) wake = deadline;
            int sleep_error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, NULL);
            if (sleep_error != 0 && sleep_error != EINTR) {
                clock_error = true;
                reason = "clock_error";
                break;
            }
        }
    }
    if (stop_requested) reason = "interrupted";
    /* Never hold this mutex while calling stop: stop waits for active sinks. */
    pthread_mutex_lock(&capture->lock);
    capture->accepting = false;
    pthread_mutex_unlock(&capture->lock);
    elapsed_valid = clock_gettime(CLOCK_MONOTONIC, &finish_time) == 0;
    if (!elapsed_valid) clock_error = true;
    printf("event=rgb_probe phase=stop_begin blocking=yes reason=%s\n", reason);
    stop_attempted = true;
    stop_code = l3_rgb_probe_stop();
    /* Adapter stop disables and drains delivery before its SDK stop call, even
     * on SDK stop error. BUSY may also mean the adapter control-lock was not
     * acquired; conservatively retain heap context until process exit even
     * when this code came from the SDK after delivery had already drained.
     * This executable itself uses exactly one control thread/start attempt.
     */
    delivery_drained = stop_code != DJI_ERROR_SYSTEM_MODULE_CODE_BUSY;
    printf("event=rgb_probe phase=stop stopcode=0x%016" PRIx64 " delivery=%s\n",
           (uint64_t)stop_code, delivery_drained ? "disabled_drained" : "unproven");
deinit:
    puts("event=rgb_probe phase=liveview_deinit_begin blocking=yes");
    deinit_code = DjiLiveview_Deinit();
    printf("event=rgb_probe phase=liveview_deinit code=0x%016" PRIx64 "\n", (uint64_t)deinit_code);
summarize:
    pthread_mutex_lock(&capture->lock);
    size_t bytes = capture->bytes;
    uint64_t callbacks = capture->callbacks, dropped = capture->dropped_bytes;
    uint32_t largest_chunk = capture->largest_chunk;
    bool overflow = capture->overflow;
    pthread_mutex_unlock(&capture->lock);
    uint64_t elapsed_ms = 0U;
    if (elapsed_valid && !time_before(&finish_time, &start_time)) {
        elapsed_ms = (uint64_t)(finish_time.tv_sec - start_time.tv_sec) * 1000U;
        long difference = finish_time.tv_nsec - start_time.tv_nsec;
        if (difference < 0) elapsed_ms -= (uint64_t)(-difference) / 1000000U;
        else elapsed_ms += (uint64_t)difference / 1000000U;
    }
    printf("event=rgb_probe phase=summary bytes=%zu callbacks=%" PRIu64 " dropped_bytes=%" PRIu64
           " largest_chunk=%u overflow=%s start_attempted=%s stop_attempted=%s stopcode=0x%016" PRIx64
           " control_elapsed_ms=%" PRIu64 " elapsed_valid=%s interrupted=%s visible_confirmed=no\n",
           bytes, callbacks, dropped, largest_chunk, overflow ? "yes" : "no",
           start_attempted ? "yes" : "no", stop_attempted ? "yes" : "no", (uint64_t)stop_code,
           elapsed_ms, elapsed_valid ? "yes" : "no", stop_requested ? "yes" : "no");
    /* Capture bytes are immutable after stop/drain, including SDK stop failure.
     * All file I/O happens on this control thread after stop and Liveview Deinit.
     */
    if (delivery_drained && bytes != 0U && !stop_requested) saved = save_h264(capture) == 0;
    else printf("event=rgb_probe phase=save_skipped reason=%s\n",
                !delivery_drained ? "delivery_unproven" : stop_requested ? "interrupted" : "empty");
    int result = start_attempted && start_code == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS &&
                 stop_attempted && stop_code == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS &&
                 liveview_started && deinit_code == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS &&
                 !clock_error && !overflow && !stop_requested && bytes != 0U && saved ? 0 : 1;
    if (delivery_drained) {
        free(capture->data);
        pthread_mutex_destroy(&capture->lock);
        free(capture);
    } else {
        puts("event=rgb_probe phase=context retained=yes reason=delivery_unproven");
    }
    return result;
}

static bool parse_number(const char *text, unsigned int minimum, unsigned int maximum,
                         unsigned int *value)
{
    size_t length = strlen(text);
    unsigned int parsed = 0U;
    if (length == 0U || length > 5U) return false;
    for (size_t i = 0U; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        parsed = parsed * 10U + (unsigned int)(text[i] - '0');
    }
    if (parsed < minimum || parsed > maximum) return false;
    *value = parsed;
    return true;
}

static int64_t point_now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

typedef struct {
    pthread_mutex_t lock;
    uint64_t callbacks;
    uint64_t bytes;
} DiagnosticVideo;

static void count_diagnostic_video(const uint8_t *bytes, uint32_t size, void *opaque)
{
    if (bytes == NULL || size == 0U) return;
    DiagnosticVideo *video = opaque;
    pthread_mutex_lock(&video->lock);
    video->callbacks = saturated_add(video->callbacks, 1U);
    video->bytes = saturated_add(video->bytes, size);
    pthread_mutex_unlock(&video->lock);
}

/* Bounded idle/Liveview diagnosis, with no point-recording or MOP API call.
 * Count-only video avoids retaining sensor data or filling the probe RAM sink.
 * An outer timeout is still needed for blocking SDK Init/Start/Stop/DeInit. */
static int run_sdk_diagnostic(E_DjiMountPosition mount, unsigned int seconds, bool with_video)
{
    DiagnosticVideo video = { .lock = PTHREAD_MUTEX_INITIALIZER };
    bool initialized = false, start_attempted = false;
    int status = 1;
    T_DjiReturnCode rc;
    printf("event=sdk_diagnostic phase=begin seconds=%u video=%s recording_start=no\n",
           seconds, with_video ? "yes" : "no");
    if (with_video) {
        puts("event=phase_begin phase=diagnostic_liveview_init blocking=yes");
        if (!sdk_result("diagnostic_liveview_init", DjiLiveview_Init())) goto finish;
        initialized = true;
        if (stop_requested) goto finish;
        start_attempted = true;
        puts("event=phase_begin phase=diagnostic_video_start blocking=yes");
        if (!sdk_result("diagnostic_video_start", l3_rgb_probe_start(mount, count_diagnostic_video, &video)))
            goto finish;
    }
    int64_t begin = point_now_ms();
    if (begin < 0) goto finish;
    int64_t deadline = begin + (int64_t)seconds * 1000, next_report = begin;
    puts("event=sdk_diagnostic phase=hold_begin");
    while (!stop_requested) {
        int64_t now = point_now_ms();
        if (now < 0) goto finish;
        if (now >= deadline) { status = 0; break; }
        if (now >= next_report) {
            pthread_mutex_lock(&video.lock);
            uint64_t callbacks = video.callbacks, bytes = video.bytes;
            pthread_mutex_unlock(&video.lock);
            printf("event=sdk_diagnostic phase=hold elapsed_ms=%" PRId64
                   " callbacks=%" PRIu64 " bytes=%" PRIu64 "\n", now - begin, callbacks, bytes);
            next_report = now + 1000;
        }
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 100L * 1000L * 1000L };
        if (nanosleep(&pause, NULL) != 0 && errno != EINTR) goto finish;
    }
finish:
    if (start_attempted) {
        puts("event=phase_begin phase=diagnostic_video_stop blocking=yes");
        rc = l3_rgb_probe_stop();
        if (!sdk_result("diagnostic_video_stop", rc)) status = 1;
        if (rc == DJI_ERROR_SYSTEM_MODULE_CODE_BUSY) {
            puts("event=sdk_diagnostic phase=exit delivery_unproven=yes sdk_cleanup=skipped");
            fflush(stdout);
            _exit(3);
        }
    }
    if (initialized) {
        puts("event=phase_begin phase=diagnostic_liveview_deinit blocking=yes");
        if (!sdk_result("diagnostic_liveview_deinit", DjiLiveview_Deinit())) status = 1;
    }
    if (with_video && video.bytes == 0U) status = 1;
    if (stop_requested) status = 1;
    printf("event=sdk_diagnostic phase=summary result=%s callbacks=%" PRIu64
           " bytes=%" PRIu64 " interrupted=%s recording_start=no\n",
           status == 0 ? "ok" : "failed", video.callbacks, video.bytes, stop_requested ? "yes" : "no");
    pthread_mutex_destroy(&video.lock);
    return status;
}

/* Explicit numeric IPv4 only: no DNS, discovery or network scan. This happens
 * before Core/recording startup. The caller owns this heap sink and socket.
 * A successful TCP connection does not prove an SSH-forwarded peer will stay up.
 */
static L3p1TcpSink *connect_point_sink(const char *host, unsigned int port)
{
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) return NULL;
    int64_t now = point_now_ms();
    int error = now < 0 ? EIO : 0;
    int64_t deadline = now + 2000;
    int fd = -1;
    puts("event=point_probe phase=tcp_connect_begin timeout_ms=2000");
    if (error == 0) {
        fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) error = errno;
    }
    if (error == 0 && connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        error = errno;
        if (error == EINPROGRESS || error == EALREADY || error == EINTR) {
            error = 0;
            for (;;) {
                if (stop_requested) { error = EINTR; break; }
                now = point_now_ms();
                if (now < 0) { error = EIO; break; }
                if (now >= deadline) { error = ETIMEDOUT; break; }
                struct pollfd wait = { .fd = fd, .events = POLLOUT };
                int ready = poll(&wait, 1, (int)(deadline - now));
                if (ready == 0) { error = ETIMEDOUT; break; }
                if (ready < 0) {
                    if (errno == EINTR) continue;
                    error = errno;
                    break;
                }
                if ((wait.revents & POLLNVAL) != 0) { error = EBADF; break; }
                socklen_t length = sizeof(error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0) error = errno;
                if (error == 0 && (wait.revents & POLLHUP) != 0) error = EPIPE;
                break;
            }
        }
    }
    L3p1TcpSink *sink = error == 0 ? calloc(1U, sizeof(*sink)) : NULL;
    if (sink == NULL && error == 0) error = ENOMEM;
    if (error != 0) {
        if (fd >= 0) close(fd);
        printf("event=point_probe phase=tcp_connect connected=no errno=%d recording_started=no\n", error);
        return NULL;
    }
    sink->fd = fd;
    sink->timeout_ms = 200U;
    printf("event=point_probe phase=tcp_connect connected=yes port=%u sink_timeout_ms=200\n", port);
    return sink;
}

static int run_point_probe(E_DjiMountPosition mount, unsigned int seconds, L3p1TcpSink *sink)
{
    L3PointProbeConfig config = {
        .mount = mount, .duration_ms = seconds * 1000U,
        .confirmed_no_existing_recording = true,
        .sink = l3p1_tcp_sink, .sink_context = sink, .cancel_flag = &stop_requested
    };
    L3PointProbeReport report;
    L3PointProbeController *retained = NULL;
    memset(&report, 0, sizeof(report));
    printf("event=point_probe phase=begin psdk_port=%u seconds=%u max_packets=30 max_attempts=60 exclusive_test=yes recording_start=explicit\n",
           (unsigned int)mount, seconds);
    L3PointProbeOutcome outcome = l3_point_probe_run(&config, &report, &retained);
    /* Emit Stop status before doing any cleanup or exiting. This is an SDK
     * command ACK, not an independently observed physical recording-state ACK.
     */
    printf("event=point_probe phase=stop_confirmation start_attempted=%s stop_attempts=%u stop_finished=%s command_ack=%s stopcode=0x%016" PRIx64 " sensor_state_verified=no\n",
           report.start_attempted ? "yes" : "no", report.stop_attempts,
           report.stop_finished ? "yes" : "no", report.stop_command_confirmed ? "yes" : "no",
           (uint64_t)report.stop_code);
    printf("event=point_probe phase=summary outcome=%u start_finished=%s reader_started=%s reader_finished=%s cleanup_safe=%s sink_failed=%s cancelled=%s clock_failed=%s\n",
           (unsigned int)outcome, report.start_finished ? "yes" : "no",
           report.reader_started ? "yes" : "no", report.reader_finished ? "yes" : "no",
           report.cleanup_safe ? "yes" : "no", report.sink_failed ? "yes" : "no",
           report.cancelled ? "yes" : "no", report.clock_failed ? "yes" : "no");
    printf("event=point_probe phase=sdk_codes camera_type=0x%016" PRIx64 " mop_init=0x%016" PRIx64
           " start=0x%016" PRIx64 " stop=0x%016" PRIx64 "\n",
           (uint64_t)report.camera_type_code, (uint64_t)report.mop_init_code,
           (uint64_t)report.start_code, (uint64_t)report.stop_code);
    printf("event=point_probe phase=reader_stats complete=%s result=%u attempts=%u packets=%u malformed=%u sdk_code=0x%016" PRIx64
           " close_code=0x%016" PRIx64 " destroy_code=0x%016" PRIx64 "\n",
           report.reader_finished ? "yes" : "no", (unsigned int)report.reader_result,
           report.reader_report.receive_attempts, report.reader_report.packets_delivered,
           report.reader_report.malformed_packets, (uint64_t)report.reader_report.sdk_error,
           (uint64_t)report.reader_report.close_error, (uint64_t)report.reader_report.destroy_error);
    printf("event=point_probe phase=timing start_requested_monotonic_ns=%" PRIu64 " deadline_monotonic_ns=%" PRIu64
           " last_stop_requested_monotonic_ns=%" PRIu64 " stop_ack_monotonic_ns=%" PRIu64 "\n",
           report.start_requested_monotonic_ns, report.deadline_monotonic_ns,
           report.last_stop_requested_monotonic_ns, report.stop_ack_monotonic_ns);
    bool stop_unconfirmed = report.start_attempted && !report.stop_command_confirmed;
    if (stop_unconfirmed) puts("event=point_probe phase=operator_action manual_stop_required=yes process_exit_stops_recording=no");
    if (retained != NULL || !report.cleanup_safe) {
        puts("event=point_probe phase=exit cleanup_safe=no sdk_cleanup=skipped fd_context=retained_until_process_exit");
        /* Do not close/reuse the socket or free sink/controller/config state
         * while the SDK reader or stopper may still be executing. _exit kills
         * process threads; it does NOT stop any aircraft recording.
         */
        fflush(stdout);
        fflush(stderr);
        _exit(stop_unconfirmed ? 4 : 3);
    }
    printf("event=point_probe phase=tcp_sink failed=%s errno=%d\n", sink->failed ? "yes" : "no", sink->error_number);
    return outcome == L3_POINT_PROBE_OK && !sink->failed ? 0 : 1;
}

typedef struct {
    L3Stream *stream;
    atomic_bool video_failed;
} LiveStreamContext;

static void publish_video(const uint8_t *bytes, uint32_t size, void *opaque)
{
    LiveStreamContext *context = opaque;
    if (!l3_stream_publish_h264(context->stream, bytes, size)) atomic_store(&context->video_failed, true);
}

static bool publish_points(const uint8_t *bytes, uint32_t size, void *opaque)
{
    LiveStreamContext *context = opaque;
    return l3_stream_publish_points(context->stream, bytes, size);
}

typedef enum {
    L3_STREAM_POINTS = 0,
    L3_STREAM_VIDEO = 1,
    L3_STREAM_BOTH = 2
} L3StreamMode;

/* These are camera-manager metadata IDs, not Liveview source arguments. Never
 * cast a range result to E_DjiLiveViewCameraSource or guess a L3 source ID. */
static int report_video_info(E_DjiMountPosition mount, const char *phase)
{
    E_DjiCameraType type = DJI_CAMERA_TYPE_UNKNOWN;
    T_DjiReturnCode rc = DjiCameraManager_GetCameraType(mount, &type);
    if (rc != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS || type != DJI_CAMERA_TYPE_L3) {
        printf("event=video_info phase=%s result=failed exact_l3=no code=0x%016" PRIx64 "\n",
               phase, (uint64_t)rc);
        return 1;
    }
    T_DjiCameraManagerRangeList range = {0};
    T_DjiCameraCurrentCameraStatus camera = {0};
    puts("event=phase_begin phase=video_source_range blocking=yes");
    T_DjiReturnCode range_rc = DjiCameraManager_GetStreamSourceRange(mount, &range);
    size_t capacity = sizeof(range.streamSource) / sizeof(range.streamSource[0]);
    bool valid_range = range_rc == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS &&
                       range.size > 0 && range.size <= capacity;
    printf("event=video_info phase=%s query=range code=0x%016" PRIx64
           " valid=%s count=%u sources=", phase, (uint64_t)range_rc,
           valid_range ? "yes" : "no", range_rc == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS ? range.size : 0U);
    if (valid_range) {
        for (size_t i = 0; i < range.size; ++i)
            printf("%s%u", i == 0 ? "" : ",", (unsigned int)range.streamSource[i]);
    }
    putchar('\n');
    puts("event=phase_begin phase=current_video_source blocking=yes");
    T_DjiReturnCode status_rc = DjiCameraManager_GetCurrentCameraStatus(mount, &camera);
    printf("event=video_info phase=%s query=current code=0x%016" PRIx64,
           phase, (uint64_t)status_rc);
    if (status_rc == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS)
        printf(" source=%u work_mode=%u zoom_tenths=%u",
               camera.liveview_source_stream, camera.work_mode, camera.zoom_factor);
    puts(" recording_start=no source_write=no");
    return valid_range && status_rc == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS ? 0 : 1;
}

static int report_camera_firmware(E_DjiMountPosition mount)
{
    T_DjiCameraManagerFirmwareVersion firmware = {0};
    puts("event=phase_begin phase=camera_firmware blocking=yes");
    T_DjiReturnCode rc = DjiCameraManager_GetFirmwareVersion(mount, &firmware);
    printf("event=camera_firmware psdk_port=%u code=0x%016" PRIx64,
           (unsigned int)mount, (uint64_t)rc);
    if (rc == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        const uint8_t *v = firmware.firmware_version;
        printf(" version=%u.%u.%u.%u hex=%02x%02x%02x%02x",
               (unsigned int)v[0], (unsigned int)v[1], (unsigned int)v[2], (unsigned int)v[3],
               (unsigned int)v[0], (unsigned int)v[1], (unsigned int)v[2], (unsigned int)v[3]);
    }
    putchar('\n');
    return rc == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS ? 0 : 1;
}

static bool check_video_source(E_DjiMountPosition mount, unsigned int expected)
{
    L3VideoControlResult current = l3_video_source_read(mount);
    if (!current.confirmed)
        return false;
    bool matched = (expected == L3_VIDEO_SOURCE_VISIBLE || expected == L3_VIDEO_SOURCE_POINT_CLOUD) &&
                   current.current_source == (int)expected;
    printf("event=video_source phase=check current=%d expected=%u matched=%s source_write=no\n",
           current.current_source, expected, matched ? "yes" : "no");
    return matched;
}

static void video_control_event(const char *action, int requested, L3VideoControlResult r, bool resubscribe)
{
    printf("event=video_control action=%s requested=%d previous=%d current=%d success=%s write_attempted=%s code=0x%016" PRIx64 " resubscribe=%s\n",
           action, requested, r.previous_source, r.current_source,
           r.confirmed ? "yes" : "no", r.write_attempted ? "yes" : "no", (uint64_t)r.code,
           resubscribe ? "yes" : "no");
}

typedef enum {
    VIDEO_ACTIVE, VIDEO_STOP_FOR_SWITCH, VIDEO_WAIT_RESUME, VIDEO_WAIT_CLIENT, VIDEO_FALLBACK
} VideoSwitchPhase;

typedef struct {
    char line[32];
    size_t used;
    bool discard;
    bool fast;
    VideoSwitchPhase phase;
    int source;
    int64_t switch_ms;
} VideoCommand;

/* One bounded input read per iteration; never interpret stdin as shell or SDK
 * packets. L3's actual camera sources are 1 (wide visible) and 14 (2D points).
 * These IDs must NOT be passed as Liveview source arguments. */
static bool poll_video_command(VideoCommand *input, E_DjiMountPosition mount, L3Stream *stream)
{
    struct pollfd fd = {.fd = STDIN_FILENO, .events = POLLIN};
    if (poll(&fd, 1, 0) <= 0) return false;
    char data[32];
    ssize_t count = read(STDIN_FILENO, data, sizeof(data));
    if (count == 0) return true;
    if (count < 0) return errno != EINTR && errno != EAGAIN;
    for (ssize_t i = 0; i < count; ++i) {
        if (data[i] != '\n') {
            if (data[i] < ' ' || data[i] > '~' || input->used == sizeof(input->line) - 1)
                input->discard = true;
            else if (!input->discard) input->line[input->used++] = data[i];
            continue;
        }
        input->line[input->used] = '\0';
        if (!input->discard && strcmp(input->line, "stop") == 0) return true;
        if (!input->discard && strcmp(input->line, "status") == 0 && input->phase == VIDEO_ACTIVE) {
            video_control_event("status", -1, l3_video_source_read(mount), false);
        } else if (!input->discard && strcmp(input->line, "resume") == 0 && input->phase == VIDEO_WAIT_RESUME) {
            /* The PC has joined its old receiver before sending resume. Drop
             * any sockets its retry loop opened before that join. */
            l3_stream_reset_video(stream);
            input->phase = VIDEO_WAIT_CLIENT;
            puts("event=fast_switch phase=video_relisten");
        } else if (!input->discard && input->phase == VIDEO_ACTIVE &&
                   (strcmp(input->line, "visible") == 0 || strcmp(input->line, "point") == 0)) {
            unsigned int source = strcmp(input->line, "visible") == 0 ?
                                  L3_VIDEO_SOURCE_VISIBLE : L3_VIDEO_SOURCE_POINT_CLOUD;
            input->switch_ms = point_now_ms();
            L3VideoControlResult result = l3_video_source_set(mount, source);
            bool changed = result.write_attempted && result.current_source != result.previous_source;
            if (input->fast)
                printf("event=fast_switch phase=source_readback elapsed_ms=%" PRId64 "\n",
                       point_now_ms() - input->switch_ms);
            /* A failed ACK can still leave the source changed or unknown. In
             * that case invalidate the old decoder too, without claiming success. */
            if (input->fast && changed) {
                input->source = result.current_source;
                input->phase = result.current_source < 0 ? VIDEO_FALLBACK : VIDEO_STOP_FOR_SWITCH;
            } else if (changed) {
                l3_stream_reset_video(stream);
                T_DjiReturnCode keyframe = DjiLiveview_RequestIntraframeFrameData(
                    (E_DjiLiveViewCameraPosition)mount, DJI_LIVEVIEW_CAMERA_SOURCE_DEFAULT);
                sdk_result("video_control_keyframe", keyframe);
            }
            video_control_event("switch", (int)source, result, input->fast && changed);
        } else {
            puts("event=video_control action=invalid current=-1 success=no code=invalid_command");
        }
        input->used = 0;
        input->discard = false;
    }
    return false;
}

static bool advance_video_switch(VideoCommand *input, E_DjiMountPosition mount,
                                 LiveStreamContext *context, unsigned int *previous_clients)
{
    if (input->phase == VIDEO_ACTIVE) return true;
    if (input->phase == VIDEO_FALLBACK) return false;
    if (input->phase == VIDEO_STOP_FOR_SWITCH) {
        if (!sdk_result("fast_video_stop", l3_rgb_probe_stop())) return false;
        if (!check_video_source(mount, (unsigned int)input->source)) return false;
        input->phase = VIDEO_WAIT_RESUME;
        printf("event=fast_switch phase=old_stopped elapsed_ms=%" PRId64 "\n",
               point_now_ms() - input->switch_ms);
    }
    int64_t elapsed = point_now_ms() - input->switch_ms;
    if (elapsed < 0 || elapsed >= 8000) return false;
    if (input->phase == VIDEO_WAIT_CLIENT) {
        L3StreamStats stats;
        l3_stream_stats(context->stream, &stats);
        if (stats.video_clients == 0) return true;
        printf("event=fast_switch phase=client_ready elapsed_ms=%" PRId64 "\n", elapsed);
        if (!sdk_result("fast_video_start", l3_rgb_probe_restart(mount, publish_video, context)))
            return false;
        input->phase = VIDEO_ACTIVE;
        *previous_clients = stats.video_clients;
        printf("event=fast_switch phase=resubscribed elapsed_ms=%" PRId64 "\n",
               point_now_ms() - input->switch_ms);
    }
    return true;
}

static int run_live_stream(E_DjiMountPosition mount, unsigned int seconds,
                           const char *bind_ip, unsigned int point_port, unsigned int video_port,
                           L3StreamMode mode, int expected_source, bool video_control, bool fast_switch)
{
    bool with_video = mode != L3_STREAM_POINTS;
    bool with_points = mode != L3_STREAM_VIDEO;
    LiveStreamContext *context = calloc(1, sizeof(*context));
    if (context == NULL) return 1;
    atomic_init(&context->video_failed, false);
    L3StreamConfig network = { .bind_ip = bind_ip, .point_port = (uint16_t)point_port,
                               .video_port = (uint16_t)video_port,
                               .video_queue_bytes = mode == L3_STREAM_VIDEO ? 8U * 1024U * 1024U : 0,
                               .send_timeout_ms = mode == L3_STREAM_VIDEO ? 3000 : 0 };
    context->stream = l3_stream_start(&network);
    if (context->stream == NULL) { free(context); return 1; }
    if (video_control) puts("event=stream phase=video_listen encoding=source_h264 reencode=no");
    int status = 1;
    bool liveview_initialized = false, video_start_attempted = false;
    T_DjiReturnCode rc = DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
    if (with_video) {
        (void)report_video_info(mount, "before_liveview");
        if (stop_requested) goto finish;
        if (expected_source >= 0 && !check_video_source(mount, (unsigned int)expected_source)) {
            puts("event=video_source phase=check result=failed");
            goto finish;
        }
        if (video_control) {
            /* L3's current encoder supplies its IDR at subscription startup;
             * RequestIntraframe ACK did not produce another IDR in testing.
             * Establish the real remote TCP client BEFORE starting Liveview. */
            int64_t until = point_now_ms() + 10000;
            bool connected = false;
            while (!stop_requested && point_now_ms() < until) {
                L3StreamStats waiting;
                l3_stream_stats(context->stream, &waiting);
                if (waiting.video_clients > 0) { connected = true; break; }
                struct timespec delay = {.tv_nsec = 100000000};
                nanosleep(&delay, NULL);
            }
            if (!connected) {
                puts("event=stream phase=failed reason=no_video_client_before_subscription");
                goto finish;
            }
        }
        rc = DjiLiveview_Init();
        if (!sdk_result("stream_liveview_init", rc)) goto finish;
        liveview_initialized = true;
        video_start_attempted = true;
        rc = l3_rgb_probe_start(mount, publish_video, context);
        sdk_result("stream_video_start", rc);
        puts("event=video_source requested=default value=0 content=unverified");
    }
    if (with_points && rc == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        L3PointProbeConfig recording = {
            .mount = mount, .duration_ms = seconds * 1000U, .continuous = true,
            .confirmed_no_existing_recording = true, .sink = publish_points,
            .sink_context = context, .cancel_flag = &stop_requested
        };
        L3PointProbeReport report;
        L3PointProbeController *retained = NULL;
        printf("event=stream phase=begin streams=%s seconds=%u point_port=%u video_port=%u recording_start=explicit\n",
               with_video ? "both" : "points", seconds, point_port, video_port);
        L3PointProbeOutcome outcome = l3_point_probe_run(&recording, &report, &retained);
        printf("event=stream phase=recording_stop outcome=%u stop_ack=%s packets=%u malformed=%u cleanup_safe=%s sensor_state_verified=no\n",
               (unsigned int)outcome, report.stop_command_confirmed ? "yes" : "no",
               report.reader_report.packets_delivered, report.reader_report.malformed_packets,
               report.cleanup_safe ? "yes" : "no");
        if (retained != NULL || !report.cleanup_safe) {
            /* Pending SDK workers still own context. Exit does not stop recording. */
            puts("event=stream phase=exit sdk_cleanup=skipped operator_stop_check_required=yes");
            fflush(stdout);
            _exit(report.stop_command_confirmed ? 3 : 4);
        }
        status = outcome == L3_POINT_PROBE_OK ? 0 : 1;
    } else if (with_video && !with_points && rc == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        /* Video-only: subscribe and forward H.264; no point recording is started. */
        int64_t begin = point_now_ms();
        if (begin < 0) goto finish;
        int64_t deadline = begin + (int64_t)seconds * 1000, next_report = begin;
        VideoCommand input = {.fast = fast_switch};
        unsigned int previous_clients = 0;
        if (video_control) video_control_event("status", -1, l3_video_source_read(mount), false);
        printf("event=stream phase=begin streams=video seconds=%u point_port=%u video_port=%u recording_start=no\n",
               seconds, point_port, video_port);
        while (!stop_requested) {
            int64_t now = point_now_ms();
            if (now < 0) goto finish;
            if (now >= deadline) { status = 0; break; }
            if (video_control && poll_video_command(&input, mount, context->stream)) {
                status = 0;
                break;
            }
            if (!advance_video_switch(&input, mount, context, &previous_clients)) {
                puts("event=fast_switch phase=video_fallback");
                status = 0; /* Only retry after the cleanup below succeeds. */
                break;
            }
            if (now >= next_report) {
                L3StreamStats stats;
                l3_stream_stats(context->stream, &stats);
                if (input.phase == VIDEO_ACTIVE && stats.video_clients > previous_clients) {
                    /* A remote viewer can join after the encoder's initial IDR.
                     * Ask for a fresh keyframe without changing source/bitrate. */
                    sdk_result("video_join_keyframe", DjiLiveview_RequestIntraframeFrameData(
                        (E_DjiLiveViewCameraPosition)mount, DJI_LIVEVIEW_CAMERA_SOURCE_DEFAULT));
                }
                previous_clients = stats.video_clients;
                printf("event=stream phase=video_hold elapsed_ms=%" PRId64 " video_bytes=%" PRIu64
                       " video_clients=%u send_errors=%" PRIu64
                       " sps=%" PRIu64 " pps=%" PRIu64 " idr=%" PRIu64 " first_idr=%" PRIu64 "\n",
                       now - begin, stats.video_bytes, (unsigned int)stats.video_clients, stats.send_errors,
                       stats.video_sps, stats.video_pps, stats.video_idr, stats.video_first_idr);
                next_report = now + 1000;
            }
            struct timespec pause = {.tv_nsec = (fast_switch ? 20L : 100L) * 1000L * 1000L};
            if (nanosleep(&pause, NULL) != 0 && errno != EINTR) goto finish;
        }
        if (stop_requested) status = 1;
    }
finish:
    if (video_start_attempted) {
        rc = l3_rgb_probe_stop();
        if (!sdk_result("stream_video_stop", rc)) status = 1;
        if (rc == DJI_ERROR_SYSTEM_MODULE_CODE_BUSY) {
            puts("event=stream phase=exit context_retained=yes delivery_unproven=yes");
            fflush(stdout);
            _exit(3);
        }
    }
    if (liveview_initialized && !sdk_result("stream_liveview_deinit", DjiLiveview_Deinit())) status = 1;
    L3StreamStats stats;
    l3_stream_stats(context->stream, &stats);
    printf("event=stream phase=summary point_packets=%" PRIu64 " video_bytes=%" PRIu64
           " point_dropped=%" PRIu64 " slow_disconnects=%" PRIu64 " send_errors=%" PRIu64 "\n",
           stats.point_packets, stats.video_bytes, stats.point_dropped, stats.slow_disconnects, stats.send_errors);
    if ((with_video && (atomic_load(&context->video_failed) || stats.video_bytes == 0)) ||
        (with_points && stats.point_packets == 0)) status = 1;
    l3_stream_stop(context->stream);
    free(context);
    return status;
}

int main(int argc, char **argv)
{
    const char *config = NULL;
    bool inspect = false, check_config = false, rgb_probe = false;
    bool point_probe = false, stop_pointcloud = false, exclusive_test = false;
    bool stream = false, sdk_diagnostic = false, diagnostic_video = false;
    bool video_info = false;
    bool video_control = false;
    bool fast_switch = false;
    int expected_source = -1;
    bool camera_firmware = false;
    const char *streams = NULL;
    const char *bind_ip = "127.0.0.1";
    unsigned int point_port = 9101, video_port = 9102;
    const char *tcp_host = NULL;
    unsigned int selected_mount = 0U, probe_seconds = 0U, tcp_port = 0U;
    L3p1TcpSink *point_sink = NULL;
    bool core_started = false, camera_started = false;
    int status = 1;
    unsigned int l3_count = 0U, query_successes = 0U;
    T_DjiUserInfo info;
    T_DjiAircraftInfoBaseInfo aircraft;
    const E_DjiMountPosition mounts[] = {
        DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1,
        DJI_MOUNT_POSITION_PAYLOAD_PORT_NO2,
        DJI_MOUNT_POSITION_PAYLOAD_PORT_NO3
    };
    memset(&info, 0, sizeof(info));
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 1) { usage(); return 0; }
    /* --help always wins and never reads a configuration or opens the SDK. */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) { usage(); return 0; }
    }
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--config") == 0 && config == NULL && i + 1 < argc) config = argv[++i];
        else if (strcmp(argv[i], "--inspect") == 0 && !inspect) inspect = true;
        else if (strcmp(argv[i], "--video-info") == 0 && !video_info) video_info = true;
        else if (strcmp(argv[i], "--video-control-stdin") == 0 && !video_control) video_control = true;
        else if (strcmp(argv[i], "--fast-switch") == 0 && !fast_switch) fast_switch = true;
        else if (strcmp(argv[i], "--expected-source") == 0 && expected_source < 0 && i + 1 < argc) {
            unsigned int value;
            if (!parse_number(argv[++i], 0U, 255U, &value)) return 2;
            expected_source = (int)value;
        }
        else if (strcmp(argv[i], "--check-config") == 0 && !check_config) check_config = true;
        else if (strcmp(argv[i], "--rgb-probe") == 0 && !rgb_probe) rgb_probe = true;
        else if (strcmp(argv[i], "--point-probe") == 0 && !point_probe) point_probe = true;
        else if (strcmp(argv[i], "--stream") == 0 && !stream) stream = true;
        else if (strcmp(argv[i], "--streams") == 0 && streams == NULL && i + 1 < argc) streams = argv[++i];
        else if (strcmp(argv[i], "--sdk-diagnostic") == 0 && !sdk_diagnostic) sdk_diagnostic = true;
        else if (strcmp(argv[i], "--diagnostic-video") == 0 && !diagnostic_video) diagnostic_video = true;
        else if (strcmp(argv[i], "--camera-firmware") == 0 && !camera_firmware) camera_firmware = true;
        else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) bind_ip = argv[++i];
        else if (strcmp(argv[i], "--point-port") == 0 && i + 1 < argc) {
            if (!parse_number(argv[++i], 1U, 65535U, &point_port)) return 2;
        }
        else if (strcmp(argv[i], "--video-port") == 0 && i + 1 < argc) {
            if (!parse_number(argv[++i], 1U, 65535U, &video_port)) return 2;
        }
        else if (strcmp(argv[i], "--stop-pointcloud-only") == 0 && !stop_pointcloud) stop_pointcloud = true;
        else if (strcmp(argv[i], "--exclusive-test") == 0 && !exclusive_test) exclusive_test = true;
        else if (strcmp(argv[i], "--tcp-host") == 0 && tcp_host == NULL && i + 1 < argc) tcp_host = argv[++i];
        else if (strcmp(argv[i], "--tcp-port") == 0 && tcp_port == 0U && i + 1 < argc) {
            if (!parse_number(argv[++i], 1U, 65535U, &tcp_port)) {
                fputs("--tcp-port requires 1..65535\n", stderr); return 2;
            }
        }
        else if (strcmp(argv[i], "--mount") == 0 && selected_mount == 0U && i + 1 < argc) {
            if (!parse_number(argv[++i], 1U, 3U, &selected_mount)) {
                fputs("--mount requires 1..3\n", stderr); return 2;
            }
        }
        else if (strcmp(argv[i], "--seconds") == 0 && probe_seconds == 0U && i + 1 < argc) {
            if (!parse_number(argv[++i], 1U, 300U, &probe_seconds)) {
                fputs("--seconds requires RGB 1..10, point 1..20 or stream/diagnostic 1..300\n", stderr); return 2;
            }
        }
        else { fputs("invalid arguments; use --help\n", stderr); return 2; }
    }
    unsigned int mode_count = (unsigned int)inspect + (unsigned int)check_config +
                              (unsigned int)rgb_probe + (unsigned int)point_probe + (unsigned int)stop_pointcloud +
                              (unsigned int)stream + (unsigned int)sdk_diagnostic + (unsigned int)camera_firmware +
                              (unsigned int)video_info;
    if (config == NULL || mode_count != 1U ||
        (expected_source >= 0 && (!stream || streams == NULL || strcmp(streams, "video") != 0)) ||
        (video_control && (!stream || streams == NULL || strcmp(streams, "video") != 0)) ||
        (fast_switch && !video_control) ||
        ((rgb_probe || point_probe || stop_pointcloud || stream || diagnostic_video || camera_firmware || video_info) && selected_mount == 0U) ||
        ((inspect || check_config) && selected_mount != 0U) ||
        (diagnostic_video && !sdk_diagnostic) ||
        (sdk_diagnostic && (probe_seconds == 0U || (!diagnostic_video && selected_mount != 0U))) ||
        (streams != NULL && (!stream || (strcmp(streams, "points") != 0 &&
                                         strcmp(streams, "both") != 0 &&
                                         strcmp(streams, "video") != 0))) ||
        (rgb_probe && (probe_seconds == 0U || probe_seconds > 10U)) ||
        (point_probe && (probe_seconds == 0U || probe_seconds > 20U || tcp_host == NULL || tcp_port == 0U)) ||
        (stream && (probe_seconds == 0U || point_port == video_port)) ||
        (!(rgb_probe || point_probe || stream || sdk_diagnostic) && probe_seconds != 0U) ||
        (!point_probe && (tcp_host != NULL || tcp_port != 0U)) ||
        ((point_probe || stop_pointcloud || stream) != exclusive_test)) {
        fputs("invalid mode/parameters; use --help; recording modes require --exclusive-test\n", stderr);
        return 2;
    }
    bool stream_records = stream && (streams == NULL ||
                                     strcmp(streams, "points") == 0 ||
                                     strcmp(streams, "both") == 0);
    if (point_probe || stream) {
        struct in_addr address;
        if (inet_pton(AF_INET, stream ? bind_ip : tcp_host, &address) != 1) {
            fputs("--bind/--tcp-host requires an explicit numeric IPv4 address\n", stderr); return 2;
        }
        if (point_probe || stream_records) {
#if !L3_ENABLE_POINT_RECORDING_PROBE
            printf("event=%s phase=blocked reason=psdk_mop_memory_error_not_resolved\n", stream ? "stream" : "point_probe");
            return 2;
#endif
        }
    }
    if (read_private_config(config, &info) != 0) return 2;
    if (check_config) {
        erase(&info, sizeof(info));
        puts("event=config valid=yes sdk_initialized=no");
        return 0;
    }
    if (prepare_private_runtime() != 0) goto done;
    if (point_probe) {
        point_sink = connect_point_sink(tcp_host, tcp_port);
        if (point_sink == NULL || stop_requested) goto done;
    }
    if (prepare_environment() != 0 || stop_requested) goto done;
    puts("event=phase_begin phase=core_init blocking=yes");
    T_DjiReturnCode rc = DjiCore_Init(&info);
    /* Keep userInfo alive until DeInit: the closed binary's retention is unknown. */
    if (!sdk_result("core_init", rc)) goto done;
    core_started = true;
    if (stop_requested) goto cleanup;
    memset(&aircraft, 0, sizeof(aircraft));
    puts("event=phase_begin phase=aircraft_info blocking=yes");
    if (!sdk_result("aircraft_info", DjiAircraftInfo_GetBaseInfo(&aircraft))) goto cleanup;
    if (aircraft.aircraftType != DJI_AIRCRAFT_TYPE_M400) {
        puts("event=aircraft model=M400_not_confirmed camera_queries=skipped");
        goto cleanup;
    }
    puts("event=aircraft model=M400");
    if (!stop_requested) {
        T_DjiAircraftVersion version;
        memset(&version, 0, sizeof(version));
        puts("event=phase_begin phase=aircraft_version blocking=yes");
        if (sdk_result("aircraft_version", DjiAircraftInfo_GetAircraftVersion(&version))) {
            printf("event=aircraft_version major=%u minor=%u modify=%u debug=%u\n",
                   (unsigned int)version.majorVersion, (unsigned int)version.minorVersion,
                   (unsigned int)version.modifyVersion, (unsigned int)version.debugVersion);
        }
    }
    if (stop_requested) goto cleanup;
    if (sdk_diagnostic && !diagnostic_video) {
        puts("event=phase_begin phase=application_start blocking=yes");
        if (sdk_result("application_start", DjiCore_ApplicationStart()))
            status = run_sdk_diagnostic((E_DjiMountPosition)0, probe_seconds, false);
        goto cleanup;
    }
    puts("event=phase_begin phase=camera_manager_init blocking=yes");
    if (!sdk_result("camera_manager_init", DjiCameraManager_Init())) goto cleanup;
    camera_started = true;
    if (stop_requested) goto cleanup;
    puts("event=phase_begin phase=application_start blocking=yes");
    if (!sdk_result("application_start", DjiCore_ApplicationStart())) goto cleanup;
    for (size_t i = 0U; i < sizeof(mounts) / sizeof(mounts[0]) && !stop_requested; ++i) {
        if ((rgb_probe || point_probe || stop_pointcloud || stream || diagnostic_video || camera_firmware || video_info) &&
            (unsigned int)mounts[i] != selected_mount) continue;
        E_DjiCameraType camera_type = DJI_CAMERA_TYPE_UNKNOWN;
        printf("event=phase_begin phase=camera_type psdk_port=%u blocking=yes\n", (unsigned int)mounts[i]);
        rc = DjiCameraManager_GetCameraType(mounts[i], &camera_type);
        printf("event=camera_type psdk_port=%u camera_type=%u is_l3=%s code=0x%016" PRIx64 "\n",
               (unsigned int)mounts[i],
               rc == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS ? (unsigned int)camera_type : 0U,
               rc == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS && camera_type == DJI_CAMERA_TYPE_L3 ? "yes" : "no",
               (uint64_t)rc);
        if (rc == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            ++query_successes;
            if (camera_type == DJI_CAMERA_TYPE_L3) ++l3_count;
        }
    }
    printf("event=summary camera_query_successes=%u l3_ports_found=%u interrupted=%s\n",
           query_successes, l3_count, stop_requested ? "yes" : "no");
    status = !stop_requested && l3_count != 0U ? 0 : 1;
    if (camera_firmware && status == 0)
        status = report_camera_firmware((E_DjiMountPosition)selected_mount);
    if (video_info && status == 0)
        status = report_video_info((E_DjiMountPosition)selected_mount, "inspect");
    if (sdk_diagnostic && status == 0)
        status = run_sdk_diagnostic((E_DjiMountPosition)selected_mount, probe_seconds, diagnostic_video);
    if (rgb_probe && status == 0) status = run_rgb_probe((E_DjiMountPosition)selected_mount, probe_seconds);
    if (point_probe && status == 0) status = run_point_probe((E_DjiMountPosition)selected_mount, probe_seconds, point_sink);
    if (stream && status == 0) {
        L3StreamMode stream_mode = L3_STREAM_BOTH;
        if (streams != NULL && strcmp(streams, "points") == 0) stream_mode = L3_STREAM_POINTS;
        else if (streams != NULL && strcmp(streams, "video") == 0) stream_mode = L3_STREAM_VIDEO;
        status = run_live_stream((E_DjiMountPosition)selected_mount,
                                 probe_seconds, bind_ip, point_port, video_port, stream_mode, expected_source,
                                 video_control, fast_switch);
    }
    if (stop_pointcloud && status == 0) {
        puts("event=point_probe phase=emergency_stop_begin start_attempted=no mop_initialized=no blocking=yes");
        rc = DjiCameraManager_StopRecordPointCloud((E_DjiMountPosition)selected_mount);
        printf("event=point_probe phase=emergency_stop command_ack=%s stopcode=0x%016" PRIx64 " sensor_state_verified=no\n",
               rc == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS ? "yes" : "no", (uint64_t)rc);
        if (rc != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) puts("event=point_probe phase=operator_action manual_stop_required=yes process_exit_stops_recording=no");
        status = rc == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS ? 0 : 1;
    }
cleanup:
    /* Cleanup calls can also block; the external guardian remains necessary. */
    if (camera_started) {
        puts("event=phase_begin phase=camera_manager_deinit blocking=yes");
        if (!sdk_result("camera_manager_deinit", DjiCameraManager_DeInit())) status = 1;
    }
    if (core_started) {
        puts("event=phase_begin phase=core_deinit blocking=yes");
        if (!sdk_result("core_deinit", DjiCore_DeInit())) status = 1;
    }
done:
    if (point_sink != NULL) {
        close(point_sink->fd);
        free(point_sink);
    }
    erase(&info, sizeof(info));
    printf("event=sdk_usb_padding corrections=%" PRIu64 "\n", l3_usb_padding_corrections());
    pthread_mutex_lock(&sdk_log_mutex);
    if (sdk_log_fd >= 0) {
        fsync(sdk_log_fd);
        close(sdk_log_fd);
        sdk_log_fd = -1;
    }
    pthread_mutex_unlock(&sdk_log_mutex);
    return status;
}
