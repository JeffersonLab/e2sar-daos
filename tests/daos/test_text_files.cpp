// text_files.hpp: whole-line appends that roll back on a short write, and trimming a cut-off last
// line, as e2sar_daos_read uses them for its manifests and e2sar_daos_monitor for its JSON lines.
// A full disk is stood in for by RLIMIT_FSIZE (the write fails with EFBIG instead of ENOSPC).

#define BOOST_TEST_MODULE TextFilesTests
#include <boost/test/included/unit_test.hpp>

#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "e2sar_daos/text_files.hpp"

namespace fs = std::filesystem;
namespace tf = e2sar_daos::textfile;

namespace {

struct TmpDir {
    fs::path dir;
    TmpDir() {
        dir = fs::temp_directory_path() / ("e2sar_text_files_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~TmpDir() { fs::remove_all(dir); }
    std::string file(const std::string& n) const { return (dir / n).string(); }
};

std::string slurp(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void spit(const std::string& p, const std::string& text) {
    std::ofstream(p, std::ios::binary) << text;
}

// Files may not grow past `bytes` while this lives; SIGXFSZ is ignored so writes fail with EFBIG.
struct FileSizeLimit {
    rlimit old{};
    void (*oldSig)(int);
    explicit FileSizeLimit(rlim_t bytes) {
        ::getrlimit(RLIMIT_FSIZE, &old);
        oldSig = ::signal(SIGXFSZ, SIG_IGN);
        rlimit r = old;
        r.rlim_cur = bytes;
        BOOST_REQUIRE_EQUAL(::setrlimit(RLIMIT_FSIZE, &r), 0);
    }
    ~FileSizeLimit() {
        ::setrlimit(RLIMIT_FSIZE, &old);
        ::signal(SIGXFSZ, oldSig);
    }
};

}  // namespace

BOOST_AUTO_TEST_CASE(append_whole_lines) {
    TmpDir t;
    const auto f = t.file("m.txt");
    BOOST_CHECK(tf::appendLine(f, "one"));
    BOOST_CHECK(tf::appendLine(f, "two", false));
    BOOST_CHECK_EQUAL(slurp(f), "one\ntwo\n");
}

BOOST_AUTO_TEST_CASE(short_write_leaves_no_half_line) {
    TmpDir t;
    const auto f = t.file("m.txt");
    BOOST_REQUIRE(tf::appendLine(f, "0123456789"));  // 11 bytes
    {
        FileSizeLimit lim(16);  // room for 5 more bytes: the next line is cut short
        errno = 0;
        BOOST_CHECK(!tf::appendLine(f, "abcdefghij"));
        BOOST_CHECK_EQUAL(errno, EFBIG);
        BOOST_CHECK(!tf::appendLine(f, std::string(100, 'x')));  // nothing written at all
    }
    BOOST_CHECK_EQUAL(slurp(f), "0123456789\n");
    BOOST_CHECK(tf::appendLine(f, "abcdefghij"));  // room again: appends go on as before
    BOOST_CHECK_EQUAL(slurp(f), "0123456789\nabcdefghij\n");
}

BOOST_AUTO_TEST_CASE(trim_cut_off_last_line) {
    TmpDir t;
    const auto f = t.file("m.txt");
    spit(f, "sha1  a.evio  100 5\nsha2  b.evio  10");
    BOOST_CHECK_EQUAL(tf::trimPartialLine(f), 15);
    BOOST_CHECK_EQUAL(slurp(f), "sha1  a.evio  100 5\n");
    BOOST_CHECK_EQUAL(tf::trimPartialLine(f), 0);  // whole lines are left alone
    BOOST_CHECK_EQUAL(slurp(f), "sha1  a.evio  100 5\n");
    BOOST_CHECK(tf::appendLine(f, "sha2  b.evio  105 5"));
    BOOST_CHECK_EQUAL(slurp(f), "sha1  a.evio  100 5\nsha2  b.evio  105 5\n");
}

BOOST_AUTO_TEST_CASE(trim_edge_cases) {
    TmpDir t;
    BOOST_CHECK_EQUAL(tf::trimPartialLine(t.file("missing.txt")), 0);
    BOOST_CHECK(!fs::exists(t.file("missing.txt")));

    const auto e = t.file("empty.txt");
    spit(e, "");
    BOOST_CHECK_EQUAL(tf::trimPartialLine(e), 0);

    const auto one = t.file("one.txt");  // a single line, cut off: the file becomes empty
    spit(one, "half a line");
    BOOST_CHECK_EQUAL(tf::trimPartialLine(one), 11);
    BOOST_CHECK_EQUAL(fs::file_size(one), 0u);

    const auto big = t.file("big.txt");  // the last '\n' is more than one 64 KiB read back
    const std::string head = "first\n", tail(200 * 1024, 'y');
    spit(big, head + tail);
    BOOST_CHECK_EQUAL(tf::trimPartialLine(big), static_cast<long long>(tail.size()));
    BOOST_CHECK_EQUAL(slurp(big), head);
}

BOOST_AUTO_TEST_CASE(free_bytes) {
    TmpDir t;
    const auto f = tf::freeBytes(t.dir.string());
    BOOST_REQUIRE(f.has_value());
    BOOST_CHECK_GT(*f, 0u);
    BOOST_CHECK(!tf::freeBytes(t.file("no/such/dir")).has_value());
}
