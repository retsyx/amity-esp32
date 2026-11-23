# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# Strict seams for the firmware files a harness does not build. Each library,
# <file>_seams, defines every public function one firmware file defines, so a
# binary links the real file or its seams, never both: the duplicate
# definitions would fail to link.
function(amity_seams file)
    cmake_parse_arguments(PARSE_ARGV 1 A "" "" "INCLUDES")
    add_library(${file}_seams OBJECT "${T}/component_seams/${file}.cpp")
    target_include_directories(${file}_seams PRIVATE ${A_INCLUDES} "${T}/core_fakes" "${T}/crypto_fakes"
        "${T}/web_request_fakes" "${T}" "${T}/fakes")
    target_compile_options(${file}_seams PRIVATE ${amity_seam_warnings})
    target_link_libraries(${file}_seams PRIVATE strict)
    set(amity_component_seams ${amity_component_seams} ${file}_seams PARENT_SCOPE)
endfunction()

set(amity_component_seams)

set(seams_system "${C}/amity_system/include")
set(seams_consortium "${C}/consortium/include" ${seams_system} "${C}/device_settings/include")
set(seams_services "${C}/system_services/include" ${seams_consortium})
set(seams_hub "${C}/amity_hub/include" "${C}/cec/include" "${C}/config_store/include" ${seams_system})

amity_seams(consortium_runtime INCLUDES ${seams_consortium})
amity_seams(consortium_credentials INCLUDES "${C}/consortium" ${seams_consortium})
amity_seams(system_services INCLUDES ${seams_services})
amity_seams(log_download INCLUDES ${seams_services})
amity_seams(firmware_update INCLUDES ${seams_services})
amity_seams(peer_firmware_update INCLUDES ${seams_services})
amity_seams(group_firmware_update INCLUDES ${seams_services})
amity_seams(adapter_settings INCLUDES ${seams_services})
amity_seams(member_removal INCLUDES ${seams_services})
amity_seams(device_settings INCLUDES "${C}/device_settings/include" ${seams_system})
amity_seams(factory_reset INCLUDES "${C}/device_settings/include" ${seams_system})
amity_seams(reset_button INCLUDES "${C}/device_settings/include" ${seams_system})
amity_seams(network_diagnostics INCLUDES "${C}/network_diagnostics/include")
amity_seams(wifi_signal INCLUDES "${C}/network_diagnostics/include")
amity_seams(web_server INCLUDES "${C}/web/include")
amity_seams(web_redirect INCLUDES "${C}/web/include")
amity_seams(web_request INCLUDES "${C}/web/include")
amity_seams(web_credentials INCLUDES "${C}/web/include")
amity_seams(config_store_nvs INCLUDES "${C}/config_store/include" ${seams_system})
amity_seams(system_identity INCLUDES "${C}/config_store/include" ${seams_system})
amity_seams(system_control INCLUDES ${seams_hub})
amity_seams(hub_service INCLUDES ${seams_hub})
amity_seams(amity_matter INCLUDES "${C}/amity_matter/include")
amity_seams(homekit INCLUDES "${C}/amity_homekit/include" "${C}/amity_matter/include" ${seams_hub})
amity_seams(ble_remote INCLUDES "${C}/ble_remote/include")
amity_seams(radio INCLUDES "${C}/radio_control/include" ${seams_consortium})
unset(seams_system)
unset(seams_consortium)
unset(seams_services)
unset(seams_hub)
