// Private log pulls. Included inside runtime's private namespace.
RuntimeLogHooks log_hooks;
// Only one collector can own this buffer, independent of the roster size.
EXT_RAM_BSS_ATTR std::array<char,log_chunk_bytes> log_chunk{};
struct LogPull {
    bool busy=false,waiting=false,received=false;
    model::MessageId transfer;
    model::LinkId link;
    std::weak_ptr<Active> active;
    size_t offset=0,total=0,size=0;
    esp_err_t result=ESP_OK;
} log_pull;

bool log_link_available(const Active& active){
    return !active.cancelled&&!active.retiring&&!active.restart_requested&&!active.execution_fenced&&
        (active.ready||active.management_ready)&&current_peer_credential(active,group);
}
void log_snapshot_clear(Active& active){
    active.log_snapshot={};active.log_transfer={};active.log_next=0;active.log_expires=0;
}
// Called only by the link task. Application errors are ordinary results and
// never invalidate an independently healthy authenticated connection.
bool log_dispatch(const std::shared_ptr<Active>& active,const Envelope& envelope,uint64_t received){
    model::MessageId transfer;
    uint64_t offset=0;
    if(!id(envelope.payload.get("transfer_id"),transfer)||!json_uint(envelope.payload.get("offset"),offset,0,log_capacity))return false;
    RuntimeLogHooks hooks;
    {
        std::lock_guard<std::mutex> guard(lock);
        if(!log_link_available(*active))return true;
        if(envelope.kind==Kind::log_result){
            if(!original_controller(group,identity.id)||!status.authority_ready||!settings_enrolled(active->peer))return false;
            if(!log_pull.busy||!log_pull.waiting||log_pull.active.lock()!=active||log_pull.transfer!=transfer||
               log_pull.link!=active->session.scope().link_id||log_pull.offset!=offset)return true;
            uint64_t total=0;json_uint(envelope.payload.get("total"),total,0,log_capacity);
            const auto& data=envelope.payload.get("data")->string;
            const auto& error=envelope.payload.get("error")->string;
            log_pull.result=error.empty()?ESP_OK:error=="expired"?ESP_ERR_TIMEOUT:ESP_ERR_INVALID_STATE;
            if(error.empty()&&offset&&total!=log_pull.total)log_pull.result=ESP_FAIL;
            log_pull.total=total;log_pull.size=data.size()/2;
            auto nibble=[](char c){return c<='9'?c-'0':c-'a'+10;};
            for(size_t i=0;i<log_pull.size;++i)log_chunk[i]=static_cast<char>((nibble(data[2*i])<<4)|nibble(data[2*i+1]));
            log_pull.received=true;log_pull.waiting=false;return true;
        }
        if(group.role!=Group::member||group.authority!=active->peer)return false;
        hooks=log_hooks;
    }
    if(envelope.payload.get("close")->boolean){
        if(active->log_transfer==transfer)log_snapshot_clear(*active);
        return true;
    }
    if(active->log_snapshot&&received>=active->log_expires)log_snapshot_clear(*active);
    const char* error="";
    if(active->log_transfer!=transfer){
        if(offset)error="expired";
        else if(active->log_snapshot)error="busy";
        else{
            active->log_snapshot=hooks.snapshot?hooks.snapshot():LogSnapshot{};
            if(!active->log_snapshot||active->log_snapshot.size>log_capacity){log_snapshot_clear(*active);error="unavailable";}
            else{active->log_transfer=transfer;active->log_next=0;active->log_expires=received+15000;}
        }
    }
    if(!*error&&offset!=active->log_next)error="expired";
    if(*error)return send(*active,Kind::log_result,log_result_json(transfer,offset,0,{},error),2000);
    const auto total=active->log_snapshot.size;
    const auto size=std::min(log_chunk_bytes,total-static_cast<size_t>(offset));
    const auto payload=log_result_json(transfer,offset,total,std::string_view(active->log_snapshot.bytes.get()+offset,size));
    active->log_next+=size;
    // The final frame owns its small encoded copy; release the PSRAM snapshot
    // before transmitting it, even if the browser never receives that frame.
    if(active->log_next==total)log_snapshot_clear(*active);
    return send(*active,Kind::log_result,payload,2000);
}

