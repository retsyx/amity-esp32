// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
// A main-only reboot seen from a member. The real main and member owner tasks
// run in one process; the sender stands for the authenticated link. The main
// stops, the member's link task detects the lost connection, the main boots
// again with a new boot ID and no selection, the link comes back (fresh
// session fence, a bind that names the new main boot, ready) and the main's
// link-ready capture runs. A main boots with nothing selected, so the member
// adopts no activity at that bind: on every configured adapter and in its own
// recipe baseline, without sending a frame.
// Each scenario runs in its own process and reports every failed check.
#include "cec_hub_fixture.hpp"
#include <memory>
#include <string>
using namespace amity;
namespace m=amity::system;
template<class T> T identity(char digit){return *T::parse(std::string(32,digit));}
static const auto group=identity<m::ConsortiumId>('1');
static const auto controller=identity<m::OctopusId>('2');
static const auto member_id=identity<m::OctopusId>('3');
static const auto first_boot=identity<m::BootId>('4');
static const auto second_boot=identity<m::BootId>('5');
static const auto member_boot=identity<m::BootId>('6');
static const auto first_link=identity<m::LinkId>('7');
static const auto hello_link=identity<m::LinkId>('8');
static const auto second_link=identity<m::LinkId>('9');
static m::QualifiedDeviceName named(m::OctopusId owner,m::Side side,const char* osd,uint8_t adapter=0){return {{group,owner,adapter,side},osd};}

static unsigned failures=0;
#define CHECK(condition,...) do{if(!(condition)){++failures;std::printf("FAIL %s:%d: ",__func__,__LINE__);std::printf(__VA_ARGS__);std::printf("\n");}else{std::printf("ok   ");std::printf(__VA_ARGS__);std::printf("\n");}}while(0)

// Several devices per wire. Each answers polls and identity requests.
struct Peer{int gpio;uint8_t address;std::string osd;uint16_t physical;uint8_t type;};
static std::vector<Peer> peers;
static const Peer* peer_at(int gpio,uint8_t address){for(const auto& peer:peers)if(peer.gpio==gpio&&peer.address==address)return &peer;return nullptr;}
static void install_wire(){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    fixture_transmit=[](int gpio,const cec::Frame& frame){return peer_at(gpio,frame.destination())?cec::TxResult::Success:cec::TxResult::Nack;};
    fixture_send=[](int gpio,const cec::Frame& frame){
        if(frame.size<2)return true;
        const auto* peer=peer_at(gpio,frame.destination());if(!peer)return true;
        cec::Frame reply;reply.data[0]=static_cast<uint8_t>((peer->address<<4)|frame.source());
        switch(frame.data[1]){
        case 0x83:reply.size=5;reply.data[0]=static_cast<uint8_t>((peer->address<<4)|15);reply.data[1]=0x84;
            reply.data[2]=static_cast<uint8_t>(peer->physical>>8);reply.data[3]=static_cast<uint8_t>(peer->physical);reply.data[4]=peer->type;break;
        case 0x46:reply.size=2;reply.data[1]=0x47;for(const char c:peer->osd)reply.data[reply.size++]=static_cast<uint8_t>(c);break;
        case 0x8c:reply.size=5;reply.data[0]=static_cast<uint8_t>((peer->address<<4)|15);reply.data[1]=0x87;reply.data[2]=0;reply.data[3]=0;reply.data[4]=1;break;
        default:return true;
        }
        fixture_incoming.push_back({gpio,reply});return true;
    };
}
static cec::Frame active_source(uint8_t address,uint16_t physical){
    return {{static_cast<uint8_t>((address<<4)|15),0x82,static_cast<uint8_t>(physical>>8),static_cast<uint8_t>(physical)},4};
}
static bool frame_is(const SentFrame& tx,int gpio,uint8_t destination,std::initializer_list<uint8_t> data){
    return tx.gpio==gpio&&tx.frame.destination()==destination&&tx.frame.size==data.size()+1&&std::equal(data.begin(),data.end(),tx.frame.data+1);
}
static size_t count(size_t from,int gpio,uint8_t destination,std::initializer_list<uint8_t> data){
    size_t n=0;for(size_t i=from;i<fixture_sent.size();++i)if(frame_is(fixture_sent[i],gpio,destination,data))++n;return n;
}
static long first(size_t from,int gpio,uint8_t destination,std::initializer_list<uint8_t> data){
    for(size_t i=from;i<fixture_sent.size();++i)if(frame_is(fixture_sent[i],gpio,destination,data))return static_cast<long>(i);
    return -1;
}
// Every addressed or broadcast message (not a poll) on these wires since a point.
static std::vector<SentFrame> messages(size_t from,std::initializer_list<int> gpios){
    std::vector<SentFrame> out;
    for(size_t i=from;i<fixture_sent.size();++i)if(fixture_sent[i].frame.size>1&&std::find(gpios.begin(),gpios.end(),fixture_sent[i].gpio)!=gpios.end())out.push_back(fixture_sent[i]);
    return out;
}
static void print(const char* label,const std::vector<SentFrame>& frames){
    std::printf("  %s (%zu):",label,frames.size());
    for(const auto& tx:frames){std::printf(" gpio%d[",tx.gpio);for(unsigned i=0;i<tx.frame.size;++i)std::printf("%s%02x",i?" ":"",tx.frame.data[i]);std::printf("]");}
    std::printf("\n");
}

