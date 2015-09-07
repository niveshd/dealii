// ---------------------------------------------------------------------
//
// Copyright (C) 2000 - 2015 by the deal.II authors
//
// This file is part of the deal.II library.
//
// The deal.II library is free software; you can use it, redistribute
// it, and/or modify it under the terms of the GNU Lesser General
// Public License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
// The full text of the license can be found in the file LICENSE at
// the top level of the deal.II distribution.
//
// ---------------------------------------------------------------------


#include <deal.II/base/derivative_form.h>
#include <deal.II/base/quadrature.h>
#include <deal.II/base/qprojector.h>
#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/tensor_product_polynomials.h>
#include <deal.II/base/memory_consumption.h>
#include <deal.II/base/std_cxx11/array.h>
#include <deal.II/base/std_cxx11/unique_ptr.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/grid/tria.h>
#include <deal.II/grid/tria_iterator.h>
#include <deal.II/dofs/dof_accessor.h>
#include <deal.II/fe/fe_tools.h>
#include <deal.II/fe/fe.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/fe/mapping_q1.h>

#include <cmath>
#include <algorithm>


DEAL_II_NAMESPACE_OPEN


template<int dim, int spacedim>
MappingQGeneric<dim,spacedim>::InternalData::InternalData (const unsigned int polynomial_degree)
  :
  polynomial_degree (polynomial_degree),
  n_shape_functions (Utilities::fixed_power<dim>(polynomial_degree+1))
{}



template<int dim, int spacedim>
std::size_t
MappingQGeneric<dim,spacedim>::InternalData::memory_consumption () const
{
  return (Mapping<dim,spacedim>::InternalDataBase::memory_consumption() +
          MemoryConsumption::memory_consumption (shape_values) +
          MemoryConsumption::memory_consumption (shape_derivatives) +
          MemoryConsumption::memory_consumption (covariant) +
          MemoryConsumption::memory_consumption (contravariant) +
          MemoryConsumption::memory_consumption (unit_tangentials) +
          MemoryConsumption::memory_consumption (aux) +
          MemoryConsumption::memory_consumption (mapping_support_points) +
          MemoryConsumption::memory_consumption (cell_of_current_support_points) +
          MemoryConsumption::memory_consumption (volume_elements) +
          MemoryConsumption::memory_consumption (polynomial_degree) +
          MemoryConsumption::memory_consumption (n_shape_functions));
}


template <int dim, int spacedim>
void
MappingQGeneric<dim,spacedim>::InternalData::
initialize (const UpdateFlags      update_flags,
            const Quadrature<dim> &q,
            const unsigned int     n_original_q_points)
{
  // store the flags in the internal data object so we can access them
  // in fill_fe_*_values()
  this->update_each = update_flags;

  const unsigned int n_q_points = q.size();

  // see if we need the (transformation) shape function values
  // and/or gradients and resize the necessary arrays
  if (this->update_each & update_quadrature_points)
    shape_values.resize(n_shape_functions * n_q_points);

  if (this->update_each & (update_covariant_transformation
                           | update_contravariant_transformation
                           | update_JxW_values
                           | update_boundary_forms
                           | update_normal_vectors
                           | update_jacobians
                           | update_jacobian_grads
                           | update_inverse_jacobians
                           | update_jacobian_pushed_forward_grads
                           | update_jacobian_2nd_derivatives
                           | update_jacobian_pushed_forward_2nd_derivatives
                           | update_jacobian_3rd_derivatives
                           | update_jacobian_pushed_forward_3rd_derivatives))
    shape_derivatives.resize(n_shape_functions * n_q_points);

  if (this->update_each & update_covariant_transformation)
    covariant.resize(n_original_q_points);

  if (this->update_each & update_contravariant_transformation)
    contravariant.resize(n_original_q_points);

  if (this->update_each & update_volume_elements)
    volume_elements.resize(n_original_q_points);

  if (this->update_each &
      (update_jacobian_grads | update_jacobian_pushed_forward_grads) )
    shape_second_derivatives.resize(n_shape_functions * n_q_points);

  if (this->update_each &
      (update_jacobian_2nd_derivatives | update_jacobian_pushed_forward_2nd_derivatives) )
    shape_third_derivatives.resize(n_shape_functions * n_q_points);

  if (this->update_each &
      (update_jacobian_3rd_derivatives | update_jacobian_pushed_forward_3rd_derivatives) )
    shape_fourth_derivatives.resize(n_shape_functions * n_q_points);

  // now also fill the various fields with their correct values
  compute_shape_function_values (q.get_points());
}



template <int dim, int spacedim>
void
MappingQGeneric<dim,spacedim>::InternalData::
initialize_face (const UpdateFlags      update_flags,
                 const Quadrature<dim> &q,
                 const unsigned int     n_original_q_points)
{
  initialize (update_flags, q, n_original_q_points);

  if (dim > 1)
    {
      if (this->update_each & update_boundary_forms)
        {
          aux.resize (dim-1, std::vector<Tensor<1,spacedim> > (n_original_q_points));

          // Compute tangentials to the
          // unit cell.
          const unsigned int nfaces = GeometryInfo<dim>::faces_per_cell;
          unit_tangentials.resize (nfaces*(dim-1),
                                   std::vector<Tensor<1,dim> > (n_original_q_points));
          if (dim==2)
            {
              // ensure a counterclockwise
              // orientation of tangentials
              static const int tangential_orientation[4]= {-1,1,1,-1};
              for (unsigned int i=0; i<nfaces; ++i)
                {
                  Tensor<1,dim> tang;
                  tang[1-i/2]=tangential_orientation[i];
                  std::fill (unit_tangentials[i].begin(),
                             unit_tangentials[i].end(), tang);
                }
            }
          else if (dim==3)
            {
              for (unsigned int i=0; i<nfaces; ++i)
                {
                  Tensor<1,dim> tang1, tang2;

                  const unsigned int nd=
                    GeometryInfo<dim>::unit_normal_direction[i];

                  // first tangential
                  // vector in direction
                  // of the (nd+1)%3 axis
                  // and inverted in case
                  // of unit inward normal
                  tang1[(nd+1)%dim]=GeometryInfo<dim>::unit_normal_orientation[i];
                  // second tangential
                  // vector in direction
                  // of the (nd+2)%3 axis
                  tang2[(nd+2)%dim]=1.;

                  // same unit tangents
                  // for all quadrature
                  // points on this face
                  std::fill (unit_tangentials[i].begin(),
                             unit_tangentials[i].end(), tang1);
                  std::fill (unit_tangentials[nfaces+i].begin(),
                             unit_tangentials[nfaces+i].end(), tang2);
                }
            }
        }
    }
}



namespace internal
{
  namespace MappingQGeneric
  {
    // These are left as templates on the spatial dimension (even though dim
    // == spacedim must be true for them to make sense) because templates are
    // expanded before the compiler eliminates code due to the 'if (dim ==
    // spacedim)' statement (see the body of the general
    // transform_real_to_unit_cell).
    template<int spacedim>
    Point<1>
    transform_real_to_unit_cell
    (const std_cxx11::array<Point<spacedim>, GeometryInfo<1>::vertices_per_cell> &vertices,
     const Point<spacedim> &p)
    {
      Assert(spacedim == 1, ExcInternalError());
      return Point<1>((p[0] - vertices[0](0))/(vertices[1](0) - vertices[0](0)));
    }



    template<int spacedim>
    Point<2>
    transform_real_to_unit_cell
    (const std_cxx11::array<Point<spacedim>, GeometryInfo<2>::vertices_per_cell> &vertices,
     const Point<spacedim> &p)
    {
      Assert(spacedim == 2, ExcInternalError());
      const double x = p(0);
      const double y = p(1);

      const double x0 = vertices[0](0);
      const double x1 = vertices[1](0);
      const double x2 = vertices[2](0);
      const double x3 = vertices[3](0);

      const double y0 = vertices[0](1);
      const double y1 = vertices[1](1);
      const double y2 = vertices[2](1);
      const double y3 = vertices[3](1);

      const double a = (x1 - x3)*(y0 - y2) - (x0 - x2)*(y1 - y3);
      const double b = -(x0 - x1 - x2 + x3)*y + (x - 2*x1 + x3)*y0 - (x - 2*x0 + x2)*y1
                       - (x - x1)*y2 + (x - x0)*y3;
      const double c = (x0 - x1)*y - (x - x1)*y0 + (x - x0)*y1;

      const double discriminant = b*b - 4*a*c;
      // fast exit if the point is not in the cell (this is the only case
      // where the discriminant is negative)
      if (discriminant < 0.0)
        {
          return Point<2>(2, 2);
        }

      double eta1;
      double eta2;
      // special case #1: if a is zero, then use the linear formula
      if (a == 0.0 && b != 0.0)
        {
          eta1 = -c/b;
          eta2 = -c/b;
        }
      // special case #2: if c is very small:
      else if (std::abs(c/b) < 1e-12)
        {
          eta1 = (-b - std::sqrt(discriminant)) / (2*a);
          eta2 = (-b + std::sqrt(discriminant)) / (2*a);
        }
      // finally, use the numerically stable version of the quadratic formula:
      else
        {
          eta1 = 2*c / (-b - std::sqrt(discriminant));
          eta2 = 2*c / (-b + std::sqrt(discriminant));
        }
      // pick the one closer to the center of the cell.
      const double eta = (std::abs(eta1 - 0.5) < std::abs(eta2 - 0.5)) ? eta1 : eta2;

      /*
       * There are two ways to compute xi from eta, but either one may have a
       * zero denominator.
       */
      const double subexpr0 = -eta*x2 + x0*(eta - 1);
      const double xi_denominator0 = eta*x3 - x1*(eta - 1) + subexpr0;
      const double max_x = std::max(std::max(std::abs(x0), std::abs(x1)),
                                    std::max(std::abs(x2), std::abs(x3)));

      if (std::abs(xi_denominator0) > 1e-10*max_x)
        {
          const double xi = (x + subexpr0)/xi_denominator0;
          return Point<2>(xi, eta);
        }
      else
        {
          const double max_y = std::max(std::max(std::abs(y0), std::abs(y1)),
                                        std::max(std::abs(y2), std::abs(y3)));
          const double subexpr1 = -eta*y2 + y0*(eta - 1);
          const double xi_denominator1 = eta*y3 - y1*(eta - 1) + subexpr1;
          if (std::abs(xi_denominator1) > 1e-10*max_y)
            {
              const double xi = (subexpr1 + y)/xi_denominator1;
              return Point<2>(xi, eta);
            }
          else // give up and try Newton iteration
            {
              return Point<2>(2, 2);
            }
        }
    }



