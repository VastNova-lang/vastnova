#ifndef VASTNOVA_H
#define VASTNOVA_H

#include "vastnova_ast.h"
#include <iostream>
#include <string>
#include <vector>
#include <cctype>
#include <stdexcept>
#include <sstream>
#include <fstream>
#include <unordered_set>
#include <filesystem>
#include <cstdlib>

namespace fs = std::filesystem;

namespace vastnova {

extern std::string g_stdDir;

struct Token {
    enum Type {
        Ident, Number, String, Keyword, Symbol, Eof
    };
    Type type;
    std::string value;
    Token(Type t, const std::string& v = "") : type(t), value(v) {}
};

class Tokenizer {
    std::string src;
    size_t pos = 0;
public:
    Tokenizer(const std::string& s) : src(s) {}
    std::vector<Token> tokenize() {
        std::vector<Token> tokens;
        size_t i = 0;
        bool inMultiLineComment = false;

        while (i < src.size()) {
            char c = src[i];

            if (inMultiLineComment) {
                if (c == '#' && i + 1 < src.size() && src[i+1] == '!') {
                    inMultiLineComment = false;
                    i += 2;
                } else {
                    i++;
                }
                continue;
            }

            if (c == '!' && i + 1 < src.size() && src[i+1] == '#') {
                inMultiLineComment = true;
                i += 2;
                continue;
            }

            if (std::isspace(c)) {
                i++;
                continue;
            }

            if (c == '/' && i + 1 < src.size() && src[i+1] == '/') {
                while (i < src.size() && src[i] != '\n') i++;
                continue;
            }
            if (c == '#') {
                while (i < src.size() && src[i] != '\n') i++;
                continue;
            }

            if (std::isalpha(c) || c == '_') {
                std::string ident;
                while (i < src.size() && (std::isalnum(src[i]) || src[i] == '_')) {
                    ident += src[i++];
                }
                if (ident == "var" || ident == "let" || ident == "if" || ident == "else" ||
                    ident == "while" || ident == "break" || ident == "continue" ||
                    ident == "print" || ident == "i32" || ident == "i64" ||
                    ident == "f32" || ident == "f64" || ident == "str" ||
                    ident == "input" || ident == "int" || ident == "float" ||
                    ident == "import") {
                    tokens.emplace_back(Token::Keyword, ident);
                } else {
                    tokens.emplace_back(Token::Ident, ident);
                }
                continue;
            }

            if (std::isdigit(c) || (c == '.' && i + 1 < src.size() && std::isdigit(src[i+1]))) {
                std::string num;
                while (i < src.size() && (std::isdigit(src[i]) || src[i] == '.')) {
                    num += src[i++];
                }
                tokens.emplace_back(Token::Number, num);
                continue;
            }

            if (c == '"') {
                std::string str;
                i++;
                while (i < src.size() && src[i] != '"') {
                    if (src[i] == '\\' && i + 1 < src.size()) {
                        str += src[i++];
                        str += src[i++];
                    } else {
                        str += src[i++];
                    }
                }
                if (i < src.size()) i++;
                tokens.emplace_back(Token::String, str);
                continue;
            }

            // Multi-character symbols
            if (c == '=' && i + 1 < src.size() && src[i+1] == '=') {
                tokens.emplace_back(Token::Symbol, "==");
                i += 2;
                continue;
            }
            if (c == '!' && i + 1 < src.size() && src[i+1] == '=') {
                tokens.emplace_back(Token::Symbol, "!=");
                i += 2;
                continue;
            }
            if (c == '>' && i + 1 < src.size() && src[i+1] == '=') {
                tokens.emplace_back(Token::Symbol, ">=");
                i += 2;
                continue;
            }
            if (c == '<' && i + 1 < src.size() && src[i+1] == '=') {
                tokens.emplace_back(Token::Symbol, "<=");
                i += 2;
                continue;
            }
            if (c == '&' && i + 1 < src.size() && src[i+1] == '&') {
                tokens.emplace_back(Token::Symbol, "&&");
                i += 2;
                continue;
            }
            if (c == '|' && i + 1 < src.size() && src[i+1] == '|') {
                tokens.emplace_back(Token::Symbol, "||");
                i += 2;
                continue;
            }

            // Single-character symbols
            if (c == '+' || c == '-' || c == '*' || c == '/' || c == '(' || c == ')' ||
                c == '{' || c == '}' || c == '[' || c == ']' || c == ':' || c == ';' ||
                c == ',' || c == '.' || c == '<' || c == '>' || c == '=' || c == '?' ||
                c == '~' || c == '%' || c == '^' || c == '&' || c == '|') {
                tokens.emplace_back(Token::Symbol, std::string(1, c));
                i++;
                continue;
            }

            throw std::runtime_error("Unexpected character: " + std::string(1, c));
        }

        tokens.emplace_back(Token::Eof, "");
        return tokens;
    }
};

class Parser {
    std::vector<Token> tokens;
    size_t idx = 0;
    std::unordered_set<std::string> importedFiles;
    Program* currentProgram = nullptr;

