#include "e2sar_daos/daos_handles.hpp"

#include <algorithm>
#include <vector>

namespace e2sar_daos {

DaosError::DaosError(const std::string& what, int rc)
    : std::runtime_error(what + ": " + d_errstr(rc) + " (" + std::to_string(rc) + ")"), rc_(rc) {}

void check(int rc, const std::string& what) {
    if (rc != 0)
        throw DaosError(what, rc);
}

// ---------------------------------------------------------------------------

DaosRuntime::DaosRuntime() {
    check(daos_init(), "daos_init (is daos_agent running?)");
}

DaosRuntime::~DaosRuntime() {
    daos_fini();
}

// ---------------------------------------------------------------------------

DaosPool::DaosPool(const std::string& label, unsigned int flags) : label_(label) {
    check(daos_pool_connect(label_.c_str(), nullptr, flags, &poh_, nullptr, nullptr),
          "daos_pool_connect(" + label_ + ")");
}

DaosPool::~DaosPool() {
    if (daos_handle_is_valid(poh_))
        daos_pool_disconnect(poh_, nullptr);
}

// ---------------------------------------------------------------------------

DaosContainer::DaosContainer(const DaosPool& pool, const std::string& label,
                             bool createIfMissing, unsigned int flags)
    : label_(label) {
    int rc = daos_cont_open(pool.handle(), label_.c_str(), flags, &coh_, nullptr, nullptr);
    if (rc == -DER_NONEXIST && createIfMissing) {
        rc = daos_cont_create_with_label(pool.handle(), label_.c_str(), nullptr, nullptr, nullptr);
        if (rc == 0)
            created_ = true;
        else if (rc != -DER_EXIST)  // someone else created it first: fine
            check(rc, "daos_cont_create_with_label(" + pool.label() + "/" + label_ + ")");
        rc = daos_cont_open(pool.handle(), label_.c_str(), flags, &coh_, nullptr, nullptr);
    }
    check(rc, "daos_cont_open(" + pool.label() + "/" + label_ + ")");
}

DaosContainer::~DaosContainer() {
    if (daos_handle_is_valid(coh_))
        daos_cont_close(coh_, nullptr);
}

// ---------------------------------------------------------------------------

DaosKV::DaosKV(const DaosContainer& cont, uint64_t oidLo, uint32_t oidHi,
               const std::string& oclass, unsigned int mode) {
    daos_oclass_id_t cid = OC_UNKNOWN;
    if (!oclass.empty()) {
        cid = static_cast<daos_oclass_id_t>(daos_oclass_name2id(oclass.c_str()));
        if (cid == OC_UNKNOWN)
            throw DaosError("unknown DAOS object class '" + oclass + "'", -DER_INVAL);
    }

    oid_.lo = oidLo;
    oid_.hi = oidHi;  // upper 32 bits are overwritten by generate_oid
    check(daos_obj_generate_oid(cont.handle(), &oid_, DAOS_OT_KV_HASHED, cid, 0, 0),
          "daos_obj_generate_oid");
    check(daos_kv_open(cont.handle(), oid_, mode, &oh_, nullptr), "daos_kv_open");
}

DaosKV::~DaosKV() {
    if (daos_handle_is_valid(oh_))
        daos_kv_close(oh_, nullptr);
}

void DaosKV::put(const std::string& key, const void* buf, size_t size) {
    check(daos_kv_put(oh_, DAOS_TX_NONE, 0, key.c_str(), size, buf, nullptr),
          "daos_kv_put(" + key + ")");
}

bool DaosKV::putIfAbsent(const std::string& key, const void* buf, size_t size) {
    const int rc = daos_kv_put(oh_, DAOS_TX_NONE, DAOS_COND_KEY_INSERT, key.c_str(), size, buf, nullptr);
    if (rc == -DER_EXIST)
        return false;
    check(rc, "daos_kv_put(" + key + ", insert)");
    return true;
}

void DaosKV::remove(const std::string& key) {
    check(daos_kv_remove(oh_, DAOS_TX_NONE, 0, key.c_str(), nullptr), "daos_kv_remove(" + key + ")");
}

void DaosKV::destroy() {
    check(daos_kv_destroy(oh_, DAOS_TX_NONE, nullptr), "daos_kv_destroy");
}

size_t DaosKV::valueSize(const std::string& key) {
    daos_size_t size = 0;  // null buffer: query the size only
    check(daos_kv_get(oh_, DAOS_TX_NONE, 0, key.c_str(), &size, nullptr, nullptr),
          "daos_kv_get(" + key + ", size)");
    return size;
}

void DaosKV::get(const std::string& key, void* buf, size_t* size) {
    daos_size_t sz = *size;
    check(daos_kv_get(oh_, DAOS_TX_NONE, 0, key.c_str(), &sz, buf, nullptr),
          "daos_kv_get(" + key + ")");
    *size = sz;
}

void DaosKV::listKeys(const std::function<void(const std::string&)>& fn) {
    constexpr uint32_t kBatch = 256;
    std::vector<daos_key_desc_t> kds(kBatch);
    std::vector<char> buf(64 * 1024);
    daos_anchor_t anchor{};

    while (!daos_anchor_is_eof(&anchor)) {
        uint32_t nr = kBatch;
        d_iov_t iov;
        d_sg_list_t sgl;
        d_iov_set(&iov, buf.data(), buf.size());
        sgl.sg_nr = 1;
        sgl.sg_nr_out = 0;
        sgl.sg_iovs = &iov;

        int rc = daos_kv_list(oh_, DAOS_TX_NONE, &nr, kds.data(), &sgl, &anchor, nullptr);
        if (rc == -DER_KEY2BIG) {  // kds[0].kd_key_len holds the size needed
            buf.resize(std::max<size_t>(buf.size() * 2, kds[0].kd_key_len));
            continue;
        }
        check(rc, "daos_kv_list");

        size_t off = 0;
        for (uint32_t i = 0; i < nr; i++) {
            fn(std::string(buf.data() + off, kds[i].kd_key_len));
            off += kds[i].kd_key_len;
        }
    }
}

}  // namespace e2sar_daos
