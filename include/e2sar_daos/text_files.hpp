#pragma once

// Line files that survive a full disk or a crash: the manifests of e2sar_daos_read and the JSON
// lines of e2sar_daos_monitor. A line is either all there, ending in '\n', or not there at all,
// so the last line of a manifest is always a whole restart point. One writer per file.

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace e2sar_daos::textfile {

// Append line + '\n'. If the write comes up short or fails (ENOSPC, EFBIG, EIO) or fsync fails,
// the file is cut back to its old size and false is returned with errno set: no half line stays.
inline bool appendLine(const std::string& path, const std::string& line, bool sync = true) {
    const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0)
        return false;
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        const int e = errno;
        ::close(fd);
        errno = e;
        return false;
    }
    const std::string s = line + "\n";
    size_t done = 0;
    int err = 0;
    while (done < s.size()) {
        const ssize_t w = ::write(fd, s.data() + done, s.size() - done);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            err = errno;
            break;
        }
        if (w == 0) {
            err = ENOSPC;
            break;
        }
        done += static_cast<size_t>(w);
    }
    if (!err && sync && ::fsync(fd) != 0)
        err = errno;
    if (err && ::ftruncate(fd, st.st_size) == 0 && sync)
        ::fsync(fd);
    ::close(fd);
    if (err) {
        errno = err;
        return false;
    }
    return true;
}

// Drop a last line without its '\n' (a write cut short by a crash or by a writer without
// appendLine). Returns the bytes dropped (0 if the file ends in '\n', is empty or doesn't
// exist), or -1 with errno set.
inline long long trimPartialLine(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    auto fail = [fd]() {
        const int e = errno;
        ::close(fd);
        errno = e;
        return -1LL;
    };
    struct stat st{};
    if (::fstat(fd, &st) != 0)
        return fail();
    const off_t size = st.st_size;
    off_t keep = 0;  // bytes up to and including the last '\n'
    std::vector<char> buf(64 * 1024);
    for (off_t end = size; end > 0;) {
        const off_t start = end > static_cast<off_t>(buf.size()) ? end - static_cast<off_t>(buf.size()) : 0;
        const ssize_t n = ::pread(fd, buf.data(), static_cast<size_t>(end - start), start);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return fail();
        }
        if (n != end - start) {
            errno = EIO;
            return fail();
        }
        bool found = false;
        for (ssize_t i = n - 1; i >= 0; i--)
            if (buf[static_cast<size_t>(i)] == '\n') {
                keep = start + i + 1;
                found = true;
                break;
            }
        if (found)
            break;
        end = start;
    }
    if (keep == size) {
        ::close(fd);
        return 0;
    }
    if (::ftruncate(fd, keep) != 0 || ::fsync(fd) != 0)
        return fail();
    ::close(fd);
    return static_cast<long long>(size - keep);
}

// Bytes free to an unprivileged user on the file system holding path; nullopt if unknown.
inline std::optional<uint64_t> freeBytes(const std::string& path) {
    struct statvfs v{};
    if (::statvfs(path.c_str(), &v) != 0)
        return std::nullopt;
    return static_cast<uint64_t>(v.f_bavail) * static_cast<uint64_t>(v.f_frsize);
}

}  // namespace e2sar_daos::textfile
