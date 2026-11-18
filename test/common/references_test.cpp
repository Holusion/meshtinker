#include "meshtinker/common/references.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <fstream>

namespace fs = std::filesystem;
using meshtinker::common::References;

namespace {

// `job` holds the model's folder, with `tex/a.png`, `m.bin`, a link to `outside/`
// and a link to `m.bin`. `outside/s.txt` and `beside.txt` sit next to it.
fs::path makeTree() {
    const fs::path root = meshtinker::test::workDirectory("references");
    fs::create_directories(root / "job" / "tex");
    fs::create_directories(root / "outside");
    for (const char* file : {"job/tex/a.png", "job/m.bin", "outside/s.txt", "beside.txt"}) {
        std::ofstream(root / file) << "x";
    }
    fs::create_directory_symlink(root / "outside", root / "job" / "link-out");
    fs::create_symlink("m.bin", root / "job" / "link-in.bin");
    return root;
}

} // namespace

TEST_CASE(confined_references_stay_inside) {
    const fs::path root = makeTree();
    const fs::path job = root / "job";
    const References references = References::forInput(job, false);
    CHECK(references.resolve(job, "m.bin").has_value());
    CHECK(references.resolve(job, "tex/a.png").has_value());
    CHECK(references.resolve(job, "./tex/../m.bin").has_value());
    CHECK(references.resolve(job / "tex", "../m.bin").has_value());
    CHECK(references.resolve(job, "link-in.bin").has_value());
    CHECK(references.resolve(job, "tex\\a.png").has_value());

    CHECK(!references.resolve(job, "../outside/s.txt").has_value());
    CHECK(!references.resolve(job, "tex/../../outside/s.txt").has_value());
    CHECK(!references.resolve(job, "link-out/s.txt").has_value());
    // The text stays inside, but the kernel resolves the link first: root/beside.txt.
    CHECK(!references.resolve(job, "link-out/../beside.txt").has_value());
    // The same way, this one lands inside, which is fine.
    CHECK(references.resolve(job, "link-out/../job/m.bin").has_value());
    CHECK(!references.resolve(job, (root / "job" / "m.bin").string()).has_value());
    CHECK(!references.resolve(job, "file:///etc/passwd").has_value());
    CHECK(!references.resolve(job, "https://example.com/m.bin").has_value());
    CHECK(!references.resolve(job, "C:/m.bin").has_value());
    CHECK(!references.resolve(job, "missing.bin").has_value());
    CHECK(!references.resolve(job, "tex").has_value());
    CHECK(!references.resolve(job, "").has_value());
}

TEST_CASE(trusted_references_go_anywhere_but_must_be_files) {
    const fs::path root = makeTree();
    const fs::path job = root / "job";
    const References references = References::forInput(job, true);
    CHECK(references.resolve(job, "../outside/s.txt").has_value());
    CHECK(references.resolve(job, "link-out/s.txt").has_value());
    CHECK(references.resolve(job, (root / "outside" / "s.txt").string()).has_value());
    CHECK(!references.resolve(job, "https://example.com/m.bin").has_value());
    CHECK(!references.resolve(job, "tex").has_value());
}
