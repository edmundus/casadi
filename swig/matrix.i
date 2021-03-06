/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010-2014 Joel Andersson, Joris Gillis, Moritz Diehl,
 *                            K.U. Leuven. All rights reserved.
 *    Copyright (C) 2011-2014 Greg Horn
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */
#ifndef CASADI_MATRIX_I
#define CASADI_MATRIX_I


namespace casadi{

#ifdef SWIGPYTHON

/**

Accepts: 2D numpy.ndarray, numpy.matrix (contiguous, native byte order, datatype double)   - DENSE
         1D numpy.ndarray, numpy.matrix (contiguous, native byte order, datatype double)   - SPARSE
         2D scipy.csc_matrix
*/

  %typemap(in,numinputs=1) (double * val,int len,int stride1, int stride2,SparsityType sp)  {
    PyObject* p = $input;
    $3 = 0;
    $4 = 0;
    if (is_array(p)) {
      if (!(array_is_native(p) && array_type(p)==NPY_DOUBLE))
        SWIG_exception_fail(SWIG_TypeError, "Array should be native & of datatype double");
			  
      if (!(array_is_contiguous(p))) {
        if (PyArray_CHKFLAGS((PyArrayObject *) p,NPY_ALIGNED)) {
          $3 = PyArray_STRIDE((PyArrayObject *) p,0)/sizeof(double);
          $4 = PyArray_STRIDE((PyArrayObject *) p,1)/sizeof(double);
        } else {
          SWIG_exception_fail(SWIG_TypeError, "Array should be contiguous or aligned");
        }
      }
	    
      if (!(array_size(p,0)==arg1->size1() && array_size(p,1)==arg1->size2()) ) {
        std::stringstream s;
        s << "SWIG::typemap(in) (double *val,int len,SparsityType sp) " << std::endl;
        s << "Array is not of correct shape.";
        s << "Expecting shape (" << arg1->size1() << "," << arg1->size2() << ")" << ", but got shape ("
          << array_size(p,0) << "," << array_size(p,1) <<") instead.";
        const std::string tmp(s.str());
        const char* cstr = tmp.c_str();
        SWIG_exception_fail(SWIG_TypeError,  cstr);
      }
      $5 = casadi::SP_DENSETRANS;
      $2 = array_size(p,0)*array_size(p,1);
      $1 = (double*) array_data(p);
    } else if (PyObjectHasClassName(p,"csc_matrix")) {
      $5 = casadi::SP_SPARSE;
      PyObject * narray=PyObject_GetAttrString( p, "data"); // narray needs to be decref'ed
      if (!(array_is_contiguous(narray) && array_is_native(narray) && array_type(narray)==NPY_DOUBLE))
        SWIG_exception_fail(SWIG_TypeError, "csc_matrix should be contiguous, native & of datatype double");
      $2 = array_size(narray,0);
      if (!(array_size(narray,0)==arg1->nnz() ) ) {
        std::stringstream s;
        s << "SWIG::typemap(in) (double *val,int len,SparsityType sp) " << std::endl;
        s << "csc_matrix does not have correct number of non-zero elements.";
        s << "Expecting " << arg1->nnz() << " non-zeros, but got " << array_size(narray,0) << " instead.";
        const std::string tmp(s.str());
        const char* cstr = tmp.c_str();
        Py_DECREF(narray);
        SWIG_exception_fail(SWIG_TypeError,  cstr);
      }
      $1 = (double*) array_data(narray);
      Py_DECREF(narray);
    } else {
      SWIG_exception_fail(SWIG_TypeError, "Unrecognised object");
    }
  }

%typemap(typecheck,precedence=SWIG_TYPECHECK_INTEGER) (double * val,int len,int stride1, int stride2,SparsityType sp) {
  PyObject* p = $input;
  if (((is_array(p) && array_numdims(p) == 2)  && array_type(p)!=NPY_OBJECT) || PyObjectHasClassName(p,"csc_matrix")) {
    $1=1;
  } else {
    $1=0;
  }
}

%typemap(in,numinputs=1) (double * val,int len,int stride1, int stride2)  {
    PyObject* p = $input;
    $3 = 0;
    $4 = 0;
    if (!(array_is_native(p) && array_type(p)==NPY_DOUBLE))
      SWIG_exception_fail(SWIG_TypeError, "Array should be native & of datatype double");
			  
    if (!(array_is_contiguous(p))) {
      if (PyArray_CHKFLAGS((PyArrayObject *) p,NPY_ALIGNED)) {
        $3 = PyArray_STRIDE((PyArrayObject *) p,0)/sizeof(double);
        $4 = PyArray_STRIDE((PyArrayObject *) p,1)/sizeof(double);
      } else {
        SWIG_exception_fail(SWIG_TypeError, "Array should be contiguous or aligned");
      }
    }
	    
    if (!(array_size(p,0)==arg1->nnz()) ) {
      std::stringstream s;
      s << "SWIG::typemap(in) (double *val,int len,SparsityType sp) " << std::endl;
      s << "Array is not of correct size. Should match number of non-zero elements.";
      s << "Expecting " << array_size(p,0) << " non-zeros, but got " << arg1->nnz() <<" instead.";
      const std::string tmp(s.str());
      const char* cstr = tmp.c_str();
      SWIG_exception_fail(SWIG_TypeError,  cstr);
    }
    $2 = array_size(p,0);
    $1 = (double*) array_data(p);
  }

%typemap(typecheck,precedence=SWIG_TYPECHECK_INTEGER) (double * val,int len,int stride1, int stride2) {
  PyObject* p = $input;
  if ((is_array(p) && array_numdims(p) == 1) && array_type(p)!=NPY_OBJECT) {
    $1=1;
  } else {
    $1=0;
  }
}

#endif // SWIGPYTHON

} // namespace casadi

