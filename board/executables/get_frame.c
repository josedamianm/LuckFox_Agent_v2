/*
 * get_frame.c - Capture a single JPEG frame from the Luckfox Pico Max camera
 *
 * Usage: ./get_frame
 * Output: /tmp/frame.jpg
 *
 * Sensor: SC3336 @ 2304x1296 (native resolution)
 * Uses: VI (capture) → VENC (JPEG hardware encode) → file
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
#define CAM_WIDTH       2304
#define CAM_HEIGHT      1296
#define OUTPUT_FILE     "/tmp/frame.jpg"
#define IQ_FILE_DIR     "/etc/iqfiles"
#define VI_PIPE         0
#define VI_CHN          0
#define VENC_CHN        0

/* ---- RKAIQ (ISP/3A) ---- */
static rk_aiq_sys_ctx_t *g_aiq_ctx = NULL;

static XCamReturn isp_error_cb(rk_aiq_err_msg_t *msg) {
    (void)msg;
    return XCAM_RETURN_NO_ERROR;
}

static XCamReturn isp_sof_cb(rk_aiq_metas_t *meta) {
    (void)meta;
    return XCAM_RETURN_NO_ERROR;
}

static int isp_init(void) {
    rk_aiq_static_info_t info;
    rk_aiq_uapi2_sysctl_enumStaticMetas(0, &info);
    printf("Sensor: %s\n", info.sensor_info.sensor_name);

    setenv("HDR_MODE", "0", 1);
    g_aiq_ctx = rk_aiq_uapi2_sysctl_init(info.sensor_info.sensor_name,
                                           IQ_FILE_DIR, isp_error_cb, isp_sof_cb);
    if (!g_aiq_ctx) {
        fprintf(stderr, "ERROR: rkaiq init failed\n");
        return -1;
    }

    if (rk_aiq_uapi2_sysctl_prepare(g_aiq_ctx, 0, 0, RK_AIQ_WORKING_MODE_NORMAL)) {
        fprintf(stderr, "ERROR: rkaiq prepare failed\n");
        return -1;
    }

    if (rk_aiq_uapi2_sysctl_start(g_aiq_ctx)) {
        fprintf(stderr, "ERROR: rkaiq start failed\n");
        return -1;
    }

    printf("ISP/3A started\n");
    return 0;
}

static void isp_deinit(void) {
    if (g_aiq_ctx) {
        rk_aiq_uapi2_sysctl_stop(g_aiq_ctx, false);
        rk_aiq_uapi2_sysctl_deinit(g_aiq_ctx);
        g_aiq_ctx = NULL;
    }
}

/* ---- VI (Video Input) ---- */
static int vi_init(void) {
    int ret;
    VI_DEV_ATTR_S dev_attr;
    VI_DEV_BIND_PIPE_S bind_pipe;
    VI_CHN_ATTR_S chn_attr;

    memset(&dev_attr, 0, sizeof(dev_attr));
    memset(&bind_pipe, 0, sizeof(bind_pipe));
    memset(&chn_attr, 0, sizeof(chn_attr));

    /* Dev */
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

    /* Channel */
    chn_attr.stIspOpt.u32BufCount = 2;
    chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    chn_attr.stSize.u32Width = CAM_WIDTH;
    chn_attr.stSize.u32Height = CAM_HEIGHT;
    chn_attr.enPixelFormat = RK_FMT_YUV420SP;
    chn_attr.enCompressMode = COMPRESS_MODE_NONE;
    chn_attr.u32Depth = 2;

    ret = RK_MPI_VI_SetChnAttr(VI_PIPE, VI_CHN, &chn_attr);
    ret |= RK_MPI_VI_EnableChn(VI_PIPE, VI_CHN);
    if (ret) { fprintf(stderr, "VI channel init failed: %x\n", ret); return -1; }

    printf("VI initialized (%dx%d)\n", CAM_WIDTH, CAM_HEIGHT);
    return 0;
}

static void vi_deinit(void) {
    RK_MPI_VI_DisableChn(VI_PIPE, VI_CHN);
    RK_MPI_VI_DisableDev(0);
}

/* ---- VENC (JPEG Hardware Encoder) ---- */
static int venc_init(void) {
    int ret;
    VENC_CHN_ATTR_S venc_attr;

    memset(&venc_attr, 0, sizeof(venc_attr));
    venc_attr.stVencAttr.enType = RK_VIDEO_ID_JPEG;
    venc_attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    venc_attr.stVencAttr.u32PicWidth = CAM_WIDTH;
    venc_attr.stVencAttr.u32PicHeight = CAM_HEIGHT;
    venc_attr.stVencAttr.u32VirWidth = CAM_WIDTH;
    venc_attr.stVencAttr.u32VirHeight = CAM_HEIGHT;
    venc_attr.stVencAttr.u32StreamBufCnt = 2;
    venc_attr.stVencAttr.u32BufSize = CAM_WIDTH * CAM_HEIGHT;

    /* JPEG quality */
    venc_attr.stVencAttr.stAttrJpege.bSupportDCF = RK_FALSE;
    venc_attr.stVencAttr.stAttrJpege.stMPFCfg.u8LargeThumbNailNum = 0;

    ret = RK_MPI_VENC_CreateChn(VENC_CHN, &venc_attr);
    if (ret) { fprintf(stderr, "VENC CreateChn failed: %x\n", ret); return -1; }

    /* Set JPEG quality (1-99, higher = better) */
    VENC_JPEG_PARAM_S jpeg_param;
    memset(&jpeg_param, 0, sizeof(jpeg_param));
    jpeg_param.u32Qfactor = 95;
    RK_MPI_VENC_SetJpegParam(VENC_CHN, &jpeg_param);

    printf("VENC JPEG encoder initialized (quality=95)\n");
    return 0;
}

