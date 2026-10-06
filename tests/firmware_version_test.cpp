#include "firmware_version.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
int main(int argc,char **argv) {
    assert(argc==2);std::ifstream input(argv[1]);assert(input);
    std::string line;size_t count=0;
    while(std::getline(input,line)) {
        auto first=line.find('\t'),second=line.find('\t',first+1);
        assert(first!=line.npos&&second!=line.npos);
        auto result=system_services::compare_firmware_versions(line.substr(0,first),line.substr(first+1,second-first-1));
        auto expected=line.substr(second+1);
        assert((result?std::to_string(*result):"unknown")==expected);++count;
    }
    std::cout<<"PASS "<<count<<" firmware version comparisons\n";
}
