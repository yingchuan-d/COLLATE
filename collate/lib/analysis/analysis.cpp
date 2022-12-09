#include "../../include/collate.hpp"

void COLLATEPass::constantExpr2Instruction(Module &M)
{
    for (auto &F : M)
    {
        for(inst_iterator I = inst_begin(F), E = inst_end(F); I != E; I++)
        {
            if (isa<LandingPadInst>(*I))
                continue;
            findConstantExpr(&(*I));
        }
    }
}

void COLLATEPass::findConstantExpr(Instruction *I)
{
    unsigned operandNum = I->getNumOperands();
    for (unsigned index = 0; index < operandNum; index++)
    {
        Value *operand = I->getOperand(index);
        if (operand->getType()->isPointerTy() && isa<ConstantExpr>(operand))
        {
            replaceCEOWithInstr(I, operand);
        }
    }
}

void COLLATEPass::replaceCEOWithInstr(Instruction *I, Value *pointer)
{
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(pointer))
    {
        switch (CE->getOpcode())
        {
        case Instruction::BitCast:
        case Instruction::GetElementPtr:
        {
            if (PHINode *PN = dyn_cast<PHINode>(I))
            {
                Instruction *insertionPtr = nullptr;
                unsigned incomingValuesNum = PN->getNumIncomingValues();
                BasicBlock *BB = nullptr;
                for (unsigned index = 0; index < incomingValuesNum; index++)
                {
                    Value *value = PN->getIncomingValue(index);
                    if (value == pointer)
                    {
                        BB = PN->getIncomingBlock(index);

                        insertionPtr = &*BB->getFirstInsertionPt();
                        Instruction *newI = CE->getAsInstruction();
                        newI->insertBefore(insertionPtr);
                        PN->setIncomingValue(index, newI);

                        for (unsigned index_2 = index + 1; index_2 < incomingValuesNum; index_2++)
                            if (PN->getIncomingBlock(index_2) == BB)
                                PN->setIncomingValue(index_2, newI);

                        findConstantExpr(newI);
                    }
                }
            }
            else
            {
                Instruction *insertionPtr = &*I->getParent()->getFirstInsertionPt();
                Instruction *newI = CE->getAsInstruction();
                newI->insertBefore(insertionPtr);
                I->replaceUsesOfWith(CE, newI);
                findConstantExpr(newI);
            }
            break;
        }
        default:
            break;
        }
    }
}

void COLLATEPass::analyzeStructTypeEquality(Module &M)
{
    // 将所有结构体类型按前缀进行分类，如A.0和A.1分作一类
    map<string, vector<StructType*>> stypesMap;
    map<string, vector<StructType*>>::iterator it;
    regex e("%\\w*\\.\\w*");
    smatch result;
    auto toTypeStr = [](Type* Ty)
    {
        string typeStr;
        raw_string_ostream rso(typeStr);
        Ty->print(rso);
        return rso.str();
    };

    for(auto *S : M.getIdentifiedStructTypes())
    {
        S = dyn_cast<StructType>(S);
        string str = toTypeStr(S);
        regex_search(str, result, e);
        it = stypesMap.find(result.str());
        if (it == stypesMap.end())
        {
            vector<StructType*> tmp;
            tmp.push_back(S);
            stypesMap[result.str()] = tmp;
        }
        else
        {
            (it->second).push_back(S);
        }
    }

    // 给实际上相同的类型设置相同的标识符
    it = stypesMap.begin();
    for(int n = 0; it != stypesMap.end(); it++, n++)
    {
        for(auto *S : it->second)
        {
            typeID[S] = n;
        }
    }
}

bool COLLATEPass::isEqual(Type *a, Type *b)
{
    if(a == b) 
        return true;
        
    // 结构体类型的函数参数都为指针，所以需要比较指针指向的类型才对
    Type *ta = a;
    Type *tb = b;
    PointerType *tmpType;
    while (ta->isPointerTy() && tb->isPointerTy())
    { 
        tmpType = dyn_cast<PointerType>(ta);
        ta = tmpType->getElementType();
        tmpType = dyn_cast<PointerType>(tb);
        tb = tmpType->getElementType();
    }

    // 如果都是结构体，就根据analyzeStructTypeEquality的结果进行分析
    if(isa<StructType>(ta) && isa<StructType>(tb)) 
    {
        StructType *sa = dyn_cast<StructType>(ta);
        StructType *sb = dyn_cast<StructType>(tb);
        if(typeID[sa] == typeID[sb])
        {
            return true;
        }
    }  
    return false;
}

