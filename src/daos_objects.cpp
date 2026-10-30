#include "e2sar_daos/daos_objects.hpp"

#include <cstring>
#include <vector>

namespace e2sar_daos {

daos_oclass_id_t oclassId(const std::string& name) {
    const auto cid = static_cast<daos_oclass_id_t>(daos_oclass_name2id(name.c_str()));
    if (cid == OC_UNKNOWN)
        throw DaosError("unknown DAOS object class '" + name + "'", -DER_INVAL);
    return cid;
}

daos_obj_id_t makeOid(const DaosContainer& cont, uint64_t lo, enum daos_otype_t type,
                      const std::string& oclass) {
    daos_obj_id_t oid{};
    oid.lo = lo;
    oid.hi = 0;  // DAOS sets the high 32 bits; the low 32 bits of hi stay 0
    check(daos_obj_generate_oid(cont.handle(), &oid, type, oclassId(oclass), 0, 0),
          "daos_obj_generate_oid");
    return oid;
}

// ---------------------------------------------------------------------------

void IntKeyUpdate::set(uint64_t d, uint64_t a, const void* buf, size_t size) {
    dkey = d;
    akey = a;
    d_iov_set(&dkeyIov, &dkey, sizeof(dkey));
    std::memset(&iod, 0, sizeof(iod));
    d_iov_set(&iod.iod_name, &akey, sizeof(akey));
    iod.iod_type = DAOS_IOD_SINGLE;
    iod.iod_size = size;
    iod.iod_nr = 1;
    iod.iod_recxs = nullptr;
    d_iov_set(&valIov, const_cast<void*>(buf), size);
    sgl.sg_nr = 1;
    sgl.sg_nr_out = 0;
    sgl.sg_iovs = &valIov;
}

int IntKeyUpdate::launch(daos_handle_t oh, daos_event_t* ev) {
    return daos_obj_update(oh, DAOS_TX_NONE, 0, &dkeyIov, 1, &iod, &sgl, ev);
}

void ArrayWrite::set(uint64_t offset, const void* buf, size_t size) {
    range.rg_idx = offset;
    range.rg_len = size;
    std::memset(&iod, 0, sizeof(iod));
    iod.arr_nr = 1;
    iod.arr_rgs = &range;
    d_iov_set(&iov, const_cast<void*>(buf), size);
    sgl.sg_nr = 1;
    sgl.sg_nr_out = 0;
    sgl.sg_iovs = &iov;
}

int ArrayWrite::launch(daos_handle_t oh, daos_event_t* ev) {
    return daos_array_write(oh, DAOS_TX_NONE, &iod, &sgl, ev);
}

// ---------------------------------------------------------------------------

DaosIntObject::DaosIntObject(const DaosContainer& cont, uint64_t oidLo, const std::string& oclass,
                             unsigned int mode)
    : oid_(makeOid(cont, oidLo, DAOS_OT_MULTI_UINT64, oclass)) {
    check(daos_obj_open(cont.handle(), oid_, mode, &oh_, nullptr), "daos_obj_open");
}

DaosIntObject::~DaosIntObject() {
    if (daos_handle_is_valid(oh_))
        daos_obj_close(oh_, nullptr);
}

void DaosIntObject::put(uint64_t dkey, uint64_t akey, const void* buf, size_t size) {
    IntKeyUpdate u;
    u.set(dkey, akey, buf, size);
    check(u.launch(oh_, nullptr), "daos_obj_update(" + std::to_string(dkey) + ")");
}

size_t DaosIntObject::valueSize(uint64_t dkey, uint64_t akey) {
    IntKeyUpdate u;
    u.set(dkey, akey, nullptr, 0);
    u.iod.iod_size = DAOS_REC_ANY;  // query the size only
    check(daos_obj_fetch(oh_, DAOS_TX_NONE, 0, &u.dkeyIov, 1, &u.iod, nullptr, nullptr, nullptr),
          "daos_obj_fetch(" + std::to_string(dkey) + ", size)");
    return u.iod.iod_size;
}

void DaosIntObject::get(uint64_t dkey, uint64_t akey, void* buf, size_t* size) {
    IntKeyUpdate u;
    u.set(dkey, akey, buf, *size);
    check(daos_obj_fetch(oh_, DAOS_TX_NONE, 0, &u.dkeyIov, 1, &u.iod, &u.sgl, nullptr, nullptr),
          "daos_obj_fetch(" + std::to_string(dkey) + ")");
    *size = u.iod.iod_size;
}

void DaosIntObject::listDkeys(const std::function<void(uint64_t)>& fn) {
    constexpr uint32_t kBatch = 1024;
    std::vector<daos_key_desc_t> kds(kBatch);
    std::vector<uint64_t> keys(kBatch);
    daos_anchor_t anchor{};
    while (!daos_anchor_is_eof(&anchor)) {
        uint32_t nr = kBatch;
        d_iov_t iov;
        d_sg_list_t sgl;
        d_iov_set(&iov, keys.data(), keys.size() * sizeof(uint64_t));
        sgl.sg_nr = 1;
        sgl.sg_nr_out = 0;
        sgl.sg_iovs = &iov;
        check(daos_obj_list_dkey(oh_, DAOS_TX_NONE, &nr, kds.data(), &sgl, &anchor, nullptr),
              "daos_obj_list_dkey");
        const auto* p = reinterpret_cast<const uint8_t*>(keys.data());
        for (uint32_t i = 0; i < nr; i++) {
            uint64_t k = 0;
            std::memcpy(&k, p, sizeof(k));
            fn(k);
            p += kds[i].kd_key_len;
        }
    }
}

void DaosIntObject::punch() {
    check(daos_obj_punch(oh_, DAOS_TX_NONE, 0, nullptr), "daos_obj_punch");
}

// ---------------------------------------------------------------------------

DaosArray::DaosArray(const DaosContainer& cont, uint64_t oidLo, const std::string& oclass,
                     size_t chunkSize, unsigned int mode)
    : oid_(makeOid(cont, oidLo, DAOS_OT_ARRAY_BYTE, oclass)) {
    check(daos_array_open_with_attr(cont.handle(), oid_, DAOS_TX_NONE, mode, 1, chunkSize, &oh_, nullptr),
          "daos_array_open_with_attr");
}

DaosArray::~DaosArray() {
    if (daos_handle_is_valid(oh_))
        daos_array_close(oh_, nullptr);
}

void DaosArray::write(uint64_t offset, const void* buf, size_t size) {
    ArrayWrite w;
    w.set(offset, buf, size);
    check(w.launch(oh_, nullptr), "daos_array_write");
}

void DaosArray::read(uint64_t offset, void* buf, size_t size) {
    ArrayWrite r;  // same descriptors
    r.set(offset, buf, size);
    check(daos_array_read(oh_, DAOS_TX_NONE, &r.iod, &r.sgl, nullptr), "daos_array_read");
}

}  // namespace e2sar_daos