    template<int spacedim>
    Point<3>
    transform_real_to_unit_cell
    (const std_cxx11::array<Point<spacedim>, GeometryInfo<3>::vertices_per_cell> &/*vertices*/,
     const Point<spacedim> &/*p*/)
    {
      // It should not be possible to get here
      Assert(false, ExcInternalError());
      return Point<3>();
    }



    template <int spacedim>
    void
    compute_shape_function_values (const unsigned int            n_shape_functions,
                                   const std::vector<Point<1> > &unit_points,
                                   typename dealii::MappingQGeneric<1,spacedim>::InternalData &data)
    {
      (void)n_shape_functions;
      const unsigned int n_points=unit_points.size();
      for (unsigned int k = 0 ; k < n_points ; ++k)
        {
          double x = unit_points[k](0);

          if (data.shape_values.size()!=0)
            {
              Assert(data.shape_values.size()==n_shape_functions*n_points,
                     ExcInternalError());
              data.shape(k,0) = 1.-x;
              data.shape(k,1) = x;
            }
          if (data.shape_derivatives.size()!=0)
            {
              Assert(data.shape_derivatives.size()==n_shape_functions*n_points,
                     ExcInternalError());
              data.derivative(k,0)[0] = -1.;
              data.derivative(k,1)[0] = 1.;
            }
          if (data.shape_second_derivatives.size()!=0)
            {
              // the following may or may not
              // work if dim != spacedim
              Assert (spacedim == 1, ExcNotImplemented());

              Assert(data.shape_second_derivatives.size()==n_shape_functions*n_points,
                     ExcInternalError());
              data.second_derivative(k,0)[0][0] = 0;
              data.second_derivative(k,1)[0][0] = 0;
            }
          if (data.shape_third_derivatives.size()!=0)
            {
              // if lower order derivative don't work, neither should this
              Assert (spacedim == 1, ExcNotImplemented());

              Assert(data.shape_third_derivatives.size()==n_shape_functions*n_points,
                     ExcInternalError());

              Tensor<3,1> zero;
              data.third_derivative(k,0) = zero;
              data.third_derivative(k,1) = zero;
            }
          if (data.shape_fourth_derivatives.size()!=0)
            {
              // if lower order derivative don't work, neither should this
              Assert (spacedim == 1, ExcNotImplemented());

              Assert(data.shape_fourth_derivatives.size()==n_shape_functions*n_points,
                     ExcInternalError());

              Tensor<4,1> zero;
              data.fourth_derivative(k,0) = zero;
              data.fourth_derivative(k,1) = zero;
            }
        }
    }


    template <int spacedim>
    void
    compute_shape_function_values (const unsigned int            n_shape_functions,
                                   const std::vector<Point<2> > &unit_points,
                                   typename dealii::MappingQGeneric<2,spacedim>::InternalData &data)
    {
      (void)n_shape_functions;
      const unsigned int n_points=unit_points.size();
      for (unsigned int k = 0 ; k < n_points ; ++k)
        {
          double x = unit_points[k](0);
          double y = unit_points[k](1);

          if (data.shape_values.size()!=0)
            {
              Assert(data.shape_values.size()==n_shape_functions*n_points,
                     ExcInternalError());
              data.shape(k,0) = (1.-x)*(1.-y);
              data.shape(k,1) = x*(1.-y);
              data.shape(k,2) = (1.-x)*y;
              data.shape(k,3) = x*y;
            }
          if (data.shape_derivatives.size()!=0)
            {
              Assert(data.shape_derivatives.size()==n_shape_functions*n_points,
                     ExcInternalError());
              data.derivative(k,0)[0] = (y-1.);
              data.derivative(k,1)[0] = (1.-y);
              data.derivative(k,2)[0] = -y;
              data.derivative(k,3)[0] = y;
              data.derivative(k,0)[1] = (x-1.);
              data.derivative(k,1)[1] = -x;
              data.derivative(k,2)[1] = (1.-x);
              data.derivative(k,3)[1] = x;
            }
          if (data.shape_second_derivatives.size()!=0)
            {
              Assert(data.shape_second_derivatives.size()==n_shape_functions*n_points,
                     ExcInternalError());
              data.second_derivative(k,0)[0][0] = 0;
              data.second_derivative(k,1)[0][0] = 0;
              data.second_derivative(k,2)[0][0] = 0;
              data.second_derivative(k,3)[0][0] = 0;
              data.second_derivative(k,0)[0][1] = 1.;
              data.second_derivative(k,1)[0][1] = -1.;
              data.second_derivative(k,2)[0][1] = -1.;
              data.second_derivative(k,3)[0][1] = 1.;
              data.second_derivative(k,0)[1][0] = 1.;
              data.second_derivative(k,1)[1][0] = -1.;
              data.second_derivative(k,2)[1][0] = -1.;
              data.second_derivative(k,3)[1][0] = 1.;
              data.second_derivative(k,0)[1][1] = 0;
              data.second_derivative(k,1)[1][1] = 0;
              data.second_derivative(k,2)[1][1] = 0;
              data.second_derivative(k,3)[1][1] = 0;
            }
          if (data.shape_third_derivatives.size()!=0)
            {
              Assert(data.shape_third_derivatives.size()==n_shape_functions*n_points,
                     ExcInternalError());

              Tensor<3,2> zero;
              for (unsigned int i=0; i<4; ++i)
                data.third_derivative(k,i) = zero;
            }
          if (data.shape_fourth_derivatives.size()!=0)
            {
              Assert(data.shape_fourth_derivatives.size()==n_shape_functions*n_points,
                     ExcInternalError());
              Tensor<4,2> zero;
              for (unsigned int i=0; i<4; ++i)
                data.fourth_derivative(k,i) = zero;
            }
        }
    }



    template <int spacedim>
    void
    compute_shape_function_values (const unsigned int            n_shape_functions,
                                   const std::vector<Point<3> > &unit_points,
                                   typename dealii::MappingQGeneric<3,spacedim>::InternalData &data)
    {
      (void)n_shape_functions;
      const unsigned int n_points=unit_points.size();
      for (unsigned int k = 0 ; k < n_points ; ++k)
        {
          double x = unit_points[k](0);
          double y = unit_points[k](1);
          double z = unit_points[k](2);

          if (data.shape_values.size()!=0)
            {
              Assert(data.shape_values.size()==n_shape_functions*n_points,
                     ExcInternalError());
              data.shape(k,0) = (1.-x)*(1.-y)*(1.-z);
              data.shape(k,1) = x*(1.-y)*(1.-z);
              data.shape(k,2) = (1.-x)*y*(1.-z);
              data.shape(k,3) = x*y*(1.-z);
              data.shape(k,4) = (1.-x)*(1.-y)*z;
              data.shape(k,5) = x*(1.-y)*z;
              data.shape(k,6) = (1.-x)*y*z;
              data.shape(k,7) = x*y*z;
            }
          if (data.shape_derivatives.size()!=0)
            {
              Assert(data.shape_derivatives.size()==n_shape_functions*n_points,
                     ExcInternalError());
              data.derivative(k,0)[0] = (y-1.)*(1.-z);
              data.derivative(k,1)[0] = (1.-y)*(1.-z);
              data.derivative(k,2)[0] = -y*(1.-z);
              data.derivative(k,3)[0] = y*(1.-z);
              data.derivative(k,4)[0] = (y-1.)*z;
              data.derivative(k,5)[0] = (1.-y)*z;
              data.derivative(k,6)[0] = -y*z;
              data.derivative(k,7)[0] = y*z;
              data.derivative(k,0)[1] = (x-1.)*(1.-z);
              data.derivative(k,1)[1] = -x*(1.-z);
              data.derivative(k,2)[1] = (1.-x)*(1.-z);
              data.derivative(k,3)[1] = x*(1.-z);
              data.derivative(k,4)[1] = (x-1.)*z;
              data.derivative(k,5)[1] = -x*z;
              data.derivative(k,6)[1] = (1.-x)*z;
              data.derivative(k,7)[1] = x*z;
              data.derivative(k,0)[2] = (x-1)*(1.-y);
              data.derivative(k,1)[2] = x*(y-1.);
              data.derivative(k,2)[2] = (x-1.)*y;
              data.derivative(k,3)[2] = -x*y;
              data.derivative(k,4)[2] = (1.-x)*(1.-y);
              data.derivative(k,5)[2] = x*(1.-y);
              data.derivative(k,6)[2] = (1.-x)*y;
              data.derivative(k,7)[2] = x*y;
            }
          if (data.shape_second_derivatives.size()!=0)
            {
              // the following may or may not
              // work if dim != spacedim
              Assert (spacedim == 3, ExcNotImplemented());

              Assert(data.shape_second_derivatives.size()==n_shape_functions*n_points,
                     ExcInternalError());
              data.second_derivative(k,0)[0][0] = 0;
              data.second_derivative(k,1)[0][0] = 0;
              data.second_derivative(k,2)[0][0] = 0;
              data.second_derivative(k,3)[0][0] = 0;
              data.second_derivative(k,4)[0][0] = 0;
              data.second_derivative(k,5)[0][0] = 0;
              data.second_derivative(k,6)[0][0] = 0;
              data.second_derivative(k,7)[0][0] = 0;
              data.second_derivative(k,0)[1][1] = 0;
              data.second_derivative(k,1)[1][1] = 0;
              data.second_derivative(k,2)[1][1] = 0;
              data.second_derivative(k,3)[1][1] = 0;
              data.second_derivative(k,4)[1][1] = 0;
              data.second_derivative(k,5)[1][1] = 0;
              data.second_derivative(k,6)[1][1] = 0;
              data.second_derivative(k,7)[1][1] = 0;
              data.second_derivative(k,0)[2][2] = 0;
              data.second_derivative(k,1)[2][2] = 0;
              data.second_derivative(k,2)[2][2] = 0;
              data.second_derivative(k,3)[2][2] = 0;
              data.second_derivative(k,4)[2][2] = 0;
              data.second_derivative(k,5)[2][2] = 0;
              data.second_derivative(k,6)[2][2] = 0;
              data.second_derivative(k,7)[2][2] = 0;

              data.second_derivative(k,0)[0][1] = (1.-z);
              data.second_derivative(k,1)[0][1] = -(1.-z);
              data.second_derivative(k,2)[0][1] = -(1.-z);
              data.second_derivative(k,3)[0][1] = (1.-z);
              data.second_derivative(k,4)[0][1] = z;
              data.second_derivative(k,5)[0][1] = -z;
              data.second_derivative(k,6)[0][1] = -z;
              data.second_derivative(k,7)[0][1] = z;
              data.second_derivative(k,0)[1][0] = (1.-z);
              data.second_derivative(k,1)[1][0] = -(1.-z);
              data.second_derivative(k,2)[1][0] = -(1.-z);
              data.second_derivative(k,3)[1][0] = (1.-z);
              data.second_derivative(k,4)[1][0] = z;
              data.second_derivative(k,5)[1][0] = -z;
              data.second_derivative(k,6)[1][0] = -z;
              data.second_derivative(k,7)[1][0] = z;

              data.second_derivative(k,0)[0][2] = (1.-y);
              data.second_derivative(k,1)[0][2] = -(1.-y);
              data.second_derivative(k,2)[0][2] = y;
              data.second_derivative(k,3)[0][2] = -y;
              data.second_derivative(k,4)[0][2] = -(1.-y);
              data.second_derivative(k,5)[0][2] = (1.-y);
              data.second_derivative(k,6)[0][2] = -y;
              data.second_derivative(k,7)[0][2] = y;
              data.second_derivative(k,0)[2][0] = (1.-y);
              data.second_derivative(k,1)[2][0] = -(1.-y);
              data.second_derivative(k,2)[2][0] = y;
              data.second_derivative(k,3)[2][0] = -y;
              data.second_derivative(k,4)[2][0] = -(1.-y);
              data.second_derivative(k,5)[2][0] = (1.-y);
              data.second_derivative(k,6)[2][0] = -y;
              data.second_derivative(k,7)[2][0] = y;

              data.second_derivative(k,0)[1][2] = (1.-x);
              data.second_derivative(k,1)[1][2] = x;
              data.second_derivative(k,2)[1][2] = -(1.-x);
              data.second_derivative(k,3)[1][2] = -x;
              data.second_derivative(k,4)[1][2] = -(1.-x);
              data.second_derivative(k,5)[1][2] = -x;
              data.second_derivative(k,6)[1][2] = (1.-x);
              data.second_derivative(k,7)[1][2] = x;
              data.second_derivative(k,0)[2][1] = (1.-x);
              data.second_derivative(k,1)[2][1] = x;
              data.second_derivative(k,2)[2][1] = -(1.-x);
              data.second_derivative(k,3)[2][1] = -x;
              data.second_derivative(k,4)[2][1] = -(1.-x);
              data.second_derivative(k,5)[2][1] = -x;
              data.second_derivative(k,6)[2][1] = (1.-x);
              data.second_derivative(k,7)[2][1] = x;
            }
          if (data.shape_third_derivatives.size()!=0)
            {
              // if lower order derivative don't work, neither should this
              Assert (spacedim == 3, ExcNotImplemented());

              Assert(data.shape_third_derivatives.size()==n_shape_functions*n_points,
                     ExcInternalError());

              for (unsigned int i=0; i<3; ++i)
                for (unsigned int j=0; j<3; ++j)
                  for (unsigned int l=0; l<3; ++l)
                    if ((i==j)||(j==l)||(l==i))
                      {
                        for (unsigned int m=0; m<8; ++m)
                          data.third_derivative(k,m)[i][j][l] = 0;
                      }
                    else
                      {
                        data.third_derivative(k,0)[i][j][l] = -1.;
                        data.third_derivative(k,1)[i][j][l] = 1.;
                        data.third_derivative(k,2)[i][j][l] = 1.;
                        data.third_derivative(k,3)[i][j][l] = -1.;
                        data.third_derivative(k,4)[i][j][l] = 1.;
                        data.third_derivative(k,5)[i][j][l] = -1.;
                        data.third_derivative(k,6)[i][j][l] = -1.;
                        data.third_derivative(k,7)[i][j][l] = 1.;
                      }

            }
          if (data.shape_fourth_derivatives.size()!=0)
            {
              // if lower order derivative don't work, neither should this
              Assert (spacedim == 3, ExcNotImplemented());

              Assert(data.shape_fourth_derivatives.size()==n_shape_functions*n_points,
                     ExcInternalError());
              Tensor<4,3> zero;
              for (unsigned int i=0; i<8; ++i)
                data.fourth_derivative(k,i) = zero;
            }
        }
    }
  }
}


