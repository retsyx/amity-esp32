#pragma once
#define HAP_SUCCESS 0
#define HAP_FAIL (-1)
#define HAP_INTERNAL_EVENT_ACC_UNPAIRED 3
int hap_send_event(int event);
