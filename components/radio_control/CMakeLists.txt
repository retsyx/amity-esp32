idf_component_register(SRCS "radio_control.cpp" INCLUDE_DIRS "include"
    REQUIRES consortium ble_remote esp_matter esp_timer)
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=_ZN4chip11DeviceLayer8Internal14BLEManagerImpl22_SetAdvertisingEnabledEb"
    "-Wl,--wrap=ble_hs_hci_rx_evt")
