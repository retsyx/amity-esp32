#pragma once
#include "system_configuration.hpp"
namespace config_store {
struct CandidateNetwork {
    model::JoinId join_id;
    std::string proposal_digest,old_ssid,old_password,ssid,password,old_driver_configuration;
    uint8_t old_index=0;
    bool decided=false,use_candidate=true;
    ~CandidateNetwork();
};
class CandidateNetworkJournal {
public:
    explicit CandidateNetworkJournal(ConfigBackend& backend):backend_(backend){}
    ConfigResult recover(CandidateNetwork&,std::string& error);
    ConfigResult stage(const CandidateNetwork&,std::string& error);
    ConfigResult decide(model::JoinId,std::string_view digest,bool use_candidate,std::string& error);
    ConfigResult finish(model::JoinId,std::string_view digest,std::string& error);
    ConfigResult abandon(model::JoinId,std::string_view digest,std::string& error);
private:ConfigBackend& backend_;
};
esp_err_t load_candidate_network(CandidateNetwork&,std::string& error);
esp_err_t stage_candidate_network(const CandidateNetwork&,std::string& error);
esp_err_t decide_candidate_network(model::JoinId,const std::string&,bool use_candidate,std::string& error);
esp_err_t finish_candidate_network(model::JoinId,const std::string&,std::string& error);
esp_err_t abandon_candidate_network(model::JoinId,const std::string&,std::string& error);
}
