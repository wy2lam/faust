/************************************************************************
 ************************************************************************
    FAUST compiler
    Copyright (C) 2003-2018 GRAME, Centre National de Creation Musicale
    ---------------------------------------------------------------------
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 ************************************************************************
 ************************************************************************/

#ifndef _FUNCTION_BUILDER_H
#define _FUNCTION_BUILDER_H

#include <string.h>
#include <algorithm>
#include <iostream>
#include <list>
#include <map>
#include <sstream>
#include <stack>
#include <string>

#include "exception.hh"
#include "global.hh"
#include "instructions.hh"

/*
    void compute(int count, float** inputs, float** ouputs)
    {
        int toto = ....  (local var outside the loop)

        loop (....count....)
        {
            toto: use of var outside the loop

            field: kStruct variable

            float titi = ....  (local var inside the loop)
            loop_code
        }
    }

    ==> local var outside the loop : function parameter
    ==> var insided the loop : stay the same
    ==> "count" of the loop : function parameter
    ==> field: kStruct variable : stay the same
    ==> global variables : stay the same

    void extracted_loop(int toto, int count, .....)
    {
        loop (....count....)
        {
            toto: use of var from paramater list

            field: kStruct variable

            float titi = ....  (local var inside the loop)
            loop_code
        }
    }

    void extracted_loop(int count, float** inputs, float** ouputs)
    {
        call_loop(toto, count, ....)
    }

*/

struct Loop2FunctionBuider : public DispatchVisitor {
    // Variable management
    std::map<std::string, Address::AccessType> fLocalVarTable;
    std::list<std::string>                     fAddedVarTable;

    // Function definition creation
    Names           fArgsTypeList;
    DeclareFunInst* fFunctionDef;

    // Function call creation
    Values    fArgsValueList;
    DropInst* fFunctionCall;

    void createParameter(Address* address)
    {
        switch (address->getAccess()) {
            case Address::kStack:
            case Address::kLoop: {
                std::string name = address->getName();
                if (fLocalVarTable.find(name) == fLocalVarTable.end()) {
                    if (find(fAddedVarTable.begin(), fAddedVarTable.end(), name) ==
                        fAddedVarTable.end()) {  // First encounter

                        // Be sure variable is defined
                        // cerr << "createParameter kStack " << name << endl;
                        faustassert(gGlobal->gVarTypeTable.find(name) !=
                                    gGlobal->gVarTypeTable.end());

                        // Local in the enclosing context, becomes a fun parameter
                        BasicCloneVisitor cloner;
                        fArgsTypeList.push_back(
                            IB::genNamedTyped(name, gGlobal->gVarTypeTable[name]->clone(&cloner)));

                        // It becomes a value in the fun-call argument list
                        fArgsValueList.push_back(IB::genLoadStackVar(name));

                        // Variable added in parameter list
                        fAddedVarTable.push_back(name);
                    }

                } else {
                    // Loop own local, nothing to do
                }
                break;
            }

            case Address::kFunArgs: {
                std::string name = address->getName();
                if (find(fAddedVarTable.begin(), fAddedVarTable.end(), name) ==
                    fAddedVarTable.end()) {  // First encounter

                    // Be sure variable is defined
                    // cerr << "createParameter kFunArgs " << name << endl;
                    faustassert(gGlobal->gVarTypeTable.find(name) != gGlobal->gVarTypeTable.end());

                    // Parameter in the enclosing function, becomes a fun parameter
                    BasicCloneVisitor cloner;
                    fArgsTypeList.push_back(
                        IB::genNamedTyped(name, gGlobal->gVarTypeTable[name]->clone(&cloner)));

                    // It becomes a value in the fun-call argument list : keep it's kFunArgs status
                    fArgsValueList.push_back(IB::genLoadFunArgsVar(name));

                    // Variable added in parameter list
                    fAddedVarTable.push_back(name);
                }
                break;
            }

            case Address::kStruct:
            case Address::kStaticStruct:
            case Address::kGlobal:
                // Nothing to do
                break;

            case Address::kLink:
                faustassert(false);
                break;

            default:
                break;
        }
    }

