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
#include <set>
#include <vector>
#include <memory>
#include <climits>
#include <string>

namespace vastnova {

class LLVMCodeGen {
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::IRBuilder<>> builder;
    llvm::Function* mainFunc;
    llvm::BasicBlock* entryBB;
    // 作用域栈：每层是一个符号表。变量与常量各维护一套，
    // 进入块（函数体/if/while）时压栈，离开时弹栈；查找从内向外。
    std::vector<std::map<std::string, llvm::AllocaInst*>> varScopes;
    std::vector<std::map<std::string, llvm::Constant*>> constScopes;
    std::vector<std::pair<llvm::BasicBlock*, llvm::BasicBlock*>> loopStack;
    std::string currentReturnType;
    bool inMain = true;
    bool hadError = false;

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

        std::vector<const FunctionDecl*> validFuncs;
        std::set<std::string> declaredFunctions;

        for (auto* f : funcs) {
            if (f->name == "main") {
                error("function name 'main' is reserved");
                continue;
            }
            if (!declaredFunctions.insert(f->name).second) {
                error("duplicate function '" + f->name + "'");
                continue;
            }
            std::vector<llvm::Type*> paramTys;
            bool badParam = false;
            for (auto& p : f->params) {
                if (p.type == "void") {
                    error("parameter '" + p.name + "' of function '" + f->name +
                          "' cannot be void");
                    badParam = true;
                    break;
                }
                paramTys.push_back(mapType(p.type));
            }
            if (badParam) continue;
            llvm::Type* retTy = mapType(f->returnType);
            auto* fnTy = llvm::FunctionType::get(retTy, paramTys, false);
            if (!module->getFunction(f->name)) {
                llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                       f->name, module.get());
            }
            validFuncs.push_back(f);
        }

        entryBB = llvm::BasicBlock::Create(context, "entry", mainFunc);
        builder->SetInsertPoint(entryBB);

        declarePrintf();
        declareScanf();
        declareStringFunctions();

        currentReturnType = "void";
        inMain = true;
        pushScope();  // 主程序作用域：变量与顶层常量
        for (auto* s : mainStmts) compileStmt(s);
        builder->CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));
        popVarScope();  // 主程序的局部变量在函数外不可见；常量保留供函数使用

        inMain = false;
        for (auto* f : validFuncs) {
            auto* fn = module->getFunction(f->name);
            auto* bb = llvm::BasicBlock::Create(context, "entry", fn);

            auto savedLoopStack = std::move(loopStack);
            auto* savedMainFunc = mainFunc;
            auto  savedRetType  = currentReturnType;

            loopStack.clear();
            mainFunc = fn;
            currentReturnType = f->returnType;

            builder->SetInsertPoint(bb);
            pushScope();  // 函数作用域

            auto argIt = fn->arg_begin();
            for (auto& p : f->params) {
                llvm::Type* pty = mapType(p.type);
                auto* alloca = builder->CreateAlloca(pty, nullptr, p.name);
                builder->CreateStore(&*argIt, alloca);
                declareVar(p.name, alloca);
                ++argIt;
            }

            auto* body = static_cast<Block*>(f->body.get());
            if (body) {
                for (auto& s : body->statements) compileStmt(s.get());
            }

            if (!builder->GetInsertBlock()->getTerminator()) {
                if (f->returnType != "void" &&
                    isReachableFrom(bb, builder->GetInsertBlock())) {
                    warn("function '" + f->name +
                         "' may not return a value on all paths");
                }
                addDefaultReturn(f->returnType);
            }

            popScope();  // 离开函数作用域
            loopStack = std::move(savedLoopStack);
            mainFunc  = savedMainFunc;
            currentReturnType = savedRetType;
        }

        if (llvm::verifyModule(*module, &llvm::errs())) {
            error("module verification failed");
        }
        if (hadError) {
            throw std::runtime_error("code generation failed");
        }

        std::string ir;
        llvm::raw_string_ostream stream(ir);
        module->print(stream, nullptr);
        return ir;
    }

