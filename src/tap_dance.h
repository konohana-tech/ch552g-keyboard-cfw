/* tap_dance.h - Vial Tap Dance 2 entries, CH552 lean */
#ifndef TAP_DANCE_H
#define TAP_DANCE_H
#include <stdint.h>
#define TD_COUNT 2
#define QK_TAP_DANCE 0x5700
#define QK_TAP_DANCE_MAX 0x57FF
#define TD_ENTRY_LEN 10
#define TD_XRAM_BASE 0x0130
#define TD_STATE_BASE 0x0150
/* Hold layer slot (0x0156: gap between TD state and mc_dtick 0x0158).
 * 0 = none, else 1..VIAL_LAYERS-1: momentary layer while TD held. */
extern __xdata __at (TD_STATE_BASE+6) uint8_t td_hold_layer;
extern __xdata __at (TD_STATE_BASE+7) uint8_t td_count; /* taps in current dance (1..2, capped) */
typedef struct {
    uint16_t on_tap;
    uint16_t on_hold;
    uint16_t on_double_tap;
    uint16_t on_tap_hold;
    uint16_t tapping_term;
} td_entry_t;
extern __xdata __at (TD_XRAM_BASE) td_entry_t td_entries[TD_COUNT];
void td_init(void);
uint8_t td_is_td_key(uint16_t kc);
uint8_t td_get_index(uint16_t kc);
void td_press(uint8_t idx);
void td_release(uint8_t idx);
void td_task(void);
uint8_t td_pending_key(void);
#endif
