/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "AddressCalculation.h"

#include "../InstructionWalker.h"
#include "../intermediate/operators.h"
#include "../periphery/VPM.h"
#include "log.h"

using namespace vc4c;
using namespace vc4c::intermediate;
using namespace vc4c::normalization;
using namespace vc4c::operators;

MemoryAccessType normalization::toMemoryAccessType(periphery::VPMUsage usage)
{
    switch(usage)
    {
    case periphery::VPMUsage::SCRATCH:
    case periphery::VPMUsage::LOCAL_MEMORY:
        return MemoryAccessType::VPM_SHARED_ACCESS;
    case periphery::VPMUsage::REGISTER_SPILLING:
    case periphery::VPMUsage::STACK:
        return MemoryAccessType::VPM_PER_QPU;
    }
    throw CompilationError(CompilationStep::NORMALIZER,
        "Unknown VPM usage type to map to memory type: ", std::to_string(static_cast<int>(usage)));
}

InstructionWalker normalization::insertAddressToOffset(InstructionWalker it, Method& method, Value& out,
    const Local* baseAddress, const MemoryInstruction* mem, const Value& ptrValue)
{
    auto indexOp = dynamic_cast<const Operation*>(ptrValue.getSingleWriter());
    if(!indexOp)
    {
        // for stores, the store itself is also a write instruction
        auto writers = ptrValue.local()->getUsers(LocalUse::Type::WRITER);
        if(writers.size() == 2 && writers.find(mem) != writers.end())
        {
            writers.erase(mem);
            indexOp = dynamic_cast<const Operation*>(*writers.begin());
        }
    }
    if(ptrValue.hasLocal(baseAddress))
    {
        // trivial case, the offset is zero
        out = INT_ZERO;
    }
    else if(indexOp && indexOp->readsLocal(baseAddress) && indexOp->op == OP_ADD)
    {
        // for simple version where the index is base address + offset, simple use the offset directly
        out = indexOp->getFirstArg().hasLocal(baseAddress) ? indexOp->getSecondArg().value() : indexOp->getFirstArg();
    }
    else
    {
        // for more complex versions, calculate offset by subtracting base address from result
        // address
        out = assign(it, baseAddress->type, "%pointer_diff") = ptrValue - baseAddress->createReference();
    }
    return it;
}

InstructionWalker normalization::insertAddressToStackOffset(InstructionWalker it, Method& method, Value& out,
    const Local* baseAddress, MemoryAccessType type, const MemoryInstruction* mem, const Value& ptrValue)
{
    Value tmpIndex = UNDEFINED_VALUE;
    it = insertAddressToOffset(it, method, tmpIndex, baseAddress, mem, ptrValue);
    if(type == MemoryAccessType::VPM_PER_QPU)
    {
        // size of one stack-frame in bytes
        auto stackByteSize = periphery::VPM::getVPMStorageType(baseAddress->type.getElementType()).getInMemoryWidth();
        // add offset of stack-frame
        Value stackOffset = method.addNewLocal(TYPE_VOID_POINTER, "%stack_offset");
        assign(it, stackOffset) = mul24(Value(Literal(stackByteSize), TYPE_INT16), Value(REG_QPU_NUMBER, TYPE_INT8));
        out = assign(it, TYPE_VOID_POINTER, "%stack_offset") = tmpIndex + stackOffset;
    }
    else
    {
        out = tmpIndex;
    }
    return it;
}

InstructionWalker normalization::insertAddressToElementOffset(InstructionWalker it, Method& method, Value& out,
    const Local* baseAddress, const Value& container, const MemoryInstruction* mem, const Value& ptrValue)
{
    Value tmpIndex = UNDEFINED_VALUE;
    it = insertAddressToOffset(it, method, tmpIndex, baseAddress, mem, ptrValue);
    // the index (as per index calculation) is in bytes, but we need index in elements, so divide by element size
    out = assign(it, TYPE_VOID_POINTER, "%element_offset") =
        tmpIndex / Literal(container.type.getElementType().getInMemoryWidth());
    return it;
}

static Optional<std::pair<Value, InstructionDecorations>> combineAdditions(
    Method& method, InstructionWalker referenceIt, FastMap<Value, InstructionDecorations>& addedValues)
{
    if(addedValues.empty())
        return {};
    Optional<std::pair<Value, InstructionDecorations>> prevResult;
    auto valIt = addedValues.begin();
    while(valIt != addedValues.end())
    {
        if(prevResult)
        {
            auto newFlags = intersect_flags(prevResult->second, valIt->second);
            auto newResult = assign(referenceIt, prevResult->first.type) = (prevResult->first + valIt->first, newFlags);
            prevResult = std::make_pair(newResult, newFlags);
        }
        else
            prevResult = std::make_pair(valIt->first, valIt->second);
        valIt = addedValues.erase(valIt);
    }
    return prevResult;
}

InstructionWalker normalization::insertAddressToWorkItemSpecificOffset(
    InstructionWalker it, Method& method, Value& out, analysis::MemoryAccessRange& range)
{
    if(range.constantOffset)
        throw CompilationError(CompilationStep::NORMALIZER,
            "Calculating work-item specific offset with constant part is not yet implemented", range.to_string());
    auto dynamicParts = combineAdditions(method, it, range.dynamicAddressParts);
    out = dynamicParts->first;
    if(range.typeSizeShift)
        out = assign(it, dynamicParts->first.type) =
            (dynamicParts->first << range.typeSizeShift->assertArgument(1), dynamicParts->second);
    return it;
}