bool COLLATEPass::isTypeMatch(CallBase *CB, Function *F, Type *ReturnType)
{
    auto fItr = F->arg_begin();
    auto aItr = CB->arg_begin();

    // 返回值类型不同，那F肯定不可能是CS调用的目标函数
    if (!isEqual(ReturnType, F->getReturnType())) 
        return false;

    // 依次比较形参和实参的类型
    while (fItr != F->arg_end() && aItr != CB->arg_end())
    { 
        Argument *formal = const_cast<Argument *>(&(*fItr));
        Value *actual = *aItr;

        if (!isEqual(formal->getType(), actual->getType()))
            return false;

        ++fItr;
        ++aItr;
    }

    if (fItr == F->arg_end() && aItr == CB->arg_end())
        return true;

    return false;
}

void COLLATEPass::analyzeIndirectCalls(Module &M)
{
    // 找到所有被取地址的函数
    unordered_set<Function*> AllFunctions;
    for (auto &F : M)
    {
        if (F.hasAddressTaken())     
            AllFunctions.insert(&F);
    }

    for (auto &F : M)
    {
        if (F.isDeclaration() || F.isIntrinsic())
            continue;

        for (inst_iterator ii = inst_begin(F), ie = inst_end(F); ii != ie; ++ii)
        { 
            Instruction *inst = &(*ii);
            if (CallInst *cInst = dyn_cast<CallInst>(inst))
            { 
                if (!(cInst->getCalledFunction()))
                {
                    unordered_set<Function *> &targets = indirectCall2Target[cInst->getCalledOperand()]; 
                    
                    // 排除这种情况：
                    // %23 = bitcast void (%struct.ngx_http_request_s.1250*, i64)* @ngx_http_finalize_request to void (%struct.ngx_http_request_s.1062*, i64)*
                    //   call void %23(%struct.ngx_http_request_s.1062* %0, i64 %5), !dbg !105392
                    // 此时函数指针实际上只能指向@ngx_http_finalize_request这一个函数
                    bool done = false;
                    if(isa<BitCastInst>(cInst->getCalledOperand()))
                    {
                        BitCastInst *bI = dyn_cast<BitCastInst>(cInst->getCalledOperand());
                        Value *op = bI->getOperand(0);
                        if(isa<Function>(op))
                        {
                            targets.insert(dyn_cast<Function>(op));
                            done = true;
                        }
                    }

                    if(!done)
                    {
                        // 通过类型匹配，找到间接调用可能的目标
                        for (Function *tmpF : AllFunctions)
                        {
                            if (isTypeMatch(dyn_cast<CallBase>(cInst), tmpF, inst->getType())) 
                                targets.insert(tmpF);                                                                                               
                        }
                    }
                }
                else
                    directCall2Target[cInst] = cInst->getCalledFunction();
            }
            else if (InvokeInst *iInst = dyn_cast<InvokeInst>(inst))
            {
                if (!(iInst->getCalledFunction()))
                {
                    unordered_set<Function *> &targets = indirectCall2Target[iInst->getCalledOperand()];
                    bool done = false;

                    if(isa<BitCastInst>(iInst->getCalledOperand()))
                    {
                        BitCastInst *bI = dyn_cast<BitCastInst>(iInst->getCalledOperand());
                        Value *op = bI->getOperand(0);
                        if(isa<Function>(op))
                        {
                            targets.insert(dyn_cast<Function>(op));
                            done = true;
                        }
                    }
                    if(!done)
                    {
                        for (Function *tmpF : AllFunctions)
                        {
                            if (isTypeMatch(dyn_cast<CallBase>(iInst), tmpF, inst->getType()))
                                targets.insert(tmpF);
                        }
                    }
                }
                else
                    directCall2Target[iInst] = iInst->getCalledFunction();
            }
        }
    }
}

void COLLATEPass::identifyTaintSources(Module &M, unordered_set<Value *> &result)
{
    for (auto const &G : M.globals())
    {
        Type *T = G.getType();
        vector<Type*> route;
        unordered_set<Type *> visited;

        if(shouldProtectType(T, visited, route) && G.getNumUses() != 0)
            result.insert(const_cast<GlobalVariable*>(&G));
    }

    for (auto &F : M)
    {
        for(inst_iterator I = inst_begin(F), E = inst_end(F); I != E; I++)
        {
            vector<Type*> route;
            unordered_set<Type *> visited;

            if(shouldProtectType(I->getType(), visited, route) && I->getNumUses() != 0)
                result.insert(&(*I));

            for(int i = 0; i < I->getNumOperands(); i++)
            {
                Value *operand = I->getOperand(i);
                vector<Type*> route;
                unordered_set<Type *> visited;

                if(shouldProtectType(operand->getType(), visited, route, 
                                    I->getMetadata(LLVMContext::MD_tbaa)))
                {
                    result.insert(operand);
                    if(isa<StoreInst>(*I))
                        result.insert(&(*I));
                }
            }

            if(StoreInst *sI = dyn_cast<StoreInst>(&(*I)))
            {
                Value *pointer = sI->getPointerOperand();
                Value *value = sI->getValueOperand();

                if (BitCastInst *bI = dyn_cast<BitCastInst>(pointer))
                {
                    if (value->getType()->isPointerTy())
                    {
                        Type *srcType = bI->getSrcTy();     
                        Type *pointedType = cast<PointerType>(srcType)->getElementType();

                        // 如果pointer原本指向的数据也是一个指针且这个指针的类型是敏感的
                        if (pointedType->isPointerTy() && 
                            shouldProtectType(pointedType, visited, route))
                        { 
                            result.insert(value);
                            result.insert(pointer);
                        }
                    }
                }
            }

            // // 待修改完SVF后补完
            // if(LoadInst *L = dyn_cast<LoadInst>(&(*I)))
            // {
            //     if(L->getType()->getPointerElementType()->isVoidTy())
            //     {

            //     }
            // }

            // 记录所有函数的所有返回值(函数可能有多条返回指令，多个返回值)
            if (ReturnInst *retInst = dyn_cast<ReturnInst>(&(*I)))
            { 
                if (retInst->getNumOperands() > 0)
                    func2RetValue[&F].push_back(retInst->getOperand(0));
            }
        }
    }

}