    virtual void visit(DeclareVarInst* inst)
    {
        DispatchVisitor::visit(inst);
        Address::AccessType access = inst->getAccess();

        if (access == Address::kStack || access == Address::kLoop) {
            // Keep local variables in the loop
            fLocalVarTable[inst->getName()] = access;
        }
    }

    virtual void visit(LoadVarInst* inst)
    {
        DispatchVisitor::visit(inst);
        createParameter(inst->fAddress);
    }

    virtual void visit(LoadVarAddressInst* inst) {}

    virtual void visit(StoreVarInst* inst)
    {
        DispatchVisitor::visit(inst);
        createParameter(inst->fAddress);
    }

    Loop2FunctionBuider(const std::string& fun_name, BlockInst* block, bool add_object = false)
    {
        // This prepare fArgsTypeList and fArgsValueList
        block->accept(this);

        // Change the status of all variables used in function parameter list
        struct LoopCloneVisitor : public BasicCloneVisitor {
            std::list<std::string>& fAddedVarTable;

            LoopCloneVisitor(std::list<std::string>& table) : fAddedVarTable(table) {}

            virtual Address* visit(NamedAddress* address)
            {
                if (find(fAddedVarTable.begin(), fAddedVarTable.end(), address->fName) !=
                    fAddedVarTable.end()) {
                    return IB::genNamedAddress(address->fName, Address::kFunArgs);
                } else {
                    return BasicCloneVisitor::visit(address);
                }
            }
        };

        // Put loop in new function
        LoopCloneVisitor cloner(fAddedVarTable);
        BlockInst*       function_code = static_cast<BlockInst*>(block->clone(&cloner));

        // Add a Ret (void) instruction (needed in LLVM backend)
        function_code->pushBackInst(IB::genRetInst());

        // Add "dsp" arg in function prototype and in parameter list
        if (add_object) {
            fArgsTypeList.push_front(IB::genNamedTyped("dsp", IB::genBasicTyped(Typed::kObj_ptr)));
            fArgsValueList.push_front(IB::genLoadFunArgsVar("dsp"));
        }

        // Create function
        fFunctionDef = IB::genVoidFunction(fun_name, fArgsTypeList, function_code);

        // Create function call
        fFunctionCall = IB::genDropInst(IB::genFunCallInst(fun_name, fArgsValueList));
    }
};

/*
Constant propagation:

1) change variables to constants in the initial code
2) clone the code with ConstantPropagationCloneVisitor
*/

struct ConstantPropagationBuilder : public BasicCloneVisitor {
    std::map<std::string, ValueInst*> fValueTable;

    virtual ValueInst* visit(BinopInst* inst)
    {
        ValueInst* val1 = inst->fInst1->clone(this);
        ValueInst* val2 = inst->fInst2->clone(this);

        FloatNumInst* float1 = dynamic_cast<FloatNumInst*>(val1);
        FloatNumInst* float2 = dynamic_cast<FloatNumInst*>(val2);

        // TODO
        Int32NumInst* int1 = dynamic_cast<Int32NumInst*>(val1);
        Int32NumInst* int2 = dynamic_cast<Int32NumInst*>(val2);

        // if (float1) float1->dump();
        // if (float2) float2->dump();

        if (float1 && float2) {
            switch (inst->fOpcode) {
                case kAdd:
                    return IB::genFloatNumInst(float1->fNum + float2->fNum);
                case kSub:
                    return IB::genFloatNumInst(float1->fNum - float2->fNum);
                case kMul:
                    return IB::genFloatNumInst(float1->fNum * float2->fNum);
                case kDiv:
                    return IB::genFloatNumInst(float1->fNum / float2->fNum);
                default:
                    return 0;
            }

        } else if (int1 && int2) {
            faustassert(false);
            return 0;
        } else {
            return IB::genBinopInst(inst->fOpcode, val1, val2);
        }
    }

