/*
 * camera_daemon.c - Persistent camera daemon for Luckfox Pico Max
 *
 * Inits ISP/3A + VI + VENC once at startup, then continuously captures
 * JPEG frames. Provides:
 *   - /tmp/frame_latest.jpg  : latest frame (atomic rename, <100ms to read)
 *   - TCP port 8554          : MJPEG stream (HTTP multipart/x-mixed-replace)
 *
 * Usage: ./camera_daemon [fps]   (default fps=10)
 * PID:   /tmp/camera_daemon.pid
 *
 * Sensor: SC3336 @ 2304x1296
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <rk_aiq_user_api2_camgroup.h>
#include <rk_aiq_user_api2_imgproc.h>
#include <rk_aiq_user_api2_sysctl.h>

#include "rk_debug.h"
#include "rk_defines.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_vpss.h"

/* ---- Configuration ---- */
#define CAM_WIDTH        2304
#define CAM_HEIGHT       1296
#define IQ_FILE_DIR      "/etc/iqfiles"
#define VI_PIPE          0
#define VI_CHN           0
#define VENC_CHN         0
#define MJPEG_PORT       8554
#define PID_FILE         "/tmp/camera_daemon.pid"
#define FRAME_LATEST     "/tmp/frame_latest.jpg"
#define FRAME_TMP        "/tmp/frame_tmp.jpg"
#define MAX_MJPEG_CLIENTS 4

/* ---- Global state ---- */
static volatile sig_atomic_t g_running = 1;
static rk_aiq_sys_ctx_t     *g_aiq_ctx = NULL;

/* Shared JPEG frame buffer for MJPEG clients */
typedef struct {
    pthread_mutex_t  lock;
    pthread_cond_t   cond;
    unsigned char   *data;
    size_t           size;
    unsigned long    seq;
} FrameBuffer;

static FrameBuffer g_fb = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .data = NULL,
    .size = 0,
    .seq  = 0
};

/* ---- Signal handler ---- */
static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ---- RKAIQ (ISP/3A) ---- */
static XCamReturn isp_error_cb(rk_aiq_err_msg_t *msg) { (void)msg; return XCAM_RETURN_NO_ERROR; }
static XCamReturn isp_sof_cb(rk_aiq_metas_t *meta)    { (void)meta; return XCAM_RETURN_NO_ERROR; }

static int isp_init(void) {
    rk_aiq_static_info_t info;
    rk_aiq_uapi2_sysctl_enumStaticMetas(0, &info);
    printf("[camera_daemon] Sensor: %s\n", info.sensor_info.sensor_name);

    setenv("HDR_MODE", "0", 1);
    g_aiq_ctx = rk_aiq_uapi2_sysctl_init(info.sensor_info.sensor_name,
                                           IQ_FILE_DIR, isp_error_cb, isp_sof_cb);
    if (!g_aiq_ctx) { fprintf(stderr, "ERROR: rkaiq init failed\n"); return -1; }

    if (rk_aiq_uapi2_sysctl_prepare(g_aiq_ctx, 0, 0, RK_AIQ_WORKING_MODE_NORMAL)) {
        fprintf(stderr, "ERROR: rkaiq prepare failed\n"); return -1;
    }
    if (rk_aiq_uapi2_sysctl_start(g_aiq_ctx)) {
        fprintf(stderr, "ERROR: rkaiq start failed\n"); return -1;
    }
    printf("[camera_daemon] ISP/3A started\n");
    return 0;
}

static void isp_deinit(void) {
    if (g_aiq_ctx) {
        rk_aiq_uapi2_sysctl_stop(g_aiq_ctx, false);
        rk_aiq_uapi2_sysctl_deinit(g_aiq_ctx);
        g_aiq_ctx = NULL;
    }
}

