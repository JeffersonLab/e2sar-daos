/**
    Baisc DAOS version test.

    First checked in by xmei@jlab.org on Mar/26/2024.
*/

#define BOOST_TEST_MODULE DaosVersionTest
#include <boost/test/included/unit_test.hpp>
#include <daos.h>

// Test whether DAOS version is 2.x.x.
// This ensures <daos.h> can be correctly compiled and linked.
BOOST_AUTO_TEST_CASE(VersionAssertion) {
    const int expected_major = 2;
    const int actual_major = DAOS_VERSION_MAJOR;

    BOOST_REQUIRE_MESSAGE(
        actual_major == expected_major,
        "DAOS version mismatch: expected major version "
            << expected_major << ".x.x, but got "
            << actual_major << "." << DAOS_VERSION_MINOR << "." << DAOS_VERSION_FIX
            << " from the installed DAOS headers.");
}