    virtual ValueInst* visit(CastInst* inst)
    {
        ValueInst*    val1   = inst->fInst->clone(this);
        FloatNumInst* float1 = dynamic_cast<FloatNumInst*>(val1);
        Int32NumInst* int1   = dynamic_cast<Int32NumInst*>(val1);

        if (inst->fType->getType() == Typed::kFloat) {
            return (float1) ? float1 : IB::genFloatNumInst(float(int1->fNum));
        } else if (inst->fType->getType() == Typed::kInt32) {
            return (int1) ? int1 : IB::genInt32NumInst(int(float1->fNum));
        } else {
            faustassert(false);
            return 0;
        }
    }

    virtual ValueInst* visit(FunCallInst* inst)
    {
        Values cloned;
        for (const auto& it : inst->fArgs) {
            cloned.push_back(it->clone(this));
        }
        // TODO : si toute la liste des values sont des nombres, alors effectuer le calcul
        return IB::genFunCallInst(inst->fName, cloned, inst->fMethod);
    }

    virtual ValueInst* visit(Select2Inst* inst)
    {
        ValueInst*    val1   = inst->fCond->clone(this);
        FloatNumInst* float1 = dynamic_cast<FloatNumInst*>(val1);
        Int32NumInst* int1   = dynamic_cast<Int32NumInst*>(val1);

        if (float1) {
            return (float1->fNum > 0.f) ? inst->fThen->clone(this) : inst->fElse->clone(this);
        } else if (int1) {
            return (int1->fNum > 0) ? inst->fThen->clone(this) : inst->fElse->clone(this);
        } else {
            return IB::genSelect2Inst(val1, inst->fThen->clone(this), inst->fElse->clone(this));
        }
    }

    virtual StatementInst* visit(DeclareVarInst* inst)
    {
        ValueInst*    val1   = inst->fValue->clone(this);
        FloatNumInst* float1 = dynamic_cast<FloatNumInst*>(val1);
        Int32NumInst* int1   = dynamic_cast<Int32NumInst*>(val1);
        std::string   name   = inst->getName();

        if (float1) {
            // float1->dump();
            // Creates a "link" so that corresponding load see the real value
            fValueTable[name] = float1;
            return IB::genDropInst();
        } else if (int1) {
            // Creates a "link" so that corresponding load see the real value
            fValueTable[name] = int1;
            return IB::genDropInst();
        } else {
            BasicCloneVisitor cloner;
            return IB::genDeclareVarInst(inst->fAddress->clone(&cloner),
                                         inst->fType->clone(&cloner), val1);
        }
    }

    virtual ValueInst* visit(LoadVarInst* inst)
    {
        std::string name = inst->getName();
        if (fValueTable.find(name) != fValueTable.end()) {
            return fValueTable[name];
        } else {
            BasicCloneVisitor cloner;
            return IB::genLoadVarInst(inst->fAddress->clone(&cloner));
        }
    }

    virtual StatementInst* visit(StoreVarInst* inst)
    {
        ValueInst*    val1   = inst->fValue->clone(this);
        FloatNumInst* float1 = dynamic_cast<FloatNumInst*>(val1);
        Int32NumInst* int1   = dynamic_cast<Int32NumInst*>(val1);
        std::string   name   = inst->getName();

        if (float1) {
            // float1->dump();
            // Creates a "link" so that corresponding load see the real value
            fValueTable[name] = float1;
            return IB::genDropInst();
        } else if (int1) {
            // Creates a "link" so that corresponding load see the real value
            fValueTable[name] = int1;
            return IB::genDropInst();
        } else {
            BasicCloneVisitor cloner;
            return IB::genStoreVarInst(inst->fAddress->clone(&cloner), val1);
        }
    }
};

#endif
