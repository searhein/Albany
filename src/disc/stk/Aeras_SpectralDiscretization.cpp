//*****************************************************************//
//    Albany 3.0:  Copyright 2016 Sandia Corporation               //
//    This Software is released under the BSD license detailed     //
//    in the file "license.txt" in the top-level Albany directory  //
//*****************************************************************//
//
// This class implements spectralelements for Aeras, by reading in a STK mesh from an Exodus file
// containing a bilinear quad/hex mesh and enriching it with
// additional nodes to create a higher order mesh.
//

#include "Aeras_SpectralDiscretization.hpp"

// Standard includes
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

// Trilinos includes
#include <Teuchos_TwoDArray.hpp>
#include <Shards_BasicTopologies.hpp>
#include <Intrepid2_CellTools.hpp>
#include <Intrepid2_Basis.hpp>
#include <Intrepid2_HGRAD_QUAD_Cn_FEM.hpp>
#include <Intrepid2_CubaturePolylib.hpp>
#include <stk_util/parallel/Parallel.hpp>
#include <stk_mesh/base/FEMHelpers.hpp>
#include <stk_mesh/base/Entity.hpp>
#include <stk_mesh/base/CreateEdges.hpp>
#include <stk_mesh/base/GetEntities.hpp>
#include <stk_mesh/base/GetBuckets.hpp>
#include <stk_mesh/base/Selector.hpp>
#include <PHAL_Dimension.hpp>
#ifdef ALBANY_SEACAS
#include <Ionit_Initializer.h>
#include <netcdf.h>
#ifdef ALBANY_PAR_NETCDF
extern "C"
{
#include <netcdf_par.h>
}
#endif
#endif

// Albany includes
#include "Albany_Utils.hpp"
#include "Albany_NodalGraphUtils.hpp"
#include "Albany_STKNodeFieldContainer.hpp"
#include "Albany_BucketArray.hpp"
#include "Albany_ThyraUtils.hpp" 

// Constants
const double pi = 3.1415926535897932385;


const GO INVALID = Teuchos::OrdinalTraits<GO>::invalid();

// Uncomment the following line if you want debug output to be printed
// to screen

//#define OUTPUT_TO_SCREEN
//#define PRINT_COORDS
//#define WRITE_TO_MATRIX_MARKET_TO_MM_FILE

Aeras::SpectralDiscretization::
SpectralDiscretization(const Teuchos::RCP<Teuchos::ParameterList>& discParams_,
                  Teuchos::RCP<Albany::AbstractSTKMeshStruct> stkMeshStruct_,
                  const int numLevels_, const int numTracers_,
                  const Teuchos::RCP<const Teuchos_Comm>& commT_,
                  const bool explicit_scheme_,
                  const Teuchos::RCP<Albany::RigidBodyModes>& rigidBodyModes_) :
  out(Teuchos::VerboseObjectBase::getDefaultOStream()),
  previous_time_label(-1.0e32),
  metaData(*stkMeshStruct_->metaData),
  bulkData(*stkMeshStruct_->bulkData),
  numLevels(numLevels_),
  numTracers(numTracers_),
  commT(commT_),
  explicit_scheme(explicit_scheme_),
  rigidBodyModes(rigidBodyModes_),
  discParams(discParams_),
  neq(stkMeshStruct_->neq),
  stkMeshStruct(stkMeshStruct_),
  interleavedOrdering(stkMeshStruct_->interleavedOrdering)
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
  *out << "Explicit scheme in Aeras? " << explicit_scheme << std::endl;
#endif
   //IKT, 9/30/15: error check that the user is not trying to prescribe periodic BCs for a problem other than a 1D one.
   //Periodic BCs are only supported for 1D (xz-hydrostatic) problems.
  int numPeriodicBCs = 0;
  for (int dim=0; dim<stkMeshStruct->numDim; dim++)
     if (stkMeshStruct->PBCStruct.periodic[dim])
       numPeriodicBCs++;
  if ((stkMeshStruct->numDim>1) && (numPeriodicBCs>0))
    TEUCHOS_TEST_FOR_EXCEPTION(
       true, std::logic_error, "Aeras::SpectralDiscretization constructor: periodic BCs are only supported for 1D spectral elements!  "
            << "You are attempting to specify periodic BCs for a " << stkMeshStruct->numDim << "D problem." << std::endl);

  // Get from parameter list how many points per edge we have (default
  // = 2: no enrichment)
  points_per_edge = stkMeshStruct->points_per_edge;
  CellTopologyData ctd = stkMeshStruct->getMeshSpecs()[0]->ctd;
  element_name = ctd.name;
  size_t len      = element_name.find("_");
  if (len != std::string::npos) element_name = element_name.substr(0,len);
  if (element_name == "Line") {
    spatial_dim = 1;
    nodes_per_element = points_per_edge;
    ElemType = LINE;
  }
  else if (element_name == "Quadrilateral" ||
           element_name == "ShellQuadrilateral") {
    spatial_dim = 2;
    nodes_per_element = points_per_edge*points_per_edge;
    ElemType = QUAD;
  }
  //IKT: the following is necessary to prevent seg fault when running Aeras
  //due to changes to Albany added by Dave Littlewood on 6/28/16.  We need
  //to recize latticeOrientation array b/c it is indexed in Albany_Application.hpp.
  stk::mesh::Selector select_owned =
    stk::mesh::Selector(metaData.locally_owned_part());
  const stk::mesh::BucketVector & buckets =
    bulkData.get_buckets(stk::topology::ELEMENT_RANK, select_owned);
  const int numBuckets = buckets.size();
  latticeOrientation.resize(numBuckets);
#if defined(ALBANY_LCM)
  boundary_indicator.resize(numBuckets);
#endif
#ifdef OUTPUT_TO_SCREEN
  *out << "points_per_edge: " << points_per_edge << std::endl;
  *out << "element name: " << element_name << std::endl;
  *out << "spatial_dim: " << spatial_dim << std::endl;
  *out << "nodes_per_element: " << nodes_per_element << std::endl;
  *out << "neq: " << neq << std::endl;
  *out << "numLevels: " << numLevels << std::endl;
  *out << "numTracers: " << numTracers << std::endl;
#endif
  //IKT, FIXME: I think this routine needs to move with 
  //the new design of the code. 
  //Aeras::SpectralDiscretization::updateMesh();
}

Aeras::SpectralDiscretization::~SpectralDiscretization()
{
#ifdef ALBANY_SEACAS
  if (stkMeshStruct->cdfOutput)
    if (netCDFp) {
      const int ierr = nc_close (netCDFp);
      ALBANY_ASSERT(!ierr,
            "close returned error code " << ierr << " - "
            << nc_strerror(ierr) << std::endl);
    }
#endif

  for (int i=0; i< toDelete.size(); i++) delete [] toDelete[i];
}

Teuchos::RCP<const Thyra_VectorSpace> 
Aeras::SpectralDiscretization::getVectorSpace() const
{
  return m_vs; 
}

Teuchos::RCP<const Thyra_VectorSpace> 
Aeras::SpectralDiscretization::getOverlapVectorSpace() const  
{
  return m_overlap_vs; 
}

Teuchos::RCP<const Thyra_VectorSpace> 
Aeras::SpectralDiscretization::getNodeVectorSpace() const 
{
  return m_node_vs; 
}

Teuchos::RCP<const Thyra_VectorSpace> 
Aeras::SpectralDiscretization::getOverlapNodeVectorSpace() const 
{
  return m_overlap_node_vs; 
}  

Teuchos::RCP<const Thyra_VectorSpace> 
Aeras::SpectralDiscretization::getNodeVectorSpace (const std::string& field_name) const
{
  TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error, "not implemented");
  return Teuchos::null;
}

Teuchos::RCP<const Thyra_VectorSpace> 
Aeras::SpectralDiscretization::getOverlapNodeVectorSpace (const std::string& field_name) const
{
  TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error, "not implemented");
  return Teuchos::null;
}

Teuchos::RCP<const Thyra_VectorSpace> 
Aeras::SpectralDiscretization::getVectorSpace(const std::string& field_name) const 
{
  TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error, "not implemented");
  return Teuchos::null;
}

Teuchos::RCP<const Thyra_VectorSpace> 
Aeras::SpectralDiscretization::getOverlapVectorSpace (const std::string& field_name) const
{
  TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error, "not implemented");
  return Teuchos::null;
}


const Albany::Conn&
Aeras::SpectralDiscretization::getWsElNodeEqID() const
{
  return wsElNodeEqID;
}

const Albany::WorksetArray<Teuchos::ArrayRCP<Teuchos::ArrayRCP<GO> > >::type&
Aeras::SpectralDiscretization::getWsElNodeID() const
{
  return wsElNodeID;
}

const Albany::WorksetArray<Teuchos::ArrayRCP<Teuchos::ArrayRCP<double*> > >::type&
Aeras::SpectralDiscretization::getCoords() const
{
  return coords;
}

#ifdef ALBANY_CONTACT
Teuchos::RCP<const Albany::ContactManager> Aeras::SpectralDiscretization::getContactManager() const
{
  return contactManager;
}
#endif

const Albany::WorksetArray<Teuchos::ArrayRCP<double> >::type&
Aeras::SpectralDiscretization::getSphereVolume() const
{
  return sphereVolume;
}

const Albany::WorksetArray<Teuchos::ArrayRCP<double*> >::type&
Aeras::SpectralDiscretization::getLatticeOrientation() const
{
  return latticeOrientation;
}

#if defined(ALBANY_LCM)
Albany::WorksetArray<Teuchos::ArrayRCP<double*>>::type const&
Aeras::SpectralDiscretization::getBoundaryIndicator() const
{
  return boundary_indicator;
}
#endif

void
Aeras::SpectralDiscretization::printCoords() const
{
  // Print coordinates
  std::cout << "Processor " << bulkData.parallel_rank() << " has "
            << coords.size() << " worksets." << std::endl;
  for (int ws = 0; ws < coords.size(); ws++)             // workset
  {
    for (int e = 0; e < coords[ws].size(); e++)          // cell
    {
      for (int j = 0; j < coords[ws][e].size(); j++)     // node
      {
        // IK, 1/27/15: the following assumes a 3D mesh.
        // FIXME, 4/21/15: add logic for the case when we have line elements.
        std::cout << "Coord for workset: " << ws << " element: " << e
                  << " node: " << j << " x, y, z: "
                  << coords[ws][e][j][0] << ", " << coords[ws][e][j][1]
                  << ", " << coords[ws][e][j][2] << std::endl;
      }
    }
  }
}

void
Aeras::SpectralDiscretization::printCoordsAndGIDs() const
{
  //print coordinates
  std::cout << "Processor " << bulkData.parallel_rank() << " has "
            << coords.size() << " worksets." << std::endl;
  for (int ws = 0; ws < coords.size(); ws++)             // workset
  {
    for (int e = 0; e < coords[ws].size(); e++)          // cell
    {
      for (int j = 0; j < coords[ws][e].size(); j++)     // node
      {
      // IK, 1/27/15: the following assumes a 3D mesh.
      // FIXME, 4/21/15: add logic for the case when we have line elements.
        std::cout << "GID, x, y, z: " << wsElNodeID[ws][e][j]<< " "
                  << coords[ws][e][j][0] << " " << coords[ws][e][j][1]
                  << " " << coords[ws][e][j][2] << std::endl;
      }
    }
  }
}

void
Aeras::SpectralDiscretization::printConnectivity(bool printEdges) const
{
  commT->barrier();
  if (printEdges)
    for (int rank = 0; rank < commT->getSize(); ++rank)
    {
      commT->barrier();
      if (rank == commT->getRank())
      {
        std::cout << std::endl << "Process rank " << rank << std::endl;
        for (std::map< GO, Teuchos::ArrayRCP< GO > >::const_iterator edge =
               enrichedEdges.begin(); edge != enrichedEdges.end(); ++edge)
        {
          Teuchos::ArrayRCP< GO > nodes = edge->second;
          int numNodes = nodes.size();
          std::cout << "    Edge " << edge->first << ": Nodes = ";
          for (size_t inode = 0; inode < numNodes; ++inode)
            std::cout << nodes[inode] << " ";
          std::cout << std::endl;
        }
      }
    }

  else
    for (int rank = 0; rank < commT->getSize(); ++rank)
    {
      commT->barrier();
      if (rank == commT->getRank())
      {
        std::cout << std::endl << "Process rank " << rank << std::endl;
        for (size_t ibuck = 0; ibuck < wsElNodeID.size(); ++ibuck)
        {
          std::cout << "  Bucket " << ibuck << std::endl;
          for (size_t ielem = 0; ielem < wsElNodeID[ibuck].size(); ++ielem)
          {
            int numNodes = wsElNodeID[ibuck][ielem].size();
            std::cout << "    Element " << ielem << ": Nodes = ";
            for (size_t inode = 0; inode < numNodes; ++inode)
              std::cout << wsElNodeID[ibuck][ielem][inode] << " ";
            std::cout << std::endl;
          }
        }
      }
    }
  commT->barrier();
}


// IK, 1/8/15, FIXME: getCoordinates() needs to be rewritten to
// include the enriched nodes.
const Teuchos::ArrayRCP<double>&
Aeras::SpectralDiscretization::getCoordinates() const
{
  // Coordinates are computed here, and not precomputed,
  // since the mesh can move in shape opt problems

  Albany::AbstractSTKFieldContainer::VectorFieldType* coordinates_field =
    stkMeshStruct->getCoordinatesField();

  for (int i=0; i < numOverlapNodes; i++)
  {
    GO node_gid = gid(overlapnodes[i]);
    auto node_lid = Albany::getLocalElement(m_overlap_node_vs,node_gid);

    double* x = stk::mesh::field_data(*coordinates_field, overlapnodes[i]);
    for (int dim=0; dim<stkMeshStruct->numDim; dim++)
      coordinates[3*node_lid + dim] = x[dim];
  }
  
  return coordinates;
}

// These methods were added to support mesh adaptation, which is currently
// limited to PUMIDiscretization.
void Aeras::SpectralDiscretization::
setCoordinates(const Teuchos::ArrayRCP<const double>& c)
{
  TEUCHOS_TEST_FOR_EXCEPTION(
    true, std::logic_error,
    "Aeras::SpectralDiscretization::setCoordinates is not implemented.");
}

void Aeras::SpectralDiscretization::
setReferenceConfigurationManager(const Teuchos::RCP<AAdapt::rc::Manager>& rcm)
{
  TEUCHOS_TEST_FOR_EXCEPTION(
    true, std::logic_error, "Aeras::SpectralDiscretization::" <<
    "setReferenceConfigurationManager is not implemented.");
}

// The function transformMesh() maps a unit cube domain by applying a
// transformation

// IK, 1/8/15, FIXME: I've removed all the LandIce stuff from
// transformMesh() as this is for now an Aeras-only class.  The
// setting of the schar mountain transformation needs to be fixed to
// use the new (enriched) nodes rather than the nodes pulled from STK.
// This is not critical -- Schar Mountain transformation only called
// for XZ Hydrostatic equations.
void
Aeras::SpectralDiscretization::transformMesh()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  using std::cout; using std::endl;
  Albany::AbstractSTKFieldContainer::VectorFieldType* coordinates_field = stkMeshStruct->getCoordinatesField();
  std::string transformType = stkMeshStruct->transformType;

  if (transformType == "None") {}
  else if (transformType == "Spherical")
  {
    // This works in Aeras_SpectralDiscretization (only transform)
    // [IKT, 3/25/15]. This form takes a mesh of a square / cube and
    // transforms it into a mesh of a circle/sphere
#ifdef OUTPUT_TO_SCREEN
    *out << "Spherical" << endl;
#endif
    const int numDim  = stkMeshStruct->numDim;
    for (int ws = 0; ws < coords.size(); ws++) { //workset
      for (int e = 0; e < coords[ws].size(); e++) {  // cell
        for (int j = 0; j < coords[ws][e].size(); j++)  {// node
          double r = 0.0;
          for (int n=0; n<numDim; n++) //dimensions
             r += coords[ws][e][j][n]*coords[ws][e][j][n];
          r = sqrt(r);
          for (int n=0; n<numDim; n++) { //dimensions
            //FIXME: there could be division by 0 here!
            coords[ws][e][j][n] = coords[ws][e][j][n]/r;
          }
        }
      }
    }
  }
  else if (transformType == "Aeras Schar Mountain")
  {
    TEUCHOS_TEST_FOR_EXCEPTION(
      true, std::logic_error, "Error: transformMesh() is not implemented yet "
      << "in Aeras::SpectralDiscretiation!" << std::endl);
#ifdef OUTPUT_TO_SCREEN
    *out << "Aeras Schar Mountain transformation!" << endl;
#endif
    double rhoOcean = 1028.0; // ocean density, in kg/m^3
    for (int i=0; i < numOverlapNodes; i++)
    {
      double* x = stk::mesh::field_data(*coordinates_field, overlapnodes[i]);
      x[0] = x[0];
      double hstar = 0.0, h;
      if (std::abs(x[0]-150.0) <= 25.0)
        hstar = 3.0* std::pow(cos(M_PI*(x[0]-150.0) / 50.0),2);
      h = hstar * std::pow(cos(M_PI*(x[0]-150.0) / 8.0),2);
      x[1] = x[1] + h*(25.0 - x[1])/25.0;
    }
  }
  else
  {
    TEUCHOS_TEST_FOR_EXCEPTION(
      true, std::logic_error, "Aeras::SpectralDiscretization::transformMesh() "
      << "Unknown transform type :" << transformType << std::endl);
  }
}

