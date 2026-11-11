#!/usr/bin/env python3
"""Block actual runtime persistence while exercising admission and competing mutations."""
from pathlib import Path
import argparse, os, shlex, subprocess, tempfile
from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=ROOT/'components/consortium/runtime.cpp')
    source = parser.parse_args().source.read_text()
    names = ['static bool queued_firmware_stream(', 'static bool queued_expired(',
             'static bool pure_key_release(', 'static bool queued_cleanup(', 'static bool queued_causal(',
             'void request_session_restart(', 'Error remember_hostname(', 'void remember_live_hostname(',
             'std::string retirement_hash(', 'esp_err_t runtime_remove_octopus(',
             'esp_err_t runtime_forget_removed_octopus(', 'RuntimeStatus runtime_status(',
             'static esp_err_t queue_update(', 'esp_err_t runtime_send_update(', 'static esp_err_t queue_control(',
             'void runtime_fence_owned_link(', 'esp_err_t runtime_send_owned(']
    actual = (ROOT/'components/consortium/removal_runtime.inc').read_text()
    actual += ''.join(function(source, name) for name in names)
    fixture = (ROOT/'tests/consortium_persistence_fixture.cpp').read_text().replace('// ACTUAL_RUNTIME', actual)
    with tempfile.TemporaryDirectory(prefix='amity-persistence-') as directory:
        temp = Path(directory)
        (temp/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_TIMEOUT=4,ESP_FAIL=5,ESP_ERR_NOT_FOUND=6,ESP_ERR_NOT_SUPPORTED=7;\n')
        cpp, binary = temp/'test.cpp', temp/'test'
        cpp.write_text(fixture)
        sources = [ROOT/'components/consortium'/name for name in ['json.cpp','wire.cpp','session.cpp','owned_intent.cpp','settings.cpp','update.cpp','transfer.cpp']]+[ROOT/'components/amity_system/model.cpp']
        subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-pthread','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-I'+str(temp)]+['-I'+str(ROOT/path) for path in ['components/consortium','components/consortium/include','components/amity_system/include']]+list(map(str,[cpp]+sources))+['-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=30)

if __name__ == '__main__': main()