struct Bench {
    struct Delivery{m::OwnedIntentKind kind;m::CecPortRef port;bool selected;uint64_t generation;m::AdmissionCode code;};
    HubService member;FakeTask* member_task=nullptr;
    std::unique_ptr<HubService> main;FakeTask* main_task=nullptr;
    m::BootId main_boot=first_boot;m::LinkId link=first_link;bool link_up=false;uint64_t revision=1;
    std::vector<Activity> activities;std::vector<m::ScopedSelector> selectors;std::vector<m::OctopusId> owners;
    std::vector<Delivery> delivered;
    explicit Bench(const cec::BoardProfile& member_profile){
        member.set_system_identity(group,member_id);member.set_boot_identity(member_boot);member.set_coordinator_authority(false);
        assert(fixture_start(member,member_profile)==ESP_OK);member_task=latest_task;
        // runtime_owned_authorized: a live ready link with exactly this link, both boots and the revision.
        member.set_owned_authorizer([this](const m::OwnedAuthorization& a){
            return link_up&&a.controller_id==controller&&a.controller_boot==main_boot&&a.owner_boot==member_boot&&a.link_id==link&&a.config_revision==revision;});
        member.set_owned_observer([this](const m::OwnedAdapterSnapshot& report){if(main&&link_up)main->update_owned_snapshot(report);});
        // The first session's bind names the main's boot, as every production bind does.
        assert(member.bind_owned_controller(controller,revision,main_boot)==ESP_OK);fixture_finish_discovery(member);
    }
    // The main's owned sender: system_sync send -> runtime_send_owned. The
    // transport admits the frame when the link is ready; the member's own
    // admission result decides only whether its link loop keeps the session.
    bool deliver(const m::OwnedIntent& intent){
        assert(intent.target.port.octopus_id==member_id);
        if(!link_up)return false;
        auto* previous=latest_task;latest_task=member_task;
        const m::OwnedAuthorization auth{controller,main_boot,member_boot,link,revision,0};
        const auto result=intent.kind==m::OwnedIntentKind::key_edges?
            member.submit_key_edges(intent.edges,intent.edge_count,intent.generation,link,0):member.submit_owned(intent,auth);
        latest_task=previous;
        delivered.push_back({intent.kind,intent.target.port,intent.selected,intent.generation,result.code});
        return true;
    }
    void start_main(){
        main=std::make_unique<HubService>();main->set_system_identity(group,controller);main->set_boot_identity(main_boot);
        assert(fixture_start(*main,1,2)==ESP_OK);main_task=latest_task;
        assert(main->configure_scoped(activities,selectors,[this](const m::OwnedIntent& intent){return deliver(intent);},controller,revision,{},1,owners)==ESP_OK);
        run_one_command();fixture_finish_discovery(*main);
        CHECK(main->snapshot().current_activity==-1,"the main boots with no current activity");
    }
    void pump(unsigned passes){
        for(unsigned pass=0;pass<passes;++pass){latest_task=member_task;run_idle(1);if(main){latest_task=main_task;run_idle(1);}}
        latest_task=main?main_task:member_task;
    }
    bool finish(int selected){
        for(unsigned pass=0;pass<200;++pass){
            pump(1);
            if(main->snapshot().current_activity==selected&&!main->activity_setup_in_progress()&&!member.activity_setup_in_progress()&&fixture_pending.empty())return true;
        }
        return false;
    }
    bool select(int index){latest_task=main_task;if(main->request_activity(index)!=ESP_OK)return false;run_one_command();return finish(index);}
    // The first connection is established and ready; the main captures context.
    void connect(){
        link_up=true;latest_task=main_task;
        assert(main->refresh_owned_context(member_id,revision,[](m::OwnedContextResult){}));run_one_command();
        for(const auto& report:member.owned_snapshots())if(report.owner==member_id)main->update_owned_snapshot(report);
        pump(2);
    }
    // The main stops: its task, wires and every bit of its state end.
    void stop_main(){link_up=false;latest_task=main_task;main.reset();main_task=nullptr;latest_task=member_task;}
    // After the lost link the member enumerates no bus: not for the bind, not
    // for adoption and not for a later selection, whose devices stay cached.
    unsigned lost_generation=~0u;
    ~Bench(){
        if(lost_generation==~0u)return;
        const auto state=member.snapshot();
        CHECK(state.discovery_generation==lost_generation&&!state.discovering,"the member enumerates no bus after the lost link (generation %u -> %u, discovering=%d)",
            lost_generation,state.discovery_generation,state.discovering);
    }
    // The member's link task detects the lost connection (system_sync disconnected).
    // The main comes back long after the member's last scan, outside its 120 s
    // lookup cooldown, so any scan the bind or adoption asked for would run.
    void link_lost(){
        latest_task=member_task;assert(member.fence_owned(controller,link));pump(6);
        lost_generation=member.snapshot().discovery_generation;fixture_time+=121000000;
    }
    // The main boots with a new boot ID, its saved configuration and no selection.
    void boot_main(){main_boot=second_boot;start_main();}
    m::OwnedContextResult reboot_main(){stop_main();link_lost();boot_main();return reconnect();}
    m::OwnedContextResult reconnect(){
        // A new authenticated connection. The member fences the new session's
        // first scope, binds the main's boot and revision before acknowledging,
        // and the link becomes ready on both sides.
        latest_task=member_task;assert(member.fence_owned(controller,hello_link));
        link=second_link;assert(member.bind_owned_controller(controller,revision,main_boot)==ESP_OK);
        link_up=true;
        // The main's link task asks its owner task for this peer's context.
        m::OwnedContextResult captured=m::OwnedContextResult::failed;
        latest_task=main_task;assert(main->refresh_owned_context(member_id,revision,[&](m::OwnedContextResult result){captured=result;}));
        run_one_command();
        for(const auto& report:member.owned_snapshots())if(report.owner==member_id)main->update_owned_snapshot(report);
        pump(4);
        return captured;
    }
};
static cec::BoardProfile member_profile(bool two_adapters){
    return {cec::BoardProfileId::Development4Tentacle,1,amity::max_tentacles,
        {{0,cec::Side::Front,3},{0,cec::Side::Back,4},{1,cec::Side::Front,two_adapters?5:-1},{1,cec::Side::Back,two_adapters?6:-1},
         {2,cec::Side::Front,-1},{2,cec::Side::Back,-1},{3,cec::Side::Front,-1},{3,cec::Side::Back,-1}},-1,-1};
}
// Main: TV on its front (gpio 1). Member adapter 0: back gpio 4, front gpio 3.
// Member adapter 1 (two-adapter boards): back gpio 6, front gpio 5.
static void main_tv(){peers.push_back({1,0,"TV",0x0000,0});}