// IK, 1/23/15: ultimately we want to implement setupMLCoords() for
// the enriched mesh.  This could only be needed with ML/MueLu
// preconditioners.
void Aeras::SpectralDiscretization::setupMLCoords()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "Warning: setupMLCoords() not implemented in Aeras::"
       << "SpectralDiscretization!  " << "ML and MueLu will not receive "
       << "coordinates for repartitioning if used." << std::endl;
#endif
}

void Aeras::SpectralDiscretization::writeCoordsToMatrixMarket() const
{
#ifdef OUTPUT_TO_SCREEN
  *out << "Warning: writeCoordsToMatrixMarketCoords() not implemented in Aeras::"
       << "SpectralDiscretization!  " <<  std::endl;
#endif
}

const Albany::WorksetArray<std::string>::type&
Aeras::SpectralDiscretization::getWsEBNames() const
{
  return wsEBNames;
}

const Albany::WorksetArray<int>::type&
Aeras::SpectralDiscretization::getWsPhysIndex() const
{
  return wsPhysIndex;
}


void
Aeras::SpectralDiscretization::writeSolution(const Thyra_Vector& solution,
                                             const double time,
                                             const bool overlapped)
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  writeSolutionToMeshDatabase(solution, time, overlapped);
  writeSolutionToFile(solution, time, overlapped);
}

void
Aeras::SpectralDiscretization::writeSolution(const Thyra_Vector& solution,
                                             const Thyra_Vector& solution_dot,
                                             const double time,
                                             const bool overlapped)
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  writeSolutionToMeshDatabase(solution, solution_dot, time, overlapped);
  //IKT, FIXME? extend writeSolutionToFile to take in solution_dot?
  writeSolutionToFile(solution, time, overlapped);
}

void
Aeras::SpectralDiscretization::writeSolution(const Thyra_Vector& solution,
                                             const Thyra_Vector& solution_dot,
                                             const Thyra_Vector& solution_dotdot,
                                             const double time,
                                             const bool overlapped)
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  writeSolutionToMeshDatabase(solution, solution_dot, solution_dotdot, time, overlapped);
  //IKT, FIXME? extend writeSolutionToFile to take in solution_dot and solution_dotdot?
  writeSolutionToFile(solution, time, overlapped);
}

void
Aeras::SpectralDiscretization::writeSolutionMV(const Thyra_MultiVector& solution,
                                              const double time,
                                              const bool overlapped)
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  writeSolutionMVToMeshDatabase(solution, time, overlapped);
  writeSolutionMVToFile(solution, time, overlapped);
}

void
Aeras::SpectralDiscretization::writeSolutionToMeshDatabase(
    const Thyra_Vector& solution,
    const double time,
    const bool overlapped)
{

#ifdef WRITE_TO_MATRIX_MARKET_TO_MM_FILE
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
  Albany::writeMatrixMarket(Teuchos::rcpFromRef(solution), "solution.mm");
#endif
  // Put solution as Thyra_Vector into STK Mesh
  setSolutionField(solution, overlapped);
}

void
Aeras::SpectralDiscretization::writeSolutionToMeshDatabase(
    const Thyra_Vector& solution,
    const Thyra_Vector& solution_dot,
    const double time,
    const bool overlapped)
{
#ifdef WRITE_TO_MATRIX_MARKET_TO_MM_FILE
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  // Put solution as Thyra_Vector into STK Mesh
  setSolutionField(solution, solution_dot, overlapped);
}

void
Aeras::SpectralDiscretization::writeSolutionToMeshDatabase(
    const Thyra_Vector& solution,
    const Thyra_Vector& solution_dot,
    const Thyra_Vector& solution_dotdot,
    const double time,
    const bool overlapped)
{
#ifdef WRITE_TO_MATRIX_MARKET_TO_MM_FILE
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  // Put solution as Thyra_Vector into STK Mesh
  setSolutionField(solution, solution_dot, solution_dotdot, overlapped);
}

void
Aeras::SpectralDiscretization::writeSolutionMVToMeshDatabase(
    const Thyra_MultiVector& solution,
    const double time,
    const bool overlapped)
{
#ifdef WRITE_TO_MATRIX_MARKET_TO_MM_FILE
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
   Albany::writeMatrixMarket(Teuchos::rcpFromRef(solution, "solution.mm");
#endif
  // Put solution as Epetra_Vector into STK Mesh
  setSolutionFieldMV(solution, overlapped);
}


void
Aeras::SpectralDiscretization::writeSolutionToFile(const Thyra_Vector& solution,
                                                   const double time,
                                                   const bool overlapped)
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
#ifdef ALBANY_SEACAS
  if (stkMeshStruct->exoOutput && stkMeshStruct->transferSolutionToCoords) {
   Teuchos::RCP<Albany::AbstractSTKFieldContainer> container =
     outputStkMeshStruct->getFieldContainer();
   container->transferSolutionToCoords();

   if (!mesh_data.is_null()) {
     // Mesh coordinates have changed. Rewrite output file by deleting
     // the mesh data object and recreate it
     setupExodusOutput();
   }
  }
  // Skip this write unless the proper interval has been reached
  if (stkMeshStruct->exoOutput &&
      !(outputInterval % stkMeshStruct->exoOutputInterval))
  {
    double time_label = monotonicTimeLabel(time);
    int out_step = mesh_data->process_output_request(outputFileIdx, time_label);
    if (Albany::getComm(m_vs)->getRank() == 0)
    {
      *out << "Aeras::SpectralDiscretization::writeSolution: writing time "
           << time;
      if (time_label != time)
        *out << " with label " << time_label;
      *out << " to index " <<out_step<<" in file "<<stkMeshStruct->exoOutFile
           << std::endl;
    }
  }
  // IKT, 4/22/15: we are not going to worry about netcdf file writing yet.
  if (stkMeshStruct->cdfOutput && !(outputInterval % stkMeshStruct->cdfOutputInterval))
  {
    TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error, "Aeras::SpectralDiscretization::writeSolutionToFile"
                               << " is not implemented for writing out NetCDF files!");
  }
  outputInterval++;
#endif
}


void
Aeras::SpectralDiscretization::writeSolutionMVToFile(const Thyra_MultiVector& solution,
                                                    const double time,
                                                    const bool overlapped)
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
#ifdef ALBANY_SEACAS
  if (stkMeshStruct->exoOutput && stkMeshStruct->transferSolutionToCoords) {
   Teuchos::RCP<Albany::AbstractSTKFieldContainer> container =
     outputStkMeshStruct->getFieldContainer();
   container->transferSolutionToCoords();

   if (!mesh_data.is_null()) {
     // Mesh coordinates have changed. Rewrite output file by deleting
     // the mesh data object and recreate it
     setupExodusOutput();
   }
  }
  // Skip this write unless the proper interval has been reached
  if (stkMeshStruct->exoOutput &&
      !(outputInterval % stkMeshStruct->exoOutputInterval))
  {
    double time_label = monotonicTimeLabel(time);
    int out_step = mesh_data->process_output_request(outputFileIdx, time_label);
    if (Albany::getComm(m_vs)->getRank() == 0)
    {
      *out << "Aeras::SpectralDiscretization::writeSolution: writing time "
           << time;
      if (time_label != time)
        *out << " with label " << time_label;
      *out << " to index " <<out_step<<" in file "<<stkMeshStruct->exoOutFile
           << std::endl;
    }
  }
  // IKT, 4/22/15: we are not going to worry about netcdf file writing yet.
  if (stkMeshStruct->cdfOutput && !(outputInterval % stkMeshStruct->cdfOutputInterval))
  {
    TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error, "Aeras::SpectralDiscretization::writeSolutionMVToFile"
                               << " is not implemented for writing out NetCDF files!");
  }
  outputInterval++;
#endif

}

double
Aeras::SpectralDiscretization::monotonicTimeLabel(const double time)
{
  // If increasing, then all is good
  if (time > previous_time_label)
  {
    previous_time_label = time;
    return time;
  }

  // Try absolute value
  double time_label = fabs(time);
  if (time_label > previous_time_label)
  {
    previous_time_label = time_label;
    return time_label;
  }

  // Try adding 1.0 to time
  if (time_label+1.0 > previous_time_label)
  {
    previous_time_label = time_label+1.0;
    return time_label+1.0;
  }

  // Otherwise, just add 1.0 to previous
  previous_time_label += 1.0;
  return previous_time_label;
}


void
Aeras::SpectralDiscretization::setResidualField(const Thyra_Vector& residual)
{
  // Nothing to do for Aeras -- LCM-only function
}

Teuchos::RCP<Thyra_Vector>
Aeras::SpectralDiscretization::getSolutionField(bool overlapped) const
{
  // Copy soln vector into solution field, one node at a time
  Teuchos::RCP<Thyra_Vector> solution = Thyra::createMember(m_vs); 
  this->getSolutionField(*solution, overlapped);
  return solution;
}

Teuchos::RCP<Thyra_MultiVector>
Aeras::SpectralDiscretization::getSolutionMV(bool overlapped) const
{
  // Copy soln multi-vector into solution field, one node at a time
  int num_time_deriv = stkMeshStruct->num_time_deriv;
  Teuchos::RCP<Thyra_MultiVector> solnMV = Thyra::createMembers(m_vs, num_time_deriv + 1); 
  this->getSolutionMV(*solnMV, overlapped);
  return solnMV;
}

void
Aeras::SpectralDiscretization::getSolutionField(Thyra_Vector &result,
                                                const bool overlapped) const
{
  TEUCHOS_TEST_FOR_EXCEPTION(overlapped, std::logic_error, "Not implemented.");

  Teuchos::RCP<Albany::AbstractSTKFieldContainer> container =
    stkMeshStruct->getFieldContainer();

  // Iterate over the on-processor nodes by getting node buckets and
  // iterating over each bucket.
  stk::mesh::Selector locally_owned = metaData.locally_owned_part();

  container->fillSolnVector(result, locally_owned, m_node_vs);
}

void
Aeras::SpectralDiscretization::getField(Thyra_Vector &result,
                                        const std::string& name) const
{
  TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error,
      "Aeras::SpectralDiscretization::getField() not implemented!");
}

void
Aeras::SpectralDiscretization::getSolutionMV(Thyra_MultiVector &result,
                                                 const bool overlapped) const
{
  TEUCHOS_TEST_FOR_EXCEPTION(overlapped, std::logic_error, "Not implemented.");

  Teuchos::RCP<Albany::AbstractSTKFieldContainer> container =
    stkMeshStruct->getFieldContainer();

  // Iterate over the on-processor nodes by getting node buckets and
  // iterating over each bucket.
  stk::mesh::Selector locally_owned = metaData.locally_owned_part();

  container->fillSolnMultiVector(result, locally_owned, m_node_vs);

}


/*****************************************************************/
/*** Private functions follow. These are just used in above code */
/*****************************************************************/

void
Aeras::SpectralDiscretization::setSolutionField(const Thyra_Vector& solution, 
                                                const bool overlapped)
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif

  // Copy soln vector into solution field, one node at a time
  // Note that soln coming in is the local (non overlapped) soln

  Teuchos::RCP<Albany::AbstractSTKFieldContainer> container =
    outputStkMeshStruct->getFieldContainer();

  // Iterate over the on-processor nodes
  stk::mesh::Selector locally_owned =
    outputStkMeshStruct->metaData->locally_owned_part();

  container->saveSolnVector(solution, locally_owned, m_node_vs);
}

void
Aeras::SpectralDiscretization::setSolutionField(const Thyra_Vector& solution, const Thyra_Vector& solution_dot,
                                                const bool overlapped)
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif

  // Copy soln and soln_dot vector into solution field, one node at a time
  // Note that soln and soln_dot coming in is the local (non overlapped) soln and soln_dot

  Teuchos::RCP<Albany::AbstractSTKFieldContainer> container =
    outputStkMeshStruct->getFieldContainer();

  // Iterate over the on-processor nodes
  stk::mesh::Selector locally_owned =
    outputStkMeshStruct->metaData->locally_owned_part();

  container->saveSolnVector(solution, solution_dot, locally_owned, m_node_vs);
}

void
Aeras::SpectralDiscretization::setSolutionField(const Thyra_Vector& solution,
                                                const Thyra_Vector& solution_dot,
                                                const Thyra_Vector& solution_dotdot,
                                                const bool overlapped)
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif

  // Copy soln, soln_dot and soln_dotdot vector into solution field, one node at a time
  // Note that soln, soln_dot and soln_dotdot coming in is the local (non overlapped) soln,
  // soln_dot and soln_dotdot

  Teuchos::RCP<Albany::AbstractSTKFieldContainer> container =
    outputStkMeshStruct->getFieldContainer();

  // Iterate over the on-processor nodes
  stk::mesh::Selector locally_owned =
    outputStkMeshStruct->metaData->locally_owned_part();

  container->saveSolnVector(solution, solution_dot, solution_dotdot, locally_owned, m_node_vs);
}

void
Aeras::SpectralDiscretization::setField(const Thyra_Vector &result,
                                        const std::string& name,
                                        bool overlapped)
{
  TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error, "Aeras::SpectralDiscretization::setField() not implemented!");
}

void
Aeras::SpectralDiscretization::setSolutionFieldMV(const Thyra_MultiVector& solution,
                                                  const bool overlapped)
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif

  // Copy soln vector into solution field, one node at a time
  // Note that soln coming in is the local (non overlapped) soln

  Teuchos::RCP<Albany::AbstractSTKFieldContainer> container =
    outputStkMeshStruct->getFieldContainer();

  // Iterate over the on-processor nodes
  stk::mesh::Selector locally_owned =
    outputStkMeshStruct->metaData->locally_owned_part();

  container->saveSolnMultiVector(solution, locally_owned, m_node_vs);
}

inline GO Aeras::SpectralDiscretization::gid(const stk::mesh::Entity node) const
{
  return bulkData.identifier(node)-1;
}

int Aeras::SpectralDiscretization::getOwnedDOF(const int inode, const int eq) const
{
  if (interleavedOrdering)
    return inode*neq + eq;
  else
    return inode + numOwnedNodes*eq;
}

int
Aeras::SpectralDiscretization::getOverlapDOF(const int inode, const int eq) const
{
  if (interleavedOrdering)
    return inode*neq + eq;
  else
    return inode + numOverlapNodes*eq;
}

GO Aeras::SpectralDiscretization::getGlobalDOF(const GO inode, const int eq) const
{
  if (interleavedOrdering)
    return inode*neq + eq;
  else
    return inode + numGlobalNodes*eq;
}

int Aeras::SpectralDiscretization::nonzeroesPerRow(const int neq) const
{
  int numDim = stkMeshStruct->numDim;
  int estNonzeroesPerRow;
  switch (numDim)
  {
  case 0: estNonzeroesPerRow=1*neq; break;
  case 1: estNonzeroesPerRow=3*neq; break;
  case 2: estNonzeroesPerRow=9*neq; break;
  case 3: estNonzeroesPerRow=27*neq; break;
  default:
    TEUCHOS_TEST_FOR_EXCEPTION(
      true, std::logic_error,
      "SpectralDiscretization:  Bad numDim"<< numDim);
  }
  return estNonzeroesPerRow;
}

