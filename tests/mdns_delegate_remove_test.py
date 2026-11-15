#!/usr/bin/env python3
"""Exercise the pinned/generated mDNS delegated-host removal goodbye."""
from pathlib import Path
import importlib.util
import os
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'managed_components/espressif__mdns/mdns_responder.c'
GENERATOR = ROOT / 'tools/patch_mdns_delegate_remove.py'
spec = importlib.util.spec_from_file_location('patch_mdns_delegate_remove', GENERATOR)
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)

PREFIX = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>
#include <vector>
#define MDNS_MAX_INTERFACES 3
#define MDNS_FLAGS_QR_AUTHORITATIVE 0x8400
#define MDNS_TYPE_A 0x0001
#define MDNS_TYPE_AAAA 0x001C
typedef enum {MDNS_IF_STA, MDNS_IF_AP, MDNS_IF_ETH} mdns_if_t;
typedef enum {MDNS_IP_PROTOCOL_V4, MDNS_IP_PROTOCOL_V6, MDNS_IP_PROTOCOL_MAX} mdns_ip_protocol_t;
struct mdns_ip_addr_t {int address;mdns_ip_addr_t* next;};
struct mdns_service_t {const char* hostname;};
struct mdns_srv_item_t {mdns_srv_item_t* next;mdns_service_t* service;};
struct mdns_host_item_t {const char* hostname;mdns_ip_addr_t* address_list;mdns_host_item_t* next;};
struct mdns_out_answer_t {uint16_t type;mdns_service_t* service;mdns_host_item_t* host;bool flush,bye;mdns_out_answer_t* next;};
struct mdns_tx_packet_t {mdns_if_t tcpip_if;mdns_ip_protocol_t ip_protocol;uint16_t flags;mdns_out_answer_t* answers;};
struct {const char* hostname;mdns_srv_item_t* services;mdns_host_item_t* host_list;} server,*s_server=&server;
struct Sent {int interface,protocol;uint16_t flags;std::vector<std::string> answers;};
std::vector<Sent> sent;
std::vector<std::string> events;
bool ready[MDNS_MAX_INTERFACES][MDNS_IP_PROTOCOL_MAX];
int live_packets=0,live_answers=0,allocations=0,fail_allocation=0,answer_calls=0,fail_answer=0;
bool mdns_utils_str_null_or_empty(const char* value){return !value||!value[0];}
bool mdns_priv_if_ready(mdns_if_t interface,mdns_ip_protocol_t protocol){return ready[interface][protocol];}
mdns_tx_packet_t* mdns_priv_alloc_packet(mdns_if_t interface,mdns_ip_protocol_t protocol){
    if(++allocations==fail_allocation)return nullptr;
    ++live_packets;return new mdns_tx_packet_t{interface,protocol,0,nullptr};
}
bool mdns_priv_create_answer(mdns_out_answer_t** destination,uint16_t type,mdns_service_t* service,mdns_host_item_t* host,bool flush,bool bye){
    if(++answer_calls==fail_answer)return false;
    auto** tail=destination;while(*tail)tail=&(*tail)->next;
    *tail=new mdns_out_answer_t{type,service,host,flush,bye,nullptr};++live_answers;return true;
}
bool listed(mdns_host_item_t* host){for(auto* h=s_server->host_list;h;h=h->next)if(h==host)return true;return false;}
void mdns_priv_dispatch_tx_packet(mdns_tx_packet_t* packet){
    Sent record{packet->tcpip_if,packet->ip_protocol,packet->flags,{}};
    for(auto* a=packet->answers;a;a=a->next){
        // The responder only writes records for a host that is still listed.
        assert(a->host&&listed(a->host)&&a->host->address_list&&!a->service);
        record.answers.push_back(std::string(a->type==MDNS_TYPE_A?"A ":"AAAA ")+a->host->hostname+(a->flush?" flush":"")+(a->bye?" bye":""));
    }
    sent.push_back(record);events.push_back("goodbye");
}
void mdns_priv_free_tx_packet(mdns_tx_packet_t* packet){
    for(auto* a=packet->answers;a;){auto* next=a->next;delete a;--live_answers;a=next;}
    delete packet;--live_packets;
}
void mdns_priv_pcb_send_bye_service(mdns_srv_item_t** services,size_t length,bool include_ip){
    assert(length==1&&!include_ip);events.push_back(std::string("service_bye:")+services[0]->service->hostname);
}
void mdns_priv_remove_scheduled_service_packets(mdns_service_t*){}
void free_service(mdns_service_t* service){free(const_cast<char*>(service->hostname));free(service);}
void mdns_mem_free(void* memory){free(memory);}
void mdns_utils_free_address_list(mdns_ip_addr_t* list){while(list){auto* next=list->next;free(list);list=next;}}
'''

SUITE = r'''
mdns_ip_addr_t* addresses(int count){
    mdns_ip_addr_t* list=nullptr;
    for(int i=0;i<count;++i){auto* a=static_cast<mdns_ip_addr_t*>(malloc(sizeof(mdns_ip_addr_t)));a->address=i;a->next=list;list=a;}
    return list;
}
void add_host(const char* name,int count){
    auto* host=static_cast<mdns_host_item_t*>(malloc(sizeof(mdns_host_item_t)));
    host->hostname=strdup(name);host->address_list=addresses(count);host->next=s_server->host_list;s_server->host_list=host;
    auto* service=static_cast<mdns_service_t*>(malloc(sizeof(mdns_service_t)));service->hostname=strdup(name);
    auto* item=static_cast<mdns_srv_item_t*>(malloc(sizeof(mdns_srv_item_t)));item->service=service;item->next=s_server->services;s_server->services=item;
}
std::vector<std::string> hosts(){std::vector<std::string> out;for(auto* h=s_server->host_list;h;h=h->next)out.push_back(h->hostname);return out;}
std::vector<std::string> services(){std::vector<std::string> out;for(auto* s=s_server->services;s;s=s->next)out.push_back(s->service->hostname);return out;}
void clear(){
    while(s_server->services){auto* s=s_server->services;s_server->services=s->next;free_service(s->service);free(s);}
    while(s_server->host_list){auto* h=s_server->host_list;s_server->host_list=h->next;mdns_utils_free_address_list(h->address_list);free(const_cast<char*>(h->hostname));free(h);}
}
void reset(int old_addresses=2){
    clear();sent.clear();events.clear();memset(ready,0,sizeof(ready));
    assert(!live_packets&&!live_answers);allocations=fail_allocation=answer_calls=fail_answer=0;
    s_server->hostname="1CDBD4451010";ready[MDNS_IF_STA][MDNS_IP_PROTOCOL_V4]=ready[MDNS_IF_STA][MDNS_IP_PROTOCOL_V6]=true;
    add_host("old",old_addresses);add_host("new",2);
}
const std::vector<std::string> goodbye{"A old flush bye","AAAA old flush bye"};
void assert_removed(){
    assert((hosts()==std::vector<std::string>{"new"})&&(services()==std::vector<std::string>{"new"}));
    assert(!live_packets&&!live_answers);
}
int main(){
    // The unchanged managed component forgets the name without telling peers.
    reset();sdk_original("old");assert_removed();
    assert(sent.empty()&&(events==std::vector<std::string>{"service_bye:old"}));
    // Each ready interface/protocol receives TTL-zero address records for the
    // host while it is still listed; service removal is unchanged.
    reset();fixed("old");assert_removed();
    assert((events==std::vector<std::string>{"service_bye:old","goodbye","goodbye"})&&sent.size()==2);
    for(int protocol=0;protocol<2;++protocol){
        assert(sent[protocol].interface==MDNS_IF_STA&&sent[protocol].protocol==protocol);
        assert(sent[protocol].flags==MDNS_FLAGS_QR_AUTHORITATIVE&&sent[protocol].answers==goodbye);
    }
    reset();ready[MDNS_IF_AP][MDNS_IP_PROTOCOL_V4]=true;ready[MDNS_IF_STA][MDNS_IP_PROTOCOL_V6]=false;fixed("OLD");assert_removed();
    assert(sent.size()==2&&sent[0].interface==MDNS_IF_STA&&sent[0].protocol==0&&sent[1].interface==MDNS_IF_AP&&sent[1].protocol==0);
    // Nothing was ever answered for an address-less host, without a responder
    // hostname, or without a ready interface.
    reset(0);fixed("old");assert_removed();assert(sent.empty());
    reset();s_server->hostname="";fixed("old");assert_removed();assert(sent.empty());
    reset();memset(ready,0,sizeof(ready));fixed("old");assert_removed();assert(sent.empty());
    // A failed allocation or record skips only that packet, without leaks.
    reset();fail_allocation=1;fixed("old");assert_removed();assert(sent.size()==1&&sent[0].protocol==1&&sent[0].answers==goodbye);
    reset();fail_answer=2;fixed("old");assert_removed();assert(sent.size()==1&&sent[0].protocol==1&&sent[0].answers==goodbye);
    // An unknown name changes nothing and says nothing.
    reset();fixed("other");assert((hosts()==std::vector<std::string>{"new","old"})&&sent.empty()&&events.empty());
    clear();
    std::puts("PASS actual pinned mDNS: original delegated-host removal sends no address goodbye; generated removal withdraws A/AAAA on every ready interface before freeing the host, preserving service removal and failure cleanup");
}
'''


def main() -> None:
    original = SOURCE.read_text()
    expected = patch.patch_source(original)
    helper = function(original, patch.SIGNATURE)
    corrected = patch.GOODBYE + helper.replace(patch.ORIGINAL, patch.CORRECTED, 1)
    assert expected == original.replace(helper, corrected, 1).replace(patch.INCLUDE, patch.INCLUDE + patch.NETWORKING, 1)
    def edited(old, new):
        assert helper.count(old) == 1
        return original.replace(helper, helper.replace(old, new), 1)
    for unknown in ('', expected, original + patch.SIGNATURE,
                    original.replace(patch.INCLUDE, ''),
                    original.replace(patch.INCLUDE, patch.INCLUDE + patch.NETWORKING),
                    edited('            mdns_mem_free(host);\n', ''),
                    edited('send_bye_service(&srv, 1, false);', 'send_bye_service(&srv, 1, true);'),
                    edited(patch.ORIGINAL, patch.CORRECTED)):
        try:
            patch.patch_source(unknown)
        except ValueError:
            pass
        else:
            raise AssertionError('Generator accepted an unknown mDNS removal preimage')
    decorated = '// unrelated component prefix\n' + original + '\n// unrelated component suffix\n'
    assert patch.patch_source(decorated) == '// unrelated component prefix\n' + expected + '\n// unrelated component suffix\n'
    with tempfile.TemporaryDirectory(prefix='amity-mdns-goodbye-') as directory:
        temp = Path(directory)
        generated, cpp, binary = temp / 'mdns_responder.c', temp / 'test.cpp', temp / 'test'
        subprocess.run(['python3', str(GENERATOR), '--input', str(SOURCE), '--output', str(generated)], check=True)
        assert generated.read_text() == expected and SOURCE.read_text() == original
        in_place = temp / 'in-place.c'
        in_place.write_text(original)
        refused = subprocess.run(['python3', str(GENERATOR), '--input', str(in_place), '--output', str(in_place)],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        assert refused.returncode != 0 and in_place.read_text() == original
        fixed = generated.read_text()
        original_helper = helper.replace(patch.SIGNATURE, 'static bool sdk_original(const char *hostname)', 1)
        goodbye = function(fixed, 'static void send_delegated_host_bye(mdns_host_item_t *host)')
        fixed_helper = function(fixed, patch.SIGNATURE).replace(patch.SIGNATURE, 'static bool fixed(const char *hostname)', 1)
        cpp.write_text(PREFIX + original_helper + goodbye + fixed_helper + SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