// Probe the source policy of one member adapter: a back device announces
// itself as the active source. With no current activity the owner forces
// Standby on both sides of that adapter; nothing else.
static void expect_no_activity_policy(Bench& bench,int front,int back,uint8_t address,uint16_t physical,const char* what){
    const auto from=fixture_sent.size();fixture_incoming.push_back({back,active_source(address,physical)});bench.pump(6);
    const auto sent=messages(from,{front,back});
    const bool forced=sent.size()==2&&count(from,front,15,{0x36})==1&&count(from,back,15,{0x36})==1;
    if(!forced)print("member policy frames",sent);
    CHECK(forced,"%s: an Active Source on member wire %d gets the no-activity policy (Standby broadcast on both sides, nothing else)",what,back);
}

// The member adapter's reply to its TV's Give Device Power Status (0x8f):
// 0 is on, 1 is standby. The member's front address on every adapter is 4.
static int power_status(Bench& bench,int front){
    const auto from=fixture_sent.size();
    fixture_incoming.push_back({front,{{0x04,0x8f},2}});bench.pump(4);
    for(size_t i=from;i<fixture_sent.size();++i)if(fixture_sent[i].gpio==front&&fixture_sent[i].frame.size==3&&fixture_sent[i].frame.data[1]==0x90)return fixture_sent[i].frame.data[2];
    return -1;
}

static void print_deliveries(const Bench& bench){
    for(const auto& d:bench.delivered)std::printf("  delivered kind=%d adapter=%u selected=%d generation=%llu code=%d\n",static_cast<int>(d.kind),d.port.tentacle_id,d.selected,static_cast<unsigned long long>(d.generation),static_cast<int>(d.code));
}
// Nothing is current on the member once it has bound the new main boot.
static void expect_nothing_current(Bench& bench,const char* what){
    CHECK(bench.member.snapshot().current_activity==-1,"%s: the member has no current activity",what);
}
// The member adopted no activity when it bound the new main boot, so before
// its first selection the new main owes this member nothing.
static void expect_new_boot_adopted(Bench& bench,m::OwnedContextResult captured){
    print_deliveries(bench);
    CHECK(captured==m::OwnedContextResult::published&&bench.delivered.empty(),"the new main's capture publishes and sends the member nothing before its first selection (deliveries=%zu)",bench.delivered.size());
    expect_nothing_current(bench,"after binding the new main boot");
}
// A key on the new link before any selection: no input binding exists yet.
// A press is refused (dropped and logged, the link stays up), a lone release
// is a no-op, and neither sends a frame.
static void expect_unbound_keys(Bench& bench,std::initializer_list<int> gpios){
    const auto mark=fixture_sent.size();
    const std::array<m::KeyEdge,2> down{{{0x41,true},{}}};const std::array<m::KeyEdge,2> up{{{0x41,false},{}}};
    latest_task=bench.member_task;const auto pressed=bench.member.submit_key_edges(down,1,1,second_link,0);bench.pump(3);
    latest_task=bench.member_task;const auto released=bench.member.submit_key_edges(up,1,1,second_link,0);bench.pump(3);
    print("member frames for a key on the new link",messages(mark,gpios));
    CHECK(messages(mark,gpios).empty()&&pressed.code==m::AdmissionCode::stale&&released.code==m::AdmissionCode::accepted,
        "a key on the new link before any selection is refused, its release is a no-op, and nothing is sent (press=%d release=%d)",static_cast<int>(pressed.code),static_cast<int>(released.code));
}

