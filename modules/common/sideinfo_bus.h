#include <vlc_common.h>

#define clamp(v, lo, hi) v < lo ? lo : (v > hi ? hi : v)

/* 單一 side-data 項（泛型） */
typedef struct vlc_side_entry {
    uint32_t            type;   /* vlc_side_type_t */
    uint32_t            flags;  /* 保留：壓縮/色彩空間/版本等 */
    uint32_t            size;   /* data bytes */
    uint8_t            *data;   /* heap owned */
    struct vlc_side_entry *next;
} vlc_side_entry_t;

typedef struct vlc_side_packet {
    vlc_tick_t        pts;      /* 通常 = picture->date */
    vlc_side_entry_t *head;     /* side info linked list */
    int               codec_id; /* AVCodecID*/
} vlc_side_packet_t;

typedef struct sidebus sidebus_t;

/* API */
bool sidebus_free_entry(vlc_side_entry_t *entry);
sidebus_t *sidebus_acquire(libvlc_int_t *root);
void sidebus_release(libvlc_int_t *root);

int sidebus_push(sidebus_t *bus, const vlc_side_packet_t *in);
/* 返回 deep copy；呼叫端用畢需 sideinfo_payload_dispose() 釋放 */
bool sidebus_pop(sidebus_t *bus, vlc_side_packet_t *out, vlc_tick_t pts);
