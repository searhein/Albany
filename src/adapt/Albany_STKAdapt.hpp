//*****************************************************************//
//    Albany 2.0:  Copyright 2012 Sandia Corporation               //
//    This Software is released under the BSD license detailed     //
//    in the file "license.txt" in the top-level Albany directory  //
//*****************************************************************//


#ifndef ALBANY_STKADAPT_HPP
#define ALBANY_STKADAPT_HPP

#include "Teuchos_RCP.hpp"
#include "Teuchos_ParameterList.hpp"

#include "Albany_AbstractAdapter.hpp"
#include "Albany_GenericSTKMeshStruct.hpp"
#include "Albany_STKDiscretization.hpp"

#include "Phalanx.hpp"
#include "PHAL_Workset.hpp"
#include "PHAL_Dimension.hpp"

#include "Albany_STKUnifSizeField.hpp"
#include "UniformRefinerPattern.hpp"

namespace Albany {

template<class SizeField>

class STKAdapt : public AbstractAdapter {
public:

   STKAdapt(const Teuchos::RCP<Teuchos::ParameterList>& params_,
                     const Teuchos::RCP<ParamLib>& paramLib_,
                     Albany::StateManager& StateMgr_,
                     const Teuchos::RCP<const Epetra_Comm>& comm_);
   //! Destructor
    ~STKAdapt();

    //! Check adaptation criteria to determine if the mesh needs adapting
    virtual bool queryAdaptationCriteria();

    //! Apply adaptation method to mesh and problem. Returns true if adaptation is performed successfully.
    // This function prints an error if called as meshAdapt needs the solution field to build the size function.
    virtual bool adaptMesh();

    //! Apply adaptation method to mesh and problem. Returns true if adaptation is performed successfully.
    // Solution is needed to calculate the size function
//    virtual bool adaptMesh(const Epetra_Vector& solution, const Teuchos::RCP<Epetra_Import>& importer);
//    virtual bool adaptMesh(const Epetra_Vector& solution, const Epetra_Vector& ovlp_solution);

    //! Transfer solution between meshes.
    virtual void solutionTransfer(const Epetra_Vector& oldSolution,
        Epetra_Vector& newSolution);

   //! Each adapter must generate it's list of valid parameters
    Teuchos::RCP<const Teuchos::ParameterList> getValidAdapterParameters() const;

private:

   // Disallow copy and assignment
   STKAdapt(const STKAdapt &);
   STKAdapt &operator=(const STKAdapt &);

   int numDim;
   int remeshFileIndex;

   Teuchos::RCP<Albany::GenericSTKMeshStruct> genericMeshStruct;

   Teuchos::RCP<Albany::AbstractDiscretization> disc;

   Albany::STKDiscretization *stk_discretization;

   Teuchos::RCP<stk::percept::PerceptMesh> eMesh;
   Teuchos::RCP<stk::adapt::UniformRefinerPatternBase> refinerPattern;

   int num_iterations;

   const Epetra_Vector* solution;
   const Epetra_Vector* ovlp_solution;

};

}

// Define macros for explicit template instantiation
#define STKADAPT_INSTANTIATE_TEMPLATE_CLASS_UNIFREFINE(name) \
  template class name<Albany::STKUnifRefineField>;

#define STKADAPT_INSTANTIATE_TEMPLATE_CLASS(name) \
  STKADAPT_INSTANTIATE_TEMPLATE_CLASS_UNIFREFINE(name) 


#endif //ALBANY_STKADAPT_HPP
