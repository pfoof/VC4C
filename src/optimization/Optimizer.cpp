/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "Optimizer.h"

#include "../Module.h"
#include "../Profiler.h"
#include "../ThreadPool.h"
#include "../intrinsics/Intrinsics.h"
#include "Combiner.h"
#include "ControlFlow.h"
#include "Eliminator.h"
#include "Flags.h"
#include "InstructionScheduler.h"
#include "LocalCompression.h"
#include "Reordering.h"
#include "log.h"

using namespace vc4c;
using namespace vc4c::optimizations;

OptimizationPass::OptimizationPass(const std::string& name, const std::string& parameterName, const Pass& pass,
    const std::string& description, OptimizationType type) :
    name(name),
    parameterName(parameterName), description(description), type(type), pass(pass)
{
}

bool OptimizationPass::operator()(const Module& module, Method& method, const Configuration& config) const
{
    return pass(module, method, config);
}

OptimizationStep::OptimizationStep(const std::string& name, const Step& step) : name(name), step(step) {}

InstructionWalker OptimizationStep::operator()(
    const Module& module, Method& method, InstructionWalker it, const Configuration& config) const
{
    return step(module, method, it, config);
}

static const std::vector<OptimizationStep> SINGLE_STEPS = {
    // combine consecutive instructions writing the same local with a value and zero depending on some flags
    OptimizationStep("CombineSelectionWithZero", combineSelectionWithZero),
    // combine successive setting of the same flags
    OptimizationStep("CombineSettingSameFlags", combineSameFlags),
    // combine writing of value to set flags with writing of same value into output
    OptimizationStep("CombineSettingFlagsWithOutput", combineFlagWithOutput),
    // calculates constant operations
    OptimizationStep("FoldConstants", foldConstants),
    // simplifies arithmetic operations into moves or into "easier" operations
    OptimizationStep("SimplifyArithmetics", simplifyOperation),
    // combines operations according to arithmetic rules
    OptimizationStep("CombineArithmetics", combineArithmeticOperations),
    // removes calls to SFU registers with constant input
    OptimizationStep("RewriteConstantSFU", rewriteConstantSFUCall)};

static bool runSingleSteps(const Module& module, Method& method, const Configuration& config)
{
    LCOV_EXCL_START
    logging::logLazy(logging::Level::DEBUG, [&](std::wostream& log) {
        log << "Running steps: ";
        for(const OptimizationStep& step : SINGLE_STEPS)
            log << step.name << ", ";
        log << logging::endl;
    });
    LCOV_EXCL_STOP

    // since an optimization-step can be run on the result of the previous step,
    // we can't just pass the resulting iterator (pointing behind the optimization result) into the next
    // optimization-step  but since lists do not reallocate elements at inserting/removing, we can re-use the previous
    // iterator
    auto it = method.walkAllInstructions();
    // this construct with previous iterator is required, because the iterator could be invalidated (if the underlying
    // node is removed)
    auto prevIt = it;
    while(!it.isEndOfMethod())
    {
        for(const OptimizationStep& step : SINGLE_STEPS)
        {
            PROFILE_START_DYNAMIC(step.name);
            auto newIt = step(module, method, it, config);
            // we can't just test newIt == it here, since if we replace the content of the iterator instead of deleting
            // it, the iterators are still the same, even if we emplace instructions before
            if(newIt.copy().previousInMethod() != prevIt || newIt != it)
                it = prevIt;
            PROFILE_END_DYNAMIC(step.name);
        }
        it.nextInMethod();
        prevIt = it.copy().previousInMethod();
    }

    // XXX
    return true;
}

static void addToPasses(const OptimizationPass& pass, std::vector<const OptimizationPass*>& initialPasses,
    std::vector<const OptimizationPass*>& repeatingPasses, std::vector<const OptimizationPass*>& finalPasses)
{
    switch(pass.type)
    {
    case OptimizationType::INITIAL:
        initialPasses.emplace_back(&pass);
        break;
    case OptimizationType::REPEAT:
        repeatingPasses.emplace_back(&pass);
        break;
    case OptimizationType::FINAL:
        finalPasses.emplace_back(&pass);
        break;
    default:
        throw CompilationError(CompilationStep::OPTIMIZER, "Unhandled optimization type for pass", pass.name);
    }
}