stk::mesh::EntityId
Aeras::SpectralDiscretization::getMaximumID(const stk::mesh::EntityRank rank) const
{
  // Get the local maximum ID
  bulkData.begin_entities(rank);
  stk::mesh::EntityId last_entity =
     (--bulkData.end_entities(rank))->first.id();

  // Use a parallel MAX reduction to obtain the global maximum ID
  stk::mesh::EntityId result;
  Teuchos::reduceAll(*commT,
                     Teuchos::REDUCE_MAX,
                     1,
                     (GO*)(&last_entity),
                     (GO*)(&result));
  return result;
}

void Aeras::SpectralDiscretization::enrichMeshLines()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  // Initialization
  size_t np  = points_per_edge;
#ifdef OUTPUT_TO_SCREEN
  *out << "Points per edge: " << np << std::endl;
#endif

  // Define the Selectors we are going to need
  stk::mesh::Selector locally_owned   = metaData.locally_owned_part();
  stk::mesh::Selector locally_unowned = !locally_owned;

  GO maxGID    = getMaximumID(stk::topology::NODE_RANK);

  // Fill in the enriched element array
  const stk::mesh::BucketVector & elementBuckets =
    bulkData.get_buckets(stk::topology::ELEMENT_RANK, locally_owned);
  wsElNodeID.resize(elementBuckets.size());
  for (size_t ibuck = 0; ibuck < elementBuckets.size(); ++ibuck)
  {
    stk::mesh::Bucket & elementBucket = *elementBuckets[ibuck];
    wsElNodeID[ibuck].resize(elementBucket.size());
    for (size_t ielem = 0; ielem < elementBucket.size(); ++ielem)
    {
      stk::mesh::Entity element = elementBucket[ielem];
      unsigned numNodes = bulkData.num_nodes(element);
      TEUCHOS_TEST_FOR_EXCEPTION(
        numNodes != 2,
        std::logic_error,
        "Starting elements for enrichment must be linear lines."
        "  Element " << gid(element) << " has " << numNodes << " nodes.");
      const stk::mesh::Entity * nodes = bulkData.begin_nodes(element);
#ifdef OUTPUT_TO_SCREEN
      std::cout << "Proc " << commT->getRank() << ": Bucket " << ibuck
                << ", Element " << gid(element) << " has nodes ";
      for (unsigned inode = 0; inode < numNodes; ++inode)
        std::cout << gid(nodes[inode]) << " ";
      std::cout << std::endl;
      commT->barrier();
#endif

      wsElNodeID[ibuck][ielem].resize(np);

      // Copy the linear end node IDs to the enriched element
      wsElNodeID[ibuck][ielem][0   ] = gid(nodes[0]);
      wsElNodeID[ibuck][ielem][np-1] = gid(nodes[1]);

      // Create new interior nodes for the enriched element
      GO offset = maxGID + gid(element) * (np-2);
      for (unsigned ii = 0; ii < np-2; ++ii) {
        wsElNodeID[ibuck][ielem][ii+1] = offset + ii;
       }
    }
  }
}

void Aeras::SpectralDiscretization::enrichMeshQuads()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  // Initialization
  size_t np  = points_per_edge;
#ifdef OUTPUT_TO_SCREEN
  *out << "Points per edge: " << np << std::endl;
#endif

  // Define the Selectors we are going to need
  stk::mesh::Selector locally_owned   = metaData.locally_owned_part();
  stk::mesh::Selector locally_unowned = !locally_owned;

  // Edges are not created by default, so we create them here
  stk::mesh::create_edges(bulkData);

  size_t np2   = np * np;
  GO maxGID    = getMaximumID(stk::topology::NODE_RANK);
  GO maxEdgeID = getMaximumID(stk::topology::EDGE_RANK);

  // Fill in the enriched edge array
  enrichedEdges.clear();
  const stk::mesh::BucketVector & edgeBuckets =
    bulkData.buckets(stk::topology::EDGE_RANK);
  for (size_t ibuck = 0; ibuck < edgeBuckets.size(); ++ibuck)
  {
    stk::mesh::Bucket & edgeBucket = *edgeBuckets[ibuck];
    for (size_t iedge = 0; iedge < edgeBucket.size(); ++iedge)
    {
      stk::mesh::Entity edge = edgeBucket[iedge];
      unsigned numNodes = bulkData.num_nodes(edge);
      TEUCHOS_TEST_FOR_EXCEPTION(
        numNodes != 2,
        std::logic_error,
        "Starting edges for enriched elements must be linear.  Edge "
        << gid(edge) << " has " << numNodes << " nodes.");
      const stk::mesh::Entity * nodes = bulkData.begin_nodes(edge);
      enrichedEdges[gid(edge)].resize(np);
      enrichedEdges[gid(edge)][0] = gid(nodes[0]);
      for (GO inode = 1; inode < np-1; ++inode)
      {
        enrichedEdges[gid(edge)][inode] =
          maxGID + gid(edge)*(np-2) + inode - 1;
      }
      enrichedEdges[gid(edge)][np-1] = gid(nodes[1]);
    }
  }

  // Fill in the enriched element array
  const stk::mesh::BucketVector & elementBuckets =
    bulkData.get_buckets(stk::topology::ELEMENT_RANK, locally_owned);
  wsElNodeID.resize(elementBuckets.size());
  for (size_t ibuck = 0; ibuck < elementBuckets.size(); ++ibuck)
  {
    stk::mesh::Bucket & elementBucket = *elementBuckets[ibuck];
    wsElNodeID[ibuck].resize(elementBucket.size());
    for (size_t ielem = 0; ielem < elementBucket.size(); ++ielem)
    {
      stk::mesh::Entity element = elementBucket[ielem];
      unsigned numNodes = bulkData.num_nodes(element);
      TEUCHOS_TEST_FOR_EXCEPTION(
        numNodes != 4,
        std::logic_error,
        "Starting elements for enrichment must be linear quadrilaterals."
        "  Element " << gid(element) << " has " << numNodes << " nodes.");
      const stk::mesh::Entity * nodes = bulkData.begin_nodes(element);
#ifdef OUTPUT_TO_SCREEN
      std::cout << "Proc " << commT->getRank() << ": Bucket " << ibuck
                << ", Element " << gid(element) << " has nodes ";
      for (unsigned inode = 0; inode < numNodes; ++inode)
        std::cout << gid(nodes[inode]) << " ";
      std::cout << std::endl;
      commT->barrier();
#endif

      wsElNodeID[ibuck][ielem].resize(np2);

      // Copy the linear corner node IDs to the enriched element
      wsElNodeID[ibuck][ielem][0                 ] = gid(nodes[0]);
      wsElNodeID[ibuck][ielem][            (np-1)] = gid(nodes[1]);
      wsElNodeID[ibuck][ielem][(np-1)*np + (np-1)] = gid(nodes[2]);
      wsElNodeID[ibuck][ielem][(np-1)*np         ] = gid(nodes[3]);

      // Copy the enriched edge nodes to the enriched element.  Note
      // that the enriched edge may or may not be aligned with the
      // tensor grid edge.  So we check the first node ID and copy
      // in the appropriate direction.
      const stk::mesh::Entity * edges = bulkData.begin_edges(element);

      // Edge 0
      const stk::mesh::Entity * edgeNodes = bulkData.begin_nodes(edges[0]);
      GO edgeID = gid(edges[0]);
      for (unsigned inode = 1; inode < np-1; ++inode)
        if (edgeNodes[0] == nodes[0])
          wsElNodeID[ibuck][ielem][inode] = enrichedEdges[edgeID][inode];
        else
          wsElNodeID[ibuck][ielem][inode] = enrichedEdges[edgeID][np-inode-1];

      // Edge 1
      edgeNodes = bulkData.begin_nodes(edges[1]);
      edgeID = gid(edges[1]);
      for (unsigned inode = 1; inode < np-1; ++inode)
        if (edgeNodes[0] == nodes[1])
          wsElNodeID[ibuck][ielem][inode*np + (np-1)] =
            enrichedEdges[edgeID][inode];
        else
          wsElNodeID[ibuck][ielem][inode*np + (np-1)] =
            enrichedEdges[edgeID][np-inode-1];

      // Edge 2
      edgeNodes = bulkData.begin_nodes(edges[2]);
      edgeID = gid(edges[2]);
      for (unsigned inode = 1; inode < np-1; ++inode)
        if (edgeNodes[0] == nodes[2])
          wsElNodeID[ibuck][ielem][(np-1)*np + inode] =
            enrichedEdges[edgeID][np-inode-1];
        else
          wsElNodeID[ibuck][ielem][(np-1)*np + inode] =
            enrichedEdges[edgeID][inode];

      // Edge 3
      edgeNodes = bulkData.begin_nodes(edges[3]);
      edgeID = gid(edges[3]);
      for (unsigned inode = 1; inode < np-1; ++inode)
        if (edgeNodes[0] == nodes[3])
          wsElNodeID[ibuck][ielem][inode*np] =
            enrichedEdges[edgeID][np-inode-1];
        else
          wsElNodeID[ibuck][ielem][inode*np] =
            enrichedEdges[edgeID][inode];

      // Create new interior nodes for the enriched element
      GO offset = maxGID + (maxEdgeID+1) * (np-2) +
        gid(element) * (np-2) * (np-2);
      for (unsigned ii = 0; ii < np-2; ++ii)
        for (unsigned jj = 0; jj < np-2; ++jj)
          wsElNodeID[ibuck][ielem][(ii+1)*np + (jj+1)] =
            offset + ii * (np-2) + jj - 1;
    }
  }

  // Mark locally owned edges as owned
  edgeIsOwned.clear();
  const stk::mesh::BucketVector & ownedEdgeBuckets =
    bulkData.get_buckets(stk::topology::EDGE_RANK, locally_owned);
  for (size_t ibuck = 0; ibuck < ownedEdgeBuckets.size(); ++ibuck)
  {
    stk::mesh::Bucket & edgeBucket = *ownedEdgeBuckets[ibuck];
    for (size_t iedge = 0; iedge < edgeBucket.size(); ++iedge)
      edgeIsOwned[gid(edgeBucket[iedge])] = true;
  }

  // Marked locally shared edges as unowned
  const stk::mesh::BucketVector & sharedEdgeBuckets =
    bulkData.get_buckets(stk::topology::EDGE_RANK, locally_unowned);
  for (size_t ibuck = 0; ibuck < sharedEdgeBuckets.size(); ++ibuck)
  {
    stk::mesh::Bucket & edgeBucket = *sharedEdgeBuckets[ibuck];
    for (size_t iedge = 0; iedge < edgeBucket.size(); ++iedge)
      edgeIsOwned[gid(edgeBucket[iedge])] = false;
  }
}

void Aeras::SpectralDiscretization::computeOwnedNodesAndUnknownsLines()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  // Initialization
  int np = points_per_edge;

  // Compute the STK Mesh selector
  stk::mesh::Selector select_owned =
    stk::mesh::Selector(metaData.locally_owned_part());

#ifdef OUTPUT_TO_SCREEN
  //////////////////////////////////////////////////////////////////////
  // Debugging code
  stk::mesh::get_selected_entities(select_owned,
           bulkData.buckets(stk::topology::ELEMENT_RANK),
           cells);
  for (int rank = 0; rank < commT->getSize(); ++rank)
  {
    if (rank == commT->getRank())
    {
      std::cout << std::endl << "Rank " << rank << ": owned elements = { ";
      for (size_t i = 0; i < cells.size(); ++i)
      {
        std::cout << gid(cells[i]) << "(";
        const stk::mesh::Entity * nodes = bulkData.begin_nodes(cells[i]);
        std::cout << gid(nodes[0]) << "," << gid(nodes[1]) << ") ";
      }
      std::cout << "}" << std::endl;
    }
    commT->barrier();
  }
  //////////////////////////////////////////////////////////////////////
#endif

  // The owned nodes will be the owned end nodes from the original
  // linear STK mesh, plus all of the enriched interior nodes.  Start
  // with the end nodes.
  stk::mesh::get_selected_entities(select_owned,
           bulkData.buckets(stk::topology::NODE_RANK),
           ownednodes);
  numOwnedNodes = ownednodes.size();
#ifdef OUTPUT_TO_SCREEN
  for (int rank = 0; rank < commT->getSize(); ++rank)
  {
    if (rank == commT->getRank())
    {
      std::cout << std::endl << "Rank " << rank << ": owned nodes = { ";
      for (size_t i = 0; i < ownednodes.size(); ++i)
        std::cout << gid(ownednodes[i]) << " ";
      std::cout << "}" << std::endl;
    }
    commT->barrier();
  }
#endif

  // Add the number of nodes from the enriched element interiors
  const stk::mesh::BucketVector & elementBuckets =
    bulkData.get_buckets(stk::topology::ELEMENT_RANK, select_owned);
  size_t numNewElementNodes = 0;
  for (size_t ibuck = 0; ibuck < elementBuckets.size(); ++ibuck)
  {
    stk::mesh::Bucket & elementBucket = *elementBuckets[ibuck];
    numNewElementNodes += elementBucket.size() * (np-2);
  }
  numOwnedNodes += numNewElementNodes;

  //////////////////////////////////////////////////////////////////////
  // N.B.: Filling the indicesT array is inherently serial
  Teuchos::Array<GO> indicesT(numOwnedNodes);
  size_t inode = 0;

  // Add the ownednodes to indicesT
  for (size_t i = 0; i < ownednodes.size(); ++i)
    indicesT[inode++] = gid(ownednodes[i]);

  // Add all of the interior nodes of the enriched elements to indicesT
  for (size_t ibuck = 0; ibuck < wsElNodeID.size(); ++ibuck)
    for (size_t ielem = 0; ielem < wsElNodeID[ibuck].size(); ++ielem)
      for (size_t ii = 1; ii < np-1; ++ii)
        indicesT[inode++] = wsElNodeID[ibuck][ielem][ii];

#ifdef OUTPUT_TO_SCREEN
  for (int rank = 0; rank < commT->getSize(); ++rank)
  {
    commT->barrier();
    if (rank == commT->getRank())
      std::cout << "P" << rank
                << ": computeOwnedNodesAndUnknownsLines(), inode = " << inode
                << ", numOwnedNodes = " << numOwnedNodes << ", indicesT = "
                << indicesT << std::endl;
  }
#endif
  assert (inode == numOwnedNodes);
  // End fill indicesT
  //////////////////////////////////////////////////////////////////////

  m_node_vs = Teuchos::null; // delete existing map happens here on remesh
  m_node_vs = Albany::createVectorSpace(commT,indicesT());

  numGlobalNodes = Albany::getMaxAllGlobalIndex(m_node_vs) + 1;

  Teuchos::Array<GO> dofIndicesT(numOwnedNodes * neq);
  for (size_t i = 0; i < numOwnedNodes; ++i)
    for (size_t j = 0; j < neq; ++j)
      dofIndicesT[getOwnedDOF(i,j)] = getGlobalDOF(indicesT[i],j);

  m_vs = Teuchos::null; // delete existing map happens here on remesh
  m_vs = Albany::createVectorSpace(commT,dofIndicesT);

  TEUCHOS_TEST_FOR_EXCEPTION(
    Teuchos::nonnull(stkMeshStruct->nodal_data_base),
    std::logic_error,
    "Nodal database not implemented for Aeras::SpectralDiscretization");

}

void Aeras::SpectralDiscretization::computeOwnedNodesAndUnknownsQuads()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  // Initialization
  int np = points_per_edge;

  // Compute the STK Mesh selector
  stk::mesh::Selector select_owned =
    stk::mesh::Selector(metaData.locally_owned_part());

