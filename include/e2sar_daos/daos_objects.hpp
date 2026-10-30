#pragma once
/**
    Handles for the object types of the run layout (docs/concepts/daos-layout.md):

        DaosIntObject   DAOS_OT_MULTI_UINT64: 64-bit integer dkeys and akeys, single values
                        (the seconds and index[T] objects)
        DaosArray       DAOS_OT_ARRAY_BYTE opened with known cell and chunk sizes
                        (the data[T, s] arrays; a byte array stores no metadata)

    Opening either is local to the client (no RPC). Handles may be shared across threads.
    The asynchronous calls themselves go through AsyncOps.
*/

#include <daos.h>

#include <cstdint>
#include <functional>
#include <string>

#include "e2sar_daos/daos_handles.hpp"

namespace e2sar_daos {

/** Object class id from its name ("SX", "S8", …); throws on an unknown name. */
daos_oclass_id_t oclassId(const std::string& name);

/** The full object id: DAOS fills the type and class bits of oid.hi, the user part is (0, lo). */
daos_obj_id_t makeOid(const DaosContainer& cont, uint64_t lo, enum daos_otype_t type,
                      const std::string& oclass);

class DaosIntObject {
public:
    DaosIntObject(const DaosContainer& cont, uint64_t oidLo, const std::string& oclass,
                  unsigned int mode = DAOS_OO_RW);
    ~DaosIntObject();
    DaosIntObject(const DaosIntObject&) = delete;
    DaosIntObject& operator=(const DaosIntObject&) = delete;

    daos_handle_t handle() const noexcept { return oh_; }
    daos_obj_id_t oid() const noexcept { return oid_; }

    /** Blocking single-value put/get under (dkey, akey). */
    void put(uint64_t dkey, uint64_t akey, const void* buf, size_t size);
    /** Size of the value; 0 if absent. */
    size_t valueSize(uint64_t dkey, uint64_t akey);
    void get(uint64_t dkey, uint64_t akey, void* buf, size_t* size);
    /** Every dkey (hash order). */
    void listDkeys(const std::function<void(uint64_t)>& fn);
    /** Remove the whole object. */
    void punch();

private:
    daos_obj_id_t oid_{};
    daos_handle_t oh_ = DAOS_HDL_INVAL;
};

class DaosArray {
public:
    DaosArray(const DaosContainer& cont, uint64_t oidLo, const std::string& oclass, size_t chunkSize,
              unsigned int mode = DAOS_OO_RW);
    ~DaosArray();
    DaosArray(const DaosArray&) = delete;
    DaosArray& operator=(const DaosArray&) = delete;

    daos_handle_t handle() const noexcept { return oh_; }
    daos_obj_id_t oid() const noexcept { return oid_; }

    /** Blocking write/read of one byte range. */
    void write(uint64_t offset, const void* buf, size_t size);
    void read(uint64_t offset, void* buf, size_t size);

private:
    daos_obj_id_t oid_{};
    daos_handle_t oh_ = DAOS_HDL_INVAL;
};

/**
    The descriptors of one single-value update under integer keys. They must stay valid
    (and must not move) until the operation completes, so keep this in heap memory owned
    by whatever waits for the completion.
*/
struct IntKeyUpdate {
    uint64_t dkey = 0;
    uint64_t akey = 0;
    daos_key_t dkeyIov{};
    daos_iod_t iod{};
    d_iov_t valIov{};
    d_sg_list_t sgl{};

    /** Fill the descriptors for writing [buf, buf + size) under (dkey, akey). */
    void set(uint64_t d, uint64_t a, const void* buf, size_t size);
    /** Launch the update on oh; ev may be null for a blocking call. */
    int launch(daos_handle_t oh, daos_event_t* ev);
};

/** The descriptors of one array write: one range, one buffer. Same lifetime rule as IntKeyUpdate. */
struct ArrayWrite {
    daos_range_t range{};
    daos_array_iod_t iod{};
    d_iov_t iov{};
    d_sg_list_t sgl{};

    void set(uint64_t offset, const void* buf, size_t size);
    int launch(daos_handle_t oh, daos_event_t* ev);
};

}  // namespace e2sar_daos
