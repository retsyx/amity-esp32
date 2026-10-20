#include "system_wire.hpp"
#include <cassert>
#include <cstdio>
#include <iostream>
#include <deque>
using namespace amity::system;
template<class T>T hold_id(unsigned n){char text[33];std::snprintf(text,sizeof(text),"%032x",n);return *T::parse(text);}
struct HoldFixture {
    CecPortRef port{hold_id<ConsortiumId>(1),hold_id<OctopusId>(2),0,Side::back};
    Component audio{hold_id<ComponentId>(3),"Receiver",{{port,"Receiver"}}};
    Component display{hold_id<ComponentId>(4),"Display",{{port,"Display"}}};
    ComponentBinding binding;
    PortPolicySnapshot policy;
    HoldFixture(){
        binding.binding_id=hold_id<BindingId>(5);binding.component_id=audio.component_id;binding.capability=Capability::volume;
        binding.device_name=audio.qualified_names.front();binding.endpoint={port,5};binding.owner_octopus_id=port.octopus_id;
        binding.domain_id=hold_id<DomainId>(6);binding.domain_revision=binding.binding_revision=1;binding.role=PortRole::tv_controller;
        binding.status=BindingStatus::verified;binding.physical_address_evidence_ref=hold_id<EvidenceId>(7);
        policy.port=port;policy.owner_boot_id=hold_id<BootId>(8);policy.domain_id=binding.domain_id;policy.domain_revision=1;
        policy.coordinator_epoch=1;policy.config_revision=1;policy.policy_generation=1;policy.authority_expires_at_ms=100000;
        policy.role=PortRole::tv_controller;policy.owned_logical_address=0;policy.components={audio,display};policy.bindings={{binding}};
        policy.route_grants={{binding.binding_id,hold_id<SessionId>(9),1,100000,{}}};
    }
    AuthorizedCecOperation press(unsigned token=10,bool repeat=true,uint64_t deadline=100) const{
        AuthorizedCecOperation op;op.command_id=hold_id<CommandId>(token+100);op.kind=OperationKind::press_key;
        op.component_id=audio.component_id;op.binding_id=binding.binding_id;op.target=binding.endpoint;op.execution_expires_at_ms=deadline;
        op.fence={policy.owner_boot_id,1,1,1,1,hold_id<SessionId>(9),1,policy.policy_generation};
        op.arguments.key=0x41;op.arguments.hold_token=hold_id<HoldToken>(token);op.arguments.repeat=repeat;return op;
    }
    Topology topology() const{
        Topology t;t.config_revision=1;t.components={audio,display};t.bindings={binding};t.ready_owners={port.octopus_id};
        const SignalPort output{audio.component_id,0,SignalDirection::output},input{display.component_id,0,SignalDirection::input};
        t.signal_edges={{output,input,SignalEdgeKind::hdmi_cable,Support::supported}};
        t.resources={{hold_id<ResourceId>(11),port.octopus_id,ResourceCompatibility::exclusive,{"active"}}};
        RouteDefinition route;route.route_id=hold_id<RouteId>(12);route.source=audio.component_id;route.display=display.component_id;
        route.physical_path={output,input};route.required_bindings={binding.binding_id};route.demands={{t.resources.front().resource_id,"active",{}}};
        route.steps={{binding.binding_id,OperationKind::query_state,{},{},false}};t.routes={route};
        t.activities={{hold_id<ActivityId>(13),hold_id<ZoneId>(14),route.route_id}};return t;
    }
};