#ifdef OUTPUT_TO_SCREEN
  //////////////////////////////////////////////////////////////////////
  // Debugging code
  stk::mesh::get_selected_entities(select_owned,
           bulkData.buckets(stk::topology::ELEMENT_RANK),
           cells);
  for (int rank = 0; rank < commT->getSize(); ++rank)
  {
    if (rank == commT->getRank())
    {
      std::cout << std::endl << "Rank " << rank << ": owned elements = { ";
      for (size_t i = 0; i < cells.size(); ++i)
      {
        std::cout << gid(cells[i]) << "(";
        const stk::mesh::Entity * nodes = bulkData.begin_nodes(cells[i]);
        std::cout << gid(nodes[0]) << "," << gid(nodes[1]) << ","
                  << gid(nodes[2]) << "," << gid(nodes[3]) << ") ";
      }
      std::cout << "}" << std::endl;
    }
    commT->barrier();
  }
  //////////////////////////////////////////////////////////////////////
#endif

  // The owned nodes will be the owned corner nodes from the original
  // linear STK mesh, the non-endpoint nodes from the owned edges, plus
  // all of the enriched interior nodes.  Start with the corner nodes.
  stk::mesh::get_selected_entities(select_owned,
           bulkData.buckets(stk::topology::NODE_RANK),
           ownednodes);
  numOwnedNodes = ownednodes.size();
#ifdef OUTPUT_TO_SCREEN
  for (int rank = 0; rank < commT->getSize(); ++rank)
  {
    if (rank == commT->getRank())
    {
      std::cout << std::endl << "Rank " << rank << ": owned nodes = { ";
      for (size_t i = 0; i < ownednodes.size(); ++i)
        std::cout << gid(ownednodes[i]) << " ";
      std::cout << "}" << std::endl;
    }
    commT->barrier();
  }
#endif

  // Now add the number of nodes from the owned edges
  const stk::mesh::BucketVector & ownedEdgeBuckets =
    bulkData.get_buckets(stk::topology::EDGE_RANK, select_owned);
  for (size_t ibuck = 0; ibuck < ownedEdgeBuckets.size(); ++ibuck)
  {
    stk::mesh::Bucket & edgeBucket = *ownedEdgeBuckets[ibuck];
    numOwnedNodes += edgeBucket.size() * (np-2);
  }

  // Now add the number of nodes from the enriched element interiors
  const stk::mesh::BucketVector & elementBuckets =
    bulkData.get_buckets(stk::topology::ELEMENT_RANK, select_owned);
  size_t numNewElementNodes = 0;
  for (size_t ibuck = 0; ibuck < elementBuckets.size(); ++ibuck)
  {
    stk::mesh::Bucket & elementBucket = *elementBuckets[ibuck];
    numNewElementNodes += elementBucket.size() * (np-2) * (np-2);
  }
  numOwnedNodes += numNewElementNodes;

  //////////////////////////////////////////////////////////////////////
  // N.B.: Filling the indicesT array is inherently serial
  Teuchos::Array<GO> indicesT(numOwnedNodes);
  size_t inode = 0;

  // Add the ownednodes to indicesT
  for (size_t i = 0; i < ownednodes.size(); ++i)
    indicesT[inode++] = gid(ownednodes[i]);

  // Get a bucket of all the edges so that the local indexes match the
  // enrichedEdges indexes.  Loop over these edges to add their nodes
  // to indicesT, when the edges are owned
  const stk::mesh::BucketVector edgeBuckets =
    bulkData.buckets(stk::topology::EDGE_RANK);
  for (size_t ibuck = 0; ibuck < edgeBuckets.size(); ++ibuck)
  {
    stk::mesh::Bucket & edgeBucket = *edgeBuckets[ibuck];
    for (size_t iedge = 0; iedge < edgeBucket.size(); ++iedge)
    {
      stk::mesh::Entity edge = edgeBucket[iedge];
      GO edgeID = gid(edge);
      if (edgeIsOwned[edgeID])
      {
        for (size_t lnode = 1; lnode < np-1; ++lnode)
          indicesT[inode++] = enrichedEdges[edgeID][lnode];
      }
    }
  }

  // Add all of the interior nodes of the enriched elements to indicesT
  for (size_t ibuck = 0; ibuck < wsElNodeID.size(); ++ibuck)
    for (size_t ielem = 0; ielem < wsElNodeID[ibuck].size(); ++ielem)
      for (size_t ii = 1; ii < np-1; ++ii)
        for (size_t jj = 1; jj < np-1; ++jj)
          indicesT[inode++] = wsElNodeID[ibuck][ielem][ii*np+jj];

#ifdef OUTPUT_TO_SCREEN
  for (int rank = 0; rank < commT->getSize(); ++rank)
  {
    commT->barrier();
    if (rank == commT->getRank())
      std::cout << "P" << rank
                << ": computeOwnedNodesAndUnknownsQuads(), inode = " << inode
                << ", numOwnedNodes = " << numOwnedNodes << ", indicesT = "
                << indicesT << std::endl;
  }
#endif
  assert (inode == numOwnedNodes);
  // End fill indicesT
  //////////////////////////////////////////////////////////////////////

  m_node_vs = Teuchos::null; // delete existing map happens here on remesh
  m_node_vs = Albany::createVectorSpace(commT, indicesT());

  numGlobalNodes = Albany::getMaxAllGlobalIndex(m_node_vs) + 1;

  Teuchos::Array<GO> dofIndicesT(numOwnedNodes * neq);
  for (size_t i = 0; i < numOwnedNodes; ++i)
    for (size_t j = 0; j < neq; ++j)
      dofIndicesT[getOwnedDOF(i,j)] = getGlobalDOF(indicesT[i],j);

  m_vs = Teuchos::null; // delete existing map happens here on remesh
  m_vs = Albany::createVectorSpace(commT, dofIndicesT());

  TEUCHOS_TEST_FOR_EXCEPTION(
    Teuchos::nonnull(stkMeshStruct->nodal_data_base),
    std::logic_error,
    "Nodal database not implemented for Aeras::SpectralDiscretization");

}

void Aeras::SpectralDiscretization::computeOverlapNodesAndUnknownsLines()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  // Initialization
  int np = points_per_edge;

  // Compute the STK Mesh selector
  stk::mesh::Selector select_unowned =
    stk::mesh::Selector(metaData.globally_shared_part()) -
    stk::mesh::Selector(metaData.locally_owned_part());

  // Use m_node_vs to get the number of locally owned nodes
  numOverlapNodes = Albany::getNumLocalElements(m_node_vs); 

  // Count the number of unowned nodes from the original linear STK mesh
  std::vector< stk::mesh::Entity > unownedNodes;
  stk::mesh::get_selected_entities(select_unowned,
           bulkData.buckets(stk::topology::NODE_RANK),
           unownedNodes);
  numOverlapNodes += unownedNodes.size();
#ifdef OUTPUT_TO_SCREEN
  for (int rank = 0; rank < commT->getSize(); ++rank)
  {
    commT->barrier();
    if (rank == commT->getRank())
    {
      std::cout << std::endl << "Rank " << rank << ": unowned nodes = { ";
      for (size_t i = 0; i < unownedNodes.size(); ++i)
        std::cout << gid(unownedNodes[i]) << " ";
      std::cout << "}" << std::endl;
    }
  }
#endif

  //////////////////////////////////////////////////////////////////////
  // N.B.: Filling the overlapIndicesT array is inherently serial

  // Copy owned indices to overlap indices
  Teuchos::ArrayView<const GO> ownedIndicesT = Albany::getNodeElementList(m_node_vs);
  Teuchos::Array<GO> overlapIndicesT(numOverlapNodes);
  for (size_t i = 0; i < ownedIndicesT.size(); ++i)
    overlapIndicesT[i] = ownedIndicesT[i];

  // Copy shared nodes from original STK mesh to overlap indices
  size_t inode = ownedIndicesT.size();
  for (size_t i = 0; i < unownedNodes.size(); ++i)
    overlapIndicesT[inode++] = gid(unownedNodes[i]);

#ifdef OUTPUT_TO_SCREEN
  for (int rank = 0; rank < commT->getSize(); ++rank)
  {
    commT->barrier();
    if (rank == commT->getRank())
      std::cout << "P" << rank
                << ": computeOverlapNodesAndUnknownsLines(), inode = " << inode
                << ", numOwnedNodes = " << numOwnedNodes << ", indicesT = "
                << overlapIndicesT << std::endl;
  }
#endif
  assert (inode == numOverlapNodes);
  // End fill overlapIndicesT
  //////////////////////////////////////////////////////////////////////

  m_overlap_node_vs = Teuchos::null; // delete existing map happens here on remesh
  m_overlap_node_vs = Albany::createVectorSpace(commT, overlapIndicesT()); 

  // Compute the overlap DOF indices.  Since these might be strided by
  // the number of overlap nodes, we compute them from scratch.
  Teuchos::Array<GO> overlapDofIndicesT(numOverlapNodes * neq);
  for (size_t i = 0; i < numOverlapNodes; ++i)
    for (size_t j = 0; j < neq; ++j)
      overlapDofIndicesT[getOverlapDOF(i,j)] =
        getGlobalDOF(overlapIndicesT[i],j);

  m_overlap_vs = Teuchos::null; // delete existing map happens here on remesh
  m_overlap_vs = Albany::createVectorSpace(commT, overlapDofIndicesT()); 

  coordinates.resize(3*numOverlapNodes);
}

void Aeras::SpectralDiscretization::computeOverlapNodesAndUnknownsQuads()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  // Initialization
  int np = points_per_edge;

  // Compute the STK Mesh selector
  stk::mesh::Selector select_unowned =
    stk::mesh::Selector(metaData.globally_shared_part()) -
    stk::mesh::Selector(metaData.locally_owned_part());

  // Use node_mapT to get the number of locally owned nodes
  numOverlapNodes = Albany::getNumLocalElements(m_node_vs);

  // Count the number of unowned nodes from the original linear STK mesh
  std::vector< stk::mesh::Entity > unownedNodes;
  stk::mesh::get_selected_entities(select_unowned,
           bulkData.buckets(stk::topology::NODE_RANK),
           unownedNodes);
  numOverlapNodes += unownedNodes.size();
#ifdef OUTPUT_TO_SCREEN
  for (int rank = 0; rank < commT->getSize(); ++rank)
  {
    commT->barrier();
    if (rank == commT->getRank())
    {
      std::cout << std::endl << "Rank " << rank << ": unowned nodes = { ";
      for (size_t i = 0; i < unownedNodes.size(); ++i)
        std::cout << gid(unownedNodes[i]) << " ";
      std::cout << "}" << std::endl;
    }
  }
#endif

  // Now add the number of nodes from the edges
  const stk::mesh::BucketVector & overlapEdgeBuckets =
    bulkData.get_buckets(stk::topology::EDGE_RANK, select_unowned);
#ifdef OUTPUT_TO_SCREEN
  for (int rank = 0; rank < commT->getSize(); ++rank)
  {
    commT->barrier();
    if (rank == commT->getRank())
    {
      std::cout << std::endl << "Rank " << rank << ": unowned shared edges = { ";
      for (size_t ibuck = 0; ibuck < overlapEdgeBuckets.size(); ++ibuck)
      {
        stk::mesh::Bucket & edgeBucket = *overlapEdgeBuckets[ibuck];
        for (size_t iedge = 0; iedge < edgeBucket.size(); ++iedge)
        {
          const stk::mesh::Entity * nodes =
            bulkData.begin_nodes(edgeBucket[iedge]);
          std::cout << "(" << gid(nodes[0]) << "," << gid(nodes[1]) << ") ";
        }
      }
      std::cout << "}" << std::endl;
    }
  }
#endif
  for (size_t ibuck = 0; ibuck < overlapEdgeBuckets.size(); ++ibuck)
  {
    stk::mesh::Bucket & edgeBucket = *overlapEdgeBuckets[ibuck];
    numOverlapNodes += edgeBucket.size() * (np-2);
  }

  //////////////////////////////////////////////////////////////////////
  // N.B.: Filling the overlapIndicesT array is inherently serial

  // Copy owned indices to overlap indices
  Teuchos::ArrayView<const GO> ownedIndicesT = Albany::getNodeElementList(m_node_vs);
  Teuchos::Array<GO> overlapIndicesT(numOverlapNodes);
  for (size_t i = 0; i < ownedIndicesT.size(); ++i)
    overlapIndicesT[i] = ownedIndicesT[i];

  // Copy shared nodes from original STK mesh to overlap indices
  size_t inode = ownedIndicesT.size();
  for (size_t i = 0; i < unownedNodes.size(); ++i)
    overlapIndicesT[inode++] = gid(unownedNodes[i]);

  // Get a bucket of all the edges so that the local indexes match the
  // enrichedEdges indexes.  Loop over these edges to add their nodes
  // to overlapIndicesT, when the edges are not owned
  for (size_t ibuck = 0; ibuck < overlapEdgeBuckets.size(); ++ibuck)
  {
    stk::mesh::Bucket & edgeBucket = *overlapEdgeBuckets[ibuck];
    for (size_t iedge = 0; iedge < edgeBucket.size(); ++iedge)
    {
      stk::mesh::Entity edge = edgeBucket[iedge];
      GO edgeID = gid(edge);
      if (!edgeIsOwned[edgeID])
      {
        for (size_t lnode = 1; lnode < np-1; ++lnode)
          overlapIndicesT[inode++] = enrichedEdges[edgeID][lnode];
      }
    }
  }

#ifdef OUTPUT_TO_SCREEN
  for (int rank = 0; rank < commT->getSize(); ++rank)
  {
    commT->barrier();
    if (rank == commT->getRank())
      std::cout << "P" << rank
                << ": computeOverlapNodesAndUnknownsQuads(), inode = " << inode
                << ", numOwnedNodes = " << numOwnedNodes << ", indicesT = "
                << overlapIndicesT << std::endl;
  }
#endif
  assert (inode == numOverlapNodes);
  // End fill overlapIndicesT
  //////////////////////////////////////////////////////////////////////

  m_overlap_node_vs = Teuchos::null; // delete existing vector space happens here on remesh
  m_overlap_node_vs = Albany::createVectorSpace(commT, overlapIndicesT());

  // Compute the overlap DOF indices.  Since these might be strided by
  // the number of overlap nodes, we compute them from scratch.
  Teuchos::Array<GO> overlapDofIndicesT(numOverlapNodes * neq);
  for (size_t i = 0; i < numOverlapNodes; ++i)
    for (size_t j = 0; j < neq; ++j)
      overlapDofIndicesT[getOverlapDOF(i,j)] = getGlobalDOF(overlapIndicesT[i],j);

  m_overlap_vs = Teuchos::null; // delete existing vector space happens here on remesh
  m_overlap_vs = Albany::createVectorSpace(commT, overlapDofIndicesT()); ;

  coordinates.resize(3*numOverlapNodes);
}