static void venc_deinit(void) {
    RK_MPI_VENC_StopRecvFrame(VENC_CHN);
    RK_MPI_VENC_DestroyChn(VENC_CHN);
}

/* ---- Main: Capture + Encode + Save ---- */
int main(int argc, char *argv[]) {
    int ret;
    const char *output_file = OUTPUT_FILE;

    /* Allow optional output path: ./get_frame [output.jpg] */
    if (argc > 1)
        output_file = argv[1];

    printf("=== Luckfox Pico Max - JPEG Frame Capture ===\n");
    printf("Output: %s\n\n", output_file);

    /* 1. Init system */
    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        fprintf(stderr, "ERROR: RK_MPI_SYS_Init failed\n");
        return -1;
    }

    /* 2. Init ISP/3A */
    if (isp_init() != 0) goto cleanup_sys;

    /* 3. Init VI */
    if (vi_init() != 0) goto cleanup_isp;

    /* 4. Init VENC (JPEG) */
    if (venc_init() != 0) goto cleanup_vi;

    /* 5. Start VENC receiving frames */
    VENC_RECV_PIC_PARAM_S recv_param;
    memset(&recv_param, 0, sizeof(recv_param));
    recv_param.s32RecvPicNum = 1;  /* Only encode 1 frame */
    ret = RK_MPI_VENC_StartRecvFrame(VENC_CHN, &recv_param);
    if (ret) {
        fprintf(stderr, "VENC StartRecvFrame failed: %x\n", ret);
        goto cleanup_venc;
    }

    /* 6. Let ISP stabilize (auto-exposure/AWB) — skip a few frames */
    printf("Waiting for ISP to stabilize...\n");
    for (int i = 0; i < 5; i++) {
        VIDEO_FRAME_INFO_S frame;
        ret = RK_MPI_VI_GetChnFrame(VI_PIPE, VI_CHN, &frame, 1000);
        if (ret == RK_SUCCESS) {
            RK_MPI_VI_ReleaseChnFrame(VI_PIPE, VI_CHN, &frame);
        }
    }
    printf("ISP stabilized\n");

    /* 7. Grab a frame from VI */
    VIDEO_FRAME_INFO_S vi_frame;
    ret = RK_MPI_VI_GetChnFrame(VI_PIPE, VI_CHN, &vi_frame, 2000);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "ERROR: VI GetChnFrame failed: %x\n", ret);
        goto cleanup_venc;
    }
    printf("Captured frame: %dx%d pts=%llu\n",
           vi_frame.stVFrame.u32Width, vi_frame.stVFrame.u32Height,
           vi_frame.stVFrame.u64PTS);

    /* 8. Send frame to VENC for JPEG encoding */
    ret = RK_MPI_VENC_SendFrame(VENC_CHN, &vi_frame, 2000);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "ERROR: VENC SendFrame failed: %x\n", ret);
        RK_MPI_VI_ReleaseChnFrame(VI_PIPE, VI_CHN, &vi_frame);
        goto cleanup_venc;
    }

    /* Release VI frame (VENC has its own copy) */
    RK_MPI_VI_ReleaseChnFrame(VI_PIPE, VI_CHN, &vi_frame);

    /* 9. Get encoded JPEG stream from VENC */
    VENC_STREAM_S venc_stream;
    memset(&venc_stream, 0, sizeof(venc_stream));
    venc_stream.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
    if (!venc_stream.pstPack) {
        fprintf(stderr, "ERROR: malloc failed\n");
        goto cleanup_venc;
    }

    ret = RK_MPI_VENC_GetStream(VENC_CHN, &venc_stream, 2000);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "ERROR: VENC GetStream failed: %x\n", ret);
        free(venc_stream.pstPack);
        goto cleanup_venc;
    }

    /* 10. Save JPEG to file */
    void *jpeg_data = RK_MPI_MB_Handle2VirAddr(venc_stream.pstPack->pMbBlk);
    RK_U32 jpeg_size = venc_stream.pstPack->u32Len;

    FILE *fp = fopen(output_file, "wb");
    if (fp) {
        fwrite(jpeg_data, 1, jpeg_size, fp);
        fclose(fp);
        printf("\n✓ Saved %s (%u bytes, %.1f KB)\n", output_file, jpeg_size,
               jpeg_size / 1024.0f);
    } else {
        fprintf(stderr, "ERROR: Cannot open %s for writing\n", output_file);
    }

    /* Release VENC stream */
    RK_MPI_VENC_ReleaseStream(VENC_CHN, &venc_stream);
    free(venc_stream.pstPack);

    /* ---- Cleanup ---- */
cleanup_venc:
    venc_deinit();
cleanup_vi:
    vi_deinit();
cleanup_isp:
    isp_deinit();
cleanup_sys:
    RK_MPI_SYS_Exit();

    printf("Done.\n");
    return 0;
}
