/**
    Toy DAOS KV round-trip test.

    Writes 5 random 0.5 MiB values into one DAOS KV object, reads them back,
    and checks that the SHA-256 of each value read back matches the one written.

    Needs a running DAOS agent/server and an existing pool and container:
        dmg pool create sc26 --size=<size>      # admin, once
        daos cont create sc26 kv-test           # once
    Pool and container labels are read from DAOS_POOL and DAOS_CONT
    (defaults: "sc26" and "kv-test").
*/

#define BOOST_TEST_MODULE DaosKVTest
#include <boost/test/included/unit_test.hpp>
#include <daos.h>
#include <openssl/evp.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

constexpr int kNumValues = 5;
constexpr size_t kValueSize = 512 * 1024;  // 0.5 MiB

std::string envOr(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && v[0] != '\0') ? std::string(v) : std::string(fallback);
}

// Hex SHA-256 digest, same format as `sha256sum`.
std::string sha256Hex(const void* data, size_t len) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdLen = 0;
    if (EVP_Digest(data, len, md, &mdLen, EVP_sha256(), nullptr) != 1)
        return "";
    std::string hex;
    char byte[3];
    for (unsigned int i = 0; i < mdLen; i++) {
        std::snprintf(byte, sizeof(byte), "%02x", md[i]);
        hex += byte;
    }
    return hex;
}

// Connects to the pool/container and opens a KV object on setup;
// tears everything down on exit.
struct DaosKVFixture {
    std::string pool = envOr("DAOS_POOL", "sc26");
    std::string cont = envOr("DAOS_CONT", "kv-test");
    daos_handle_t poh = DAOS_HDL_INVAL;
    daos_handle_t coh = DAOS_HDL_INVAL;
    daos_handle_t oh = DAOS_HDL_INVAL;
    bool inited = false;

    DaosKVFixture() {
        BOOST_TEST_MESSAGE("DAOS pool: " << pool << ", container: " << cont);

        int rc = daos_init();
        BOOST_REQUIRE_MESSAGE(rc == 0, "daos_init failed: " << d_errstr(rc)
            << " (is daos_agent running?)");
        inited = true;

        rc = daos_pool_connect(pool.c_str(), nullptr, DAOS_PC_RW, &poh, nullptr, nullptr);
        BOOST_REQUIRE_MESSAGE(rc == 0, "daos_pool_connect(" << pool << ") failed: " << d_errstr(rc));

        rc = daos_cont_open(poh, cont.c_str(), DAOS_COO_RW, &coh, nullptr, nullptr);
        BOOST_REQUIRE_MESSAGE(rc == 0, "daos_cont_open(" << cont << ") failed: " << d_errstr(rc));

        // Fixed object id; keys are unique per run so repeated runs don't collide.
        daos_obj_id_t oid{};
        oid.lo = 0xE25A;
        rc = daos_obj_generate_oid(coh, &oid, DAOS_OT_KV_HASHED, OC_UNKNOWN, 0, 0);
        BOOST_REQUIRE_MESSAGE(rc == 0, "daos_obj_generate_oid failed: " << d_errstr(rc));

        rc = daos_kv_open(coh, oid, DAOS_OO_RW, &oh, nullptr);
        BOOST_REQUIRE_MESSAGE(rc == 0, "daos_kv_open failed: " << d_errstr(rc));
    }

    ~DaosKVFixture() {
        if (daos_handle_is_valid(oh))
            daos_kv_close(oh, nullptr);
        if (daos_handle_is_valid(coh))
            daos_cont_close(coh, nullptr);
        if (daos_handle_is_valid(poh))
            daos_pool_disconnect(poh, nullptr);
        if (inited)
            daos_fini();
    }
};

}  // namespace

BOOST_FIXTURE_TEST_CASE(KVWriteReadSha256, DaosKVFixture) {
    const std::string runTag = std::to_string(getpid()) + "_" + std::to_string(std::time(nullptr));
    const auto seed = static_cast<std::mt19937_64::result_type>(std::time(nullptr));
    BOOST_TEST_MESSAGE("run tag: " << runTag << ", random seed: " << seed);
    std::mt19937_64 rng(seed);

    std::vector<std::string> keys;
    std::vector<std::string> writtenSha;

    // Write
    for (int i = 0; i < kNumValues; i++) {
        std::vector<unsigned char> value(kValueSize);
        for (auto& b : value)
            b = static_cast<unsigned char>(rng());

        keys.push_back("toy_" + runTag + "_" + std::to_string(i));
        writtenSha.push_back(sha256Hex(value.data(), value.size()));

        int rc = daos_kv_put(oh, DAOS_TX_NONE, 0, keys[i].c_str(), value.size(), value.data(), nullptr);
        BOOST_REQUIRE_MESSAGE(rc == 0, "daos_kv_put(" << keys[i] << ") failed: " << d_errstr(rc));
        BOOST_TEST_MESSAGE("put " << writtenSha[i] << "  " << keys[i]);
    }

    // Read back and compare digests
    for (int i = 0; i < kNumValues; i++) {
        daos_size_t size = 0;  // null buffer: query the value size
        int rc = daos_kv_get(oh, DAOS_TX_NONE, 0, keys[i].c_str(), &size, nullptr, nullptr);
        BOOST_REQUIRE_MESSAGE(rc == 0, "daos_kv_get(" << keys[i] << ", size) failed: " << d_errstr(rc));
        BOOST_CHECK_EQUAL(size, kValueSize);

        std::vector<unsigned char> buf(size);
        rc = daos_kv_get(oh, DAOS_TX_NONE, 0, keys[i].c_str(), &size, buf.data(), nullptr);
        BOOST_REQUIRE_MESSAGE(rc == 0, "daos_kv_get(" << keys[i] << ") failed: " << d_errstr(rc));

        const std::string readSha = sha256Hex(buf.data(), size);
        BOOST_TEST_MESSAGE("get " << readSha << "  " << keys[i]);
        BOOST_CHECK_EQUAL(readSha, writtenSha[i]);
    }

    // Clean up so the container doesn't grow across runs
    for (const auto& key : keys) {
        int rc = daos_kv_remove(oh, DAOS_TX_NONE, 0, key.c_str(), nullptr);
        BOOST_CHECK_MESSAGE(rc == 0, "daos_kv_remove(" << key << ") failed: " << d_errstr(rc));
    }
}