// One adapter: the old activity's source and AVR are on member adapter 0.
static void one_adapter(bool hold){
    install_wire();main_tv();
    peers.push_back({4,4,"Source",0x1100,4});peers.push_back({4,5,"AVR",0x1000,5});peers.push_back({4,8,"Console",0x1200,4});
    Bench bench(member_profile(false));
    bench.selectors={{"tv",named(controller,m::Side::front,"TV")},{"source",named(member_id,m::Side::back,"Source")},
                     {"avr",named(member_id,m::Side::back,"AVR")},{"console",named(member_id,m::Side::back,"Console")}};
    bench.activities={{"Watch","tv","source","avr","",0},{"Game","tv","console","","",0}};
    bench.owners={controller,member_id};
    bench.start_main();bench.connect();
    size_t from=fixture_sent.size();
    CHECK(bench.select(0),"Watch completes before the reboot");
    CHECK(count(from,4,4,{0x44,0x6d})==1&&count(from,4,5,{0x44,0x6d})==1&&count(from,4,15,{0x86,0x11,0x00})==1,"the member powered on Source and AVR and routed Source for Watch");
    // Before the reboot, a Console announcement is theft: directed Standby and route repair.
    from=fixture_sent.size();fixture_incoming.push_back({4,active_source(8,0x1200)});bench.pump(6);
    CHECK(count(from,4,8,{0x36})==1&&count(from,4,15,{0x86,0x11,0x00})==1,"before the reboot the member applies Watch's source policy on adapter 0");
    if(hold){
        latest_task=bench.main_task;assert(bench.main->press_key(0x41,true));run_one_command();bench.pump(3);
        CHECK(count(from,4,5,{0x44,0x41})>=2,"Volume Up is held on the member AVR and repeats");
    }
    const auto before_reboot=fixture_sent.size();
    bench.delivered.clear();
    const auto captured=bench.reboot_main();
    const auto after_reboot=fixture_sent.size();
    print("member frames during the reboot",messages(before_reboot,{3,4}));
    if(hold){
        CHECK(count(before_reboot,4,5,{0x45})==1,"the lost link ends the hold with one release to the AVR");
        const auto last_press=[&]{long last=-1;for(size_t i=before_reboot;i<fixture_sent.size();++i)if(frame_is(fixture_sent[i],4,5,{0x44,0x41}))last=static_cast<long>(i);return last;}();
        CHECK(last_press<first(before_reboot,4,5,{0x45}),"no Volume Up press follows that release");
        const auto presses=count(0,4,5,{0x44,0x41});bench.pump(10);
        CHECK(count(0,4,5,{0x44,0x41})==presses,"the stopped hold does not repeat");
        // Releasing an audio key asks its recipient for the new audio status.
        const auto sent=messages(before_reboot,{3,4});
        CHECK(std::all_of(sent.begin(),sent.end(),[](const auto& tx){return frame_is(tx,4,5,{0x45})||frame_is(tx,4,5,{0x71});}),
            "the release and its audio status request to the AVR are the only member frames of the reboot");
    }else CHECK(messages(before_reboot,{3,4}).empty(),"the reboot and the new boot's bind send nothing on member wires");
    expect_new_boot_adopted(bench,captured);
    bench.pump(10);
    CHECK(messages(after_reboot,{3,4}).empty(),"no member TX after the bind while idle");
    expect_no_activity_policy(bench,3,4,8,0x1200,"adapter 0");
    const auto status=power_status(bench,3);
    CHECK(status==1,"adapter 0 reports standby to its TV's power status request (reply=%d)",status);
    expect_unbound_keys(bench,{3,4});
    // Nothing is current, so the next selection powers on its devices. Old
    // devices are not part of a transition from no activity.
    from=fixture_sent.size();
    CHECK(bench.select(1),"Game completes after the reboot");
    print("member frames for Game",messages(from,{3,4}));
    CHECK(count(from,4,8,{0x44,0x6d})==1&&count(from,4,15,{0x86,0x12,0x00})==1,"Game powers on Console once and routes it");
    CHECK(count(from,4,4,{0x36})==0&&count(from,4,5,{0x36})==0,"Game sends no Standby to Source or AVR, which no current activity uses");
}

// Two adapters: Source on member adapter 0, AVR on member adapter 1.
static void two_adapters(){
    install_wire();main_tv();
    peers.push_back({4,4,"Source",0x1100,4});peers.push_back({4,8,"Console",0x1200,4});
    peers.push_back({6,5,"AVR",0x1000,5});peers.push_back({6,4,"Player",0x1100,4});
    Bench bench(member_profile(true));
    bench.selectors={{"tv",named(controller,m::Side::front,"TV")},{"source",named(member_id,m::Side::back,"Source")},
                     {"avr",named(member_id,m::Side::back,"AVR",1)},{"console",named(member_id,m::Side::back,"Console")}};
    bench.activities={{"Watch","tv","source","avr","",0},{"Game","tv","console","","",0}};
    bench.owners={controller,member_id};
    bench.start_main();bench.connect();
    size_t from=fixture_sent.size();
    CHECK(bench.select(0),"Watch completes before the reboot");
    CHECK(count(from,4,4,{0x44,0x6d})==1&&count(from,6,5,{0x44,0x6d})==1,"the member powered on Source (adapter 0) and AVR (adapter 1)");
    from=fixture_sent.size();fixture_incoming.push_back({6,active_source(4,0x1100)});bench.pump(6);
    CHECK(count(from,6,4,{0x36})==1&&count(from,5,15,{0x36})==0,"before the reboot adapter 1 applies Watch's foreign-source policy");
    latest_task=bench.main_task;assert(bench.main->press_key(4,true));run_one_command();bench.pump(3);
    CHECK(count(from,4,4,{0x44,0x04})>=2,"Right is held on the member Source and repeats");
    const auto before_reboot=fixture_sent.size();bench.delivered.clear();
    const auto captured=bench.reboot_main();
    const auto after_reboot=fixture_sent.size();
    print("member frames during the reboot",messages(before_reboot,{3,4,5,6}));
    CHECK(count(before_reboot,4,4,{0x45})==1&&messages(before_reboot,{3,4,5,6}).size()==1,"the lost link ends the hold with one release to Source, and nothing else is sent");
    expect_new_boot_adopted(bench,captured);
    const auto presses=count(0,4,4,{0x44,0x04});bench.pump(10);
    CHECK(count(0,4,4,{0x44,0x04})==presses,"the stopped hold does not repeat");
    CHECK(messages(after_reboot,{3,4,5,6}).empty(),"no member TX after the bind while idle");
    expect_no_activity_policy(bench,3,4,8,0x1200,"adapter 0");
    expect_no_activity_policy(bench,5,6,4,0x1100,"adapter 1");
    from=fixture_sent.size();
    CHECK(bench.select(1),"Game completes after the reboot");
    print("member frames for Game",messages(from,{3,4,5,6}));
    CHECK(count(from,4,8,{0x44,0x6d})==1&&count(from,4,4,{0x36})==0&&count(from,6,5,{0x36})==0,"Game powers on Console and sends no Standby to Source or AVR");
}

