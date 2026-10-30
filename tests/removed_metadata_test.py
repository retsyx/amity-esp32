#!/usr/bin/env python3
"""Exercise the actual post-removal metadata purge and durable name registry."""

from pathlib import Path
import os
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "amity/consortium/runtime.hpp"
#include "controller_policy.hpp"
#include "hostname_registry.hpp"
#include <cassert>
#include <iostream>
#include <memory>
#include <mutex>
namespace amity::consortium {
using namespace platform;
template<class T>T id(unsigned n){char value[33];snprintf(value,sizeof(value),"%032x",n);return *T::parse(value);}
std::mutex lock;Group group;Identity identity;RuntimeStatus status;RuntimeLifecycleHooks lifecycle;HostnameRegistry hostnames;
struct Active{model::OctopusId peer;};
struct Cache{model::OctopusId owner;std::string snapshot;};
struct SettingsAuthorization{bool local=false;std::weak_ptr<Active> link;};
std::vector<Cache> settings_caches,settings_operations;
SettingsAuthorization settings_authorization;
bool storage_ok=true;unsigned writes=0,revoked=0;std::string stored;model::OctopusId revoked_owner;
esp_err_t write_record(const char* ns,const char* key,std::string_view bytes){
 assert(std::string(ns)=="amity_group"&&std::string(key)=="hostnames");++writes;
 if(!storage_ok)return ESP_FAIL;stored=bytes;return ESP_OK;
}
void reset(){
 group={};identity={};status={};hostnames={};settings_caches.clear();settings_operations.clear();settings_authorization={};
 identity.id=id<model::OctopusId>(1);group.role=Group::coordinator;group.authority=identity.id;group.epoch=1;status.authority_ready=true;
 const auto remember=[](std::string_view bytes){stored=bytes;return true;};
 assert(hostnames.remember(id<model::OctopusId>(2),"removed",remember)==Error::none);
 assert(hostnames.remember(id<model::OctopusId>(3),"kept",remember)==Error::none);
 settings_caches={{id<model::OctopusId>(2),"removed settings"},{id<model::OctopusId>(3),"kept settings"}};
 settings_operations=settings_caches;storage_ok=true;writes=revoked=0;revoked_owner={};
 lifecycle.revoked=[](model::OctopusId owner){++revoked;revoked_owner=owner;};
}
'''
SUITE = r'''
int main(){using namespace amity::consortium;
 const auto target=id<model::OctopusId>(2),other=id<model::OctopusId>(3);
 reset();auto active=std::make_shared<Active>();active->peer=target;settings_authorization={false,active};
 assert(runtime_forget_removed_octopus(target)==ESP_OK&&writes==1&&revoked==1&&revoked_owner==target);
 assert(hostnames.lookup(target).empty()&&hostnames.lookup(other)=="kept");
 assert(settings_caches.size()==1&&settings_caches[0].owner==other&&settings_caches[0].snapshot=="kept settings");
 assert(settings_operations.size()==1&&settings_operations[0].owner==other&&settings_authorization.link.expired());
 HostnameRegistry reboot;assert(reboot.restore(stored)&&reboot.lookup(target).empty()&&reboot.lookup(other)=="kept");
 assert(runtime_forget_removed_octopus(target)==ESP_OK&&writes==1); // Absent metadata is idempotent.
 std::cout<<"PASS actual removal metadata: durable name deletion, exact settings purge, authorization cancellation, owner cleanup and reboot\n";
 reset();const auto before=stored;storage_ok=false;
 assert(runtime_forget_removed_octopus(target)==ESP_FAIL&&writes==1&&!revoked&&stored==before);
 assert(hostnames.lookup(target)=="removed"&&settings_caches.size()==2&&settings_operations.size()==2);
 std::cout<<"PASS metadata storage failure cannot claim deletion or discard unchanged in-memory records\n";
 reset();assert(runtime_forget_removed_octopus({})==ESP_ERR_INVALID_ARG);
 assert(runtime_forget_removed_octopus(identity.id)==ESP_ERR_INVALID_STATE&&!writes&&!revoked);
 group.peers.push_back({target,"credential","pin",false});
 assert(runtime_forget_removed_octopus(target)==ESP_ERR_INVALID_STATE&&!writes&&!revoked&&hostnames.lookup(target)=="removed");
 group.peers.clear();status.authority_ready=false;assert(runtime_forget_removed_octopus(target)==ESP_ERR_INVALID_STATE);
 status.authority_ready=true;group.role=Group::member;assert(runtime_forget_removed_octopus(target)==ESP_ERR_INVALID_STATE&&!writes);
 std::cout<<"PASS original-main authority, self protection and re-enrollment/provisional fences precede metadata mutation\n";
}
'''


def main():
    source = (ROOT / 'components/consortium/runtime.cpp').read_text()
    actual = function(source, 'esp_err_t runtime_forget_removed_octopus(')
    with tempfile.TemporaryDirectory(prefix='amity-forget-removed-') as directory:
        directory = Path(directory)
        (directory / 'esp_err.h').write_text(
            '#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_FAIL=1,'
            'ESP_ERR_INVALID_ARG=2,ESP_ERR_INVALID_STATE=3;\n')
        cpp, binary = directory / 'test.cpp', directory / 'test'
        cpp.write_text(PREFIX + actual + '\n}\n' + SUITE)
        sources = [ROOT / 'components/consortium' / name for name in
                   ('json.cpp', 'wire.cpp', 'session.cpp', 'owned_intent.cpp',
                    'settings.cpp', 'update.cpp', 'transfer.cpp')]
        sources.append(ROOT / 'components/amity_system/model.cpp')
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) +
                       ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                        '-I' + str(directory)] +
                       ['-I' + str(ROOT / path) for path in
                        ('components/consortium', 'components/consortium/include', 'components/amity_system/include')] +
                       [str(cpp), *map(str, sources), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
