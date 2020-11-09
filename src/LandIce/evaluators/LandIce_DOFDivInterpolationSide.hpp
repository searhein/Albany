//*****************************************************************//
//    Albany 2.0:  Copyright 2012 Sandia Corporation               //
//    This Software is released under the BSD license detailed     //
//    in the file "license.txt" in the top-level Albany directory  //
//*****************************************************************//

#ifndef LANDICE_DOF_DIV_INTERPOLATION_SIDE_HPP
#define LANDICE_DOF_DIV_INTERPOLATION_SIDE_HPP 1

#include "Phalanx_config.hpp"
#include "Phalanx_Evaluator_WithBaseImpl.hpp"
#include "Phalanx_Evaluator_Derived.hpp"
#include "Phalanx_MDField.hpp"

#include "Albany_Layouts.hpp"
#include "Albany_SacadoTypes.hpp"
#include "PHAL_Utilities.hpp"

namespace LandIce {

template<typename EvalT, typename Traits, typename ScalarT>
class DOFDivInterpolationSideBase : public PHX::EvaluatorWithBaseImpl<Traits>,
                                    public PHX::EvaluatorDerived<EvalT, Traits>
{
public:

  DOFDivInterpolationSideBase (const Teuchos::ParameterList& p,
                               const Teuchos::RCP<Albany::Layouts>& dl_side);

  void postRegistrationSetup (typename Traits::SetupData d,
                              PHX::FieldManager<Traits>& vm);

  void evaluateFields (typename Traits::EvalData d);

private:

  typedef typename EvalT::MeshScalarT MeshScalarT;
  typedef typename Albany::StrongestScalarType<ScalarT,MeshScalarT>::type OutputScalarT;

  std::string sideSetName;

  // Input:
  //! Values at nodes
  PHX::MDField<const ScalarT> val_node;
  //! Basis Functions and side tangents
  PHX::MDField<const MeshScalarT> gradBF;
  PHX::MDField<const MeshScalarT>  tangents;

  // Output:
  //! Values at quadrature points
  PHX::MDField<OutputScalarT> val_qp;

  unsigned int numSideNodes;
  unsigned int numSideQPs;
  unsigned int numDims;

  bool useCollapsedSidesets;

  PHAL::MDFieldMemoizer<Traits> memoizer;

  Albany::LocalSideSetInfo sideSet;
};

// Some shortcut names
template<typename EvalT, typename Traits>
using DOFDivInterpolationSide = DOFDivInterpolationSideBase<EvalT,Traits,typename EvalT::ScalarT>;

template<typename EvalT, typename Traits>
using DOFDivInterpolationSideMesh = DOFDivInterpolationSideBase<EvalT,Traits,typename EvalT::MeshScalarT>;

template<typename EvalT, typename Traits>
using DOFDivInterpolationSideParam = DOFDivInterpolationSideBase<EvalT,Traits,typename EvalT::ParamScalarT>;

} // Namespace LandIce

#endif // LANDICE_DOF_DIV_INTERPOLATION_SIDE_HPP
