#include "llvm/Pass.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LegacyPassManager.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/TargetFolder.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/TargetLibraryInfo.h"

#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/UnifyFunctionExitNodes.h"

#include <string>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <sstream>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <bits/stdc++.h>
#include <map> 



#include "DDA/DDAClient.h"
#include "DDA/ContextDDA.h"
#include "SVF-FE/LLVMUtil.h"
#include "SVF-FE/SVFIRBuilder.h"
#include "Util/SCC.h"
#include "Util/Options.h"
#include "MemoryModel/PointerAnalysisImpl.h"

using namespace std;
using namespace SVF;
using namespace llvm;

namespace 
{
    class COLLATEPass : public ModulePass
    {
    public:
        static char ID;
        bool runOnModule(Module &M) override;

        /*将所有常量表达式转换为一组指令*/
        void constantExpr2Instruction(Module &M);
		void findConstantExpr(Instruction * I);
		void replaceCEOWithInstr(Instruction * I, Value * pointer);
        
        /*检查所有结构体，确定带不同后缀的类型的等同关系。
          因为llvm的typesystem，导致头文件中的一个类型
          在不同文件中被使用时会被认为是同名的不同类型，
          加上数字后缀，如%struct.ngx_http_connection_t.1248。
        */
        void analyzeStructTypeEquality(Module &M);
        bool isEqual(Type *a, Type *b);
        bool isTypeMatch(CallBase *CB, Function *F, Type *ReturnType);
        void analyzeIndirectCalls(Module &M);

        /*污点源是包含函数指针的内存对象*/
        void identifyTaintSources(Module &M, unordered_set<Value *> &result);
        bool shouldProtectType(Type *Ty, unordered_set<Type *> &Visited, vector<Type *> &Route, MDNode *TBAATag = NULL);

        void taintPropagation(Module &M, unordered_set<Value *> source, unordered_set<Value *> &result);
        bool doInFunction(Function &F, unordered_set<Value *> &taintValues);
        bool handleCallsite(CallBase *CS, Function *F, unordered_set<Value *> &taintValues);

        void dumpCrData(unordered_set<Value *> &content);

        void runPointerAnalysis(Module &M);
        void getPointsToSet(Value *val, vector<Value*> &result);

        void getMemOfCrData(unordered_set<Value *> &values, unordered_set<Value *> &mems);

        void instrumentTrustedInstructions(Module &M, unordered_set<Value *> &protectedMems);

        COLLATEPass() : ModulePass(ID) {}
    private:
        map<StructType *, int> typeID;
        DenseMap<Value *, Function *> directCall2Target;
        DenseMap<Value *, unordered_set<Function *>> indirectCall2Target;
        DenseMap<Type *, bool> taintSourceTypes;
        unordered_set<Function *> tiantVarArgs;
        unordered_set<Function *> tiantReturnFuncs;
        DenseMap<Function*, vector<Value*>> func2RetValue;
        PointerAnalysis *pta;

        /*debug*/
        DenseMap<Type *, vector<Type *>> Routes;
    };
}

