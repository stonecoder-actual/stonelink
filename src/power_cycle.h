#pragma once
#include <stdbool.h>
#include <stdint.h>

// Main-task-only controller. Timings use boot milliseconds, never wall clock.
// Manual confirmations describe the external supply, not measured adapter VBUS.
typedef enum { POWER_MANUAL, POWER_GPIO } power_mode_t;
typedef enum {
    POWER_IDLE, POWER_WAIT_OFF, POWER_OFF_HOLD, POWER_WAIT_ON,
    POWER_SETTLE, POWER_WAIT_DEVICE, POWER_WORK, POWER_RETRY,
    POWER_WAIT_FINAL_OFF, POWER_DONE
} power_phase_t;
typedef bool (*power_drive_fn)(bool on);
typedef void (*power_notice_fn)(const char *message);

void power_cycle_init(power_drive_fn drive, power_notice_fn notice);
bool power_cycle_set_mode(power_mode_t mode); // Idle only; GPIO starts LOW.
power_mode_t power_cycle_mode(void);
power_phase_t power_cycle_phase(void);
const char *power_cycle_phase_name(void);
int64_t power_cycle_deadline(void);
unsigned power_cycle_attempts(void);
bool power_cycle_busy(void);
bool power_cycle_begin(int64_t now_ms); // Caller must release MSC first.
bool power_cycle_confirm(bool on, int64_t now_ms);
// Returns true exactly once per acquisition attempt; USB events must keep polling.
bool power_cycle_tick(int64_t now_ms, bool device_present);
void power_cycle_acquisition_failed(int64_t now_ms);
void power_cycle_finish(bool success, int64_t now_ms); // Caller releases MSC first.
void power_cycle_cancel(void); // GPIO goes LOW; manual state remains unverified.
void power_cycle_abandon(void); // Cleanup failed: stop WITHOUT changing supply.
bool power_cycle_result(bool *success); // Consumes DONE and returns to IDLE.
bool power_cycle_off_confirmed(void); // Final OFF acknowledgment/command, not measured VBUS.