namespace
{
  template <int dim>
  std::vector<unsigned int>
  get_dpo_vector (const unsigned int degree)
  {
    std::vector<unsigned int> dpo(dim+1, 1U);
    for (unsigned int i=1; i<dpo.size(); ++i)
      dpo[i]=dpo[i-1]*(degree-1);
    return dpo;
  }
}



template<int dim, int spacedim>
void
MappingQGeneric<dim,spacedim>::InternalData::
compute_shape_function_values (const std::vector<Point<dim> > &unit_points)
{
  // if the polynomial degree is one, then we can simplify code a bit
  // by using hard-coded shape functions.
  if ((polynomial_degree == 1)
      &&
      (dim == spacedim))
    internal::MappingQGeneric::compute_shape_function_values<spacedim> (n_shape_functions,
        unit_points, *this);
  else
    // otherwise ask an object that describes the polynomial space
    {
      const unsigned int n_points=unit_points.size();

      // Construct the tensor product polynomials used as shape functions for the
      // Qp mapping of cells at the boundary.
      const QGaussLobatto<1> line_support_points (polynomial_degree + 1);
      const TensorProductPolynomials<dim>
      tensor_pols (Polynomials::generate_complete_Lagrange_basis(line_support_points.get_points()));
      Assert (n_shape_functions==tensor_pols.n(),
              ExcInternalError());

      // then also construct the mapping from lexicographic to the Qp shape function numbering
      const std::vector<unsigned int>
      renumber (FETools::
                lexicographic_to_hierarchic_numbering (
                  FiniteElementData<dim> (get_dpo_vector<dim>(polynomial_degree), 1,
                                          polynomial_degree)));

      std::vector<double> values;
      std::vector<Tensor<1,dim> > grads;
      if (shape_values.size()!=0)
        {
          Assert(shape_values.size()==n_shape_functions*n_points,
                 ExcInternalError());
          values.resize(n_shape_functions);
        }
      if (shape_derivatives.size()!=0)
        {
          Assert(shape_derivatives.size()==n_shape_functions*n_points,
                 ExcInternalError());
          grads.resize(n_shape_functions);
        }

      std::vector<Tensor<2,dim> > grad2;
      if (shape_second_derivatives.size()!=0)
        {
          Assert(shape_second_derivatives.size()==n_shape_functions*n_points,
                 ExcInternalError());
          grad2.resize(n_shape_functions);
        }

      std::vector<Tensor<3,dim> > grad3;
      if (shape_third_derivatives.size()!=0)
        {
          Assert(shape_third_derivatives.size()==n_shape_functions*n_points,
                 ExcInternalError());
          grad3.resize(n_shape_functions);
        }

      std::vector<Tensor<4,dim> > grad4;
      if (shape_fourth_derivatives.size()!=0)
        {
          Assert(shape_fourth_derivatives.size()==n_shape_functions*n_points,
                 ExcInternalError());
          grad4.resize(n_shape_functions);
        }


      if (shape_values.size()!=0 ||
          shape_derivatives.size()!=0 ||
          shape_second_derivatives.size()!=0 ||
          shape_third_derivatives.size()!=0 ||
          shape_fourth_derivatives.size()!=0 )
        for (unsigned int point=0; point<n_points; ++point)
          {
            tensor_pols.compute(unit_points[point], values, grads, grad2, grad3, grad4);

            if (shape_values.size()!=0)
              for (unsigned int i=0; i<n_shape_functions; ++i)
                shape(point,renumber[i]) = values[i];

            if (shape_derivatives.size()!=0)
              for (unsigned int i=0; i<n_shape_functions; ++i)
                derivative(point,renumber[i]) = grads[i];

            if (shape_second_derivatives.size()!=0)
              for (unsigned int i=0; i<n_shape_functions; ++i)
                second_derivative(point,renumber[i]) = grad2[i];

            if (shape_third_derivatives.size()!=0)
              for (unsigned int i=0; i<n_shape_functions; ++i)
                third_derivative(point,renumber[i]) = grad3[i];

            if (shape_fourth_derivatives.size()!=0)
              for (unsigned int i=0; i<n_shape_functions; ++i)
                fourth_derivative(point,renumber[i]) = grad4[i];
          }
    }
}



template<int dim, int spacedim>
MappingQGeneric<dim,spacedim>::MappingQGeneric (const unsigned int p)
  :
  polynomial_degree(p)
{}



template<int dim, int spacedim>
unsigned int
MappingQGeneric<dim,spacedim>::get_degree() const
{
  return polynomial_degree;
}



template<int dim, int spacedim>
UpdateFlags
MappingQGeneric<dim,spacedim>::requires_update_flags (const UpdateFlags in) const
{
  // add flags if the respective quantities are necessary to compute
  // what we need. note that some flags appear in both the conditions
  // and in subsequent set operations. this leads to some circular
  // logic. the only way to treat this is to iterate. since there are
  // 5 if-clauses in the loop, it will take at most 5 iterations to
  // converge. do them:
  UpdateFlags out = in;
  for (unsigned int i=0; i<5; ++i)
    {
      // The following is a little incorrect:
      // If not applied on a face,
      // update_boundary_forms does not
      // make sense. On the other hand,
      // it is necessary on a
      // face. Currently,
      // update_boundary_forms is simply
      // ignored for the interior of a
      // cell.
      if (out & (update_JxW_values
                 | update_normal_vectors))
        out |= update_boundary_forms;

      if (out & (update_covariant_transformation
                 | update_JxW_values
                 | update_jacobians
                 | update_jacobian_grads
                 | update_boundary_forms
                 | update_normal_vectors))
        out |= update_contravariant_transformation;

      if (out & (update_inverse_jacobians
                 | update_jacobian_pushed_forward_grads
                 | update_jacobian_pushed_forward_2nd_derivatives
                 | update_jacobian_pushed_forward_3rd_derivatives) )
        out |= update_covariant_transformation;

      // The contravariant transformation
      // used in the Piola transformation, which
      // requires the determinant of the
      // Jacobi matrix of the transformation.
      // Because we have no way of knowing here whether the finite
      // elements wants to use the contravariant of the Piola
      // transforms, we add the JxW values to the list of flags to be
      // updated for each cell.
      if (out & update_contravariant_transformation)
        out |= update_JxW_values;

      if (out & update_normal_vectors)
        out |= update_JxW_values;
    }

  return out;
}



template<int dim, int spacedim>
typename MappingQGeneric<dim,spacedim>::InternalData *
MappingQGeneric<dim,spacedim>::get_data (const UpdateFlags update_flags,
                                         const Quadrature<dim> &q) const
{
  InternalData *data = new InternalData(polynomial_degree);
  data->initialize (this->requires_update_flags(update_flags), q, q.size());

  return data;
}