esp_err_t pull_logs(model::OctopusId owner,const std::function<bool(std::string_view)>& consume){
    if(!owner||!consume)return ESP_ERR_INVALID_ARG;
    std::shared_ptr<Active> active;
    const auto transfer=random_id<model::MessageId>();
    model::LinkId link;
    uint64_t revision=0;
    {
        std::lock_guard<std::mutex> guard(lock);
        if(log_pull.busy||!original_controller(group,identity.id)||!status.authority_ready||!settings_enrolled(owner))return ESP_ERR_INVALID_STATE;
        for(const auto& candidate:links)if(candidate->peer==owner&&log_link_available(*candidate)){
            std::lock_guard<std::mutex> snapshot(candidate->mutex);
            if(!(candidate->status.negotiated_capabilities&peer_logs))return ESP_ERR_NOT_SUPPORTED;
            active=candidate;link=candidate->session.scope().link_id;revision=candidate->status.config_revision;break;
        }
        if(!active)return ESP_ERR_INVALID_STATE;
        log_pull={};log_pull.busy=true;log_pull.transfer=transfer;log_pull.link=link;log_pull.active=active;
    }
    // All exits cancel unsent requests and release the collector. Cancellation
    // travels on the same worker-owned stream; its loss is bounded by expiry.
    struct Finish {
        std::shared_ptr<Active> active;model::MessageId transfer;model::LinkId link;uint64_t revision;
        ~Finish(){
            std::lock_guard<std::mutex> guard(lock);std::lock_guard<std::mutex> queue(active->mutex);
            const auto token=transfer.str();
            active->queue.erase(std::remove_if(active->queue.begin(),active->queue.end(),[&](const auto& item){
                return item.kind==Kind::log_request&&item.payload.find(token)!=std::string::npos;
            }),active->queue.end());
            if(log_link_available(*active)&&active->session.scope().link_id==link&&active->queue.size()<15)
                active->queue.push_back({Kind::log_request,log_request_json(transfer,0,true),2000,now_ms(),revision,{}});
            log_pull={};
        }
    } finish{active,transfer,link,revision};
    const auto until=now_ms()+15000;
    size_t offset=0;
    while(now_ms()<until){
        const auto chunk_until=std::min<uint64_t>(until,now_ms()+2000);
        {
            std::lock_guard<std::mutex> guard(lock);std::lock_guard<std::mutex> queue(active->mutex);
            if(!original_controller(group,identity.id)||!status.authority_ready||!settings_enrolled(owner)||
               !log_link_available(*active)||active->session.scope().link_id!=link||active->status.config_revision!=revision)return ESP_ERR_INVALID_STATE;
            if(active->queue.size()>=15)return ESP_ERR_NO_MEM;
            const auto queued_at=now_ms();
            if(queued_at>=chunk_until)return ESP_ERR_TIMEOUT;
            log_pull.offset=offset;log_pull.waiting=true;log_pull.received=false;
            active->queue.push_back({Kind::log_request,log_request_json(transfer,offset,false),static_cast<uint32_t>(chunk_until-queued_at),queued_at,revision,{}});
        }
        bool received=false;
        size_t size=0,total=0;
        esp_err_t result=ESP_OK;
        while(now_ms()<chunk_until){
            {
                std::lock_guard<std::mutex> guard(lock);
                if(!log_link_available(*active)||active->session.scope().link_id!=link)return ESP_ERR_INVALID_STATE;
                if(log_pull.received){received=true;size=log_pull.size;total=log_pull.total;result=log_pull.result;break;}
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if(!received)return ESP_ERR_TIMEOUT;
        if(result!=ESP_OK)return result;
        if(size&&!consume(std::string_view(log_chunk.data(),size)))return ESP_FAIL;
        offset+=size;
        if(offset==total)return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}
