#include "apple_fabric_cleanup.hpp"
#include <cassert>
#include <cstdio>

using amity_matter::AppleFabricCleanup;
using amity_matter::apple_home_vendor;
using amity_matter::apple_keychain_vendor;

int main()
{
    {
        AppleFabricCleanup policy;
        policy.committed(1, apple_keychain_vendor);
        policy.committed(2, apple_home_vendor);
        policy.committed(3, 0x6006);
        assert(policy.will_remove(2)); // Persist before removing the last Home.
        assert(!policy.pending());    // Do not delete Keychain within this callback.
        assert(policy.removed(2));
        assert(policy.pending() && !policy.has_home());
        assert(!policy.will_remove(1));
        assert(!policy.removed(1));    // Cleaning Keychain does not recurse.
        policy.completed();
        assert(!policy.pending());
        assert(!policy.removed(2));    // Duplicate notification is harmless.
    }
    {
        AppleFabricCleanup policy;
        policy.committed(1, apple_keychain_vendor);
        policy.committed(2, apple_home_vendor);
        policy.committed(254, apple_home_vendor);
        assert(!policy.will_remove(2));
        assert(!policy.removed(2));
        assert(policy.has_home() && !policy.pending());
        assert(policy.will_remove(254));
        assert(policy.removed(254));
        assert(policy.pending() && !policy.has_home());
    }
    {
        // A failed, uncommitted Apple Home pairing must leave Keychain intact.
        AppleFabricCleanup policy;
        policy.committed(7, apple_keychain_vendor);
        assert(!policy.will_remove(8));
        assert(!policy.removed(8));
        assert(!policy.pending());
    }
    {
        // The stored marker resumes cleanup after a reset between callbacks.
        AppleFabricCleanup policy;
        policy.restore_pending(true);
        policy.committed(10, apple_keychain_vendor);
        policy.committed(11, 0x6006);
        assert(policy.pending() && !policy.has_home());
    }
    {
        // A reset before deletion, or a new Home committed before deferred
        // cleanup executes, must preserve Keychain.
        AppleFabricCleanup policy;
        policy.restore_pending(true);
        policy.committed(10, apple_home_vendor);
        assert(policy.pending() && policy.has_home());
        policy.completed();
        assert(!policy.pending());
        assert(policy.will_remove(10));
        assert(policy.removed(10));
        policy.committed(11, apple_home_vendor);
        assert(policy.pending() && policy.has_home());
    }
    {
        AppleFabricCleanup policy;
        policy.committed(2, apple_home_vendor);
        policy.committed(2, 0x6006); // An index reused by a different platform.
        assert(!policy.has_home());
        assert(!policy.will_remove(2));
        assert(!policy.removed(2));
        policy.committed(3, apple_keychain_vendor);
        assert(!policy.will_remove(3));
        assert(!policy.removed(3));
        assert(!policy.pending());
        for (uint8_t invalid : {uint8_t(0), uint8_t(255)}) {
            policy.committed(invalid, apple_home_vendor);
            assert(!policy.will_remove(invalid));
            assert(!policy.removed(invalid));
        }
        assert(!policy.has_home() && !policy.pending());
    }
    std::puts("Apple fabric cleanup tests passed");
}