bool COLLATEPass::shouldProtectType(Type *Ty, unordered_set<Type *> &Visited, vector<Type *> &Route, MDNode *TBAATag)
{
    auto it = taintSourceTypes.find(Ty);
    if (it != taintSourceTypes.end() && !TBAATag)
    {
        if(it->second)
            Route.push_back(Ty);
        return it->second;
    }

    if (Ty->isFunctionTy())
    {
        taintSourceTypes[Ty] = true;
        Route.push_back(Ty);
        return true;
    }

    if (Ty->getTypeID() <= Type::X86_MMXTyID || Ty->isIntegerTy())
    {
        taintSourceTypes[Ty] = false;
        return false;
    }

    auto getMDString = [](MDNode *TBAATag)
    {
        MDString *TagName = dyn_cast<MDString>(TBAATag->getOperand(0));
        if (TagName)
            return TagName;

        MDNode *TBAATag2 = dyn_cast<MDNode>(TBAATag->getOperand(0));
        if (!TBAATag2 || TBAATag2->getNumOperands() <= 1)
            return static_cast<MDString *>(nullptr);

        TagName = dyn_cast<MDString>(TBAATag2->getOperand(0));
        return TagName;
    };

    // 处理包含在结构体中的i8*指针
    if (TBAATag && TBAATag->getNumOperands() > 1)
    {
        MDString *TagName = getMDString(TBAATag);
        if (TagName)
        {
            if (TagName->getString() == "vtable pointer" ||
                TagName->getString() == "function pointer")
            {
                taintSourceTypes[Ty] = true;
                Route.push_back(Ty);
                return true;
            }
        }
    }

    // 找到指针最终指向的类型
    Type *elemType = Ty;
    while (elemType->isPointerTy())
    {
        PointerType *tmpType = dyn_cast<PointerType>(elemType);
        elemType = tmpType->getElementType(); 
    }

    // 这是针对递归类型，如：
    //              struct A{
    //                  struct A* a;……}
    if (Visited.find(elemType) != Visited.end())
    {
        return false;
    }

    // 最终指向函数
    if (elemType->isFunctionTy())
    {
        taintSourceTypes[Ty] = true;
        Route.push_back(Ty);
        return true;
    }

    // 最终指向数组
    if (elemType->isArrayTy())
    {
        ArrayType *tmpTy;
        tmpTy = dyn_cast<ArrayType>(elemType);
        Visited.insert(tmpTy);
        // 判断元素的类型是否敏感
        bool isSensitive = shouldProtectType(tmpTy->getElementType(), Visited, Route, nullptr);

        if (isSensitive)
        {
            Route.push_back(Ty);
            Routes[Ty] = Route;
        }
        
        taintSourceTypes[Ty] = isSensitive;
        return isSensitive;
    }

    // 最终指向容器
    if (elemType->isVectorTy())
    {
        VectorType *tmpTy;
        tmpTy = dyn_cast<VectorType>(elemType);// 转换成容器类型
        Visited.insert(tmpTy);
        bool isSensitive = shouldProtectType(tmpTy->getElementType(), Visited, Route);// 判断元素的类型是否敏感

        if (isSensitive)
        {
            Route.push_back(Ty);
            Routes[Ty] = Route;
        }
        
        taintSourceTypes[Ty] = isSensitive;
        return isSensitive;
    }

    // 处理结构体、联合体、类
    if (elemType->isStructTy())
    {
        StructType* tmpTy = dyn_cast<StructType>(elemType);
        
        // 特殊情况，只声明，没赋值，如：void Bar(struct Foo *);
        if (tmpTy->isOpaque())
        {
            taintSourceTypes[Ty] = false;
            return false;
        }

        /*特殊情况，clang由于类型系统的缺陷，在某些情况下会将结构体中函数指针的类型设置为{}*
          详见https://stackoverflow.com/questions/18730620/ 和
          https://lists.llvm.org/pipermail/cfe-dev/2016-November/051601.html
          但这种情况下TBAATag仍能正确的将其标识为函数指针
        */
        if (tmpTy->getNumElements() == 0 && TBAATag && TBAATag->getNumOperands() > 1)
        {
            MDString *TagName = getMDString(TBAATag);
            if (TagName && TagName->getString() == "function pointer")
            {
                taintSourceTypes[Ty] = true;
                Route.push_back(Ty);
                return true;
            }
        }

        bool isSensitive = false;
        Visited.insert(tmpTy);

        for (int i = 0; i < tmpTy->getNumElements(); i++)
        {
            auto subTy = tmpTy->getElementType(i);
            isSensitive = shouldProtectType(subTy, Visited, Route);
            if(isSensitive)
                break;
        }
        if(isSensitive)
        {
            Route.push_back(Ty);
            Routes[Ty] = Route;
        }

        taintSourceTypes[Ty] = isSensitive;
        return isSensitive;
    }

    taintSourceTypes[Ty] = false;
    return false;
}

