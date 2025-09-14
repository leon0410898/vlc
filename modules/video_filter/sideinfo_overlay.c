#include <string.h>
#include <inttypes.h>
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_picture.h>
#include "sideinfo_bus.h"
#include <libavcodec/avcodec.h>
#include <libavutil/motion_vector.h>
#define OPT_DRAW_SIDEINFO "draw-sideinfo"

typedef struct {
    bool draw_sideinfo;
} codec_info_t;

static void put_Y(picture_t *pic, int x, int y, uint8_t yval) {
    plane_t *Y = &pic->p[0];
    int W = pic->format.i_visible_width;
    int H = pic->format.i_visible_height;
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    Y->p_pixels[y * Y->i_pitch + x] = yval;
}

static void draw_line_Y(picture_t *pic, int x0, int y0, int x1, int y1, uint8_t yval) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (;;) {
        put_Y(pic, x0, y0, yval);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_arrow_Y(picture_t *pic, int x0, int y0, int x1, int y1, uint8_t yval) {
    if (x0 == x1 && y0 == y1) return;
    draw_line_Y(pic, x0, y0, x1, y1, yval);
    put_Y(pic, x1, y1, yval);
    put_Y(pic, x1 + (x1 > x0 ? -1 : 1), y1, yval);
    put_Y(pic, x1, y1 + (y1 > y0 ? -1 : 1), yval);
}

/* 在 Y 平面畫出矩形邊框（支援 8-bit 與 P010 10-bit） */
static inline void draw_hollow_rect_Y(picture_t *pic, int x, int y, int w, int h, uint8_t yval)
{
    plane_t *Y = &pic->p[0];
    const int W = pic->format.i_visible_width;
    const int H = pic->format.i_visible_height;
    const bool is_p010 = (pic->format.i_chroma == VLC_CODEC_P010);

    /* 參數裁剪 */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= W || y >= H || w <= 0 || h <= 0) return;
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;

    if (!is_p010) {
        /* ---- 8-bit 路徑 ---- */
        for (int j = 0; j < h; ++j) {
            uint8_t *row = Y->p_pixels + (y + j) * Y->i_pitch + x;
            if (j == 0 || j == h - 1) {
                memset(row, yval, (size_t)w);
            } else {
                row[0] = yval;
                row[w - 1] = yval;
            }
        }
    } else {
        /* ---- P010 10-bit 路徑（16-bit 對齊，資料在高 10 bits）---- */
        /* 將 8-bit y 映射到 P010：y10 = y8<<2，再左移 6 變成 MSB 對齊 => y16 = y8<<8 */
        const uint16_t y16 = (uint16_t)yval << 8;

        for (int j = 0; j < h; ++j) {
            uint16_t *row = (uint16_t *)(Y->p_pixels + (y + j) * Y->i_pitch) + x;
            if (j == 0 || j == h - 1) {
                for (int i = 0; i < w; ++i) row[i] = y16;
            } else {
                row[0]      = y16;
                row[w - 1]  = y16;
            }
        }
    }
}

static inline void draw_solid_rect_Y(picture_t *pic, int x, int y, int w, int h, uint8_t yval)
{
    plane_t *Y = &pic->p[0];
    const int W = pic->format.i_visible_width;
    const int H = pic->format.i_visible_height;

    // 將 (x,y,w,h) 規範化成 (x0,y0)~(x1,y1)
    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }

    // 與可視區域做裁切
    if (x0 >= W || y0 >= H || x1 <= 0 || y1 <= 0) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;

    const int rw = x1 - x0;
    const int rh = y1 - y0;
    if (rw <= 0 || rh <= 0) return;

    // 逐行填滿
    uint8_t *row = Y->p_pixels + y0 * Y->i_pitch + x0;
    for (int j = 0; j < rh; ++j) {
        memset(row, yval, (size_t)rw);
        row += Y->i_pitch;
    }
}

static bool is_supported_chroma(vlc_fourcc_t fcc)
{
    switch (fcc) {
        case VLC_CODEC_I420:
        case VLC_CODEC_YV12:
        case VLC_CODEC_I422:
        case VLC_CODEC_I444:
        case VLC_CODEC_NV12:
        case VLC_CODEC_NV21:
            return true;
        default:
            return false;
    }
}

