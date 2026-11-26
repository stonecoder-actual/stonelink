// Production timing/state policy with simulated clock, GPIO and USB presence.
// No physical power, USB devices or filesystems are touched.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/power_cycle.c"

static bool levels[100], gpio_ok;
static unsigned writes, messages;
static char last_message[256];
static bool output(bool on) { assert(writes<100); levels[writes++]=on; return gpio_ok; }
static void message(const char *text) { messages++; snprintf(last_message,sizeof(last_message),"%s",text); }
static void init(void) {
    writes=messages=0; gpio_ok=true;
    power_cycle_init(output,message);
    assert(power_cycle_mode()==POWER_MANUAL && !power_cycle_busy() && writes==0);
    assert(!power_cycle_off_confirmed());
}
static void take(bool expected) {
    bool success=!expected;
    assert(power_cycle_result(&success) && success==expected);
    assert(!power_cycle_busy() && !power_cycle_result(NULL));
}
static void manual_work(void) {
    assert(power_cycle_begin(0));
    assert(!power_cycle_off_confirmed());
    assert(!power_cycle_confirm(true,1));
    assert(power_cycle_confirm(false,2));
    assert(!power_cycle_confirm(true,10001));
    assert(!power_cycle_tick(10001,true));
    assert(power_cycle_phase()==POWER_OFF_HOLD);
    assert(!power_cycle_tick(10002,true));
    assert(power_cycle_phase()==POWER_WAIT_ON);
    assert(power_cycle_confirm(true,10003));
    assert(!power_cycle_tick(15002,true));
    assert(power_cycle_tick(15003,true));
    assert(!power_cycle_tick(15004,true)); // Work handed out exactly once.
    assert(power_cycle_attempts()==1);
    assert(!power_cycle_off_confirmed());
}
int main(void) {
    init(); manual_work();
    assert(!power_cycle_begin(16000));
    assert(!power_cycle_set_mode(POWER_GPIO));
    power_cycle_finish(true,20000);
    assert(power_cycle_phase()==POWER_WAIT_FINAL_OFF);
    assert(!power_cycle_result(NULL)); // Acquisition is not a completed cycle.
    assert(!power_cycle_off_confirmed()); // Backend must wait for final OFF.
    assert(!power_cycle_confirm(true,20001));
    assert(power_cycle_confirm(false,20002));
    take(true); assert(power_cycle_off_confirmed());
    assert(power_cycle_begin(21000) && !power_cycle_off_confirmed());
    power_cycle_cancel(); take(false); assert(!power_cycle_off_confirmed());
    assert(writes==0); // Manual mode must never command any GPIO.

    init(); manual_work();
    power_cycle_acquisition_failed(15004);
    assert(!power_cycle_tick(20003,true));
    assert(power_cycle_tick(20004,true));
    power_cycle_acquisition_failed(20005);
    assert(power_cycle_tick(25005,true));
    assert(power_cycle_attempts()==3);
    power_cycle_acquisition_failed(25006);
    assert(power_cycle_phase()==POWER_WAIT_FINAL_OFF);
    assert(power_cycle_confirm(false,25007)); take(false);

    // Reminders do not reset deadlines; timeouts never fabricate success.
    init(); assert(power_cycle_begin(100));
    unsigned old_messages=messages;
    assert(!power_cycle_tick(30100,false)); assert(messages>old_messages);
    assert(power_cycle_deadline()==300100);
    assert(!power_cycle_tick(300100,false)); take(false); assert(writes==0);
    init(); assert(power_cycle_begin(0));
    assert(!power_cycle_confirm(false,300000)); take(false);
    init(); manual_work(); power_cycle_finish(true,20000);
    assert(!power_cycle_tick(320000,true)); take(false);
    assert(!power_cycle_off_confirmed()); // Timeout is not an OFF confirmation.
    init(); manual_work(); power_cycle_finish(false,20000);
    assert(power_cycle_confirm(false,20001)); take(false);

    init(); assert(power_cycle_set_mode(POWER_GPIO));
    assert(writes==1 && !levels[0]);
    assert(power_cycle_begin(100)); assert(writes==2 && !levels[1]);
    assert(!power_cycle_confirm(false,101));
    assert(!power_cycle_tick(10099,true)); assert(writes==2);
    assert(!power_cycle_tick(10100,true)); assert(writes==3 && levels[2]);
    assert(!power_cycle_tick(15099,true));
    assert(!power_cycle_tick(15100,false)); // No connected USB yet.
    assert(power_cycle_phase()==POWER_WAIT_DEVICE);
    assert(power_cycle_tick(20000,true));
    power_cycle_finish(true,21000);
    assert(writes==4 && !levels[3]); take(true);
    assert(power_cycle_off_confirmed());
    assert(power_cycle_set_mode(POWER_MANUAL)); assert(!levels[writes-1]);

    init(); assert(power_cycle_set_mode(POWER_GPIO)); assert(power_cycle_begin(0));
    power_cycle_tick(10000,false); power_cycle_tick(15000,false);
    assert(!power_cycle_tick(45000,false)); // Bounded enumeration wait.
    assert(!levels[writes-1]); take(false);
    init(); assert(power_cycle_set_mode(POWER_GPIO)); assert(power_cycle_begin(0));
    power_cycle_cancel(); assert(!levels[writes-1]); take(false);
    init(); manual_work(); power_cycle_cancel(); take(false); assert(writes==0);

    // If cleanup fails, do not cut supply under a potentially mounted source.
    init(); assert(power_cycle_set_mode(POWER_GPIO)); assert(power_cycle_begin(0));
    power_cycle_tick(10000,true); assert(power_cycle_tick(15000,true));
    unsigned before=writes; power_cycle_abandon(); assert(writes==before);
    power_cycle_cancel(); assert(writes==before); take(false);
    assert(!power_cycle_off_confirmed()); // Failed release prevents backend gate.

    init(); gpio_ok=false;
    assert(!power_cycle_set_mode(POWER_GPIO)); assert(!power_cycle_busy());
    assert(power_cycle_mode()==POWER_MANUAL);
    init(); assert(power_cycle_set_mode(POWER_GPIO)); assert(power_cycle_begin(0));
    gpio_ok=false; power_cycle_tick(10000,true); take(false);
    puts("PASS power cycle: manual/GPIO, confirmations, timing, retries, timeout, cancel, cleanup failure");
    return 0;
}