template<int dim, int spacedim>
typename Mapping<dim,spacedim>::InternalDataBase *
MappingQGeneric<dim,spacedim>::get_face_data (const UpdateFlags        update_flags,
                                              const Quadrature<dim-1> &quadrature) const
{
  InternalData *data = new InternalData(polynomial_degree);
  data->initialize_face (this->requires_update_flags(update_flags),
                         QProjector<dim>::project_to_all_faces(quadrature),
                         quadrature.size());

  return data;
}



template<int dim, int spacedim>
typename Mapping<dim,spacedim>::InternalDataBase *
MappingQGeneric<dim,spacedim>::get_subface_data (const UpdateFlags update_flags,
                                                 const Quadrature<dim-1>& quadrature) const
{
  InternalData *data = new InternalData(polynomial_degree);
  data->initialize_face (this->requires_update_flags(update_flags),
                         QProjector<dim>::project_to_all_subfaces(quadrature),
                         quadrature.size());

  return data;
}



namespace internal
{
  namespace
  {
    /**
     * Compute the locations of quadrature points on the object described by
     * the first argument (and the cell for which the mapping support points
     * have already been set), but only if the update_flags of the @p data
     * argument indicate so.
     */
    template <int dim, int spacedim>
    void
    maybe_compute_q_points (const typename QProjector<dim>::DataSetDescriptor                 data_set,
                            const typename dealii::MappingQGeneric<dim,spacedim>::InternalData      &data,
                            std::vector<Point<spacedim> >                                     &quadrature_points)
    {
      const UpdateFlags update_flags = data.update_each;

      if (update_flags & update_quadrature_points)
        {
          for (unsigned int point=0; point<quadrature_points.size(); ++point)
            {
              const double *shape = &data.shape(point+data_set,0);
              Point<spacedim> result = (shape[0] *
                                        data.mapping_support_points[0]);
              for (unsigned int k=1; k<data.n_shape_functions; ++k)
                for (unsigned int i=0; i<spacedim; ++i)
                  result[i] += shape[k] * data.mapping_support_points[k][i];
              quadrature_points[point] = result;
            }
        }
    }


    /**
     * Update the co- and contravariant matrices as well as their determinant, for the cell
     * described stored in the data object, but only if the update_flags of the @p data
     * argument indicate so.
     *
     * Skip the computation if possible as indicated by the first argument.
     */
    template <int dim, int spacedim>
    void
    maybe_update_Jacobians (const CellSimilarity::Similarity                                   cell_similarity,
                            const typename dealii::QProjector<dim>::DataSetDescriptor          data_set,
                            const typename dealii::MappingQGeneric<dim,spacedim>::InternalData      &data)
    {
      const UpdateFlags update_flags = data.update_each;

      if (update_flags & update_contravariant_transformation)
        // if the current cell is just a
        // translation of the previous one, no
        // need to recompute jacobians...
        if (cell_similarity != CellSimilarity::translation)
          {
            const unsigned int n_q_points = data.contravariant.size();

            std::fill(data.contravariant.begin(), data.contravariant.end(),
                      DerivativeForm<1,dim,spacedim>());

            Assert (data.n_shape_functions > 0, ExcInternalError());
            const Tensor<1,spacedim> *supp_pts =
              &data.mapping_support_points[0];

            for (unsigned int point=0; point<n_q_points; ++point)
              {
                const Tensor<1,dim> *data_derv =
                  &data.derivative(point+data_set, 0);

                double result [spacedim][dim];

                // peel away part of sum to avoid zeroing the
                // entries and adding for the first time
                for (unsigned int i=0; i<spacedim; ++i)
                  for (unsigned int j=0; j<dim; ++j)
                    result[i][j] = data_derv[0][j] * supp_pts[0][i];
                for (unsigned int k=1; k<data.n_shape_functions; ++k)
                  for (unsigned int i=0; i<spacedim; ++i)
                    for (unsigned int j=0; j<dim; ++j)
                      result[i][j] += data_derv[k][j] * supp_pts[k][i];

                // write result into contravariant data. for
                // j=dim in the case dim<spacedim, there will
                // never be any nonzero data that arrives in
                // here, so it is ok anyway because it was
                // initialized to zero at the initialization
                for (unsigned int i=0; i<spacedim; ++i)
                  for (unsigned int j=0; j<dim; ++j)
                    data.contravariant[point][i][j] = result[i][j];
              }
          }

      if (update_flags & update_covariant_transformation)
        if (cell_similarity != CellSimilarity::translation)
          {
            const unsigned int n_q_points = data.contravariant.size();
            for (unsigned int point=0; point<n_q_points; ++point)
              {
                data.covariant[point] = (data.contravariant[point]).covariant_form();
              }
          }

      if (update_flags & update_volume_elements)
        if (cell_similarity != CellSimilarity::translation)
          {
            const unsigned int n_q_points = data.contravariant.size();
            for (unsigned int point=0; point<n_q_points; ++point)
              data.volume_elements[point] = data.contravariant[point].determinant();
          }

    }

    /**
     * Update the Hessian of the transformation from unit to real cell, the
     * Jacobian gradients.
     *
     * Skip the computation if possible as indicated by the first argument.
     */
    template <int dim, int spacedim>
    void
    maybe_update_jacobian_grads (const CellSimilarity::Similarity                                   cell_similarity,
                                 const typename QProjector<dim>::DataSetDescriptor                  data_set,
                                 const typename dealii::MappingQGeneric<dim,spacedim>::InternalData      &data,
                                 std::vector<DerivativeForm<2,dim,spacedim> >                      &jacobian_grads)
    {
      const UpdateFlags update_flags = data.update_each;
      if (update_flags & update_jacobian_grads)
        {
          const unsigned int n_q_points = jacobian_grads.size();

          if (cell_similarity != CellSimilarity::translation)
            {
              for (unsigned int point=0; point<n_q_points; ++point)
                {
                  const Tensor<2,dim> *second =
                    &data.second_derivative(point+data_set, 0);
                  double result [spacedim][dim][dim];
                  for (unsigned int i=0; i<spacedim; ++i)
                    for (unsigned int j=0; j<dim; ++j)
                      for (unsigned int l=0; l<dim; ++l)
                        result[i][j][l] = (second[0][j][l] *
                                           data.mapping_support_points[0][i]);
                  for (unsigned int k=1; k<data.n_shape_functions; ++k)
                    for (unsigned int i=0; i<spacedim; ++i)
                      for (unsigned int j=0; j<dim; ++j)
                        for (unsigned int l=0; l<dim; ++l)
                          result[i][j][l]
                          += (second[k][j][l]
                              *
                              data.mapping_support_points[k][i]);

                  for (unsigned int i=0; i<spacedim; ++i)
                    for (unsigned int j=0; j<dim; ++j)
                      for (unsigned int l=0; l<dim; ++l)
                        jacobian_grads[point][i][j][l] = result[i][j][l];
                }
            }
        }
    }

    /**
     * Update the Hessian of the transformation from unit to real cell, the
     * Jacobian gradients, pushed forward to the real cell coordinates.
     *
     * Skip the computation if possible as indicated by the first argument.
     */
    template <int dim, int spacedim>
    void
    maybe_update_jacobian_pushed_forward_grads (const CellSimilarity::Similarity                                   cell_similarity,
                                                const typename QProjector<dim>::DataSetDescriptor                  data_set,
                                                const typename dealii::MappingQGeneric<dim,spacedim>::InternalData      &data,
                                                std::vector<Tensor<3,spacedim> >                      &jacobian_pushed_forward_grads)
    {
      const UpdateFlags update_flags = data.update_each;
      if (update_flags & update_jacobian_pushed_forward_grads)
        {
          const unsigned int n_q_points = jacobian_pushed_forward_grads.size();

          if (cell_similarity != CellSimilarity::translation)
            {
              for (unsigned int point=0; point<n_q_points; ++point)
                {
                  const Tensor<2,dim> *second =
                    &data.second_derivative(point+data_set, 0);
                  double result [spacedim][dim][dim];
                  for (unsigned int i=0; i<spacedim; ++i)
                    for (unsigned int j=0; j<dim; ++j)
                      for (unsigned int l=0; l<dim; ++l)
                        result[i][j][l] = (second[0][j][l] *
                                           data.mapping_support_points[0][i]);
                  for (unsigned int k=1; k<data.n_shape_functions; ++k)
                    for (unsigned int i=0; i<spacedim; ++i)
                      for (unsigned int j=0; j<dim; ++j)
                        for (unsigned int l=0; l<dim; ++l)
                          result[i][j][l]
                          += (second[k][j][l]
                              *
                              data.mapping_support_points[k][i]);

                  // pushing forward the derivative coordinates
                  for (unsigned int i=0; i<spacedim; ++i)
                    for (unsigned int j=0; j<spacedim; ++j)
                      for (unsigned int l=0; l<spacedim; ++l)
                        {
                          jacobian_pushed_forward_grads[point][i][j][l] = result[i][0][0] *
                                                                          data.covariant[point][j][0] *
                                                                          data.covariant[point][l][0];
                          for (unsigned int jr=0; jr<dim; ++jr)
                            {
                              const unsigned int lr_start = jr==0? 1:0;
                              for (unsigned int lr=lr_start; lr<dim; ++lr)
                                jacobian_pushed_forward_grads[point][i][j][l] += result[i][jr][lr] *
                                                                                 data.covariant[point][j][jr] *
                                                                                 data.covariant[point][l][lr];
                            }
                        }
                }
            }
        }
    }

    /**
     * Update the third derivatives of the transformation from unit to real cell, the
     * Jacobian hessians.
     *
     * Skip the computation if possible as indicated by the first argument.
     */
    template <int dim, int spacedim>
    void
    maybe_update_jacobian_2nd_derivatives (const CellSimilarity::Similarity                              cell_similarity,
                                           const typename QProjector<dim>::DataSetDescriptor             data_set,
                                           const typename dealii::MappingQGeneric<dim,spacedim>::InternalData &data,
                                           std::vector<DerivativeForm<3,dim,spacedim> >                 &jacobian_2nd_derivatives)
    {
      const UpdateFlags update_flags = data.update_each;
      if (update_flags & update_jacobian_2nd_derivatives)
        {
          const unsigned int n_q_points = jacobian_2nd_derivatives.size();

          if (cell_similarity != CellSimilarity::translation)
            {
              for (unsigned int point=0; point<n_q_points; ++point)
                {
                  const Tensor<3,dim> *third =
                    &data.third_derivative(point+data_set, 0);
                  double result [spacedim][dim][dim][dim];
                  for (unsigned int i=0; i<spacedim; ++i)
                    for (unsigned int j=0; j<dim; ++j)
                      for (unsigned int l=0; l<dim; ++l)
                        for (unsigned int m=0; m<dim; ++m)
                          result[i][j][l][m] = (third[0][j][l][m] *
                                                data.mapping_support_points[0][i]);
                  for (unsigned int k=1; k<data.n_shape_functions; ++k)
                    for (unsigned int i=0; i<spacedim; ++i)
                      for (unsigned int j=0; j<dim; ++j)
                        for (unsigned int l=0; l<dim; ++l)
                          for (unsigned int m=0; m<dim; ++m)
                            result[i][j][l][m]
                            += (third[k][j][l][m]
                                *
                                data.mapping_support_points[k][i]);

                  for (unsigned int i=0; i<spacedim; ++i)
                    for (unsigned int j=0; j<dim; ++j)
                      for (unsigned int l=0; l<dim; ++l)
                        for (unsigned int m=0; m<dim; ++m)
                          jacobian_2nd_derivatives[point][i][j][l][m] = result[i][j][l][m];
                }
            }
        }
    }