/* 依 payload 畫 MV（會把 quarter/half/…pel 轉成像素） */
static inline void draw_mv_payload(picture_t *pic, const vlc_side_entry_t *mv_info, int codec_id)
{
    if (!mv_info) return;

    const AVMotionVector *src = (const AVMotionVector*)mv_info->data;
    const int mv_count = mv_info->size / (int)sizeof(*src);
    static const uint8_t Y_FWD = 220, Y_BWD = 60, Y_UNK = 128;

    const AVMotionVector *mv = src;

    for (int i = 0; i < mv_count; ++i, ++mv) {
        const int x0 = mv->dst_x + (mv->w >> 1);
        const int y0 = mv->dst_y + (mv->h >> 1);
        const int dx = mv->motion_x; /* C 的整數除法已向 0 取整 */
        const int dy = mv->motion_y;
        const int x1 = x0 + dx;
        const int y1 = y0 + dy;
        const uint8_t yv = (mv->source > 0) ? Y_FWD : (mv->source < 0) ? Y_BWD : Y_UNK;
        draw_arrow_Y(pic, x0, y0, x1, y1, yv);
    }
}

static inline void draw_qp_payload(picture_t *pic, const vlc_side_entry_t *qp_info, int codec_id)
{
    if (!qp_info) return;

    unsigned char *pY = pic->p[0].p_pixels;
    unsigned char *pU = pic->p[1].p_pixels;
    unsigned char *pV = pic->p[2].p_pixels;

    typedef struct FFQPTblHdr {
        uint32_t tag;        // 'Q''T''B''0' = 0x52534D30
        uint8_t  blk_px;     // blk unit size in pixels
    } FFQPTblHdr;

    const FFQPTblHdr *hdr = (const FFQPTblHdr*)qp_info->data;
    const  int8_t   *vals = (const int8_t*)(hdr + 1);
    const int count = (qp_info->size - sizeof(FFQPTblHdr)) / (int)sizeof(*vals);
    if (count <= 0) return;

    int blk_xy = 0, blk_px = hdr->blk_px;
    int QP = 0;

    for (int j = 0; j < pic->p[0].i_visible_lines; j += blk_px) {
        for (int i = 0; i < pic->p[0].i_visible_pitch; i += blk_px, ++blk_xy) {
            QP = 255 - (int)((float)vals[blk_xy] * 255 / 51 + 0.5);
            for (int k = 0; k < blk_px; k++)
                memset(pY + (j + k) * pic->p[0].i_pitch + i, QP, blk_px);
            }
            if (codec_id == AV_CODEC_ID_H264) ++blk_xy;
        }

        memset(pU, 128, pic->p[1].i_lines * pic->p[1].i_pitch);
        memset(pV, 128, pic->p[1].i_lines * pic->p[1].i_pitch);
    
}

static inline void draw_blk_info_payload(picture_t *pic, const vlc_side_entry_t *blk_type, int codec_id)
{
/* MB types */

#define IS_INTRA(a)      ((a) & 7)
#define IS_INTER(a)      ((a) & 0x0078)
#define IS_SKIP(a)       ((a) & 0x0800)

    if (!blk_type) return;

    unsigned char *pU = pic->p[1].p_pixels;
    unsigned char *pV = pic->p[2].p_pixels;

    const uint32_t *blk_table = (const uint32_t*)blk_type->data;
    const int count = blk_type->size / (int)sizeof(*blk_table);
    if (count <= 0) return;

    unsigned int blk_idx = 0;

    for (int j = 0; j < pic->p[1].i_visible_lines; j += 8) {
        for (int i = 0; i < pic->p[1].i_visible_pitch; i += 8, ++blk_idx) {
            uint32_t mb_type = blk_table[blk_idx];
            if (IS_INTRA(mb_type)) {
                for (int k = 0; k < 8; k++) {
                    memset(pU + (j + k) * pic->p[1].i_pitch + i, 84, 8);
                    memset(pV + (j + k) * pic->p[2].i_pitch + i, 255, 8);
                }
            }
            else if (!IS_SKIP(mb_type)) {
                for (int k = 0; k < 8; k++) {
                    memset(pU + (j + k) * pic->p[1].i_pitch + i, 255, 8);
                    memset(pV + (j + k) * pic->p[2].i_pitch + i, 128, 8);
                }
            }
        }
        ++blk_idx; //skip last MB in row, due to ffmpeg pad one zero MB in the end of MB row
    }
}

