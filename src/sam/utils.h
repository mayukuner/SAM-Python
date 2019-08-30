#ifndef UTILS_H
#define UTILS_H

#include "eigen3/Eigen/Dense"
using Eigen::VectorXd;

namespace SAM {
  extern double calc_norm(const VectorXd &x);
  extern double sqr(double x);
}

#endif