// Two adapters, but every saved component is on adapter 0. Adapter 1 is
// configured and received the old activity's context like every adapter.
static void unreferenced_adapter(){
    install_wire();main_tv();
    peers.push_back({4,4,"Source",0x1100,4});peers.push_back({4,5,"AVR",0x1000,5});peers.push_back({4,8,"Console",0x1200,4});
    peers.push_back({6,4,"Player",0x1100,4});
    Bench bench(member_profile(true));
    bench.selectors={{"tv",named(controller,m::Side::front,"TV")},{"source",named(member_id,m::Side::back,"Source")},
                     {"avr",named(member_id,m::Side::back,"AVR")},{"console",named(member_id,m::Side::back,"Console")}};
    bench.activities={{"Watch","tv","source","avr","",0},{"Game","tv","console","","",0}};
    bench.owners={controller,member_id};
    bench.start_main();bench.connect();
    CHECK(bench.select(0),"Watch completes before the reboot");
    size_t from=fixture_sent.size();fixture_incoming.push_back({6,active_source(4,0x1100)});bench.pump(6);
    CHECK(count(from,6,4,{0x36})==1&&count(from,5,15,{0x36})==0,"before the reboot adapter 1 applies Watch's foreign-source policy");
    CHECK(power_status(bench,5)==0,"before the reboot adapter 1 reports on to its TV");
    bench.delivered.clear();
    const auto before_reboot=fixture_sent.size();
    const auto captured=bench.reboot_main();
    CHECK(messages(before_reboot,{3,4,5,6}).empty(),"the reboot and the new boot's bind send nothing on member wires");
    expect_new_boot_adopted(bench,captured);
    expect_no_activity_policy(bench,3,4,8,0x1200,"adapter 0");
    expect_no_activity_policy(bench,5,6,4,0x1100,"adapter 1 (no saved equipment)");
    const auto status=power_status(bench,5);
    CHECK(status==1,"adapter 1 (no saved equipment) reports standby to its TV after the reboot (reply=%d)",status);
}

// The member is in the saved roster but none of its equipment is saved. The
// old activity reached it as an owner and set every adapter's context.
static void roster_only_member(){
    install_wire();main_tv();peers.push_back({2,4,"Player",0x1100,4});
    peers.push_back({4,4,"Source",0x1100,4});
    Bench bench(member_profile(false));
    bench.selectors={{"tv",named(controller,m::Side::front,"TV")},{"player",named(controller,m::Side::back,"Player")}};
    bench.activities={{"Watch","tv","player","","",0}};
    bench.owners={controller,member_id};
    bench.start_main();bench.connect();
    CHECK(bench.select(0),"Watch completes before the reboot");
    size_t from=fixture_sent.size();fixture_incoming.push_back({4,active_source(4,0x1100)});bench.pump(6);
    CHECK(count(from,4,4,{0x36})==1&&count(from,3,15,{0x36})==0,"before the reboot the member applies Watch's foreign-source policy");
    CHECK(power_status(bench,3)==0,"before the reboot the member reports on to its TV");
    bench.delivered.clear();
    const auto before_reboot=fixture_sent.size();
    const auto captured=bench.reboot_main();
    CHECK(messages(before_reboot,{3,4}).empty(),"the reboot and the new boot's bind send nothing on member wires");
    expect_new_boot_adopted(bench,captured);
    expect_no_activity_policy(bench,3,4,4,0x1100,"roster-only member adapter 0");
    const auto status=power_status(bench,3);
    CHECK(status==1,"the roster-only member reports standby to its TV after the reboot (reply=%d)",status);
}

// After the reboot nothing is current, so the member's own no-activity policy
// may put the old activity's devices in standby. Selecting that activity again
// must power them on.
static void reselect_after_forced_standby(){
    install_wire();main_tv();
    peers.push_back({4,4,"Source",0x1100,4});peers.push_back({4,5,"AVR",0x1000,5});
    Bench bench(member_profile(false));
    bench.selectors={{"tv",named(controller,m::Side::front,"TV")},{"source",named(member_id,m::Side::back,"Source")},{"avr",named(member_id,m::Side::back,"AVR")}};
    bench.activities={{"Watch","tv","source","avr","",0}};
    bench.owners={controller,member_id};
    bench.start_main();bench.connect();
    CHECK(bench.select(0),"Watch completes before the reboot");
    bench.delivered.clear();
    const auto captured=bench.reboot_main();
    expect_new_boot_adopted(bench,captured);
    // The source wakes and announces itself; with no activity the member
    // forces Standby on both sides of adapter 0.
    size_t from=fixture_sent.size();fixture_incoming.push_back({4,active_source(4,0x1100)});bench.pump(6);
    CHECK(count(from,3,15,{0x36})==1&&count(from,4,15,{0x36})==1,"the member forced Standby on adapter 0 (Source and AVR are now off)");
    from=fixture_sent.size();
    CHECK(bench.select(0),"Watch completes again after the reboot");
    print("member frames for Watch after the reboot",messages(from,{3,4}));
    CHECK(count(from,4,4,{0x44,0x6d})==1&&count(from,4,5,{0x44,0x6d})==1&&count(from,4,15,{0x86,0x11,0x00})==1,"selecting Watch powers on the member's Source and AVR and routes Source");
}

