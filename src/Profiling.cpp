#include <algorithm>
#include <map>
#include <string>
#include <limits>

#include "Profiling.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "Scope.h"
#include "Simplify.h"
#include "Target.h"
#include "Util.h"

namespace Halide {
namespace Internal {

using std::map;
using std::string;
using std::vector;

namespace {

template <typename K, typename V>
inline V get_value(const map <K, V>& m, const K& key) {
    typename map<K, V>::const_iterator it = m.find(key);
    if (it == m.end()) {
        return 0;
    } else {
        return it->second;
    }
}

}

class InjectProfiling : public IRMutator {
public:
    map<string, int> indices;   // maps from func name -> index in buffer.

    vector<int> stack; // What produce nodes are we currently inside of.

    string pipeline_name;

    InjectProfiling(const string& pipeline_name) : pipeline_name(pipeline_name) {
        indices["overhead"] = 0;
        stack.push_back(0);
    }

    map<int, Expr> func_stack_current; // map from func id -> current stack allocation
    map<int, Expr> func_stack_peak; // map from func id -> peak stack allocation

private:
    using IRMutator::visit;

    struct AllocSize {
        bool on_stack;
        Expr size;
    };

    Scope<AllocSize> func_alloc_sizes;

    // Strip down the tupple name, e.g. f.0 into f
    string normalize_name(const string& name) {
        vector<string> v = split_string(name, ".");
        internal_assert(v.size() > 0);
        return v[0];
    }

    int get_func_id(const string& name) {
        string norm_name = normalize_name(name);
        int idx = -1;
        map<string, int>::iterator iter = indices.find(norm_name);
        if (iter == indices.end()) {
            idx = (int)indices.size();
            indices[norm_name] = idx;
        } else {
            idx = iter->second;
        }
        return idx;
    }

    Expr compute_allocation_size(const vector<Expr> &extents,
                                 const Expr& condition,
                                 const Type& type,
                                 const std::string &name,
                                 bool& on_stack) {
        on_stack = true;
        int32_t constant_size = Allocate::constant_allocation_size(extents, name);
        if (constant_size > 0) {
            int64_t stack_bytes = constant_size * type.bytes();
            if (stack_bytes > ((int64_t(1) << 31) - 1)) { // Out of memory
                return 0;
            } else if (get_host_target().is_allocation_on_stack(stack_bytes)) { // Allocation on stack
                return Expr((int32_t)stack_bytes);
            }
        }

        // Check that the allocation is not scalar (if it were scalar
        // it would have constant size).
        internal_assert(extents.size() > 0);

        on_stack = false;
        Expr size = extents[0];
        for (size_t i = 1; i < extents.size(); i++) {
            size *= extents[i];
        }
        size = simplify(Select::make(condition, size * type.bytes(), 0));
        return size;
    }

    void visit(const Allocate *op) {
        int idx = get_func_id(op->name);

        vector<Expr> new_extents;
        bool all_extents_unmodified = true;
        for (size_t i = 0; i < op->extents.size(); i++) {
            new_extents.push_back(mutate(op->extents[i]));
            all_extents_unmodified &= new_extents[i].same_as(op->extents[i]);
        }
        Expr condition = mutate(op->condition);

        bool on_stack;
        Expr size = compute_allocation_size(new_extents, condition, op->type, op->name, on_stack);
        func_alloc_sizes.push(op->name, {on_stack, size});

        if (!is_zero(size) && on_stack) {
            func_stack_current[idx] = simplify(size + get_value(func_stack_current, idx));
            func_stack_peak[idx] = simplify(
                max(get_value(func_stack_peak, idx), get_value(func_stack_current, idx)));
            debug(1) << "  Allocation on stack: " << op->name << "(" << size << ") in pipeline " << pipeline_name
                     << "; current: " << func_stack_current[idx] << "; peak: " << func_stack_peak[idx] << "\n";
        }

        Stmt body = mutate(op->body);
        Expr new_expr;
        if (op->new_expr.defined()) {
            new_expr = mutate(op->new_expr);
        }
        if (all_extents_unmodified &&
            body.same_as(op->body) &&
            condition.same_as(op->condition) &&
            new_expr.same_as(op->new_expr)) {
            stmt = op;
        } else {
            stmt = Allocate::make(op->name, op->type, new_extents, condition, body, new_expr, op->free_function);
        }

        //debug(0) << stmt << "\n\n";

        if (!is_zero(size) && !on_stack) {
            Expr profiler_pipeline_state = Variable::make(Handle(), "profiler_pipeline_state");
            debug(1) << "  Allocation on heap: " << op->name << "(" << size << ") in pipeline " << pipeline_name << "\n";
            Expr set_task = Call::make(Int(32), "halide_profiler_memory_allocate",
                                       {profiler_pipeline_state, idx, size}, Call::Extern);
            stmt = Block::make(Evaluate::make(set_task), stmt);
        }
    }

