/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#ifndef VC4C_LOCAL_ANALYSIS
#define VC4C_LOCAL_ANALYSIS

#include "../BasicBlock.h"
#include "../InstructionWalker.h"

#include <functional>
#include <unordered_map>

namespace vc4c
{
    namespace analysis
    {
        enum class AnalysisDirection
        {
            FORWARD,
            BACKWARD
        };

        template <typename V>
        using DefaultLocalTransferFunction = std::function<V(const intermediate::IntermediateInstruction*, const V&)>;

        /*
         * Template for local analyses (within a single basic block) traversing the block to create the analysis
         * result
         *
         * Adapted from here: https://web.stanford.edu/class/archive/cs/cs143/cs143.1128/lectures/15/Slides15.pdf
         */
        template <AnalysisDirection D, typename V, typename F = DefaultLocalTransferFunction<V>>
        class LocalAnalysis
        {
        public:
            static constexpr AnalysisDirection Direction = D;
            using Values = V;
            using TransferFunction = F;

            /*
             * Analyses the given basic block and fills the internal result store
             *
             * NOTE: One instance of a LocalAnalysis can only analyze a single basic block!
             */
            void operator()(const BasicBlock& block)
            {
                if(Direction == AnalysisDirection::FORWARD)
                    analyzeForward(block);
                else
                    analyzeBackward(block);

                resultAtStart = &results.at(block.begin().get());
                resultAtEnd = &results.at(block.end().previousInBlock().get());
            }

            const Values& getResult(const intermediate::IntermediateInstruction* instr) const
            {
                return results.at(instr);
            }

            const Values& getStartResult() const
            {
                return *resultAtStart;
            }

            const Values& getEndResult() const
            {
                return *resultAtEnd;
            }

        protected:
            LocalAnalysis(TransferFunction&& transferFunction, Values&& initialValue = {}) :
                transferFunction(std::forward<TransferFunction>(transferFunction)),
                initialValue(std::forward<Values>(initialValue))
            {
            }

            const TransferFunction transferFunction;
            std::unordered_map<const intermediate::IntermediateInstruction*, Values> results;
            const Values initialValue;
            const Values* resultAtStart = nullptr;
            const Values* resultAtEnd = nullptr;

        private:
            void analyzeForward(const BasicBlock& block)
            {
                const auto* prevVal = &initialValue;
                for(auto it = block.begin(); !it.isEndOfBlock(); it.nextInBlock())
                {
                    auto pos = results.emplace(it.get(), std::forward<Values>(transferFunction(it.get(), *prevVal)));
                    prevVal = &(pos.first->second);
                }
            }

            void analyzeBackward(const BasicBlock& block)
            {
                const auto* prevVal = &initialValue;
                auto it = block.end();
                do
                {
                    it.previousInBlock();

                    auto pos = results.emplace(it.get(), std::forward<Values>(transferFunction(it.get(), *prevVal)));
                    prevVal = &(pos.first->second);
                } while(!it.isStartOfBlock());
            }
        };

        /*
         * The default template for a transfer function retrieving information about a basic block
         *
         * The first return value is the initial value (before the block executes), the second the final value (after
         * the block executes)
         */
        template <typename V>
        using DefaultGlobalTransferFunction = std::function<std::pair<V, V>(const BasicBlock&)>;

        /*
         * Template for global analyses
         *
         * A global analysis analyzes a basic block as a whole and retrieves information about pre- and post-conditions
         * of the single blocks
         *
         */
        template <typename V, typename F = DefaultGlobalTransferFunction<V>>
        class GlobalAnalysis
        {
        public:
            using Values = V;
            using TransferFunction = F;

            /*
             * Analyses the given method and fills the internal result store
             */
            void operator()(const Method& method)
            {
                for(const BasicBlock& block : method)
                {
                    results.emplace(&block, std::forward<Values, Values>(transferFunction(block)));
                }
            }

            const Values& getInitialResult(const BasicBlock& block) const
            {
                return results.at(&block).first;
            }

            const Values& getFinalResult(const BasicBlock& block) const
            {
                return results.at(&block).second;
            }

        protected:
            GlobalAnalysis(TransferFunction&& transferFunction) :
                transferFunction(std::forward<TransferFunction>(transferFunction))
            {
            }

            const TransferFunction transferFunction;
            std::unordered_map<const BasicBlock*, std::pair<Values, Values>> results;
        };
    } /* namespace analysis */
} /* namespace vc4c */

#endif /* VC4C_LOCAL_ANALYSIS */