void COLLATEPass::taintPropagation(Module &M, unordered_set<Value *> source, unordered_set<Value *> &result)
{
    analyzeIndirectCalls(M);

    bool flag = true;
    unordered_set<Value *> taintedSet = source;
    result = source;
    
    while(flag)
    {
        flag = false;
        for (auto &F : M)
        {
            flag |= doInFunction(F, taintedSet);
        }
    }

    unordered_set<Value *> complement;
    for (auto it : taintedSet)
    {
        if(Instruction *I = dyn_cast<Instruction>(it))
        {
            if(isa<CallInst>(I) || isa<InvokeInst>(I))
                continue;

            /*处理phi node：
                如果phi node为敏感值，则说明在前面的分支中对敏感值进行了不同的赋值操作，
                也就意味着分支条件间接决定了敏感值的值，即分支条件是constraining data。
                如：
                    if(i == 0)
                    {
                        fun = arr[0];
                    }
                    else{
                        fun = arr[1];
                    }
                    转换成IR后，分支回合的phinode中会有如下一条指令：
                        %.01 = phi void (i8*)* [ %12, %10 ], [ %17, %15 ]
                    其中%.01就是fun。意味着在两个分支中给fun赋了不同的值，因此i是constraining data。
            */
            if(PHINode *PN = dyn_cast<PHINode>(I))
            {
                DominatorTree DT = DominatorTree(*(PN->getFunction()));
                DomTreeNodeBase< BasicBlock > *BN = DT.getNode(PN->getParent());
                BasicBlock *X = BN->getIDom()->getBlock();

                if(BranchInst *BI = dyn_cast<BranchInst>(X->getTerminator()))
                {
                    if(BI->isConditional())
                    {
                        Value* cond = BI->getCondition();
                        Instruction *I = dyn_cast<Instruction>(cond);
                        Value *operand = I->getOperand(0);
                        complement.insert(operand);
                    }
                }
                else if(SwitchInst *SI = dyn_cast<SwitchInst>(X->getTerminator()))
                {
                    Value* cond = SI->getCondition();
                    complement.insert(cond);
                }
            }

            int n = I->getNumOperands();
            for (unsigned i = 0; i < n; ++i)
            {
                Value *operand = I->getOperand(i);
                if (!result.count(operand) && !isa<ConstantData>(operand))
                    complement.insert(operand);
            }
        }
    }

    map<Function*, vector<Value*>> func2Caller;
    for(auto it : directCall2Target)
    {
        Value *cs = it.getFirst(); // call/invoke指令
        Function *f = it.getSecond(); //调用的函数
        auto fc = func2Caller.find(f);
        if(fc == func2Caller.end())
        {
            vector<Value*> tmp;
            tmp.push_back(cs);
            func2Caller[f] = tmp;
        }
        else
            (fc->second).push_back(cs);
    }

    for(auto it : indirectCall2Target)
    {
        for(auto user : (it.getFirst())->users())
        {
            if(isa<CallInst>(user) || isa<InvokeInst>(user))
            {
                Value *cs = user;
                unordered_set<Function*> funcs = it.getSecond();
                for(auto f : funcs)
                {
                    auto fc = func2Caller.find(f);
                    if(fc == func2Caller.end())
                    {
                        vector<Value*> tmp;
                        tmp.push_back(cs);
                        func2Caller[f] = tmp;
                    }
                    else
                        (fc->second).push_back(cs);
                }
            }
        }
    }

    auto backTrace = [](unordered_set<Value *> &tmp)
    {
        queue<Value *> q;
        for(auto it : tmp)
            q.push(it);
        while(!q.empty())
        {
            auto v = q.front();
            q.pop();
            if(Instruction *I = dyn_cast<Instruction>(v))
            {
                if(!isa<AllocaInst>(I) && !isa<CallInst>(I) && !isa<LoadInst>(I))
                {
                    int n = I->getNumOperands();
                    for (int i = 0; i < n; i++)
                    {
                        Value *op = I->getOperand(i);
                        if(!tmp.count(op) && !isa<ConstantData>(op)) 
                        {
                            q.push(op);
                            tmp.insert(op);
                        }
                    }
                }
            }
        }
    };

    vector<Argument*> params; // 形参
    unordered_set<Argument*> checkedParams; 
    vector<Value*> args; // 形参对应的实参

    while(true)
    {
        args.clear();
        params.clear();
        backTrace(complement);
        for(auto it : complement)
        {
            result.insert(it);
            if(Argument *param = dyn_cast<Argument>(it))
            {
                if(!checkedParams.count(param))
                {
                    args.push_back(param);
                    checkedParams.insert(param);
                }
            }
        }

        for(auto param : params)
        {
            Function *f = param->getParent();
            vector<Value*> callSites = func2Caller[param->getParent()];
            for(auto cs : callSites)
            {
                CallBase *CB = dyn_cast<CallBase>(cs);
                auto fit = f->arg_begin(); // 形参
                auto ait = CB->arg_begin(); // 实参
                // 查找形参param对应的实参arg
                while (fit != f->arg_end() && ait != CB->arg_end())
                {
                    Argument *formal = dyn_cast<Argument>(&(*pit));
                    Value *actual = *ait;
                    if(formal == param)
                        break; 
                    ++fit;
                    ++ait;
                }
                args.push_back(*ait);
            }
        }

        if(args.empty())
            break;
        
        complement.clear();
        for(auto it : args)
            complement.insert(it);
    }
}

