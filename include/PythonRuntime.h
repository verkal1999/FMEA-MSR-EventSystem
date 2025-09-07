// PythonRuntime.h
#pragma once
#include <pybind11/embed.h>
#include <mutex>
namespace py = pybind11;

class PythonRuntime {
public:
    static void ensureStarted() {
        static PythonRuntime inst; (void)inst;
    }
private:
    py::scoped_interpreter guard_; // startet Interpreter einmal pro Prozess
    PythonRuntime() : guard_{} {}
};