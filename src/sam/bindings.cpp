#include <pybind11/pybind11.h>
#include "math.hpp"
#include "backend/c_api/grpLR.h"
#include "c_api.h"

namespace py = pybind11;

PYBIND11_PLUGIN(sam) {
  py::module m("sam", R"doc(
        Python module
        -----------------------
        .. currentmodule:: python_cpp_example
        .. autosummary::
           :toctree: _generate
           
           add
           subtract
    )doc");

  m.def("add", &add, R"doc(
        Add two numbers
        
        Some other information about the add function.
    )doc");

  m.def("subtract", &subtract, R"doc(
        Subtract two numbers

        Some other information about the subtract function.
    )doc");

  m.def("__grplasso", &__grplasso);

  return m.ptr();
}