bool COLLATEPass::doInFunction(Function &F, unordered_set<Value *> &taintValues)
{
    auto addIfOneIsSensitive = [&taintValues](Value *V1, Value *V2)
    {
        if(taintValues.find(V1) != taintValues.end())
            return taintValues.insert(V2).second;
        else if(taintValues.find(V2) != taintValues.end())
            return taintValues.insert(V1).second;
        else
            return false;
    };

    auto addSecondIfFirstIsSensitive = [&taintValues](Value *V1, Value *V2)
    {
        if(taintValues.find(V1) != taintValues.end())
            return taintValues.insert(V2).second;
        else
            return false;
    };

    auto addValueIfReturnIsSensitive = [&](Value *V, Function *F)
    {
        if (F->getReturnType()->isPointerTy())
        {
            if (tiantReturnFuncs.count(F))
                return taintValues.insert(V).second;
        }
        return false;
    };

    bool ret = false;
    for (inst_iterator ii = inst_begin(F), ie = inst_end(F);
         ii != ie; ++ii)
    {
        Instruction *inst = &(*ii);
        if (BitCastInst *bcInst = dyn_cast<BitCastInst>(inst))
        {
            ret |= addIfOneIsSensitive(bcInst, bcInst->getOperand(0));
        }
        else if (LoadInst *lInst = dyn_cast<LoadInst>(inst))
        {
            ret |= addSecondIfFirstIsSensitive(lInst, lInst->getPointerOperand());
        }
        else if (StoreInst *sInst = dyn_cast<StoreInst>(inst))
        { 
            bool tmpR = addSecondIfFirstIsSensitive(sInst->getValueOperand(), sInst->getPointerOperand());
            ret |= tmpR;
            tmpR = addSecondIfFirstIsSensitive(sInst->getPointerOperand(), sInst->getValueOperand());
            ret |= tmpR;
            if (taintValues.find(sInst->getValueOperand()) != taintValues.end() || taintValues.find(sInst->getPointerOperand()) != taintValues.end()) 
                ret |= taintValues.insert(sInst).second;
        }
        else if (GetElementPtrInst *gepInst = dyn_cast<GetElementPtrInst>(inst))
        {   
            // 如果发现这条getelementptr是用于获取敏感的va_list内元素的地址，那么它是敏感值
            ret |= addSecondIfFirstIsSensitive(gepInst, gepInst->getPointerOperand());
            Value *pOperand = gepInst->getPointerOperand();
            Type *OTy = pOperand->getType();
            if (OTy->isVectorTy())
                OTy = dyn_cast<VectorType>(OTy)->getElementType();
                
            Type *pTy = (cast<PointerType>(OTy))->getElementType(); 
            if (StructType *sTy = dyn_cast<StructType>(pTy))
            {
                if (sTy->hasName() && sTy->getName() == "struct.__va_list_tag" && tiantVarArgs.count(&F))
                    if (gepInst->getType()->isPointerTy())
                    {
                        PointerType *pTy = cast<PointerType>(gepInst->getType());
                        if (pTy->getElementType()->isIntegerTy(8))
                            ret = taintValues.insert(gepInst).second;
                    }
            }
        }
        else if (CallInst *cInst = dyn_cast<CallInst>(inst))
        {
            Function *f = cInst->getCalledFunction();
            CallBase *CB = dyn_cast<CallBase>(cInst);
            if (f)
            {
                // 处理直接调用，跨函数传播
                ret |= handleCallsite(CB, f, taintValues);//进行污点传播
                ret |= addValueIfReturnIsSensitive(inst, f);
                /*
                if (func2RetValue.find(f) != func2RetValue.end()) {
                    ret |= addSecondIfFirstIsSensitive(cInst, func2RetValue[f]);
                }
                */
            }
            else
            {
                // 处理间接调用，跨函数传播
                auto &targets = indirectCall2Target[cInst->getCalledOperand()];
                for (Function *target : targets)
                {
                    ret |= handleCallsite(CB, target, taintValues);
                    ret |= addValueIfReturnIsSensitive(inst, target);
                    /*
                    if (func2RetValue.find(target) != func2RetValue.end()) {
                        ret |= addSecondIfFirstIsSensitive(cInst, func2RetValue[target]);
                    }
                    */
                }
            }
        }
        else if (InvokeInst *iInst = dyn_cast<InvokeInst>(inst))
        {
            // 处理invoke指令，和call一样
            Function *f = iInst->getCalledFunction();
            CallBase *CB = dyn_cast<CallBase>(iInst);
            if (f)
            {
                ret |= handleCallsite(CB, f, taintValues);
                ret |= addValueIfReturnIsSensitive(inst, f);
                /*
                if (func2RetValue.find(f) != func2RetValue.end()) {
                    ret |= addSecondIfFirstIsSensitive(iInst, func2RetValue[f]);
                }
                */
            }
            else
            {
                std::unordered_set<Function *> &targets = indirectCall2Target[iInst->getCalledOperand()];
                for (Function *target : targets)
                {
                    ret |= handleCallsite(CB, target, taintValues);
                    ret |= addValueIfReturnIsSensitive(inst, target);
                    /*
                    if (func2RetValue.find(target) != func2RetValue.end()) {
                        ret |= addSecondIfFirstIsSensitive(iInst, func2RetValue[target]);
                    }
                    */
                }
            }
        }
        else if (ReturnInst *rInst = dyn_cast<ReturnInst>(inst))
        {
            // 处理return指令，跨函数传播
            Value *retValue = rInst->getReturnValue();
            if (retValue && taintValues.count(retValue))
            {
                Function *f = rInst->getParent()->getParent();
                tiantReturnFuncs.insert(f);// 记录函数有敏感的返回值
                if (func2RetValue.find(f) != func2RetValue.end())// 函数可能有多个返回指令多个返回值，只要有一个是敏感的，那全都设为敏感值
                    for (auto value : func2RetValue[f])
                        ret |= taintValues.insert(value).second;
                ret |= taintValues.insert(rInst).second;
            }
        }
        else if (PHINode *pNode = dyn_cast<PHINode>(inst))
        {
            // 如果传给phinode的值是敏感值，那么phinode也设为敏感值
            for (unsigned i = 0; i < pNode->getNumIncomingValues(); ++i)
            {
                Value *incomingV = pNode->getIncomingValue(i);
                ret |= addSecondIfFirstIsSensitive(incomingV, pNode);
            }
            // 如果phinode是敏感值，那么传给phinode的值都设为敏感值
            for (unsigned i = 0; i < pNode->getNumIncomingValues(); ++i)
            {
                Value *incomingV = pNode->getIncomingValue(i);
                ret |= addSecondIfFirstIsSensitive(pNode, incomingV);
            }
        }
        else if (SelectInst *SI = dyn_cast<SelectInst>(inst))
        {
            Value *TrueValue = SI->getTrueValue();
            Value *FalseValue = SI->getFalseValue();
            ret |= addSecondIfFirstIsSensitive(SI, TrueValue);
            ret |= addSecondIfFirstIsSensitive(SI, FalseValue);
        }
        
        else if (ExtractElementInst *EEI = dyn_cast<ExtractElementInst>(inst))
        {
            Value *vectorOperand = EEI->getVectorOperand();
            ret |= addSecondIfFirstIsSensitive(EEI, vectorOperand);
        }
        else if (ExtractValueInst *EVI = dyn_cast<ExtractValueInst>(inst))
        {
            Value *aggregateOperand = EVI->getAggregateOperand();
            ret |= addSecondIfFirstIsSensitive(EVI, aggregateOperand);
        }

        else if (InsertElementInst *IEI = dyn_cast<InsertElementInst>(inst))
        {
            Value *vectorOperand = IEI->getOperand(0);
            Value *valueOperand = IEI->getOperand(1);
            ret |= addSecondIfFirstIsSensitive(valueOperand, vectorOperand);
            ret |= addSecondIfFirstIsSensitive(valueOperand, IEI);
        }

        else if (InsertValueInst *IVI = dyn_cast<InsertValueInst>(inst))
        {
            Value *baseOperand = IVI->getOperand(0);
            Value *valueOperand = IVI->getOperand(1);
            ret |= addSecondIfFirstIsSensitive(valueOperand, baseOperand);
            ret |= addSecondIfFirstIsSensitive(valueOperand, IVI);
        }
    }
    return ret;
}

