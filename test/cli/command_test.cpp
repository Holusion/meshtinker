#include "meshtinker/cli/command.hpp"
#include "meshtinker/common/failure.hpp"
#include "support/check.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

using namespace meshtinker;

namespace {

struct Result {
    int code;
    std::string out;
    std::string err;
};

// Runs a command in-process with `arguments`, capturing stdout and stderr.
Result runCommand(cli::Command& command, std::vector<std::string> arguments, const std::function<int()>& body) {
    arguments.insert(arguments.begin(), "meshtinker-test");
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }
    std::ostringstream out;
    std::ostringstream err;
    std::streambuf* old_out = std::cout.rdbuf(out.rdbuf());
    std::streambuf* old_err = std::cerr.rdbuf(err.rdbuf());
    const int code = command.run(static_cast<int>(argv.size()), argv.data(), body);
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    return {code, out.str(), err.str()};
}

void configure(cli::Command& command) {
    command.addJsonOption();
    command.addTrustedOption();
}

} // namespace

TEST_CASE(summary_is_one_line_on_stdout) {
    cli::Command command("test", "Do a test thing");
    configure(command);
    const Result result = runCommand(command, {"--summary"}, [] { return 0; });
    CHECK_EQ(result.code, 0);
    CHECK_EQ(result.out, std::string("Do a test thing\n"));
}

TEST_CASE(usage_errors_exit_2_and_keep_json_on_stdout) {
    cli::Command command("test", "Do a test thing");
    configure(command);
    const Result result = runCommand(command, {"--json", "--nope"}, [] { return 0; });
    CHECK_EQ(result.code, 2);
    const auto report = nlohmann::json::parse(result.out);
    CHECK_EQ(report.at("error").at("code").get<std::string>(), std::string("usage"));
    CHECK_EQ(report.at("error").at("exitCode").get<int>(), 2);
    CHECK_EQ(report.at("tool").at("name").get<std::string>(), std::string("meshtinker-test"));
    CHECK(result.err.find("meshtinker-test: error:") != std::string::npos);
}

TEST_CASE(failures_keep_their_exit_code_and_diagnostics) {
    cli::Command command("test", "Do a test thing");
    configure(command);
    const Result result = runCommand(command, {"--json"}, [&]() -> int {
        command.diagnostics().warn("odd-thing", "Something odd", "mesh 0");
        command.diagnostics().warn("odd-thing", "Something odd", "mesh 1");
        throw common::rejected("invalid-gltf", "bad model");
    });
    CHECK_EQ(result.code, 1);
    const auto report = nlohmann::json::parse(result.out);
    CHECK_EQ(report.at("error").at("code").get<std::string>(), std::string("invalid-gltf"));
    CHECK_EQ(report.at("diagnostics").at(0).at("count").get<int>(), 2);
    CHECK(result.err.find("warning: Something odd [odd-thing] (2 times, e.g. mesh 0; mesh 1)") != std::string::npos);
}

TEST_CASE(unexpected_exceptions_are_internal_errors) {
    cli::Command command("test", "Do a test thing");
    configure(command);
    const Result result = runCommand(command, {}, []() -> int { throw std::logic_error("bug"); });
    CHECK_EQ(result.code, 70);
    CHECK(result.out.empty());
    CHECK(result.err.find("internal error: bug") != std::string::npos);
}

TEST_CASE(success_prints_diagnostics_on_stderr_only) {
    cli::Command command("test", "Do a test thing");
    configure(command);
    const Result result = runCommand(command, {"--trusted"}, [&] {
        CHECK(command.trusted());
        command.diagnostics().warn("odd-thing", "Something odd");
        return 0;
    });
    CHECK_EQ(result.code, 0);
    CHECK(result.out.empty());
    CHECK_EQ(result.err, std::string("meshtinker-test: warning: Something odd [odd-thing]\n"));
}

TEST_CASE(diagnostics_keep_three_examples) {
    common::Diagnostics diagnostics;
    for (int i = 0; i < 5; ++i) {
        diagnostics.warn("x", "first message", "example " + std::to_string(i));
    }
    diagnostics.warn("x", "second message");
    CHECK_EQ(diagnostics.entries().size(), 1u);
    CHECK_EQ(diagnostics.entries()[0].count, 6u);
    CHECK_EQ(diagnostics.entries()[0].examples.size(), 3u);
    CHECK_EQ(diagnostics.entries()[0].message, std::string("first message"));
}

TEST_CASE(output_must_end_in_gltf) {
    cli::Command command("test", "Do a test thing");
    command.addInput();
    command.addOutput();
    const Result result = runCommand(command, {"in.glb", "out.glb"}, [] { return 0; });
    CHECK_EQ(result.code, 2);
}

TEST_CASE(output_directory_must_exist) {
    cli::Command command("test", "Do a test thing");
    command.addInput();
    command.addOutput();
    command.addJsonOption();
    std::string directory;
    std::string stem;
    const Result result = runCommand(command, {"--json", "in.glb", "missing-directory/out.gltf"}, [&] {
        stem = command.outputStem();
        directory = command.outputDirectory().string();
        return 0;
    });
    CHECK_EQ(result.code, 4);
    CHECK_EQ(stem, std::string("out"));
    CHECK(directory.empty());
    const auto report = nlohmann::json::parse(result.out);
    CHECK_EQ(report.at("error").at("code").get<std::string>(), std::string("output-directory-missing"));
}

TEST_CASE(output_without_directory_is_the_working_directory) {
    cli::Command command("test", "Do a test thing");
    command.addInput();
    command.addOutput();
    std::string directory;
    const Result result = runCommand(command, {"in.glb", "out.gltf"}, [&] {
        directory = command.outputDirectory().string();
        return 0;
    });
    CHECK_EQ(result.code, 0);
    CHECK_EQ(directory, std::string("."));
    CHECK_EQ(command.input().string(), std::string("in.glb"));
}

TEST_CASE(workspace_default_follows_the_positionals) {
    cli::Command reader("test", "Do a test thing");
    reader.addInput();
    reader.addWorkspaceOptions();
    CHECK(reader.app().get_option("--tmp-dir")->get_description().find("(default: the input's directory)") != std::string::npos);
    cli::Command writer("test", "Do a test thing");
    writer.addInput();
    writer.addOutput();
    writer.addWorkspaceOptions();
    CHECK(writer.app().get_option("--tmp-dir")->get_description().find("(default: the output's directory)") != std::string::npos);
}
