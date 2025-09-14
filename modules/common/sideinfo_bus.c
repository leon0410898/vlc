#include "sideinfo_bus.h"
#include <vlc_variables.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>  /* for PRId64 */
#define SIDEINFO_BUS_VAR   "sideinfo-bus"               /* stored on libvlc root */
#define SIDEINFO_BUS_CAP   256
#define SIDEINFO_PTS_TOL   (8 * CLOCK_FREQ / 1000)  /* 8ms */

/* 與一幀對應的 side list（以 PTS 索引） */
typedef struct sidebus {
    vlc_mutex_t lock;
    vlc_side_packet_t ring[SIDEINFO_BUS_CAP];
    size_t      head, tail;          /* ring buffer */
    size_t      size;
} sidebus_t;

static void sidebus_free_entry_list(vlc_side_entry_t *head) {
    while (head) {
        vlc_side_entry_t *n = head->next;
        free(head->data);
        free(head);
        head = n;
    }
}

static vlc_side_entry_t *deep_clone_entry_list(const vlc_side_entry_t *src) {
    vlc_side_entry_t *head = NULL, **tail = &head;
    for ( ; src; src = src->next) {
        vlc_side_entry_t *e = malloc(sizeof(*e));
        if (!e) { sidebus_free_entry_list(head); return NULL; }
        e->type  = src->type;
        e->flags = src->flags;
        e->size  = src->size;
        e->data  = NULL;
        e->next  = NULL;
        if (e->size) {
            e->data = malloc(e->size);
            if (!e->data) { free(e); sidebus_free_entry_list(head); return NULL; }
            memcpy(e->data, src->data, e->size);
        }
        *tail = e; tail = &e->next;
    }
    return head;
}

/*
設計目的:
1. 單例（per-libvlc instance）：用變數系統把 bus 綁在 root 上，等於在整個播放器實例內共享同一套 ring buffer。
2. 延遲初始化：只有真的有人需要時才配置記憶體。
3. 生命週期簡單：你後面 mvqp_bus_release() 不會 free，等程序結束由 OS 回收，對 VLC 外掛來說很常見也安全。
*/
sidebus_t *sidebus_acquire(libvlc_int_t *root)
{
    /* 在 root 上宣告/確保存在一個名為 SIDEINFO_BUS_VAR 的「變數槽」，型別是地址（指標）。
    VLC 內部有一套「變數系統」（var_* API），可以把值綁在某個物件上，常拿來做模組之間的鬆耦合溝通。 */
    var_Create(root, SIDEINFO_BUS_VAR, VLC_VAR_ADDRESS);

    /*讀出這個變數槽目前存的指標（可能是先前已建立好的 bus）。
    如果已經有，if (bus) return bus; 直接回傳，避免重複建立 —— 這就是「get」的部份。*/
    sidebus_t *bus = var_GetAddress(root, SIDEINFO_BUS_VAR);
    if (bus) return bus;

    /*如果沒有，就建立一個新的 bus：*/
    bus = calloc(1, sizeof(*bus));
    if (!bus) return NULL;
    bus->head = bus->tail = 0;
    bus->size = SIDEINFO_BUS_CAP;

    vlc_mutex_init(&bus->lock);

    /*把新建的指標寫回 root 的變數槽，之後大家再呼叫這個函式就能拿到同一個 bus*/
    var_SetAddress(root, SIDEINFO_BUS_VAR, bus); 
    return bus;
}

void sidebus_release(libvlc_int_t *root)
{
    var_Create(root, SIDEINFO_BUS_VAR, VLC_VAR_ADDRESS);

    /*讀出這個變數槽目前存的指標（可能是先前已建立好的 bus）。
    如果已經有，if (bus) return bus; 直接回傳，避免重複建立 —— 這就是「get」的部份。*/
    sidebus_t *bus = var_GetAddress(root, SIDEINFO_BUS_VAR);
    if (!bus) return;

    vlc_mutex_lock(&bus->lock);

    for (unsigned i = 0; i < SIDEINFO_BUS_CAP; i++)
        sidebus_free_entry_list(bus->ring[i].head);
    vlc_mutex_unlock(&bus->lock);
    vlc_mutex_destroy(&bus->lock);

    VLC_UNUSED(root);
    VLC_UNUSED(bus);
    /* keep it alive until process exit; simple and safe for plugins */
}