bool COLLATEPass::handleCallsite(CallBase *CS, Function *F, unordered_set<Value *> &taintValues)
{
    bool ret = false;
    auto fItr = F->arg_begin();// 形参
    auto aItr = CS->arg_begin();// 实参

    // 忽略IgnoreFuncs中的函数
    const char *ignoreFuncs[] = {
    // c functions
    "__cxa_atexit", "realloc", "free", "obstack_free",
    "printf", "sprintf", "vsprintf", "fprintf", "vfprintf",
    "read", "puts", "scanf", "fread", "fgets", "fputs", "fwrite", 
    "sscanf", "memchr", "memcmp", "strlen", "strchr", "strtoul", 
    "strcmp", "strncmp", "strcpy", "strncpy", "strrchr", "strcat",
    "strtol", "strpbrk", "strstr", "strcspn", "strspn", "strerror", 
    "strtok", "strtod", "bsearch", "remove", "getenv",
    // c++ functions
    "_ZdlPv", "_ZdaPv", "__cxa_begin_catch", "_ZSt20__throw_length_errorPKc", 
    "__cxa_free_exception", "_cxa_throw", "__dynamic_cast", nullptr};
    const char *fn = F->getName().data();
    for (unsigned i = 0; ignoreFuncs[i] != nullptr; ++i)
    {
        if (strcmp(ignoreFuncs[i], fn) == 0)
        {
            return false;
        }
    }

    const char *propagateArgFuncs[] = {"memcpy", "llvm.memcpy", nullptr};
    for (unsigned i = 0; propagateArgFuncs[i] != nullptr; i++)
    {
        unsigned fnLen = strlen(propagateArgFuncs[i]);
        if (strncmp(propagateArgFuncs[i], fn, fnLen) == 0)
        {
            Value *first = *aItr++;
            Value *second = *aItr;

            if(taintValues.find(first) != taintValues.end())
            {
                taintValues.insert(second);
                return true;
            }
            else if(taintValues.find(second) != taintValues.end())
            {
                taintValues.insert(first);
                return true;
            }
            else
                return false;
        }
    }

    if (F->isIntrinsic())
        return false;

    // 如果形参和对应实参有一个是敏感值，那么将另一个也设为敏感值
    while(fItr != F->arg_end() && aItr != CS->arg_end())
    {
        Argument *formal = const_cast<Argument *>(&(*fItr));
        Value *actual = *aItr;

        if(taintValues.find(formal) != taintValues.end())
            ret |= taintValues.insert(actual).second;
        else if(taintValues.find(actual) != taintValues.end())
            ret |= taintValues.insert(formal).second;

        ++fItr;
        ++aItr;
    }

    // 实参比形参多，说明使用了可变参数列表(va_list)来传参
    while(aItr != CS->arg_end())
    {
        Value *actual = *aItr;
        if (taintValues.find(actual) != taintValues.end())
            tiantVarArgs.insert(F);
        ++aItr;
    }

    return ret;
}

