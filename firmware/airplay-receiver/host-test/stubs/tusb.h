#pragma once
#include <stdint.h>
#include <stdbool.h>
// Test stub: captures reports instead of touching hardware.
bool tud_hid_report(uint8_t report_id, void const *report, uint16_t len);
