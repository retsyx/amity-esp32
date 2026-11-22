// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
// Included in runtime.cpp's private namespace. No destructive request survives
// the current TLS link, target boot, or its original two-second deadline.
struct RemovalOperation {
    model::OctopusId owner;model::MessageId id;uint64_t until=0;
    std::string reset="pending";bool complete=false;
    // The already-admitted reset keeps its original authenticated scope while
    // this same Remove transaction publishes the saved-configuration deletion.
    std::optional<LinkScope> scope={};
};
std::vector<RemovalOperation> removal_operations;
void removal_expire(uint64_t now){
    for(auto& operation:removal_operations)if(!operation.complete&&now>=operation.until){operation.complete=true;operation.reset="unconfirmed";}
    while(std::count_if(removal_operations.begin(),removal_operations.end(),[](const auto& value){return value.complete;})>8){
        const auto old=std::find_if(removal_operations.begin(),removal_operations.end(),[](const auto& value){return value.complete;});
        removal_operations.erase(old);
    }
}
void removal_finish(model::OctopusId owner,model::MessageId id,bool acknowledged){
    std::lock_guard<std::mutex> guard(lock);
    for(auto& operation:removal_operations)if(operation.owner==owner&&operation.id==id&&!operation.complete){
        operation.complete=true;operation.reset=acknowledged?"scheduled":"unconfirmed";break;
    }
    removal_expire(now_ms());
}