static inline void draw_res_size_payload(picture_t *pic, const vlc_side_entry_t *res_info, int codec_id)
{
    if (!res_info) return;

    typedef struct FFResidualHdr {
        uint32_t tag;
        uint16_t mb_w, mb_h;
        uint8_t  unit;
        uint8_t  block_px;
    } FFResidualHdr;

    const FFResidualHdr *hdr = (const FFResidualHdr*)res_info->data;
    int mb_w = hdr->mb_w, mb_h = hdr->mb_h;
    const uint16_t *vals = (const uint16_t*)(hdr + 1);
    const int count = (res_info->size - sizeof(FFResidualHdr))/ (int)sizeof(*vals);
    if (count <= 0) return;

    const int B = hdr->block_px; // 16
    const int W = pic->format.i_visible_width;
    const int H = pic->format.i_visible_height;
    const int bw = hdr->mb_w;
    const int bh = hdr->mb_h;

    // 找 max 做線性映射
    uint16_t vmax = 1;
    int total = bw * bh;
    unsigned int blk_idx = 0;

    for (int i = 0; i < total; ++i)
        if (vals[i] > vmax) vmax = vals[i];

    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx, ++blk_idx) {
            uint16_t v = vals[blk_idx];
            // 線性映射到 16..235，避免太黑/太白
            uint8_t y = 16 + (uint8_t)((219u * v) / vmax);

            int x = bx * B;
            int ypix = by * B;
            int w = (x + B <= W) ? B : W - x;
            int h = (ypix + B <= H) ? B : H - ypix;

            draw_solid_rect_Y(pic, x, ypix, w, h, y);
        }
        ++blk_idx;
    }
    // clean chroma
    memset(pic->p[1].p_pixels, 128, pic->p[1].i_lines * pic->p[1].i_pitch);
    memset(pic->p[2].p_pixels, 128, pic->p[2].i_lines * pic->p[2].i_pitch);
}
/* 濾鏡框架 */
static picture_t *Filter(filter_t *f, picture_t *pic)
{
    if (!is_supported_chroma(pic->format.i_chroma))
        return pic;

    codec_info_t *sys = f->p_sys;
    if (!sys || !sys->draw_sideinfo)
        return pic;

    sidebus_t *bus = sidebus_acquire(f->obj.libvlc);
    const vlc_tick_t pts = pic->i_pts;
    if (!bus || pts == VLC_TICK_INVALID)
        return pic;

    vlc_side_packet_t pkt = {0};

    if (!sidebus_pop(bus, &pkt, pts))
        return pic;

    for (vlc_side_entry_t *e = pkt.head; e; e = e->next) {
        switch (e->type) {
            case AV_FRAME_DATA_MOTION_VECTORS:
                draw_mv_payload(pic, e, pkt.codec_id);
                break;
            case AV_FRAME_DATA_QP_TABLE_DATA:
                draw_qp_payload(pic, e, pkt.codec_id);
                break;
            case AV_FRAME_DATA_BLK_TYPE:
                draw_blk_info_payload(pic, e, pkt.codec_id);
                break;
            case AV_FRAME_DATA_RES_SIZE:
                draw_res_size_payload(pic, e, pkt.codec_id);
                break;
            default:
                msg_Warn(f, "unsupported sideinfo type: 0x%08" PRIx32, e->type);
                break;
        }
    }

    sidebus_free_entry(pkt.head); /* 我們拿到的是 deep copy，畫完就釋放 */

    return pic;
}

static int Open(vlc_object_t *obj) {
    filter_t *f = (filter_t *)obj;
    f->fmt_out = f->fmt_in;
    f->b_allow_fmt_out_change = false;

    codec_info_t *sys = calloc(1, sizeof(*sys));
    if (!sys) return VLC_ENOMEM;
    f->p_sys = sys;

    sys->draw_sideinfo  = var_InheritBool(f, OPT_DRAW_SIDEINFO);

    f->pf_video_filter = Filter;

    msg_Info(f, "sideinfo_overlay enabled (draw_sideinfo=%d)", sys->draw_sideinfo);
    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj) {
    filter_t *f = (filter_t *)obj;
    free(f->p_sys);
}

vlc_module_begin()
    set_description("MV overlay (playback only)")
    set_shortname("MV Overlay")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_capability("video filter", 0)
    add_shortcut("sideinfo_overlay")
    set_callbacks(Open, Close)

    add_bool(OPT_DRAW_SIDEINFO, true, "Draw codec sideinfo", "Overlay codec sideinfo", false)
vlc_module_end()
