#ifndef C_API_H
#define C_API_H

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
using std::vector;
using std::string;
using std::tuple;


tuple<vector<int>, vector<double>, vector<double>, vector<double>> __grplasso(vector<double> y, vector<vector<vector<double>>> X, vector<double> lambda, int max_ite, double thol, string regfunc, int input);


#endif