    /**
     * Update the Hessian of the Hessian of the transformation from unit
     * to real cell, the Jacobian Hessian gradients, pushed forward to the
     * real cell coordinates.
     *
     * Skip the computation if possible as indicated by the first argument.
     */
    template <int dim, int spacedim>
    void
    maybe_update_jacobian_pushed_forward_2nd_derivatives (const CellSimilarity::Similarity                                   cell_similarity,
                                                          const typename QProjector<dim>::DataSetDescriptor                  data_set,
                                                          const typename dealii::MappingQGeneric<dim,spacedim>::InternalData      &data,
                                                          std::vector<Tensor<4,spacedim> >                      &jacobian_pushed_forward_2nd_derivatives)
    {
      const UpdateFlags update_flags = data.update_each;
      if (update_flags & update_jacobian_pushed_forward_2nd_derivatives)
        {
          const unsigned int n_q_points = jacobian_pushed_forward_2nd_derivatives.size();

          if (cell_similarity != CellSimilarity::translation)
            {
              for (unsigned int point=0; point<n_q_points; ++point)
                {
                  const Tensor<3,dim> *third =
                    &data.third_derivative(point+data_set, 0);
                  double result [spacedim][dim][dim][dim];
                  for (unsigned int i=0; i<spacedim; ++i)
                    for (unsigned int j=0; j<dim; ++j)
                      for (unsigned int l=0; l<dim; ++l)
                        for (unsigned int m=0; m<dim; ++m)
                          result[i][j][l][m] = (third[0][j][l][m] *
                                                data.mapping_support_points[0][i]);
                  for (unsigned int k=1; k<data.n_shape_functions; ++k)
                    for (unsigned int i=0; i<spacedim; ++i)
                      for (unsigned int j=0; j<dim; ++j)
                        for (unsigned int l=0; l<dim; ++l)
                          for (unsigned int m=0; m<dim; ++m)
                            result[i][j][l][m]
                            += (third[k][j][l][m]
                                *
                                data.mapping_support_points[k][i]);

                  for (unsigned int i=0; i<spacedim; ++i)
                    for (unsigned int j=0; j<spacedim; ++j)
                      for (unsigned int l=0; l<spacedim; ++l)
                        for (unsigned int m=0; m<spacedim; ++m)
                          {
                            jacobian_pushed_forward_2nd_derivatives[point][i][j][l][m]
                              = result[i][0][0][0]*
                                data.covariant[point][j][0] *
                                data.covariant[point][l][0] *
                                data.covariant[point][m][0];
                            for (unsigned int jr=0; jr<dim; ++jr)
                              for (unsigned int lr=0; lr<dim; ++lr)
                                {
                                  const unsigned int mr0 = (jr+lr == 0)? 1:0;
                                  for (unsigned int mr=mr0; mr<dim; ++mr)
                                    jacobian_pushed_forward_2nd_derivatives[point][i][j][l][m]
                                    += result[i][jr][lr][mr] *
                                       data.covariant[point][j][jr] *
                                       data.covariant[point][l][lr] *
                                       data.covariant[point][m][mr];
                                }
                          }
                }
            }
        }
    }

    /**
         * Update the fourth derivatives of the transformation from unit to real cell, the
         * Jacobian hessian gradients.
         *
         * Skip the computation if possible as indicated by the first argument.
         */
    template <int dim, int spacedim>
    void
    maybe_update_jacobian_3rd_derivatives (const CellSimilarity::Similarity                              cell_similarity,
                                           const typename QProjector<dim>::DataSetDescriptor             data_set,
                                           const typename dealii::MappingQGeneric<dim,spacedim>::InternalData &data,
                                           std::vector<DerivativeForm<4,dim,spacedim> >                 &jacobian_3rd_derivatives)
    {
      const UpdateFlags update_flags = data.update_each;
      if (update_flags & update_jacobian_3rd_derivatives)
        {
          const unsigned int n_q_points = jacobian_3rd_derivatives.size();

          if (cell_similarity != CellSimilarity::translation)
            {
              for (unsigned int point=0; point<n_q_points; ++point)
                {
                  const Tensor<4,dim> *fourth =
                    &data.fourth_derivative(point+data_set, 0);
                  double result [spacedim][dim][dim][dim][dim];
                  for (unsigned int i=0; i<spacedim; ++i)
                    for (unsigned int j=0; j<dim; ++j)
                      for (unsigned int l=0; l<dim; ++l)
                        for (unsigned int m=0; m<dim; ++m)
                          for (unsigned int n=0; n<dim; ++n)
                            result[i][j][l][m][n] = (fourth[0][j][l][m][n] *
                                                     data.mapping_support_points[0][i]);
                  for (unsigned int k=1; k<data.n_shape_functions; ++k)
                    for (unsigned int i=0; i<spacedim; ++i)
                      for (unsigned int j=0; j<dim; ++j)
                        for (unsigned int l=0; l<dim; ++l)
                          for (unsigned int m=0; m<dim; ++m)
                            for (unsigned int n=0; n<dim; ++n)
                              result[i][j][l][m][n]
                              += (fourth[k][j][l][m][n]
                                  *
                                  data.mapping_support_points[k][i]);

                  for (unsigned int i=0; i<spacedim; ++i)
                    for (unsigned int j=0; j<dim; ++j)
                      for (unsigned int l=0; l<dim; ++l)
                        for (unsigned int m=0; m<dim; ++m)
                          for (unsigned int n=0; n<dim; ++n)
                            jacobian_3rd_derivatives[point][i][j][l][m][n] = result[i][j][l][m][n];
                }
            }
        }
    }

    /**
     * Update the Hessian gradient of the transformation from unit to real cell, the
     * Jacobian Hessians, pushed forward to the real cell coordinates.
     *
     * Skip the computation if possible as indicated by the first argument.
     */
    template <int dim, int spacedim>
    void
    maybe_update_jacobian_pushed_forward_3rd_derivatives (const CellSimilarity::Similarity                                   cell_similarity,
                                                          const typename QProjector<dim>::DataSetDescriptor                  data_set,
                                                          const typename dealii::MappingQGeneric<dim,spacedim>::InternalData      &data,
                                                          std::vector<Tensor<5,spacedim> >                      &jacobian_pushed_forward_3rd_derivatives)
    {
      const UpdateFlags update_flags = data.update_each;
      if (update_flags & update_jacobian_pushed_forward_3rd_derivatives)
        {
          const unsigned int n_q_points = jacobian_pushed_forward_3rd_derivatives.size();

          if (cell_similarity != CellSimilarity::translation)
            {
              for (unsigned int point=0; point<n_q_points; ++point)
                {
                  const Tensor<4,dim> *fourth =
                    &data.fourth_derivative(point+data_set, 0);
                  double result [spacedim][dim][dim][dim][dim];
                  for (unsigned int i=0; i<spacedim; ++i)
                    for (unsigned int j=0; j<dim; ++j)
                      for (unsigned int l=0; l<dim; ++l)
                        for (unsigned int m=0; m<dim; ++m)
                          for (unsigned int n=0; n<dim; ++n)
                            result[i][j][l][m][n] = (fourth[0][j][l][m][n] *
                                                     data.mapping_support_points[0][i]);
                  for (unsigned int k=1; k<data.n_shape_functions; ++k)
                    for (unsigned int i=0; i<spacedim; ++i)
                      for (unsigned int j=0; j<dim; ++j)
                        for (unsigned int l=0; l<dim; ++l)
                          for (unsigned int m=0; m<dim; ++m)
                            for (unsigned int n=0; n<dim; ++n)
                              result[i][j][l][m][n]
                              += (fourth[k][j][l][m][n]
                                  *
                                  data.mapping_support_points[k][i]);

                  for (unsigned int i=0; i<spacedim; ++i)
                    for (unsigned int j=0; j<spacedim; ++j)
                      for (unsigned int l=0; l<spacedim; ++l)
                        for (unsigned int m=0; m<spacedim; ++m)
                          for (unsigned int n=0; n<spacedim; ++n)
                            {
                              jacobian_pushed_forward_3rd_derivatives[point][i][j][l][m][n]
                                = result[i][0][0][0][0] *
                                  data.covariant[point][j][0] *
                                  data.covariant[point][l][0] *
                                  data.covariant[point][m][0] *
                                  data.covariant[point][n][0];
                              for (unsigned int jr=0; jr<dim; ++jr)
                                for (unsigned int lr=0; lr<dim; ++lr)
                                  for (unsigned int mr=0; mr<dim; ++mr)
                                    {
                                      const unsigned int nr0 = (jr+lr+mr==0)? 1:0;
                                      for (unsigned int nr=nr0; nr<dim; ++nr)
                                        jacobian_pushed_forward_3rd_derivatives[point][i][j][l][m][n]
                                        += result[i][jr][lr][mr][nr] *
                                           data.covariant[point][j][jr] *
                                           data.covariant[point][l][lr] *
                                           data.covariant[point][m][mr] *
                                           data.covariant[point][n][nr];
                                    }
                            }
                }
            }
        }
    }
  }
}