// After the reboot nothing is current. A member adapter settings change is
// gated only by an activity that is current or being set up.
static void member_settings_after_reboot(){
    install_wire();main_tv();
    peers.push_back({4,4,"Source",0x1100,4});peers.push_back({4,5,"AVR",0x1000,5});
    Bench bench(member_profile(false));
    bench.selectors={{"tv",named(controller,m::Side::front,"TV")},{"source",named(member_id,m::Side::back,"Source")},{"avr",named(member_id,m::Side::back,"AVR")}};
    bench.activities={{"Watch","tv","source","avr","",0}};
    bench.owners={controller,member_id};
    bench.start_main();bench.connect();
    CHECK(bench.select(0),"Watch completes before the reboot");
    latest_task=bench.member_task;unsigned writes=0;
    const auto during=bench.member.commit_adapter_settings(member_boot,{{0,0x2000}},uint64_t(fixture_time)/1000+5000,[&]{++writes;return ESP_OK;});
    CHECK(during==ESP_ERR_INVALID_STATE&&!writes,"while Watch is current the member refuses an adapter settings change");
    bench.delivered.clear();
    const auto captured=bench.reboot_main();
    expect_new_boot_adopted(bench,captured);
    latest_task=bench.member_task;
    const auto after=bench.member.commit_adapter_settings(member_boot,{{0,0x2000}},uint64_t(fixture_time)/1000+5000,[&]{++writes;return ESP_OK;});
    CHECK(after==ESP_OK&&writes==1,"with no current activity after the reboot the member accepts it (result=%d writes=%u)",static_cast<int>(after),writes);
    // A selection after the reboot is current again and gates settings.
    CHECK(bench.select(0),"Watch completes after the reboot");
    latest_task=bench.member_task;
    const auto selected=bench.member.commit_adapter_settings(member_boot,{{0,0x3000}},uint64_t(fixture_time)/1000+5000,[&]{++writes;return ESP_OK;});
    CHECK(selected==ESP_ERR_INVALID_STATE&&writes==1,"once the new main selects Watch the member refuses it again (result=%d)",static_cast<int>(selected));
}

// As reselect_after_forced_standby, but the next activity shares only the AVR.
static void shared_after_forced_standby(){
    install_wire();main_tv();
    peers.push_back({4,4,"Source",0x1100,4});peers.push_back({4,5,"AVR",0x1000,5});peers.push_back({4,8,"Console",0x1200,4});
    Bench bench(member_profile(false));
    bench.selectors={{"tv",named(controller,m::Side::front,"TV")},{"source",named(member_id,m::Side::back,"Source")},
                     {"avr",named(member_id,m::Side::back,"AVR")},{"console",named(member_id,m::Side::back,"Console")}};
    bench.activities={{"Watch","tv","source","avr","",0},{"Music","tv","console","avr","",0}};
    bench.owners={controller,member_id};
    bench.start_main();bench.connect();
    CHECK(bench.select(0),"Watch completes before the reboot");
    bench.delivered.clear();
    const auto captured=bench.reboot_main();
    expect_new_boot_adopted(bench,captured);
    size_t from=fixture_sent.size();fixture_incoming.push_back({4,active_source(4,0x1100)});bench.pump(6);
    CHECK(count(from,3,15,{0x36})==1&&count(from,4,15,{0x36})==1,"the member forced Standby on adapter 0 (Source and AVR are now off)");
    from=fixture_sent.size();
    CHECK(bench.select(1),"Music completes after the reboot");
    print("member frames for Music after the reboot",messages(from,{3,4}));
    CHECK(count(from,4,8,{0x44,0x6d})==1&&count(from,4,5,{0x44,0x6d})==1,"selecting Music powers on Console and the AVR that the member itself put in standby");
}

// The main stops while the member's part of Watch is still running. The
// member finishes that admitted recipe; the new main's bind waits for it and
// then adopts no activity.
static void reboot_mid_recipe(){
    install_wire();main_tv();
    peers.push_back({4,4,"Source",0x1100,4});peers.push_back({4,5,"AVR",0x1000,5});peers.push_back({4,8,"Console",0x1200,4});
    Bench bench(member_profile(false));
    bench.selectors={{"tv",named(controller,m::Side::front,"TV")},{"source",named(member_id,m::Side::back,"Source")},
                     {"avr",named(member_id,m::Side::back,"AVR")},{"console",named(member_id,m::Side::back,"Console")}};
    bench.activities={{"Watch","tv","source","avr","",0},{"Game","tv","console","","",0}};
    bench.owners={controller,member_id};
    bench.start_main();bench.connect();
    const auto from=fixture_sent.size();
    fixture_async_autocomplete=false;
    latest_task=bench.main_task;assert(bench.main->request_activity(0)==ESP_OK);run_one_command();
    const auto member_pending=[]{return std::any_of(fixture_pending.begin(),fixture_pending.end(),[](const auto& p){return p.gpio==4&&p.frame.size==3&&p.frame.data[1]==0x44&&p.frame.data[2]==0x6d;});};
    for(unsigned pass=0;pass<64&&!member_pending();++pass){while(fixture_complete_next(cec::TxResult::Pending,1)||fixture_complete_next(cec::TxResult::Pending,2)){}bench.pump(1);}
    CHECK(member_pending(),"the member's Watch recipe is waiting on its first power-on when the main stops");
    bench.stop_main();bench.link_lost();
    fixture_async_autocomplete=true;bench.boot_main();
    // The member's recipe goes on while the new connection binds.
    fixture_delay=[]{fixture_complete_next(cec::TxResult::Pending);};
    bench.delivered.clear();
    const auto captured=bench.reconnect();fixture_delay={};fixture_complete_all();bench.pump(4);
    print("member frames from Watch through the reboot",messages(from,{3,4}));
    CHECK(count(from,4,4,{0x44,0x6d})==1&&count(from,4,5,{0x44,0x6d})==1&&count(from,4,15,{0x86,0x11,0x00})==1,"the admitted recipe powered on Source and AVR and routed Source, once each");
    expect_new_boot_adopted(bench,captured);
    const auto after=fixture_sent.size();bench.pump(6);
    CHECK(messages(after,{3,4}).empty(),"no member TX after the bind while idle");
    expect_no_activity_policy(bench,3,4,8,0x1200,"adapter 0");
    const auto next=fixture_sent.size();
    CHECK(bench.select(1),"Game completes after the reboot");
    print("member frames for Game",messages(next,{3,4}));
    CHECK(count(next,4,8,{0x44,0x6d})==1&&count(next,4,4,{0x36})==0&&count(next,4,5,{0x36})==0,"Game powers on Console and sends no Standby to the finished recipe's Source or AVR");
}