void driver_fifo_only(){
    HoldFixture f;LocalCecAgent owner;assert(owner.install_policy(f.policy,0));auto query=f.press();query.kind=OperationKind::query_state;query.execution_expires_at_ms=10000;
    assert(!owner.execute(query,10)&&owner.pending_work()==0);TestWire driver;driver.bind(owner);
    for(unsigned n=0;n<8;++n){query.command_id=hold_id<CommandId>(200+n);assert(owner.execute(query,10));}
    assert(driver.fifo.size()==8&&owner.pending_work()==8);query.command_id=hold_id<CommandId>(208);assert(!owner.execute(query,10));
    assert(!owner.execute(f.press(19,false,10000),10)&&driver.fifo.size()==8); // Atomic batch cannot put its ordinary press into slot nine.
    for(unsigned n=0;n<8;++n){auto work=driver.take(20+n);assert(work&&work->command_id==hold_id<CommandId>(200+n));driver.complete(true,20+n);}
    owner.tick(100);assert(driver.fifo.empty()&&owner.pending_work()==0); // Rejected ordinary work is never buffered/retried.
    for(unsigned n=0;n<7;++n){query.command_id=hold_id<CommandId>(300+n);assert(owner.execute(query,110));}
    auto tap=f.press(20,false,10000);assert(owner.execute(tap,110)&&driver.fifo.size()==9); // Eighth ordinary UCP plus the one extra release slot, atomically.
    auto extra=f.press(22,false,10000);assert(!owner.execute(extra,112));
    for(unsigned n=0;n<7;++n){assert(driver.take(120+n)->frame.payload[0]==0x8f);driver.complete(true,120+n);}
    auto press=driver.take(130);assert(press&&press->frame.payload[0]==0x44&&press->attempts==1);driver.complete(WireOutcome::nack,131);
    auto release=driver.take(132);assert(release&&release->emergency_release&&release->attempts==3&&release->requires_previous_start);driver.complete(true,133);
    assert(owner.held_keys()==0);
    LocalCecAgent expired;TestWire late;late.bind(expired);assert(expired.install_policy(f.policy,0));tap=f.press(21,false,100);
    assert(expired.execute(tap,10)&&late.fifo.size()==2);expired.tick(600,false);assert(late.fifo.size()==2);
    assert(!late.take(700)&&late.emitted.empty()&&expired.pending_work()==0); // Timer cannot admit a query while asynchronous prestart cancellation has not yet reached the owner.
    LocalCecAgent delayed;TestWire delayed_wire;delayed_wire.bind(delayed);assert(delayed.install_policy(f.policy,0));
    assert(delayed.execute(tap,10));assert(delayed_wire.take(11)->frame.payload[0]==0x44);
    delayed.tick(600,false);assert(delayed_wire.fifo.size()==1);delayed_wire.complete(true,650);delayed.tick(650,false);
    assert(delayed_wire.fifo.size()==2&&delayed_wire.fifo.back().frame.payload[0]==0x71); // Confirming start retains the original +500ms due time.
    assert(delayed_wire.take(651)->emergency_release);delayed_wire.complete(true,652);assert(delayed_wire.take(653)->frame.payload[0]==0x71);delayed_wire.complete(true,654);
    auto power_policy=f.policy;power_policy.bindings.front().binding.capability=Capability::power;power_policy.bindings.front().discrete_power_on=true;
    LocalCecAgent power;TestWire power_driver;power_driver.bind(power);assert(power.install_policy(power_policy,0));auto op=f.press();op.kind=OperationKind::ensure_power;op.arguments={};
    assert(power.execute(op,10)&&power_driver.fifo.size()==2);assert(power_driver.take(11)->attempts==1);power_driver.complete(true,12);
    assert(power_driver.take(13)->attempts==3);power_driver.complete(true,14);
}
void successful_hold_and_exact_up(){
    HoldFixture f;LocalCecAgent owner;TestWire driver;driver.bind(owner);assert(owner.install_policy(f.policy,0));auto down=f.press();down.arguments.key=1;
    assert(owner.execute(down,10)&&driver.fifo.size()==1);assert(driver.take(11));
    for(uint64_t now=20;now<1000;++now){owner.tick(now);assert(driver.fifo.empty()&&owner.pending_work()==1);}
    driver.complete(true,1000);
    for(uint64_t now=1100;now<8000;now+=130){owner.tick(now);auto repeat=driver.take(now);assert(repeat&&repeat->attempts==1&&repeat->expires_at_ms==100000);assert(driver.fifo.empty());driver.complete(true,now+1);}
    auto query=down;query.command_id=hold_id<CommandId>(200);query.kind=OperationKind::query_state;query.execution_expires_at_ms=10000;
    assert(owner.execute(query,8000));owner.tick(8000,false);assert(driver.fifo.size()==1);assert(driver.take(8001)->frame.payload[0]==0x8f);driver.complete(true,8002);
    owner.tick(8003);assert(driver.fifo.size()==1);assert(owner.release_hold(*down.arguments.hold_token,8004));assert(driver.fifo.size()==1);
    assert(!driver.take(8005)); // Only the queued repeat was canceled; terminal driver notification retains the stream boundary.
    owner.tick(8005,false);auto release=driver.take(8005);assert(release&&release->emergency_release);driver.complete(true,8006);
    auto next=f.press(20,true,10000);next.arguments.key=1;assert(owner.execute(next,8010));assert(!owner.release_hold(*down.arguments.hold_token,8010));assert(driver.take(8011));
    assert(owner.release_hold(*next.arguments.hold_token,8012));owner.tick(8012,false);assert(driver.fifo.empty()); // Initial press must finish before final release.
    driver.complete(true,8013);owner.tick(8013,false);assert(driver.take(8014)->emergency_release);driver.complete(true,8015);
    LocalCecAgent short_tap;TestWire short_driver;short_driver.bind(short_tap);assert(short_tap.install_policy(f.policy,0));
    auto short_down=f.press(30,true,100);assert(short_tap.execute(short_down,10));assert(short_tap.release_hold(*short_down.arguments.hold_token,11));
    assert(short_driver.fifo.size()==1&&short_driver.take(12)->frame.payload[0]==0x44);short_driver.complete(true,13);short_tap.tick(13,false);
    assert(short_driver.take(14)->emergency_release);short_driver.complete(true,15);
}
void failed_hold_ends_stream(){
    HoldFixture f;
    for(auto outcome:{WireOutcome::nack,WireOutcome::arbitration_lost,WireOutcome::line_error,WireOutcome::canceled,WireOutcome::expired})for(bool first:{true,false}){
        LocalCecAgent owner;TestWire driver;driver.bind(owner);assert(owner.install_policy(f.policy,0));auto down=f.press();down.arguments.key=1;
        assert(owner.execute(down,10));assert(driver.take(11));
        if(!first){driver.complete(true,12);owner.tick(200);assert(driver.take(200));}
        driver.complete(outcome,201);assert(owner.held_keys()==0&&owner.active_key_contexts().empty()&&!owner.repeat_ready(202)&&driver.fifo.empty());
        owner.tick(202,false);auto release=driver.take(203);assert(release&&release->emergency_release&&release->frame.payload==std::vector<uint8_t>{0x45});driver.complete(true,204);
        assert(!owner.release_hold(*down.arguments.hold_token,205)); // Failure forgot the key; token-up does not generate another release.
        for(uint64_t now=206;now<4000;now+=100){owner.tick(now);assert(driver.fifo.empty());}
        auto fresh=down;fresh.command_id=hold_id<CommandId>(500);fresh.execution_expires_at_ms=5000;assert(owner.renew_authority(fresh,4000));
        auto refreshed=f.policy;++refreshed.policy_generation;assert(owner.install_policy(refreshed,4001));owner.tick(4001);assert(driver.fifo.empty());
        auto next=f.press(20,true,6000);next.arguments.key=1;next.fence.policy_generation=refreshed.policy_generation;
        assert(owner.execute(next,4010));assert(driver.take(4011)->frame.payload[0]==0x44);
    }
    LocalCecAgent never_started;TestWire late;late.bind(never_started);assert(never_started.install_policy(f.policy,0));auto down=f.press();down.arguments.key=1;
    assert(never_started.execute(down,10));assert(!late.take(100));assert(never_started.held_keys()==0);never_started.tick(101);assert(late.fifo.empty()&&never_started.pending_work()==0);
    assert(!never_started.release_hold(*down.arguments.hold_token,102)); // No first edge means no UCR.
    for(bool established:{false,true}){
        LocalCecAgent rejected;TestWire wire;wire.bind(rejected);assert(rejected.install_policy(f.policy,0));auto key=f.press();key.arguments.key=1;
        assert(rejected.execute(key,10));assert(wire.take(11));
        if(established){wire.complete(true,12);rejected.tick(13);assert(wire.take(14));}
        wire.complete(WireOutcome::admission_rejected,15,false);rejected.tick(16,false);
        if(established){assert(wire.take(17)->emergency_release);wire.complete(true,18);}else assert(wire.fifo.empty());
        assert(rejected.held_keys()==0&&!rejected.release_hold(*key.arguments.hold_token,19));
    }
}
void repeat_busy_and_already_received_up(){
    HoldFixture f;LocalCecAgent owner;TestWire driver;driver.bind(owner);assert(owner.install_policy(f.policy,0));auto down=f.press();down.arguments.key=1;
    assert(owner.execute(down,10));assert(driver.take(11));driver.complete(true,12);driver.admitting=false;owner.tick(200);
    assert(owner.held_keys()==0&&driver.fifo.empty());driver.admitting=true;owner.tick(201,false);assert(driver.take(201)->emergency_release);driver.complete(true,202);
    assert(!owner.release_hold(*down.arguments.hold_token,203));
    auto next=f.press(20,true,1000);next.arguments.key=1;assert(owner.execute(next,210));assert(driver.take(211));assert(owner.release_hold(*next.arguments.hold_token,212));
    owner.tick(212,false);assert(driver.fifo.empty());driver.complete(WireOutcome::nack,213);owner.tick(213,false);
    assert(driver.take(214)->emergency_release);driver.complete(true,215);assert(owner.held_keys()==0);
}
void full_driver_release_slot_and_drop(){
    HoldFixture f;LocalCecAgent owner;TestWire driver;driver.bind(owner);assert(owner.install_policy(f.policy,0));auto down=f.press();down.arguments.key=1;
    assert(owner.execute(down,10));assert(driver.take(11));driver.complete(true,12);
    auto query=down;query.kind=OperationKind::query_state;query.execution_expires_at_ms=10000;
    for(unsigned n=0;n<8;++n){query.command_id=hold_id<CommandId>(200+n);assert(owner.execute(query,20));}
    assert(owner.release_hold(*down.arguments.hold_token,21));assert(driver.fifo.size()==8);owner.tick(22,false);
    assert(driver.fifo.size()==9&&driver.fifo.back().emergency_release&&driver.fifo.back().allow_overflow);
    auto next=f.press(20,true,1000);next.arguments.key=1;assert(!owner.execute(next,23)); // Ordinary work cannot use the ninth release slot.
    for(unsigned n=0;n<2;++n){assert(driver.take(30+n)->command_id==hold_id<CommandId>(200+n));driver.complete(true,30+n);}
    assert(owner.execute(next,32)); // Old admitted UCR stays before the new positive in the single FIFO.
    for(unsigned n=2;n<8;++n){assert(driver.take(40+n)->command_id==hold_id<CommandId>(200+n));driver.complete(true,40+n);}
    assert(driver.take(50)->emergency_release);driver.complete(true,51);assert(driver.take(52)->frame.payload[0]==0x44);driver.complete(true,53);
    assert(owner.held_keys()==1);assert(owner.release_hold(*next.arguments.hold_token,54));owner.tick(54,false);assert(driver.take(55)->emergency_release);driver.complete(true,56);
    // Another driver producer can occupy the extra slot. Dropped final release is counted, consumed, and never retried.
    auto again=f.press(30,true,1000);again.arguments.key=1;assert(owner.execute(again,60));assert(driver.take(61));driver.complete(true,62);
    auto external=driver.emitted.back();external.command_id={};external.work_id=100000;external.expires_at_ms=0;
    for(unsigned n=0;n<8;++n){++external.work_id;assert(driver.submit(&external,1));}external.allow_overflow=true;assert(driver.submit(&external,1));
    assert(owner.release_hold(*again.arguments.hold_token,63));owner.tick(64,false);assert(owner.pending_work()==0&&owner.uncertain_releases()==1&&driver.fifo.size()==9);
    driver.fifo.clear();owner.tick(65,false);assert(driver.fifo.empty()&&!owner.release_hold(*again.arguments.hold_token,66));
    // An observed end behind a still-running initial press cannot be erased by a newer down.
    assert(owner.execute(next,70));assert(driver.take(71));assert(owner.release_hold(*next.arguments.hold_token,72));
    assert(!owner.execute(again,73));driver.complete(true,74);assert(owner.execute(again,75));
    assert(driver.take(76)->emergency_release);driver.complete(true,77);assert(driver.take(78)->frame.payload[0]==0x44);driver.complete(true,79);
}
void aggregate_keys_and_taps(){
    HoldFixture f;LocalCecAgent owner;TestWire driver;driver.bind(owner);assert(owner.install_policy(f.policy,0));
    auto a=f.press(10,true,1000),b=f.press(20,true,1000);a.arguments.key=1;b.arguments.key=2;
    assert(owner.execute(a,10));assert(owner.execute(b,10));assert(driver.take(11)->frame.payload[1]==1);driver.complete(true,12);
    assert(driver.take(13)->frame.payload[1]==2);driver.complete(true,14);assert(owner.held_keys()==2&&owner.active_key_contexts().size()==2);
    assert(owner.release_hold(*a.arguments.hold_token,15));owner.tick(15,false);assert(driver.fifo.empty()&&owner.held_keys()==1&&owner.active_key_contexts().front().command_id==b.command_id);
    auto tap=f.press(30,false,1000);tap.arguments.key=3;assert(owner.execute(tap,16)&&driver.fifo.size()==1); // Joining finite tap cannot append UCR while B is down.
    assert(driver.take(17)->frame.payload[1]==3);driver.complete(true,18);owner.tick(18,false);assert(driver.fifo.empty()&&owner.held_keys()==1);
    assert(owner.release_hold(*b.arguments.hold_token,19));assert(driver.fifo.empty());owner.tick(19,false);assert(driver.take(20)->emergency_release);driver.complete(true,21);
    assert(owner.held_keys()==0&&!owner.release_hold(*a.arguments.hold_token,22)&&!owner.release_hold(*b.arguments.hold_token,22));
    // Failure of A ends only A. B's accepted press must finish before a final release, even if B never reaches its first edge.
    assert(owner.execute(a,30));assert(owner.execute(b,30));assert(driver.take(31));driver.complete(WireOutcome::nack,32);owner.tick(32,false);
    assert(driver.fifo.size()==1&&owner.held_keys()==1);assert(owner.release_hold(*b.arguments.hold_token,33));
    // Initial B remains in FIFO; expire it before start. Sticky A-started state still requires final UCR.
    assert(!driver.take(1000));owner.tick(1001,false);assert(driver.take(1002)->emergency_release);driver.complete(true,1003);
    assert(!owner.release_hold(*a.arguments.hold_token,1004)&&owner.held_keys()==0);
}
void aggregate_failure_and_independent_targets(){
    HoldFixture f;auto second=f.binding;second.binding_id=hold_id<BindingId>(15);second.endpoint.logical_address=4;
    second.device_name=QualifiedDeviceName{f.port,"Player"};auto component=f.audio;component.component_id=hold_id<ComponentId>(16);component.qualified_names={*second.device_name};second.component_id=component.component_id;
    auto policy=f.policy;policy.components.push_back(component);policy.bindings.push_back({second});policy.route_grants.push_back({second.binding_id,hold_id<SessionId>(9),1,100000,{}});
    LocalCecAgent owner;TestWire driver;driver.bind(owner);assert(owner.install_policy(policy,0));auto a=f.press(10,true,1000),b=f.press(20,true,1000);a.arguments.key=1;b.arguments.key=2;
    assert(owner.execute(a,10)&&owner.execute(b,10));assert(driver.take(11));driver.complete(WireOutcome::nack,12);owner.tick(12,false);
    assert(driver.take(13)->frame.payload[1]==2);driver.complete(true,14);owner.tick(14,false);assert(driver.fifo.empty()&&owner.held_keys()==1); // Failure A cannot end B's stream.
    auto c=f.press(30,true,1000);c.arguments.key=3;c.component_id=component.component_id;c.binding_id=second.binding_id;c.target=second.endpoint;
    assert(owner.execute(c,15));assert(driver.take(16)->frame.destination==4);driver.complete(true,17);
    assert(owner.release_hold(*b.arguments.hold_token,18));owner.tick(18,false);auto release=driver.take(19);assert(release&&release->emergency_release&&release->frame.destination==5);driver.complete(true,20);
    owner.tick(21);assert(driver.take(22)->frame.destination==4);driver.complete(WireOutcome::line_error,23);owner.tick(23,false);release=driver.take(24);assert(release&&release->emergency_release&&release->frame.destination==4);driver.complete(true,25);
    assert(owner.held_keys()==0&&!owner.release_hold(*c.arguments.hold_token,26)&&!owner.release_hold(*a.arguments.hold_token,26));
}
void audio_due_and_fences(){
    HoldFixture f;
    for(uint8_t key:{0x41,0x42,0x43,0x65,0x66}){
        LocalCecAgent owner;TestWire driver;driver.bind(owner);assert(owner.install_policy(f.policy,0));auto down=f.press();down.arguments.key=key;
        assert(owner.execute(down,10));assert(driver.take(11));driver.complete(true,12);assert(owner.release_hold(*down.arguments.hold_token,100));assert(driver.fifo.empty());
        owner.tick(100,false);owner.tick(599,false);assert(driver.fifo.size()==1);owner.tick(600,false);assert(driver.fifo.size()==2); // Due from actual UCR admission, never completion.
        assert(driver.take(700)->emergency_release);driver.complete(true,701);assert(driver.take(702)->frame.payload==std::vector<uint8_t>{0x71});driver.complete(WireOutcome::nack,703);
        owner.tick(1300);assert(driver.fifo.empty());
    }
    LocalCecAgent busy;TestWire driver;driver.bind(busy);assert(busy.install_policy(f.policy,0));auto down=f.press();assert(busy.execute(down,10));assert(driver.take(11));driver.complete(true,12);
    assert(busy.release_hold(*down.arguments.hold_token,100));busy.tick(100,false);assert(driver.take(101)->emergency_release);driver.complete(true,102);
    driver.admitting=false;busy.tick(600);driver.admitting=true;busy.tick(1099);assert(driver.fifo.empty());busy.tick(1100);assert(driver.take(1100)->frame.payload[0]==0x71);driver.complete(true,1101);
    for(bool unsafe:{false,true}){
        LocalCecAgent fenced;TestWire wire;wire.bind(fenced);assert(fenced.install_policy(f.policy,0));assert(fenced.execute(down,10));assert(wire.take(11));
        if(unsafe)fenced.fence_binding(f.binding.binding_id,1,false);else fenced.close_route(hold_id<SessionId>(9),1);
        assert(fenced.pending_work()==1&&!fenced.quiescent(hold_id<SessionId>(9),1));wire.complete(true,12);fenced.tick(12,false);
        if(unsafe)assert(wire.fifo.empty());else{assert(wire.take(13)->emergency_release);wire.complete(true,14);}
        assert(fenced.quiescent(hold_id<SessionId>(9),1));
    }
    LocalCecAgent failed;TestWire stale;stale.bind(failed);assert(failed.install_policy(f.policy,0));assert(failed.execute(down,10));assert(stale.take(11));stale.complete(WireOutcome::nack,12);
    failed.fence_binding(f.binding.binding_id,1,false);failed.tick(13,false);assert(stale.fifo.empty()&&failed.pending_work()==0); // Failed key was forgotten, but binding reuse still fences retained cleanup.
}
void policy_updates_do_not_dispatch(){
    HoldFixture f;LocalCecAgent owner;TestWire driver;driver.bind(owner);assert(owner.install_policy(f.policy,0));auto down=f.press();
    assert(owner.execute(down,10));assert(driver.take(11));driver.complete(true,12);
    assert(owner.release_hold(*down.arguments.hold_token,100));assert(driver.fifo.empty()&&owner.held_keys()==0);
    auto invalid=f.policy;invalid.policy_generation=0;assert(!owner.install_policy(invalid,200));assert(driver.fifo.empty());
    assert(owner.renew_discovery_authority(f.port,1000,200));assert(driver.fifo.empty());
    auto fresh=down;fresh.kind=OperationKind::query_state;fresh.execution_expires_at_ms=1000;assert(owner.renew_authority(fresh,200));assert(driver.fifo.empty());
    auto next=f.policy;++next.policy_generation;assert(owner.install_policy(next,200));assert(driver.fifo.empty());
    owner.tick(200,false);assert(driver.take(201)->emergency_release);driver.complete(true,202);
    fresh.fence.policy_generation=next.policy_generation;assert(owner.renew_authority(fresh,700));assert(driver.fifo.empty());
    assert(!owner.install_policy(invalid,700));assert(driver.fifo.empty());owner.tick(700,false); // A stale due query cannot cross a policy generation change.
    assert(driver.fifo.empty());
    LocalCecAgent removed;TestWire removal;removal.bind(removed);assert(removed.install_policy(f.policy,0));assert(removed.execute(down,10));assert(removal.take(11));removal.complete(true,12);
    auto without=f.policy;++without.policy_generation;without.route_grants.clear();assert(removed.install_policy(without,20));assert(removal.fifo.empty());
    removed.tick(21,false);assert(removal.take(22)->emergency_release);removal.complete(true,23);
}
void all_lane_policy_preflight(){
    HoldFixture a,b;b.port.tentacle_id=1;b.audio.component_id=hold_id<ComponentId>(33);b.display.component_id=hold_id<ComponentId>(34);
    b.audio.qualified_names.front().port=b.port;b.display.qualified_names.front().port=b.port;
    b.binding.binding_id=hold_id<BindingId>(35);b.binding.component_id=b.audio.component_id;b.binding.device_name=b.audio.qualified_names.front();
    b.binding.endpoint.port=b.port;b.binding.domain_id=hold_id<DomainId>(36);
    b.policy.port=b.port;b.policy.domain_id=b.binding.domain_id;b.policy.bindings={{b.binding}};b.policy.components={b.audio,b.display};b.policy.route_grants.clear();
    auto first=a.policy;first.route_grants.clear();auto topology=a.topology();topology.components.push_back(b.audio);topology.components.push_back(b.display);topology.bindings.push_back(b.binding);
    uint8_t random=1;Coordinator owner([&](uint8_t* out,size_t n){for(size_t i=0;i<n;++i)out[i]=random++;return true;});TestWire front,back;front.bind(owner,a.port);back.bind(owner,b.port);
    assert(owner.install(topology,{first,b.policy},a.port.octopus_id,0));
    auto query_a=a.press();query_a.kind=OperationKind::query_state;query_a.execution_expires_at_ms=1000;
    auto query_b=b.press();query_b.command_id=hold_id<CommandId>(611);query_b.kind=OperationKind::query_state;query_b.execution_expires_at_ms=1000;
    assert(owner.execute_authorized(query_a,10));assert(owner.execute_authorized(query_b,10));
    auto inactive=b.policy;inactive.role=PortRole::monitor;inactive.owned_logical_address.reset();inactive.bindings.clear();
    auto changed=topology;changed.bindings.back().status=BindingStatus::unresolved;
    auto rejected=owner.refresh_bindings(changed,{first,inactive},20);assert(!rejected);
    assert(owner.policies()[0].policy_generation==1&&owner.policies()[1].policy_generation==1&&front.canceled.empty());
    // A failed later identity change must not install/cancel the earlier lane.
    auto first_frame=front.take(21);assert(first_frame&&first_frame->command_id==query_a.command_id);front.complete(true,22);
    assert(!back.take(23)); // The independently known lost binding was fenced, but its completion was retained until now.
    assert(owner.refresh_bindings(changed,{first,inactive},24));
    assert(owner.policies()[0].policy_generation==2&&owner.policies()[1].policy_generation==2&&owner.policies()[1].role==PortRole::monitor);
    auto fresh=query_a;fresh.command_id=hold_id<CommandId>(900);fresh.fence.policy_generation=2;assert(owner.execute_authorized(fresh,25));assert(front.take(26));front.complete(true,27);
}
void actual_coordinator(){
    HoldFixture f;uint8_t random=1;Coordinator owner([&](uint8_t* data,size_t n){for(size_t i=0;i<n;++i)data[i]=random++;return true;});TestWire driver;driver.bind(owner,f.port);
    auto base=f.policy;base.route_grants.clear();assert(owner.install(f.topology(),{base},f.port.octopus_id,0));
    SelectRequest select{hold_id<MessageId>(30),hold_id<ZoneId>(14),hold_id<ActivityId>(13),hold_id<SessionId>(31),hold_id<PlanId>(32),{}};
    assert(owner.select(select,10).plan);assert(driver.take(11)->frame.payload[0]==0x8f);driver.complete(true,12);assert(owner.snapshot().sessions.size()==1);
    auto token=hold_id<HoldToken>(40);assert(owner.key(select.zone_id,f.audio.component_id,0x41,true,token,20,100));assert(driver.take(21));driver.complete(true,22);
    for(uint64_t now=200;now<4000;now+=150){owner.tick(now);assert(driver.take(now)->frame.payload[0]==0x44);driver.complete(true,now+1);}
    assert(owner.release(token,4000));owner.tick(4000,false);assert(driver.take(4001)->emergency_release);driver.complete(true,4002);
    assert(owner.key(select.zone_id,f.audio.component_id,0x41,true,hold_id<HoldToken>(41),4010));assert(driver.take(4011));driver.complete(WireOutcome::nack,4012);
    owner.tick(4100,false);assert(driver.take(4101)->emergency_release);driver.complete(true,4102);assert(!owner.release(hold_id<HoldToken>(41),4200));assert(!driver.take(4201));
}
void remote_outcome_keeps_release(){
    for(auto reported:{StepResult::timed_out,StepResult::unknown,StepResult::rejected}){
        HoldFixture f;uint8_t random=1;auto rng=[&](uint8_t* data,size_t size){for(size_t i=0;i<size;++i)data[i]=random++;return true;};
        Coordinator leader(rng),follower(rng);TestWire driver;driver.bind(follower,f.port);auto base=f.policy;base.route_grants.clear();const auto leader_id=hold_id<OctopusId>(99);
        assert(leader.install(f.topology(),{},leader_id,0));assert(follower.install(f.topology(),{base},f.port.octopus_id,0));
        assert(leader.update_remote_owner(f.port.octopus_id,{base},true,5000,0));
        std::deque<std::function<void()>> messages;std::vector<AuthorizedCecOperation> sent;uint64_t now=10;
        RemoteExecutionHooks hooks;
        hooks.prepare=[&](OctopusId,const Plan& plan){messages.push_back([&,plan]{auto policy=follower.policies().front();++policy.policy_generation;
            policy.route_grants={{f.binding.binding_id,plan.proposed_session.session_id,plan.proposed_session.route_generation,now+5000,{}}};
            assert(follower.prepare_remote_plan(plan,{policy},now));assert(leader.remote_plan_result(f.port.octopus_id,plan.plan_id,RemotePlanPhase::prepare,true,{},now));});return true;};
        hooks.fence=[&](OctopusId,const Plan& plan,const std::optional<Session>&){messages.push_back([&,plan]{assert(leader.remote_plan_result(f.port.octopus_id,plan.plan_id,RemotePlanPhase::fence,true,{},now));});return true;};
        hooks.activate=[&](OctopusId,const Plan& plan,uint32_t ttl){messages.push_back([&,plan,ttl]{assert(follower.activate_remote_plan(plan.plan_id,ttl,now));assert(leader.remote_plan_result(f.port.octopus_id,plan.plan_id,RemotePlanPhase::activate,true,{},now));});return true;};
        hooks.send=[&](OctopusId,PlanId,const AuthorizedCecOperation& op){sent.push_back(op);messages.push_back([&,op]{auto result=follower.execute_authorized(op,now);if(op.kind==OperationKind::release_key)assert(!result);else assert(result);});return true;};
        leader.remote_execution_hooks(std::move(hooks));
        SelectRequest select{hold_id<MessageId>(60),hold_id<ZoneId>(14),hold_id<ActivityId>(13),hold_id<SessionId>(61),hold_id<PlanId>(62),{}};
        assert(leader.select(select,now).plan);
        for(unsigned i=0;i<20;++i){++now;if(!messages.empty()){auto message=std::move(messages.front());messages.pop_front();message();}
            if(auto frame=driver.take(now)){driver.complete(true,now);for(const auto& result:follower.snapshot().command_outcomes)
                if(result.command_id==frame->command_id)leader.remote_operation_result(f.port.octopus_id,result.command_id,result.result,result.error,now);}}
        assert(leader.snapshot().sessions.size()==1&&!leader.snapshot().executing_plan);sent.clear();
        const auto token=hold_id<HoldToken>(70);assert(leader.key(select.zone_id,f.audio.component_id,0x41,true,token,now,now+100));
        assert(messages.size()==1);auto down_message=std::move(messages.front());messages.pop_front();down_message();
        auto first=driver.take(++now);assert(first&&first->frame.payload[0]==0x44);driver.complete(WireOutcome::nack,++now);
        const auto actual=follower.snapshot().command_outcomes.back();assert(actual.result==StepResult::unknown&&actual.error=="nack");
        assert(leader.remote_operation_result(f.port.octopus_id,actual.command_id,actual.result,actual.error,now));
        assert(leader.remote_operation_result(f.port.octopus_id,actual.command_id,reported,"reported wire outcome",now));
        auto final_release=driver.take(++now);assert(final_release&&final_release->emergency_release);driver.complete(true,++now);
        for(now=200;now<4000;now+=150)assert(!driver.take(now)); // Failed key ended its stream locally; token-up is a later NOP.
        assert(sent.size()==1&&messages.empty()); // No periodic key network messages.
        assert(leader.release(token,now)&&!leader.release(token,now));assert(sent.size()==2&&sent.back().kind==OperationKind::release_key);
        assert(sent.back().arguments.hold_token==token&&sent.back().target==sent.front().target&&sent.back().fence==sent.front().fence);
        auto up=std::move(messages.front());messages.pop_front();up();assert(!driver.take(now+1));
    }
}

int main(){driver_fifo_only();successful_hold_and_exact_up();failed_hold_ends_stream();repeat_busy_and_already_received_up();full_driver_release_slot_and_drop();aggregate_keys_and_taps();aggregate_failure_and_independent_targets();audio_due_and_fences();policy_updates_do_not_dispatch();all_lane_policy_preflight();actual_coordinator();remote_outcome_keeps_release();std::cout<<"PASS direct executor: sole driver FIFO8 plus one release slot, immediate busy/no hidden retry, atomic taps/power, held completion pacing, aggregate target streams, last-key failure ends stream, stale-up NOP, exact cancellation/drain, full-driver release drop/end boundary, audio admission+500ms, actual local/remote coordinator\n";}
