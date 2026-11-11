// Private bounded settings management. Included inside runtime's private namespace.
RuntimeSettingsHooks settings_hooks;
struct SettingsCache {
    model::OctopusId owner;model::BootId boot;model::MessageId read_id;model::LinkId read_link;
    std::string snapshot,error;uint64_t received=0,attempted=0,read_until=0;
};
struct SettingsOperation {
    model::OctopusId owner;model::MessageId id;model::BootId target_boot;model::LinkId link;
    std::string core,stage="admitted",error;uint64_t expires=0;
    std::optional<std::string> hostname{};
};
struct SettingsReceived {
    model::MessageId id;model::BootId issuer_boot;std::string core,response;bool pending=true;
};
struct SettingsAuthorization {
    model::MessageId id;bool local=false;uint64_t expires=0,config=0;
    model::LinkId execution_link;std::weak_ptr<Active> link;
};
std::vector<SettingsCache> settings_caches;
std::vector<SettingsOperation> settings_operations;
std::vector<SettingsReceived> settings_received;
SettingsAuthorization settings_authorization;
// Caller holds the runtime lock. An admitted rename reserves its name until
// its outcome is known, including against a concurrent Add operation.
bool settings_hostname_pending(model::OctopusId owner,const std::string& name){
    for(const auto& operation:settings_operations)if(operation.owner!=owner&&operation.stage=="admitted"&&operation.hostname&&*operation.hostname==name)return true;
    return false;
}
std::string settings_quote(std::string_view text){Json value;value.type=Json::Type::string;value.string=text;return settings_json(value);}
std::string settings_result(model::MessageId id,std::string_view operation,std::string_view stage,std::string_view error,std::string_view snapshot="null"){
    return "{\"request_id\":\""+id.str()+"\",\"operation\":"+settings_quote(operation)+",\"stage\":"+settings_quote(stage)+",\"error\":"+settings_quote(error)+",\"snapshot\":"+std::string(snapshot)+",\"rebooting\":false}";
}
const char* settings_error(Error result){switch(result){case Error::stale_boot:return "STALE_BOOT";case Error::transaction_conflict:case Error::stale_config:return "CONFLICT";case Error::expired:return "EXPIRED";case Error::storage_failure:return "STORAGE_FAILURE";case Error::queue_full:return "QUEUE_FULL";case Error::invalid_message:return "INVALID_MESSAGE";default:return "NOT_READY";}}
const char* settings_message(std::string_view error){if(error=="STALE_BOOT")return "This device restarted. Open its settings again.";if(error=="CONFLICT")return "Settings changed. Open the settings again.";if(error=="EXPIRED")return "The request expired. Open the settings again.";if(error=="STORAGE_FAILURE")return "The device could not save its settings.";if(error=="QUEUE_FULL")return "The device is busy. Try again shortly.";if(error=="INVALID_MESSAGE")return "Check the device settings.";return "The device is not ready. Open its settings again.";}
bool settings_enrolled(model::OctopusId owner){for(const auto& peer:group.peers)if(peer.id==owner&&peer.active)return true;return false;}
struct SettingsLink {std::shared_ptr<Active> active;RuntimePeerStatus status;};
SettingsLink settings_link(model::OctopusId owner){
    for(auto& active:links)if(active->peer==owner&&!active->cancelled&&!active->retiring&&!active->restart_requested&&!active->execution_fenced&&current_peer_credential(*active,group)&&(active->ready||active->management_ready)){
        std::lock_guard<std::mutex> snapshot(active->mutex);if(active->status.config_revision==configuration&&active->status.boot_id)return {active,active->status};
    }
    return {};
}
SettingsCache* settings_cache(model::OctopusId owner,bool create){
    for(auto& item:settings_caches)if(item.owner==owner)return &item;
    if(!create)return nullptr;
    if(settings_caches.size()>=3){const auto obsolete=std::find_if(settings_caches.begin(),settings_caches.end(),[](const auto& item){return item.owner!=identity.id&&!settings_enrolled(item.owner);});if(obsolete==settings_caches.end())return nullptr;settings_caches.erase(obsolete);}
    settings_caches.push_back({});settings_caches.back().owner=owner;return &settings_caches.back();
}
void settings_expire(uint64_t now){
    for(auto& item:settings_operations)if(item.stage=="admitted"&&now>=item.expires){item.stage="unknown";item.error="The result was not confirmed. The request will not be repeated.";}
    while(std::count_if(settings_operations.begin(),settings_operations.end(),[](const auto& item){return item.stage!="admitted";})>8){auto old=std::find_if(settings_operations.begin(),settings_operations.end(),[](const auto& item){return item.stage!="admitted";});settings_operations.erase(old);}
    while(std::count_if(settings_received.begin(),settings_received.end(),[](const auto& item){return !item.pending;})>8){auto old=std::find_if(settings_received.begin(),settings_received.end(),[](const auto& item){return !item.pending;});settings_received.erase(old);}
}
bool settings_authorized_locked(model::MessageId id){
    const auto& permit=settings_authorization;const auto now=now_ms();
    if(!id||permit.id!=id||now>=permit.expires||configuration!=permit.config)return false;
    if(permit.local)return original_controller(group,identity.id)&&status.authority_ready;
    auto active=permit.link.lock();if(!active||active->cancelled||active->retiring||active->restart_requested||active->execution_fenced||!active->ready||group.role!=Group::member||group.authority!=active->peer||!current_peer_credential(*active,group)||active->session.scope().link_id!=permit.execution_link)return false;
    // The TLS worker reserved this request's original message deadline.
    // Cancellation, role/configuration or logical-session changes fence it
    // here, including a restart on the same TLS object.
    return true;
}
// Returns a validated wire result. The owner service must recheck the exact
// permit through runtime_device_settings_authorized immediately before NVS I/O.
std::string settings_apply(const std::shared_ptr<Active>& active,model::MessageId id,const Json& core,uint64_t deadline){
    const auto encoded=settings_json(core);const auto issuer=active?active->session.scope().peer_boot_id:boot;RuntimeSettingsHooks hooks;
    {std::lock_guard<std::mutex> guard(lock);settings_expire(now_ms());
        for(const auto& old:settings_received)if(old.id==id&&old.issuer_boot==issuer){if(old.core!=encoded)return settings_result(id,"write","failed","INVALID_MESSAGE");return old.response;}
        if(settings_authorization.id||settings_received.size()>=9)return settings_result(id,"write","failed","QUEUE_FULL");
        settings_received.push_back({id,issuer,encoded,settings_result(id,"write","admitted","")});
        settings_authorization={id,!active,deadline,configuration,active?active->session.scope().link_id:model::LinkId{},active};hooks=settings_hooks;
        if(!settings_authorized_locked(id)){settings_authorization={};settings_received.back().pending=false;settings_received.back().response=settings_result(id,"write","failed","EXPIRED");return settings_received.back().response;}
    }
    std::string response;auto result=hooks.apply?hooks.apply(id,core,deadline,response):Error::not_ready;
    Json returned;JsonError detail;bool applied=false;std::string wire;
    if(parse_json(response,returned,detail,4096)&&closed(returned,{"request_id","stage","error","snapshot"})&&returned.get("request_id")->type==Json::Type::string&&returned.get("request_id")->string==id.str()&&returned.get("stage")->type==Json::Type::string&&returned.get("error")->type==Json::Type::string){
        const auto stage=returned.get("stage")->string;applied=result==Error::none&&stage=="applied";
        wire=settings_result(id,"write",applied?"applied":"failed",applied?"":settings_error(result),applied?settings_json(*returned.get("snapshot")):"null");
    }
    if(wire.empty())wire=settings_result(id,"write","failed",settings_error(result==Error::none?Error::invalid_message:result));
    {std::lock_guard<std::mutex> guard(lock);settings_authorization={};for(auto& saved:settings_received)if(saved.id==id&&saved.issuer_boot==issuer){saved.pending=false;saved.response=wire;}settings_expire(now_ms());}
    return wire;
}
bool settings_dispatch(const std::shared_ptr<Active>& active,const Envelope& envelope,uint64_t received){
    if(!is_settings_kind(envelope.kind))return false;
    if(active->session.admit(envelope,received,received)!=Error::none)return false;
    model::MessageId id;if(!consortium::id(envelope.payload.get("request_id"),id))return false;
    if(envelope.kind==Kind::settings_result){
        std::lock_guard<std::mutex> guard(lock);if(!original_controller(group,identity.id)||!status.authority_ready||!settings_enrolled(active->peer))return false;
        const auto operation=envelope.payload.get("operation")->string;const auto stage=envelope.payload.get("stage")->string;
        if(operation=="read"){
            auto* cache=settings_cache(active->peer,false);if(!cache||cache->read_id!=id||cache->read_link!=active->session.scope().link_id)return true;
            cache->read_id={};cache->read_until=0;
            if(stage=="applied"&&envelope.payload.get("snapshot")->get("boot_id")->string==active->session.scope().peer_boot_id.str()){cache->snapshot=settings_json(*envelope.payload.get("snapshot"));cache->boot=active->session.scope().peer_boot_id;cache->received=received;cache->error.clear();remember_live_hostname(*active,envelope.payload.get("snapshot")->get("hostname")->string);}
            else cache->error=settings_message(envelope.payload.get("error")->string);
        }else for(auto& pending:settings_operations)if(pending.owner==active->peer&&pending.id==id&&pending.link==active->session.scope().link_id&&pending.target_boot==active->session.scope().peer_boot_id&&pending.stage=="admitted"){
            if(stage=="admitted")return true;
            if(stage=="applied"&&envelope.payload.get("snapshot")->get("boot_id")->string!=pending.target_boot.str())return false;
            pending.stage=stage=="applied"?"applied":"failed";pending.error=stage=="applied"?"":settings_message(envelope.payload.get("error")->string);
            if(stage=="applied"){remember_live_hostname(*active,envelope.payload.get("snapshot")->get("hostname")->string);auto* cache=settings_cache(active->peer,true);if(cache){cache->snapshot=settings_json(*envelope.payload.get("snapshot"));cache->boot=pending.target_boot;cache->received=received;cache->error.clear();}}break;
        }
        settings_expire(received);return true;
    }
    RuntimeSettingsHooks hooks;{std::lock_guard<std::mutex> guard(lock);if(group.role!=Group::member||group.authority!=active->peer||!current_peer_credential(*active,group)||(!active->ready&&!active->management_ready))return false;hooks=settings_hooks;}
    if(envelope.kind==Kind::settings_read){const auto snapshot=hooks.snapshot?hooks.snapshot():"";return send(*active,Kind::settings_result,settings_result(id,"read",snapshot.empty()?"failed":"applied",snapshot.empty()?"NOT_READY":"",snapshot.empty()?"null":snapshot),2000);}
    DeviceSettingsWrite write;
    if(!id.valid()||parse_device_settings_write(*envelope.payload.get("settings"),write)!=Error::none||write.expected_boot!=boot)return false;
    if(received>UINT64_MAX-envelope.ttl_ms)return false;
    const auto response=settings_apply(active,id,*envelope.payload.get("settings"),received+envelope.ttl_ms);
    return send(*active,Kind::settings_result,response,2000);
}
