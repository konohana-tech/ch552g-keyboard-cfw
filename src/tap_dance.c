/* Lean 2 entries: tap/hold, double tap, tap-hold; tapping_term fixed.
 * Hold supports basic keys (report[2]) and layer keys MO/TO/LT
 * (momentary layer while held, level semantics). Modifiers/complex out of scope.
 * 3+ taps are capped to double. Second tap must land before first tap fires.
 * issue #18 (QMK preprocess_tap_dance): foreign press interrupt while
 * unsettled settles to HOLD immediately; own second-tap is ignored. */
#include "tap_dance.h"
#include "ch552.h"
#include "config.h"

__xdata __at (TD_XRAM_BASE) td_entry_t td_entries[TD_COUNT];
__xdata __at (TD_STATE_BASE) uint8_t td_active;
__xdata __at (TD_STATE_BASE+1) uint8_t td_pressed;
__xdata __at (TD_STATE_BASE+2) uint8_t td_start;
__xdata __at (TD_STATE_BASE+3) uint8_t td_hold;
__xdata __at (TD_STATE_BASE+4) uint8_t td_pend;
__xdata __at (TD_STATE_BASE+5) uint8_t td_polls;
__xdata __at (TD_STATE_BASE+6) uint8_t td_hold_layer;
__xdata __at (TD_STATE_BASE+7) uint8_t td_count;

extern __xdata __at (0x00FB) uint8_t tt_now;

void td_init(void){
    uint8_t i;
    for(i=0;i<TD_COUNT;i++){
        td_entries[i].on_tap=0;
        td_entries[i].on_hold=0;
        td_entries[i].on_double_tap=0;
        td_entries[i].on_tap_hold=0;
        td_entries[i].tapping_term=200;
    }
    td_active=0xFF; td_pressed=0; td_hold=0; td_pend=0; td_polls=0; td_hold_layer=0; td_count=0;
}
uint8_t td_is_td_key(uint16_t kc){ return kc>=QK_TAP_DANCE && kc<(QK_TAP_DANCE+TD_COUNT); }
uint8_t td_get_index(uint16_t kc){ return kc&0xFF; }

void td_press(uint8_t idx){
    if(td_active==0xFF){ td_active=idx; td_count=1; td_pressed=1; td_start=tt_now; }
    else if(td_active==idx && !td_pressed && td_count==1){ td_count=2; td_pressed=1; td_start=tt_now; }
}
void td_release(uint8_t idx){
    if(td_active!=idx){
        if(td_hold) td_hold=0;
        td_hold_layer=0;
        return;
    }
    if(td_hold || td_hold_layer){ td_hold=0; td_hold_layer=0; td_active=0xFF; return; }
    td_pressed=0;
}
/* Settle the active dance as HOLD now. Shared by the timeout path and the
 * issue #18 foreign-press interrupt (both run in main-loop context, never
 * ISR, so a plain static helper is overlay-safe). */
static void td_settle_hold(void){
    __xdata td_entry_t *e=&td_entries[td_active];
    uint16_t kc=(td_count>=2 && e->on_tap_hold) ? e->on_tap_hold : e->on_hold;
    uint8_t hi, lo, nl=0;
    if(!kc) kc=e->on_tap;
    hi=(uint8_t)(kc>>8); lo=(uint8_t)kc;
    if(hi==0x52) nl=lo&0x1F; /* MO/TO family */
    else if((hi&0xF0)==0x40) nl=hi&0x0F; /* LT(layer,kc) */
    else if((kc & 0xFF80)==0x7700) nl=0; /* macro: mark but no layer */
    if(nl && nl<VIAL_LAYERS){ td_hold_layer=nl; return; }
    if(kc && kc<=0xFF) td_hold=(uint8_t)kc;
}
void td_task(void){
    if(td_active==0xFF){
        if(td_polls && --td_polls==0) td_pend=0;
        return;
    }
    __xdata td_entry_t *e=&td_entries[td_active];
    uint8_t elapsed=(uint8_t)(tt_now - td_start);
    if(td_pressed){
        if(elapsed>3){ td_settle_hold(); return; } /* timeout HOLD */
    } else {
        if(elapsed>3){ /* released + term: double (3+ capped) else single */
            uint16_t kc=(td_count>=2 && e->on_double_tap) ? e->on_double_tap : e->on_tap;
            if(kc && kc<=0xFF){ td_pend=(uint8_t)kc; td_polls=10; }
            td_active=0xFF;
        }
    }
    if(td_polls && --td_polls==0) td_pend=0;
}
uint8_t td_pending_key(void){ return td_hold ? td_hold : td_pend; }
/* issue #18: foreign-press interrupt (QMK preprocess_tap_dance). Called
 * from debounce_update when a key is pressed while a TD is held
 * undecided. Settles the active dance as HOLD synchronously so the
 * layer is visible to layer_resolve in the same poll. Released-waiting
 * dances (tap proceeds) and the TD's own second tap are excluded by
 * the caller-side guards below. */
void td_notify_press(uint8_t pressed_td_idx){
    if(td_active==0xFF) return;
    if(!td_pressed) return; /* released-waiting: tap proceeds (QMK SINGLE_TAP) */
    if(pressed_td_idx==td_active) return; /* own second tap: no interrupt */
    td_settle_hold(); /* immediate HOLD (QMK SINGLE_HOLD/DOUBLE_HOLD) */
}