int sidebus_push(sidebus_t *bus, const vlc_side_packet_t *in) {
    if (!bus || !in) return -1;

    vlc_mutex_lock(&bus->lock);
    const int write_idx = bus->tail % SIDEINFO_BUS_CAP;
    vlc_side_packet_t *s_write = &bus->ring[write_idx];

    if (bus->tail - bus->head >= bus->size) {
        /* ring buffer 滿了，覆寫最舊的那格 */
        const int read_idx  = bus->head % SIDEINFO_BUS_CAP;
        vlc_side_packet_t *s_read = &bus->ring[read_idx];
        sidebus_free_entry_list(s_read->head);
        
        fprintf(stderr, "[sidebus] buffer full, clear the oldest member\n");
        fflush(stderr);
    }
    fprintf(stderr, "[sidebus] push pts=%"PRId64"\n",
                (int64_t)in->pts);
    fflush(stderr);

    s_write->pts      = in->pts;
    s_write->codec_id = in->codec_id;

    /* 深拷貝 payload 內容到 slot 內部 */
    if (!(s_write->head = deep_clone_entry_list(in->head))) {
        /* 若記憶體不足，保證 slot 內容仍一致且安全 */
        sidebus_free_entry_list(s_write->head);
    }
    ++bus->tail;    /* advance head */

    vlc_mutex_unlock(&bus->lock);
    return 0;
}

bool sidebus_pop(sidebus_t *bus, vlc_side_packet_t *out, vlc_tick_t pts)
{
    if (!bus || !out) return false;

    vlc_mutex_lock(&bus->lock);

    int best_idx = -1;
    vlc_tick_t best_absdiff = INT64_MAX;
    vlc_tick_t best_diff = 0;

    for (size_t i = bus->head; i < bus->tail; ++i) {
        vlc_side_packet_t *s = &bus->ring[i % SIDEINFO_BUS_CAP];

        vlc_tick_t diff = s->pts - pts;
        vlc_tick_t ad   = llabs(diff);

        // 同距離時偏好「不晚於當前幀」
        if (ad < best_absdiff || (ad == best_absdiff && diff <= 0 && best_diff > 0)) {
            best_absdiff = ad;
            best_diff    = diff;
            best_idx     = i % SIDEINFO_BUS_CAP;
        }
    }

    if (best_idx < 0) {
        fprintf(stderr, "[sideinfo] pop MISS: req=%"PRId64"\n",
                (int64_t)pts);
        vlc_mutex_unlock(&bus->lock);
        fflush(stderr);
        return false;
    }

    vlc_tick_t best_pts = bus->ring[best_idx].pts;
    vlc_tick_t diff     = best_pts - pts;
    vlc_tick_t ad       = llabs(diff);

    fprintf(stderr, "[sideinfo] pop CAND: req=%"PRId64" best=%"PRId64" diff=%"PRId64" |d|=%"PRId64" tol=%"PRId64"\n",
            (int64_t)pts, (int64_t)best_pts, (int64_t)diff, (int64_t)ad,
            (int64_t)SIDEINFO_PTS_TOL);

    if (ad > SIDEINFO_PTS_TOL) {
        fprintf(stderr, "[sideinfo] pop MISS: out-of-tol \n");
        vlc_mutex_unlock(&bus->lock);
        fflush(stderr);
        return false;
    }

    // Deep copy
    if ((out->head = deep_clone_entry_list(bus->ring[best_idx].head))) {
        out->pts      = best_pts;
        out->codec_id = bus->ring[best_idx].codec_id;

        // 清掉被取用的那格
        sidebus_free_entry_list(bus->ring[best_idx].head);
        bus->ring[best_idx].pts  = 0;
        ++bus->head;
    } 

    vlc_mutex_unlock(&bus->lock);

    fprintf(stderr, "[sideinfo] pop HIT : best=%"PRId64"  size=%ld\n",
            (int64_t)best_pts, bus->size);
    fflush(stderr);
    return true;
}

bool sidebus_free_entry(vlc_side_entry_t *entry) {
    sidebus_free_entry_list(entry);
}