// Standby is the new main's first selection. Nothing is current on the
// member, so its part is the no-activity Standby broadcast on every member
// port, front and back, as the main's own Standby does on its ports.
static void standby_after_reboot(){
    install_wire();main_tv();
    peers.push_back({4,4,"Source",0x1100,4});peers.push_back({4,5,"AVR",0x1000,5});
    Bench bench(member_profile(false));
    bench.selectors={{"tv",named(controller,m::Side::front,"TV")},{"source",named(member_id,m::Side::back,"Source")},{"avr",named(member_id,m::Side::back,"AVR")}};
    bench.activities={{"Watch","tv","source","avr","",0}};
    bench.owners={controller,member_id};
    bench.start_main();bench.connect();
    CHECK(bench.select(0),"Watch completes before the reboot");
    bench.delivered.clear();
    const auto captured=bench.reboot_main();
    expect_new_boot_adopted(bench,captured);
    const auto from=fixture_sent.size();
    CHECK(bench.select(-1),"Standby completes after the reboot");
    print("member frames for Standby after the reboot",messages(from,{3,4}));
    CHECK(count(from,3,15,{0x36})==1&&count(from,4,15,{0x36})==1&&messages(from,{3,4}).size()==2,"the member broadcasts Standby on its front and back and sends nothing else");
}

// Saved equipment names a member adapter that the member does not configure
// (adapter 1 is unwired on this board). Before its first selection the new
// main sends the member nothing, so nothing is refused and the link stays up.
static void unconfigured_context(){
    install_wire();main_tv();
    peers.push_back({4,4,"Source",0x1100,4});
    Bench bench(member_profile(false));
    bench.selectors={{"tv",named(controller,m::Side::front,"TV")},{"spare",named(member_id,m::Side::back,"Spare",1)},{"source",named(member_id,m::Side::back,"Source")}};
    bench.activities={{"Watch","tv","source","","",0}};
    bench.owners={controller,member_id};
    bench.start_main();bench.connect();
    CHECK(bench.select(0),"Watch completes before the reboot");
    bench.delivered.clear();
    const auto captured=bench.reboot_main();
    expect_new_boot_adopted(bench,captured);
    CHECK(std::none_of(bench.delivered.begin(),bench.delivered.end(),[](const auto& d){return d.code==m::AdmissionCode::invalid;}),"the member refuses nothing as invalid");
    expect_no_activity_policy(bench,3,4,4,0x1100,"adapter 0");
    // The new main's first selection never names a connector either.
    bench.delivered.clear();
    CHECK(bench.select(0),"Watch completes after the reboot");
    print_deliveries(bench);
    CHECK(!bench.delivered.empty()&&std::all_of(bench.delivered.begin(),bench.delivered.end(),[](const auto& d){return d.code==m::AdmissionCode::accepted;}),"the member accepts every intent of the new main's first selection");
}
// The main comes back with a saved configuration that no longer names the
// member's equipment (edited while the member's link was down).
static void dropped_equipment(){
    install_wire();main_tv();peers.push_back({2,4,"Player",0x1100,4});
    peers.push_back({4,4,"Source",0x1100,4});peers.push_back({4,5,"AVR",0x1000,5});peers.push_back({4,8,"Console",0x1200,4});
    Bench bench(member_profile(false));
    bench.selectors={{"tv",named(controller,m::Side::front,"TV")},{"source",named(member_id,m::Side::back,"Source")},{"avr",named(member_id,m::Side::back,"AVR")}};
    bench.activities={{"Watch","tv","source","avr","",0}};
    bench.owners={controller,member_id};
    bench.start_main();bench.connect();
    CHECK(bench.select(0),"Watch completes before the reboot");
    bench.stop_main();bench.link_lost();
    bench.selectors={{"tv",named(controller,m::Side::front,"TV")},{"player",named(controller,m::Side::back,"Player")}};
    bench.activities={{"Movie","tv","player","","",0}};
    bench.delivered.clear();bench.boot_main();
    const auto captured=bench.reconnect();
    expect_new_boot_adopted(bench,captured);
    expect_no_activity_policy(bench,3,4,8,0x1200,"adapter 0 (equipment no longer saved)");
    const auto status=power_status(bench,3);
    CHECK(status==1,"adapter 0 reports standby to its TV after the reboot (reply=%d)",status);
}

