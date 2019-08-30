#include "c_api.h"
#include <stdlib.h> // for NULL
#include <pybind11/stl.h>
#include <cassert>
#include "backend/c_api/grplasso.h"
#include "backend/c_api/grpSVM.h"
#include "backend/c_api/grpLR.h"
#include "backend/c_api/grpPR.h"

using std::vector;
using std::string;
using std::tuple;

tuple<vector<int>, vector<double>, vector<double>, vector<double>> __grplasso(vector<double> y, vector<vector<vector<double>>> X, vector<double> lambda, int max_ite, double thol, string regfunc, int input) {
  // return df, sse, func_norm
  int n = X.size(), d = X[0].size(), p = X[0][0].size();
  int nlambda = lambda.size();

  vector<int> df(nlambda);
  vector<double> sse(nlambda*d);
  vector<double> func_norm(nlambda);
  vector<double> w(nlambda*d*p);

  double* XX = new double[n * d * p];


  for (int i = 0; i < n; i++) {
    for (int j = 0; j < d; j++) {
      for (int k = 0; k < p; k++) {
        XX[j*p*n + k*n + i] = X[i][j][k];
      }
    }
  }

  const char *p_regfunc = regfunc.data ();

  grplasso(y.data(), XX, lambda.data(), &nlambda, &n, &d, &p, w.data(), &max_ite, &thol, &p_regfunc, &input, df.data(), sse.data(), func_norm.data());

  return make_tuple(df, sse, func_norm, w);
  // return df;
}

void __grpLR(vector<vector<vector<double>>> A, vector<double> y, vector<double> lambda, int nlambda, double L0, int n, int d, int p, double x, double a0, int max_ite, double thol, string regfunc, double alpha, double z, int df, double func_norm) {

}

void __grpPR() {

}

void __grpSVM() {

}
