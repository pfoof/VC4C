/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "Helper.h"

#include "CompilationError.h"
#include "TypeConversions.h"
#include "config.h"
#include "log.h"
#include "operators.h"

#include <algorithm>

using namespace vc4c;
using namespace vc4c::intermediate;
using namespace vc4c::operators;

InstructionWalker intermediate::insertMakePositive(
    InstructionWalker it, Method& method, const Value& src, Value& dest, Value& writeIsNegative)
{
    if(auto lit = src.getLiteralValue())
    {
        bool isNegative = lit->signedInt() < 0;
        dest = isNegative ? Value(Literal(-lit->signedInt()), src.type) : src;
        writeIsNegative = isNegative ? INT_MINUS_ONE : INT_ZERO;
    }
    else if(auto vector = src.checkVector())
    {
        SIMDVector tmpDest;
        SIMDVector tmpNegative;
        for(unsigned i = 0; i < vector->size(); ++i)
        {
            auto elem = (*vector)[i];
            bool isNegative = elem.signedInt() < 0;
            tmpDest[i] = isNegative ? Literal(-elem.signedInt()) : elem;
            tmpNegative[i] = isNegative ? Literal(-1) : Literal(0u);
        }
        dest = Value(std::move(tmpDest), src.type);
        writeIsNegative = Value(std::move(tmpNegative), src.type);
    }
    else if(src.getSingleWriter() != nullptr &&
        src.getSingleWriter()->hasDecoration(InstructionDecorations::UNSIGNED_RESULT))
    {
        // the value is already unsigned
        dest = src;
        writeIsNegative = INT_ZERO;
    }
    else
    {
        /*
         * Calculation of positive value:
         * %sign = asr %src, 31 -> -1 for negative, 0 for positive numbers
         * %tmp = xor %src, %sign
         * %unsigned = sub %tmp, %sign
         *
         * For positive:
         * %sign = 0
         * %tmp = %src
         * %unsigned = sub %src, 0 -> %src
         *
         * For negative:
         * %sign = -1
         * %tmp = ~%src
         * %unsigned = ~%src, -1 -> ~%src + 1 -> two's complement
         *
         * Source:
         * https://llvm.org/doxygen/IntegerDivision_8cpp_source.html
         */

        //%sign = asr %src, 31 -> -1 for negative, 0 for positive numbers
        Value srcInt = src;
        if(src.type.getScalarBitCount() < 32)
        {
            // to make sure, the leading bits are set
            srcInt = method.addNewLocal(TYPE_INT32.toVectorType(src.type.getVectorWidth()), "%sext");
            it = insertSignExtension(it, method, src, srcInt, true);
        }
        if(!writeIsNegative.checkLocal())
            writeIsNegative = method.addNewLocal(TYPE_INT32.toVectorType(src.type.getVectorWidth()), "%sign");
        it.emplace(new Operation(OP_ASR, writeIsNegative, srcInt, Value(Literal(31u), TYPE_INT8)));
        it.nextInBlock();
        //%tmp = xor %src, %sign
        Value tmp = assign(it, src.type, "%twos_complement") = srcInt ^ writeIsNegative;
        //%unsigned = sub %tmp, %sign
        if(!dest.isWriteable())
            dest = method.addNewLocal(src.type, "%unsigned");
        assign(it, dest) = (tmp - writeIsNegative, InstructionDecorations::UNSIGNED_RESULT);
    }
    return it;
}

InstructionWalker intermediate::insertRestoreSign(
    InstructionWalker it, Method& method, const Value& src, Value& dest, const Value& sign)
{
    if(src.getLiteralValue() && sign.getLiteralValue())
    {
        dest = sign.isZeroInitializer() ? src : Value(Literal(-src.literal().signedInt()), src.type);
    }
    else
    {
        /*
         * Calculation of signed value:
         *
         * %tmp = xor %src, %sign
         * %dest = sub %tmp, %sign
         *
         * To restore positive value (%sign = 0):
         * %tmp = %src
         * %dest = sub %src, 0 -> %src
         *
         * To restore negative value (%sign = -1):
         * %tmp = ~%src
         * %dest = sub ~%src, -1 -> ~%src + 1 -> tow's complement
         *
         * Source:
         * https://llvm.org/doxygen/IntegerDivision_8cpp_source.html
         */

        //%tmp = xor %src, %sign
        Value tmp = assign(it, src.type, "%twos_complement") = src ^ sign;
        //%dest = sub %tmp, %sign
        if(!dest.isWriteable())
            dest = method.addNewLocal(src.type, "%twos_complement");
        assign(it, dest) = tmp - sign;
    }
    return it;
}