template<int dim, int spacedim>
CellSimilarity::Similarity
MappingQGeneric<dim,spacedim>::
fill_fe_values (const typename Triangulation<dim,spacedim>::cell_iterator &cell,
                const CellSimilarity::Similarity                           cell_similarity,
                const Quadrature<dim>                                     &quadrature,
                const typename Mapping<dim,spacedim>::InternalDataBase    &internal_data,
                internal::FEValues::MappingRelatedData<dim,spacedim>      &output_data) const
{
  // ensure that the following static_cast is really correct:
  Assert (dynamic_cast<const InternalData *>(&internal_data) != 0,
          ExcInternalError());
  const InternalData &data = static_cast<const InternalData &>(internal_data);

  const unsigned int n_q_points=quadrature.size();

  // if necessary, recompute the support points of the transformation of this cell
  // (note that we need to first check the triangulation pointer, since otherwise
  // the second test might trigger an exception if the triangulations are not the
  // same)
  if ((data.mapping_support_points.size() == 0)
      ||
      (&cell->get_triangulation() !=
       &data.cell_of_current_support_points->get_triangulation())
      ||
      (cell != data.cell_of_current_support_points))
    {
      compute_mapping_support_points(cell, data.mapping_support_points);
      data.cell_of_current_support_points = cell;
    }

  internal::maybe_compute_q_points<dim,spacedim> (QProjector<dim>::DataSetDescriptor::cell (),
                                                  data,
                                                  output_data.quadrature_points);
  internal::maybe_update_Jacobians<dim,spacedim> (cell_similarity,
                                                  QProjector<dim>::DataSetDescriptor::cell (),
                                                  data);

  const UpdateFlags update_flags = data.update_each;
  const std::vector<double> &weights=quadrature.get_weights();

  // Multiply quadrature weights by absolute value of Jacobian determinants or
  // the area element g=sqrt(DX^t DX) in case of codim > 0

  if (update_flags & (update_normal_vectors
                      | update_JxW_values))
    {
      AssertDimension (output_data.JxW_values.size(), n_q_points);

      Assert( !(update_flags & update_normal_vectors ) ||
              (output_data.normal_vectors.size() == n_q_points),
              ExcDimensionMismatch(output_data.normal_vectors.size(), n_q_points));


      if (cell_similarity != CellSimilarity::translation)
        for (unsigned int point=0; point<n_q_points; ++point)
          {

            if (dim == spacedim)
              {
                const double det = data.contravariant[point].determinant();

                // check for distorted cells.

                // TODO: this allows for anisotropies of up to 1e6 in 3D and
                // 1e12 in 2D. might want to find a finer
                // (dimension-independent) criterion
                Assert (det > 1e-12*Utilities::fixed_power<dim>(cell->diameter()/
                                                                std::sqrt(double(dim))),
                        (typename Mapping<dim,spacedim>::ExcDistortedMappedCell(cell->center(), det, point)));

                output_data.JxW_values[point] = weights[point] * det;
              }
            // if dim==spacedim, then there is no cell normal to
            // compute. since this is for FEValues (and not FEFaceValues),
            // there are also no face normals to compute
            else //codim>0 case
              {
                Tensor<1, spacedim> DX_t [dim];
                for (unsigned int i=0; i<spacedim; ++i)
                  for (unsigned int j=0; j<dim; ++j)
                    DX_t[j][i] = data.contravariant[point][i][j];

                Tensor<2, dim> G; //First fundamental form
                for (unsigned int i=0; i<dim; ++i)
                  for (unsigned int j=0; j<dim; ++j)
                    G[i][j] = DX_t[i] * DX_t[j];

                output_data.JxW_values[point]
                  = sqrt(determinant(G)) * weights[point];

                if (cell_similarity == CellSimilarity::inverted_translation)
                  {
                    // we only need to flip the normal
                    if (update_flags & update_normal_vectors)
                      output_data.normal_vectors[point] *= -1.;
                  }
                else
                  {
                    const unsigned int codim = spacedim-dim;
                    (void)codim;

                    if (update_flags & update_normal_vectors)
                      {
                        Assert( codim==1 , ExcMessage("There is no cell normal in codim 2."));

                        if (dim==1)
                          cross_product(output_data.normal_vectors[point],
                                        -DX_t[0]);
                        else //dim == 2
                          cross_product(output_data.normal_vectors[point],DX_t[0],DX_t[1]);

                        output_data.normal_vectors[point] /= output_data.normal_vectors[point].norm();

                        if (cell->direction_flag() == false)
                          output_data.normal_vectors[point] *= -1.;
                      }

                  }
              } //codim>0 case

          }
    }



  // copy values from InternalData to vector given by reference
  if (update_flags & update_jacobians)
    {
      AssertDimension (output_data.jacobians.size(), n_q_points);
      if (cell_similarity != CellSimilarity::translation)
        for (unsigned int point=0; point<n_q_points; ++point)
          output_data.jacobians[point] = data.contravariant[point];
    }

  // copy values from InternalData to vector given by reference
  if (update_flags & update_inverse_jacobians)
    {
      AssertDimension (output_data.inverse_jacobians.size(), n_q_points);
      if (cell_similarity != CellSimilarity::translation)
        for (unsigned int point=0; point<n_q_points; ++point)
          output_data.inverse_jacobians[point] = data.covariant[point].transpose();
    }

  internal::maybe_update_jacobian_grads<dim,spacedim> (cell_similarity,
                                                       QProjector<dim>::DataSetDescriptor::cell (),
                                                       data,
                                                       output_data.jacobian_grads);

  internal::maybe_update_jacobian_pushed_forward_grads<dim,spacedim> (cell_similarity,
      QProjector<dim>::DataSetDescriptor::cell (),
      data,
      output_data.jacobian_pushed_forward_grads);

  internal::maybe_update_jacobian_2nd_derivatives<dim,spacedim> (cell_similarity,
      QProjector<dim>::DataSetDescriptor::cell (),
      data,
      output_data.jacobian_2nd_derivatives);

  internal::maybe_update_jacobian_pushed_forward_2nd_derivatives<dim,spacedim> (cell_similarity,
      QProjector<dim>::DataSetDescriptor::cell (),
      data,
      output_data.jacobian_pushed_forward_2nd_derivatives);

  internal::maybe_update_jacobian_3rd_derivatives<dim,spacedim> (cell_similarity,
      QProjector<dim>::DataSetDescriptor::cell (),
      data,
      output_data.jacobian_3rd_derivatives);

  internal::maybe_update_jacobian_pushed_forward_3rd_derivatives<dim,spacedim> (cell_similarity,
      QProjector<dim>::DataSetDescriptor::cell (),
      data,
      output_data.jacobian_pushed_forward_3rd_derivatives);

  return cell_similarity;
}






namespace internal
{
  namespace
  {
    /**
     * Depending on what information is called for in the update flags of the
     * @p data object, compute the various pieces of information that is required
     * by the fill_fe_face_values() and fill_fe_subface_values() functions.
     * This function simply unifies the work that would be done by
     * those two functions.
     *
     * The resulting data is put into the @p output_data argument.
     */
    template <int dim, int spacedim>
    void
    maybe_compute_face_data (const dealii::MappingQGeneric<dim,spacedim> &mapping,
                             const typename dealii::Triangulation<dim,spacedim>::cell_iterator &cell,
                             const unsigned int               face_no,
                             const unsigned int               subface_no,
                             const unsigned int               n_q_points,
                             const std::vector<double>        &weights,
                             const typename dealii::MappingQGeneric<dim,spacedim>::InternalData &data,
                             internal::FEValues::MappingRelatedData<dim,spacedim>         &output_data)
    {
      const UpdateFlags update_flags = data.update_each;

      if (update_flags & update_boundary_forms)
        {
          AssertDimension (output_data.boundary_forms.size(), n_q_points);
          if (update_flags & update_normal_vectors)
            AssertDimension (output_data.normal_vectors.size(), n_q_points);
          if (update_flags & update_JxW_values)
            AssertDimension (output_data.JxW_values.size(), n_q_points);

          // map the unit tangentials to the real cell. checking for d!=dim-1
          // eliminates compiler warnings regarding unsigned int expressions <
          // 0.
          for (unsigned int d=0; d!=dim-1; ++d)
            {
              Assert (face_no+GeometryInfo<dim>::faces_per_cell*d <
                      data.unit_tangentials.size(),
                      ExcInternalError());
              Assert (data.aux[d].size() <=
                      data.unit_tangentials[face_no+GeometryInfo<dim>::faces_per_cell*d].size(),
                      ExcInternalError());

              mapping.transform (data.unit_tangentials[face_no+GeometryInfo<dim>::faces_per_cell*d],
                                 mapping_contravariant,
                                 data,
                                 data.aux[d]);
            }

          // if dim==spacedim, we can use the unit tangentials to compute the
          // boundary form by simply taking the cross product
          if (dim == spacedim)
            {
              for (unsigned int i=0; i<n_q_points; ++i)
                switch (dim)
                  {
                  case 1:
                    // in 1d, we don't have access to any of the data.aux
                    // fields (because it has only dim-1 components), but we
                    // can still compute the boundary form by simply
                    // looking at the number of the face
                    output_data.boundary_forms[i][0] = (face_no == 0 ?
                                                        -1 : +1);
                    break;
                  case 2:
                    cross_product (output_data.boundary_forms[i], data.aux[0][i]);
                    break;
                  case 3:
                    cross_product (output_data.boundary_forms[i], data.aux[0][i], data.aux[1][i]);
                    break;
                  default:
                    Assert(false, ExcNotImplemented());
                  }
            }
          else //(dim < spacedim)
            {
              // in the codim-one case, the boundary form results from the
              // cross product of all the face tangential vectors and the cell
              // normal vector
              //
              // to compute the cell normal, use the same method used in
              // fill_fe_values for cells above
              AssertDimension (data.contravariant.size(), n_q_points);

              for (unsigned int point=0; point<n_q_points; ++point)
                {
                  if (dim==1)
                    {
                      // J is a tangent vector
                      output_data.boundary_forms[point] = data.contravariant[point].transpose()[0];
                      output_data.boundary_forms[point] /=
                        (face_no == 0 ? -1. : +1.) * output_data.boundary_forms[point].norm();
                    }

                  if (dim==2)
                    {
                      Tensor<1,spacedim> cell_normal;
                      const DerivativeForm<1,spacedim,dim> DX_t =
                        data.contravariant[point].transpose();
                      cross_product(cell_normal,DX_t[0],DX_t[1]);
                      cell_normal /= cell_normal.norm();

                      // then compute the face normal from the face tangent
                      // and the cell normal:
                      cross_product (output_data.boundary_forms[point],
                                     data.aux[0][point], cell_normal);
                    }
                }
            }

          if (update_flags & (update_normal_vectors
                              | update_JxW_values))
            for (unsigned int i=0; i<output_data.boundary_forms.size(); ++i)
              {
                if (update_flags & update_JxW_values)
                  {
                    output_data.JxW_values[i] = output_data.boundary_forms[i].norm() * weights[i];

                    if (subface_no!=numbers::invalid_unsigned_int)
                      {
                        const double area_ratio=GeometryInfo<dim>::subface_ratio(
                                                  cell->subface_case(face_no), subface_no);
                        output_data.JxW_values[i] *= area_ratio;
                      }
                  }

                if (update_flags & update_normal_vectors)
                  output_data.normal_vectors[i] = Point<spacedim>(output_data.boundary_forms[i] /
                                                                  output_data.boundary_forms[i].norm());
              }

          if (update_flags & update_jacobians)
            for (unsigned int point=0; point<n_q_points; ++point)
              output_data.jacobians[point] = data.contravariant[point];

          if (update_flags & update_inverse_jacobians)
            for (unsigned int point=0; point<n_q_points; ++point)
              output_data.inverse_jacobians[point] = data.covariant[point].transpose();
        }
    }


