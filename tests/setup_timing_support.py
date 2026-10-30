"""Shared ordinary-log seam for native tests of the actual timed functions."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def timing_support():
    return '#include "' + str(ROOT / 'components/consortium/setup_timing.hpp') + '"\n' + r'''
#include <vector>
namespace amity::consortium::platform {
struct RecordedSetupTiming {const char *area;SetupTimingEntry entry;model::OctopusId peer;model::LinkId link;model::MessageId scan;};
inline std::vector<RecordedSetupTiming> recorded_setup_timings;
inline void emit_setup_timing(const char *area,const SetupTimingEntry& entry,model::OctopusId peer,model::LinkId link,model::MessageId scan){recorded_setup_timings.push_back({area,entry,peer,link,scan});}
}
using amity::consortium::platform::SetupTimings;
using amity::consortium::platform::BoundedSetupTimings;
'''