InstructionWalker intermediate::insertCalculateIndices(InstructionWalker it, Method& method, const Value& container,
    const Value& dest, const std::vector<Value>& indices, const bool firstIndexIsElement)
{
    // handle multi-level indices
    Value offset = INT_ZERO;
    DataType subContainerType = container.type;
    for(const Value& index : indices)
    {
        Value subOffset(UNDEFINED_VALUE);
        if(subContainerType.getPointerType() || subContainerType.getArrayType())
        {
            // index is index in pointer/array
            //-> add offset of element at given index to global offset
            if(auto lit = index.getLiteralValue())
            {
                subOffset = Value(Literal(lit->signedInt() *
                                      static_cast<int32_t>(subContainerType.getElementType().getPhysicalWidth())),
                    TYPE_INT32);
            }
            else
            {
                subOffset = method.addNewLocal(TYPE_INT32, "%index_offset");
                it.emplace(new intermediate::IntrinsicOperation("mul", Value(subOffset), Value(index),
                    Value(Literal(subContainerType.getElementType().getPhysicalWidth()), TYPE_INT32)));
                it.nextInBlock();
            }

            // according to SPIR-V 1.2 specification, the type doesn't change if the first index is the "element":
            //"The type of Base after being dereferenced with Element is still the same as the original type of Base."
            if(!firstIndexIsElement || &index != &indices.front())
                subContainerType = subContainerType.getElementType();
        }
        else if(auto structType = subContainerType.getStructType())
        {
            // index is element in struct -> MUST be literal
            if(!index.getLiteralValue())
                throw CompilationError(CompilationStep::LLVM_2_IR, "Can't access struct-element with non-literal index",
                    index.to_string());

            subOffset = Value(Literal(structType->getStructSize(index.getLiteralValue()->signedInt())), TYPE_INT32);
            subContainerType = subContainerType.getElementType(index.getLiteralValue()->signedInt());
        }
        else if(subContainerType.isVectorType())
        {
            // takes the address of an element of the vector
            if(auto lit = index.getLiteralValue())
                subOffset = Value(Literal(lit->signedInt() *
                                      static_cast<int32_t>(subContainerType.getElementType().getPhysicalWidth())),
                    TYPE_INT32);
            else
                subOffset = assign(it, TYPE_INT32, "%vector_element_offset") =
                    index * Literal(subContainerType.getElementType().getPhysicalWidth());
            subContainerType = subContainerType.getElementType();
        }
        else
            throw CompilationError(CompilationStep::LLVM_2_IR, "Invalid container-type to retrieve element via index",
                subContainerType.to_string());

        if(offset.getLiteralValue() && subOffset.getLiteralValue())
        {
            offset = Value(
                Literal(offset.getLiteralValue()->signedInt() + subOffset.getLiteralValue()->signedInt()), TYPE_INT32);
        }
        else if(offset.isZeroInitializer())
        {
            // previous offset is zero -> zero + x = x
            offset = subOffset;
        }
        else if(subOffset.isZeroInitializer())
        {
            // sub-offset is zero -> x + zero = x
            // offset = offset -> do nothing
        }
        else
        {
            Value tmp = assign(it, TYPE_INT32, "%index_offset") = offset + subOffset;
            offset = tmp;
        }
    }
    // add last offset to container
    assign(it, dest) = container + offset;

    /*
     * associates the index with the local/parameter it refers to.
     * This is required, so the input/output-parameters are correctly recognized
     *
     * NOTE: The associated index can only be set, if there is a single literal index.
     * (Or the element is element 0, than the reference-index can be retrieved from the second index)
     */
    Value index = UNDEFINED_VALUE;
    if(indices.size() == 1)
        index = indices[0];
    if(firstIndexIsElement && indices.at(0).isZeroInitializer())
        index = indices.size() > 1 ? indices.at(1) : UNDEFINED_VALUE;
    const int refIndex = index.getLiteralValue().value_or(Literal(ANY_ELEMENT)).signedInt();
    const_cast<std::pair<Local*, int>&>(dest.local()->reference) = std::make_pair(container.local(), refIndex);

    DataType finalType = subContainerType;
    if(auto arrayType = subContainerType.getArrayType())
        // convert x[num] to x*
        // TODO shouldn't x[num] be converted to x[num]* ?? (e.g. for HandBrake/vscale_all_dither_opencl.cl)
        // or distinguish between first and following indices?
        finalType = method.createPointerType(arrayType->elementType,
            container.type.getPointerType() ? container.type.getPointerType()->addressSpace : AddressSpace::PRIVATE);
    else if(!(firstIndexIsElement && indices.size() == 1))
        finalType = method.createPointerType(subContainerType, container.type.getPointerType()->addressSpace);

    if(dest.type != finalType)
    {
        logging::error() << "Final index does not match expected type for source " << container.to_string()
                         << ", destination " << dest.to_string() << ", final index type " << finalType.to_string()
                         << " and indices: " << to_string<Value>(indices)
                         << (firstIndexIsElement ? " (first index is element)" : "") << logging::endl;
        throw CompilationError(
            CompilationStep::LLVM_2_IR, "Types of retrieving indices do not match!", finalType.to_string());
    }

    return it;
}

