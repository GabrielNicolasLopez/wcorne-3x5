#pragma once

#include <stdbool.h>
#include <stdint.h>

int64_t corne_active_time_total(void);
/* Last received right-half total; fresh is false after a missed polling interval. */
bool corne_right_active_time(int64_t *total_ms, bool *fresh);
