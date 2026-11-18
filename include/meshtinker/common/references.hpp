#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace meshtinker::common {

// The one way to follow a file reference written inside a model, after
// mesh2gltf's references.rs.
//
// Confined (the default) references reach only regular files inside the
// input's folder: relative paths whose `..` never climb above it, and whose
// symlinks, once resolved, stay inside it. Absolute paths, drive letters and
// URI schemes are refused. Unrestricted references (`--trusted`) may be
// absolute and lead anywhere, but must still name a regular file.
//
// A refused reference and a missing file get the same answer, so a model
// can't probe what exists outside its folder.
class References {
public:
    // What a model in `directory` may reach. Throws a Failure if `directory`
    // can't be resolved.
    static References forInput(const std::filesystem::path& directory, bool trusted);

    // The file `reference` leads to, written in a file in `base`. `reference` is
    // a decoded path, not a URI. Empty when refused or missing.
    std::optional<std::filesystem::path> resolve(const std::filesystem::path& base,
                                                 const std::string& reference) const;

    bool trusted() const { return root_.empty(); }

private:
    // The canonical directory references must stay in; empty when unrestricted.
    std::filesystem::path root_;
};

// The message for a refused reference.
inline constexpr const char* refused_reference_message = "not found, or outside the model's folder";

} // namespace meshtinker::common