InstructionWalker intermediate::insertByteSwap(
    InstructionWalker it, Method& method, const Value& src, const Value& dest)
{
    /*
     * llvm.bswap:
     * "The llvm.bswap.i16 intrinsic returns an i16 value that has the high and low byte of the input i16 swapped.
     * Similarly, the llvm.bswap.i32 intrinsic returns an i32 value that has the four bytes of the input i32 swapped,
     * so that if the input bytes are numbered 0, 1, 2, 3 then the returned i32 will have its bytes in 3, 2, 1, 0 order.
     * "
     */
    auto numBytes = src.type.getScalarBitCount() / 8;

    if(numBytes == 2)
    {
        // TODO shorts lose signedness!

        // ? ? A B -> 0 ? ? A
        Value tmpA0 = assign(it, src.type, "byte_swap") = src >> 8_val;
        // ? ? A B -> ? A B 0
        Value tmpB0 = assign(it, src.type, "byte_swap") = src << 8_val;
        // 0 ? ? A -> 0 0 0 A
        Value tmpA1 = assign(it, src.type, "byte_swap") = tmpA0 & 0x000000FF_val;
        // ? A B 0 -> 0 0 B 0
        Value tmpB1 = assign(it, src.type, "byte_swap") = tmpB0 & 0x0000FF00_val;
        // 0 0 0 A | 0 0 B 0 -> 0 0 A B
        assign(it, dest) = tmpA1 | tmpB1;
    }
    else if(numBytes == 4)
    {
        // A B C D -> B C D A
        const Value tmpAC0 = method.addNewLocal(src.type, "byte_swap");
        it.emplace(new Operation(OP_ROR, tmpAC0, src, Value(Literal(24u), TYPE_INT8)));
        it.nextInBlock();
        // A B C D -> D A B C
        const Value tmpBD0 = method.addNewLocal(src.type, "byte_swap");
        it.emplace(new Operation(OP_ROR, tmpBD0, src, Value(Literal(16u), TYPE_INT8)));
        it.nextInBlock();
        // B C D A -> 0 0 0 A
        Value tmpA1 = assign(it, src.type, "byte_swap") = tmpAC0 & 0x000000FF_val;
        // D A B C -> 0 0 B 0
        Value tmpB1 = assign(it, src.type, "byte_swap") = tmpBD0 & 0x0000FF00_val;
        // B C D A -> 0 C 0 0
        Value tmpC1 = assign(it, src.type, "byte_swap") = tmpAC0 & 0x00FF0000_val;
        // D A B C -> D 0 0 0
        Value tmpD1 = assign(it, src.type, "byte_swap") = tmpBD0 & 0xFF000000_val;
        // 0 0 0 A | 0 0 B 0 -> 0 0 B A
        Value tmpAB2 = assign(it, src.type, "byte_swap") = tmpA1 | tmpB1;
        // 0 C 0 0 | D 0 0 0 -> D C 0 0
        Value tmpCD2 = assign(it, src.type, "byte_swap") = tmpC1 | tmpD1;
        // 0 0 B A | D C 0 0 -> D C B A
        assign(it, dest) = tmpAB2 | tmpCD2;
    }
    else
        throw CompilationError(
            CompilationStep::GENERAL, "Invalid number of bytes for byte-swap", std::to_string(numBytes));

    return it;
}
