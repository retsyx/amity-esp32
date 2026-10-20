#include "amity/consortium/bootstrap_record.hpp"
#include <cassert>
#include <iostream>
using namespace amity::consortium;
struct Cut{};
struct Store {
    std::string durable;int step=0,cut=-1;bool write_error=false,read_error=false;
    BootstrapReadResult read(std::string& value){if(read_error)return BootstrapReadResult::error;value=durable;if(++step==cut)throw Cut{};return BootstrapReadResult::found;}
    bool write(std::string_view bytes){if(++step==cut)throw Cut{};if(write_error)return false;durable=bytes;if(++step==cut)throw Cut{};return true;}
};
int main(){
    const std::string legacy=std::string(64,'a')+":"+std::string(768,'b');BootstrapRecord record;
    assert(decode_bootstrap_record(legacy,record)&&record.generation==0);
    for(int cut=1;cut<=4;++cut){Store store{legacy,0,cut};try{rotate_bootstrap_record(store,std::string(64,'c'),std::string(768,'d'),record);}catch(const Cut&){}
        BootstrapRecord recovered;assert(decode_bootstrap_record(store.durable,recovered));
        assert((recovered.generation==0&&store.durable==legacy)||(recovered.generation==1&&recovered.salt_hex==std::string(64,'c')&&recovered.verifier_hex==std::string(768,'d')));
        assert(!store.durable.empty()); // Every interrupted atomic cut retains one whole verifier, never a mix or empty origin.
    }
    Store store{legacy};assert(rotate_bootstrap_record(store,std::string(64,'c'),std::string(768,'d'),record)==Error::none&&record.generation==1);
    assert(rotate_bootstrap_record(store,std::string(64,'e'),std::string(768,'f'),record)==Error::none&&record.generation==2);
    assert(rotate_bootstrap_record(store,std::string(64,'e'),std::string(768,'f'),record)==Error::invalid_message);
    const auto prior=store.durable;assert(rotate_bootstrap_record(store,std::string(64,'z'),std::string(768,'f'),record)==Error::invalid_message&&store.durable==prior);
    store.write_error=true;assert(rotate_bootstrap_record(store,std::string(64,'0'),std::string(768,'1'),record)==Error::unknown_outcome&&store.durable==prior);
    store.write_error=false;store.read_error=true;assert(rotate_bootstrap_record(store,std::string(64,'0'),std::string(768,'1'),record)==Error::storage_failure);store.read_error=false;
    store.durable=encode_bootstrap_record({UINT64_MAX,std::string(64,'a'),std::string(768,'b')});assert(rotate_bootstrap_record(store,std::string(64,'0'),std::string(768,'1'),record)==Error::transaction_conflict);
    for(const auto& invalid:{std::string("2:01:")+legacy,std::string("2:18446744073709551616:")+legacy,std::string("3:1:")+legacy,legacy+":"})assert(!decode_bootstrap_record(invalid,record));
    std::cout<<"PASS bootstrap rotation generation, retained whole-record crash cuts, corruption and uncertain outcome\n";
}
