/* tap_dance.c - lean 2 entries: single tap/hold, double tap, tap-hold; 200ms fixed.
 * Hold supports basic keys (report[2]) and layer keys MO/TG/TO/LT
 * (momentary layer while held, level semantics). Modifiers/complex out of scope.
 * 3+ taps are capped to double. Second tap must land before first tap fires. */
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
void td_task(void){
    if(td_active==0xFF){
        if(td_polls && --td_polls==0) td_pend=0;
        return;
    }
    __xdata td_entry_t *e=&td_entries[td_active];
    uint8_t elapsed=(uint8_t)(tt_now - td_start);
    if(td_pressed){
        if(elapsed>3){ /* >196ms hold: double uses on_tap_hold, single uses on_hold */
            uint16_t kc=(td_count>=2 && e->on_tap_hold) ? e->on_tap_hold : e->on_hold;
            if(!kc) kc=e->on_tap;
            uint8_t hi=(uint8_t)(kc>>8), lo=(uint8_t)kc, nl=0;
            if(hi==0x52) nl=lo&0x1F; /* MO/TG/TO family */
            else if((hi&0xF0)==0x40) nl=hi&0x0F; /* LT(layer,kc) */
            if(nl && nl<VIAL_LAYERS){ td_hold_layer=nl; return; }
            if(kc && kc<=0xFF) td_hold=(uint8_t)kc;
            return;
        }
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
