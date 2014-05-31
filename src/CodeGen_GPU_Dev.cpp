#include "CodeGen_GPU_Dev.h"

namespace Halide {
namespace Internal {

CodeGen_GPU_Dev::~CodeGen_GPU_Dev() {
}

bool CodeGen_GPU_Dev::is_gpu_var(const std::string &name) {
    return is_gpu_block_var(name) || is_gpu_thread_var(name);
}

bool CodeGen_GPU_Dev::is_gpu_block_var(const std::string &name) {
    std::string n = base_name(name);

    bool result = (n == "blockidx" ||
                   n == "blockidy" ||
                   n == "blockidz" ||
                   n == "blockidw");

    return result;
}

bool CodeGen_GPU_Dev::is_gpu_thread_var(const std::string &name) {
    std::string n = base_name(name);

    bool result = (n == "threadidx" ||
                   n == "threadidy" ||
                   n == "threadidz" ||
                   n == "threadidw");

    return result;
}

}}
