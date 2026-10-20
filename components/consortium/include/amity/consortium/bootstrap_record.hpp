#pragma once
#include "amity/consortium/wire.hpp"
namespace amity::consortium {
struct BootstrapRecord { uint64_t generation=0; std::string salt_hex,verifier_hex; };
enum class BootstrapReadResult { found,missing,error };
bool decode_bootstrap_record(std::string_view,BootstrapRecord&);
std::string encode_bootstrap_record(const BootstrapRecord&);
// Store writes one atomic retained-origin blob. It must not erase the previous
// value first. Uncertain write/readback closes enrollment until physical retry.
template<class Store> Error rotate_bootstrap_record(Store& store,std::string_view salt,std::string_view verifier,BootstrapRecord& committed){
    std::string bytes;const auto read=store.read(bytes);
    if(read==BootstrapReadResult::missing)return Error::not_ready;
    if(read!=BootstrapReadResult::found)return Error::storage_failure;
    BootstrapRecord old;if(!decode_bootstrap_record(bytes,old))return Error::storage_failure;
    if(old.generation==UINT64_MAX)return Error::transaction_conflict;
    BootstrapRecord next{old.generation+1,std::string(salt),std::string(verifier)};
    const auto encoded=encode_bootstrap_record(next);
    if(encoded.empty()||(old.salt_hex==next.salt_hex&&old.verifier_hex==next.verifier_hex))return Error::invalid_message;
    if(!store.write(encoded))return Error::unknown_outcome;
    std::string actual;
    if(store.read(actual)!=BootstrapReadResult::found||actual!=encoded)return Error::unknown_outcome;
    BootstrapRecord verified;if(!decode_bootstrap_record(actual,verified))return Error::unknown_outcome;
    committed=std::move(verified);return Error::none;
}
}
