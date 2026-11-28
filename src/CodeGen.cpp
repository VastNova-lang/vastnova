#include "CodeGen.h"
#include "vastnova_types.h"
#include <llvm/ADT/APFloat.h>
#include <llvm/Config/llvm-config.h>
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
    std::vector<std::map<std::string, llvm::AllocaInst*>> varScopes;
    std::vector<std::map<std::string, TypeInfo>> varTypeScopes;
    std::vector<std::map<std::string, llvm::Constant*>> constScopes;
    std::vector<std::map<std::string, TypeInfo>> constTypeScopes;
    std::vector<std::pair<llvm::BasicBlock*, llvm::BasicBlock*>> loopStack;
    std::map<std::string, TypeInfo> functionReturnTypes;
    std::map<std::string, std::vector<TypeInfo>> functionParamTypes;
    std::string currentReturnType;
    TypeInfo currentReturnInfo;
    TypeInfo lastExprType;
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
            std::vector<TypeInfo> paramInfos;
            bool badParam = false;
            for (auto& p : f->params) {
                TypeInfo pt;
                std::string parseError;
                if (!parseTypeInfo(p.type, pt, parseError)) {
                    error(parseError);
                    badParam = true;
                    break;
                }
                if (pt.kind == TypeInfo::Void) {
                    error("parameter '" + p.name + "' of function '" + f->name +
                          "' cannot be void");
                    badParam = true;
                    break;
                }
                paramInfos.push_back(pt);
                paramTys.push_back(llvmTypeFor(pt));
            }
            if (badParam) continue;
            TypeInfo retInfo;
            std::string retError;
            if (!parseTypeInfo(f->returnType, retInfo, retError)) {
                error(retError);
                continue;
            }
            llvm::Type* retTy = llvmTypeFor(retInfo);
            auto* fnTy = llvm::FunctionType::get(retTy, paramTys, false);
            if (!module->getFunction(f->name)) {
                llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                       f->name, module.get());
            }
            functionReturnTypes[f->name] = retInfo;
            functionParamTypes[f->name] = paramInfos;
            validFuncs.push_back(f);
        }

        entryBB = llvm::BasicBlock::Create(context, "entry", mainFunc);
        builder->SetInsertPoint(entryBB);

        declarePrintf();
        declareScanf();
        declareStringFunctions();

        currentReturnType = "void";
        currentReturnInfo = TypeInfo::voidType();
        inMain = true;
        pushScope();
        for (auto* s : mainStmts) compileStmt(s);
        builder->CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));
        popScope();

        inMain = false;
        for (auto* f : validFuncs) {
            auto* fn = module->getFunction(f->name);
            auto* bb = llvm::BasicBlock::Create(context, "entry", fn);

            auto savedLoopStack = std::move(loopStack);
            auto* savedMainFunc = mainFunc;
            auto  savedRetType  = currentReturnType;
            auto  savedRetInfo  = currentReturnInfo;

            loopStack.clear();
            mainFunc = fn;
            currentReturnType = f->returnType;
            currentReturnInfo = functionReturnTypes[f->name];

            builder->SetInsertPoint(bb);
            pushScope();

            auto argIt = fn->arg_begin();
            size_t paramIndex = 0;
            for (auto& p : f->params) {
                const TypeInfo& pt = functionParamTypes[f->name][paramIndex++];
                llvm::Type* pty = llvmTypeFor(pt);
                auto* alloca = builder->CreateAlloca(pty, nullptr, p.name);
                builder->CreateStore(&*argIt, alloca);
                declareVar(p.name, alloca, pt);
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

            popScope();
            loopStack = std::move(savedLoopStack);
            mainFunc  = savedMainFunc;
            currentReturnType = savedRetType;
            currentReturnInfo = savedRetInfo;
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

    void pushVarScope() {
        varScopes.emplace_back();
        varTypeScopes.emplace_back();
    }
    void popVarScope() {
        if (!varScopes.empty()) varScopes.pop_back();
        if (!varTypeScopes.empty()) varTypeScopes.pop_back();
    }
    void pushConstScope() {
        constScopes.emplace_back();
        constTypeScopes.emplace_back();
    }
    void popConstScope() {
        if (!constScopes.empty()) constScopes.pop_back();
        if (!constTypeScopes.empty()) constTypeScopes.pop_back();
    }
    void pushScope()      { pushVarScope(); pushConstScope(); }
    void popScope()       { popVarScope(); popConstScope(); }

    llvm::AllocaInst* lookupVar(const std::string& name) {
        for (auto it = varScopes.rbegin(); it != varScopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return f->second;
        }
        return nullptr;
    }

    TypeInfo lookupVarType(const std::string& name) {
        for (auto it = varTypeScopes.rbegin(); it != varTypeScopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return f->second;
        }
        return TypeInfo{};
    }

    llvm::Constant* lookupConst(const std::string& name) {
        for (auto it = constScopes.rbegin(); it != constScopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return f->second;
        }
        return nullptr;
    }

    TypeInfo lookupConstType(const std::string& name) {
        for (auto it = constTypeScopes.rbegin(); it != constTypeScopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return f->second;
        }
        return TypeInfo{};
    }

    bool declareVar(const std::string& name, llvm::AllocaInst* alloca,
                    const TypeInfo& type) {
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
        varTypeScopes.back()[name] = type;
        return true;
    }

    bool declareConst(const std::string& name, llvm::Constant* c,
                      const TypeInfo& type) {
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
        constTypeScopes.back()[name] = type;
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

    llvm::Type* llvmTypeFor(const TypeInfo& type) {
        switch (type.kind) {
            case TypeInfo::Int:
                return llvm::Type::getIntNTy(context, type.bits);
            case TypeInfo::Float:
                if (type.bits == 16) return llvm::Type::getHalfTy(context);
                if (type.bits == 32) return llvm::Type::getFloatTy(context);
                if (type.bits == 128) return llvm::Type::getFP128Ty(context);
                return llvm::Type::getDoubleTy(context);
            case TypeInfo::Str:
                return getInt8PtrTy();
            case TypeInfo::Void:
                return llvm::Type::getVoidTy(context);
            case TypeInfo::Array:
                if (!type.elem) return llvm::Type::getInt32Ty(context);
                return llvm::ArrayType::get(llvmTypeFor(*type.elem), type.count);
            default:
                return llvm::Type::getInt32Ty(context);
        }
    }

    llvm::Type* mapType(const TypeInfo& type) {
        return llvmTypeFor(type);
    }

    llvm::Type* mapType(const std::string& t) {
        TypeInfo type;
        std::string parseError;
        if (!parseTypeInfo(t, type, parseError)) {
            error(parseError);
            return llvm::Type::getInt32Ty(context);
        }
        return llvmTypeFor(type);
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
        } else if (ty->isArrayTy()) {
            builder->CreateRet(llvm::ConstantAggregateZero::get(ty));
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
        } else if (ty->isArrayTy()) {
            builder->CreateStore(llvm::ConstantAggregateZero::get(ty), alloca);
        } else {
            error("cannot initialize a variable of this type");
        }
    }

    static std::string apIntToString(const llvm::APInt& value, bool isSigned) {
#if LLVM_VERSION_MAJOR >= 16
        llvm::SmallVector<char, 64> buffer;
        value.toString(buffer, 10, isSigned);
        return std::string(buffer.begin(), buffer.end());
#else
        return value.toString(10, isSigned);
#endif
    }

    static double apIntToDouble(const llvm::APInt& value, bool isSigned) {
        llvm::APFloat converted(llvm::APFloat::IEEEdouble());
        converted.convertFromAPInt(value, isSigned,
                                   llvm::APFloat::rmNearestTiesToEven);
        return converted.convertToDouble();
    }

    llvm::ConstantInt* convertConstantInt(const llvm::ConstantInt* constant,
                                          const TypeInfo& from,
                                          const TypeInfo& to, bool& ok) {
        ok = false;
        if (to.kind != TypeInfo::Int) return nullptr;
        llvm::APInt value = constant->getValue();
        unsigned width = value.getBitWidth();
        if (width < to.bits) width = to.bits;
        ++width;
        llvm::APInt extended = from.isUnsigned ? value.zext(width)
                                               : value.sext(width);
        llvm::APInt low;
        llvm::APInt high;
        if (to.isUnsigned) {
            low = llvm::APInt(width, 0);
            high = llvm::APInt::getMaxValue(to.bits).zext(width);
        } else {
            low = llvm::APInt::getSignedMinValue(to.bits).sext(width);
            high = llvm::APInt::getSignedMaxValue(to.bits).sext(width);
        }
        bool fits = extended.sge(low) && extended.sle(high);
        if (!fits) return nullptr;
        ok = true;
        return llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(
            llvm::Type::getIntNTy(context, to.bits),
            extended.trunc(to.bits)));
    }

    llvm::Value* convertTyped(llvm::Value* value, const TypeInfo& from,
                              const TypeInfo& to, bool explicitConversion,
                              const std::string& where) {
        if (!value) return nullptr;
        if (from == to) return value;
        std::string prefix = where.empty() ? std::string() : where + ": ";

        if (from.kind == TypeInfo::Invalid || to.kind == TypeInfo::Invalid)
            return value;

        if (from.kind == TypeInfo::Array || to.kind == TypeInfo::Array) {
            error(prefix + "array values cannot be converted");
            return value;
        }
        if (from.kind == TypeInfo::Void || to.kind == TypeInfo::Void) {
            error(prefix + "cannot convert '" + from.name() + "' to '" +
                  to.name() + "'");
            return value;
        }
        if (from.kind == TypeInfo::Str || to.kind == TypeInfo::Str) {
            if (from.kind == TypeInfo::Str && to.kind == TypeInfo::Str)
                return value;
            error(prefix + "cannot convert '" + from.name() + "' to '" +
                  to.name() + "'");
            return value;
        }

        if (from.kind == TypeInfo::Int && to.kind == TypeInfo::Int) {
            if (!explicitConversion) {
                if (auto* constant = llvm::dyn_cast<llvm::ConstantInt>(value)) {
                    bool ok = false;
                    auto* converted = convertConstantInt(constant, from, to, ok);
                    if (ok) return converted;
                    error(prefix + "constant " +
                          apIntToString(constant->getValue(), !from.isUnsigned) +
                          " does not fit in type '" + to.name() + "'");
                    return value;
                }
                if (from.isUnsigned != to.isUnsigned) {
                    error(prefix + "cannot implicitly convert '" + from.name() +
                          "' to '" + to.name() +
                          "' because it changes signedness; use int() or uint()");
                    return value;
                }
                if (to.bits < from.bits) {
                    error(prefix + "narrowing from '" + from.name() + "' to '" +
                          to.name() + "' requires an explicit conversion");
                    return value;
                }
            }
            llvm::Type* targetTy = llvm::Type::getIntNTy(context, to.bits);
            if (to.bits > from.bits)
                return from.isUnsigned ? builder->CreateZExt(value, targetTy)
                                       : builder->CreateSExt(value, targetTy);
            if (to.bits < from.bits)
                return builder->CreateTrunc(value, targetTy);
            return value;
        }

        if (from.kind == TypeInfo::Int && to.kind == TypeInfo::Float) {
            llvm::Type* targetTy = llvmTypeFor(to);
            return from.isUnsigned ? builder->CreateUIToFP(value, targetTy)
                                   : builder->CreateSIToFP(value, targetTy);
        }

        if (from.kind == TypeInfo::Float && to.kind == TypeInfo::Int) {
            if (!explicitConversion) {
                error(prefix + "converting '" + from.name() + "' to '" +
                      to.name() + "' requires int() or uint()");
                return value;
            }
            llvm::Type* targetTy = llvmTypeFor(to);
            return to.isUnsigned ? builder->CreateFPToUI(value, targetTy)
                                 : builder->CreateFPToSI(value, targetTy);
        }

        if (from.kind == TypeInfo::Float && to.kind == TypeInfo::Float) {
            llvm::Type* targetTy = llvmTypeFor(to);
            if (to.bits > from.bits)
                return builder->CreateFPExt(value, targetTy);
            if (to.bits < from.bits)
                return builder->CreateFPTrunc(value, targetTy);
            return value;
        }

        error(prefix + "cannot convert '" + from.name() + "' to '" +
              to.name() + "'");
        return value;
    }

    llvm::Constant* convertConstantTyped(llvm::Constant* constant,
                                         const TypeInfo& from,
                                         const TypeInfo& to,
                                         const std::string& name) {
        if (from == to) return constant;
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(constant)) {
            if (to.kind == TypeInfo::Int) {
                bool ok = false;
                auto* converted = convertConstantInt(ci, from, to, ok);
                if (ok) return converted;
                error("constant '" + name + "' value " +
                      apIntToString(ci->getValue(), !from.isUnsigned) +
                      " does not fit in type '" + to.name() + "'");
                return constant;
            }
            if (to.kind == TypeInfo::Float) {
                double number =
                    apIntToDouble(ci->getValue(), !from.isUnsigned);
                return llvm::ConstantFP::get(llvmTypeFor(to), number);
            }
        }
        if (auto* cf = llvm::dyn_cast<llvm::ConstantFP>(constant)) {
            if (to.kind == TypeInfo::Float)
                return llvm::ConstantFP::get(llvmTypeFor(to),
                    cf->getValueAPF().convertToDouble());
            if (to.kind == TypeInfo::Int) {
                error("constant '" + name + "' of type '" + from.name() +
                      "' requires an explicit conversion to '" + to.name() +
                      "' with int() or uint()");
                return constant;
            }
        }
        error("constant '" + name + "' has an incompatible declared type");
        return constant;
    }

    bool mergeNumericTypes(TypeInfo& leftType, TypeInfo& rightType,
                           llvm::Value*& leftValue, llvm::Value*& rightValue,
                           TypeInfo& result, std::string& error) {
        if (leftType.kind == TypeInfo::Str || rightType.kind == TypeInfo::Str) {
            if (leftType.kind == TypeInfo::Str &&
                rightType.kind == TypeInfo::Str) {
                result = TypeInfo::strType();
                return true;
            }
            error = "cannot mix a string with a number";
            return false;
        }
        if (leftType.kind == TypeInfo::Float ||
            rightType.kind == TypeInfo::Float) {
            if (leftType.kind == TypeInfo::Float &&
                rightType.kind == TypeInfo::Float) {
                result = TypeInfo::floatType(
                    leftType.bits > rightType.bits ? leftType.bits
                                                   : rightType.bits);
                return true;
            }
            if (leftType.kind == TypeInfo::Int && rightType.kind == TypeInfo::Float) {
                result = TypeInfo::floatType(64);
                return true;
            }
            if (leftType.kind == TypeInfo::Float && rightType.kind == TypeInfo::Int) {
                result = TypeInfo::floatType(64);
                return true;
            }
            error = "cannot mix a number with a string";
            return false;
        }

        if (leftType.kind != TypeInfo::Int || rightType.kind != TypeInfo::Int) {
            error = "operator requires numeric operands";
            return false;
        }

        auto adaptConstant = [&](TypeInfo& from, TypeInfo& to,
                                 llvm::Value*& value) -> bool {
            auto* constant = llvm::dyn_cast<llvm::ConstantInt>(value);
            if (!constant || to.kind != TypeInfo::Int) return false;
            bool ok = false;
            auto* converted = convertConstantInt(constant, from, to, ok);
            if (!ok) return false;
            value = converted;
            from = to;
            return true;
        };

        bool leftConstant = llvm::isa<llvm::ConstantInt>(leftValue);
        bool rightConstant = llvm::isa<llvm::ConstantInt>(rightValue);
        if (leftConstant && !rightConstant) {
            adaptConstant(leftType, rightType, leftValue);
        } else if (rightConstant && !leftConstant) {
            adaptConstant(rightType, leftType, rightValue);
        } else if (leftConstant && rightConstant && leftType != rightType) {
            if (!adaptConstant(leftType, rightType, leftValue))
                adaptConstant(rightType, leftType, rightValue);
        }

        if (leftType.isUnsigned != rightType.isUnsigned) {
            error = "cannot mix signed '" + leftType.name() +
                    "' with unsigned '" + rightType.name() +
                    "'; convert one operand explicitly with int() or uint()";
            return false;
        }

        result = TypeInfo::intType(
            leftType.bits > rightType.bits ? leftType.bits : rightType.bits,
            leftType.isUnsigned);
        return true;
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
                builder->CreateCall(module->getFunction("strlen"), {v}, "strlen"),
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0), "scond");
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

    llvm::Function* integerToStringFunction(const TypeInfo& type) {
        std::string name = std::string("__vn_itoa_") +
                           (type.isUnsigned ? "u" : "i") +
                           std::to_string(type.bits);
        if (auto* existing = module->getFunction(name)) return existing;

        auto savedIP = builder->saveIP();
        auto* i8Ty = llvm::Type::getInt8Ty(context);
        auto* i32Ty = llvm::Type::getInt32Ty(context);
        auto* i64Ty = llvm::Type::getInt64Ty(context);
        unsigned workBits = type.bits < 8 ? 8 : type.bits;
        if (!type.isUnsigned) ++workBits;
        auto* workTy = llvm::Type::getIntNTy(context, workBits);
        auto* valueTy = llvm::Type::getIntNTy(context, type.bits);
        auto* fn = llvm::Function::Create(
            llvm::FunctionType::get(getInt8PtrTy(), {valueTy}, false),
            llvm::Function::PrivateLinkage, name, module.get());

        auto* entry = llvm::BasicBlock::Create(context, "entry", fn);
        auto* loopBB = llvm::BasicBlock::Create(context, "digits", fn);
        auto* doneBB = llvm::BasicBlock::Create(context, "done", fn);
        auto* copyHead = llvm::BasicBlock::Create(context, "copy_head", fn);
        auto* copyBody = llvm::BasicBlock::Create(context, "copy_body", fn);
        auto* finishBB = llvm::BasicBlock::Create(context, "finish", fn);
        builder->SetInsertPoint(entry);

        llvm::Argument* argument = fn->arg_begin();
        llvm::Value* negative = nullptr;
        llvm::Value* magnitude = nullptr;
        if (type.isUnsigned) {
            negative = llvm::ConstantInt::getFalse(context);
            magnitude = builder->CreateZExt(argument, workTy, "mag");
        } else {
            auto* extended = builder->CreateSExt(argument, workTy, "signed");
            negative = builder->CreateICmpSLT(
                extended, llvm::ConstantInt::get(workTy, 0), "neg");
            auto* negated = builder->CreateNeg(extended, "neg_mag");
            magnitude = builder->CreateSelect(
                negative, negated, extended, "mag");
        }

        unsigned maxDigits = type.bits / 3 + 3;
        auto* bufferTy = llvm::ArrayType::get(i8Ty, maxDigits);
        auto* digits = builder->CreateAlloca(bufferTy, nullptr, "digits");
        auto* magSlot = builder->CreateAlloca(workTy, nullptr, "mag_slot");
        auto* idxSlot = builder->CreateAlloca(i64Ty, nullptr, "idx_slot");
        builder->CreateStore(magnitude, magSlot);
        builder->CreateStore(llvm::ConstantInt::get(i64Ty, 0), idxSlot);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(loopBB);
        auto* mag = builder->CreateLoad(workTy, magSlot, "mag_val");
        auto* idx = builder->CreateLoad(i64Ty, idxSlot, "idx_val");
        auto* ten = llvm::ConstantInt::get(workTy, 10);
        auto* digit = builder->CreateURem(mag, ten, "digit");
        auto* digit8 = builder->CreateTrunc(digit, i8Ty, "digit8");
        auto* ch = builder->CreateAdd(
            digit8, llvm::ConstantInt::get(i8Ty, '0'), "digit_ch");
        auto* slot = builder->CreateInBoundsGEP(
            bufferTy, digits,
            {llvm::ConstantInt::get(i32Ty, 0), idx}, "digit_slot");
        builder->CreateStore(ch, slot);
        auto* next = builder->CreateUDiv(mag, ten, "mag_next");
        builder->CreateStore(next, magSlot);
        auto* idxNext = builder->CreateAdd(
            idx, llvm::ConstantInt::get(i64Ty, 1), "idx_next");
        builder->CreateStore(idxNext, idxSlot);
        auto* zero = builder->CreateICmpEQ(
            next, llvm::ConstantInt::get(workTy, 0), "mag_zero");
        builder->CreateCondBr(zero, doneBB, loopBB);

        builder->SetInsertPoint(doneBB);
        auto* finalIdx = builder->CreateLoad(i64Ty, idxSlot, "idx_final");
        auto* negative64 = builder->CreateZExt(negative, i64Ty, "neg64");
        auto* total = builder->CreateAdd(finalIdx, negative64, "total_len");
        auto* allocSize = builder->CreateAdd(
            total, llvm::ConstantInt::get(i64Ty, 1), "alloc_size");
        auto* mallocFn = module->getFunction("malloc");
        auto* buffer = builder->CreateCall(mallocFn, {allocSize}, "itoa_buf");
        builder->CreateStore(
            llvm::ConstantInt::get(i8Ty, '-'),
            builder->CreateInBoundsGEP(
                i8Ty, buffer, llvm::ConstantInt::get(i64Ty, 0), "buf0"));
        auto* offset = builder->CreateZExt(negative, i64Ty, "offset");
        auto* outStart = builder->CreateInBoundsGEP(i8Ty, buffer, offset, "out_start");
        auto* outSlot = builder->CreateAlloca(getInt8PtrTy(), nullptr, "out_slot");
        builder->CreateStore(outStart, outSlot);
        auto* jSlot = builder->CreateAlloca(i64Ty, nullptr, "j_slot");
        builder->CreateStore(finalIdx, jSlot);
        builder->CreateBr(copyHead);

        builder->SetInsertPoint(copyHead);
        auto* j = builder->CreateLoad(i64Ty, jSlot, "j");
        auto* jZero = builder->CreateICmpEQ(
            j, llvm::ConstantInt::get(i64Ty, 0), "j_zero");
        builder->CreateCondBr(jZero, finishBB, copyBody);

        builder->SetInsertPoint(copyBody);
        auto* jPrev = builder->CreateSub(
            j, llvm::ConstantInt::get(i64Ty, 1), "j_prev");
        builder->CreateStore(jPrev, jSlot);
        auto* source = builder->CreateInBoundsGEP(
            bufferTy, digits,
            {llvm::ConstantInt::get(i32Ty, 0), jPrev}, "src");
        auto* sourceChar = builder->CreateLoad(i8Ty, source, "src_ch");
        auto* outPtr = builder->CreateLoad(getInt8PtrTy(), outSlot, "out_ptr");
        builder->CreateStore(sourceChar, outPtr);
        auto* outNext = builder->CreateInBoundsGEP(
            i8Ty, outPtr, llvm::ConstantInt::get(i64Ty, 1), "out_next");
        builder->CreateStore(outNext, outSlot);
        builder->CreateBr(copyHead);

        builder->SetInsertPoint(finishBB);
        auto* endPtr = builder->CreateLoad(getInt8PtrTy(), outSlot, "end_ptr");
        builder->CreateStore(llvm::ConstantInt::get(i8Ty, 0), endPtr);
        builder->CreateRet(buffer);

        builder->restoreIP(savedIP);
        return fn;
    }

    llvm::Function* integerFromStringFunction(const TypeInfo& type) {
        std::string name = std::string("__vn_atoi_") +
                           (type.isUnsigned ? "u" : "i") +
                           std::to_string(type.bits);
        if (auto* existing = module->getFunction(name)) return existing;

        auto savedIP = builder->saveIP();
        auto* i8Ty = llvm::Type::getInt8Ty(context);
        auto* i64Ty = llvm::Type::getInt64Ty(context);
        auto* valueTy = llvm::Type::getIntNTy(context, type.bits);
        auto* fn = llvm::Function::Create(
            llvm::FunctionType::get(valueTy, {getInt8PtrTy()}, false),
            llvm::Function::PrivateLinkage, name, module.get());

        auto* entry = llvm::BasicBlock::Create(context, "entry", fn);
        auto* loopBB = llvm::BasicBlock::Create(context, "digits", fn);
        auto* bodyBB = llvm::BasicBlock::Create(context, "digit", fn);
        auto* doneBB = llvm::BasicBlock::Create(context, "done", fn);
        builder->SetInsertPoint(entry);

        llvm::Argument* argument = fn->arg_begin();
        auto* first = builder->CreateLoad(i8Ty, argument, "first");
        auto* negative = builder->CreateICmpEQ(
            first, llvm::ConstantInt::get(i8Ty, '-'), "negative");
        auto* start = builder->CreateSelect(
            negative, llvm::ConstantInt::get(i64Ty, 1),
            llvm::ConstantInt::get(i64Ty, 0), "start");
        auto* indexSlot = builder->CreateAlloca(i64Ty, nullptr, "index_slot");
        auto* valueSlot = builder->CreateAlloca(valueTy, nullptr, "value_slot");
        auto* negativeSlot = builder->CreateAlloca(
            llvm::Type::getInt1Ty(context), nullptr, "negative_slot");
        builder->CreateStore(start, indexSlot);
        builder->CreateStore(llvm::ConstantInt::get(valueTy, 0), valueSlot);
        builder->CreateStore(negative, negativeSlot);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(loopBB);
        auto* index = builder->CreateLoad(i64Ty, indexSlot, "index");
        auto* charPtr = builder->CreateInBoundsGEP(
            i8Ty, argument, index, "char_ptr");
        auto* ch = builder->CreateLoad(i8Ty, charPtr, "char");
        auto* atLeastZero = builder->CreateICmpUGE(
            ch, llvm::ConstantInt::get(i8Ty, '0'), "digit_low");
        auto* atMostNine = builder->CreateICmpULE(
            ch, llvm::ConstantInt::get(i8Ty, '9'), "digit_high");
        auto* isDigit = builder->CreateAnd(atLeastZero, atMostNine, "is_digit");
        builder->CreateCondBr(isDigit, bodyBB, doneBB);

        builder->SetInsertPoint(bodyBB);
        auto* value = builder->CreateLoad(valueTy, valueSlot, "value");
        auto* digit = builder->CreateSub(
            builder->CreateZExt(ch, valueTy), 
            llvm::ConstantInt::get(valueTy, '0'), "digit");
        auto* scaled = builder->CreateMul(
            value, llvm::ConstantInt::get(valueTy, 10), "scaled");
        auto* next = builder->CreateAdd(scaled, digit, "next");
        builder->CreateStore(next, valueSlot);
        builder->CreateStore(
            builder->CreateAdd(index, llvm::ConstantInt::get(i64Ty, 1)),
            indexSlot);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        auto* result = builder->CreateLoad(valueTy, valueSlot, "result");
        auto* wasNegative = builder->CreateLoad(
            llvm::Type::getInt1Ty(context), negativeSlot, "was_negative");
        auto* negated = builder->CreateNeg(result, "negated");
        auto* finalValue = builder->CreateSelect(
            wasNegative, negated, result, "final");
        builder->CreateRet(finalValue);

        builder->restoreIP(savedIP);
        return fn;
    }

    llvm::Value* compileStr(llvm::Value* value, const TypeInfo& type) {
        if (!value) return nullptr;
        if (type.kind == TypeInfo::Str) return value;

        if (type.kind == TypeInfo::Int && type.bits > 64) {
            auto* fn = integerToStringFunction(type);
            return builder->CreateCall(fn, {value}, "int_str");
        }

        llvm::Type* bufType = llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 64);
        auto bufAlloca = builder->CreateAlloca(bufType, nullptr, "str_buf");
        auto bufPtr = builder->CreatePointerCast(bufAlloca, getInt8PtrTy());

        std::string formatStr;
        if (type.kind == TypeInfo::Int) {
            formatStr = type.isUnsigned ? "%llu" : "%lld";
            if (type.bits < 64) {
                value = type.isUnsigned
                    ? builder->CreateZExt(value, llvm::Type::getInt64Ty(context))
                    : builder->CreateSExt(value, llvm::Type::getInt64Ty(context));
            }
        } else if (type.kind == TypeInfo::Float) {
            formatStr = "%g";
            if (type.bits == 16 || type.bits == 32)
                value = builder->CreateFPExt(value, llvm::Type::getDoubleTy(context));
            else if (type.bits == 128)
                value = builder->CreateFPTrunc(value, llvm::Type::getDoubleTy(context));
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

    unsigned conversionWidth(const Call* call, unsigned defaultWidth) {
        if (call->args.size() < 2) return defaultWidth;
        auto* number = static_cast<const Number*>(call->args[1].get());
        if (!number) return defaultWidth;
        return static_cast<unsigned>(std::stoul(number->value));
    }

    llvm::Value* compileIntegerConversion(const Call* call, bool unsignedTarget) {
        if (call->args.empty() || call->args.size() > 2) {
            error(std::string(unsignedTarget ? "uint" : "int") +
                  "() expects one argument and an optional width");
            return nullptr;
        }
        unsigned bits = conversionWidth(call, 32);
        TypeInfo target = TypeInfo::intType(bits, unsignedTarget);
        auto arg = compileExpr(call->args[0].get());
        if (!arg) return nullptr;
        TypeInfo from = lastExprType;

        if (from.kind == TypeInfo::Str) {
            if (bits > 64) {
                auto* fn = integerFromStringFunction(target);
                return builder->CreateCall(fn, {arg}, "parsed_int");
            }
            auto* i64Ty = llvm::Type::getInt64Ty(context);
            auto callee = module->getOrInsertFunction(
                unsignedTarget ? "strtoull" : "strtoll",
                llvm::FunctionType::get(i64Ty,
                    {getInt8PtrTy(), getInt8PtrTy(),
                     llvm::Type::getInt32Ty(context)}, false));
            auto* parsed = builder->CreateCall(callee, {
                arg,
                llvm::ConstantPointerNull::get(getInt8PtrTy()),
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 10)
            }, "parsed_int");
            return convertTyped(parsed, TypeInfo::intType(64, unsignedTarget),
                                target, true, "conversion");
        }

        if (from.kind == TypeInfo::Int || from.kind == TypeInfo::Float)
            return convertTyped(arg, from, target, true, "conversion");

        error(std::string(unsignedTarget ? "uint" : "int") +
              "() cannot convert this value");
        return nullptr;
    }

    llvm::Value* compileFloat(const Call* call) {
        if (call->args.empty() || call->args.size() > 2) {
            error("float() expects one argument and an optional width");
            return nullptr;
        }
        unsigned bits = conversionWidth(call, 64);
        TypeInfo target = TypeInfo::floatType(bits);
        auto arg = compileExpr(call->args[0].get());
        if (!arg) return nullptr;
        TypeInfo from = lastExprType;

        if (from.kind == TypeInfo::Str) {
            auto* atofFn = module->getFunction("atof");
            auto* parsed = builder->CreateCall(atofFn, {arg}, "parsed_float");
            return convertTyped(parsed, TypeInfo::floatType(64), target, true,
                                "conversion");
        }

        if (from.kind == TypeInfo::Int || from.kind == TypeInfo::Float)
            return convertTyped(arg, from, target, true, "conversion");

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

    // Indexing a string yields a newly allocated one-byte string. The index is
    // a byte offset into the UTF-8 buffer and is not bounds-checked.
    llvm::Value* compileStringIndex(llvm::Value* str, const ASTNode* indexNode) {
        auto* index = compileExpr(indexNode);
        if (!index) return nullptr;
        if (!index->getType()->isIntegerTy()) {
            error("string index must be an integer");
            return nullptr;
        }

        auto* i8Ty = llvm::Type::getInt8Ty(context);
        auto* charPtr = builder->CreateInBoundsGEP(i8Ty, str, index,
                                                   "str_char_ptr");
        auto* ch = builder->CreateLoad(i8Ty, charPtr, "str_char");

        auto* mallocFn = module->getFunction("malloc");
        auto* result = builder->CreateCall(mallocFn, {
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 2)
        }, "char_str");
        auto* offset0 = llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(context), 0);
        auto* offset1 = llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(context), 1);
        auto* charSlot = builder->CreateInBoundsGEP(i8Ty, result, offset0,
                                                    "char_slot");
        builder->CreateStore(ch, charSlot);
        auto* endSlot = builder->CreateInBoundsGEP(i8Ty, result, offset1,
                                                   "char_end");
        builder->CreateStore(
            llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 0), endSlot);
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

    static std::string targetVarName(const ASTNode* node) {
        if (node->type == NodeType::Variable)
            return static_cast<const Variable*>(node)->name;
        if (node->type == NodeType::IndexExpr)
            return targetVarName(static_cast<const IndexExpr*>(node)->base.get());
        return "";
    }

    llvm::Value* compileLValue(const ASTNode* node, TypeInfo& outTy) {
        if (node->type == NodeType::Variable) {
            auto* var = static_cast<const Variable*>(node);
            if (lookupConst(var->name)) {
                error("cannot modify constant '" + var->name + "'");
                return nullptr;
            }
            auto* alloca = lookupVar(var->name);
            if (!alloca) {
                error("unknown variable '" + var->name + "'");
                return nullptr;
            }
            outTy = lookupVarType(var->name);
            return alloca;
        }
        if (node->type == NodeType::IndexExpr) {
            auto* ix = static_cast<const IndexExpr*>(node);
            TypeInfo baseTy;
            auto* basePtr = compileLValue(ix->base.get(), baseTy);
            if (!basePtr) return nullptr;
            if (baseTy.kind == TypeInfo::Str) {
                error("cannot assign to a character of a string; "
                      "strings are immutable");
                return nullptr;
            }
            if (baseTy.kind != TypeInfo::Array || !baseTy.elem) {
                error("cannot index a non-array value");
                return nullptr;
            }
            auto index = compileExpr(ix->index.get());
            if (!index) return nullptr;
            if (!index->getType()->isIntegerTy()) {
                error("array index must be an integer");
                return nullptr;
            }
            outTy = *baseTy.elem;
            return builder->CreateInBoundsGEP(
                llvmTypeFor(baseTy), basePtr,
                {llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0), index},
                "elem_ptr");
        }
        error("invalid assignment target");
        return nullptr;
    }

    bool compileArrayElements(const ArrayLiteral* lit, const std::string& where,
                              std::vector<llvm::Value*>& values,
                              std::vector<TypeInfo>& types) {
        for (auto& element : lit->elements) {
            auto* value = compileExpr(element.get());
            if (!value) return false;
            TypeInfo type = lastExprType;
            if (type.kind == TypeInfo::Void || type.kind == TypeInfo::Array) {
                error("invalid element in the initializer of " + where);
                return false;
            }
            values.push_back(value);
            types.push_back(type);
        }
        return true;
    }

    TypeInfo inferArrayElementType(std::vector<llvm::Value*>& values,
                                   std::vector<TypeInfo>& types,
                                   const std::string& where, bool& ok) {
        ok = false;
        if (values.empty()) {
            error("array literal for " + where + " cannot be empty");
            return TypeInfo{};
        }
        TypeInfo result = types[0];
        llvm::Value* representative = values[0];
        for (size_t i = 1; i < values.size(); ++i) {
            TypeInfo rightType = types[i];
            llvm::Value* rightValue = values[i];
            TypeInfo merged;
            std::string mergeError;
            if (!mergeNumericTypes(result, rightType, representative, rightValue,
                                   merged, mergeError)) {
                error(where + ": " + mergeError);
                return TypeInfo{};
            }
            result = merged;
            if (!llvm::isa<llvm::ConstantInt>(rightValue) &&
                !llvm::isa<llvm::ConstantFP>(rightValue) &&
                rightType.kind != TypeInfo::Str)
                representative = rightValue;
        }
        ok = true;
        return result;
    }

    llvm::Value* buildArrayFromValues(const std::vector<llvm::Value*>& values,
                                      const std::vector<TypeInfo>& types,
                                      const TypeInfo& arrayType,
                                      const std::string& where, bool& failed) {
        failed = false;
        if (!arrayType.elem) {
            error(where + " has an invalid array type");
            failed = true;
            return nullptr;
        }
        std::vector<llvm::Value*> elems;
        for (size_t i = 0; i < values.size(); ++i) {
            auto* value = convertTyped(values[i], types[i], *arrayType.elem,
                                       false, where + " element " +
                                       std::to_string(i + 1));
            if (!value || value->getType() != llvmTypeFor(*arrayType.elem)) {
                failed = true;
                return nullptr;
            }
            elems.push_back(value);
        }

        bool allConstant = true;
        for (auto* value : elems)
            if (!llvm::isa<llvm::Constant>(value)) allConstant = false;

        auto* llvmArrayTy = llvm::cast<llvm::ArrayType>(llvmTypeFor(arrayType));
        if (allConstant) {
            std::vector<llvm::Constant*> constants;
            for (auto* value : elems)
                constants.push_back(llvm::cast<llvm::Constant>(value));
            return llvm::ConstantArray::get(llvmArrayTy, constants);
        }

        llvm::Value* aggregate = llvm::UndefValue::get(llvmArrayTy);
        for (size_t i = 0; i < elems.size(); ++i) {
            aggregate = builder->CreateInsertValue(
                aggregate, elems[i], {static_cast<unsigned>(i)}, "elem_init");
        }
        return aggregate;
    }

    // Array values are handled by value: an array variable is loaded as a whole
    // aggregate, an array literal is materialized at the expected type, and any
    // other expression (such as a call) must produce exactly the expected type.
    llvm::Value* compileArrayValue(const ASTNode* node, const TypeInfo& expected,
                                   const std::string& where) {
        if (node->type == NodeType::ArrayLiteral) {
            auto* lit = static_cast<const ArrayLiteral*>(node);
            if (lit->elements.size() != expected.count) {
                error(where + " expects " + std::to_string(expected.count) +
                      " element(s), but the array literal has " +
                      std::to_string(lit->elements.size()));
                return nullptr;
            }
            std::vector<llvm::Value*> values;
            std::vector<TypeInfo> types;
            if (!compileArrayElements(lit, where, values, types)) return nullptr;
            bool failed = false;
            auto* value = buildArrayFromValues(values, types, expected, where,
                                               failed);
            return failed ? nullptr : value;
        }

        if (node->type == NodeType::Variable) {
            auto* var = static_cast<const Variable*>(node);
            auto* alloca = lookupVar(var->name);
            if (!alloca) {
                error("unknown variable '" + var->name + "'");
                return nullptr;
            }
            if (lookupVarType(var->name) != expected) {
                error(where + " requires an array of the exact expected type; '" +
                      var->name + "' does not match");
                return nullptr;
            }
            return builder->CreateLoad(llvmTypeFor(expected), alloca, var->name);
        }

        auto* value = compileExpr(node);
        if (!value) return nullptr;
        if (lastExprType != expected) {
            error(where + " requires an array of the exact expected type; "
                  "the value does not match");
            return nullptr;
        }
        return value;
    }

    llvm::Value* compileExpr(const ASTNode* node) {
        TypeInfo type;
        auto* value = compileExprTyped(node, type);
        lastExprType = value ? type : TypeInfo{};
        return value;
    }

    llvm::Value* compileExprTyped(const ASTNode* node, TypeInfo& outType) {
        switch (node->type) {
            case NodeType::Number: {
                auto* num = static_cast<const Number*>(node);
                if (num->value.find('.') != std::string::npos) {
                    double number = 0.0;
                    try {
                        number = std::stod(num->value);
                    } catch (...) {
                        error("invalid floating-point literal '" + num->value + "'");
                        return nullptr;
                    }
                    outType = TypeInfo::floatType(64);
                    return llvm::ConstantFP::get(
                        llvm::Type::getDoubleTy(context), number);
                }
                uint64_t value = 0;
                for (char c : num->value) {
                    unsigned digit = static_cast<unsigned>(c - '0');
                    if (value > (UINT64_MAX - digit) / 10) {
                        error("integer literal '" + num->value + "' is too large");
                        return nullptr;
                    }
                    value = value * 10 + digit;
                }
                if (value <= static_cast<uint64_t>(INT32_MAX)) {
                    outType = TypeInfo::intType(32, false);
                    return llvm::ConstantInt::get(
                        llvm::Type::getInt32Ty(context), value);
                }
                if (value <= static_cast<uint64_t>(INT64_MAX)) {
                    outType = TypeInfo::intType(64, false);
                    return llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(context), value);
                }
                outType = TypeInfo::intType(64, true);
                return llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(context), value);
            }
            case NodeType::StringLit: {
                auto* str = static_cast<const StringLiteral*>(node);
                outType = TypeInfo::strType();
                return builder->CreateGlobalString(str->value, "str_lit");
            }
            case NodeType::Variable: {
                auto* var = static_cast<const Variable*>(node);
                auto* constant = lookupConst(var->name);
                if (constant) {
                    outType = lookupConstType(var->name);
                    return constant;
                }
                auto* alloca = lookupVar(var->name);
                if (alloca) {
                    outType = lookupVarType(var->name);
                    if (outType.kind == TypeInfo::Array) {
                        error("array '" + var->name +
                              "' cannot be used as a value here; index it, "
                              "return it, or pass it to an array parameter");
                        return nullptr;
                    }
                    return builder->CreateLoad(llvmTypeFor(outType), alloca,
                                               var->name);
                }
                error("unknown variable '" + var->name + "'");
                return nullptr;
            }
            case NodeType::IndexExpr: {
                auto* ix = static_cast<const IndexExpr*>(node);

                if (ix->base->type == NodeType::Variable) {
                    auto* var = static_cast<const Variable*>(ix->base.get());
                    if (!lookupConst(var->name)) {
                        auto* alloca = lookupVar(var->name);
                        TypeInfo varType = lookupVarType(var->name);
                        if (alloca && varType.kind == TypeInfo::Array) {
                            TypeInfo elemType;
                            auto* ptr = compileLValue(node, elemType);
                            if (!ptr) return nullptr;
                            outType = elemType;
                            return builder->CreateLoad(
                                llvmTypeFor(elemType), ptr, "elem");
                        }
                    }
                }

                auto* base = compileExpr(ix->base.get());
                if (!base) return nullptr;
                TypeInfo baseType = lastExprType;
                if (baseType.kind == TypeInfo::Str) {
                    outType = TypeInfo::strType();
                    return compileStringIndex(base, ix->index.get());
                }
                if (baseType.kind != TypeInfo::Array || !baseType.elem) {
                    error("cannot index a value that is neither an array nor a string");
                    return nullptr;
                }

                auto* index = compileExpr(ix->index.get());
                if (!index) return nullptr;
                if (!index->getType()->isIntegerTy()) {
                    error("array index must be an integer");
                    return nullptr;
                }

                auto* arrTy = llvm::cast<llvm::ArrayType>(llvmTypeFor(baseType));
                auto* tmp = builder->CreateAlloca(arrTy, nullptr, "array_value");
                builder->CreateStore(base, tmp);
                auto* ptr = builder->CreateInBoundsGEP(
                    arrTy, tmp,
                    {llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0),
                     index},
                    "elem_ptr");
                outType = *baseType.elem;
                return builder->CreateLoad(llvmTypeFor(outType), ptr, "elem");
            }
            case NodeType::ArrayLiteral:
                error("an array literal can only be used to initialize an array, "
                      "pass an array argument, or return an array value");
                return nullptr;
            case NodeType::UnaryOp: {
                auto* unary = static_cast<const UnaryOp*>(node);
                auto* operand = compileExpr(unary->operand.get());
                if (!operand) return nullptr;
                TypeInfo operandType = lastExprType;
                if (unary->op == "-") {
                    if (operandType.kind == TypeInfo::Int) {
                        if (operandType.isUnsigned) {
                            if (auto* constant =
                                    llvm::dyn_cast<llvm::ConstantInt>(operand)) {
                                llvm::APInt value = constant->getValue();
                                if (value == llvm::APInt::getSignedMinValue(
                                                 value.getBitWidth())) {
                                    outType = TypeInfo::intType(
                                        operandType.bits, false);
                                    return constant;
                                }
                            }
                        }
                        outType = operandType;
                        return builder->CreateNeg(operand, "negtmp");
                    }
                    if (operandType.kind == TypeInfo::Float) {
                        outType = operandType;
                        return builder->CreateFNeg(operand, "fnegtmp");
                    }
                    error("unary '-' cannot be applied to this type");
                    return nullptr;
                }
                error("unknown unary operator '" + unary->op + "'");
                return nullptr;
            }
            case NodeType::BinaryOp: {
                auto* binary = static_cast<const BinaryOp*>(node);
                std::string op = binary->op;

                if (op == "&&" || op == "||") {
                    auto* left = compileExpr(binary->left.get());
                    if (!left) return nullptr;
                    auto* leftBool = toBool(left);
                    if (!leftBool) return nullptr;
                    llvm::BasicBlock* leftEndBB = builder->GetInsertBlock();

                    llvm::BasicBlock* rhsBB = llvm::BasicBlock::Create(
                        context, "logic_rhs", mainFunc);
                    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(
                        context, "logic_end", mainFunc);
                    if (op == "&&")
                        builder->CreateCondBr(leftBool, rhsBB, endBB);
                    else
                        builder->CreateCondBr(leftBool, endBB, rhsBB);

                    builder->SetInsertPoint(rhsBB);
                    auto* right = compileExpr(binary->right.get());
                    if (!right) return nullptr;
                    auto* rightBool = toBool(right);
                    if (!rightBool) return nullptr;
                    auto* rightValue = builder->CreateZExt(
                        rightBool, llvm::Type::getInt32Ty(context),
                        "logic_zext");
                    auto* rightEndBB = builder->GetInsertBlock();
                    builder->CreateBr(endBB);

                    builder->SetInsertPoint(endBB);
                    auto* phi = builder->CreatePHI(
                        llvm::Type::getInt32Ty(context), 2, "logic_phi");
                    phi->addIncoming(
                        llvm::ConstantInt::get(
                            llvm::Type::getInt32Ty(context),
                            op == "&&" ? 0 : 1),
                        leftEndBB);
                    phi->addIncoming(rightValue, rightEndBB);
                    outType = TypeInfo::intType(32, false);
                    return phi;
                }

                auto* left = compileExpr(binary->left.get());
                TypeInfo leftType = lastExprType;
                auto* right = compileExpr(binary->right.get());
                TypeInfo rightType = lastExprType;
                if (!left || !right) return nullptr;

                if (leftType.kind == TypeInfo::Array ||
                    rightType.kind == TypeInfo::Array) {
                    error("arrays cannot be used with operator '" + op + "'");
                    return nullptr;
                }

                if (op == ">" || op == "<" || op == "==" || op == "!=" ||
                    op == ">=" || op == "<=") {
                    if (leftType.kind == TypeInfo::Str ||
                        rightType.kind == TypeInfo::Str) {
                        if (leftType.kind == TypeInfo::Str &&
                            rightType.kind == TypeInfo::Str) {
                            if (op == "==" || op == "!=") {
                                auto* equal = callStrcmp(left, right);
                                if (op == "!=") {
                                    equal = builder->CreateXor(
                                        equal,
                                        llvm::ConstantInt::get(
                                            llvm::Type::getInt32Ty(context), 1),
                                        "cmpnot");
                                }
                                outType = TypeInfo::intType(32, false);
                                return equal;
                            }
                            error("string comparison with '" + op +
                                  "' is not allowed; use only == or !=");
                            return nullptr;
                        }
                        error("cannot compare a string with a non-string value");
                        return nullptr;
                    }

                    TypeInfo commonType;
                    std::string mergeError;
                    if (!mergeNumericTypes(leftType, rightType, left, right,
                                           commonType, mergeError)) {
                        error(mergeError);
                        return nullptr;
                    }
                    left = convertTyped(left, leftType, commonType, false,
                                        "comparison");
                    right = convertTyped(right, rightType, commonType, false,
                                         "comparison");

                    llvm::Value* cmp = nullptr;
                    if (commonType.kind == TypeInfo::Int) {
                        bool unsignedType = commonType.isUnsigned;
                        if (op == ">")
                            cmp = unsignedType
                                ? builder->CreateICmpUGT(left, right, "cmpgt")
                                : builder->CreateICmpSGT(left, right, "cmpgt");
                        else if (op == "<")
                            cmp = unsignedType
                                ? builder->CreateICmpULT(left, right, "cmplt")
                                : builder->CreateICmpSLT(left, right, "cmplt");
                        else if (op == "==")
                            cmp = builder->CreateICmpEQ(left, right, "cmpeq");
                        else if (op == "!=")
                            cmp = builder->CreateICmpNE(left, right, "cmpne");
                        else if (op == ">=")
                            cmp = unsignedType
                                ? builder->CreateICmpUGE(left, right, "cmpge")
                                : builder->CreateICmpSGE(left, right, "cmpge");
                        else if (op == "<=")
                            cmp = unsignedType
                                ? builder->CreateICmpULE(left, right, "cmple")
                                : builder->CreateICmpSLE(left, right, "cmple");
                    } else {
                        if (op == ">")
                            cmp = builder->CreateFCmpOGT(left, right, "fcmpgt");
                        else if (op == "<")
                            cmp = builder->CreateFCmpOLT(left, right, "fcmplt");
                        else if (op == "==")
                            cmp = builder->CreateFCmpOEQ(left, right, "fcmpeq");
                        else if (op == "!=")
                            cmp = builder->CreateFCmpONE(left, right, "fcmpne");
                        else if (op == ">=")
                            cmp = builder->CreateFCmpOGE(left, right, "fcmpge");
                        else if (op == "<=")
                            cmp = builder->CreateFCmpOLE(left, right, "fcmple");
                    }
                    outType = TypeInfo::intType(32, false);
                    return builder->CreateZExt(
                        cmp, llvm::Type::getInt32Ty(context), "cmp_zext");
                }

                if (leftType.kind == TypeInfo::Str ||
                    rightType.kind == TypeInfo::Str) {
                    if (op == "+" && leftType.kind == TypeInfo::Str &&
                        rightType.kind == TypeInfo::Str) {
                        outType = TypeInfo::strType();
                        return concatStrings(left, right);
                    }
                    error("operator '" + op + "' cannot be applied to a string");
                    return nullptr;
                }

                TypeInfo commonType;
                std::string mergeError;
                if (!mergeNumericTypes(leftType, rightType, left, right,
                                       commonType, mergeError)) {
                    error(mergeError);
                    return nullptr;
                }
                left = convertTyped(left, leftType, commonType, false,
                                    "arithmetic");
                right = convertTyped(right, rightType, commonType, false,
                                     "arithmetic");
                outType = commonType;

                if (commonType.kind == TypeInfo::Int) {
                    if (op == "+") return builder->CreateAdd(left, right, "addtmp");
                    if (op == "-") return builder->CreateSub(left, right, "subtmp");
                    if (op == "*") return builder->CreateMul(left, right, "multmp");
                    if (op == "/")
                        return commonType.isUnsigned
                            ? builder->CreateUDiv(left, right, "divtmp")
                            : builder->CreateSDiv(left, right, "divtmp");
                } else if (commonType.kind == TypeInfo::Float) {
                    if (op == "+") return builder->CreateFAdd(left, right, "faddtmp");
                    if (op == "-") return builder->CreateFSub(left, right, "fsubtmp");
                    if (op == "*") return builder->CreateFMul(left, right, "fmultmp");
                    if (op == "/") return builder->CreateFDiv(left, right, "fdivtmp");
                }
                return nullptr;
            }
            case NodeType::Call: {
                auto* call = static_cast<const Call*>(node);
                if (call->name == "input") {
                    outType = TypeInfo::strType();
                    return compileInput(call);
                }
                if (call->name == "str") {
                    if (call->args.size() != 1) {
                        error("str() expects one argument");
                        return nullptr;
                    }
                    auto* arg = compileExpr(call->args[0].get());
                    if (!arg) return nullptr;
                    TypeInfo argType = lastExprType;
                    outType = TypeInfo::strType();
                    return compileStr(arg, argType);
                }
                if (call->name == "int") {
                    outType = TypeInfo::intType(conversionWidth(call, 32), false);
                    return compileIntegerConversion(call, false);
                }
                if (call->name == "uint") {
                    outType = TypeInfo::intType(conversionWidth(call, 32), true);
                    return compileIntegerConversion(call, true);
                }
                if (call->name == "float") {
                    outType = TypeInfo::floatType(conversionWidth(call, 64));
                    return compileFloat(call);
                }

                auto* fn = module->getFunction(call->name);
                if (!fn) {
                    error("unknown function '" + call->name + "'");
                    return nullptr;
                }
                TypeInfo returnType = TypeInfo::voidType();
                auto returnIt = functionReturnTypes.find(call->name);
                if (returnIt != functionReturnTypes.end())
                    returnType = returnIt->second;

                auto paramsIt = functionParamTypes.find(call->name);
                if (call->args.size() != fn->arg_size()) {
                    error("function '" + call->name + "' expects " +
                          std::to_string(fn->arg_size()) +
                          " argument(s), got " +
                          std::to_string(call->args.size()));
                    return nullptr;
                }
                std::vector<llvm::Value*> args;
                for (size_t i = 0; i < call->args.size(); ++i) {
                    TypeInfo paramType;
                    if (paramsIt != functionParamTypes.end() &&
                        i < paramsIt->second.size())
                        paramType = paramsIt->second[i];
                    std::string where = "argument " + std::to_string(i + 1) +
                                        " of function '" + call->name + "'";
                    llvm::Value* value = nullptr;
                    if (paramType.kind == TypeInfo::Array) {
                        value = compileArrayValue(call->args[i].get(),
                                                  paramType, where);
                    } else {
                        value = compileExpr(call->args[i].get());
                        if (value) {
                            TypeInfo argType = lastExprType;
                            if (argType.kind == TypeInfo::Array) {
                                error(where + " is an array, but the parameter is "
                                      "not an array");
                                return nullptr;
                            }
                            value = convertTyped(value, argType, paramType,
                                                 false, where);
                            if (value &&
                                value->getType() != llvmTypeFor(paramType))
                                return nullptr;
                        }
                    }
                    if (!value) return nullptr;
                    args.push_back(value);
                }
                outType = returnType;
                if (returnType.kind == TypeInfo::Void)
                    return builder->CreateCall(fn, args);
                return builder->CreateCall(fn, args, "calltmp");
            }
            default:
                return nullptr;
        }
    }
    void emitPrintValue(llvm::Value* value, const TypeInfo& type) {
        std::string format;
        if (type.kind == TypeInfo::Int) {
            if (type.bits > 64) {
                format = "%s";
                value = compileStr(value, type);
            } else {
                format = type.isUnsigned ? "%llu" : "%lld";
                if (type.bits < 64) {
                    value = type.isUnsigned
                        ? builder->CreateZExt(
                              value, llvm::Type::getInt64Ty(context))
                        : builder->CreateSExt(
                              value, llvm::Type::getInt64Ty(context));
                }
            }
        } else if (type.kind == TypeInfo::Float) {
            format = "%f";
            if (type.bits == 16 || type.bits == 32)
                value = builder->CreateFPExt(
                    value, llvm::Type::getDoubleTy(context));
            else if (type.bits == 128)
                value = builder->CreateFPTrunc(
                    value, llvm::Type::getDoubleTy(context));
        } else if (type.kind == TypeInfo::Str) {
            format = "%s";
        } else if (type.kind == TypeInfo::Array) {
            error("print() cannot print an array");
            return;
        } else {
            error("print() cannot print this value");
            return;
        }

        auto* formatString = builder->CreateGlobalString(format, "printf_fmt");
        builder->CreateCall(module->getFunction("printf"),
                            {formatString, value});
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

                if (currentReturnInfo.kind == TypeInfo::Void) {
                    if (rs->value) {
                        error("'return' with a value inside a void function");
                    }
                    builder->CreateRetVoid();
                } else {
                    llvm::Value* value = nullptr;
                    if (!rs->value) {
                        error("'return' without a value inside a non-void function");
                    } else {
                        std::string where = "return value of function '" +
                                            mainFunc->getName().str() + "'";
                        if (currentReturnInfo.kind == TypeInfo::Array) {
                            value = compileArrayValue(rs->value.get(),
                                                      currentReturnInfo, where);
                        } else {
                            value = compileExpr(rs->value.get());
                            if (value) {
                                TypeInfo valueType = lastExprType;
                                if (valueType.kind == TypeInfo::Void) {
                                    error("cannot return a void value");
                                    value = nullptr;
                                } else if (valueType.kind == TypeInfo::Array) {
                                    error("cannot return an array from a function "
                                          "returning '" +
                                          currentReturnInfo.name() + "'");
                                    value = nullptr;
                                } else {
                                    value = convertTyped(value, valueType,
                                                         currentReturnInfo, false,
                                                         where);
                                }
                            }
                        }
                    }
                    if (!value) {
                        addDefaultReturn(currentReturnType);
                    } else {
                        builder->CreateRet(value);
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
                TypeInfo declared;
                bool hasDeclared = !vd->type.empty();
                if (hasDeclared) {
                    std::string parseError;
                    if (!parseTypeInfo(vd->type, declared, parseError)) {
                        error(parseError);
                        hasDeclared = false;
                    }
                }

                llvm::Value* initVal = nullptr;
                bool initFailed = false;

                if (vd->init && vd->init->type == NodeType::ArrayLiteral) {
                    auto* lit = static_cast<const ArrayLiteral*>(vd->init.get());
                    if (hasDeclared && declared.kind != TypeInfo::Array) {
                        error("variable '" + vd->name + "' of type '" +
                              vd->type +
                              "' cannot be initialized with an array literal");
                        initFailed = true;
                    } else {
                        std::vector<llvm::Value*> values;
                        std::vector<TypeInfo> types;
                        if (!compileArrayElements(
                                lit, "array '" + vd->name + "'", values, types)) {
                            initFailed = true;
                        } else {
                            TypeInfo arrayType;
                            if (hasDeclared) {
                                if (lit->elements.size() != declared.count) {
                                    error("array '" + vd->name + "' expects " +
                                          std::to_string(declared.count) +
                                          " element(s), but the initializer has " +
                                          std::to_string(lit->elements.size()));
                                    initFailed = true;
                                } else {
                                    arrayType = declared;
                                }
                            } else {
                                bool ok = false;
                                TypeInfo elem = inferArrayElementType(
                                    values, types, "array '" + vd->name + "'",
                                    ok);
                                if (!ok) {
                                    initFailed = true;
                                } else {
                                    arrayType = TypeInfo::arrayType(
                                        elem,
                                        static_cast<unsigned>(lit->elements.size()));
                                }
                            }
                            if (!initFailed) {
                                initVal = buildArrayFromValues(
                                    values, types, arrayType,
                                    "array '" + vd->name + "'", initFailed);
                                if (!initFailed) {
                                    declared = arrayType;
                                    hasDeclared = true;
                                }
                            }
                        }
                    }
                } else if (vd->init) {
                    if (hasDeclared && declared.kind == TypeInfo::Array) {
                        initVal = compileArrayValue(
                            vd->init.get(), declared,
                            "initializer of array '" + vd->name + "'");
                        if (!initVal) initFailed = true;
                    } else {
                        initVal = compileExpr(vd->init.get());
                        if (!initVal) {
                            initFailed = true;
                        } else {
                            TypeInfo initType = lastExprType;
                            if (initType.kind == TypeInfo::Void) {
                                error("variable '" + vd->name +
                                      "' cannot be initialized with a void value");
                                initVal = nullptr;
                                initFailed = true;
                            } else if (initType.kind == TypeInfo::Array) {
                                if (!hasDeclared) {
                                    declared = initType;
                                    hasDeclared = true;
                                } else {
                                    error("variable '" + vd->name +
                                          "' of type '" + declared.name() +
                                          "' cannot be initialized with an array "
                                          "value");
                                    initVal = nullptr;
                                    initFailed = true;
                                }
                            } else if (!hasDeclared) {
                                declared = initType;
                                hasDeclared = true;
                            } else {
                                initVal = convertTyped(
                                    initVal, initType, declared, false,
                                    "initializer of variable '" + vd->name + "'");
                            }
                        }
                    }
                }

                if (!hasDeclared) {
                    declared = TypeInfo::intType(32, false);
                    hasDeclared = true;
                }
                if (declared.kind == TypeInfo::Void) {
                    error("variable '" + vd->name + "' cannot be of type void");
                    declared = TypeInfo::intType(32, false);
                }

                llvm::Type* ty = llvmTypeFor(declared);
                auto* alloca = builder->CreateAlloca(ty, nullptr, vd->name);
                declareVar(vd->name, alloca, declared);

                if (initVal) {
                    if (initVal->getType() == ty)
                        builder->CreateStore(initVal, alloca);
                } else if (!initFailed) {
                    storeDefaultValue(alloca, ty);
                }
                break;
            }

            case NodeType::ConstDecl: {
                auto* cd = static_cast<const ConstDecl*>(stmt);
                if (cd->init && cd->init->type == NodeType::ArrayLiteral) {
                    error("constant arrays are not supported");
                    break;
                }
                if (!cd->init) break;
                auto* val = compileExpr(cd->init.get());
                if (!val) break;
                if (!llvm::isa<llvm::Constant>(val)) {
                    error("initializer for constant '" + cd->name +
                          "' is not a constant expression");
                    break;
                }
                TypeInfo valueType = lastExprType;
                if (valueType.kind == TypeInfo::Void ||
                    valueType.kind == TypeInfo::Array) {
                    error("constant '" + cd->name +
                          "' has an invalid type");
                    break;
                }
                auto* c = llvm::cast<llvm::Constant>(val);
                if (!cd->type.empty()) {
                    TypeInfo declared;
                    std::string parseError;
                    if (!parseTypeInfo(cd->type, declared, parseError)) {
                        error(parseError);
                        break;
                    }
                    if (declared.kind == TypeInfo::Void) {
                        error("constant '" + cd->name +
                              "' cannot be of type void");
                        break;
                    }
                    c = convertConstantTyped(c, valueType, declared, cd->name);
                    valueType = declared;
                }
                declareConst(cd->name, c, valueType);
                break;
            }

            case NodeType::Assign: {
                auto* as = static_cast<const Assign*>(stmt);
                std::string name = as->target ? targetVarName(as->target.get())
                                              : as->name;
                if (!name.empty() && lookupConst(name)) {
                    error("cannot assign to constant '" + name + "'");
                    break;
                }

                TypeInfo targetType;
                llvm::Value* targetPtr = nullptr;
                if (as->target) {
                    targetPtr = compileLValue(as->target.get(), targetType);
                } else {
                    auto* alloca = lookupVar(as->name);
                    if (!alloca) {
                        error("unknown variable '" + as->name + "'");
                        break;
                    }
                    targetType = lookupVarType(as->name);
                    targetPtr = alloca;
                }
                if (!targetPtr) break;

                if (targetType.kind == TypeInfo::Array) {
                    error("cannot assign to array '" + name +
                          "' as a whole; assign one element instead");
                    break;
                }

                auto* val = compileExpr(as->value.get());
                if (val) {
                    TypeInfo valueType = lastExprType;
                    if (valueType.kind == TypeInfo::Void) {
                        error("cannot assign a void value to '" + name + "'");
                        break;
                    }
                    if (valueType.kind == TypeInfo::Array) {
                        error("cannot assign an array value to '" + name +
                              "'; assign one element instead");
                        break;
                    }
                    val = convertTyped(val, valueType, targetType, false,
                                       "assignment to '" + name + "'");
                    builder->CreateStore(val, targetPtr);
                }
                break;
            }

            case NodeType::PrintStmt: {
                auto* ps = static_cast<const PrintStmt*>(stmt);
                size_t n = ps->args.size();
                for (size_t i = 0; i < n; ++i) {
                    auto* val = compileExpr(ps->args[i].get());
                    if (!val) continue;
                    TypeInfo type = lastExprType;
                    emitPrintValue(val, type);
                    if (i != n - 1) {
                        auto* space = builder->CreateGlobalString(" ", "space");
                        builder->CreateCall(module->getFunction("printf"),
                                            {space});
                    }
                }
                auto* newline = builder->CreateGlobalString("\n", "newline");
                builder->CreateCall(module->getFunction("printf"), {newline});
                break;
            }

            case NodeType::IfStmt: {
                auto* ifs = static_cast<const IfStmt*>(stmt);
                auto* condVal = compileExpr(ifs->condition.get());
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
                pushScope();
                if (auto* tn = static_cast<Block*>(ifs->thenBlock.get()))
                    for (auto& s : tn->statements) compileStmt(s.get());
                popScope();
                builder->CreateBr(endBB);

                if (elseBB) {
                    builder->SetInsertPoint(elseBB);
                    pushScope();
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
                auto* condVal = compileExpr(ws->condition.get());
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
                pushScope();
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