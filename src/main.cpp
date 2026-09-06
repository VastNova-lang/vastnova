#include "vastnova.h"
#include "CodeGen.h"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;
using namespace vastnova;

// Helper to check if string ends with a suffix (for C++17 compatibility)
static bool ends_with(const std::string& str, const std::string& suffix) {
    if (suffix.size() > str.size()) return false;
    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool ends_with(const std::string& str, const char* suffix) {
    return ends_with(str, std::string(suffix));
}

// Definition of the global standard library directory variable
std::string vastnova::g_stdDir = "std";

int main(int argc, char* argv[]) {
    // Read environment variable VASTNOVA_STD if set
    const char* env_std = std::getenv("VASTNOVA_STD");
    if (env_std && *env_std) {
        g_stdDir = env_std;
    }

    if (argc < 2) {
        std::cerr << "Usage: vastnova <input.vn> [output] [--no-run]\n";
        std::cerr << "  <input.vn>  : VastNova source file\n";
        std::cerr << "  [output]    : optional output executable name (default: a.out or input name)\n";
        std::cerr << "  [--no-run]  : only generate IR, do not compile to executable\n";
        std::cerr << "Standard library directory can be set via VASTNOVA_STD environment variable.\n";
        return 1;
    }

    std::string inputFile = argv[1];
    std::string outputFile;
    bool run = true;

    // Parse optional arguments
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--no-run") {
            run = false;
        } else if (outputFile.empty()) {
            outputFile = arg;
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            return 1;
        }
    }

    // Read source file
    std::ifstream in(inputFile);
    if (!in.is_open()) {
        std::cerr << "Error: Cannot open file '" << inputFile << "'" << std::endl;
        return 1;
    }
    std::string code((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    in.close();

    // Parse and compile to LLVM IR
    auto ast = parse(code);
    if (!ast) {
        std::cerr << "Parsing failed." << std::endl;
        return 1;
    }
    std::string ir = compileToLLVM(*ast);

    // Generate temporary .ll file (same basename as input, but with .ll extension)
    std::string llFile = inputFile;
    if (ends_with(llFile, ".vn")) {
        llFile = llFile.substr(0, llFile.size() - 3) + ".ll";
    } else {
        llFile += ".ll";
    }

    std::ofstream llOut(llFile);
    if (!llOut) {
        std::cerr << "Error: Cannot write to '" << llFile << "'" << std::endl;
        return 1;
    }
    llOut << ir;
    llOut.close();

    std::cout << "Generated LLVM IR: " << llFile << std::endl;

    if (!run) {
        std::cout << "Compilation stopped as requested (--no-run).\n";
        return 0;
    }

    // Determine output executable name
    if (outputFile.empty()) {
        outputFile = inputFile;
        if (ends_with(outputFile, ".vn")) {
            outputFile = outputFile.substr(0, outputFile.size() - 3);
        }
#ifdef _WIN32
        outputFile += ".exe";
#endif
    } else {
#ifdef _WIN32
        if (!ends_with(outputFile, ".exe")) outputFile += ".exe";
#endif
    }

    // Invoke clang to generate executable
    std::string cmd = "clang " + llFile + " -o " + outputFile;
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        std::cerr << "Error: clang compilation failed (exit code " << ret << ")" << std::endl;
        return ret;
    }

    std::cout << "Executable created: " << outputFile << std::endl;

    // Optionally delete temporary .ll file
    // fs::remove(llFile);

    return 0;
}