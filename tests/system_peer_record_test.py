#!/usr/bin/env python3
"""The /api/system roster record carries every runtime peer state the UI reads."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from legacy_fixtures.removal import PREFIX
from slice_support import block, function

ROOT = Path(__file__).resolve().parents[1]

SUITE = r'''
namespace amity::consortium {
HostnameRegistry hostnames;
@RUNTIME_PEERS@
std::string records(){
    Json_ owner(cJSON_CreateObject(),cJSON_Delete);auto* membership=owner.get();
    auto* peers=cJSON_AddArrayToObject(membership,"peers");
@RECORD@
    char* text=cJSON_PrintUnformatted(peers);std::string out=text;cJSON_free(text);return out;
}
void checks(){
    const auto member=ident<model::OctopusId>(3),joining=ident<model::OctopusId>(9);
    // A committed member and a candidate whose join is stored on the main but
    // not yet committed over the LAN, before its name is remembered.
    group.peers={{member,"leaf-a","pin-a",true},{joining,"leaf-b","pin-b",false}};links.clear();
    const auto runtime=runtime_peers();
    assert(runtime.size()==2&&runtime[1].id==joining&&!runtime[1].enrolled&&runtime[1].provisional);
    Json_ parsed(cJSON_Parse(records().c_str()),cJSON_Delete);assert(cJSON_GetArraySize(parsed.get())==2);
    // The UI lists, and reviews removal of, every enrolled or provisional peer.
    for(auto* record=parsed.get()->child;record;record=record->next){
        const bool enrolled=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(record,"enrolled"));
        const bool provisional=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(record,"provisional"));
        assert(enrolled!=provisional);
    }
    std::cout<<"PASS /api/system peer records state enrolled or provisional for every roster peer\n";
}
}
int main(){amity::consortium::checks();}
'''


def main():
    runtime = (ROOT / 'components/consortium/runtime.cpp').read_text()
    admin = (ROOT / 'components/web_admin/system_admin.cpp').read_text()
    snapshot = admin[admin.index('esp_err_t snapshot(httpd_req_t* req){'):]
    record = block(snapshot[snapshot.index('    auto* peers=cJSON_AddArrayToObject(membership,"peers");'):],
                   '    for(const auto& peer:amity::consortium::runtime_peers()){')
    suite = SUITE.replace('@RUNTIME_PEERS@', function(runtime, 'std::vector<RuntimePeerStatus> runtime_peers(')).replace('@RECORD@', record)
    json_dir = ROOT / 'managed_components/espressif__cjson/cJSON'
    if not (json_dir / 'cJSON.c').exists():
        json_dir = json_dir.parent
    with tempfile.TemporaryDirectory(prefix='amity-peer-record-') as directory:
        directory = Path(directory)
        (directory / 'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_TIMEOUT=4,ESP_FAIL=5,ESP_ERR_NOT_FOUND=6,ESP_ERR_NOT_SUPPORTED=7;\n')
        cpp, binary = directory / 'test.cpp', directory / 'test'
        cpp.write_text('#include "hostname_registry.hpp"\n#include "cJSON.h"\n' + PREFIX +
                       'using Json_=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;\n' + suite)
        subprocess.run(['cc', '-g', '-Wno-deprecated-declarations', '-fsanitize=address,undefined', '-I' + str(json_dir), '-c',
                        str(json_dir / 'cJSON.c'), '-o', str(directory / 'json.o')], check=True)
        sources = [ROOT / 'components/consortium' / name for name in ('json.cpp', 'wire.cpp', 'session.cpp', 'owned_intent.cpp', 'settings.cpp', 'update.cpp')]
        sources.append(ROOT / 'components/amity_system/model.cpp')
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                       '-I' + str(directory), '-I' + str(json_dir)] + ['-I' + str(ROOT / path) for path in ('components/consortium', 'components/consortium/include', 'components/amity_system/include')] +
                       [str(cpp), *map(str, sources), str(directory / 'json.o'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
