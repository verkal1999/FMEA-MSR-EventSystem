#include "PythonRuntime.h"
std::unique_ptr<pybind11::scoped_interpreter> PythonRuntime::guard_;