void COLLATEPass::dumpCrData(unordered_set<Value *> &content)
{
    int num = 0;
    auto getLoc = [](Value *V)
    {
        if(Instruction *I = dyn_cast<Instruction>(V))
        {
            if(DILocation *loc = I->getDebugLoc())
            {
                unsigned line = loc->getLine();
                StringRef file = loc->getFilename();
                StringRef dir = loc->getDirectory();
                string location = dir.str() + "/" + file.str();
                ifstream in; 
                string source;

                in.open(location);
                for(int i = 0;i < line; i++)
                    getline(in, source);
                in.close();
                return source + "(" + location + ":" + to_string(line) + ")";
            }
        }
        // else if(GlobalVariable *G = dyn_cast<GlobalVariable>(V))

        return string("");
    };

    for(auto it : content)
    {
        if(Instruction *I = dyn_cast<Instruction>(it))
        {
            errs() << num++ << ": " << getLoc(I) << "\n\t";
            errs() << *I << " in " << I->getFunction()->getName() << "\n";
        }
        else if (GlobalVariable *G = dyn_cast<GlobalVariable>(it))
        {
            errs() << num++ << ": " << *G << "\n";
        }
    }
}

void COLLATEPass::runPointerAnalysis(Module &M)
{
    SVFModule* svfModule = LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(M);
    svfModule->buildSymbolTableInfo();

    SVFIRBuilder builder;
    SVFIR *pag = builder.build(svfModule);
    
    DDAClient* client = new DDAClient(svfModule);
    client->initialise(svfModule);
    
    ContextCond::setMaxPathLen(100000);
    ContextCond::setMaxCxtLen(3);

    pta = new ContextDDA(pag, client);
    pta->initialize();
    client->answerQueries(pta);
    pta->finalize();
}