%include "printable_object.i"
%include "generic_matrix.i"
%include "generic_expression.i"

// FIXME: Placing in printable_object.i does not work
%template(PrintSX)           casadi::PrintableObject<casadi::Matrix<casadi::SXElement> >;

%include <casadi/core/matrix/matrix.hpp>

%template(IMatrix)           casadi::Matrix<int>;
%template(DMatrix)           casadi::Matrix<double>;

%extend casadi::Matrix<double> {
   %template(DMatrix) Matrix<int>;
};

namespace casadi{
  %extend Matrix<double> {

    void assign(const casadi::Matrix<double>&rhs) { (*$self)=rhs; }
    %matrix_convertors
    %matrix_helpers(casadi::Matrix<double>)

  }
  %extend Matrix<int> {

    void assign(const casadi::Matrix<int>&rhs) { (*$self)=rhs; }
    %matrix_convertors
    %matrix_helpers(casadi::Matrix<int>)

  }
}

// Extend DMatrix with SWIG unique features
namespace casadi{
  %extend Matrix<double> {
    // Convert to a dense matrix
    GUESTOBJECT* full() const {
#ifdef SWIGPYTHON
      npy_intp dims[2] = {$self->size1(), $self->size2()};
      PyObject* ret = PyArray_SimpleNew(2, dims, NPY_DOUBLE);
      double* d = static_cast<double*>(array_data(ret));
      $self->get(d, true); // Row-major
      return ret;
#elif defined(SWIGMATLAB)
      mxArray *p  = mxCreateDoubleMatrix($self->size1(), $self->size2(), mxREAL);
      double* d = static_cast<double*>(mxGetData(p));
      $self->get(d); // Column-major
      return p;
#else
      return 0;
#endif
    }

#ifdef SWIGMATLAB
    // Convert to a sparse matrix
    GUESTOBJECT* zz_sparse() const {
      mxArray *p  = mxCreateSparse($self->size1(), $self->size2(), $self->nnz(), mxREAL);
      $self->getNZ(static_cast<double*>(mxGetData(p)));
      std::copy($self->colind(), $self->colind()+$self->size2()+1, mxGetJc(p));
      std::copy($self->row(), $self->row()+$self->size2()+1, mxGetIr(p));
      return p;
    }
#endif
  }
} // namespace casadi


