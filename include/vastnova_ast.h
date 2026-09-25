#ifndef VASTNOVA_AST_H
#define VASTNOVA_AST_H

#include <string>
#include <vector>
#include <memory>

namespace vastnova {

enum class NodeType {
    Program,
    VarDecl,
    ConstDecl,
    Assign,
    PrintStmt,
    IfStmt,
    WhileStmt,
    BreakStmt,
    ContinueStmt,
    Block,
    Number,
    StringLit,
    Variable,
    BinaryOp,
    Call,
    ImportStmt
};

struct ASTNode {
    NodeType type;
    ASTNode(NodeType t) : type(t) {}
    virtual ~ASTNode() = default;
};

struct Program : ASTNode {
    std::vector<std::unique_ptr<ASTNode>> statements;
    Program() : ASTNode(NodeType::Program) {}
};

struct VarDecl : ASTNode {
    std::string name;
    std::string type;
    std::unique_ptr<ASTNode> init;
    VarDecl() : ASTNode(NodeType::VarDecl) {}
};

struct ConstDecl : ASTNode {
    std::string name;
    std::string type;
    std::unique_ptr<ASTNode> init;
    ConstDecl() : ASTNode(NodeType::ConstDecl) {}
};

struct Assign : ASTNode {
    std::string name;
    std::unique_ptr<ASTNode> value;
    Assign() : ASTNode(NodeType::Assign) {}
};

struct PrintStmt : ASTNode {
    std::vector<std::unique_ptr<ASTNode>> args;
    PrintStmt() : ASTNode(NodeType::PrintStmt) {}
};

struct IfStmt : ASTNode {
    std::unique_ptr<ASTNode> condition;
    std::unique_ptr<ASTNode> thenBlock;
    std::unique_ptr<ASTNode> elseBlock;
    IfStmt() : ASTNode(NodeType::IfStmt) {}
};

struct WhileStmt : ASTNode {
    std::unique_ptr<ASTNode> condition;
    std::unique_ptr<ASTNode> body;
    WhileStmt() : ASTNode(NodeType::WhileStmt) {}
};

struct BreakStmt : ASTNode {
    BreakStmt() : ASTNode(NodeType::BreakStmt) {}
};

struct ContinueStmt : ASTNode {
    ContinueStmt() : ASTNode(NodeType::ContinueStmt) {}
};

struct Block : ASTNode {
    std::vector<std::unique_ptr<ASTNode>> statements;
    Block() : ASTNode(NodeType::Block) {}
};

struct Number : ASTNode {
    std::string value;
    Number(const std::string& v) : ASTNode(NodeType::Number), value(v) {}
};

struct StringLiteral : ASTNode {
    std::string value;
    StringLiteral(const std::string& v) : ASTNode(NodeType::StringLit), value(v) {}
};

struct Variable : ASTNode {
    std::string name;
    Variable(const std::string& n) : ASTNode(NodeType::Variable), name(n) {}
};

struct BinaryOp : ASTNode {
    std::string op;
    std::unique_ptr<ASTNode> left;
    std::unique_ptr<ASTNode> right;
    BinaryOp(const std::string& o, std::unique_ptr<ASTNode> l, std::unique_ptr<ASTNode> r)
        : ASTNode(NodeType::BinaryOp), op(o), left(std::move(l)), right(std::move(r)) {}
};

struct Call : ASTNode {
    std::string name;
    std::vector<std::unique_ptr<ASTNode>> args;
    Call(const std::string& n) : ASTNode(NodeType::Call), name(n) {}
};

struct ImportStmt : ASTNode {
    std::string path;
    ImportStmt() : ASTNode(NodeType::ImportStmt) {}
};

} // namespace vastnova

#endif