    Token current() const { return tokens[idx]; }
    void advance() { if (idx < tokens.size()) idx++; }
    void expect(Token::Type type, const std::string& msg) {
        if (current().type != type) throw std::runtime_error(msg);
    }
    bool match(Token::Type type) {
        if (current().type == type) { advance(); return true; }
        return false;
    }
    bool matchSymbol(const std::string& sym) {
        if (current().type == Token::Symbol && current().value == sym) {
            advance();
            return true;
        }
        return false;
    }

    std::unique_ptr<ASTNode> parseExpression();
    std::unique_ptr<ASTNode> parsePrimary();
    std::unique_ptr<ASTNode> parseBinaryOp(int minPrec);
    std::unique_ptr<ASTNode> parseCondition();
    std::unique_ptr<ASTNode> parseStatement();
    std::unique_ptr<Block> parseBlock();

    std::unique_ptr<Program> loadAndParseFile(const std::string& path) {
        std::vector<std::string> searchPaths;
        // 1. User-defined via global variable
        if (!g_stdDir.empty()) {
            searchPaths.push_back(g_stdDir + "/" + path);
        }
        // 2. Current working directory's std/
        searchPaths.push_back("std/" + path);
        // 3. Direct path (relative or absolute)
        searchPaths.push_back(path);

        fs::path foundPath;
        bool found = false;
        for (const auto& tryPath : searchPaths) {
            if (fs::exists(tryPath)) {
                foundPath = tryPath;
                found = true;
                break;
            }
        }

        if (!found) {
            throw std::runtime_error("Import file not found: " + path);
        }

        std::string canonical = fs::canonical(foundPath).string();
        if (importedFiles.count(canonical)) {
            return std::make_unique<Program>();
        }
        importedFiles.insert(canonical);

        std::ifstream in(foundPath);
        if (!in.is_open()) {
            throw std::runtime_error("Cannot open import file: " + path);
        }
        std::string code((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        in.close();

        Tokenizer tokenizer(code);
        auto tokens = tokenizer.tokenize();
        Parser parser(tokens);
        parser.importedFiles = this->importedFiles;
        auto importedProg = parser.parseProgram();
        this->importedFiles = parser.importedFiles;
        return importedProg;
    }

public:
    Parser(const std::vector<Token>& t) : tokens(t) {}
    std::unique_ptr<Program> parseProgram();
};

int getPrecedence(const std::string& op) {
    if (op == "||") return 0;
    if (op == "&&") return 1;
    if (op == "==" || op == "!=" || op == ">" || op == "<" || op == ">=" || op == "<=") return 2;
    if (op == "+" || op == "-") return 3;
    if (op == "*" || op == "/") return 4;
    return -1;
}

std::unique_ptr<ASTNode> Parser::parsePrimary() {
    Token tok = current();
    if (tok.type == Token::Number) {
        auto num = std::make_unique<Number>(tok.value);
        advance();
        return num;
    }
    if (tok.type == Token::String) {
        auto str = std::make_unique<StringLiteral>(tok.value);
        advance();
        return str;
    }
    if (tok.type == Token::Ident) {
        auto var = std::make_unique<Variable>(tok.value);
        advance();
        return var;
    }
    if (matchSymbol("(")) {
        auto expr = parseExpression();
        if (!matchSymbol(")")) throw std::runtime_error("Expected ')'");
        return expr;
    }
    if (tok.type == Token::Keyword && tok.value == "input") {
        advance();
        if (!matchSymbol("(")) throw std::runtime_error("Expected '(' after input");
        auto call = std::make_unique<Call>("input");
        if (!matchSymbol(")")) {
            while (true) {
                call->args.push_back(parseExpression());
                if (matchSymbol(",")) continue;
                if (matchSymbol(")")) break;
                throw std::runtime_error("Expected ',' or ')' in input arguments");
            }
        }
        return call;
    }
    if (tok.type == Token::Keyword && tok.value == "str") {
        advance();
        if (!matchSymbol("(")) throw std::runtime_error("Expected '(' after str");
        auto call = std::make_unique<Call>("str");
        if (matchSymbol(")")) {
            throw std::runtime_error("str requires one argument");
        }
        call->args.push_back(parseExpression());
        if (!matchSymbol(")")) throw std::runtime_error("Expected ')' after str argument");
        return call;
    }
    if (tok.type == Token::Keyword && tok.value == "int") {
        advance();
        if (!matchSymbol("(")) throw std::runtime_error("Expected '(' after int");
        auto call = std::make_unique<Call>("int");
        if (matchSymbol(")")) {
            throw std::runtime_error("int requires one argument");
        }
        call->args.push_back(parseExpression());
        if (!matchSymbol(")")) throw std::runtime_error("Expected ')' after int argument");
        return call;
    }
    if (tok.type == Token::Keyword && tok.value == "float") {
        advance();
        if (!matchSymbol("(")) throw std::runtime_error("Expected '(' after float");
        auto call = std::make_unique<Call>("float");
        if (matchSymbol(")")) {
            throw std::runtime_error("float requires one argument");
        }
        call->args.push_back(parseExpression());
        if (!matchSymbol(")")) throw std::runtime_error("Expected ')' after float argument");
        return call;
    }
    throw std::runtime_error("Unexpected token in expression: " + tok.value);
}

std::unique_ptr<ASTNode> Parser::parseBinaryOp(int minPrec) {
    auto left = parsePrimary();
    while (true) {
        if (current().type != Token::Symbol) break;
        std::string op = current().value;
        int prec = getPrecedence(op);
        if (prec < minPrec) break;
        if (op == "&&" || op == "||" || op == "==" || op == "!=" || op == ">" || op == "<" ||
            op == ">=" || op == "<=" || op == "+" || op == "-" || op == "*" || op == "/") {
            advance();
            auto right = parseBinaryOp(prec + 1);
            left = std::make_unique<BinaryOp>(op, std::move(left), std::move(right));
        } else {
            break;
        }
    }
    return left;
}

std::unique_ptr<ASTNode> Parser::parseExpression() {
    return parseBinaryOp(0);
}

std::unique_ptr<ASTNode> Parser::parseCondition() {
    return parseExpression();
}

std::unique_ptr<Block> Parser::parseBlock() {
    if (!matchSymbol("{")) throw std::runtime_error("Expected '{'");
    auto block = std::make_unique<Block>();
    while (!(current().type == Token::Symbol && current().value == "}") && current().type != Token::Eof) {
        auto stmt = parseStatement();
        if (stmt) {
            block->statements.push_back(std::move(stmt));
        }
    }
    if (!matchSymbol("}")) throw std::runtime_error("Expected '}'");
    return block;
}

std::unique_ptr<ASTNode> Parser::parseStatement() {
    Token tok = current();
    if (tok.type == Token::Keyword && tok.value == "var") {
        advance();
        if (current().type != Token::Ident) throw std::runtime_error("Expected variable name after 'var'");
        std::string name = current().value;
        advance();
        auto varDecl = std::make_unique<VarDecl>();
        varDecl->name = name;
        if (matchSymbol(":")) {
            if (current().type != Token::Keyword) throw std::runtime_error("Expected type after ':'");
            varDecl->type = current().value;
            advance();
        }
        if (matchSymbol("=")) {
            varDecl->init = parseExpression();
        }
        return varDecl;
    }
    if (tok.type == Token::Keyword && tok.value == "let") {
        advance();
        if (current().type != Token::Ident) throw std::runtime_error("Expected variable name after 'let'");
        std::string name = current().value;
        advance();
        auto constDecl = std::make_unique<ConstDecl>();
        constDecl->name = name;
        if (matchSymbol(":")) {
            if (current().type != Token::Keyword) throw std::runtime_error("Expected type after ':'");
            constDecl->type = current().value;
            advance();
        }
        if (!matchSymbol("=")) throw std::runtime_error("Expected '=' after constant declaration");
        constDecl->init = parseExpression();
        return constDecl;
    }
    if (tok.type == Token::Keyword && tok.value == "if") {
        advance();
        auto ifStmt = std::make_unique<IfStmt>();
        ifStmt->condition = parseCondition();
        ifStmt->thenBlock = parseBlock();
        if (current().type == Token::Keyword && current().value == "else") {
            advance();
            ifStmt->elseBlock = parseBlock();
        }
        return ifStmt;
    }
    if (tok.type == Token::Keyword && tok.value == "while") {
        advance();
        auto whileStmt = std::make_unique<WhileStmt>();
        whileStmt->condition = parseCondition();
        whileStmt->body = parseBlock();
        return whileStmt;
    }
    if (tok.type == Token::Keyword && tok.value == "break") {
        advance();
        return std::make_unique<BreakStmt>();
    }
    if (tok.type == Token::Keyword && tok.value == "continue") {
        advance();
        return std::make_unique<ContinueStmt>();
    }
    if (tok.type == Token::Keyword && tok.value == "print") {
        advance();
        if (!matchSymbol("(")) throw std::runtime_error("Expected '(' after print");
        auto printStmt = std::make_unique<PrintStmt>();
        if (!matchSymbol(")")) {
            while (true) {
                printStmt->args.push_back(parseExpression());
                if (matchSymbol(",")) continue;
                if (matchSymbol(")")) break;
                throw std::runtime_error("Expected ',' or ')' in print arguments");
            }
        }
        return printStmt;
    }
    if (tok.type == Token::Keyword && tok.value == "import") {
        advance();
        if (current().type != Token::String) throw std::runtime_error("Expected string literal after import");
        std::string path = current().value;
        advance();

        auto importedProg = loadAndParseFile(path);
        if (importedProg && currentProgram) {
            for (auto& stmt : importedProg->statements) {
                currentProgram->statements.push_back(std::move(stmt));
            }
        }
        return nullptr;
    }
    if (tok.type == Token::Ident) {
        std::string name = tok.value;
        advance();
        if (matchSymbol("=")) {
            auto assign = std::make_unique<Assign>();
            assign->name = name;
            assign->value = parseExpression();
            return assign;
        } else {
            throw std::runtime_error("Unexpected identifier in statement, maybe missing '='?");
        }
    }
    throw std::runtime_error("Unexpected token in statement: " + tok.value);
}

std::unique_ptr<Program> Parser::parseProgram() {
    auto prog = std::make_unique<Program>();
    currentProgram = prog.get();
    while (current().type != Token::Eof) {
        auto stmt = parseStatement();
        if (stmt) {
            prog->statements.push_back(std::move(stmt));
        }
    }
    return prog;
}

std::unique_ptr<Program> parse(const std::string& code) {
    Tokenizer tokenizer(code);
    auto tokens = tokenizer.tokenize();
    Parser parser(tokens);
    return parser.parseProgram();
}

void printAST(const ASTNode* node, int indent = 0) {
    std::string prefix(indent * 2, ' ');
    switch (node->type) {
        case NodeType::Program: {
            auto p = static_cast<const Program*>(node);
            std::cout << prefix << "Program\n";
            for (auto& stmt : p->statements) printAST(stmt.get(), indent + 1);
            break;
        }
        case NodeType::VarDecl: {
            auto v = static_cast<const VarDecl*>(node);
            std::cout << prefix << "VarDecl: " << v->name;
            if (!v->type.empty()) std::cout << " : " << v->type;
            if (v->init) { std::cout << " = "; printAST(v->init.get(), 0); }
            std::cout << "\n";
            break;
        }
        case NodeType::ConstDecl: {
            auto c = static_cast<const ConstDecl*>(node);
            std::cout << prefix << "ConstDecl: " << c->name;
            if (!c->type.empty()) std::cout << " : " << c->type;
            std::cout << " = "; printAST(c->init.get(), 0);
            std::cout << "\n";
            break;
        }
        case NodeType::Assign: {
            auto a = static_cast<const Assign*>(node);
            std::cout << prefix << "Assign: " << a->name << " = ";
            printAST(a->value.get(), 0);
            std::cout << "\n";
            break;
        }
        case NodeType::PrintStmt: {
            auto p = static_cast<const PrintStmt*>(node);
            std::cout << prefix << "Print: ";
            for (size_t i = 0; i < p->args.size(); ++i) {
                if (i > 0) std::cout << ", ";
                printAST(p->args[i].get(), 0);
            }
            std::cout << "\n";
            break;
        }
        case NodeType::IfStmt: {
            auto i = static_cast<const IfStmt*>(node);
            std::cout << prefix << "If condition: ";
            printAST(i->condition.get(), 0);
            std::cout << "\n" << prefix << "Then:\n";
            printAST(i->thenBlock.get(), indent + 1);
            if (i->elseBlock) {
                std::cout << prefix << "Else:\n";
                printAST(i->elseBlock.get(), indent + 1);
            }
            break;
        }
        case NodeType::WhileStmt: {
            auto w = static_cast<const WhileStmt*>(node);
            std::cout << prefix << "While condition: ";
            printAST(w->condition.get(), 0);
            std::cout << "\n" << prefix << "Body:\n";
            printAST(w->body.get(), indent + 1);
            break;
        }
        case NodeType::BreakStmt:
            std::cout << prefix << "Break\n";
            break;
        case NodeType::ContinueStmt:
            std::cout << prefix << "Continue\n";
            break;
        case NodeType::Block: {
            auto b = static_cast<const Block*>(node);
            std::cout << prefix << "Block:\n";
            for (auto& stmt : b->statements) printAST(stmt.get(), indent + 1);
            break;
        }
        case NodeType::Number: {
            auto n = static_cast<const Number*>(node);
            std::cout << prefix << "Number: " << n->value;
            break;
        }
        case NodeType::StringLit: {
            auto s = static_cast<const StringLiteral*>(node);
            std::cout << prefix << "String: \"" << s->value << "\"";
            break;
        }
        case NodeType::Variable: {
            auto v = static_cast<const Variable*>(node);
            std::cout << prefix << "Variable: " << v->name;
            break;
        }
        case NodeType::BinaryOp: {
            auto b = static_cast<const BinaryOp*>(node);
            std::cout << prefix << "BinaryOp(" << b->op << "):\n";
            printAST(b->left.get(), indent + 1);
            printAST(b->right.get(), indent + 1);
            break;
        }
        case NodeType::Call: {
            auto c = static_cast<const Call*>(node);
            std::cout << prefix << "Call: " << c->name << "(";
            for (size_t i = 0; i < c->args.size(); ++i) {
                if (i > 0) std::cout << ", ";
                printAST(c->args[i].get(), 0);
            }
            std::cout << ")";
            break;
        }
        default: break;
    }
}

} // namespace vastnova

#endif