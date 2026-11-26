#include "power_cycle.h"
#include <stddef.h>

#define OFF_MS 10000
#define SETTLE_MS 5000
#define DEVICE_MS 30000
#define MANUAL_MS 300000
#define REMINDER_MS 30000
#define RETRY_MS 5000
#define MAX_ATTEMPTS 3

static power_mode_t mode;
static power_phase_t phase;
static power_drive_fn drive_output;
static power_notice_fn notice;
static int64_t deadline, reminder;
static unsigned attempts;
static bool result;

static void say(const char *message) { if(notice) notice(message); }
static bool drive(bool on) {
    if(drive_output && drive_output(on)) return true;
    say("POWER ERROR GPIO21 command failed; cycle stopped; external supply state unverified");
    result=false; phase=POWER_DONE; deadline=0;
    return false;
}
static void wait_manual(power_phase_t next, int64_t now) {
    phase=next; deadline=now+MANUAL_MS; reminder=now+REMINDER_MS;
    if(next==POWER_WAIT_ON)
        say("POWER ACTION reconnect ONLY external +5 V to USB breakout, then type POWER ON");
    else if(next==POWER_WAIT_FINAL_OFF)
        say("POWER ACTION collection released; disconnect ONLY external +5 V, then type POWER OFF to finish");
    else
        say("POWER ACTION disconnect ONLY external +5 V to USB breakout, then type POWER OFF; keep grounds/data connected");
}
void power_cycle_init(power_drive_fn output, power_notice_fn message) {
    drive_output=output; notice=message; mode=POWER_MANUAL; phase=POWER_IDLE;
    deadline=0; attempts=0; result=false;
}
bool power_cycle_set_mode(power_mode_t next) {
    if(phase!=POWER_IDLE || (next!=POWER_MANUAL && next!=POWER_GPIO)) return false;
    if((mode==POWER_GPIO || next==POWER_GPIO) && !drive(false)) {
        // Configuration failure is not a collection result.
        phase=POWER_IDLE; return false;
    }
    mode=next;
    say(mode==POWER_GPIO ? "POWER mode=GPIO pin=21 active_high=1 command=OFF; not a VBUS measurement"
                        : "POWER mode=MANUAL; GPIO21 not used; external +5 V requires confirmations");
    return true;
}
power_mode_t power_cycle_mode(void) { return mode; }
power_phase_t power_cycle_phase(void) { return phase; }
int64_t power_cycle_deadline(void) { return deadline; }
unsigned power_cycle_attempts(void) { return attempts; }
bool power_cycle_busy(void) { return phase!=POWER_IDLE; }
const char *power_cycle_phase_name(void) {
    static const char *names[]={"IDLE","WAIT_OFF","OFF_HOLD","WAIT_ON","SETTLE",
                               "WAIT_DEVICE","WORK","RETRY","WAIT_FINAL_OFF","DONE"};
    return names[phase];
}
bool power_cycle_begin(int64_t now) {
    if(phase!=POWER_IDLE) return false;
    attempts=0; result=false;
    if(mode==POWER_MANUAL) wait_manual(POWER_WAIT_OFF,now);
    else if(drive(false)) {
        say("POWER GPIO21=0; external supply commanded OFF; holding 10 seconds");
        phase=POWER_OFF_HOLD; deadline=now+OFF_MS;
    }
    return true;
}
bool power_cycle_confirm(bool on, int64_t now) {
    if(mode!=POWER_MANUAL) return false;
    // Late confirmation cannot bypass a timeout that has not been ticked yet.
    if((phase==POWER_WAIT_OFF || phase==POWER_WAIT_ON || phase==POWER_WAIT_FINAL_OFF) && now>=deadline) {
        power_cycle_cancel(); return false;
    }
    if(!on && phase==POWER_WAIT_OFF) {
        say("POWER manual OFF confirmed; holding external supply disconnected for 10 seconds");
        phase=POWER_OFF_HOLD; deadline=now+OFF_MS;
    } else if(on && phase==POWER_WAIT_ON) {
        say("POWER manual ON confirmed; settling for 5 seconds; adapter VBUS not measured");
        phase=POWER_SETTLE; deadline=now+SETTLE_MS;
    } else if(!on && phase==POWER_WAIT_FINAL_OFF) {
        say("POWER manual final OFF confirmed; source released");
        phase=POWER_DONE; deadline=0;
    } else return false;
    return true;
}
void power_cycle_finish(bool success, int64_t now) {
    if(phase!=POWER_WORK && phase!=POWER_WAIT_DEVICE) return;
    result=success;
    if(mode==POWER_MANUAL) wait_manual(POWER_WAIT_FINAL_OFF,now);
    else if(drive(false)) {
        say("POWER GPIO21=0; collection released; external supply commanded OFF");
        phase=POWER_DONE; deadline=0;
    }
}
void power_cycle_cancel(void) {
    if(phase==POWER_IDLE || phase==POWER_DONE) return;
    result=false;
    if(mode==POWER_GPIO) drive(false);
    say("POWER CANCELED; collection failed; manual users disconnect external +5 V; no VBUS measurement");
    phase=POWER_DONE; deadline=0;
}
void power_cycle_abandon(void) {
    say("POWER ERROR storage release failed; supply unchanged; REINIT required before another collection");
    result=false; phase=POWER_DONE; deadline=0;
}
bool power_cycle_tick(int64_t now, bool present) {
    if(phase==POWER_WAIT_OFF || phase==POWER_WAIT_ON || phase==POWER_WAIT_FINAL_OFF) {
        if(now>=deadline) {
            say("POWER TIMEOUT waiting for manual confirmation; no successful cycle recorded");
            power_cycle_cancel();
        } else if(now>=reminder) {
            say(phase==POWER_WAIT_ON ? "POWER WAITING reconnect external +5 V, then POWER ON; POWER CANCEL aborts"
                                    : "POWER WAITING disconnect external +5 V, then POWER OFF; POWER CANCEL aborts");
            reminder=now+REMINDER_MS;
        }
    } else if(phase==POWER_OFF_HOLD && now>=deadline) {
        if(mode==POWER_MANUAL) wait_manual(POWER_WAIT_ON,now);
        else if(drive(true)) {
            say("POWER GPIO21=1; external supply commanded ON; settling 5 seconds");
            phase=POWER_SETTLE; deadline=now+SETTLE_MS;
        }
    } else if(phase==POWER_SETTLE && now>=deadline) {
        phase=POWER_WAIT_DEVICE; deadline=now+DEVICE_MS;
        say("POWER waiting up to 30 seconds for a USB device; fresh enumeration not required if camera keeps USB alive");
    } else if(phase==POWER_RETRY && now>=deadline) {
        phase=POWER_WAIT_DEVICE; deadline=now+DEVICE_MS;
    }
    if(phase==POWER_WAIT_DEVICE) {
        if(now>=deadline) {
            say("POWER ERROR no USB device before deadline");
            power_cycle_finish(false,now);
        } else if(present) {
            attempts++; phase=POWER_WORK; deadline=0; return true;
        }
    }
    return false;
}
void power_cycle_acquisition_failed(int64_t now) {
    if(phase!=POWER_WORK) return;
    if(attempts<MAX_ATTEMPTS) {
        say("POWER acquisition failed; retry in 5 seconds; at most 3 attempts per cycle");
        phase=POWER_RETRY; deadline=now+RETRY_MS;
    } else {
        say("POWER ERROR acquisition attempts exhausted; no reset or power-cycle loop");
        power_cycle_finish(false,now);
    }
}
bool power_cycle_result(bool *success) {
    if(phase!=POWER_DONE) return false;
    if(success) *success=result;
    phase=POWER_IDLE; deadline=0;
    return true;
}