#ifdef SWIGPYTHON
namespace casadi{
%extend Matrix<double> {
/// Create a 2D contiguous NP_DOUBLE numpy.ndarray

PyObject* arrayView() {
  if ($self->nnz()!=$self->numel()) 
    throw  casadi::CasadiException("Matrix<double>::arrayview() can only construct arrayviews for dense DMatrices.");
  npy_intp dims[2];
  dims[0] = $self->size2();
  dims[1] = $self->size1();
  std::vector<double> &v = $self->data();
  PyArrayObject* temp = (PyArrayObject*) PyArray_New(&PyArray_Type, 2, dims, NPY_DOUBLE, NULL, &v[0], 0, NPY_ARRAY_CARRAY, NULL);
  PyObject* ret = PyArray_Transpose(temp,NULL);
  Py_DECREF(temp); 
  return ret;
}
    
%pythoncode %{
  def toArray(self,shared=False):
    import numpy as n
    if shared:
      if not self.isDense():
        raise Expection("toArray(shared=True) only possible for dense arrays.")
      return self.arrayView()
    else:
      r = n.zeros((self.size1(),self.size2()))
      self.get(r)
      return r
%}

%python_array_wrappers(999.0)

// The following code has some trickery to fool numpy ufunc.
// Normally, because of the presence of __array__, an ufunctor like nump.sqrt
// will unleash its activity on the output of __array__
// However, we wish DMatrix to remain a DMatrix
// So when we receive a call from a functor, we return a dummy empty array
// and return the real result during the postprocessing (__array_wrap__) of the functor.
%pythoncode %{
  def __array_custom__(self,*args,**kwargs):
    if "dtype" in kwargs and not(isinstance(kwargs["dtype"],n.double)):
      return n.array(self.toArray(),dtype=kwargs["dtype"])
    else:
      return self.toArray()
%}

%pythoncode %{
  def toCsc_matrix(self):
    import numpy as n
    import warnings
    with warnings.catch_warnings():
      warnings.simplefilter("ignore")
      from scipy.sparse import csc_matrix
    return csc_matrix( (self.nonzeros(),self.row(),self.colind()), shape = self.shape, dtype=n.double )

  def tocsc(self):
    return self.toCsc_matrix()

%}

%pythoncode %{
  def __nonzero__(self):
    if self.numel()!=1:
      raise Exception("Only a scalar can be cast to a float")
    if self.nnz()==0:
      return 0
    return float(self)!=0
%}

%pythoncode %{
  def __abs__(self):
    return abs(float(self))
%}

#ifdef SWIGPYTHON
binopsrFull(casadi::Matrix<double>)
binopsFull(const casadi::SX & b,,casadi::SX,casadi::SX)
binopsFull(const casadi::MX & b,,casadi::MX,casadi::MX)
#endif // SWIGPYTHON

}; // extend Matrix<double>

%extend Matrix<int> {

  %python_array_wrappers(998.0)

  %pythoncode %{
    def toArray(self):
      import numpy as n
      r = n.zeros((self.size1(),self.size2()))
      d = self.nonzeros_int()
      for j in range(self.size2()):
        for k in range(self.colind(j),self.colind(j+1)):
          i = self.row(k)
          r[i,j] = d[k]
      return r
  %}
  
#ifdef SWIGPYTHON
  binopsrFull(casadi::Matrix<int>)
  binopsFull(const casadi::SX & b,,casadi::SX,casadi::SX)
  binopsFull(const casadi::Matrix<double> & b,,casadi::Matrix<double>,casadi::Matrix<double>)
  binopsFull(const casadi::MX & b,,casadi::MX,casadi::MX)
#endif // SWIGPYTHON
  %pythoncode %{
    def __abs__(self):
      return abs(int(self))
  %}
} // extend Matrix<int>


// Logic for pickling

%extend Matrix<int> {

  %pythoncode %{
    def __setstate__(self, state):
        sp = Sparsity.__new__(Sparsity)
        sp.__setstate__(state["sparsity"])
        self.__init__(sp,state["data"])

    def __getstate__(self):
        return {"sparsity" : self.sparsity().__getstate__(), "data": numpy.array(self.nonzeros_int(),dtype=int)}
  %} 
}

%extend Matrix<double> {

  %pythoncode %{
    def __setstate__(self, state):
        sp = Sparsity.__new__(Sparsity)
        sp.__setstate__(state["sparsity"])
        self.__init__(sp,state["data"])

    def __getstate__(self):
        return {"sparsity" : self.sparsity().__getstate__(), "data": numpy.array(self.nonzeros(),dtype=float)}
  %}
  
}


} // namespace casadi
#endif // SWIGPYTHON


#endif // CASADI_MATRIX_I
