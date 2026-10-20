// Private runtime integration. Included inside runtime.cpp's anonymous namespace.
struct RelayServer {
    std::shared_ptr<Active> active;LinkScope scope;model::MessageId reservation;model::JoinId join;model::OctopusId candidate;
    std::mutex mutex;OpaqueRelay codec;std::atomic_bool cancelled{false},connected{false};uint64_t deadline=0;
};
struct RelayClient {
    std::shared_ptr<Active> active;model::MessageId reservation;model::JoinId join;model::OctopusId candidate;
    std::mutex mutex;RelayBuffer response;model::MessageId request;uint64_t sequence=0,deadline=0;
    bool ready=false,failed=false,closed=false;
};
std::mutex relay_lock;std::shared_ptr<RelayServer> relay_server;std::shared_ptr<RelayClient> relay_client;
std::atomic_bool relay_running{false};
void defer_radio_release(uint64_t);
bool relay_queue(const std::shared_ptr<Active> &active,Kind kind,std::string payload){
    if(!active||active->cancelled||!active->ready||payload.size()>6144)return false;
    std::lock_guard<std::mutex> guard(active->mutex);if(active->queue.size()>=16)return false;
    active->queue.push_back({kind,std::move(payload),5000,now_ms()});return true;
}
std::string relay_ids(model::MessageId reservation,model::JoinId join){return "\"reservation_id\":\""+reservation.str()+"\",\"join_id\":\""+join.str()+"\"";}
bool relay_status(const std::shared_ptr<RelayServer> &service,const char *phase,const char *result,const char *error){return relay_queue(service->active,Kind::relay_result,"{"+relay_ids(service->reservation,service->join)+",\"phase\":\""+phase+"\",\"result\":\""+result+"\",\"error\":\""+error+"\"}");}
bool relay_reply(const std::shared_ptr<RelayServer> &service){
    RelayFragment fragment;std::string reply;
    {std::lock_guard<std::mutex> guard(service->mutex);if(!service->codec.cached())return false;fragment=service->codec.request();reply=service->codec.response();}
    fragment.total=reply.size();for(size_t offset=0;offset<reply.size();offset+=2048){fragment.offset=offset;fragment.data=reply.substr(offset,2048);if(!relay_queue(service->active,Kind::relay_result,relay_fragment_json(fragment,true)))return false;}return true;
}
void relay_worker_work(void *argument){
    std::unique_ptr<std::shared_ptr<RelayServer>> holder(static_cast<std::shared_ptr<RelayServer>*>(argument));auto service=*holder;holder.reset();
    RuntimeRadioHooks hooks;{std::lock_guard<std::mutex> guard(lock);hooks=radio;}
    uint64_t generation=0;const bool granted=!service->cancelled&&hooks.reserve&&hooks.reserve(true,generation)==ESP_OK;
    if(!granted)generation=0;
    const bool connected=granted&&!service->cancelled&&gatt_client_connect(service->candidate,generation)==ESP_OK;
    service->connected=connected;
    if(connected)relay_status(service,"reserve","ready","");else relay_status(service,"reserve",granted?"failed":"busy",granted?"NO_CANDIDATE":"RADIO_BUSY");
    while(connected&&!service->cancelled&&!service->active->cancelled&&now_ms()<service->deadline){
        std::optional<RelayFragment> request;{std::lock_guard<std::mutex> guard(service->mutex);request=service->codec.take_request();}
        if(!request){vTaskDelay(pdMS_TO_TICKS(10));continue;}
        std::string reply;if(!gatt_client_exchange(request->encrypted,request->data,reply)){service->cancelled=true;break;}
        bool completed;{std::lock_guard<std::mutex> guard(service->mutex);completed=service->codec.finish(service->scope,request->request_id,request->sequence,std::move(reply),now_ms());}
        if(!completed||!relay_reply(service)){service->cancelled=true;break;}
    }
    gatt_client_disconnect();defer_radio_release(generation);service->connected=false;
    {std::lock_guard<std::mutex> guard(service->mutex);service->codec.cancel();}
    relay_status(service,"close","closed","");
    {std::lock_guard<std::mutex> guard(relay_lock);if(relay_server==service)relay_server.reset();}
    // Returning releases the service and its owning reference to the link.
}
void relay_worker(void *argument){relay_worker_work(argument);relay_running=false;vTaskDeleteWithCaps(nullptr);}
void relay_dispatch(const std::shared_ptr<Active> &active,const Envelope &envelope){
    if(envelope.kind==Kind::relay_result){
        std::shared_ptr<RelayClient> client;{std::lock_guard<std::mutex> guard(relay_lock);client=relay_client;}
        if(!client||client->active!=active)return;
        model::MessageId reservation;model::JoinId join;if(!id(envelope.payload.get("reservation_id"),reservation)||!id(envelope.payload.get("join_id"),join)||reservation!=client->reservation||join!=client->join)return;
        std::lock_guard<std::mutex> guard(client->mutex);const auto phase=envelope.payload.get("phase")->string,result=envelope.payload.get("result")->string;
        if(phase=="reserve"){client->ready=result=="ready";client->failed=!client->ready;}
        else if(phase=="close")client->closed=true;
        else if(result=="failed")client->failed=true;
        else{RelayFragment fragment;if(parse_relay_fragment(envelope.payload,fragment)!=Error::none||fragment.request_id!=client->request||fragment.sequence!=client->sequence){client->failed=true;return;}const auto accepted=client->response.append(fragment);if(accepted!=Error::none&&accepted!=Error::duplicate)client->failed=true;}
        return;
    }
    bool authority=false;{std::lock_guard<std::mutex> guard(lock);authority=group.role==Group::member&&group.authority==active->peer;}
    if(!authority)return;
    model::MessageId reservation;model::JoinId join;if(!id(envelope.payload.get("reservation_id"),reservation)||!id(envelope.payload.get("join_id"),join))return;
    if(envelope.kind==Kind::relay_reserve){
        model::OctopusId candidate;uint64_t duration=0;if(!id(envelope.payload.get("candidate_id"),candidate)||!json_uint(envelope.payload.get("validity_ms"),duration,1,180000))return;
        auto service=std::make_shared<RelayServer>();service->active=active;service->scope=active->session.scope();service->reservation=reservation;service->join=join;service->candidate=candidate;service->deadline=now_ms()+duration;
        {std::lock_guard<std::mutex> guard(relay_lock);if(relay_server||relay_running){relay_status(service,"reserve","busy","RADIO_BUSY");return;}relay_running=true;relay_server=service;}
        if(service->codec.reserve(service->scope,reservation,join,candidate,duration,now_ms())!=Error::none){service->cancelled=true;relay_running=false;return;}
        auto argument=new std::shared_ptr<RelayServer>(service);if(xTaskCreateWithCaps(relay_worker,"group_relay",16384,argument,2,nullptr,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS){delete argument;relay_status(service,"reserve","failed","INTERNAL_ERROR");std::lock_guard<std::mutex> guard(relay_lock);relay_server.reset();relay_running=false;}
        return;
    }
    std::shared_ptr<RelayServer> service;{std::lock_guard<std::mutex> guard(relay_lock);service=relay_server;}
    if(!service||service->active!=active||service->reservation!=reservation||service->join!=join)return;
    if(envelope.kind==Kind::relay_close){service->cancelled=true;gatt_client_disconnect();return;}
    RelayFragment fragment;if(parse_relay_fragment(envelope.payload,fragment)!=Error::none)return;
    bool replay=false;{std::lock_guard<std::mutex> guard(service->mutex);const auto accepted=service->codec.append(active->session.scope(),fragment,now_ms());if(accepted!=Error::none&&accepted!=Error::duplicate){service->cancelled=true;return;}replay=accepted==Error::duplicate&&service->codec.cached()&&service->codec.request().sequence==fragment.sequence&&service->codec.request().request_id==fragment.request_id;}
    if(replay)relay_reply(service);
}
void relay_link_closed(const std::shared_ptr<Active> &active){
    bool cancel=false;{std::lock_guard<std::mutex> guard(relay_lock);if(relay_server&&relay_server->active==active){relay_server->cancelled=true;cancel=true;}if(relay_client&&relay_client->active==active){std::lock_guard<std::mutex> client_guard(relay_client->mutex);relay_client->failed=true;}}
    if(cancel)gatt_client_disconnect();
}
void relay_expire(){bool cancel=false;{std::lock_guard<std::mutex> guard(relay_lock);if(relay_server&&(relay_server->cancelled||relay_server->active->cancelled||now_ms()>=relay_server->deadline)){relay_server->cancelled=true;cancel=true;}}if(cancel)gatt_client_disconnect();}

void relay_cancel_all(){std::lock_guard<std::mutex> guard(relay_lock);if(relay_server)relay_server->cancelled=true;if(relay_client){std::lock_guard<std::mutex> client_guard(relay_client->mutex);relay_client->failed=true;}}
std::shared_ptr<RelayClient> relay_open(model::OctopusId candidate,model::JoinId join){
    std::vector<std::shared_ptr<Active>> helpers;{std::lock_guard<std::mutex> guard(lock);for(const auto &active:links)if(active->ready&&!active->cancelled){std::lock_guard<std::mutex> qguard(active->mutex);if(active->status.negotiated_capabilities&bootstrap_relay)helpers.push_back(active);}}
    for(const auto &helper:helpers){
        if(invitation_cancelled)return {};
        auto client=std::make_shared<RelayClient>();client->active=helper;client->candidate=candidate;client->join=join;client->reservation=random_id<model::MessageId>();client->deadline=now_ms()+180000;
        {std::lock_guard<std::mutex> guard(relay_lock);relay_client=client;}
        if(!relay_queue(helper,Kind::relay_reserve,"{"+relay_ids(client->reservation,join)+",\"candidate_id\":\""+candidate.str()+"\",\"validity_ms\":180000}"))continue;
        const auto wait_until=now_ms()+40000;while(!invitation_cancelled&&!helper->cancelled&&now_ms()<wait_until){{std::lock_guard<std::mutex> guard(client->mutex);if(client->ready)return client;if(client->failed)break;}vTaskDelay(pdMS_TO_TICKS(10));}
        relay_queue(helper,Kind::relay_close,"{"+relay_ids(client->reservation,join)+",\"reason\":\"cancelled\"}");
    }
    return {};
}
bool relay_exchange(const std::shared_ptr<RelayClient> &client,bool encrypted,std::string_view request,std::string &response){
    if(!client||request.empty()||request.size()>4096||client->active->cancelled||now_ms()>=client->deadline)return false;
    RelayFragment fragment;{std::lock_guard<std::mutex> guard(client->mutex);if(client->failed||client->sequence==UINT64_MAX)return false;client->response.reset();client->request=random_id<model::MessageId>();++client->sequence;fragment.reservation_id=client->reservation;fragment.join_id=client->join;fragment.request_id=client->request;fragment.sequence=client->sequence;fragment.encrypted=encrypted;fragment.total=request.size();}
    for(size_t offset=0;offset<request.size();offset+=2048){fragment.offset=offset;fragment.data=std::string(request.substr(offset,2048));if(!relay_queue(client->active,Kind::relay_record,relay_fragment_json(fragment,false)))return false;}
    const auto wait_until=std::min<uint64_t>(client->deadline,now_ms()+30000);
    while(!invitation_cancelled&&!client->active->cancelled&&now_ms()<wait_until){
        {std::lock_guard<std::mutex> guard(client->mutex);if(client->failed)return false;if(client->response.complete()){response=client->response.bytes();return true;}}
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}
void relay_close_client(const std::shared_ptr<RelayClient> &client,bool completed){if(!client)return;relay_queue(client->active,Kind::relay_close,"{"+relay_ids(client->reservation,client->join)+",\"reason\":\""+(completed?"completed":"cancelled")+"\"}");std::lock_guard<std::mutex> guard(relay_lock);if(relay_client==client)relay_client.reset();}
