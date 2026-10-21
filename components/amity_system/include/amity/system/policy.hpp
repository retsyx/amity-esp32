#pragma once
#include "amity/system/discovery.hpp"

namespace amity::system {

enum class OperationKind { ensure_power, select_path, announce_active_source, press_key, release_key, query_state };
struct OperationArguments {
    bool on = true;
    uint8_t key = 0;
    uint16_t physical_path = 0xffff;
    uint8_t query_opcode = 0x8f;
    std::optional<HoldToken> hold_token;
    bool repeat = false;
};
struct AuthorizedCecOperation {
    CommandId command_id;
    OperationKind kind = OperationKind::query_state;
    ComponentId component_id;
    BindingId binding_id;
    FullyQualifiedCecAddress target;
    ExecutionFence fence;
    uint64_t execution_expires_at_ms = 0; // Issued by the receiver, never sender time.
    OperationArguments arguments;
};
struct BindingPermission {
    ComponentBinding binding;
    bool discrete_power_on = false;
    bool discrete_power_off = false;
    bool image_view_on = false;
    bool exact_path_selection = false;
};
struct RouteGrant {
    BindingId binding_id;
    SessionId session_id;
    uint64_t route_generation = 0;
    uint64_t expires_at_ms = 0;
    std::vector<uint16_t> permitted_paths;
};
struct PortPolicySnapshot {
    CecPortRef port;
    BootId owner_boot_id;
    DomainId domain_id;
    uint64_t domain_revision = 0, coordinator_epoch = 0, config_revision = 0, policy_generation = 0;
    uint64_t authority_expires_at_ms = 0;
    PortRole role = PortRole::disabled;
    bool qualified_cec20_backup_addresses = false;
    std::optional<uint8_t> owned_logical_address;
    std::optional<PhysicalAddressEvidence> physical_address;
    std::string proxy_osd_name = "Amity";
    std::vector<Component> components;
    std::vector<BindingPermission> bindings;
    std::vector<RouteGrant> route_grants;
};
struct CecFrame {
    CecPortRef port;
    uint8_t initiator = 15, destination = 15;
    std::vector<uint8_t> payload; // opcode and operands; header is separate.
};
enum class WireOutcome { success, nack, arbitration_lost, line_error, canceled, expired, admission_rejected };
enum class AdmissionCode { accepted, invalid, stale, expired, unauthorized, unsupported, overloaded, held_key_conflict };
struct AdmissionResult { AdmissionCode code; std::string reason; explicit operator bool() const { return code == AdmissionCode::accepted; } };
struct WireWork {
    CommandId command_id;
    CecFrame frame;
    bool emergency_release = false;
    BindingId binding_id;
    uint64_t binding_revision = 0;
    uint64_t expires_at_ms = 0;
    unsigned attempts = 3;
    uint64_t work_id = 0;
    bool requires_previous_start = false;
};
using WireSubmit = std::function<bool(const WireWork*, size_t)>;
using WireCancel = std::function<void(uint64_t)>;

// Default-deny semantic executor. All wire work, including automatic replies,
// passes an immediate validated handoff to the sole CEC driver FIFO.
class LocalCecAgent {
public:
    explicit LocalCecAgent(size_t maximum_holds = 16)
        : maximum_holds_(std::min<size_t>(maximum_holds, 16)) {}
    void set_wire_adapter(WireSubmit submit, WireCancel cancel) { submit_ = std::move(submit); cancel_ = std::move(cancel); }
    AdmissionResult install_policy(const PortPolicySnapshot&, uint64_t now_ms);
    AdmissionResult check_policy(const PortPolicySnapshot&, uint64_t now_ms) const;
    bool can_rebase_revision(uint64_t expected_revision, uint64_t next_revision) const;
    bool rebase_revision(uint64_t expected_revision, uint64_t next_revision);
    AdmissionResult execute(const AuthorizedCecOperation&, uint64_t now_ms);
    AdmissionResult renew_authority(const AuthorizedCecOperation& fresh_receiver_authority, uint64_t now_ms);
    AdmissionResult renew_discovery_authority(const CecPortRef&, uint32_t ttl_ms, uint64_t now_ms);
    bool observe(const CecFrame&, uint64_t now_ms);
    std::optional<CommandId> complete_frame(uint64_t work_id, WireOutcome, uint64_t now_ms, bool started = true);
    bool command_pending(CommandId) const;
    void quiesce();
    void tick(uint64_t now_ms);
    bool release_hold(HoldToken, uint64_t now_ms = 0);
    void fence_binding(BindingId, uint64_t old_revision, bool old_peer_still_identified = false);
    void close_route(SessionId, uint64_t old_generation);
    bool quiescent(SessionId, uint64_t old_generation) const;
    size_t pending_work() const;
    size_t held_keys() const { return holds_.size(); }
    std::vector<AuthorizedCecOperation> active_key_contexts() const;
    bool repeat_ready(uint64_t now_ms) const;
    bool repeating_frame_in_flight() const;
    size_t rejected_at_wire_start() const { return rejected_at_wire_start_; }
    size_t uncertain_releases() const { return uncertain_releases_; }
private:
    struct Transmission {
        uint64_t work_id = 0;
        AuthorizedCecOperation operation;
        CecPortRef port;
        bool automatic_identity = false, emergency_release = false;
        uint64_t identity_policy_generation = 0;
        bool automatic_route_reply = false;
        uint64_t stream_id = 0;
    };
    struct HeldKey {
        HoldToken token;
        AuthorizedCecOperation operation;
        uint8_t initiator = 15;
        bool started = false, active = false;
        ExecutionFence cleanup_fence;
        uint64_t rebased_revision = 0, rebased_policy_generation = 0;
        bool up_requested = false;
        uint64_t press_work_id = 0;
        uint64_t stream_id = 0;
        bool finite = false;
    };
    struct ReleaseStream {
        uint64_t stream_id = 0;
        AuthorizedCecOperation cleanup;
        uint8_t initiator = 15;
        bool started = false, ended = false, safe = true;
        uint64_t release_work_id = 0;
        std::optional<AuthorizedCecOperation> audio_context;
        bool admitted = false;
    };
    const PortPolicySnapshot* policy(const CecPortRef&) const;
    const BindingPermission* permission(const PortPolicySnapshot&, BindingId) const;
    AdmissionResult validate(const AuthorizedCecOperation&, uint64_t now_ms, bool internal_route_reply = false) const;
    std::optional<std::vector<CecFrame>> compile(const AuthorizedCecOperation&, const PortPolicySnapshot&, const BindingPermission&) const;
    std::optional<AuthorizedCecOperation> held_authority(const HeldKey&, uint64_t now_ms) const;
    void release_at(size_t index, bool safe, bool dispatch = true);
    void maintain(uint64_t now_ms);
    void close_route_impl(SessionId, uint64_t generation, bool dispatch);
    bool submit_frames(const AuthorizedCecOperation&, const std::vector<CecFrame>&, uint64_t now_ms,
                       bool automatic_identity = false, bool internal_reply = false, uint64_t stream_id = 0);
    bool flush_stream(uint64_t stream_id, uint64_t now_ms);
    void observe_stream_end(uint64_t stream_id);
    void discard_stream(uint64_t stream_id);
    void cancel_transmissions(const std::function<bool(const Transmission&)>&);
    void schedule_audio_query(const AuthorizedCecOperation&, uint64_t due_ms, uint64_t stream_id);
    bool closed(const AuthorizedCecOperation&) const;
    void prune_fences();
    size_t maximum_holds_, rejected_at_wire_start_ = 0, uncertain_releases_ = 0;
    std::vector<PortPolicySnapshot> policies_;
    std::vector<Transmission> transmissions_;
    WireSubmit submit_;
    WireCancel cancel_;
    uint64_t next_work_id_ = 0;
    std::vector<HeldKey> holds_;
    std::vector<ReleaseStream> streams_;
    uint64_t next_stream_id_ = 0;
    struct AudioQuery {
        AuthorizedCecOperation operation;
        uint64_t due_ms, stream_id;
        bool press_started;
    };
    std::vector<AudioQuery> audio_queries_;
    uint64_t now_ms_ = 0;
    std::vector<std::pair<BindingId, uint64_t>> fenced_bindings_;
    std::vector<std::pair<SessionId, uint64_t>> closed_routes_;
    bool fail_closed_ = false;
};

} // namespace amity::system