Optimizer::Optimizer(const Configuration& config) : config(config)
{
    auto enabledPasses = getPasses(config.optimizationLevel);
    for(const OptimizationPass& pass : ALL_PASSES)
    {
        if(config.additionalDisabledOptimizations.find(pass.parameterName) !=
            config.additionalDisabledOptimizations.end())
            continue;
        if(config.additionalEnabledOptimizations.find(pass.parameterName) !=
            config.additionalEnabledOptimizations.end())
            addToPasses(pass, initialPasses, repeatingPasses, finalPasses);
        // don't add a pass twice if it is manually enabled and in the list of enabled passed via the optimization level
        else if(enabledPasses.find(pass.parameterName) != enabledPasses.end())
            addToPasses(pass, initialPasses, repeatingPasses, finalPasses);
    }
}

static bool runPass(
    const OptimizationPass& pass, std::size_t index, const Module& module, Method& method, const Configuration& config)
{
    logging::logLazy(logging::Level::DEBUG, [&]() {
        logging::debug() << logging::endl;
        logging::debug() << "Running pass: " << pass.name << logging::endl;
    });
    PROFILE_COUNTER(vc4c::profiler::COUNTER_OPTIMIZATION + index, pass.name + " (before)", method.countInstructions());
    PROFILE_START_DYNAMIC(pass.name);
    bool changedMethod = (pass)(module, method, config);
    PROFILE_END_DYNAMIC(pass.name);
    PROFILE_COUNTER_WITH_PREV(vc4c::profiler::COUNTER_OPTIMIZATION + index + 10, pass.name + " (after)",
        method.countInstructions(), vc4c::profiler::COUNTER_OPTIMIZATION + index);
    return changedMethod;
}

static void runOptimizationPasses(const Module& module, Method& method, const Configuration& config,
    const std::vector<const OptimizationPass*>& initialPasses,
    const std::vector<const OptimizationPass*>& repeatingPasses,
    const std::vector<const OptimizationPass*>& finalPasses)
{
    CPPLOG_LAZY(logging::Level::DEBUG, log << "-----" << logging::endl);
    CPPLOG_LAZY(logging::Level::INFO, log << "Running optimization passes for: " << method.name << logging::endl);
    std::size_t numInstructions = method.countInstructions();

    std::size_t index = 0;
    for(const OptimizationPass* pass : initialPasses)
    {
        runPass(*pass, index, module, method, config);
        index += 100;
    }

    const OptimizationPass* lastChangingOptimization = nullptr;
    std::size_t startIndex = index;
    bool continueLoop = !repeatingPasses.empty();
    unsigned iterationsLeft = config.additionalOptions.maxOptimizationIterations;
    for(; continueLoop && iterationsLeft > 0; --iterationsLeft)
    {
        CPPLOG_LAZY(logging::Level::DEBUG,
            log << "Running optimization iteration "
                << (config.additionalOptions.maxOptimizationIterations - iterationsLeft) << "..." << logging::endl);
        index = startIndex;
        for(const OptimizationPass* pass : repeatingPasses)
        {
            if(lastChangingOptimization == pass)
            {
                // the last optimization that changed anything was this one, one iteration ago
                continueLoop = false;
                break;
            }
            if(runPass(*pass, index, module, method, config))
                lastChangingOptimization = pass;
            index += 100;
        }
    }
    index = startIndex + repeatingPasses.size() * 100;
    if(iterationsLeft == 0 && config.additionalOptions.maxOptimizationIterations > 0 &&
        config.optimizationLevel != OptimizationLevel::NONE)
        logging::warn()
            << "Stopped optimizing, because the iteration limit was reached."
            << " This indicates either an error in the optimizations or that there is more optimizations to be done!"
            << logging::endl;

    for(const OptimizationPass* pass : finalPasses)
    {
        runPass(*pass, index, module, method, config);
        index += 100;
    }

    LCOV_EXCL_START
    logging::logLazy(logging::Level::INFO, [&]() {
        logging::info() << logging::endl;
        if(numInstructions != method.countInstructions())
        {
            logging::info() << "Optimizations done in "
                            << (config.additionalOptions.maxOptimizationIterations - iterationsLeft - 1)
                            << " iterations, changed number of instructions from " << numInstructions << " to "
                            << method.countInstructions() << logging::endl;
        }
        else
        {
            logging::info() << "Optimizations done in "
                            << (config.additionalOptions.maxOptimizationIterations - iterationsLeft - 1)
                            << " iterations" << logging::endl;
        }
    });
    LCOV_EXCL_STOP
    PROFILE_COUNTER(vc4c::profiler::COUNTER_OPTIMIZATION + index, "OptimizationIterations",
        config.additionalOptions.maxOptimizationIterations - iterationsLeft - 1);
    CPPLOG_LAZY(logging::Level::DEBUG, log << "-----" << logging::endl);
    method.dumpInstructions();
}

