#!/usr/bin/env python3
"""Run actual discovery/admission runtime against deterministic task/radio seams."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
from legacy_fixtures.admission_workflow import PREFIX, RESET
from setup_timing_support import timing_support
from slice_support import function

ROOT = Path(__file__).resolve().parents[1]
SUITE = '\n' + RESET + r'''void run(){auto* arg=task_argument;task_argument=nullptr;invite_worker_work(arg);invitation_running=false;}
void select(){
    if(admission.view.phase!="found")return;
    assert(runtime_add_octopus(ident<model::MessageId>(99),"two","room")==ESP_ERR_INVALID_STATE);
    assert(runtime_add_octopus(ident<model::MessageId>(1),"absent","room")==ESP_ERR_INVALID_ARG);
    assert(runtime_add_octopus(ident<model::MessageId>(1),"two","Invalid Name")==ESP_ERR_INVALID_ARG);
    assert(runtime_add_octopus(ident<model::MessageId>(1),"two","main-amity")==ESP_ERR_INVALID_ARG);
    assert(runtime_add_octopus(ident<model::MessageId>(1),"two","room")==ESP_OK);
    assert(runtime_add_octopus(ident<model::MessageId>(1),"two","room")==ESP_ERR_INVALID_STATE);
}
void cancellation_waits_for_proposal_store(){
    reset();const auto scan=ident<model::MessageId>(1);assert(runtime_scan_admission(scan)==ESP_OK);during_wait=select;
    std::promise<void> entered,release,cancelling;auto released=release.get_future().share();
    during_save=[&]{entered.set_value();released.wait();};
    auto worker=std::async(std::launch::async,[]{run();});
    assert(entered.get_future().wait_for(2s)==std::future_status::ready);
    auto cancel=std::async(std::launch::async,[&]{cancelling.set_value();return runtime_cancel_admission(scan);});cancelling.get_future().wait();
    auto read=std::async(std::launch::async,[]{std::lock_guard<std::mutex> guard(lock);return !group.join_id&&admission.view.phase=="connecting";});
    assert(read.wait_for(2s)==std::future_status::ready&&read.get());
    assert(cancel.wait_for(50ms)==std::future_status::timeout);
    release.set_value();assert(cancel.get()==ESP_ERR_INVALID_STATE);worker.get();
    assert(group.join_id&&group.joining_id==ident<model::OctopusId>(3)&&!invitation_cancelled&&admission.view.phase=="finishing");
    std::cout<<"PASS actual Add cancellation waits for blocked proposal storage and cannot claim to cancel a durable enrollment; runtime readers remain responsive\n";
}
void proposal_record_lifetime(){
    reset();auto local=identity;Peer peer{ident<model::OctopusId>(3),"candidate-leaf",std::string(64,'c'),false};
    const auto bytes=proposal_json(ident<model::JoinId>(90),group.consortium,local,peer,2,"192.0.2.1","room");
    assert(!bytes.empty()&&certificates==0); // No repeat signature or global cache.
    local.der="changed-local";local.spki=std::string(64,'b');peer.der="changed-peer";peer.pin=std::string(64,'d');
    JoinProposal decoded;assert(decode_join_proposal(bytes,decoded));
    assert(decoded.coordinator_leaf=="controller-leaf"&&decoded.coordinator_pin==std::string(64,'a')&&decoded.candidate_leaf=="candidate-leaf"&&decoded.candidate_pin==std::string(64,'c'));
    peer.der="candidate-leaf";peer.pin.clear();
    assert(proposal_json(ident<model::JoinId>(90),group.consortium,identity,peer,2,"192.0.2.1","room").empty());
    peer.pin=identity.spki;assert(proposal_json(ident<model::JoinId>(90),group.consortium,identity,peer,2,"192.0.2.1","room").empty());
}
void roster_capacity(){
    reset();
    for(unsigned n=0;n<2;++n)group.peers.push_back({ident<model::OctopusId>(10+n),"existing-leaf",std::string(64,'e'+n),true});
    assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);
    during_wait=select;run();
    assert(group.peers.size()==3&&saves==1&&wifi_exports==1&&admission.view.phase=="finishing");
    group.join_id={};group.joining_id={};group.join_digest.clear();group.join_hostname.clear();group.join_revision=0;
    assert(runtime_scan_admission(ident<model::MessageId>(2))==ESP_ERR_INVALID_SIZE&&!invitation_running);
    assert(reservations==1&&saves==1&&wifi_exports==1); // A fourth Add never starts radio or transfers credentials.

    reset();assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);
    during_wait=[](){if(admission.view.phase=="found"){
        group.peers.resize(3);
        assert(runtime_add_octopus(ident<model::MessageId>(1),"two","room")==ESP_ERR_INVALID_STATE);
        assert(runtime_cancel_admission(ident<model::MessageId>(1))==ESP_OK);
    }};
    run();assert(!saves&&!wifi_exports&&!connections&&admission.view.phase=="cancelled");

    reset();assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);
    during_wait=select;during_connect=[](){group.peers.resize(3);};run();
    assert(!saves&&!wifi_exports&&group.peers.size()==3&&admission.view.phase=="failed");
    std::cout<<"PASS three secondary Amities admitted; fourth rejected before scan, submission and prepared membership storage\n";
}
void rename_name_reservation(){
    reset();invitation_running=true;admission.view.scan_id=ident<model::MessageId>(1);admission.view.phase="found";admission.deadline=1000;admission.view.candidates={{"two","Amity-654321"}};
    const auto owner=ident<model::OctopusId>(3);group.peers.push_back({owner,"leaf","pin",true});{std::lock_guard<std::mutex> persistence(persistence_lock);std::unique_lock<std::mutex> guard(lock);assert(remember_hostname(owner,"old-room",guard)==Error::none);}
    auto active=std::make_shared<Active>();active->peer=owner;active->ready=active->status.ready=true;active->status.hostname="live-room";links.push_back(active);
    assert(runtime_add_octopus(ident<model::MessageId>(1),"two","live-room")==ESP_ERR_INVALID_ARG); // Live authenticated name wins if its metadata storage failed.
    DeviceSettingsWrite rename;rename.expected_boot=ident<model::BootId>(7);rename.expected_hostname_revision=2;rename.hostname="room";
    settings_operations.push_back({owner,device_settings_write_json(rename),"admitted",500,rename.hostname});
    assert(runtime_add_octopus(ident<model::MessageId>(1),"two","room")==ESP_ERR_INVALID_ARG&&admission.view.phase=="found");
    assert(!connections&&!saves&&!wifi_exports&&!disconnects); // A rejected conflicting Add has no networking effect.
    clock_ms=500;assert(runtime_add_octopus(ident<model::MessageId>(1),"two","room")==ESP_OK&&admission.submitted);
    assert(settings_operations.front().stage=="unknown");
    std::cout<<"PASS Add preserves hostname uniqueness against current live names and admitted renames before any network or enrollment work\n";
}
int run_tests(){
    rename_name_reservation();
    proposal_record_lifetime();
    roster_capacity();
    {
        reset();assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);
        assert(runtime_admission_status().phase=="scanning");
        assert(runtime_scan_admission(ident<model::MessageId>(2))==ESP_ERR_INVALID_STATE);
        during_wait=select;run();assert(discoveries==2&&reservations==1&&releases==1&&wifi_exports==1&&encrypted_exchanges==3&&saves==1);
        assert(connections==1&&handshakes==1);
        assert(early_candidate_published&&candidate_snapshots>=2&&clock_ms<21000); // Later candidate remained selectable without another full scan.
        assert(certificates==1); // Verify the untrusted candidate once, reuse exact validated records.
        for(const char* stage:{"worker_selected","selected_total","gatt_connect","crypto_handshake","identify_rpc","peer_validation","proposal_store","prepare_rpc","prepare_ack","wifi_rpc","finishing"}){
            const auto found=std::find_if(platform::recorded_setup_timings.begin(),platform::recorded_setup_timings.end(),[&](const auto& row){return std::string(row.area)=="invite"&&std::string(row.entry.stage)==stage;});
            assert(found!=platform::recorded_setup_timings.end()&&std::string(found->entry.result)=="ok"&&found->scan==ident<model::MessageId>(1)&&found->peer==ident<model::OctopusId>(3));
        }
        assert(admission.view.phase=="finishing"&&group.joining_id==ident<model::OctopusId>(3)&&hostnames.lookup(group.joining_id)=="room");
        assert(runtime_cancel_admission(ident<model::MessageId>(1))==ESP_ERR_INVALID_STATE);
        auto link=std::make_shared<Active>();links={link};admission.view.error="The result was not confirmed yet.";
        for(unsigned failure=0;failure<5;++failure){
            link->peer=group.joining_id;link->ready=link->status.ready=true;link->cancelled=false;link->status.hostname="room";
            switch(failure){case 0:link->peer=ident<model::OctopusId>(99);break;case 1:link->ready=false;break;case 2:link->status.ready=false;break;case 3:link->cancelled=true;break;case 4:link->status.hostname="wrong";break;}
            const auto pending=runtime_admission_status();assert(pending.phase=="finishing"&&!pending.error.empty()&&admission.view.phase=="finishing");
        }
        link->status.hostname="room";assert(runtime_admission_status().phase=="complete"&&admission.view.phase=="complete"&&admission.view.error.empty());
        const auto completed=runtime_admission_status();
        link->cancelled=true;link->ready=link->status.ready=false;links.clear();group.peers.clear();group.join_id={};group.joining_id={};group.join_digest.clear();group.join_hostname.clear();group.join_revision=0;
        const auto removed=runtime_admission_status();assert(removed.phase=="complete"&&removed.error.empty()&&removed.candidate_id==completed.candidate_id&&removed.scan_id==completed.scan_id);
        assert(runtime_scan_admission(ident<model::MessageId>(2))==ESP_OK);
        const auto next=runtime_admission_status();assert(next.phase=="scanning"&&next.scan_id==ident<model::MessageId>(2)&&!next.candidate_id&&next.error.empty());
        std::cout<<"PASS actual discovery-before-button, exact selection/name, encrypted setup, membership and readiness\n";
    }
    reset();assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);assert(runtime_cancel_admission(ident<model::MessageId>(2))==ESP_ERR_INVALID_STATE);assert(!invitation_cancelled);
    assert(runtime_cancel_admission(ident<model::MessageId>(1))==ESP_OK);run();assert(reservations==0&&saves==0&&wifi_exports==0&&admission.view.phase=="cancelled");
    reset();assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);during_wait=[](){if(admission.view.phase=="found")assert(runtime_cancel_admission(ident<model::MessageId>(1))==ESP_OK);};run();assert(releases==1&&saves==0&&encrypted_exchanges==0&&connections==0);
    reset();discover_ok=false;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);run();assert(admission.view.phase=="failed"&&releases==1&&saves==0&&wifi_exports==0&&discoveries<=9);
    reset();bad_identity=true;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);during_wait=select;run();assert(admission.view.phase=="failed"&&saves==0&&wifi_exports==0);
    reset();bad_certificate=true;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);during_wait=select;run();assert(certificates==1&&admission.view.phase=="failed"&&saves==0&&wifi_exports==0);
    reset();networked=true;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);during_wait=select;run();assert(admission.view.phase=="finishing"&&wifi_exports==0&&encrypted_exchanges==2);
    for(const std::string phase:{"prepare","wifi"}){
        reset();fail_phase=phase;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);during_wait=select;run();
        assert(saves==1&&group.join_id&&admission.view.phase=="finishing"&&!admission.view.error.empty());
        auto link=std::make_shared<Active>();link->peer=group.joining_id;link->ready=link->status.ready=true;link->status.hostname="room";links={link};
        assert(runtime_admission_status().phase=="complete"&&runtime_admission_status().error.empty());
        links.clear();assert(runtime_admission_status().phase=="complete"&&admission.view.error.empty());
    }
    reset();spawn_ok=false;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_ERR_NO_MEM);assert(!invitation_running&&admission.view.phase=="failed");
    reset();group.role=Group::member;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_ERR_INVALID_STATE&&!invitation_running);
    for(unsigned unavailable=0;unavailable<2;++unavailable){reset();if(unavailable)status.credential_ready=false;else status.initialized=false;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_ERR_INVALID_STATE&&!invitation_running);}
    std::cout<<"PASS actual stale IDs/tokens, invalid inputs, pre-scan/found cancellation, timeout, identity mismatch, configured network preservation and task/role rejection; exact readiness latches transient completion across removal until a new scan\n";
    cancellation_waits_for_proposal_store();
    return 0;
}
}
int main(){return amity::consortium::run_tests();}
'''


def main():
    source = (ROOT / 'components/consortium/runtime.cpp').read_text()
    names = ['std::string proposal_json(', 'void admission_failed(', 'void invite_worker_work(', 'RuntimeAdmissionStatus runtime_admission_status(',
             'esp_err_t runtime_scan_admission(', 'esp_err_t runtime_cancel_admission(', 'esp_err_t runtime_add_octopus(']
    actual = function((ROOT / 'components/consortium/settings_runtime.inc').read_text(), 'bool settings_hostname_pending(') + ''.join(function(source, name) for name in names)
    with tempfile.TemporaryDirectory(prefix='amity-admission-workflow-') as directory:
        cpp = Path(directory) / 'test.cpp'
        binary = Path(directory) / 'test'
        cpp.write_text(timing_support() + PREFIX + actual + SUITE)
        include = ['tests/reset_fakes', 'components/consortium', 'components/consortium/include',
                   'components/amity_system/include', 'components/device_settings/include']
        sources = sorted((ROOT / 'components/consortium').glob('*.cpp')) + [ROOT / 'components/amity_system/model.cpp']
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) +
                       ['-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined'] +
                       ['-I' + str(ROOT / path) for path in include] +
                       [str(cpp)] + [str(path) for path in sources] + ['-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