private:
    void error(const std::string& msg) {
        llvm::errs() << "Error: " << msg << "\n";
        hadError = true;
    }

    void warn(const std::string& msg) {
        llvm::errs() << "Warning: " << msg << "\n";
    }

    // ---- 作用域管理 ----
    void pushVarScope()   { varScopes.emplace_back(); }
    void popVarScope()    { if (!varScopes.empty()) varScopes.pop_back(); }
    void pushConstScope() { constScopes.emplace_back(); }
    void popConstScope()  { if (!constScopes.empty()) constScopes.pop_back(); }
    void pushScope()      { pushVarScope(); pushConstScope(); }
    void popScope()       { popVarScope(); popConstScope(); }

    llvm::AllocaInst* lookupVar(const std::string& name) {
        for (auto it = varScopes.rbegin(); it != varScopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return f->second;
        }
        return nullptr;
    }

    llvm::Constant* lookupConst(const std::string& name) {
        for (auto it = constScopes.rbegin(); it != constScopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return f->second;
        }
        return nullptr;
    }

    bool declareVar(const std::string& name, llvm::AllocaInst* alloca) {
        if (varScopes.empty()) return false;
        auto& cur = varScopes.back();
        if (cur.count(name)) {
            error("variable '" + name + "' already declared in this scope");
            return false;
        }
        if (!constScopes.empty() && constScopes.back().count(name)) {
            error("'" + name + "' already declared as a constant in this scope");
            return false;
        }
        cur[name] = alloca;
        return true;
    }

    bool declareConst(const std::string& name, llvm::Constant* c) {
        if (constScopes.empty()) return false;
        auto& cur = constScopes.back();
        if (cur.count(name)) {
            error("constant '" + name + "' already declared in this scope");
            return false;
        }
        if (!varScopes.empty() && varScopes.back().count(name)) {
            error("'" + name + "' already declared as a variable in this scope");
            return false;
        }
        cur[name] = c;
        return true;
    }


    static bool isReachableFrom(llvm::BasicBlock* from, llvm::BasicBlock* target) {
        std::set<llvm::BasicBlock*> visited;
        std::vector<llvm::BasicBlock*> work;
        work.push_back(from);
        while (!work.empty()) {
            auto* bb = work.back();
            work.pop_back();
            if (!visited.insert(bb).second) continue;
            if (bb == target) return true;
            auto* term = bb->getTerminator();
            if (!term) continue;
            for (unsigned i = 0; i < term->getNumSuccessors(); ++i)
                work.push_back(term->getSuccessor(i));
        }
        return false;
    }

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
            auto* empty = builder->CreateGlobalString("", "empty_ret_str");
            builder->CreateRet(builder->CreatePointerCast(empty, ty));
        } else {
            builder->CreateRetVoid();
        }
    }

    void storeDefaultValue(llvm::AllocaInst* alloca, llvm::Type* ty) {
        if (ty->isIntegerTy()) {
            builder->CreateStore(llvm::ConstantInt::get(ty, 0), alloca);
        } else if (ty->isFloatingPointTy()) {
            builder->CreateStore(llvm::ConstantFP::get(ty, 0.0), alloca);
        } else if (ty->isPointerTy()) {
            auto* empty = builder->CreateGlobalString("", "empty_str");
            builder->CreateStore(
                builder->CreatePointerCast(empty, getInt8PtrTy()), alloca);
        } else {
            error("cannot initialize a variable of this type");
        }
    }

    llvm::Type* commonNumericType(llvm::Value* a, llvm::Value* b) {
        if (a->getType()->isFloatingPointTy() || b->getType()->isFloatingPointTy())
            return llvm::Type::getDoubleTy(context);
        if (a->getType()->isIntegerTy(64) || b->getType()->isIntegerTy(64))
            return llvm::Type::getInt64Ty(context);
        return llvm::Type::getInt32Ty(context);
    }

    llvm::Value* toBool(llvm::Value* v) {
        if (!v) return nullptr;
        llvm::Type* ty = v->getType();
        if (ty->isIntegerTy())
            return builder->CreateICmpNE(
                v, llvm::ConstantInt::get(ty, 0), "cond");
        if (ty->isFloatingPointTy())
            return builder->CreateFCmpONE(
                v, llvm::ConstantFP::get(ty, 0.0), "fcond");
        if (ty->isPointerTy())
            return builder->CreateICmpNE(
                v, llvm::ConstantPointerNull::get(
                       llvm::cast<llvm::PointerType>(ty)), "pcond");
        error("condition must be a number or a string");
        return nullptr;
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

        std::vector<llvm::Type*> reallocArgs = {
            getInt8PtrTy(), llvm::Type::getInt64Ty(context)
        };
        llvm::FunctionType* reallocType = llvm::FunctionType::get(
            getInt8PtrTy(), reallocArgs, false);
        module->getOrInsertFunction("realloc", reallocType);

        std::vector<llvm::Type*> snprintfArgs = {
            getInt8PtrTy(), llvm::Type::getInt64Ty(context), getInt8PtrTy()
        };
        llvm::FunctionType* snprintfType = llvm::FunctionType::get(
            llvm::Type::getInt32Ty(context), snprintfArgs, true);
        module->getOrInsertFunction("snprintf", snprintfType);

        std::vector<llvm::Type*> getcharArgs = {};
        llvm::FunctionType* getcharType = llvm::FunctionType::get(
            llvm::Type::getInt32Ty(context), getcharArgs, false);
        module->getOrInsertFunction("getchar", getcharType);

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
                    return builder->CreateSExt(val, targetTy);
                else
                    return builder->CreateTrunc(val, targetTy);
            } else if (val->getType()->isFloatingPointTy()) {
                return builder->CreateFPToSI(val, targetTy);
            } else if (val->getType()->isPointerTy()) {
                error("cannot convert a string to a number");
                return val;
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
                error("cannot convert a number to a string");
                return val;
            }
        }
        return val;
    }

    llvm::Constant* convertConstant(llvm::Constant* c, llvm::Type* ty,
                                    const std::string& name) {
        llvm::Type* from = c->getType();
        if (from == ty) return c;
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(c)) {
            if (ty->isIntegerTy())
                return llvm::ConstantInt::get(ty, ci->getSExtValue(), true);
            if (ty->isFloatingPointTy())
                return llvm::ConstantFP::get(ty, (double)ci->getSExtValue());
        }
        if (auto* cf = llvm::dyn_cast<llvm::ConstantFP>(c)) {
            if (ty->isIntegerTy())
                return llvm::ConstantInt::get(
                    ty, (int64_t)cf->getValueAPF().convertToDouble(), true);
            if (ty->isFloatingPointTy())
                return llvm::ConstantFP::get(
                    ty, cf->getValueAPF().convertToDouble());
        }
        error("constant '" + name + "' has an incompatible declared type");
        return c;
    }

    llvm::Value* compileStr(llvm::Value* value) {
        if (!value) return nullptr;
        if (value->getType()->isPointerTy()) return value;

        llvm::Type* bufType = llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 64);
        auto bufAlloca = builder->CreateAlloca(bufType, nullptr, "str_buf");
        auto bufPtr = builder->CreatePointerCast(bufAlloca, getInt8PtrTy());

        std::string formatStr;
        if (value->getType()->isIntegerTy()) {
            formatStr = "%lld";
            if (value->getType()->getIntegerBitWidth() < 64)
                value = builder->CreateSExt(value, llvm::Type::getInt64Ty(context));
        } else if (value->getType()->isFloatingPointTy()) {
            formatStr = "%g";
            if (value->getType()->isFloatTy())
                value = builder->CreateFPExt(value, llvm::Type::getDoubleTy(context));
        } else {
            error("str() cannot convert this value");
            return nullptr;
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
        if (call->args.size() != 1) {
            error("int() expects one argument");
            return nullptr;
        }
        auto arg = compileExpr(call->args[0].get());
        if (!arg) return nullptr;
        if (arg->getType()->isPointerTy()) {
            auto atoiFn = module->getFunction("atoi");
            return builder->CreateCall(atoiFn, {arg});
        } else if (arg->getType()->isIntegerTy() || arg->getType()->isFloatingPointTy()) {
            return convertValue(arg, llvm::Type::getInt32Ty(context));
        }
        error("int() cannot convert this value");
        return nullptr;
    }

    llvm::Value* compileFloat(const Call* call) {
        if (call->args.size() != 1) {
            error("float() expects one argument");
            return nullptr;
        }
        auto arg = compileExpr(call->args[0].get());
        if (!arg) return nullptr;
        if (arg->getType()->isPointerTy()) {
            auto atofFn = module->getFunction("atof");
            return builder->CreateCall(atofFn, {arg});
        } else if (arg->getType()->isIntegerTy() || arg->getType()->isFloatingPointTy()) {
            return convertValue(arg, llvm::Type::getDoubleTy(context));
        }
        error("float() cannot convert this value");
        return nullptr;
    }

    llvm::Value* compileInput(const Call* call) {
        if (call->args.size() > 1) {
            error("input() takes at most one prompt argument");
        }
        if (!call->args.empty()) {
            auto prompt = compileExpr(call->args[0].get());
            if (prompt) {
                if (!prompt->getType()->isPointerTy()) {
                    error("input() prompt must be a string");
                } else {
                    auto promptFmt = builder->CreateGlobalString("%s", "prompt_fmt");
                    builder->CreateCall(module->getFunction("printf"),
                                        {promptFmt, prompt});
                }
            }
        }
        // 逐字符读取整行，缓冲区按需增长，不再有固定长度上限
        auto* i64Ty = llvm::Type::getInt64Ty(context);
        auto* i32Ty = llvm::Type::getInt32Ty(context);
        auto* initCap = llvm::ConstantInt::get(i64Ty, 1024);
        auto* mallocFn = module->getFunction("malloc");
        auto* reallocFn = module->getFunction("realloc");
        auto* getcharFn = module->getFunction("getchar");
        auto* buf0 = builder->CreateCall(mallocFn, {initCap}, "input_buf");

        llvm::BasicBlock* preBB   = builder->GetInsertBlock();
        llvm::BasicBlock* headBB  = llvm::BasicBlock::Create(
            context, "input_read", mainFunc);
        llvm::BasicBlock* checkBB = llvm::BasicBlock::Create(
            context, "input_check", mainFunc);
        llvm::BasicBlock* growBB  = llvm::BasicBlock::Create(
            context, "input_grow", mainFunc);
        llvm::BasicBlock* storeBB = llvm::BasicBlock::Create(
            context, "input_store", mainFunc);
        llvm::BasicBlock* doneBB  = llvm::BasicBlock::Create(
            context, "input_done", mainFunc);
        builder->CreateBr(headBB);

        builder->SetInsertPoint(headBB);
        auto* idxPhi = builder->CreatePHI(i64Ty, 2, "input_idx");
        auto* bufPhi = builder->CreatePHI(getInt8PtrTy(), 2, "input_ptr");
        auto* capPhi = builder->CreatePHI(i64Ty, 2, "input_cap");
        idxPhi->addIncoming(llvm::ConstantInt::get(i64Ty, 0), preBB);
        bufPhi->addIncoming(buf0, preBB);
        capPhi->addIncoming(initCap, preBB);

        auto* ch = builder->CreateCall(getcharFn, {}, "input_char");
        auto* isNewline = builder->CreateICmpEQ(
            ch, llvm::ConstantInt::get(i32Ty, 10), "is_nl");
        auto* isEof = builder->CreateICmpEQ(
            ch, llvm::ConstantInt::get(i32Ty, -1), "is_eof");
        auto* stop = builder->CreateOr(isNewline, isEof, "stop_read");
        builder->CreateCondBr(stop, doneBB, checkBB);

        // 留一个字节给结尾的 '\0'，容量不足则扩容
        builder->SetInsertPoint(checkBB);
        auto* capMinusOne = builder->CreateSub(
            capPhi, llvm::ConstantInt::get(i64Ty, 1), "cap_last");
        auto* needGrow = builder->CreateICmpUGE(idxPhi, capMinusOne, "need_grow");
        builder->CreateCondBr(needGrow, growBB, storeBB);

        builder->SetInsertPoint(growBB);
        auto* newCap = builder->CreateMul(
            capPhi, llvm::ConstantInt::get(i64Ty, 2), "new_cap");
        auto* newBuf = builder->CreateCall(reallocFn, {bufPhi, newCap}, "input_realloc");
        builder->CreateBr(storeBB);

        builder->SetInsertPoint(storeBB);
        auto* storeBufPhi = builder->CreatePHI(getInt8PtrTy(), 2, "store_buf");
        auto* storeCapPhi = builder->CreatePHI(i64Ty, 2, "store_cap");
        storeBufPhi->addIncoming(bufPhi, checkBB);
        storeBufPhi->addIncoming(newBuf, growBB);
        storeCapPhi->addIncoming(capPhi, checkBB);
        storeCapPhi->addIncoming(newCap, growBB);

        auto* slot = builder->CreateInBoundsGEP(
            llvm::Type::getInt8Ty(context), storeBufPhi, idxPhi, "input_slot");
        builder->CreateStore(builder->CreateTrunc(ch, llvm::Type::getInt8Ty(context)), slot);
        auto* nextIdx = builder->CreateAdd(
            idxPhi, llvm::ConstantInt::get(i64Ty, 1), "idx_next");
        builder->CreateBr(headBB);

        idxPhi->addIncoming(nextIdx, storeBB);
        bufPhi->addIncoming(storeBufPhi, storeBB);
        capPhi->addIncoming(storeCapPhi, storeBB);

        builder->SetInsertPoint(doneBB);
        auto* endSlot = builder->CreateInBoundsGEP(
            llvm::Type::getInt8Ty(context), bufPhi, idxPhi, "input_end");
        builder->CreateStore(
            llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 0), endSlot);
        return bufPhi;
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
                    if (val > INT32_MAX || val < INT32_MIN) {
                        return llvm::ConstantInt::get(
                            llvm::Type::getInt64Ty(context), val, true);
                    }
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
                auto* c = lookupConst(var->name);
                if (c) return c;
                auto* alloca = lookupVar(var->name);
                if (alloca) {
                    auto* ty = alloca->getAllocatedType();
                    return builder->CreateLoad(ty, alloca, var->name);
                }
                error("unknown variable '" + var->name + "'");
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
                        error("unary '-' cannot be applied to this type");
                        return nullptr;
                    }
                }
                error("unknown unary operator '" + u->op + "'");
                return nullptr;
            }
            case NodeType::BinaryOp: {
                auto* bin = static_cast<const BinaryOp*>(node);
                std::string op = bin->op;

                // 逻辑运算按短路语义求值：右侧只在必要时执行
                if (op == "&&" || op == "||") {
                    auto left = compileExpr(bin->left.get());
                    if (!left) return nullptr;
                    auto* lb = toBool(left);
                    if (!lb) return nullptr;
                    llvm::BasicBlock* leftEndBB = builder->GetInsertBlock();

                    llvm::BasicBlock* rhsBB = llvm::BasicBlock::Create(
                        context, "logic_rhs", mainFunc);
                    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(
                        context, "logic_end", mainFunc);
                    if (op == "&&")
                        builder->CreateCondBr(lb, rhsBB, endBB);
                    else
                        builder->CreateCondBr(lb, endBB, rhsBB);

                    builder->SetInsertPoint(rhsBB);
                    auto right = compileExpr(bin->right.get());
                    if (!right) return nullptr;
                    auto* rb = toBool(right);
                    if (!rb) return nullptr;
                    auto* rhsVal = builder->CreateZExt(
                        rb, llvm::Type::getInt32Ty(context), "logic_zext");
                    auto* rhsEndBB = builder->GetInsertBlock();
                    builder->CreateBr(endBB);

                    builder->SetInsertPoint(endBB);
                    auto* phi = builder->CreatePHI(
                        llvm::Type::getInt32Ty(context), 2, "logic_phi");
                    phi->addIncoming(
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(context),
                                               op == "&&" ? 0 : 1),
                        leftEndBB);
                    phi->addIncoming(rhsVal, rhsEndBB);
                    return phi;
                }

                auto left = compileExpr(bin->left.get());
                auto right = compileExpr(bin->right.get());
                if (!left || !right) return nullptr;

                bool leftIsPtr  = left->getType()->isPointerTy();
                bool rightIsPtr = right->getType()->isPointerTy();

                if (op == ">" || op == "<" || op == "==" || op == "!=" ||
                    op == ">=" || op == "<=") {
                    if (leftIsPtr && rightIsPtr) {
                        if (op == "==" || op == "!=") {
                            auto* eq = callStrcmp(left, right);
                            if (op == "!=") {
                                eq = builder->CreateXor(eq,
                                    llvm::ConstantInt::get(
                                        llvm::Type::getInt32Ty(context), 1),
                                    "cmpnot");
                            }
                            return eq;
                        }
                        error("string comparison with '" + op +
                              "' is not allowed; use only == or !=");
                        return nullptr;
                    }
                    if (leftIsPtr || rightIsPtr) {
                        error("cannot compare a string with a non-string value");
                        return nullptr;
                    }

                    llvm::Type* commonTy = commonNumericType(left, right);
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

                if (leftIsPtr || rightIsPtr) {
                    if (op == "+" && leftIsPtr && rightIsPtr)
                        return concatStrings(left, right);
                    error("operator '" + op + "' cannot be applied to a string");
                    return nullptr;
                }

                llvm::Type* commonTy = commonNumericType(left, right);
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
                    if (call->args.size() != 1) {
                        error("str() expects one argument");
                        return nullptr;
                    }
                    auto arg = compileExpr(call->args[0].get());
                    if (!arg) return nullptr;
                    return compileStr(arg);
                }
                if (call->name == "int")   return compileInt(call);
                if (call->name == "float") return compileFloat(call);

                auto* fn = module->getFunction(call->name);
                if (!fn) {
                    error("unknown function '" + call->name + "'");
                    return nullptr;
                }
                if (call->args.size() != fn->arg_size()) {
                    error("function '" + call->name + "' expects " +
                          std::to_string(fn->arg_size()) + " argument(s), got " +
                          std::to_string(call->args.size()));
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
                if (inMain) {
                    error("'return' outside of a function");
                    break;
                }

                if (currentReturnType == "void") {
                    if (rs->value) {
                        error("'return' with a value inside a void function");
                    }
                    builder->CreateRetVoid();
                } else {
                    llvm::Type* retTy = mapType(currentReturnType);
                    if (!rs->value) {
                        error("'return' without a value inside a non-void function");
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
                llvm::Value* initVal = nullptr;
                bool initFailed = false;

                if (!vd->type.empty()) ty = mapType(vd->type);

                if (vd->init) {
                    initVal = compileExpr(vd->init.get());
                    if (!initVal) {
                        initFailed = true;
                    } else if (initVal->getType()->isVoidTy()) {
                        error("variable '" + vd->name +
                              "' cannot be initialized with a void value");
                        initVal = nullptr;
                        initFailed = true;
                    } else if (vd->type.empty()) {
                        ty = initVal->getType();
                    }
                }

                if (!ty) ty = llvm::Type::getInt32Ty(context);
                if (ty->isVoidTy()) {
                    error("variable '" + vd->name + "' cannot be of type void");
                    ty = llvm::Type::getInt32Ty(context);
                }

                auto* alloca = builder->CreateAlloca(ty, nullptr, vd->name);
                declareVar(vd->name, alloca);

                if (initVal) {
                    if (initVal->getType() != ty) initVal = convertValue(initVal, ty);
                    builder->CreateStore(initVal, alloca);
                } else if (!initFailed) {
                    storeDefaultValue(alloca, ty);
                }
                break;
            }

            case NodeType::ConstDecl: {
                auto* cd = static_cast<const ConstDecl*>(stmt);
                auto val = compileExpr(cd->init.get());
                if (!val) break;
                if (!llvm::isa<llvm::Constant>(val)) {
                    error("initializer for constant '" + cd->name +
                          "' is not a constant expression");
                    break;
                }
                auto* c = llvm::cast<llvm::Constant>(val);
                if (!cd->type.empty()) {
                    llvm::Type* ty = mapType(cd->type);
                    if (ty->isVoidTy()) {
                        error("constant '" + cd->name + "' cannot be of type void");
                        break;
                    }
                    c = convertConstant(c, ty, cd->name);
                }
                declareConst(cd->name, c);
                break;
            }

            case NodeType::Assign: {
                auto* as = static_cast<const Assign*>(stmt);
                if (lookupConst(as->name)) {
                    error("cannot assign to constant '" + as->name + "'");
                    break;
                }
                auto* alloca = lookupVar(as->name);
                if (!alloca) {
                    error("unknown variable '" + as->name + "'");
                    break;
                }
                auto val = compileExpr(as->value.get());
                if (val) {
                    auto* varTy = alloca->getAllocatedType();
                    if (val->getType()->isVoidTy()) {
                        error("cannot assign a void value to variable '" +
                              as->name + "'");
                        break;
                    }
                    if (val->getType() != varTy) val = convertValue(val, varTy);
                    builder->CreateStore(val, alloca);
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
                    if (val->getType()->isVoidTy()) {
                        error("print() cannot print a void value");
                        continue;
                    } else if (val->getType()->isIntegerTy()) {
                        if (val->getType()->getIntegerBitWidth() == 64)
                            format = "%lld";
                        else
                            format = "%d";
                    } else if (val->getType()->isFloatingPointTy()) {
                        if (val->getType()->isFloatTy())
                            val = builder->CreateFPExt(
                                val, llvm::Type::getDoubleTy(context));
                        format = "%f";
                    } else if (val->getType()->isPointerTy()) {
                        format = "%s";
                    } else {
                        format = "%p";
                    }

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
                llvm::Value* condBool = toBool(condVal);
                if (!condBool) break;

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
                pushScope();  // then 块作用域
                if (auto* tn = static_cast<Block*>(ifs->thenBlock.get()))
                    for (auto& s : tn->statements) compileStmt(s.get());
                popScope();
                builder->CreateBr(endBB);

                if (elseBB) {
                    builder->SetInsertPoint(elseBB);
                    pushScope();  // else 块作用域
                    if (auto* en = static_cast<Block*>(ifs->elseBlock.get()))
                        for (auto& s : en->statements) compileStmt(s.get());
                    popScope();
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
                llvm::Value* condBool = toBool(condVal);
                if (!condBool) {
                    loopStack.pop_back();
                    break;
                }
                builder->CreateCondBr(condBool, bodyBB, endBB);

                builder->SetInsertPoint(bodyBB);
                pushScope();  // 循环体作用域
                if (auto* bn = static_cast<Block*>(ws->body.get()))
                    for (auto& s : bn->statements) compileStmt(s.get());
                popScope();
                builder->CreateBr(condBB);

                builder->SetInsertPoint(endBB);
                loopStack.pop_back();
                break;
            }

            case NodeType::BreakStmt: {
                if (loopStack.empty()) {
                    error("'break' outside of loop");
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
                    error("'continue' outside of loop");
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