void Optimizer::optimize(Module& module) const
{
    auto kernels = module.getKernels();
    const auto f = [&](Method* kernelFunc) {
        runOptimizationPasses(module, *kernelFunc, config, initialPasses, repeatingPasses, finalPasses);
    };
    ThreadPool{"Optimizer"}.scheduleAll<Method*>(kernels, f);
}

const std::vector<OptimizationPass> Optimizer::ALL_PASSES = {
    /*
     * The first optimizations run modify the control-flow of the method.
     * After this block of optimizations is run, the CFG of the method is stable (does not change anymore)
     */
    OptimizationPass("AddWorkGroupLoops", "loop-work-groups", addWorkGroupLoop,
        "merges all work-group executions into a single kernel execution", OptimizationType::INITIAL),
    OptimizationPass("ReorderBasicBlocks", "reorder-blocks", reorderBasicBlocks,
        "reorders basic blocks to eliminate as many explicit branches as possible", OptimizationType::INITIAL),
    OptimizationPass("SimplifyConditionalBlocks", "simplify-conditionals", simplifyConditionalBlocks,
        "simplifies selected if-else and switch-case blocks by replacing the control-flow with conditional execution",
        OptimizationType::INITIAL),
    OptimizationPass("SimplifyBranches", "simplify-branches", simplifyBranches,
        "combines successive branches to the same label and replaces unnecessary branches with fall-through",
        OptimizationType::INITIAL),
    OptimizationPass("MergeBasicBlocks", "merge-blocks", mergeAdjacentBasicBlocks,
        "merges adjacent basic blocks if there are no other conflicting transitions", OptimizationType::INITIAL),
    OptimizationPass("VectorizeLoops", "vectorize-loops", vectorizeLoops, "vectorizes supported types of loops",
        OptimizationType::INITIAL),
    /*
     * The second block executes optimizations only within a single basic block.
     * These optimizations may be executed in a loop until there are not more changes to the instructions
     */
    OptimizationPass("SingleSteps", "single-steps", runSingleSteps,
        "runs all the single-step optimizations. Combining them results in fewer iterations over the instructions",
        OptimizationType::REPEAT),
    OptimizationPass("CombineRotations", "combine-rotations", combineVectorRotations,
        "combines duplicate vector rotations, e.g. introduced by vector-shuffle into a single rotation",
        OptimizationType::REPEAT),
    OptimizationPass("EliminateMoves", "eliminate-moves", eliminateRedundantMoves,
        "Replaces moves with the operation producing their source", OptimizationType::REPEAT),
    // executed after eliminate-moves to not have to rewrite simple moves with the more complex expression rewrite
    OptimizationPass("CommonSubexpressionElimination", "eliminate-common-subexpressions", eliminateCommonSubexpressions,
        "eliminates repetitive calculations of common expressions by re-using previous results (WIP, slow)",
        OptimizationType::REPEAT),
    OptimizationPass("EliminateBitOperations", "eliminate-bit-operations", eliminateRedundantBitOp,
        "Rewrites redundant bit operations", OptimizationType::REPEAT),
    OptimizationPass("PropagateMoves", "copy-propagation", propagateMoves,
        "Replaces operands with their moved-from value", OptimizationType::REPEAT),
    OptimizationPass("RemoveFlags", "remove-unused-flags", removeUselessFlags,
        "rewrites and removes all flags with constant conditions", OptimizationType::REPEAT),
    OptimizationPass("EliminateDeadCode", "eliminate-dead-code", eliminateDeadCode,
        "eliminates dead code (move to same, redundant arithmetic operations, ...)", OptimizationType::REPEAT),
    /*
     * The third block of optimizations is executed once after all the other optimizations finished and
     * can therefore introduce instructions or constructs (e.g. combined instructions) not supported by
     * the other optimizations.
     */
    // XXX not enabled with any optimization level for now. TODO also move before repeated optimizations?
    OptimizationPass("CompressWorkGroupInfo", "compress-work-group-info", compressWorkGroupLocals,
        "compresses work-group info into single local", OptimizationType::FINAL),
    OptimizationPass("SplitReadAfterWrites", "split-read-write", splitReadAfterWrites,
        "splits read-after-writes (except if the local is used only very locally), so the reordering and "
        "register-allocation have an easier job",
        OptimizationType::FINAL),
    OptimizationPass("CombineConstantLoads", "combine-loads", combineLoadingConstants,
        "combines loadings of the same constant value within a small range of a basic block", OptimizationType::FINAL),
    OptimizationPass("RemoveConstantLoadInLoops", "extract-loads-from-loops", removeConstantLoadInLoops,
        "move constant loads in (nested) loops outside the loops", OptimizationType::FINAL),
    OptimizationPass("CacheAcrossWorkGroup", "work-group-cache", cacheWorkGroupDMAAccess,
        "finds memory access across the work-group which can be cached in VPM to combine the DMA operation (WIP)",
        OptimizationType::FINAL),
    OptimizationPass("InstructionScheduler", "schedule-instructions", reorderInstructions,
        "schedule instructions according to their dependencies within basic blocks (WIP, slow)",
        OptimizationType::FINAL),
    OptimizationPass("ReorderInstructions", "reorder", reorderWithinBasicBlocks,
        "re-order instructions to eliminate more NOPs and stall cycles", OptimizationType::FINAL),
    OptimizationPass("CombineALUIinstructions", "combine", combineOperations,
        "run peep-hole optimization to combine ALU-operations", OptimizationType::FINAL)};

