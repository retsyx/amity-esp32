#pragma once
#include <esp_hap_controllers.h>
#include <esp_hap_pair_common.h>
typedef struct {
    hap_ctrl_data_t controllers[HAP_MAX_CONTROLLERS];
} hap_priv_t;
extern hap_priv_t hap_priv;
