#include "vastnova.h"
#include "CodeGen.h"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdlib>

// Helper to check if string ends with a suffix (for C++17 compatibility)
static bool ends_with(const std::string& str, const std::string& suffix) {
    if (suffix.size() > str.size()) return false;
    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool ends_with(const std::string& str, const char* suffix) {
    return ends_with(str, std::string(suffix));
}

int main(int argc, char* argv[]) {
    using namespace vastnova;

    if (argc < 2) {
        std::cerr << "Usage: vastnova <input.vn> [output] [--no-run]\n";
        std::cerr << "  <input.vn>  : VastNova source file\n";
        std::cerr << "  [output]    : optional output executable name\n";
        std::cerr << "  [--no-run]  : only generate IR, do not compile to executable\n";
        return 1;
    }

    std::string inputFile = argv[1];
    std::string outputFile;
    bool run = true;

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

    std::ifstream in(inputFile);
    if (!in.is_open()) {
        std::cerr << "Error: Cannot open file '" << inputFile << "'" << std::endl;
        return 1;
    }
    std::string code((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    in.close();

    try {
        auto ast = parse(code, inputFile);
        if (!ast) {
            std::cerr << "Parsing failed." << std::endl;
            return 1;
        }
        std::string ir = compileToLLVM(*ast);

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

        if (outputFile.empty()) {
            outputFile = inputFile;
            if (ends_with(outputFile, ".vn")) {
                outputFile = outputFile.substr(0, outputFile.size() - 3);
            }
        }

        std::string cmd = "clang " + llFile + " -o " + outputFile;
        int ret = std::system(cmd.c_str());
        if (ret != 0) {
            std::cerr << "Error: clang compilation failed (exit code " << ret << ")" << std::endl;
            return ret;
        }

        std::cout << "Executable created: " << outputFile << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}