void COLLATEPass::getPointsToSet(Value *val, vector<Value*> &result)
{
    if(!(isa<Instruction>(val) || isa<GlobalVariable>(val)))
        return;

    if(!pta->getPAG()->hasValueNode(val))
        return;

    NodeID pNodeId = pta->getPAG()->getValueNode(val);
    const PointsTo& pts = pta->getPts(pNodeId);
    for (PointsTo::iterator ii = pts.begin(), ie = pts.end(); ii != ie; ii++)
    {
        PAGNode* targetObj = pta->getPAG()->getGNode(*ii);
        if(targetObj && !isa<DummyValVar>(targetObj) && 
            !isa<DummyObjVar>(targetObj) && targetObj->hasValue())
        {
            Value* memObj = const_cast<Value*>(targetObj->getValue());

            // 可能出现非指针指向函数，原因不明
            if(!isa<Function>(memObj))
                result.push_back(memObj);
        }
    }
}

void COLLATEPass::getMemOfCrData(unordered_set<Value *> &values, unordered_set<Value *> &mems)
{
    for(auto it : values)
    {
        if(LoadInst *lI = dyn_cast<LoadInst>(it))
        {
            vector<Value *> pts;
            Value *pointer = lI->getPointerOperand();
            getPointsToSet(pointer, pts);
            for(auto t : pts)
            {
                if(!mems.count(t))
                    mems.insert(t);
            }
        }
        else if(AllocaInst *aI = dyn_cast<AllocaInst>(it))
        {
            mems.insert(aI);
        }
        else if(GlobalVariable *gV = dyn_cast<GlobalVariable>(it))
        {
            mems.insert(gV);
        }
    }
}

void COLLATEPass::instrumentTrustedInstructions(Module &M, unordered_set<Value *> &protectedMems)
{
    // 定义A的情况下，会在所有store和库函数调用后面插入close_gate
    // 在后面识别访问safe_region的指令时不处理函数调用指令。同时在main中插入init_handler，设置
    // sigfault的handler，在里面会调用open_gate，这样当没有识别并授权的指令访问safe region时，
    // 会引发段错误，然后回调handler，handler中使用open_gate允许访问，从而使访问能够正常进行，
    // 访问结束后，close_gate禁止访问，从而不影响下一条指令的处理。
    // #define A
    #ifdef A
    // 在所有store指令后插入close_gate，因为在sigfault的handler里会open_gate，
    // 为了防止open后就一直运行访问safe region，需要进行关闭
    // 在库函数调用指令前后插入open_gate和close_gate，暂时不管它
    for (auto &F : M)
    {
        if (F.isDeclaration() || F.isIntrinsic())
            continue;
        
        IRBuilder<> IRB(&*F.getEntryBlock().getFirstInsertionPt());

        if(F.getName() == "main")
        {
            FunctionCallee initHandler = M.getOrInsertFunction("init_handler", Type::getVoidTy(M.getContext()));
            IRB.CreateCall(initHandler); // 插入init_handler，初始化sigfault处理函数
        }

        for(inst_iterator I = inst_begin(F), E = inst_end(F); I != E; I++)
        {
            if (isa<LandingPadInst>(*I))
                continue;
            findConstantExpr(&(*I));
        }
    }
}

bool COLLATEPass::runOnModule(Module &M)
{
    constantExpr2Instruction(M);
    analyzeStructTypeEquality(M);

    unordered_set<Value *> taintSource;
    identifyTaintSources(M, taintSource);

    unordered_set<Value *> controlRelatedData;
    taintPropagation(M, taintSource, controlRelatedData);

    dumpCrData(controlRelatedData);

    runPointerAnalysis(M);

    unordered_set<Value *> memOfCrData;
    getMemOfCrData(controlRelatedData, memOfCrData);
    return true;
}

char COLLATEPass::ID = 0;
static RegisterPass<COLLATEPass> X("COLLATE", "COLLATE instrumentation pass");

static void registerMyPass(const PassManagerBuilder &PMB,
                           legacy::PassManagerBase &PM)
{
    PM.add(new COLLATEPass());
}

static RegisterStandardPasses RegisterMyPass(PassManagerBuilder::EP_OptimizerLast, registerMyPass);
static RegisterStandardPasses RegisterMyPass2(PassManagerBuilder::EP_EnabledOnOptLevel0, registerMyPass);