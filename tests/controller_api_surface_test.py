#!/usr/bin/env python3
"""Execute the real mutation router against the supported administration contract."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / 'components/web_admin/v2_admin.cpp').read_text()
    assert 'path=="/api/front-physical-address"' not in (ROOT / 'components/web_admin/web_admin.cpp').read_text()
    start = source.index('std::optional<std::string> device_settings_target(')
    router = source[start:source.index('void publish_inputs()', start)]
    # Unknown routes are rejected before reading a request body or admitting work.
    rejection = source.index('const auto fields=request_fields(path);if(!fields)')
    assert rejection < source.index('std::string data;if(!web::body(req,data))', rejection)
    program = r'''
#include <algorithm>
#include <cassert>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
''' + router + r'''
int main(){
    for(const auto* path:{"membership/create","membership/move-coordinator",
        "membership/input-role","remotes/manage","integrations/primary-zone",
        "setup-draft","setup/connection","setup/propose-activity",
        "backups/legacy/preview","backups/schema2/preview","input-context",
        "scans","zones/0123456789abcdef0123456789abcdef/activity",
        "zones/0123456789abcdef0123456789abcdef/key",
        "zones/0123456789abcdef0123456789abcdef/standby","settings/remotes"}){
        assert(!request_fields(std::string("/api/v2/")+path));
    }
    for(const auto* path:{"config","config/validate","replacements/preview",
        "replacements/apply","membership/add","membership/remove",
        "updates/validate","updates/cohort","updates/chunk","updates/install",
        "updates/cancel"}){
        const auto fields=request_fields(std::string("/api/v2/")+path);
        assert(fields&&std::count(fields->begin(),fields->end(),"request_id")==1);
        for(const auto& field:*fields)assert(field!="remote_id"&&field!="allow"&&field!="primary_remote_id");
    }
    const auto replacement=request_fields("/api/v2/replacements/apply");
    assert(std::find(replacement->begin(),replacement->end(),"remote_pairing_acknowledged")==replacement->end());
    const auto addition=request_fields("/api/v2/membership/add");
    assert(std::find(addition->begin(),addition->end(),"name")!=addition->end());
    assert(std::find(addition->begin(),addition->end(),"location")==addition->end());
    const auto settings=request_fields("/api/v2/devices/0123456789abcdef0123456789abcdef/settings");
    assert(settings&&settings->size()==5);
    for(const auto* path:{"/api/v2/devices/00000000000000000000000000000000/settings",
        "/api/v2/devices/0123456789ABCDEF0123456789abcdef/settings",
        "/api/v2/devices/0123456789abcdef0123456789abcdef/settings/extra",
        "/api/v2/devices/0123456789abcdef/settings"})assert(!request_fields(path));
    assert(!request_fields("/api/v2/unknown"));
    std::cout<<"PASS actual administration router rejects retired transfer, member-input, remote-mapping, wizard and alternate-control routes before admission\n";
}
'''
    with tempfile.TemporaryDirectory(dir=ROOT / 'test-results') as directory:
        path = Path(directory)
        (path / 'test.cpp').write_text(program)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                        '-fsanitize=address,undefined', str(path / 'test.cpp'),
                        '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    main()