// A link outage without a main reboot: the same main boot re-offers the
// current selection on the new link. The member keeps Watch on every adapter.
static void outage_keeps_activity(){
    install_wire();main_tv();
    peers.push_back({4,4,"Source",0x1100,4});peers.push_back({4,5,"AVR",0x1000,5});peers.push_back({4,8,"Console",0x1200,4});
    peers.push_back({6,4,"Player",0x1100,4});
    Bench bench(member_profile(true));
    bench.selectors={{"tv",named(controller,m::Side::front,"TV")},{"source",named(member_id,m::Side::back,"Source")},{"avr",named(member_id,m::Side::back,"AVR")}};
    bench.activities={{"Watch","tv","source","avr","",0}};
    bench.owners={controller,member_id};
    bench.start_main();bench.connect();
    CHECK(bench.select(0),"Watch completes before the outage");
    const auto current=bench.member.snapshot().current_activity;
    bench.link_lost();bench.delivered.clear();
    const auto from_outage=fixture_sent.size();
    const auto captured=bench.reconnect();
    std::printf("  capture result=%d\n",static_cast<int>(captured));print_deliveries(bench);
    CHECK(messages(from_outage,{3,4,5,6}).empty()&&bench.member.snapshot().current_activity==current&&current>=0,"the same-boot bind and re-offer send nothing and keep Watch current on the member");
    size_t from=fixture_sent.size();fixture_incoming.push_back({4,active_source(8,0x1200)});bench.pump(6);
    CHECK(count(from,4,8,{0x36})==1&&count(from,4,15,{0x86,0x11,0x00})==1&&count(from,3,15,{0x36})==0,"after the outage adapter 0 still applies Watch's source policy");
    from=fixture_sent.size();fixture_incoming.push_back({6,active_source(4,0x1100)});bench.pump(6);
    CHECK(count(from,6,4,{0x36})==1&&count(from,5,15,{0x36})==0,"after the outage adapter 1 still applies Watch's foreign-source policy");
    CHECK(power_status(bench,5)==0,"after the outage adapter 1 still reports on");
}

// A held key's release must still reach the device that was pressed. Here the
// release's lookup misses the cache when the link drops (the device renamed
// itself), so it waits on a lookup scan. The restarted main's bind waits for
// that release before it ends the activity the release resolves against.
static void release_before_adoption(){
    install_wire();main_tv();
    peers.push_back({4,4,"Source",0x1100,4});
    Bench bench(member_profile(false));
    bench.selectors={{"tv",named(controller,m::Side::front,"TV")},{"source",named(member_id,m::Side::back,"Source")}};
    bench.activities={{"Watch","tv","source","source","",0}};
    bench.owners={controller,member_id};
    bench.start_main();bench.connect();
    CHECK(bench.select(0),"Watch completes before the reboot");
    latest_task=bench.main_task;assert(bench.main->press_key(0x41,true));run_one_command();bench.pump(2);
    CHECK(count(0,4,4,{0x44,0x41})>=1,"Volume Up is held on the member Source");
    // Long after the last scan, Source reports another name. Its next repeat
    // misses the cache and starts a lookup scan.
    fixture_time+=200000000;
    fixture_incoming.push_back({4,{{0x40,0x47,'R','e','n','a','m','e','d'},9}});bench.pump(1);
    const auto mark=fixture_sent.size();
    // The main stops; the member's link task fences the old link at once.
    bench.stop_main();latest_task=bench.member_task;assert(bench.member.fence_owned(controller,bench.link));
    bench.boot_main();
    latest_task=bench.member_task;assert(bench.member.fence_owned(controller,hello_link));
    bench.link=second_link;
    // The member's synchronization retries its bind each second until the bind is admitted.
    esp_err_t bound=ESP_FAIL;
    for(unsigned attempt=0;attempt<20&&bound!=ESP_OK;++attempt){latest_task=bench.member_task;bound=bench.member.bind_owned_controller(controller,bench.revision,bench.main_boot);if(bound!=ESP_OK)bench.pump(4);}
    CHECK(bound==ESP_OK,"the restarted main's bind is admitted");
    bench.link_up=true;bench.pump(8);
    print("member frames from the link loss",messages(mark,{3,4}));
    CHECK(count(mark,4,4,{0x45})==1,"the hold's release reaches Source once");
    expect_nothing_current(bench,"after the bind");
    expect_no_activity_policy(bench,3,4,4,0x1100,"adapter 0");
}

int main(int argc,char** argv){
    const std::string scenario=argc>1?argv[1]:"";
    if(scenario=="one-adapter")one_adapter(false);
    else if(scenario=="one-adapter-hold")one_adapter(true);
    else if(scenario=="two-adapters")two_adapters();
    else if(scenario=="unreferenced-adapter")unreferenced_adapter();
    else if(scenario=="roster-only")roster_only_member();
    else if(scenario=="reselect-after-forced-standby")reselect_after_forced_standby();
    else if(scenario=="member-settings")member_settings_after_reboot();
    else if(scenario=="shared-after-forced-standby")shared_after_forced_standby();
    else if(scenario=="reboot-mid-recipe")reboot_mid_recipe();
    else if(scenario=="standby-after-reboot")standby_after_reboot();
    else if(scenario=="unconfigured-context")unconfigured_context();
    else if(scenario=="dropped-equipment")dropped_equipment();
    else if(scenario=="outage-keeps-activity")outage_keeps_activity();
    else if(scenario=="release-before-adoption")release_before_adoption();
    else{std::printf("unknown scenario '%s'\n",scenario.c_str());return 2;}
    std::printf("%s: %u failed check(s)\n",scenario.c_str(),failures);
    fixture_send={};fixture_transmit={};
    return failures?1:0;
}
