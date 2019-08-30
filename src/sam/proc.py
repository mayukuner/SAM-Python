from . import __grplasso
def grplasso(y, X, lbd, max_ite, thol, regfunc, inp):
    return __grplasso(y, X, lbd, max_ite, thol, regfunc, inp)