/* ---- VI ---- */
static int vi_init(void) {
    int ret;
    VI_DEV_ATTR_S dev_attr;
    VI_DEV_BIND_PIPE_S bind_pipe;
    VI_CHN_ATTR_S chn_attr;

    memset(&dev_attr,   0, sizeof(dev_attr));
    memset(&bind_pipe,  0, sizeof(bind_pipe));
    memset(&chn_attr,   0, sizeof(chn_attr));

    ret = RK_MPI_VI_GetDevAttr(0, &dev_attr);
    if (ret == RK_ERR_VI_NOT_CONFIG) {
        ret = RK_MPI_VI_SetDevAttr(0, &dev_attr);
        if (ret) { fprintf(stderr, "VI SetDevAttr failed: %x\n", ret); return -1; }
    }

    ret = RK_MPI_VI_GetDevIsEnable(0);
    if (ret != RK_SUCCESS) {
        ret = RK_MPI_VI_EnableDev(0);
        if (ret) { fprintf(stderr, "VI EnableDev failed: %x\n", ret); return -1; }
        bind_pipe.u32Num = 1;
        bind_pipe.PipeId[0] = VI_PIPE;
        ret = RK_MPI_VI_SetDevBindPipe(0, &bind_pipe);
        if (ret) { fprintf(stderr, "VI BindPipe failed: %x\n", ret); return -1; }
    }

    chn_attr.stIspOpt.u32BufCount  = 3;
    chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    chn_attr.stSize.u32Width       = CAM_WIDTH;
    chn_attr.stSize.u32Height      = CAM_HEIGHT;
    chn_attr.enPixelFormat         = RK_FMT_YUV420SP;
    chn_attr.enCompressMode        = COMPRESS_MODE_NONE;
    chn_attr.u32Depth              = 2;

    ret  = RK_MPI_VI_SetChnAttr(VI_PIPE, VI_CHN, &chn_attr);
    ret |= RK_MPI_VI_EnableChn(VI_PIPE, VI_CHN);
    if (ret) { fprintf(stderr, "VI channel init failed: %x\n", ret); return -1; }

    printf("[camera_daemon] VI initialized (%dx%d)\n", CAM_WIDTH, CAM_HEIGHT);
    return 0;
}

static void vi_deinit(void) {
    RK_MPI_VI_DisableChn(VI_PIPE, VI_CHN);
    RK_MPI_VI_DisableDev(0);
}

/* ---- VENC (JPEG continuous) ---- */
static int venc_init(void) {
    int ret;
    VENC_CHN_ATTR_S venc_attr;
    memset(&venc_attr, 0, sizeof(venc_attr));

    venc_attr.stVencAttr.enType          = RK_VIDEO_ID_JPEG;
    venc_attr.stVencAttr.enPixelFormat   = RK_FMT_YUV420SP;
    venc_attr.stVencAttr.u32PicWidth     = CAM_WIDTH;
    venc_attr.stVencAttr.u32PicHeight    = CAM_HEIGHT;
    venc_attr.stVencAttr.u32VirWidth     = CAM_WIDTH;
    venc_attr.stVencAttr.u32VirHeight    = CAM_HEIGHT;
    venc_attr.stVencAttr.u32StreamBufCnt = 2;
    venc_attr.stVencAttr.u32BufSize      = CAM_WIDTH * CAM_HEIGHT;
    venc_attr.stVencAttr.stAttrJpege.bSupportDCF = RK_FALSE;
    venc_attr.stVencAttr.stAttrJpege.stMPFCfg.u8LargeThumbNailNum = 0;

    ret = RK_MPI_VENC_CreateChn(VENC_CHN, &venc_attr);
    if (ret) { fprintf(stderr, "VENC CreateChn failed: %x\n", ret); return -1; }

    VENC_JPEG_PARAM_S jpeg_param;
    memset(&jpeg_param, 0, sizeof(jpeg_param));
    jpeg_param.u32Qfactor = 90;
    RK_MPI_VENC_SetJpegParam(VENC_CHN, &jpeg_param);

    /* Continuous mode: receive frames indefinitely */
    VENC_RECV_PIC_PARAM_S recv_param;
    memset(&recv_param, 0, sizeof(recv_param));
    recv_param.s32RecvPicNum = -1;
    ret = RK_MPI_VENC_StartRecvFrame(VENC_CHN, &recv_param);
    if (ret) { fprintf(stderr, "VENC StartRecvFrame failed: %x\n", ret); return -1; }

    printf("[camera_daemon] VENC JPEG encoder ready (continuous, quality=90)\n");
    return 0;
}