    void visit(const Free *op) {
        int idx = get_func_id(op->name);

        AllocSize alloc = func_alloc_sizes.get(op->name);
        func_alloc_sizes.pop(op->name);

        IRMutator::visit(op);

        if (!is_zero(alloc.size)) {
            Expr profiler_pipeline_state = Variable::make(Handle(), "profiler_pipeline_state");

            if (!alloc.on_stack) {
                debug(1) << "  Free on heap: " << op->name << "(" << alloc.size << ") in pipeline " << pipeline_name << "\n";
                Expr set_task = Call::make(Int(32), "halide_profiler_memory_free",
                                           {profiler_pipeline_state, idx, alloc.size}, Call::Extern);
                stmt = Block::make(Evaluate::make(set_task), stmt);
            } else {
                func_stack_current[idx] = simplify(get_value(func_stack_current, idx) - alloc.size);
                debug(1) << "  Free on stack: " << op->name << "(" << alloc.size << ") in pipeline " << pipeline_name
                         << "; current: " << func_stack_current[idx] << "; peak: " << func_stack_peak[idx] << "\n";
            }
        }
    }

    void visit(const ProducerConsumer *op) {
        //debug(1) << "  Injecting profiler into ProducerConsumer " << op->name << " in pipeline " << pipeline_name << "\n";
        int idx = get_func_id(op->name);

        stack.push_back(idx);
        Stmt produce = mutate(op->produce);
        Stmt update = op->update.defined() ? mutate(op->update) : Stmt();
        stack.pop_back();

        Stmt consume = mutate(op->consume);

        Expr profiler_token = Variable::make(Int(32), "profiler_token");
        Expr profiler_state = Variable::make(Handle(), "profiler_state");

        // This call gets inlined and becomes a single store instruction.
        Expr set_task = Call::make(Int(32), "halide_profiler_set_current_func",
                                   {profiler_state, profiler_token, idx}, Call::Extern);

        // At the beginning of the consume step, set the current task
        // back to the outer one.
        Expr set_outer_task = Call::make(Int(32), "halide_profiler_set_current_func",
                                         {profiler_state, profiler_token, stack.back()}, Call::Extern);

        produce = Block::make(Evaluate::make(set_task), produce);
        consume = Block::make(Evaluate::make(set_outer_task), consume);

        stmt = ProducerConsumer::make(op->name, produce, update, consume);
    }

    void visit(const For *op) {
        // We profile by storing a token to global memory, so don't enter GPU loops
        if (op->device_api == DeviceAPI::Parent ||
            op->device_api == DeviceAPI::Host) {
            IRMutator::visit(op);
        } else {
            stmt = op;
        }
    }
};

Stmt inject_profiling(Stmt s, string pipeline_name) {
    InjectProfiling profiling(pipeline_name);
    s = profiling.mutate(s);

    int num_funcs = (int)(profiling.indices.size());

    Expr func_names_buf = Load::make(Handle(), "profiling_func_names", 0, Buffer(), Parameter());
    func_names_buf = Call::make(Handle(), Call::address_of, {func_names_buf}, Call::Intrinsic);

    Expr start_profiler = Call::make(Int(32), "halide_profiler_pipeline_start",
                                     {pipeline_name, num_funcs, func_names_buf}, Call::Extern);

    Expr get_state = Call::make(Handle(), "halide_profiler_get_state", {}, Call::Extern);

    Expr get_pipeline_state = Call::make(Handle(), "halide_profiler_get_pipeline_state", {pipeline_name}, Call::Extern);

    Expr profiler_token = Variable::make(Int(32), "profiler_token");

    Expr stop_profiler = Call::make(Int(32), Call::register_destructor,
                                    {Expr("halide_profiler_pipeline_end"), get_state}, Call::Intrinsic);

    Expr stack_peak = 0;
    for (const auto& iter : profiling.func_stack_peak) {
        stack_peak = max(stack_peak, iter.second);
    }
    stack_peak = simplify(stack_peak);

    if (!is_zero(stack_peak)) {
        Expr func_stack_peak_buf = Load::make(Handle(), "profiling_func_stack_peak_buf", 0, Buffer(), Parameter());
        func_stack_peak_buf = Call::make(Handle(), Call::address_of, {func_stack_peak_buf}, Call::Intrinsic);

        Expr profiler_pipeline_state = Variable::make(Handle(), "profiler_pipeline_state");
        Stmt update_stack = Evaluate::make(Call::make(Int(32), "halide_profiler_stack_peak_update",
                                           {profiler_pipeline_state, stack_peak, func_stack_peak_buf}, Call::Extern));
        s = Block::make(update_stack, s);
    }

    s = LetStmt::make("profiler_pipeline_state", get_pipeline_state, s);
    s = LetStmt::make("profiler_state", get_state, s);
    // If there was a problem starting the profiler, it will call an
    // appropriate halide error function and then return the
    // (negative) error code as the token.
    s = Block::make(AssertStmt::make(profiler_token >= 0, profiler_token), s);
    s = LetStmt::make("profiler_token", start_profiler, s);

    if (!is_zero(stack_peak)) {
        for (int i = num_funcs-1; i >= 0; --i) {
            s = Block::make(Store::make("profiling_func_stack_peak_buf", get_value(profiling.func_stack_peak, i), i), s);
        }
        s = Block::make(s, Free::make("profiling_func_stack_peak_buf"));
        s = Allocate::make("profiling_func_stack_peak_buf", Int(32), {num_funcs}, const_true(), s);
    }

    for (std::pair<string, int> p : profiling.indices) {
        s = Block::make(Store::make("profiling_func_names", p.first, p.second), s);
    }

    s = Block::make(s, Free::make("profiling_func_names"));
    s = Allocate::make("profiling_func_names", Handle(), {num_funcs}, const_true(), s);
    s = Block::make(Evaluate::make(stop_profiler), s);

    return s;
}

}
}