void Aeras::SpectralDiscretization::computeCoordsLines()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  // Initialization
  typedef Kokkos::DynRankView<RealType, PHX::Device> Field_t;
  typedef Albany::AbstractSTKFieldContainer::VectorFieldType VectorFieldType;
  int np  = points_per_edge;
  int deg = np - 1;

  // Compute the 1D Gauss-Lobatto quadrature
  Teuchos::RCP< Intrepid2::Cubature<PHX::Device> > gl1D =
    Teuchos::rcp(
      new Intrepid2::CubaturePolylib<PHX::Device, RealType, RealType>(
        2*deg-1, Intrepid2::POLYTYPE_GAUSS_LOBATTO));
  Field_t refCoords("AAA", np, 1);
  Field_t refWeights("AAA", np);
  gl1D->getCubature(refCoords, refWeights);

  // Get the appropriate STK element buckets for extracting the
  // element end nodes
  stk::mesh::Selector select_all =
    stk::mesh::Selector(metaData.universal_part());
  stk::mesh::BucketVector const& buckets =
    bulkData.get_buckets(stk::topology::ELEMENT_RANK, select_all);

  // Allocate and populate the coordinates
  VectorFieldType * coordinates_field = stkMeshStruct->getCoordinatesField();
  double c[2];
  size_t numWorksets = wsElNodeID.size();
  coords.resize(numWorksets);
  for (size_t iws = 0; iws < numWorksets; ++iws)
  {
    stk::mesh::Bucket & bucket = *buckets[iws];
    size_t numElements = wsElNodeID[iws].size();
    coords[iws].resize(numElements);
    for (size_t ielem = 0; ielem < numElements; ++ielem)
    {
      stk::mesh::Entity element = bucket[ielem];
      const stk::mesh::Entity * stkNodes = bulkData.begin_nodes(element);
      coords[iws][ielem].resize(np);
      for (size_t inode = 0; inode < np; ++inode)
      {
        double * coordVals = new double[3];
        coords[iws][ielem][inode] = coordVals;
        toDelete.push_back(coordVals);
      }

      // Get the coordinates value along this axis of the end nodes
      // from the STK mesh
      for (size_t ii = 0; ii < 2; ++ii) {
        c[ii] = stk::mesh::field_data(*coordinates_field,
                                      stkNodes[ii])[0];
      }
      //The following is for periodic BCs.  This will only be relevant for the x-z hydrostatic equations.
      if (stkMeshStruct->PBCStruct.periodic[0])
      {
        bool anyXeqZero=false;
        for (int j=0; j < 2; j++) {
          if (c[j] == 0.0)
            anyXeqZero=true;
        }
        if (anyXeqZero)
        {
          bool flipZeroToScale=false;
          for (int j=0; j < 2; j++)
            if (c[j] > stkMeshStruct->PBCStruct.scale[0]/1.9)
              flipZeroToScale=true;
          if (flipZeroToScale)
          {
            for (int j=0; j < 2; j++)
            {
              if (c[j] == 0.0)
              {
                c[j] = stkMeshStruct->PBCStruct.scale[0];
              }
            }
          }
        }
      }
      for (size_t inode = 0; inode < np; ++inode)
      {
        double x = refCoords(inode,0);
        coords[iws][ielem][inode][0] = (-c[0] * (x-1.0) +
                                        c[1] * (x+1.0)) * 0.5;
        coords[iws][ielem][inode][1] = 0.0;
        coords[iws][ielem][inode][2] = 0.0;
      }
    }
  }
}

void Aeras::SpectralDiscretization::computeCoordsQuads()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  // Initialization
  typedef Kokkos::DynRankView<RealType, PHX::Device> Field_t;
  typedef Albany::AbstractSTKFieldContainer::VectorFieldType VectorFieldType;
  int np  = points_per_edge;
  int np2 = np * np;
  int deg = np - 1;

  // Compute the 1D Gauss-Lobatto quadrature
  Intrepid2::CubaturePolylib<PHX::Device, RealType, RealType>
    gl1D(2*deg-1, Intrepid2::POLYTYPE_GAUSS_LOBATTO);

  // Compute the 2D Gauss-Lobatto cubature.  These will be the nodal
  // points of the reference spectral element
//  std::vector<Teuchos::RCP< Intrepid2::Cubature<PHX::Device> > > axes;
//  axes.push_back(gl1D);
//  axes.push_back(gl1D);
  //Intrepid2::CubatureTensor<PHX::Device> gl2D(axes);
  Intrepid2::CubatureTensor<PHX::Device> gl2D(gl1D, gl1D);
  Field_t refCoords("AAA", np2, 2);
  Field_t refWeights("AAA", np2);
  gl2D.getCubature(refCoords, refWeights);

  // Get the appropriate STK element buckets for extracting the
  // element corner nodes
  stk::mesh::Selector select_all =
    stk::mesh::Selector(metaData.universal_part());
  stk::mesh::BucketVector const& buckets =
    bulkData.get_buckets(stk::topology::ELEMENT_RANK, select_all);

  // Allocate and populate the coordinates
  VectorFieldType * coordinates_field = stkMeshStruct->getCoordinatesField();
  double c[4];
  size_t numWorksets = wsElNodeID.size();
  coords.resize(numWorksets);
  for (size_t iws = 0; iws < numWorksets; ++iws)
  {
    stk::mesh::Bucket & bucket = *buckets[iws];
    size_t numElements = wsElNodeID[iws].size();
    coords[iws].resize(numElements);
    for (size_t ielem = 0; ielem < numElements; ++ielem)
    {
      stk::mesh::Entity element = bucket[ielem];
      const stk::mesh::Entity * stkNodes = bulkData.begin_nodes(element);
      coords[iws][ielem].resize(np2);
      for (size_t inode = 0; inode < np2; ++inode)
      {
        double * coordVals = new double[3];
        coords[iws][ielem][inode] = coordVals;
        toDelete.push_back(coordVals);
      }

      // Phase I: project the reference element coordinates onto the
      // "twisted plane" defined by the four corners of the linear STK
      // shell element, using bilinear interpolation
      for (size_t idim = 0; idim < 3; ++idim)
      {
        // Get the coordinates value along this axis of the corner
        // nodes from the STK mesh
        for (size_t ii = 0; ii < 4; ++ii)
          c[ii] = stk::mesh::field_data(*coordinates_field, stkNodes[ii])[idim];
        for (size_t inode = 0; inode < np2; ++inode)
        {
          double x = refCoords(inode,0);
          double y = refCoords(inode,1);
          coords[iws][ielem][inode][idim] = (c[0] * (x-1.0) * (y-1.0) -
                                             c[1] * (x+1.0) * (y-1.0) +
                                             c[2] * (x+1.0) * (y+1.0) -
                                             c[3] * (x-1.0) * (y+1.0)) * 0.25;
        }
      }

      // Phase II: project the coordinate values computed in Phase I
      // from the "twisted plane" onto the unit sphere
      for (size_t inode = 0; inode < np2; ++inode)
      {
        double distance = 0.0;
        for (size_t idim = 0; idim < 3; ++idim)
          distance += coords[iws][ielem][inode][idim] *
                      coords[iws][ielem][inode][idim];
        distance = sqrt(distance);
        for (size_t idim = 0; idim < 3; ++idim)
          coords[iws][ielem][inode][idim] /= distance;
      }
    }
  }
}


void Aeras::SpectralDiscretization::computeGraphs()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  computeGraphsUpToFillComplete();
  fillCompleteGraphs();
}

void Aeras::SpectralDiscretization::computeGraphs_Explicit()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  if (explicit_scheme == true) {
    computeGraphsExplicitUpToFillComplete();
    fillCompleteGraphsExplicit();
  }
}

void Aeras::SpectralDiscretization::computeGraphsExplicitUpToFillComplete()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif

#ifdef OUTPUT_TO_SCREEN
  *out << "nodes_per_element: " << nodes_per_element << std::endl;
#endif

  m_overlap_jac_factory = Teuchos::null; // delete existing graph here on remesh
  //Graph for diagonal matrix
  m_overlap_jac_factory = Teuchos::rcp( new Albany::ThyraCrsMatrixFactory(m_overlap_vs,m_overlap_vs,1) );

  stk::mesh::Selector select_owned =
    stk::mesh::Selector(metaData.locally_owned_part());

  const stk::mesh::BucketVector & buckets =
    bulkData.get_buckets(stk::topology::ELEMENT_RANK, select_owned);

  const int numBuckets = buckets.size();

  if (commT->getRank()==0)
    *out << "SpectralDisc: " << cells.size() << " elements on Proc 0 "
         << std::endl;

  GO row;
  Teuchos::ArrayView<GO> colAV;

  //Populate the graphs
  for (int b = 0; b < numBuckets; ++b)
  {
    stk::mesh::Bucket & buck = *buckets[b];
    // i is the element index within bucket b
    for (std::size_t i = 0; i < buck.size(); ++i)
    {
      Teuchos::ArrayRCP< GO > node_rels = wsElNodeID[b][i];
      for (int j = 0; j < nodes_per_element; ++j)
      {
        const GO rowNode = node_rels[j];
        // loop over eqs
        for (std::size_t k=0; k < neq; k++)
        {
          row = getGlobalDOF(rowNode, k);
          //col = row
          colAV = Teuchos::arrayView(&row, 1);
          m_overlap_jac_factory->insertGlobalIndices(row, colAV);
        }
      }
    }
  }
}

void Aeras::SpectralDiscretization::computeGraphsUpToFillComplete()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
#ifdef OUTPUT_TO_SCREEN
  *out << "nodes_per_element: " << nodes_per_element << std::endl;
#endif

  //Create implicit overlap jac factory and populate  
  m_implicit_overlap_jac_factory = Teuchos::rcp( new Albany::ThyraCrsMatrixFactory(m_overlap_vs,m_overlap_vs,neq*nodes_per_element) );
  //For implicit scheme, m_overlap_jac_factory = m_implicit_overlap_jac_factory
  if (explicit_scheme == false) {
    m_overlap_jac_factory = Teuchos::rcp( new Albany::ThyraCrsMatrixFactory(m_overlap_vs,m_overlap_vs,neq*nodes_per_element) );
  }
#ifdef OUTPUT_TO_SCREEN
  *out << "neq*nodes_per_element: " << neq*nodes_per_element << std::endl;
#endif

  stk::mesh::Selector select_owned =
    stk::mesh::Selector(metaData.locally_owned_part());

  const stk::mesh::BucketVector & buckets =
    bulkData.get_buckets(stk::topology::ELEMENT_RANK, select_owned);

  const int numBuckets = buckets.size();

  if (commT->getRank()==0)
    *out << "SpectralDisc: " << cells.size() << " elements on Proc 0 "
         << std::endl;

  GO row, col;
  Teuchos::ArrayView<GO> colAV;

  //Populate the graphs
  for (int b = 0; b < numBuckets; ++b)
  {
    stk::mesh::Bucket & buck = *buckets[b];
    // i is the element index within bucket b
    for (std::size_t i = 0; i < buck.size(); ++i)
    {
      Teuchos::ArrayRCP< GO > node_rels = wsElNodeID[b][i];
      for (int j = 0; j < nodes_per_element; ++j)
      {
        const GO rowNode = node_rels[j];
        // loop over eqs
        for (std::size_t k=0; k < neq; k++)
        {
          row = getGlobalDOF(rowNode, k);
          for (std::size_t l=0; l < nodes_per_element; l++)
          {
            const GO colNode = node_rels[l];
            for (std::size_t m=0; m < neq; m++)
            {
              col = getGlobalDOF(colNode, m);
              m_implicit_overlap_jac_factory->insertGlobalIndices(row, Teuchos::arrayView(&col,1));
              m_implicit_overlap_jac_factory->insertGlobalIndices(row, Teuchos::arrayView(&col,1));
              //IKT, FIXME?  The following line might be needed 
              //m_implicit_overlap_jac_factory->insertGlobalIndices(col, Teuchos::arrayView(&row,1));
            }
          }
        }
      }
    }
  }
  //For implicit scheme, m_overlap_jac_factory = m_implicit_overlap_jac_factory 
  if (explicit_scheme == false) {
    for (int b = 0; b < numBuckets; ++b)
    {
      stk::mesh::Bucket & buck = *buckets[b];
      // i is the element index within bucket b
      for (std::size_t i = 0; i < buck.size(); ++i)
      {
        Teuchos::ArrayRCP< GO > node_rels = wsElNodeID[b][i];
        for (int j = 0; j < nodes_per_element; ++j)
        {
          const GO rowNode = node_rels[j];
          // loop over eqs
          for (std::size_t k=0; k < neq; k++)
          {
            row = getGlobalDOF(rowNode, k);
            for (std::size_t l=0; l < nodes_per_element; l++)
            {
              const GO colNode = node_rels[l];
              for (std::size_t m=0; m < neq; m++)
              {
                col = getGlobalDOF(colNode, m);
                m_overlap_jac_factory->insertGlobalIndices(row, Teuchos::arrayView(&col,1));
                m_overlap_jac_factory->insertGlobalIndices(row, Teuchos::arrayView(&col,1));
                //IKT, FIXME?  The following line might be needed 
                //m_overlap_jac_factory->insertGlobalIndices(col, Teuchos::arrayView(&row,1));
              }
            }
          }
        }
      }
    }
  }
}


void Aeras::SpectralDiscretization::fillCompleteGraphs()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif

  //fill complete m_implicit_overlap_jac_factory
  m_implicit_overlap_jac_factory->fillComplete();
  //For implicit scheme, m_overlap_jac_factory = m_implicit_overlap_jac_factory 
  if (explicit_scheme == false) {
    m_overlap_jac_factory->fillComplete();
  }

  //create m_implicit_jac_factory (owned) 
  m_implicit_jac_factory = Teuchos::rcp( new Albany::ThyraCrsMatrixFactory(m_vs, m_vs, m_implicit_overlap_jac_factory) );
  //For implicit scheme, m_jac_factory = m_implicit_jac_factory 
  if (explicit_scheme == false) {
    m_jac_factory = Teuchos::rcp( new Albany::ThyraCrsMatrixFactory(m_vs, m_vs, m_overlap_jac_factory) );
  }
}

void Aeras::SpectralDiscretization::fillCompleteGraphsExplicit()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif

  //fill complete m_overlap_jac_factory
  m_overlap_jac_factory->fillComplete();

  //create m_jac_factory (owned) 
  m_jac_factory = Teuchos::rcp( new Albany::ThyraCrsMatrixFactory(m_vs, m_vs, m_overlap_jac_factory) );
}