static void venc_deinit(void) {
    RK_MPI_VENC_StopRecvFrame(VENC_CHN);
    RK_MPI_VENC_DestroyChn(VENC_CHN);
}

/* ---- Capture one frame, update shared buffer + write file ---- */
static int capture_one_frame(void) {
    int ret;
    VIDEO_FRAME_INFO_S vi_frame;

    ret = RK_MPI_VI_GetChnFrame(VI_PIPE, VI_CHN, &vi_frame, 1000);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "[camera_daemon] VI GetChnFrame failed: %x\n", ret);
        return -1;
    }

    ret = RK_MPI_VENC_SendFrame(VENC_CHN, &vi_frame, 1000);
    RK_MPI_VI_ReleaseChnFrame(VI_PIPE, VI_CHN, &vi_frame);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "[camera_daemon] VENC SendFrame failed: %x\n", ret);
        return -1;
    }

    VENC_STREAM_S venc_stream;
    memset(&venc_stream, 0, sizeof(venc_stream));
    venc_stream.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
    if (!venc_stream.pstPack) return -1;

    ret = RK_MPI_VENC_GetStream(VENC_CHN, &venc_stream, 2000);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "[camera_daemon] VENC GetStream failed: %x\n", ret);
        free(venc_stream.pstPack);
        return -1;
    }

    void   *jpeg_data = RK_MPI_MB_Handle2VirAddr(venc_stream.pstPack->pMbBlk);
    RK_U32  jpeg_size = venc_stream.pstPack->u32Len;

    /* Write atomically to /tmp/frame_latest.jpg */
    FILE *fp = fopen(FRAME_TMP, "wb");
    if (fp) {
        fwrite(jpeg_data, 1, jpeg_size, fp);
        fclose(fp);
        rename(FRAME_TMP, FRAME_LATEST);
    }

    /* Update shared MJPEG buffer */
    pthread_mutex_lock(&g_fb.lock);
    unsigned char *buf = (unsigned char *)realloc(g_fb.data, jpeg_size);
    if (buf) {
        memcpy(buf, jpeg_data, jpeg_size);
        g_fb.data = buf;
        g_fb.size = jpeg_size;
        g_fb.seq++;
    }
    pthread_cond_broadcast(&g_fb.cond);
    pthread_mutex_unlock(&g_fb.lock);

    RK_MPI_VENC_ReleaseStream(VENC_CHN, &venc_stream);
    free(venc_stream.pstPack);
    return 0;
}

/* ---- MJPEG client handler ---- */
static void *mjpeg_client_thread(void *arg) {
    int fd = *(int *)arg;
    free(arg);

    /* Read and discard the HTTP request */
    char buf[1024];
    int n = recv(fd, buf, sizeof(buf) - 1, 0);
    (void)n;

    /* Send HTTP multipart header */
    const char *hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=--luckfoxframe\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";
    send(fd, hdr, strlen(hdr), MSG_NOSIGNAL);

    unsigned long last_seq = 0;
    unsigned char *local_buf = NULL;
    size_t local_cap = 0;

    while (g_running) {
        pthread_mutex_lock(&g_fb.lock);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;
        while (g_fb.seq == last_seq && g_running) {
            if (pthread_cond_timedwait(&g_fb.cond, &g_fb.lock, &ts) == ETIMEDOUT)
                break;
        }
        if (!g_running || g_fb.size == 0) {
            pthread_mutex_unlock(&g_fb.lock);
            break;
        }
        if (g_fb.size > local_cap) {
            unsigned char *tmp = (unsigned char *)realloc(local_buf, g_fb.size);
            if (!tmp) { pthread_mutex_unlock(&g_fb.lock); break; }
            local_buf = tmp;
            local_cap = g_fb.size;
        }
        memcpy(local_buf, g_fb.data, g_fb.size);
        size_t frame_size = g_fb.size;
        last_seq = g_fb.seq;
        pthread_mutex_unlock(&g_fb.lock);

        char part_hdr[256];
        int hlen = snprintf(part_hdr, sizeof(part_hdr),
            "--luckfoxframe\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %zu\r\n"
            "\r\n", frame_size);

        if (send(fd, part_hdr, hlen, MSG_NOSIGNAL) < 0) break;
        if (send(fd, local_buf, frame_size, MSG_NOSIGNAL) < 0) break;
        if (send(fd, "\r\n", 2, MSG_NOSIGNAL) < 0) break;
    }

    free(local_buf);
    close(fd);
    return NULL;
}

