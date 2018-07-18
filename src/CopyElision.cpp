#include "CopyElision.h"
#include "IREquality.h"
#include "Func.h"
#include "FindCalls.h"
#include "Schedule.h"
#include "AutoScheduleUtils.h"

#include <set>

namespace Halide {
namespace Internal {

using std::map;
using std::set;
using std::string;
using std::vector;

namespace {

string print_function(const Function &f) {
    std::ostringstream stream;
    stream << f.name() << "(";
    for (int i = 0; i < f.dimensions(); ++i) {
        stream << f.args()[i];
        if (i != f.dimensions()-1) {
            stream << ", ";
        }
    }
    stream << ") = ";

    if (f.has_extern_definition()) {
        vector<Expr> extern_call_args;
        const vector<ExternFuncArgument> &args = f.extern_arguments();
        for (const ExternFuncArgument &arg : args) {
            if (arg.is_expr()) {
                extern_call_args.push_back(arg.expr);
            } else if (arg.is_func()) {
                Function input(arg.func);
                LoopLevel store_level = input.schedule().store_level().lock();
                LoopLevel compute_level = input.schedule().compute_level().lock();
                if (store_level == compute_level) {
                    for (int k = 0; k < input.outputs(); k++) {
                        string buf_name = input.name();
                        if (input.outputs() > 1) {
                            buf_name += "." + std::to_string(k);
                        }
                        buf_name += ".buffer";
                        Expr buffer = Variable::make(type_of<struct halide_buffer_t *>(), buf_name);
                        extern_call_args.push_back(buffer);
                    }
                } else {
                    for (int k = 0; k < input.outputs(); k++) {
                        string buf_name = input.name() + "." + std::to_string(k) + ".tmp_buffer";
                        extern_call_args.push_back(Variable::make(type_of<struct halide_buffer_t *>(), buf_name));
                    }
                }
            } else if (arg.is_buffer()) {
                Buffer<> b = arg.buffer;
                Parameter p(b.type(), true, b.dimensions(), b.name());
                p.set_buffer(b);
                Expr buf = Variable::make(type_of<struct halide_buffer_t *>(), b.name() + ".buffer", p);
                extern_call_args.push_back(buf);
            } else if (arg.is_image_param()) {
                Parameter p = arg.image_param;
                Expr buf = Variable::make(type_of<struct halide_buffer_t *>(), p.name() + ".buffer", p);
                extern_call_args.push_back(buf);
            } else {
                internal_error << "Bad ExternFuncArgument type\n";
            }
        }
        Expr expr = f.make_call_to_extern_definition(extern_call_args, get_target_from_environment());
        stream << expr;
    } else {
        if (f.values().size() > 1) {
            stream << "{";
        }
        for (int i = 0; i < (int)f.values().size(); ++i) {
            stream << f.values()[i];
            if (i != (int)f.values().size()-1) {
                stream << ", ";
            }
        }
        if (f.values().size() > 1) {
            stream << "}";
        }
    }
    return stream.str();
}

// Given a copy-elision pair, return true if the producer is computed
// (compute_at) within the scope of the consumer's buffer realization
// (store_at). If consumer is an output of the pipeline, the producer
// is always in scope.
bool is_prod_within_cons_realization(const Function &prod_f,
                                     const Function &cons_f,
                                     bool is_cons_output) {
    const LoopLevel &prod_compute_level = prod_f.schedule().compute_level();
    if (prod_compute_level.is_inlined() ||
        (prod_compute_level.is_root() && !is_cons_output)) {
        debug(4) << "...Function \"" << cons_f.name() << "\" calls function \""
                 << prod_f.name() << "\", which either is computed at root or inlined\n";
        return false;
    }

    // TODO(psuriana): Need to add another pass to check if the producer is
    // produced before the consumer's buffer is allocated).

    return true;
}

// If there is a potentially valid copy-elision pair, return the name of the
// function from which it copies from; otherwise, return an empty string.
string get_elision_pair_candidates(const Function &f,
                                   const vector<Function> &outputs,
                                   const map<string, Function> &env,
                                   const map<string, int> &num_callers,
                                   const set<string> &inlined) {

    // Ignore the case when 'f' has updates or is an extern function or
    // is inlined, since in these cases, the copy elision will not be valid.
    if (f.has_update_definition() || f.has_extern_definition() ||
        inlined.count(f.name())) {
        return "";
    }

    const vector<Expr> &f_args = f.definition().args();

    bool is_cons_output = false;
    for (const Function &o : outputs) {
        is_cons_output = is_cons_output | o.same_as(f);
    }

    string prod;
    for (int i = 0; i < (int)f.values().size(); ++i) {
        // Perform all inlining first to get the actual producer-consumer
        // copy relation
        Expr val = perform_inline(f.values()[i], env, inlined);
        if (const Call *call = val.as<Call>()) {
            if (call->call_type == Call::Halide) {
                // Check 'f' only calls one function
                if (!prod.empty() && (prod != call->name)) {
                    debug(4) << "...Function \"" << f.name() << "\" calls multiple "
                             << "functions: \"" << prod << "\" and \""
                             << call->name << "\"\n";
                    return "";
                }
                prod = call->name;

                if (!is_prod_within_cons_realization(env.at(prod), f, is_cons_output)) {
                    debug(4) << "...Function \"" << f.name() << "\" and "
                             << "function \"" << prod << "\", is not a valid "
                             << "copy-elision pair\n";
                    return "";
                }

                // Check only 'f' calls 'prod'
                const auto &iter = num_callers.find(prod);
                if ((iter != num_callers.end()) && (iter->second > 1)) {
                    debug(4) << "...Function \"" << f.name() << "\" is a simple copy but \""
                             << prod << "\" has multiple callers\n";
                    return "";
                }

                // Check 'f' and 'prod' have the same loop dimensions
                Function prod_f = Function(call->func);
                if (f.dimensions() != prod_f.dimensions()) {
                    debug(4) << "...Function \"" << f.name() << "\" and \""
                             << prod_f.name() << "\" have different dimensions ("
                             << f.dimensions() << " vs " << prod_f.dimensions() << ")\n";
                    return "";
                }
                internal_assert(f_args.size() == call->args.size());

                // Check 'f' and 'prod' have the same number of outputs
                // (or tuple sizes)
                if (f.outputs() != prod_f.outputs()) {
                    debug(4) << "...Function \"" << f.name() << "\" does not call "
                             << "the whole tuple values of function \""
                             << prod_f.name() << "\"(" << f.outputs()
                             << " vs " << prod_f.outputs() << ")\n";
                    return "";
                }

                // Check f[i] also calls prod[i]
                if (i != call->value_index) {
                    debug(4) << "...Function \"" << f.name() << "\" calls "
                             << prod_f.name() << "[" << call->value_index
                             << "] at value index " << i << "\n";
                    return "";
                }

                for (int j = 0; j < f.dimensions(); ++j) {
                    // Check if the call args are equivalent for both the
                    // RHS ('f') and LHS ('prod_f').
                    // TODO(psuriana): Handle case when copying with index shifting
                    if (!equal(f_args[j], call->args[j])) {
                        debug(4) << "At arg " << j << ", " << f.name() << " (arg: "
                                 << f_args[i] << ") != " << prod_f.name()
                                 << "[" << call->value_index << "] (arg: "
                                 << call->args[j] << ")\n";
                        return "";
                    }
                }
            }
        } else if (!prod.empty()) {
            debug(4) << "...Function \"" << f.name() << "\" does not call "
                     << "the whole tuple values of function \""
                     << prod << "\" or is not a simple copy\n";
            return "";
        }
    }
    return prod;
}

} // anonymous namespace

map<string, string> get_valid_copy_elision_pairs(
        const vector<Function> &outputs, const map<string, Function> &env) {

    // Figure out the functions being (valid to be) inlined and the number
    // of callers (excluding calls by itself, e.g. within update stages) of
    // each functions within 'env'.
    map<string, int> num_callers;
    set<string> inlined;
    for (const auto &caller : env) {
        if (caller.second.can_be_inlined() &&
            caller.second.schedule().compute_level().is_inlined()) {
            inlined.insert(caller.first);
        }
        for (const auto &callee : find_direct_calls(caller.second)) {
            if (callee.first != caller.first) {
                num_callers[callee.first] += 1;
            }
        }
    }

    map<string, string> elision_pairs;
    for (const auto &iter : env) {
        string copied_from =
            get_elision_pair_candidates(iter.second, outputs, env, num_callers, inlined);
        if (!copied_from.empty()) {
            elision_pairs.emplace(iter.first, copied_from);
        }
    }

    // Simplify elision chaining. The following case {{"out" -> "g"}, {"g" -> "f"}}
    // will be simplified into {{"out" -> "f"}, {"g" -> ""}}.
    bool fixed = false;
    while (!fixed) {
        fixed = true;
        for (auto iter = elision_pairs.begin(); iter != elision_pairs.end(); ++iter) {
            auto other = elision_pairs.find(iter->second);
            if (other != elision_pairs.end()) {
                iter->second = other->second;
                other->second = ""; // Set the producer to be empty string
                fixed = true;
            }
        }
    }

    debug(0) << "\nElision pairs:\n";
    for (const auto &p : elision_pairs) {
        debug(0) << "cons: " << p.first << " (compute: " << env.at(p.first).schedule().store_level().to_string()
                 << ", store: " << env.at(p.first).schedule().compute_level().to_string()
                 << ") -> prod: " << p.second;
        if (!p.second.empty()) {
            debug(0) << " (compute: " << env.at(p.second).schedule().store_level().to_string()
                     << ", store: " << env.at(p.second).schedule().compute_level().to_string() << ")";
        }
        debug(0) << "\n\tcons: " << print_function(env.at(p.first)) << "\n";
        if (p.second.empty()) {
            debug(0) << "\tprod: NONE\n\n";
        } else {
            debug(0) << "\tprod: " << print_function(env.at(p.second)) << "\n\n";
        }
    }

    return elision_pairs;
}

}  // namespace Internal
}  // namespace Halide
