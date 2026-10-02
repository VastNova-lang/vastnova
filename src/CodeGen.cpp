#include "CodeGen.h"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>
#include <sstream>
#include <map>
#include <vector>
#include <memory>

namespace vastnova {

class LLVMCodeGen {
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::IRBuilder<>> builder;
    llvm::Function* mainFunc;
    llvm::BasicBlock* entryBB;
    std::map<std::string, llvm::AllocaInst*> varMap;
    std::map<std::string, llvm::Constant*> constMap;
    std::vector<std::pair<llvm::BasicBlock*, llvm::BasicBlock*>> loopStack;
    std::string currentReturnType;

public:
    LLVMCodeGen() : module(std::make_unique<llvm::Module>("vastnova", context)),
                    builder(std::make_unique<llvm::IRBuilder<>>(context)) {}

    std::string generate(const Program& prog) {
        std::vector<const FunctionDecl*> funcs;
        std::vector<const ASTNode*> mainStmts;
        for (auto& s : prog.statements) {
            if (s->type == NodeType::FunctionDecl)
                funcs.push_back(static_cast<const FunctionDecl*>(s.get()));
            else
                mainStmts.push_back(s.get());
        }

        llvm::FunctionType* mainType = llvm::FunctionType::get(
            llvm::Type::getInt32Ty(context), false);
        mainFunc = llvm::Function::Create(mainType, llvm::Function::ExternalLinkage,
                                          "main", module.get());

        for (auto* f : funcs) {
            std::vector<llvm::Type*> paramTys;
            for (auto& p : f->params) paramTys.push_back(mapType(p.type));
            llvm::Type* retTy = mapType(f->returnType);
            auto* fnTy = llvm::FunctionType::get(retTy, paramTys, false);
            if (!module->getFunction(f->name)) {
                llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                       f->name, module.get());
            }
        }

        entryBB = llvm::BasicBlock::Create(context, "entry", mainFunc);
        builder->SetInsertPoint(entryBB);

        declarePrintf();
        declareScanf();
        declareStringFunctions();

        currentReturnType = "void";
        for (auto* s : mainStmts) compileStmt(s);
        builder->CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));

        for (auto* f : funcs) {
            auto* fn = module->getFunction(f->name);
            auto* bb = llvm::BasicBlock::Create(context, "entry", fn);

            auto savedVarMap    = std::move(varMap);
            auto savedConstMap  = std::move(constMap);
            auto savedLoopStack = std::move(loopStack);
            auto* savedMainFunc = mainFunc;
            auto  savedRetType  = currentReturnType;

            varMap.clear();
            constMap.clear();
            loopStack.clear();
            mainFunc = fn;
            currentReturnType = f->returnType;

            builder->SetInsertPoint(bb);

            auto argIt = fn->arg_begin();
            for (auto& p : f->params) {
                llvm::Type* pty = mapType(p.type);
                auto* alloca = builder->CreateAlloca(pty, nullptr, p.name);
                builder->CreateStore(&*argIt, alloca);
                varMap[p.name] = alloca;
                ++argIt;
            }

            auto* body = static_cast<Block*>(f->body.get());
            if (body) {
                for (auto& s : body->statements) compileStmt(s.get());
            }

            if (!builder->GetInsertBlock()->getTerminator()) {
                addDefaultReturn(f->returnType);
            }

            varMap    = std::move(savedVarMap);
            constMap  = std::move(savedConstMap);
            loopStack = std::move(savedLoopStack);
            mainFunc  = savedMainFunc;
            currentReturnType = savedRetType;
        }

        if (llvm::verifyModule(*module, &llvm::errs())) {
            llvm::errs() << "Module verification failed\n";
        }

        std::string ir;
        llvm::raw_string_ostream stream(ir);
        module->print(stream, nullptr);
        return ir;
    }