/* ---- MJPEG server thread ---- */
static void *mjpeg_server_thread(void *arg) {
    (void)arg;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return NULL; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(MJPEG_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(server_fd); return NULL;
    }
    listen(server_fd, MAX_MJPEG_CLIENTS);
    printf("[camera_daemon] MJPEG server listening on port %d\n", MJPEG_PORT);

    /* Make accept() non-blocking so we can check g_running */
    fcntl(server_fd, F_SETFL, O_NONBLOCK);

    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(50000);
                continue;
            }
            break;
        }
        printf("[camera_daemon] MJPEG client connected: %s\n",
               inet_ntoa(client_addr.sin_addr));

        int *pfd = (int *)malloc(sizeof(int));
        if (!pfd) { close(client_fd); continue; }
        *pfd = client_fd;

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, mjpeg_client_thread, pfd);
        pthread_attr_destroy(&attr);
    }

    close(server_fd);
    return NULL;
}

/* ---- Main ---- */
int main(int argc, char *argv[]) {
    int target_fps = 10;
    if (argc > 1) target_fps = atoi(argv[1]);
    if (target_fps < 1)  target_fps = 1;
    if (target_fps > 30) target_fps = 30;

    long frame_interval_us = 1000000L / target_fps;

    printf("[camera_daemon] Starting (target=%d fps, interval=%ldms)\n",
           target_fps, frame_interval_us / 1000);

    /* Write PID file */
    FILE *pf = fopen(PID_FILE, "w");
    if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Init MPI system */
    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        fprintf(stderr, "ERROR: RK_MPI_SYS_Init failed\n");
        return 1;
    }

    if (isp_init()  != 0) goto cleanup_sys;
    if (vi_init()   != 0) goto cleanup_isp;
    if (venc_init() != 0) goto cleanup_vi;

    /* Warm up ISP: discard first N frames */
    printf("[camera_daemon] Warming up ISP (5 frames)...\n");
    for (int i = 0; i < 5; i++) {
        VIDEO_FRAME_INFO_S frame;
        if (RK_MPI_VI_GetChnFrame(VI_PIPE, VI_CHN, &frame, 1000) == RK_SUCCESS)
            RK_MPI_VI_ReleaseChnFrame(VI_PIPE, VI_CHN, &frame);
    }
    printf("[camera_daemon] ISP ready. Starting capture loop.\n");

    /* Start MJPEG server thread */
    pthread_t mjpeg_tid;
    pthread_create(&mjpeg_tid, NULL, mjpeg_server_thread, NULL);

    /* Capture loop */
    unsigned long frame_count = 0;
    struct timespec t_start, t_end;

    while (g_running) {
        clock_gettime(CLOCK_MONOTONIC, &t_start);

        if (capture_one_frame() == 0) {
            frame_count++;
            if (frame_count % 100 == 0)
                printf("[camera_daemon] %lu frames captured\n", frame_count);
        }

        clock_gettime(CLOCK_MONOTONIC, &t_end);
        long elapsed_us = (t_end.tv_sec - t_start.tv_sec) * 1000000L
                        + (t_end.tv_nsec - t_start.tv_nsec) / 1000L;
        long sleep_us = frame_interval_us - elapsed_us;
        if (sleep_us > 0)
            usleep((useconds_t)sleep_us);
    }

    printf("[camera_daemon] Shutting down after %lu frames...\n", frame_count);

    pthread_join(mjpeg_tid, NULL);

    venc_deinit();
cleanup_vi:
    vi_deinit();
cleanup_isp:
    isp_deinit();
cleanup_sys:
    RK_MPI_SYS_Exit();

    free(g_fb.data);
    unlink(PID_FILE);
    printf("[camera_daemon] Done.\n");
    return 0;
}
