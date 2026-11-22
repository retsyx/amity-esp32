// Private log pulls. Included inside runtime's private namespace.
RuntimeLogHooks log_hooks;
// One collector at a time. Its buffer holds a whole member log, so the
// member streams without waiting for the browser to read each chunk.
struct LogPull {
    bool busy=false,done=false;
    model::MessageId transfer;
    model::LinkId link;
    std::weak_ptr<Active> active;
    std::unique_ptr<char[]> bytes;
    size_t received=0,total=0;
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
    RuntimeLogHooks hooks;
    model::MessageId transfer;
    {
        std::lock_guard<std::mutex> guard(lock);
        if(!log_link_available(*active))return true;
        if(envelope.kind==Kind::log_result){
            if(!original_controller(group,identity.id)||!status.authority_ready||!settings_enrolled(active->peer)||!envelope.log_chunk)return false;
            const auto& chunk=*envelope.log_chunk;
            // A chunk of an ended or different pull is late, not an error.
            if(!log_pull.busy||log_pull.done||log_pull.active.lock()!=active||log_pull.transfer!=chunk.transfer||
               log_pull.link!=active->session.scope().link_id)return true;
            if(chunk.unavailable)log_pull.result=ESP_ERR_NO_MEM;
            else if(chunk.offset!=log_pull.received||(log_pull.received&&chunk.total!=log_pull.total))log_pull.result=ESP_FAIL;
            else{
                std::memcpy(log_pull.bytes.get()+chunk.offset,chunk.bytes.data(),chunk.bytes.size());
                log_pull.total=chunk.total;log_pull.received+=chunk.bytes.size();
                if(log_pull.received<log_pull.total)return true;
            }
            log_pull.done=true;return true;
        }
        if(group.role!=Group::member||group.authority!=active->peer||!id(envelope.payload.get("transfer_id"),transfer))return false;
        hooks=log_hooks;
    }
    // A new request replaces an unfinished stream. The link loop sends the
    // snapshot taken now, one chunk per pass.
    log_snapshot_clear(*active);
    active->log_snapshot=hooks.snapshot?hooks.snapshot():LogSnapshot{};
    if(active->log_snapshot.size>log_capacity)active->log_snapshot={};
    active->log_transfer=transfer;active->log_expires=received+15000;
    return true;
}
// Link task only: sends the next chunk of the current log stream, reporting
// a missing snapshot once as unavailable.
bool send_log_chunk(Active& active){
    if(!active.log_transfer)return true;
    LogChunk chunk;chunk.transfer=active.log_transfer;chunk.offset=static_cast<uint32_t>(active.log_next);chunk.unavailable=!active.log_snapshot;
    if(active.log_snapshot){
        chunk.total=static_cast<uint32_t>(active.log_snapshot.size);
        const auto size=std::min(log_chunk_bytes,active.log_snapshot.size-active.log_next);
        chunk.bytes.assign(active.log_snapshot.bytes.get()+active.log_next,size);active.log_next+=size;
    }
    // The final chunk owns its copy; release the PSRAM snapshot before sending it.
    if(chunk.unavailable||active.log_next==chunk.total)log_snapshot_clear(active);
    return send(active,Kind::log_result,encode_log_chunk(chunk),2000);
}

esp_err_t pull_logs(model::OctopusId owner,const std::function<bool(std::string_view)>& consume){
    if(!owner||!consume)return ESP_ERR_INVALID_ARG;
    std::unique_ptr<char[]> buffer(new (std::nothrow) char[log_capacity]);
    if(!buffer)return ESP_ERR_NO_MEM;
    std::shared_ptr<Active> active;
    const auto transfer=random_id<model::MessageId>();
    model::LinkId link;
    {
        std::lock_guard<std::mutex> guard(lock);
        if(log_pull.busy||!original_controller(group,identity.id)||!status.authority_ready||!settings_enrolled(owner))return ESP_ERR_INVALID_STATE;
        for(const auto& candidate:links)if(candidate->peer==owner&&log_link_available(*candidate)){
            std::lock_guard<std::mutex> queue(candidate->mutex);
            if(candidate->queue.size()>=15)return ESP_ERR_NO_MEM;
            active=candidate;link=candidate->session.scope().link_id;
            candidate->queue.push_back({Kind::log_request,log_request_json(transfer),2000,now_ms(),candidate->status.config_revision,{}});wake(*candidate);
            break;
        }
        if(!active)return ESP_ERR_INVALID_STATE;
        log_pull={};log_pull.busy=true;log_pull.transfer=transfer;log_pull.link=link;log_pull.active=active;log_pull.bytes=std::move(buffer);
    }
    // Every exit releases the collector and drops an unsent request. Chunks
    // that arrive later belong to no pull and are ignored.
    struct Finish {
        std::shared_ptr<Active> active;std::string token;
        ~Finish(){
            std::lock_guard<std::mutex> guard(lock);std::lock_guard<std::mutex> queue(active->mutex);
            active->queue.erase(std::remove_if(active->queue.begin(),active->queue.end(),[&](const auto& item){
                return item.kind==Kind::log_request&&item.payload.find(token)!=std::string::npos;
            }),active->queue.end());
            log_pull={};
        }
    } finish{active,transfer.str()};
    const auto until=now_ms()+15000;
    auto progress=now_ms();
    size_t consumed=0;
    for(;;){
        const char* bytes=nullptr;size_t received=0;bool done=false;esp_err_t result=ESP_OK;
        {
            std::lock_guard<std::mutex> guard(lock);
            if(!log_link_available(*active)||active->session.scope().link_id!=link)return ESP_ERR_INVALID_STATE;
            bytes=log_pull.bytes.get();received=log_pull.received;done=log_pull.done;result=log_pull.result;
        }
        // Received bytes are never rewritten, so they are read outside the lock.
        for(;consumed<received;){
            const auto size=std::min(log_chunk_bytes,received-consumed);
            if(!consume(std::string_view(bytes+consumed,size)))return ESP_FAIL;
            consumed+=size;progress=now_ms();
        }
        if(done)return result;
        if(now_ms()>=until||now_ms()-progress>=2000)return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