private:
    llvm::PointerType* getInt8PtrTy() {
        return llvm::PointerType::get(context, 0);
    }

    llvm::Type* mapType(const std::string& t) {
        if (t == "i32")  return llvm::Type::getInt32Ty(context);
        if (t == "i64")  return llvm::Type::getInt64Ty(context);
        if (t == "f32")  return llvm::Type::getFloatTy(context);
        if (t == "f64")  return llvm::Type::getDoubleTy(context);
        if (t == "str")  return getInt8PtrTy();
        if (t == "void" || t.empty()) return llvm::Type::getVoidTy(context);
        return llvm::Type::getInt32Ty(context);
    }

    void addDefaultReturn(const std::string& retType) {
        llvm::Type* ty = mapType(retType);
        if (ty->isVoidTy()) {
            builder->CreateRetVoid();
        } else if (ty->isIntegerTy()) {
            builder->CreateRet(llvm::ConstantInt::get(ty, 0));
        } else if (ty->isFloatingPointTy()) {
            builder->CreateRet(llvm::ConstantFP::get(ty, 0.0));
        } else if (ty->isPointerTy()) {
            builder->CreateRet(llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(ty)));
        } else {
            builder->CreateRetVoid();
        }
    }

    void declarePrintf() {
        std::vector<llvm::Type*> args = {getInt8PtrTy()};
        llvm::FunctionType* fnType = llvm::FunctionType::get(
            llvm::Type::getInt32Ty(context), args, true);
        module->getOrInsertFunction("printf", fnType);
    }

    void declareScanf() {
        std::vector<llvm::Type*> args = {getInt8PtrTy()};
        llvm::FunctionType* fnType = llvm::FunctionType::get(
            llvm::Type::getInt32Ty(context), args, true);
        module->getOrInsertFunction("scanf", fnType);
    }

    void declareStringFunctions() {
        std::vector<llvm::Type*> mallocArgs = {llvm::Type::getInt64Ty(context)};
        llvm::FunctionType* mallocType = llvm::FunctionType::get(
            getInt8PtrTy(), mallocArgs, false);
        module->getOrInsertFunction("malloc", mallocType);

        std::vector<llvm::Type*> strcpyArgs = {getInt8PtrTy(), getInt8PtrTy()};
        llvm::FunctionType* strcpyType = llvm::FunctionType::get(
            getInt8PtrTy(), strcpyArgs, false);
        module->getOrInsertFunction("strcpy", strcpyType);

        std::vector<llvm::Type*> strcatArgs = {getInt8PtrTy(), getInt8PtrTy()};
        llvm::FunctionType* strcatType = llvm::FunctionType::get(
            getInt8PtrTy(), strcatArgs, false);
        module->getOrInsertFunction("strcat", strcatType);

        std::vector<llvm::Type*> strlenArgs = {getInt8PtrTy()};
        llvm::FunctionType* strlenType = llvm::FunctionType::get(
            llvm::Type::getInt64Ty(context), strlenArgs, false);
        module->getOrInsertFunction("strlen", strlenType);

        std::vector<llvm::Type*> snprintfArgs = {
            getInt8PtrTy(), llvm::Type::getInt64Ty(context), getInt8PtrTy()
        };
        llvm::FunctionType* snprintfType = llvm::FunctionType::get(
            llvm::Type::getInt32Ty(context), snprintfArgs, true);
        module->getOrInsertFunction("snprintf", snprintfType);

        std::vector<llvm::Type*> atoiArgs = {getInt8PtrTy()};
        llvm::FunctionType* atoiType = llvm::FunctionType::get(
            llvm::Type::getInt32Ty(context), atoiArgs, false);
        module->getOrInsertFunction("atoi", atoiType);

        std::vector<llvm::Type*> atofArgs = {getInt8PtrTy()};
        llvm::FunctionType* atofType = llvm::FunctionType::get(
            llvm::Type::getDoubleTy(context), atofArgs, false);
        module->getOrInsertFunction("atof", atofType);
    }

    llvm::Value* convertValue(llvm::Value* val, llvm::Type* targetTy) {
        if (val->getType() == targetTy) return val;
        if (targetTy->isIntegerTy()) {
            if (val->getType()->isIntegerTy()) {
                if (targetTy->getIntegerBitWidth() > val->getType()->getIntegerBitWidth())
                    return builder->CreateZExt(val, targetTy);
                else
                    return builder->CreateTrunc(val, targetTy);
            } else if (val->getType()->isFloatingPointTy()) {
                return builder->CreateFPToSI(val, targetTy);
            } else if (val->getType()->isPointerTy()) {
                return builder->CreatePtrToInt(val, targetTy);
            }
        } else if (targetTy->isFloatingPointTy()) {
            if (val->getType()->isIntegerTy()) {
                return builder->CreateSIToFP(val, targetTy);
            } else if (val->getType()->isFloatingPointTy()) {
                if (targetTy->getFPMantissaWidth() > val->getType()->getFPMantissaWidth())
                    return builder->CreateFPExt(val, targetTy);
                else
                    return builder->CreateFPTrunc(val, targetTy);
            }
        } else if (targetTy->isPointerTy()) {
            if (val->getType()->isIntegerTy()) {
                return builder->CreateIntToPtr(val, targetTy);
            }
        }
        return val;
    }

    llvm::Value* compileStr(llvm::Value* value) {
        llvm::Type* bufType = llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 64);
        auto bufAlloca = builder->CreateAlloca(bufType, nullptr, "str_buf");
        auto bufPtr = builder->CreatePointerCast(bufAlloca, getInt8PtrTy());

        std::string formatStr;
        if (value->getType()->isIntegerTy()) {
            formatStr = "%lld";
            value = builder->CreateSExt(value, llvm::Type::getInt64Ty(context));
        } else if (value->getType()->isFloatingPointTy()) {
            formatStr = "%g";
        } else {
            return llvm::ConstantPointerNull::get(getInt8PtrTy());
        }
        auto formatG = builder->CreateGlobalString(formatStr, "str_fmt");

        auto snprintfFn = module->getFunction("snprintf");
        builder->CreateCall(snprintfFn, {
            bufPtr,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 64),
            formatG,
            value
        });

        auto strlenFn = module->getFunction("strlen");
        auto actualLen = builder->CreateCall(strlenFn, {bufPtr});
        auto plusOne = builder->CreateAdd(actualLen,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 1));

        auto mallocFn = module->getFunction("malloc");
        auto result = builder->CreateCall(mallocFn, {plusOne}, "str_result");
        auto strcpyFn = module->getFunction("strcpy");
        builder->CreateCall(strcpyFn, {result, bufPtr});
        return result;
    }

    llvm::Value* compileInt(const Call* call) {
        if (call->args.size() != 1) return nullptr;
        auto arg = compileExpr(call->args[0].get());
        if (!arg) return nullptr;
        if (arg->getType()->isPointerTy()) {
            auto atoiFn = module->getFunction("atoi");
            return builder->CreateCall(atoiFn, {arg});
        } else if (arg->getType()->isIntegerTy() || arg->getType()->isFloatingPointTy()) {
            return convertValue(arg, llvm::Type::getInt32Ty(context));
        }
        return nullptr;
    }

    llvm::Value* compileFloat(const Call* call) {
        if (call->args.size() != 1) return nullptr;
        auto arg = compileExpr(call->args[0].get());
        if (!arg) return nullptr;
        if (arg->getType()->isPointerTy()) {
            auto atofFn = module->getFunction("atof");
            return builder->CreateCall(atofFn, {arg});
        } else if (arg->getType()->isIntegerTy() || arg->getType()->isFloatingPointTy()) {
            return convertValue(arg, llvm::Type::getDoubleTy(context));
        }
        return nullptr;
    }

    llvm::Value* compileInput(const Call* call) {
        if (!call->args.empty()) {
            auto prompt = compileExpr(call->args[0].get());
            if (prompt) {
                builder->CreateCall(module->getFunction("printf"), {prompt});
            }
        }
        auto bufferType = llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 1024);
        auto bufferAlloca = builder->CreateAlloca(bufferType, nullptr, "input_buffer");
        auto formatStr = builder->CreateGlobalString("%1023s", "scanf_fmt");
        auto bufferPtr = builder->CreatePointerCast(bufferAlloca, getInt8PtrTy());
        builder->CreateCall(module->getFunction("scanf"), {formatStr, bufferPtr});

        auto strlenFn = module->getFunction("strlen");
        auto len = builder->CreateCall(strlenFn, {bufferPtr}, "strlen");
        auto plusOne = builder->CreateAdd(len,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 1));
        auto mallocFn = module->getFunction("malloc");
        auto result = builder->CreateCall(mallocFn, {plusOne}, "strdup_result");
        auto strcpyFn = module->getFunction("strcpy");
        builder->CreateCall(strcpyFn, {result, bufferPtr});
        return result;
    }

    llvm::Value* concatStrings(llvm::Value* left, llvm::Value* right) {
        auto strlenFn = module->getFunction("strlen");
        auto lenL = builder->CreateCall(strlenFn, {left});
        auto lenR = builder->CreateCall(strlenFn, {right});
        auto total = builder->CreateAdd(builder->CreateAdd(lenL, lenR),
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 1));
        auto mallocFn = module->getFunction("malloc");
        auto result = builder->CreateCall(mallocFn, {total});
        auto strcpyFn = module->getFunction("strcpy");
        builder->CreateCall(strcpyFn, {result, left});
        auto strcatFn = module->getFunction("strcat");
        builder->CreateCall(strcatFn, {result, right});
        return result;
    }

    llvm::Value* callStrcmp(llvm::Value* left, llvm::Value* right) {
        auto strcmpFn = module->getOrInsertFunction("strcmp",
            llvm::FunctionType::get(llvm::Type::getInt32Ty(context),
                                    {getInt8PtrTy(), getInt8PtrTy()}, false));
        auto cmp = builder->CreateCall(strcmpFn, {left, right});
        auto cmpZero = builder->CreateICmpEQ(cmp,
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));
        return builder->CreateZExt(cmpZero, llvm::Type::getInt32Ty(context));
    }

    llvm::Value* compileExpr(const ASTNode* node) {
        switch (node->type) {
            case NodeType::Number: {
                auto* num = static_cast<const Number*>(node);
                if (num->value.find('.') != std::string::npos) {
                    return llvm::ConstantFP::get(llvm::Type::getDoubleTy(context),
                                                 std::stod(num->value));
                } else {
                    int64_t val = std::stoll(num->value);
                    return llvm::ConstantInt::get(
                        llvm::Type::getInt32Ty(context), val, true);
                }
            }
            case NodeType::StringLit: {
                auto* str = static_cast<const StringLiteral*>(node);
                return builder->CreateGlobalString(str->value, "str_lit");
            }
            case NodeType::Variable: {
                auto* var = static_cast<const Variable*>(node);
                if (constMap.count(var->name)) {
                    return constMap[var->name];
                }
                auto it = varMap.find(var->name);
                if (it != varMap.end()) {
                    auto* alloca = it->second;
                    auto* ty = alloca->getAllocatedType();
                    return builder->CreateLoad(ty, alloca, var->name);
                }
                llvm::errs() << "Error: unknown variable '" << var->name << "'\n";
                return nullptr;
            }
            case NodeType::UnaryOp: {
                auto* u = static_cast<const UnaryOp*>(node);
                auto v = compileExpr(u->operand.get());
                if (!v) return nullptr;
                if (u->op == "-") {
                    if (v->getType()->isIntegerTy()) {
                        return builder->CreateNeg(v, "negtmp");
                    } else if (v->getType()->isFloatingPointTy()) {
                        return builder->CreateFNeg(v, "fnegtmp");
                    } else {
                        llvm::errs() << "Error: unary '-' cannot be applied to this type\n";
                        return nullptr;
                    }
                }
                llvm::errs() << "Error: unknown unary operator '" << u->op << "'\n";
                return nullptr;
            }
            case NodeType::BinaryOp: {
                auto* bin = static_cast<const BinaryOp*>(node);
                auto left = compileExpr(bin->left.get());
                auto right = compileExpr(bin->right.get());
                if (!left || !right) return nullptr;
                std::string op = bin->op;

                if (op == ">" || op == "<" || op == "==" || op == "!=" ||
                    op == ">=" || op == "<=") {
                    bool leftIsPtr  = left->getType()->isPointerTy();
                    bool rightIsPtr = right->getType()->isPointerTy();

                    if (leftIsPtr && rightIsPtr) {
                        if (op == "==" || op == "!=") {
                            return callStrcmp(left, right);
                        }
                        llvm::errs() << "Error: string comparison with '" << op
                                     << "' is not allowed. Use only == or != for strings.\n";
                        return nullptr;
                    }

                    llvm::Type* commonTy;
                    if (left->getType()->isFloatingPointTy() || right->getType()->isFloatingPointTy())
                        commonTy = llvm::Type::getDoubleTy(context);
                    else
                        commonTy = llvm::Type::getInt32Ty(context);

                    left  = convertValue(left, commonTy);
                    right = convertValue(right, commonTy);

                    llvm::Value* cmp = nullptr;
                    if (commonTy->isIntegerTy()) {
                        if (op == ">")  cmp = builder->CreateICmpSGT(left, right, "cmpgt");
                        else if (op == "<")  cmp = builder->CreateICmpSLT(left, right, "cmplt");
                        else if (op == "==") cmp = builder->CreateICmpEQ(left, right, "cmpeq");
                        else if (op == "!=") cmp = builder->CreateICmpNE(left, right, "cmpne");
                        else if (op == ">=") cmp = builder->CreateICmpSGE(left, right, "cmpge");
                        else if (op == "<=") cmp = builder->CreateICmpSLE(left, right, "cmple");
                    } else {
                        if (op == ">")  cmp = builder->CreateFCmpOGT(left, right, "fcmpgt");
                        else if (op == "<")  cmp = builder->CreateFCmpOLT(left, right, "fcmplt");
                        else if (op == "==") cmp = builder->CreateFCmpOEQ(left, right, "fcmpeq");
                        else if (op == "!=") cmp = builder->CreateFCmpONE(left, right, "fcmpne");
                        else if (op == ">=") cmp = builder->CreateFCmpOGE(left, right, "fcmpge");
                        else if (op == "<=") cmp = builder->CreateFCmpOLE(left, right, "fcmple");
                    }
                    return builder->CreateZExt(cmp,
                        llvm::Type::getInt32Ty(context), "cmp_zext");
                }

                if (op == "&&") {
                    auto lb = builder->CreateICmpNE(left,
                        llvm::ConstantInt::get(left->getType(), 0));
                    auto rb = builder->CreateICmpNE(right,
                        llvm::ConstantInt::get(right->getType(), 0));
                    auto andVal = builder->CreateAnd(lb, rb, "andtmp");
                    return builder->CreateZExt(andVal,
                        llvm::Type::getInt32Ty(context), "and_zext");
                }
                if (op == "||") {
                    auto lb = builder->CreateICmpNE(left,
                        llvm::ConstantInt::get(left->getType(), 0));
                    auto rb = builder->CreateICmpNE(right,
                        llvm::ConstantInt::get(right->getType(), 0));
                    auto orVal = builder->CreateOr(lb, rb, "ortmp");
                    return builder->CreateZExt(orVal,
                        llvm::Type::getInt32Ty(context), "or_zext");
                }

                bool stringConcat =
                    (op == "+" &&
                     left->getType()->isPointerTy() &&
                     right->getType()->isPointerTy());
                if (stringConcat) return concatStrings(left, right);

                llvm::Type* commonTy;
                if (left->getType()->isFloatingPointTy() ||
                    right->getType()->isFloatingPointTy())
                    commonTy = llvm::Type::getDoubleTy(context);
                else
                    commonTy = llvm::Type::getInt32Ty(context);

                left  = convertValue(left, commonTy);
                right = convertValue(right, commonTy);

                if (op == "+") {
                    return commonTy->isIntegerTy()
                        ? builder->CreateAdd(left, right, "addtmp")
                        : builder->CreateFAdd(left, right, "faddtmp");
                } else if (op == "-") {
                    return commonTy->isIntegerTy()
                        ? builder->CreateSub(left, right, "subtmp")
                        : builder->CreateFSub(left, right, "fsubtmp");
                } else if (op == "*") {
                    return commonTy->isIntegerTy()
                        ? builder->CreateMul(left, right, "multmp")
                        : builder->CreateFMul(left, right, "fmultmp");
                } else if (op == "/") {
                    return commonTy->isIntegerTy()
                        ? builder->CreateSDiv(left, right, "divtmp")
                        : builder->CreateFDiv(left, right, "fdivtmp");
                }
                return nullptr;
            }
            case NodeType::Call: {
                auto* call = static_cast<const Call*>(node);
                if (call->name == "input") return compileInput(call);
                if (call->name == "str") {
                    if (call->args.size() != 1) return nullptr;
                    auto arg = compileExpr(call->args[0].get());
                    if (!arg) return nullptr;
                    return compileStr(arg);
                }
                if (call->name == "int")   return compileInt(call);
                if (call->name == "float") return compileFloat(call);

                auto* fn = module->getFunction(call->name);
                if (!fn) {
                    llvm::errs() << "Error: unknown function '" << call->name << "'\n";
                    return nullptr;
                }
                if (call->args.size() != fn->arg_size()) {
                    llvm::errs() << "Error: function '" << call->name << "' expects "
                                 << fn->arg_size() << " argument(s), got "
                                 << call->args.size() << "\n";
                    return nullptr;
                }
                std::vector<llvm::Value*> args;
                for (size_t i = 0; i < call->args.size(); ++i) {
                    auto v = compileExpr(call->args[i].get());
                    if (!v) return nullptr;
                    auto* pty = fn->getFunctionType()->getParamType(i);
                    if (v->getType() != pty) v = convertValue(v, pty);
                    args.push_back(v);
                }
                if (fn->getReturnType()->isVoidTy()) {
                    return builder->CreateCall(fn, args);
                }
                return builder->CreateCall(fn, args, "calltmp");
            }
            default:
                return nullptr;
        }
    }

    void compileStmt(const ASTNode* stmt) {
        switch (stmt->type) {
            case NodeType::FunctionDecl:
                break;

            case NodeType::ReturnStmt: {
                auto* rs = static_cast<const ReturnStmt*>(stmt);

                if (currentReturnType == "void") {
                    if (rs->value) {
                        llvm::errs() << "Error: 'return' with a value inside a void function\n";
                    }
                    builder->CreateRetVoid();
                } else {
                    llvm::Type* retTy = mapType(currentReturnType);
                    if (!rs->value) {
                        llvm::errs() << "Error: 'return' without a value inside a non-void function\n";
                        addDefaultReturn(currentReturnType);
                    } else {
                        auto v = compileExpr(rs->value.get());
                        if (!v) {
                            addDefaultReturn(currentReturnType);
                        } else {
                            v = convertValue(v, retTy);
                            builder->CreateRet(v);
                        }
                    }
                }
                {
                    llvm::BasicBlock* dummy = llvm::BasicBlock::Create(
                        context, "after_ret", mainFunc);
                    builder->SetInsertPoint(dummy);
                }
                break;
            }

            case NodeType::VarDecl: {
                auto* vd = static_cast<const VarDecl*>(stmt);
                llvm::Type* ty = nullptr;

                if (!vd->type.empty()) {
                    ty = mapType(vd->type);
                } else if (vd->init) {
                    auto v = compileExpr(vd->init.get());
                    if (!v) {
                        ty = llvm::Type::getInt32Ty(context);
                    } else {
                        ty = v->getType();
                        auto* alloca = builder->CreateAlloca(ty, nullptr, vd->name);
                        varMap[vd->name] = alloca;
                        builder->CreateStore(v, alloca);
                        break;
                    }
                } else {
                    ty = llvm::Type::getInt32Ty(context);
                }

                auto* alloca = builder->CreateAlloca(ty, nullptr, vd->name);
                varMap[vd->name] = alloca;
                if (vd->init) {
                    auto val = compileExpr(vd->init.get());
                    if (val) {
                        if (val->getType() != ty) val = convertValue(val, ty);
                        builder->CreateStore(val, alloca);
                    }
                }
                break;
            }

            case NodeType::ConstDecl: {
                auto* cd = static_cast<const ConstDecl*>(stmt);
                auto val = compileExpr(cd->init.get());
                if (val && llvm::isa<llvm::Constant>(val)) {
                    constMap[cd->name] = llvm::cast<llvm::Constant>(val);
                }
                break;
            }

            case NodeType::Assign: {
                auto* as = static_cast<const Assign*>(stmt);
                auto it = varMap.find(as->name);
                if (it == varMap.end()) {
                    llvm::errs() << "Error: unknown variable '" << as->name << "'\n";
                    break;
                }
                auto val = compileExpr(as->value.get());
                if (val) {
                    auto* varTy = it->second->getAllocatedType();
                    if (val->getType() != varTy) val = convertValue(val, varTy);
                    builder->CreateStore(val, it->second);
                }
                break;
            }

            case NodeType::PrintStmt: {
                auto* ps = static_cast<const PrintStmt*>(stmt);
                size_t n = ps->args.size();
                for (size_t i = 0; i < n; ++i) {
                    auto val = compileExpr(ps->args[i].get());
                    if (!val) continue;
                    std::string format;
                    if (val->getType()->isIntegerTy())
                        format = "%d";
                    else if (val->getType()->isFloatingPointTy())
                        format = "%f";
                    else if (val->getType()->isPointerTy())
                        format = "%s";
                    else
                        format = "%p";

                    auto formatStr = builder->CreateGlobalString(format, "printf_fmt");
                    builder->CreateCall(module->getFunction("printf"), {formatStr, val});

                    if (i != n - 1) {
                        auto space = builder->CreateGlobalString(" ", "space");
                        builder->CreateCall(module->getFunction("printf"), {space});
                    }
                }
                auto newline = builder->CreateGlobalString("\n", "newline");
                builder->CreateCall(module->getFunction("printf"), {newline});
                break;
            }

            case NodeType::IfStmt: {
                auto* ifs = static_cast<const IfStmt*>(stmt);
                auto condVal = compileExpr(ifs->condition.get());
                if (!condVal) break;

                llvm::Value* condBool;
                if (condVal->getType()->isIntegerTy())
                    condBool = builder->CreateICmpNE(condVal,
                        llvm::ConstantInt::get(condVal->getType(), 0));
                else if (condVal->getType()->isFloatingPointTy())
                    condBool = builder->CreateFCmpONE(condVal,
                        llvm::ConstantFP::get(condVal->getType(), 0.0));
                else if (condVal->getType()->isPointerTy())
                    condBool = builder->CreateICmpNE(condVal,
                        llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(condVal->getType())));
                else
                    condBool = builder->CreateICmpNE(condVal,
                        llvm::ConstantInt::get(condVal->getType(), 0));

                llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(
                    context, "if_then", mainFunc);
                llvm::BasicBlock* endBB = llvm::BasicBlock::Create(
                    context, "if_end", mainFunc);
                llvm::BasicBlock* elseBB = nullptr;

                if (ifs->elseBlock) {
                    elseBB = llvm::BasicBlock::Create(context, "if_else", mainFunc);
                    builder->CreateCondBr(condBool, thenBB, elseBB);
                } else {
                    builder->CreateCondBr(condBool, thenBB, endBB);
                }

                builder->SetInsertPoint(thenBB);
                if (auto* tn = static_cast<Block*>(ifs->thenBlock.get()))
                    for (auto& s : tn->statements) compileStmt(s.get());
                builder->CreateBr(endBB);

                if (elseBB) {
                    builder->SetInsertPoint(elseBB);
                    if (auto* en = static_cast<Block*>(ifs->elseBlock.get()))
                        for (auto& s : en->statements) compileStmt(s.get());
                    builder->CreateBr(endBB);
                }

                builder->SetInsertPoint(endBB);
                break;
            }

            case NodeType::WhileStmt: {
                auto* ws = static_cast<const WhileStmt*>(stmt);
                llvm::BasicBlock* condBB = llvm::BasicBlock::Create(
                    context, "while_cond", mainFunc);
                llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(
                    context, "while_body", mainFunc);
                llvm::BasicBlock* endBB  = llvm::BasicBlock::Create(
                    context, "while_end", mainFunc);

                loopStack.push_back({condBB, endBB});
                builder->CreateBr(condBB);

                builder->SetInsertPoint(condBB);
                auto condVal = compileExpr(ws->condition.get());
                if (!condVal) {
                    loopStack.pop_back();
                    break;
                }
                llvm::Value* condBool;
                if (condVal->getType()->isIntegerTy())
                    condBool = builder->CreateICmpNE(condVal,
                        llvm::ConstantInt::get(condVal->getType(), 0));
                else if (condVal->getType()->isFloatingPointTy())
                    condBool = builder->CreateFCmpONE(condVal,
                        llvm::ConstantFP::get(condVal->getType(), 0.0));
                else if (condVal->getType()->isPointerTy())
                    condBool = builder->CreateICmpNE(condVal,
                        llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(condVal->getType())));
                else
                    condBool = builder->CreateICmpNE(condVal,
                        llvm::ConstantInt::get(condVal->getType(), 0));
                builder->CreateCondBr(condBool, bodyBB, endBB);

                builder->SetInsertPoint(bodyBB);
                if (auto* bn = static_cast<Block*>(ws->body.get()))
                    for (auto& s : bn->statements) compileStmt(s.get());
                builder->CreateBr(condBB);

                builder->SetInsertPoint(endBB);
                loopStack.pop_back();
                break;
            }

            case NodeType::BreakStmt: {
                if (loopStack.empty()) {
                    llvm::errs() << "Error: 'break' outside of loop\n";
                    break;
                }
                builder->CreateBr(loopStack.back().second);
                llvm::BasicBlock* dummy = llvm::BasicBlock::Create(
                    context, "break_dummy", mainFunc);
                builder->SetInsertPoint(dummy);
                break;
            }

            case NodeType::ContinueStmt: {
                if (loopStack.empty()) {
                    llvm::errs() << "Error: 'continue' outside of loop\n";
                    break;
                }
                builder->CreateBr(loopStack.back().first);
                llvm::BasicBlock* dummy = llvm::BasicBlock::Create(
                    context, "continue_dummy", mainFunc);
                builder->SetInsertPoint(dummy);
                break;
            }

            case NodeType::Call: {
                compileExpr(stmt);
                break;
            }

            default:
                break;
        }
    }
};

std::string compileToLLVM(const Program& prog) {
    LLVMCodeGen cg;
    return cg.generate(prog);
}

} // namespace vastnova