#pragma once
/**
    RAII wrappers around the DAOS client handles used by e2sar-daos.

    Lifetime order (construct top to bottom, destroy in reverse):
        DaosRuntime  -> daos_init() / daos_fini()
        DaosPool     -> daos_pool_connect() / daos_pool_disconnect()
        DaosContainer-> daos_cont_open() (optionally create) / daos_cont_close()
        DaosKV       -> daos_kv_open() / daos_kv_close()

    All handles are non-copyable. Failures throw DaosError carrying the DER_* code.
    Pool, container and KV handles may be shared across threads; event queues may not
    (see AsyncKVWriter).
*/

#include <daos.h>

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

namespace e2sar_daos {

class DaosError : public std::runtime_error {
public:
    DaosError(const std::string& what, int rc);
    int rc() const noexcept { return rc_; }

private:
    int rc_;
};

/** Throw DaosError if rc != 0. */
void check(int rc, const std::string& what);

/** daos_init()/daos_fini(). Keep one alive for as long as any other DAOS object exists. */
class DaosRuntime {
public:
    DaosRuntime();
    ~DaosRuntime();
    DaosRuntime(const DaosRuntime&) = delete;
    DaosRuntime& operator=(const DaosRuntime&) = delete;
};

class DaosPool {
public:
    /** Connect to a pool by label or UUID string. */
    explicit DaosPool(const std::string& label, unsigned int flags = DAOS_PC_RW);
    ~DaosPool();
    DaosPool(const DaosPool&) = delete;
    DaosPool& operator=(const DaosPool&) = delete;

    daos_handle_t handle() const noexcept { return poh_; }
    const std::string& label() const noexcept { return label_; }

private:
    std::string label_;
    daos_handle_t poh_ = DAOS_HDL_INVAL;
};

class DaosContainer {
public:
    /**
        Open a container by label. If createIfMissing is set and the container does
        not exist, create it with default properties first. A concurrent creator
        winning the race (-DER_EXIST) is treated as success.
    */
    DaosContainer(const DaosPool& pool, const std::string& label,
                  bool createIfMissing = true, unsigned int flags = DAOS_COO_RW);
    ~DaosContainer();
    DaosContainer(const DaosContainer&) = delete;
    DaosContainer& operator=(const DaosContainer&) = delete;

    daos_handle_t handle() const noexcept { return coh_; }
    const std::string& label() const noexcept { return label_; }
    /** True if this instance created the container. */
    bool created() const noexcept { return created_; }

private:
    std::string label_;
    daos_handle_t coh_ = DAOS_HDL_INVAL;
    bool created_ = false;
};

/** A flat key-value object (DAOS_OT_KV_HASHED). */
class DaosKV {
public:
    /**
        Open (implicitly creating) the KV object whose user part of the OID is
        (oidHi, oidLo). oclass is a DAOS object class name such as "SX" or "RP_2G1";
        an empty string lets DAOS pick the container default.
    */
    DaosKV(const DaosContainer& cont, uint64_t oidLo, uint32_t oidHi = 0,
           const std::string& oclass = "", unsigned int mode = DAOS_OO_RW);
    ~DaosKV();
    DaosKV(const DaosKV&) = delete;
    DaosKV& operator=(const DaosKV&) = delete;

    daos_handle_t handle() const noexcept { return oh_; }
    daos_obj_id_t oid() const noexcept { return oid_; }

    /** Blocking put/get helpers for small values (metadata, tools, tests). */
    void put(const std::string& key, const void* buf, size_t size);
    /** Returns the size of the value; 0 if the key does not exist. */
    size_t valueSize(const std::string& key);
    /** Reads up to *size bytes; on return *size is the value size. */
    void get(const std::string& key, void* buf, size_t* size);
    /** Enumerate every key in the object (hash order, not sorted). */
    void listKeys(const std::function<void(const std::string&)>& fn);

private:
    daos_obj_id_t oid_{};
    daos_handle_t oh_ = DAOS_HDL_INVAL;
};

}  // namespace e2sar_daos