void Aeras::SpectralDiscretization::computeWorksetInfo()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif

  stk::mesh::Selector select_owned =
    stk::mesh::Selector(metaData.locally_owned_part());

  const stk::mesh::BucketVector & buckets =
    bulkData.get_buckets(stk::topology::ELEMENT_RANK, select_owned);

  const int numBuckets = buckets.size();

  typedef Albany::AbstractSTKFieldContainer::ScalarFieldType ScalarFieldType;
  typedef Albany::AbstractSTKFieldContainer::VectorFieldType VectorFieldType;
  typedef Albany::AbstractSTKFieldContainer::TensorFieldType TensorFieldType;
  typedef Albany::AbstractSTKFieldContainer::SphereVolumeFieldType SphereVolumeFieldType;

  VectorFieldType* coordinates_field = stkMeshStruct->getCoordinatesField();
  // IK, 1/22/15: changing type of sphereVolume_field to propagate
  // David Littlewood's change yesterday, so code will compile.  Need
  // to look into whether sphereVolume_field is needed for Aeras.
  // ScalarFieldType* sphereVolume_field;
  SphereVolumeFieldType* sphereVolume_field;

  if(stkMeshStruct->getFieldContainer()->hasSphereVolumeField())
    sphereVolume_field =
      stkMeshStruct->getFieldContainer()->getSphereVolumeField();

  wsEBNames.resize(numBuckets);
  for (int i = 0; i < numBuckets; ++i)
  {
    const stk::mesh::PartVector & bpv = buckets[i]->supersets();

    for (std::size_t j = 0; j < bpv.size(); ++j)
    {
      if (bpv[j]->primary_entity_rank() == stk::topology::ELEMENT_RANK &&
          !stk::mesh::is_auto_declared_part(*bpv[j]))
      {
        // *out << "Bucket " << i << " is in Element Block:  " << bpv[j]->name()
        //      << "  and has " << buckets[i]->size() << " elements." << std::endl;
        wsEBNames[i] = bpv[j]->name();
      }
    }
  }

  wsPhysIndex.resize(numBuckets);
  if (stkMeshStruct->allElementBlocksHaveSamePhysics)
    for (int i = 0; i < numBuckets; ++i)
      wsPhysIndex[i] = 0;
  else
    for (int i = 0; i < numBuckets; ++i)
      wsPhysIndex[i] = stkMeshStruct->getMeshSpecs()[0]->ebNameToIndex[wsEBNames[i]];

  // Fill  wsElNodeEqID(workset, el_LID, local node, Eq) => unk_LID

  wsElNodeEqID.resize(numBuckets);
  //wsElNodeID.resize(numBuckets);
  //coords.resize(numBuckets);
  sphereVolume.resize(numBuckets);

  nodesOnElemStateVec.resize(numBuckets);
  stateArrays.elemStateArrays.resize(numBuckets);
  const Albany::StateInfoStruct& nodal_states =
    stkMeshStruct->getFieldContainer()->getNodalSIS();

  // Clear map if remeshing
  if(!elemGIDws.empty()) elemGIDws.clear();

  typedef stk::mesh::Cartesian NodeTag;
  typedef stk::mesh::Cartesian ElemTag;
  typedef stk::mesh::Cartesian CompTag;

  for (int b = 0; b < numBuckets; ++b)
  {

    stk::mesh::Bucket & buck = *buckets[b];
    //wsElNodeID[b].resize(buck.size());
    //coords[b].resize(buck.size());

    // Set size of Kokkos views
    wsElNodeEqID[b] = Albany::WorksetConn("wsElNodeEqID", buck.size(), nodes_per_element, neq);

    {  // nodalDataToElemNode.

      nodesOnElemStateVec[b].resize(nodal_states.size());

      for (int is = 0; is < nodal_states.size(); ++is)
      {
        const std::string & name = nodal_states[is]->name;
        const Albany::StateStruct::FieldDims & dim = nodal_states[is]->dim;
        Albany::MDArray & array = stateArrays.elemStateArrays[b][name];
        std::vector<double> & stateVec = nodesOnElemStateVec[b][is];
        int dim0 = buck.size(); // may be different from dim[0];
        switch (dim.size())
        {
        case 2:     // scalar
        {
          const ScalarFieldType& field = *metaData.get_field<ScalarFieldType>(stk::topology::NODE_RANK, name);
          stateVec.resize(dim0*dim[1]);
          array.assign<ElemTag, NodeTag>(stateVec.data(),dim0,dim[1]);
          for (int i = 0; i < dim0; ++i)
          {
            stk::mesh::Entity element = buck[i];
            stk::mesh::Entity const* rel = bulkData.begin_nodes(element);
            for (int j=0; j < dim[1]; j++)
            {
              stk::mesh::Entity rowNode = rel[j];
              array(i,j) = *stk::mesh::field_data(field, rowNode);
            }
          }
          break;
        }
        case 3:  // vector
        {
          const VectorFieldType& field =
            *metaData.get_field<VectorFieldType>(stk::topology::NODE_RANK,name);
          stateVec.resize(dim0*dim[1]*dim[2]);
          array.assign< ElemTag, NodeTag, CompTag >(stateVec.data(),
                                                    dim0,
                                                    dim[1],
                                                    dim[2]);
          for (int i=0; i < dim0; i++)
          {
            stk::mesh::Entity element = buck[i];
            stk::mesh::Entity const* rel = bulkData.begin_nodes(element);
            for (int j=0; j < dim[1]; j++)
            {
              stk::mesh::Entity rowNode = rel[j];
              double* entry = stk::mesh::field_data(field, rowNode);
              for(int k=0; k<dim[2]; k++)
                array(i,j,k) = entry[k];
            }
          }
          break;
        }
        case 4: // tensor
        {
          const TensorFieldType& field = *metaData.get_field<TensorFieldType>(stk::topology::NODE_RANK, name);
          stateVec.resize(dim0*dim[1]*dim[2]*dim[3]);
          array.assign<ElemTag, NodeTag, CompTag, CompTag>(stateVec.data(),dim0,dim[1],dim[2],dim[3]);
          for (int i=0; i < dim0; i++)
          {
            stk::mesh::Entity element = buck[i];
            stk::mesh::Entity const* rel = bulkData.begin_nodes(element);
            for (int j=0; j < dim[1]; j++)
            {
              stk::mesh::Entity rowNode = rel[j];
              double* entry = stk::mesh::field_data(field, rowNode);
              for(int k=0; k<dim[2]; k++)
                for(int l=0; l<dim[3]; l++)
                  // Check this: is stride correct?
                  array(i,j,k,l) = entry[k*dim[3]+l];
            }
          }
          break;
        }
        }
      }
    }

    // i is the element index within bucket b
    for (std::size_t i = 0; i < buck.size(); ++i)
    {

      // Traverse all the elements in this bucket
      stk::mesh::Entity element = buck[i];

      // Now, save a map from element GID to workset on this PE
      elemGIDws[gid(element)].ws = b;

      // Now, save a map from element GID to local id on this workset on this PE
      elemGIDws[gid(element)].LID = i;

      // const stk::mesh::Entity * node_rels = bulkData.begin_nodes(element);
      Teuchos::ArrayRCP< GO > node_rels = wsElNodeID[b][i];
      // const int nodes_per_element = bulkData.num_nodes(element);

      //wsElNodeID[b][i].resize(nodes_per_element);
      //coords[b][i].resize(nodes_per_element);

      // loop over local nodes
      for (int j = 0; j < nodes_per_element; ++j)
      {
        // const stk::mesh::Entity rowNode = node_rels[j];
        // const GO node_gid = gid(rowNode);
        const GO node_gid = node_rels[j];
        const LO node_lid = Albany::getLocalElement(m_overlap_node_vs,node_gid);

        TEUCHOS_TEST_FOR_EXCEPTION(
          node_lid < 0,
          std::logic_error,
    "STK1D_Disc: node_lid out of range " << node_lid << std::endl);
        //coords[b][i][j] = stk::mesh::field_data(*coordinates_field, rowNode);

        //wsElNodeID[b][i][j] = node_gid;

        for (std::size_t eq = 0; eq < neq; ++eq)
          wsElNodeEqID[b](i,j,eq) = getOverlapDOF(node_lid,eq);
      }
    }
  }

  //The following is for periodic BCs.  This will only be relevant for the x-z hydrostatic equations.
  for (int d=0; d<stkMeshStruct->numDim; d++)
  {
  if (stkMeshStruct->PBCStruct.periodic[d])
  {
    for (int b=0; b < numBuckets; b++)
    {
      for (std::size_t i=0; i < buckets[b]->size(); i++)
      {
        bool anyXeqZero=false;
        for (int j=0; j < nodes_per_element; j++) {
          if (coords[b][i][j][d]==0.0)
            anyXeqZero=true;
        }
        if (anyXeqZero)
        {
          bool flipZeroToScale=false;
          for (int j=0; j < nodes_per_element; j++)
            if (coords[b][i][j][d] > stkMeshStruct->PBCStruct.scale[d]/1.9)
              flipZeroToScale=true;
          if (flipZeroToScale)
          {
            for (int j=0; j < nodes_per_element; j++)
            {
              if (coords[b][i][j][d] == 0.0)
              {
                double* xleak = new double [stkMeshStruct->numDim];
                for (int k=0; k < stkMeshStruct->numDim; k++)
                  if (k==d)
                    xleak[d]=stkMeshStruct->PBCStruct.scale[d];
                  else
                    xleak[k] = coords[b][i][j][k];
                coords[b][i][j] = xleak; // replace ptr to coords
                toDelete.push_back(xleak);
              }
            }
          }
        }
      }
    }
  }
  }
  typedef Albany::AbstractSTKFieldContainer::ScalarValueState ScalarValueState;
  typedef Albany::AbstractSTKFieldContainer::QPScalarState    QPScalarState;
  typedef Albany::AbstractSTKFieldContainer::QPVectorState    QPVectorState;
  typedef Albany::AbstractSTKFieldContainer::QPTensorState    QPTensorState;
  typedef Albany::AbstractSTKFieldContainer::ScalarState      ScalarState;
  typedef Albany::AbstractSTKFieldContainer::VectorState      VectorState;
  typedef Albany::AbstractSTKFieldContainer::TensorState      TensorState;

  // Pull out pointers to shards::Arrays for every bucket, for every state
  // Code is data-type dependent

  ScalarValueState scalarValue_states = stkMeshStruct->getFieldContainer()->getScalarValueStates();
  QPScalarState qpscalar_states = stkMeshStruct->getFieldContainer()->getQPScalarStates();
  QPVectorState qpvector_states = stkMeshStruct->getFieldContainer()->getQPVectorStates();
  QPTensorState qptensor_states = stkMeshStruct->getFieldContainer()->getQPTensorStates();
  std::map<std::string, double>& time = stkMeshStruct->getFieldContainer()->getTime();

  for (std::size_t b = 0; b < buckets.size(); ++b)
  {
    stk::mesh::Bucket & buck = *buckets[b];
    for (QPScalarState::iterator qpss = qpscalar_states.begin();
              qpss != qpscalar_states.end(); ++qpss)
    {
      Albany::BucketArray<Albany::AbstractSTKFieldContainer::QPScalarFieldType> array(**qpss, buck);
      // Debug
      // std::cout << "Buck.size(): " << buck.size() << " QPSFT dim[1]: "
      //           << array.extent(1) << std::endl;
      Albany::MDArray ar = array;
      stateArrays.elemStateArrays[b][(*qpss)->name()] = ar;
    }
    for (QPVectorState::iterator qpvs = qpvector_states.begin();
              qpvs != qpvector_states.end(); ++qpvs)
    {
      Albany::BucketArray<Albany::AbstractSTKFieldContainer::QPVectorFieldType>
        array(**qpvs, buck);
      // Debug
      // std::cout << "Buck.size(): " << buck.size() << " QPVFT dim[2]: "
      //           << array.extent(2) << std::endl;
      Albany::MDArray ar = array;
      stateArrays.elemStateArrays[b][(*qpvs)->name()] = ar;
    }
    for (QPTensorState::iterator qpts = qptensor_states.begin();
              qpts != qptensor_states.end(); ++qpts)
    {
      Albany::BucketArray<Albany::AbstractSTKFieldContainer::QPTensorFieldType> array(**qpts, buck);
      // Debug
      // std::cout << "Buck.size(): " << buck.size() << " QPTFT dim[3]: "
      //           << array.extent(3) << std::endl;
      Albany::MDArray ar = array;
      stateArrays.elemStateArrays[b][(*qpts)->name()] = ar;
    }
    for (ScalarValueState::iterator svs = scalarValue_states.begin();
         svs != scalarValue_states.end(); ++svs)
    {
      const int size = 1;
      shards::Array<double, shards::NaturalOrder, Cell> array(&time[**svs], size);
      // Debug
      // std::cout << "Buck.size(): " << buck.size() << " SVState dim[0]: "
      //           << array.extent(0) << std::endl;
      // std::cout << "SV Name: " << **svs << " address : " << &array << std::endl;
      Albany::MDArray ar = array;
      stateArrays.elemStateArrays[b][**svs] = ar;
    }
  }

// Process node data sets if present

  if (Teuchos::nonnull(stkMeshStruct->nodal_data_base) &&
      stkMeshStruct->nodal_data_base->isNodeDataPresent())
  {
    Teuchos::RCP<Albany::NodeFieldContainer> node_states =
      stkMeshStruct->nodal_data_base->getNodeContainer();

    std::cout << "g" << std::endl;
    stk::mesh::BucketVector const& node_buckets =
      bulkData.get_buckets( stk::topology::NODE_RANK, select_owned );

    const size_t numNodeBuckets = node_buckets.size();

    stateArrays.nodeStateArrays.resize(numNodeBuckets);
    for (std::size_t b=0; b < numNodeBuckets; b++)
    {
      stk::mesh::Bucket& buck = *node_buckets[b];
      for (Albany::NodeFieldContainer::iterator nfs = node_states->begin();
                nfs != node_states->end(); ++nfs)
      {
        stateArrays.nodeStateArrays[b][(*nfs).first] =
             Teuchos::rcp_dynamic_cast<Albany::AbstractSTKNodeFieldContainer>((*nfs).second)->getMDA(buck);
      }
    }
  }
}

void Aeras::SpectralDiscretization::computeSideSetsLines()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  // Clean up existing sideset structure if remeshing
  for(int i = 0; i < sideSets.size(); i++)
    sideSets[i].clear(); // empty the ith map

  const stk::mesh::EntityRank element_rank = stk::topology::ELEMENT_RANK;

  // iterator over all side_rank parts found in the mesh
  std::map<std::string, stk::mesh::Part*>::iterator ss = stkMeshStruct->ssPartVec.begin();

  int numBuckets = wsEBNames.size();

  sideSets.resize(numBuckets); // Need a sideset list per workset

  while ( ss != stkMeshStruct->ssPartVec.end() )
  {
    // Get all owned sides in this side set
    stk::mesh::Selector select_owned_in_sspart =

      // get only entities in the ss part (ss->second is the current sideset part)
      stk::mesh::Selector( *(ss->second) ) &
      // and only if the part is local
      stk::mesh::Selector( metaData.locally_owned_part() );

    std::vector< stk::mesh::Entity > sides ;
    stk::mesh::get_selected_entities( select_owned_in_sspart , // sides local to this processor
              bulkData.buckets( metaData.side_rank() ) ,
              sides ); // store the result in "sides"

    *out << "STKDisc: sideset "<< ss->first <<" has size " << sides.size() << "  on Proc 0." << std::endl;

    // loop over the sides to see what they are, then fill in the data holder
    // for side set options, look at $TRILINOS_DIR/packages/stk/stk_usecases/mesh/UseCase_13.cpp

    for (std::size_t localSideID=0; localSideID < sides.size(); localSideID++)
    {

      stk::mesh::Entity sidee = sides[localSideID];

      TEUCHOS_TEST_FOR_EXCEPTION(bulkData.num_elements(sidee) != 1, std::logic_error,
                                 "STKDisc: cannot figure out side set topology for side set " << ss->first << std::endl);

      stk::mesh::Entity elem = bulkData.begin_elements(sidee)[0];

      // containing the side. Note that if the side is internal, it will show up twice in the
      // element list, once for each element that contains it.

      Albany::SideStruct sStruct;

      // Save elem id. This is the global element id
      sStruct.elem_GID = gid(elem);

      int workset = elemGIDws[sStruct.elem_GID].ws; // Get the ws that this element lives in

      // Save elem id. This is the local element id within the workset
      sStruct.elem_LID = elemGIDws[sStruct.elem_GID].LID;

      // Save the side identifier inside of the element. This starts at zero here.
      sStruct.side_local_id = determine_local_side_id(elem, sidee);

      // Save the index of the element block that this elem lives in
      sStruct.elem_ebIndex = stkMeshStruct->getMeshSpecs()[0]->ebNameToIndex[wsEBNames[workset]];

      Albany::SideSetList& ssList = sideSets[workset];   // Get a ref to the side set map for this ws
      Albany::SideSetList::iterator it = ssList.find(ss->first); // Get an iterator to the correct sideset (if
                                                                // it exists)

      if(it != ssList.end()) // The sideset has already been created

        it->second.push_back(sStruct); // Save this side to the vector that belongs to the name ss->first

      else { // Add the key ss->first to the map, and the side vector to that map

        std::vector<Albany::SideStruct> tmpSSVec;
        tmpSSVec.push_back(sStruct);

        ssList.insert(Albany::SideSetList::value_type(ss->first, tmpSSVec));

      }

    }

    ss++;
  }
}

unsigned
Aeras::SpectralDiscretization::determine_local_side_id(
    const stk::mesh::Entity elem,
    stk::mesh::Entity side)
{
  using namespace stk;

  stk::topology elem_top = bulkData.bucket(elem).topology();

  const unsigned num_elem_nodes = bulkData.num_nodes(elem);
  const unsigned num_side_nodes = bulkData.num_nodes(side);

  stk::mesh::Entity const* elem_nodes = bulkData.begin_nodes(elem);
  stk::mesh::Entity const* side_nodes = bulkData.begin_nodes(side);

  const stk::topology::rank_t side_rank = metaData.side_rank();

  int side_id = -1 ;

  if(num_elem_nodes == 0 || num_side_nodes == 0)
  {
    // Node relations are not present, look at elem->face
    const unsigned num_sides = bulkData.num_connectivity(elem, side_rank);
    stk::mesh::Entity const* elem_sides = bulkData.begin(elem, side_rank);

    for ( unsigned i = 0 ; i < num_sides ; ++i )
    {
      const stk::mesh::Entity elem_side = elem_sides[i];

      if (bulkData.identifier(elem_side) == bulkData.identifier(side))
      {
        // Found the local side in the element
        side_id = static_cast<int>(i);
        return side_id;
      }
    }

    if ( side_id < 0 )
    {
      std::ostringstream msg;
      msg << "determine_local_side_id( " ;
      msg << elem_top.name() ;
      msg << " , Element[ " ;
      msg << bulkData.identifier(elem);
      msg << " ]{" ;
      for ( unsigned i = 0 ; i < num_sides ; ++i )
      {
        msg << " " << bulkData.identifier(elem_sides[i]);
      }
      msg << " } , Side[ " ;
      msg << bulkData.identifier(side);
      msg << " ] ) FAILED" ;
      throw std::runtime_error( msg.str() );
    }

  }
  else
  {
    // Conventional elem->node - side->node connectivity present
    std::vector<unsigned> side_map;
    for ( unsigned i = 0 ; side_id == -1 && i < elem_top.num_sides() ; ++i )
    {
      stk::topology side_top    = elem_top.side_topology(i);
      side_map.clear();
      elem_top.side_node_ordinals(i, std::back_inserter(side_map));

      if ( num_side_nodes == side_top.num_nodes() )
      {

        side_id = i ;

        for ( unsigned j = 0 ;
              side_id == static_cast<int>(i) && j < side_top.num_nodes() ; ++j )
        {

          stk::mesh::Entity elem_node = elem_nodes[ side_map[j] ];

          bool found = false ;

          for ( unsigned k = 0 ; ! found && k < side_top.num_nodes() ; ++k )
          {
            found = elem_node == side_nodes[k];
          }

          if ( ! found ) { side_id = -1 ; }
        }
      }
    }

    if ( side_id < 0 )
    {
      std::ostringstream msg ;
      msg << "determine_local_side_id( " ;
      msg << elem_top.name() ;
      msg << " , Element[ " ;
      msg << bulkData.identifier(elem);
      msg << " ]{" ;
      for ( unsigned i = 0 ; i < num_elem_nodes ; ++i )
      {
        msg << " " << bulkData.identifier(elem_nodes[i]);
      }
      msg << " } , Side[ " ;
      msg << bulkData.identifier(side);
      msg << " ]{" ;
      for ( unsigned i = 0 ; i < num_side_nodes ; ++i )
      {
        msg << " " << bulkData.identifier(side_nodes[i]);
      }
      msg << " } ) FAILED" ;
      throw std::runtime_error( msg.str() );
    }
  }

  return static_cast<unsigned>(side_id) ;
}