    /**
     * Do the work of MappingQGeneric::fill_fe_face_values() and
     * MappingQGeneric::fill_fe_subface_values() in a generic way,
     * using the 'data_set' to differentiate whether we will
     * work on a face (and if so, which one) or subface.
     */
    template<int dim, int spacedim>
    void
    do_fill_fe_face_values (const dealii::MappingQGeneric<dim,spacedim>                             &mapping,
                            const typename dealii::Triangulation<dim,spacedim>::cell_iterator &cell,
                            const unsigned int                                                 face_no,
                            const unsigned int                                                 subface_no,
                            const typename QProjector<dim>::DataSetDescriptor                  data_set,
                            const Quadrature<dim-1>                                           &quadrature,
                            const typename dealii::MappingQGeneric<dim,spacedim>::InternalData      &data,
                            internal::FEValues::MappingRelatedData<dim,spacedim>              &output_data)
    {
      maybe_compute_q_points<dim,spacedim> (data_set,
                                            data,
                                            output_data.quadrature_points);
      maybe_update_Jacobians<dim,spacedim> (CellSimilarity::none,
                                            data_set,
                                            data);
      maybe_update_jacobian_grads<dim,spacedim> (CellSimilarity::none,
                                                 data_set,
                                                 data,
                                                 output_data.jacobian_grads);
      maybe_update_jacobian_pushed_forward_grads<dim,spacedim> (CellSimilarity::none,
                                                                data_set,
                                                                data,
                                                                output_data.jacobian_pushed_forward_grads);
      maybe_update_jacobian_2nd_derivatives<dim,spacedim> (CellSimilarity::none,
                                                           data_set,
                                                           data,
                                                           output_data.jacobian_2nd_derivatives);
      maybe_update_jacobian_pushed_forward_2nd_derivatives<dim,spacedim> (CellSimilarity::none,
          data_set,
          data,
          output_data.jacobian_pushed_forward_2nd_derivatives);
      maybe_update_jacobian_3rd_derivatives<dim,spacedim> (CellSimilarity::none,
                                                           data_set,
                                                           data,
                                                           output_data.jacobian_3rd_derivatives);
      maybe_update_jacobian_pushed_forward_3rd_derivatives<dim,spacedim> (CellSimilarity::none,
          data_set,
          data,
          output_data.jacobian_pushed_forward_3rd_derivatives);

      maybe_compute_face_data (mapping,
                               cell, face_no, subface_no, quadrature.size(),
                               quadrature.get_weights(), data,
                               output_data);
    }
  }
}



template<int dim, int spacedim>
void
MappingQGeneric<dim,spacedim>::
fill_fe_face_values (const typename Triangulation<dim,spacedim>::cell_iterator &cell,
                     const unsigned int                                         face_no,
                     const Quadrature<dim-1>                                   &quadrature,
                     const typename Mapping<dim,spacedim>::InternalDataBase    &internal_data,
                     internal::FEValues::MappingRelatedData<dim,spacedim>      &output_data) const
{
  // ensure that the following cast is really correct:
  Assert ((dynamic_cast<const InternalData *>(&internal_data) != 0),
          ExcInternalError());
  const InternalData &data
    = static_cast<const InternalData &>(internal_data);

  // if necessary, recompute the support points of the transformation of this cell
  // (note that we need to first check the triangulation pointer, since otherwise
  // the second test might trigger an exception if the triangulations are not the
  // same)
  if ((data.mapping_support_points.size() == 0)
      ||
      (&cell->get_triangulation() !=
       &data.cell_of_current_support_points->get_triangulation())
      ||
      (cell != data.cell_of_current_support_points))
    {
      compute_mapping_support_points(cell, data.mapping_support_points);
      data.cell_of_current_support_points = cell;
    }

  internal::do_fill_fe_face_values (*this,
                                    cell, face_no, numbers::invalid_unsigned_int,
                                    QProjector<dim>::DataSetDescriptor::face (face_no,
                                        cell->face_orientation(face_no),
                                        cell->face_flip(face_no),
                                        cell->face_rotation(face_no),
                                        quadrature.size()),
                                    quadrature,
                                    data,
                                    output_data);
}



template<int dim, int spacedim>
void
MappingQGeneric<dim,spacedim>::
fill_fe_subface_values (const typename Triangulation<dim,spacedim>::cell_iterator &cell,
                        const unsigned int                                         face_no,
                        const unsigned int                                         subface_no,
                        const Quadrature<dim-1>                                   &quadrature,
                        const typename Mapping<dim,spacedim>::InternalDataBase    &internal_data,
                        internal::FEValues::MappingRelatedData<dim,spacedim>      &output_data) const
{
  // ensure that the following cast is really correct:
  Assert ((dynamic_cast<const InternalData *>(&internal_data) != 0),
          ExcInternalError());
  const InternalData &data
    = static_cast<const InternalData &>(internal_data);

  // if necessary, recompute the support points of the transformation of this cell
  // (note that we need to first check the triangulation pointer, since otherwise
  // the second test might trigger an exception if the triangulations are not the
  // same)
  if ((data.mapping_support_points.size() == 0)
      ||
      (&cell->get_triangulation() !=
       &data.cell_of_current_support_points->get_triangulation())
      ||
      (cell != data.cell_of_current_support_points))
    {
      compute_mapping_support_points(cell, data.mapping_support_points);
      data.cell_of_current_support_points = cell;
    }

  internal::do_fill_fe_face_values (*this,
                                    cell, face_no, subface_no,
                                    QProjector<dim>::DataSetDescriptor::subface (face_no, subface_no,
                                        cell->face_orientation(face_no),
                                        cell->face_flip(face_no),
                                        cell->face_rotation(face_no),
                                        quadrature.size(),
                                        cell->subface_case(face_no)),
                                    quadrature,
                                    data,
                                    output_data);
}



namespace
{
  template <int dim, int spacedim, int rank>
  void
  transform_fields(const VectorSlice<const std::vector<Tensor<rank,dim> > > input,
                   const MappingType                                        mapping_type,
                   const typename Mapping<dim,spacedim>::InternalDataBase  &mapping_data,
                   VectorSlice<std::vector<Tensor<rank,spacedim> > >        output)
  {
    AssertDimension (input.size(), output.size());
    Assert ((dynamic_cast<const typename MappingQGeneric<dim,spacedim>::InternalData *>(&mapping_data) != 0),
            ExcInternalError());
    const typename MappingQGeneric<dim,spacedim>::InternalData
    &data = static_cast<const typename MappingQGeneric<dim,spacedim>::InternalData &>(mapping_data);

    switch (mapping_type)
      {
      case mapping_contravariant:
      {
        Assert (data.update_each & update_contravariant_transformation,
                typename FEValuesBase<dim>::ExcAccessToUninitializedField("update_contravariant_transformation"));

        for (unsigned int i=0; i<output.size(); ++i)
          output[i] = apply_transformation(data.contravariant[i], input[i]);

        return;
      }

      case mapping_piola:
      {
        Assert (data.update_each & update_contravariant_transformation,
                typename FEValuesBase<dim>::ExcAccessToUninitializedField("update_contravariant_transformation"));
        Assert (data.update_each & update_volume_elements,
                typename FEValuesBase<dim>::ExcAccessToUninitializedField("update_volume_elements"));
        Assert (rank==1, ExcMessage("Only for rank 1"));
        if (rank!=1)
          return;

        for (unsigned int i=0; i<output.size(); ++i)
          {
            output[i] = apply_transformation(data.contravariant[i], input[i]);
            output[i] /= data.volume_elements[i];
          }
        return;
      }
      //We still allow this operation as in the
      //reference cell Derivatives are Tensor
      //rather than DerivativeForm
      case mapping_covariant:
      {
        Assert (data.update_each & update_contravariant_transformation,
                typename FEValuesBase<dim>::ExcAccessToUninitializedField("update_covariant_transformation"));

        for (unsigned int i=0; i<output.size(); ++i)
          output[i] = apply_transformation(data.covariant[i], input[i]);

        return;
      }

      default:
        Assert(false, ExcNotImplemented());
      }
  }


  template <int dim, int spacedim, int rank>
  void
  transform_gradients(const VectorSlice<const std::vector<Tensor<rank,dim> > > input,
                      const MappingType                                        mapping_type,
                      const typename Mapping<dim,spacedim>::InternalDataBase  &mapping_data,
                      VectorSlice<std::vector<Tensor<rank,spacedim> > >        output)
  {
    AssertDimension (input.size(), output.size());
    Assert ((dynamic_cast<const typename MappingQGeneric<dim,spacedim>::InternalData *>(&mapping_data) != 0),
            ExcInternalError());
    const typename MappingQGeneric<dim,spacedim>::InternalData
    &data = static_cast<const typename MappingQGeneric<dim,spacedim>::InternalData &>(mapping_data);

    switch (mapping_type)
      {
      case mapping_contravariant_gradient:
      {
        Assert (data.update_each & update_covariant_transformation,
                typename FEValuesBase<dim>::ExcAccessToUninitializedField("update_covariant_transformation"));
        Assert (data.update_each & update_contravariant_transformation,
                typename FEValuesBase<dim>::ExcAccessToUninitializedField("update_contravariant_transformation"));
        Assert (rank==2, ExcMessage("Only for rank 2"));

        for (unsigned int i=0; i<output.size(); ++i)
          {
            DerivativeForm<1,spacedim,dim> A =
              apply_transformation(data.contravariant[i], transpose(input[i]) );
            output[i] = apply_transformation(data.covariant[i], A.transpose() );
          }

        return;
      }

      case mapping_covariant_gradient:
      {
        Assert (data.update_each & update_covariant_transformation,
                typename FEValuesBase<dim>::ExcAccessToUninitializedField("update_covariant_transformation"));
        Assert (rank==2, ExcMessage("Only for rank 2"));

        for (unsigned int i=0; i<output.size(); ++i)
          {
            DerivativeForm<1,spacedim,dim> A =
              apply_transformation(data.covariant[i], transpose(input[i]) );
            output[i] = apply_transformation(data.covariant[i], A.transpose() );
          }

        return;
      }

      case mapping_piola_gradient:
      {
        Assert (data.update_each & update_covariant_transformation,
                typename FEValuesBase<dim>::ExcAccessToUninitializedField("update_covariant_transformation"));
        Assert (data.update_each & update_contravariant_transformation,
                typename FEValuesBase<dim>::ExcAccessToUninitializedField("update_contravariant_transformation"));
        Assert (data.update_each & update_volume_elements,
                typename FEValuesBase<dim>::ExcAccessToUninitializedField("update_volume_elements"));
        Assert (rank==2, ExcMessage("Only for rank 2"));

        for (unsigned int i=0; i<output.size(); ++i)
          {
            DerivativeForm<1,spacedim,dim> A =
              apply_transformation(data.covariant[i], input[i] );
            Tensor<2,spacedim> T =
              apply_transformation(data.contravariant[i], A.transpose() );

            output[i] = transpose(T);
            output[i] /= data.volume_elements[i];
          }

        return;
      }

      default:
        Assert(false, ExcNotImplemented());
      }
  }




