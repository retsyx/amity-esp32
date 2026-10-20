#pragma once
#include "system_configuration.hpp"
namespace config_store {
struct RequestedSession {model::ZoneId zone_id;model::ActivityId activity_id;};
// Logical requests only. This record deliberately has no session/plan IDs,
// bindings, address, outcome, hold, grant, lease, or executable queue.
struct CoordinatorIntent {
    model::ConsortiumId consortium_id;uint64_t revision=0;
    std::vector<RequestedSession> last_requested_sessions;
};
class CoordinatorIntentJournal {
public:
    explicit CoordinatorIntentJournal(ConfigBackend& backend):backend_(backend){}
    ConfigResult recover(CoordinatorIntent&,std::string& error);
    ConfigResult request(model::ConsortiumId,model::ZoneId,std::optional<model::ActivityId>,std::string& error);
private:ConfigBackend& backend_;
};
esp_err_t remember_requested_session(model::ConsortiumId,model::ZoneId,std::optional<model::ActivityId>,std::string& error);
esp_err_t load_last_requested_sessions(model::ConsortiumId,std::vector<RequestedSession>&,std::string& error);
}