void Aeras::SpectralDiscretization::computeNodeSetsLines()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  std::map<std::string, stk::mesh::Part*>::iterator ns = stkMeshStruct->nsPartVec.begin();
  Albany::AbstractSTKFieldContainer::VectorFieldType* coordinates_field = stkMeshStruct->getCoordinatesField();

  while ( ns != stkMeshStruct->nsPartVec.end() ) { // Iterate over Node Sets
    // Get all owned nodes in this node set
    stk::mesh::Selector select_owned_in_nspart =
      stk::mesh::Selector( *(ns->second) ) &
      stk::mesh::Selector( metaData.locally_owned_part() );

    std::vector< stk::mesh::Entity > nodes ;
    stk::mesh::get_selected_entities( select_owned_in_nspart ,
              bulkData.buckets( stk::topology::NODE_RANK ) ,
              nodes );

    nodeSets[ns->first].resize(nodes.size());
    nodeSetCoords[ns->first].resize(nodes.size());
//    nodeSetIDs.push_back(ns->first); // Grab string ID
    *out << "STKDisc: nodeset "<< ns->first <<" has size " << nodes.size() << "  on Proc 0." << std::endl;
    for (std::size_t i=0; i < nodes.size(); i++)
    {
      GO node_gid = gid(nodes[i]);
      auto node_lid = Albany::getLocalElement(m_node_vs,node_gid);
      nodeSets[ns->first][i].resize(neq);
      for (std::size_t eq=0; eq < neq; eq++)  nodeSets[ns->first][i][eq] = getOwnedDOF(node_lid,eq);
      nodeSetCoords[ns->first][i] = stk::mesh::field_data(*coordinates_field, nodes[i]);
    }
    ns++;
  }
}

void Aeras::SpectralDiscretization::createOutputMesh()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
#ifdef ALBANY_SEACAS
  //construct new mesh struct for output
  //IKT, 9/22/15: this needs to be called all the time even when no exodus output is requested b/c outputStkMeshStruct is
  //called in Aeras::SpectralDiscretization::setOvlpSolutionFieldT, which is always called.
  outputStkMeshStruct =
    Teuchos::rcp(new Aeras::SpectralOutputSTKMeshStruct(
        discParams,
        commT,
        stkMeshStruct->numDim,
        stkMeshStruct->getMeshSpecs()[0]->worksetSize,
        stkMeshStruct->PBCStruct.periodic[0],
        stkMeshStruct->PBCStruct.scale[0],
        wsElNodeID,
        coords,
        points_per_edge, element_name));
  Teuchos::RCP<Albany::StateInfoStruct> sis =
    Teuchos::rcp(new Albany::StateInfoStruct);
  Albany::AbstractFieldContainer::FieldContainerRequirements req;
  //set field and bulk data for new struct (for output)
  outputStkMeshStruct->setFieldAndBulkData(
      commT,
      discParams,
      neq,
      req,
      sis,
      stkMeshStruct->getMeshSpecs()[0]->worksetSize);
#endif
}

void Aeras::SpectralDiscretization::setupExodusOutput()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
#ifdef ALBANY_SEACAS
  if (stkMeshStruct->exoOutput)
  {
    outputInterval = 0;
    std::string str = stkMeshStruct->exoOutFile;
    Ioss::Init::Initializer io;
    mesh_data =
      Teuchos::rcp(new stk::io::StkMeshIoBroker(
          Albany::getMpiCommFromTeuchosComm(commT)));
    mesh_data->set_bulk_data(*outputStkMeshStruct->bulkData);
    //IKT, 5/7/15:
    //Uncomment the following out if you want to see the un-enriched mesh
    // written out
    //mesh_data->set_bulk_data(bulkData);
    outputFileIdx = mesh_data->create_output_mesh(str, stk::io::WRITE_RESULTS);

    const stk::mesh::FieldVector &fields = mesh_data->meta_data().get_fields();
    for (size_t i=0; i < fields.size(); i++)
    {
      // Hacky, but doesn't appear to be a way to query if a field is already
      // going to be output.
      try
      {
        mesh_data->add_field(outputFileIdx, *fields[i]);
      }
      catch (std::runtime_error const&) { }
    }
  }
#else
  if (stkMeshStruct->exoOutput)
    *out << "\nWARNING: exodus output requested but SEACAS not compiled in:"
         << " disabling exodus output \n" << std::endl;

#endif
}

namespace
{
const std::vector<double>
spherical_to_cart(const std::pair<double, double> & sphere)
{
  const double radius_of_earth = 1;
  std::vector<double> cart(3);

  cart[0] = radius_of_earth*std::cos(sphere.first)*std::cos(sphere.second);
  cart[1] = radius_of_earth*std::cos(sphere.first)*std::sin(sphere.second);
  cart[2] = radius_of_earth*std::sin(sphere.first);

  return cart;
}

double distance (const double* x, const double* y)
{
  const double d = std::sqrt((x[0]-y[0])*(x[0]-y[0]) +
                             (x[1]-y[1])*(x[1]-y[1]) +
                             (x[2]-y[2])*(x[2]-y[2]));
  return d;
}

double distance (const std::vector<double> &x, const std::vector<double> &y)
{
  const double d = std::sqrt((x[0]-y[0])*(x[0]-y[0]) +
                             (x[1]-y[1])*(x[1]-y[1]) +
                             (x[2]-y[2])*(x[2]-y[2]));
  return d;
}

bool point_inside(const Teuchos::ArrayRCP<double*> &coords,
                  const std::vector<double>        &sphere_xyz)
{
  // first check if point is near the element:
  const double  tol_inside = 1e-12;
  const double elem_diam =
    std::max(::distance(coords[0],coords[2]), ::distance(coords[1],coords[3]));
  std::vector<double> center(3,0);
  for (unsigned i=0; i<4; ++i)
    for (unsigned j=0; j<3; ++j)
      center[j] += coords[i][j];
  for (unsigned j=0; j<3; ++j)
    center[j] /= 4;
  bool inside = true;

  if ( ::distance(&center[0],&sphere_xyz[0]) > 1.0*elem_diam )
    inside = false;

  unsigned j=3;
  for (unsigned i=0; i<4 && inside; ++i)
  {
    std::vector<double> cross(3);
    // outward normal to plane containing j->i edge:  corner(i) x corner(j)
    // sphere dot (corner(i) x corner(j) ) = negative if inside
    cross[0]=  coords[i][1]*coords[j][2] - coords[i][2]*coords[j][1];
    cross[1]=-(coords[i][0]*coords[j][2] - coords[i][2]*coords[j][0]);
    cross[2]=  coords[i][0]*coords[j][1] - coords[i][1]*coords[j][0];
    j = i;
    const double dotprod = cross[0]*sphere_xyz[0] +
                           cross[1]*sphere_xyz[1] +
                           cross[2]*sphere_xyz[2];

      // dot product is proportional to elem_diam. positive means outside,
      // but allow machine precision tolorence:
      if (tol_inside*elem_diam < dotprod) inside = false;
    }
    return inside;
  }


  const Teuchos::RCP<Intrepid2::Basis<PHX::Device, RealType, RealType> >
  Basis(const int C)
  {
    // Static types
    typedef Kokkos::DynRankView<RealType, PHX::Device> Field_t;
    typedef Intrepid2::Basis<PHX::Device, RealType, RealType> Basis_t;
    static const Teuchos::RCP< Basis_t > HGRAD_Basis_4 =
      Teuchos::rcp( new Intrepid2::Basis_HGRAD_QUAD_C1_FEM<PHX::Device>() );
    static const Teuchos::RCP< Basis_t > HGRAD_Basis_9 =
      Teuchos::rcp( new Intrepid2::Basis_HGRAD_QUAD_C2_FEM<PHX::Device>() );

    // Check for valid value of C
    int deg = (int) std::sqrt((double)C);
    TEUCHOS_TEST_FOR_EXCEPTION(
      deg*deg != C || deg < 2,
      std::logic_error,
      " Aeras::SpectralDiscretization Error Basis not perfect "
      "square > 1" << std::endl);

    // Quick return for linear or quad
    if (C == 4) return HGRAD_Basis_4;
    if (C == 9) return HGRAD_Basis_9;

    // Spectral bases
std::cout << "AGS -- changing POINTTYPE_SPECTRAL to POINTTYPE_WARPBLEND -- check with Kyungjoo" << std::endl;
    return Teuchos::rcp(
      new Intrepid2::Basis_HGRAD_QUAD_Cn_FEM<PHX::Device>(
        deg, Intrepid2::POINTTYPE_WARPBLEND) );
//        deg, Intrepid2::POINTTYPE_SPECTRAL) );
  }

  double value(const std::vector<double> &soln,
               const std::pair<double, double> &ref)
  {

    const int C = soln.size();
    const Teuchos::RCP<Intrepid2::Basis<PHX::Device, RealType, RealType> >
      HGRAD_Basis = Basis(C);

    const int numPoints = 1;
    Kokkos::DynRankView<RealType, PHX::Device> basisVals ("AAA", C, numPoints);
    Kokkos::DynRankView<RealType, PHX::Device> tempPoints("AAA", numPoints, 2);
    tempPoints(0,0) = ref.first;
    tempPoints(0,1) = ref.second;

    HGRAD_Basis->getValues(basisVals, tempPoints, Intrepid2::OPERATOR_VALUE);

    double x = 0;
    for (unsigned j=0; j<C; ++j) x += soln[j] * basisVals(j,0);
    return x;
  }

  void value(double x[3],
             const Teuchos::ArrayRCP<double*> &coords,
             const std::pair<double, double> &ref)
  {

    const int C = coords.size();
    const Teuchos::RCP<Intrepid2::Basis<PHX::Device, RealType, RealType> >
      HGRAD_Basis = Basis(C);

    const int numPoints = 1;
    Kokkos::DynRankView<RealType, PHX::Device> basisVals ("AAA", C, numPoints);
    Kokkos::DynRankView<RealType, PHX::Device> tempPoints("AAA", numPoints, 2);
    tempPoints(0,0) = ref.first;
    tempPoints(0,1) = ref.second;

    HGRAD_Basis->getValues(basisVals, tempPoints, Intrepid2::OPERATOR_VALUE);

    for (unsigned i = 0; i < 3; ++i)
      x[i] = 0;
    for (unsigned i = 0; i < 3; ++i)
      for (unsigned j = 0; j < C; ++j)
        x[i] += coords[j][i] * basisVals(j,0);
  }

  void grad(double x[3][2],
            const Teuchos::ArrayRCP<double*> &coords,
            const std::pair<double, double> &ref)
  {
    const int C = coords.size();
    const Teuchos::RCP<Intrepid2::Basis<PHX::Device, RealType, RealType> >
      HGRAD_Basis = Basis(C);

    const int numPoints = 1;
    Kokkos::DynRankView<RealType, PHX::Device> basisGrad ("AAA", C, numPoints, 2);
    Kokkos::DynRankView<RealType, PHX::Device> tempPoints("AAA", numPoints, 2);
    tempPoints(0,0) = ref.first;
    tempPoints(0,1) = ref.second;

    HGRAD_Basis->getValues(basisGrad, tempPoints, Intrepid2::OPERATOR_GRAD);

    for (unsigned i = 0; i < 3; ++i)
      x[i][0] = x[i][1] = 0;
    for (unsigned i = 0; i < 3; ++i)
      for (unsigned j = 0; j < C; ++j)
      {
        x[i][0] += coords[j][i] * basisGrad(j,0,0);
        x[i][1] += coords[j][i] * basisGrad(j,0,1);
      }
  }

  std::pair<double, double>  ref2sphere(const Teuchos::ArrayRCP<double*> &coords,
                                        const std::pair<double, double> &ref)
  {

    static const double DIST_THRESHOLD= 1.0e-9;

    double x[3];
    value(x,coords,ref);

    const double r = std::sqrt(x[0]*x[0] + x[1]*x[1] + x[2]*x[2]);

    for (unsigned i=0; i<3; ++i) x[i] /= r;

    std::pair<double, double> sphere(std::asin(x[2]), std::atan2(x[1],x[0]));

    // ==========================================================
    // enforce three facts:
    //
    // 1) lon at poles is defined to be zero
    //
    // 2) Grid points must be separated by about .01 Meter (on earth)
    //   from pole to be considered "not the pole".
    //
    // 3) range of lon is { 0<= lon < 2*PI }
    //
    // ==========================================================

    if (std::abs(std::abs(sphere.first)-pi/2) < DIST_THRESHOLD) sphere.second = 0;
    else if (sphere.second < 0) sphere.second += 2*pi;

    return sphere;
  }

  void Dmap(const Teuchos::ArrayRCP<double*> &coords,
            const std::pair<double, double>  &sphere,
            const std::pair<double, double>  &ref,
            double D[][2])
  {

    const double th     = sphere.first;
    const double lam    = sphere.second;
    const double sinlam = std::sin(lam);
    const double sinth  = std::sin(th);
    const double coslam = std::cos(lam);
    const double costh  = std::cos(th);

    const double D1[2][3] = {{-sinlam, coslam, 0},
                             {      0,      0, 1}};

    const double D2[3][3] =
      {{ sinlam*sinlam*costh*costh+sinth*sinth, -sinlam*coslam*costh*costh,             -coslam*sinth*costh},
       {-sinlam*coslam*costh*costh,              coslam*coslam*costh*costh+sinth*sinth, -sinlam*sinth*costh},
       {-coslam*sinth,                          -sinlam*sinth,                          costh              }};

    double D3[3][2] = {0};
    grad(D3,coords,ref);

    double D4[3][2] = {0};
    for (unsigned i = 0; i < 3; ++i)
      for (unsigned j = 0; j < 2; ++j)
        for (unsigned k = 0; k < 3; ++k)
           D4[i][j] += D2[i][k] * D3[k][j];

    for (unsigned i=0; i<2; ++i)
      for (unsigned j=0; j<2; ++j)
        D[i][j] = 0;

    for (unsigned i=0; i<2; ++i)
      for (unsigned j=0; j<2; ++j)
        for (unsigned k=0; k<3; ++k)
          D[i][j] += D1[i][k] * D4[k][j];
  }