  template <int dim, int spacedim>
  void
  transform_hessians(const VectorSlice<const std::vector<Tensor<3,dim> > >   input,
                     const MappingType                                       mapping_type,
                     const typename Mapping<dim,spacedim>::InternalDataBase &mapping_data,
                     VectorSlice<std::vector<Tensor<3,spacedim> > >          output)
  {
    AssertDimension (input.size(), output.size());
    Assert ((dynamic_cast<const typename MappingQGeneric<dim,spacedim>::InternalData *>(&mapping_data) != 0),
            ExcInternalError());
    const typename MappingQGeneric<dim,spacedim>::InternalData
    &data = static_cast<const typename MappingQGeneric<dim,spacedim>::InternalData &>(mapping_data);

    switch (mapping_type)
      {
      case mapping_contravariant_hessian:
      {
        Assert (data.update_each & update_covariant_transformation,
                typename FEValuesBase<dim>::ExcAccessToUninitializedField("update_covariant_transformation"));
        Assert (data.update_each & update_contravariant_transformation,
                typename FEValuesBase<dim>::ExcAccessToUninitializedField("update_contravariant_transformation"));

        for (unsigned int q=0; q<output.size(); ++q)
          for (unsigned int i=0; i<spacedim; ++i)
            for (unsigned int j=0; j<spacedim; ++j)
              for (unsigned int k=0; k<spacedim; ++k)
                {
                  output[q][i][j][k] =    data.contravariant[q][i][0]
                                          * data.covariant[q][j][0]
                                          * data.covariant[q][k][0]
                                          * input[q][0][0][0];
                  for (unsigned int I=0; I<dim; ++I)
                    for (unsigned int J=0; J<dim; ++J)
                      {
                        const unsigned int K0 = (0==(I+J))? 1 : 0;
                        for (unsigned int K=K0; K<dim; ++K)
                          output[q][i][j][k] +=    data.contravariant[q][i][I]
                                                   * data.covariant[q][j][J]
                                                   * data.covariant[q][k][K]
                                                   * input[q][I][J][K];
                      }

                }
        return;
      }

      case mapping_covariant_hessian:
      {
        Assert (data.update_each & update_covariant_transformation,
                typename FEValuesBase<dim>::ExcAccessToUninitializedField("update_covariant_transformation"));

        for (unsigned int q=0; q<output.size(); ++q)
          for (unsigned int i=0; i<spacedim; ++i)
            for (unsigned int j=0; j<spacedim; ++j)
              for (unsigned int k=0; k<spacedim; ++k)
                {
                  output[q][i][j][k] =    data.covariant[q][i][0]
                                          * data.covariant[q][j][0]
                                          * data.covariant[q][k][0]
                                          * input[q][0][0][0];
                  for (unsigned int I=0; I<dim; ++I)
                    for (unsigned int J=0; J<dim; ++J)
                      {
                        const unsigned int K0 = (0==(I+J))? 1 : 0;
                        for (unsigned int K=K0; K<dim; ++K)
                          output[q][i][j][k] +=   data.covariant[q][i][I]
                                                  * data.covariant[q][j][J]
                                                  * data.covariant[q][k][K]
                                                  * input[q][I][J][K];
                      }

                }

        return;
      }

      case mapping_piola_hessian:
      {
        Assert (data.update_each & update_covariant_transformation,
                typename FEValuesBase<dim>::ExcAccessToUninitializedField("update_covariant_transformation"));
        Assert (data.update_each & update_contravariant_transformation,
                typename FEValuesBase<dim>::ExcAccessToUninitializedField("update_contravariant_transformation"));
        Assert (data.update_each & update_volume_elements,
                typename FEValuesBase<dim>::ExcAccessToUninitializedField("update_volume_elements"));

        for (unsigned int q=0; q<output.size(); ++q)
          for (unsigned int i=0; i<spacedim; ++i)
            for (unsigned int j=0; j<spacedim; ++j)
              for (unsigned int k=0; k<spacedim; ++k)
                {
                  output[q][i][j][k] =    data.contravariant[q][i][0]
                                          / data.volume_elements[q]
                                          * data.covariant[q][j][0]
                                          * data.covariant[q][k][0]
                                          * input[q][0][0][0];
                  for (unsigned int I=0; I<dim; ++I)
                    for (unsigned int J=0; J<dim; ++J)
                      {
                        const unsigned int K0 = (0==(I+J))? 1 : 0;
                        for (unsigned int K=K0; K<dim; ++K)
                          output[q][i][j][k] +=    data.contravariant[q][i][I]
                                                   / data.volume_elements[q]
                                                   * data.covariant[q][j][J]
                                                   * data.covariant[q][k][K]
                                                   * input[q][I][J][K];
                      }

                }

        return;
      }

      default:
        Assert(false, ExcNotImplemented());
      }
  }




  template<int dim, int spacedim, int rank>
  void
  transform_differential_forms(const VectorSlice<const std::vector<DerivativeForm<rank, dim,spacedim> > > input,
                               const MappingType                                                          mapping_type,
                               const typename Mapping<dim,spacedim>::InternalDataBase                    &mapping_data,
                               VectorSlice<std::vector<Tensor<rank+1, spacedim> > >                       output)
  {
    AssertDimension (input.size(), output.size());
    Assert ((dynamic_cast<const typename MappingQGeneric<dim,spacedim>::InternalData *>(&mapping_data) != 0),
            ExcInternalError());
    const typename MappingQGeneric<dim,spacedim>::InternalData
    &data = static_cast<const typename MappingQGeneric<dim,spacedim>::InternalData &>(mapping_data);

    switch (mapping_type)
      {
      case mapping_covariant:
      {
        Assert (data.update_each & update_contravariant_transformation,
                typename FEValuesBase<dim>::ExcAccessToUninitializedField("update_covariant_transformation"));

        for (unsigned int i=0; i<output.size(); ++i)
          output[i] = apply_transformation(data.covariant[i], input[i]);

        return;
      }
      default:
        Assert(false, ExcNotImplemented());
      }
  }
}



template<int dim, int spacedim>
void
MappingQGeneric<dim,spacedim>::
transform (const VectorSlice<const std::vector<Tensor<1, dim> > >   input,
           const MappingType                                        mapping_type,
           const typename Mapping<dim,spacedim>::InternalDataBase  &mapping_data,
           VectorSlice<std::vector<Tensor<1, spacedim> > >          output) const
{
  transform_fields(input, mapping_type, mapping_data, output);
}



template<int dim, int spacedim>
void
MappingQGeneric<dim,spacedim>::
transform (const VectorSlice<const std::vector<DerivativeForm<1, dim,spacedim> > > input,
           const MappingType                                                       mapping_type,
           const typename Mapping<dim,spacedim>::InternalDataBase                 &mapping_data,
           VectorSlice<std::vector<Tensor<2, spacedim> > >                         output) const
{
  transform_differential_forms(input, mapping_type, mapping_data, output);
}



template<int dim, int spacedim>
void
MappingQGeneric<dim,spacedim>::
transform (const VectorSlice<const std::vector<Tensor<2, dim> > >   input,
           const MappingType                                        mapping_type,
           const typename Mapping<dim,spacedim>::InternalDataBase  &mapping_data,
           VectorSlice<std::vector<Tensor<2, spacedim> > >          output) const
{
  switch (mapping_type)
    {
    case mapping_contravariant:
      transform_fields(input, mapping_type, mapping_data, output);
      return;

    case mapping_piola_gradient:
    case mapping_contravariant_gradient:
    case mapping_covariant_gradient:
      transform_gradients(input, mapping_type, mapping_data, output);
      return;
    default:
      Assert(false, ExcNotImplemented());
    }
}



template<int dim, int spacedim>
void
MappingQGeneric<dim,spacedim>::
transform (const VectorSlice<const std::vector< DerivativeForm<2, dim, spacedim> > > input,
           const MappingType                                                         mapping_type,
           const typename Mapping<dim,spacedim>::InternalDataBase                   &mapping_data,
           VectorSlice<std::vector<Tensor<3,spacedim> > >                            output) const
{

  AssertDimension (input.size(), output.size());
  Assert (dynamic_cast<const InternalData *>(&mapping_data) != 0,
          ExcInternalError());
  const InternalData &data = static_cast<const InternalData &>(mapping_data);

  switch (mapping_type)
    {
    case mapping_covariant_gradient:
    {
      Assert (data.update_each & update_contravariant_transformation,
              typename FEValuesBase<dim>::ExcAccessToUninitializedField("update_covariant_transformation"));

      for (unsigned int q=0; q<output.size(); ++q)
        for (unsigned int i=0; i<spacedim; ++i)
          for (unsigned int j=0; j<spacedim; ++j)
            for (unsigned int k=0; k<spacedim; ++k)
              {
                output[q][i][j][k] = data.covariant[q][j][0]
                                     * data.covariant[q][k][0]
                                     * input[q][i][0][0];
                for (unsigned int J=0; J<dim; ++J)
                  {
                    const unsigned int K0 = (0==J)? 1 : 0;
                    for (unsigned int K=K0; K<dim; ++K)
                      output[q][i][j][k] += data.covariant[q][j][J]
                                            * data.covariant[q][k][K]
                                            * input[q][i][J][K];
                  }

              }
      return;
    }

    default:
      Assert(false, ExcNotImplemented());
    }
}



template<int dim, int spacedim>
void
MappingQGeneric<dim,spacedim>::
transform (const VectorSlice<const std::vector< Tensor<3,dim> > >   input,
           const MappingType                                        mapping_type,
           const typename Mapping<dim,spacedim>::InternalDataBase  &mapping_data,
           VectorSlice<std::vector<Tensor<3,spacedim> > >           output) const
{

  switch (mapping_type)
    {
    case mapping_piola_hessian:
    case mapping_contravariant_hessian:
    case mapping_covariant_hessian:
      transform_hessians(input, mapping_type, mapping_data, output);
      return;
    default:
      Assert(false, ExcNotImplemented());
    }

}



//--------------------------- Explicit instantiations -----------------------
#include "mapping_q_generic.inst"


DEAL_II_NAMESPACE_CLOSE