std::set<std::string> Optimizer::getPasses(OptimizationLevel level)
{
    std::set<std::string> passes;
    switch(level)
    {
    case OptimizationLevel::FULL:
        passes.emplace("vectorize-loops");
        passes.emplace("extract-loads-from-loops");
        passes.emplace("schedule-instructions");
        passes.emplace("work-group-cache");
        // XXX move CSE to medium? Need to profile performance and re-check all emulation tests with CSE enabled
        passes.emplace("eliminate-common-subexpressions");
        // XXX if tested enough, move to full
        passes.emplace("simplify-conditionals");
        FALL_THROUGH
    case OptimizationLevel::MEDIUM:
        passes.emplace("merge-blocks");
        passes.emplace("combine-rotations");
        passes.emplace("eliminate-moves");
        passes.emplace("eliminate-bit-operations");
        passes.emplace("copy-propagation");
        passes.emplace("combine-loads");
        FALL_THROUGH
    case OptimizationLevel::BASIC:
        passes.emplace("reorder-blocks");
        passes.emplace("simplify-branches");
        passes.emplace("eliminate-dead-code");
        passes.emplace("single-steps");
        passes.emplace("reorder");
        passes.emplace("combine");
        passes.emplace("remove-unused-flags");
        passes.emplace("loop-work-groups");
        FALL_THROUGH
    case OptimizationLevel::NONE:
        // TODO this is not an optimization, more a normalization step.
        // Move out of optimizations/remove when instruction scheduling is implemented
        passes.emplace("split-read-write");
        FALL_THROUGH
    default:
        break;
    }

    return passes;
}