  std::pair<double, double>
  parametric_coordinates(const Teuchos::ArrayRCP<double*> &coords,
                         const std::pair<double, double>  &sphere)
  {
    static const double tol_sq = 1e-26;
    static const unsigned MAX_NR_ITER = 10;
    double costh = std::cos(sphere.first);
    double D[2][2], Dinv[2][2];
    double resa = 1;
    double resb = 1;
    std::pair<double, double> ref(0,0); // initial guess is center of element.

    for (unsigned i = 0; i < MAX_NR_ITER && tol_sq < (costh*resb*resb+resa*resa);
         ++i)
    {
      const std::pair<double, double> sph = ref2sphere(coords,ref);
      resa = sph.first  - sphere.first;
      resb = sph.second - sphere.second;

      if (resb >  pi) resb -= 2*pi;
      if (resb < -pi) resb += 2*pi;

      Dmap(coords, sph, ref, D);
      const double detD = D[0][0]*D[1][1] - D[0][1]*D[1][0];
      Dinv[0][0] =  D[1][1]/detD;
      Dinv[0][1] = -D[0][1]/detD;
      Dinv[1][0] = -D[1][0]/detD;
      Dinv[1][1] =  D[0][0]/detD;

      const std::pair<double, double>
        del( Dinv[0][0]*costh*resb + Dinv[0][1]*resa,
             Dinv[1][0]*costh*resb + Dinv[1][1]*resa);
      ref.first  -= del.first;
      ref.second -= del.second;
    }
    return ref;
  }

  const std::pair<bool,std::pair<unsigned, unsigned> >
  point_in_element(
    const std::pair<double, double> &sphere,
    const Albany::WorksetArray<Teuchos::ArrayRCP<Teuchos::ArrayRCP<double*> > >::type& coords,
    std::pair<double, double> &parametric)
  {
    const std::vector<double> sphere_xyz = spherical_to_cart(sphere);
    std::pair<bool,std::pair<unsigned, unsigned> >
      element(false,
              std::pair<unsigned, unsigned>(0,0));
    for (unsigned i = 0; i < coords.size() && !element.first; ++i)
    {
      for (unsigned j = 0; j < coords[i].size() && !element.first; ++j)
      {
        const bool found =  point_inside(coords[i][j], sphere_xyz);
        if (found)
        {
          parametric = parametric_coordinates(coords[i][j], sphere);
          if (parametric.first  < -1) parametric.first  = -1;
          if (parametric.second < -1) parametric.second = -1;
          if (1 < parametric.first  ) parametric.first  =  1;
          if (1 < parametric.second ) parametric.second =  1;
          element.first         = true;
          element.second.first  = i;
          element.second.second = j;
        }
      }
    }
    return element;
  }

  void setup_latlon_interp(
    const unsigned nlat,
    const double nlon,
    const Albany::WorksetArray<Teuchos::ArrayRCP<Teuchos::ArrayRCP<double*> > >::type& coords,
    Albany::WorksetArray<Teuchos::ArrayRCP<std::vector<Aeras::SpectralDiscretization::interp> > >::type& interpdata,
    const Teuchos::RCP<const Teuchos_Comm> commT)
  {
    double err=0;
    const long long unsigned rank = commT->getRank();
    std::vector<double> lat(nlat);
    std::vector<double> lon(nlon);

    unsigned count = 0;
    for (unsigned i = 0; i < nlat; ++i)
      lat[i] = -pi/2 + i*pi/(nlat-1);
    for (unsigned j = 0; j < nlon; ++j)
      lon[j] = 2*j*pi/nlon;
    for (unsigned i = 0; i < nlat; ++i)
    {
      for (unsigned j=0; j<nlon; ++j)
      {
        const std::pair<double, double> sphere(lat[i],lon[j]);
        std::pair<double, double> paramtric;
        const std::pair<bool,std::pair<unsigned, unsigned> >element =
          point_in_element(sphere, coords, paramtric);
        if (element.first)
        {
          // compute error: map 'cart' back to sphere and compare with original
          // interpolation point:
          const unsigned b = element.second.first ;
          const unsigned e = element.second.second;
          const std::vector<double> sphere2_xyz =
            spherical_to_cart(ref2sphere(coords[b][e], paramtric));
          const std::vector<double> sphere_xyz  =
            spherical_to_cart(sphere);
          err = std::max(err, ::distance(&sphere2_xyz[0],&sphere_xyz[0]));
          Aeras::SpectralDiscretization::interp interp;
          interp.parametric_coords = paramtric;
          interp.latitude_longitude = std::pair<unsigned,unsigned>(i,j);
          interpdata[b][e].push_back(interp);
          ++count;
        }
      }
      if (!rank && (!(i%64) || i==nlat-1))
        std::cout << "Finished Latitude " << i << " of " << nlat << std::endl;
    }
    if (!rank)
      std::cout<<"Max interpolation point search error: " <<err<<std::endl;
  }
}

int
Aeras::SpectralDiscretization::processNetCDFOutputRequestT(const Thyra_Vector& solution_field)
{
#ifdef ALBANY_SEACAS
  // IK, 10/13/14: need to implement!
#endif
  return 0;
}

void Aeras::SpectralDiscretization::setupNetCDFOutput()
{
  const long long unsigned rank = commT->getRank();
#ifdef ALBANY_SEACAS
  if (stkMeshStruct->cdfOutput)
  {
    outputInterval = 0;
    const unsigned nlat = stkMeshStruct->nLat;
    const unsigned nlon = stkMeshStruct->nLon;


    std::string str = stkMeshStruct->cdfOutFile;

    interpolateData.resize(coords.size());
    for (int b=0; b < coords.size(); b++) interpolateData[b].resize(coords[b].size());

    setup_latlon_interp(nlat, nlon, coords, interpolateData, commT);

    const std::string name = stkMeshStruct->cdfOutFile;
    netCDFp=0;
    netCDFOutputRequest=0;

#ifdef ALBANY_PAR_NETCDF
    MPI_Comm theMPIComm = Albany::getMpiCommFromTeuchosComm(commT);
    MPI_Info info;
    MPI_Info_create(&info);
    if (const int ierr = nc_create_par (name.c_str(), NC_NETCDF4 | NC_MPIIO | NC_CLOBBER | NC_64BIT_OFFSET, theMPIComm, info, &netCDFp))
      TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error,
        "nc_create_par returned error code "<<ierr<<" - "<<nc_strerror(ierr)<<std::endl);
    MPI_Info_free(&info);
#else
    if (!rank)
    if (const int ierr = nc_create (name.c_str(), NC_CLOBBER | NC_SHARE | NC_64BIT_OFFSET | NC_CLASSIC_MODEL, &netCDFp))
      TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error,
        "nc_create returned error code "<<ierr<<" - "<<nc_strerror(ierr)<<std::endl);
#endif

    const size_t nlev = 1;
    const char *dimnames[] = {"time","lev","lat","lon"};
    const size_t  dimlen[] = {NC_UNLIMITED, nlev, nlat, nlon};
    int dimID[4]={0,0,0,0};

    for (unsigned i=0; i<4; ++i)
    {
      if (netCDFp)
      if (const int ierr = nc_def_dim (netCDFp,  dimnames[i], dimlen[i], &dimID[i]))
        TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error,
          "nc_def_dim returned error code "<<ierr<<" - "<<nc_strerror(ierr)<<std::endl);
    }
    varSolns.resize(neq,0);

    for (unsigned n=0; n<neq; ++n)
    {
      std::ostringstream var;
      var <<"variable_"<<n;
      const char *field_name = var.str().c_str();
      if (netCDFp)
      if (const int ierr = nc_def_var (netCDFp,  field_name, NC_DOUBLE, 4, dimID, &varSolns[n]))
        TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error,
          "nc_def_var "<<field_name<<" returned error code "<<ierr<<" - "<<nc_strerror(ierr)<<std::endl);

      const double fillVal = -9999.0;
      if (netCDFp)
      if (const int ierr = nc_put_att (netCDFp,  varSolns[n], "FillValue", NC_DOUBLE, 1, &fillVal))
        TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error,
          "nc_put_att FillValue returned error code "<<ierr<<" - "<<nc_strerror(ierr)<<std::endl);
    }

    const char lat_name[] = "latitude";
    const char lat_unit[] = "degrees_north";
    const char lon_name[] = "longitude";
    const char lon_unit[] = "degrees_east";
    int latVarID=0;
      if (netCDFp)
    if (const int ierr = nc_def_var (netCDFp,  "lat", NC_DOUBLE, 1, &dimID[2], &latVarID))
      TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error,
        "nc_def_var lat returned error code "<<ierr<<" - "<<nc_strerror(ierr)<<std::endl);
      if (netCDFp)
    if (const int ierr = nc_put_att_text (netCDFp,  latVarID, "long_name", sizeof(lat_name), lat_name))
      TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error,
        "nc_put_att_text "<<lat_name<<" returned error code "<<ierr<<" - "<<nc_strerror(ierr)<<std::endl);
      if (netCDFp)
    if (const int ierr = nc_put_att_text (netCDFp,  latVarID, "units", sizeof(lat_unit), lat_unit))
      TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error,
        "nc_put_att_text "<<lat_unit<<" returned error code "<<ierr<<" - "<<nc_strerror(ierr)<<std::endl);

    int lonVarID=0;
      if (netCDFp)
    if (const int ierr = nc_def_var (netCDFp,  "lon", NC_DOUBLE, 1, &dimID[3], &lonVarID))
      TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error,
        "nc_def_var lon returned error code "<<ierr<<" - "<<nc_strerror(ierr)<<std::endl);
      if (netCDFp)
    if (const int ierr = nc_put_att_text (netCDFp,  lonVarID, "long_name", sizeof(lon_name), lon_name))
      TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error,
        "nc_put_att_text "<<lon_name<<" returned error code "<<ierr<<" - "<<nc_strerror(ierr)<<std::endl);
      if (netCDFp)
    if (const int ierr = nc_put_att_text (netCDFp,  lonVarID, "units", sizeof(lon_unit), lon_unit))
      TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error,
        "nc_put_att_text "<<lon_unit<<" returned error code "<<ierr<<" - "<<nc_strerror(ierr)<<std::endl);

    const char history[]="Created by Albany";
      if (netCDFp)
    if (const int ierr = nc_put_att_text (netCDFp,  NC_GLOBAL, "history", sizeof(history), history))
      TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error,
        "nc_put_att_text "<<history<<" returned error code "<<ierr<<" - "<<nc_strerror(ierr)<<std::endl);

      if (netCDFp)
    if (const int ierr = nc_enddef (netCDFp))
      TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error,
        "nc_enddef returned error code "<<ierr<<" - "<<nc_strerror(ierr)<<std::endl);

    std::vector<double> deglon(nlon);
    std::vector<double> deglat(nlat);
    for (unsigned i=0; i<nlon; ++i) deglon[i] =((      2*i*pi/nlon) *   (180/pi)) - 180;
    for (unsigned i=0; i<nlat; ++i) deglat[i] = (-pi/2 + i*pi/(nlat-1))*(180/pi);


      if (netCDFp)
    if (const int ierr = nc_put_var (netCDFp, lonVarID, &deglon[0]))
      TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error,
        "nc_put_var lon returned error code "<<ierr<<" - "<<nc_strerror(ierr)<<std::endl);
      if (netCDFp)
    if (const int ierr = nc_put_var (netCDFp, latVarID, &deglat[0]))
      TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error,
        "nc_put_var lat returned error code "<<ierr<<" - "<<nc_strerror(ierr)<<std::endl);

  }
#else
  if (stkMeshStruct->cdfOutput)
    *out << "\nWARNING: NetCDF output requested but SEACAS not compiled in:"
         << " disabling NetCDF output \n" << std::endl;
  stkMeshStruct->cdfOutput = false;

#endif
}

void Aeras::SpectralDiscretization::reNameExodusOutput(std::string& filename)
{
#ifdef ALBANY_SEACAS
  if (stkMeshStruct->exoOutput && !mesh_data.is_null())
  {
    // Delete the mesh data object and recreate it
    mesh_data = Teuchos::null;

    stkMeshStruct->exoOutFile = filename;

    // reset reference value for monotonic time function call as we are writing to a new file
    previous_time_label = -1.0e32;
  }
#else
  if (stkMeshStruct->exoOutput)
    *out << "\nWARNING: exodus output requested but SEACAS not compiled in:"
         << " disabling exodus output \n" << std::endl;

#endif
}



void
Aeras::SpectralDiscretization::updateMesh()
{
#ifdef OUTPUT_TO_SCREEN
  *out << "DEBUG: " << __PRETTY_FUNCTION__ << std::endl;
#endif
  if (spatial_dim == 1)
    enrichMeshLines();
  else if (spatial_dim == 2)
    enrichMeshQuads();

#ifdef OUTPUT_TO_SCREEN
  printConnectivity(true);
  commT->barrier();
  printConnectivity();
#endif

  if (spatial_dim == 1)
    computeOwnedNodesAndUnknownsLines();
  else if (spatial_dim == 2)
    computeOwnedNodesAndUnknownsQuads();

#ifdef WRITE_TO_MATRIX_MARKET_TO_MM_FILE
  //write owned vector spaces to matrix market file for debug
  Albany::writeMatrixMarket(m_vs, "m_vs.mm");
  Albany::writeMatrixMarket(m_node_vs, "m_node_vs.mm");
#endif

  // IK, 1/23/15: I've commented out the guts of this function.  It is
  // only needed for ML/MueLu and is not critical right now to get
  // spectral elements to work.
  setupMLCoords();

  if (spatial_dim == 1)
    computeOverlapNodesAndUnknownsLines();
  else if (spatial_dim == 2)
    computeOverlapNodesAndUnknownsQuads();

#ifdef WRITE_TO_MATRIX_MARKET_TO_MM_FILE
  //write overlap vector spaces to matrix market file for debug
  Albany::writeMatrixMarket(m_overlap_vs, "m_overlap_vs.mm");
  Albany::writeMatrixMarket(m_overlap_node_vs, "m_overlap_node_vs.mm");
#endif

    // Note that getCoordinates has not been converted to use the
    // enriched mesh, but I believe it's not used anywhere.
  if (spatial_dim == 1)
    computeCoordsLines();
  else if (spatial_dim == 2)
    computeCoordsQuads();

  computeWorksetInfo();

  // IKT, 2/16/15: moving computeGraphs() to after
  // computeWorksetInfoQuads(), as computeGraphs() relies on wsElNodeEqID
  // array which is set in computeWorksetInfoQuads()
  // IKT, 1/18/16: check if time-integration scheme is explicit or implicit
  // and call appropriate computeGraphs routine depending on what kind of scheme.
  // Right now, computeGraphs_Explicit() will not work with shallow water; therefore
  // only call this function for hydrostatic (numLevels > 0)

  //computeGraphs populates m_implicit_graph_factory and m_implicit_overlap_graph_factory
  //for an explicit scheme; for an implicit scheme, it also populates m_graph_factory and m_overlap_graph_factory.
  //For an explicit scheme, these the implicit graph factories are needed to populate correctly the Laplace 
  //operator, needed for hyperviscosity
  computeGraphs(); 
  //computeGraphs_Explicit will populate m_graph_factory and m_overlap_graph_factory
  //for an explicit scheme, which will have graphs of diagonal matrices. 
  computeGraphs_Explicit(); 

#ifdef WRITE_TO_MATRIX_MARKET_TO_MM_FILE
  Albany::writeMatrixMarket(m_implicit_jac_factory->createOp(), "ImplicitOp.mm"); 
  Albany::writeMatrixMarket(m_jac_factory->createOp(), "Op.mm"); 
  Albany::writeMatrixMarket(m_overlap_jac_factory->createOp(), "OverlapOp.mm"); 
#endif

  // IK, 1/23/15, FIXME: to implement -- transform mesh based on new
  // enriched coordinates This function is not critical and only
  // called for XZ hydrostatic equations.
  transformMesh();

  // IK, 1/27/15: debug output
#ifdef OUTPUT_TO_SCREEN
  printCoords();
#endif
#ifdef PRINT_COORDS
  printCoordsAndGIDs();
#endif

  // IK, 1/23/15: I have changed it so nothing happens in the
  // following functions b/c we have no Dirichlet/Neumann BCs for
  // spherical mesh.  Ultimately we probably want to remove these.
  if (spatial_dim == 1)
  {
    computeNodeSetsLines();
    computeSideSetsLines();
  }

   createOutputMesh();
   setupExodusOutput();

  //IKT, 9/22/15: the following routine needs to be implemented, if we care about netCDFoutput.
  //setupNetCDFOutput();
}
