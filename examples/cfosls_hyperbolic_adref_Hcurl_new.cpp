//                                MFEM(with 4D elements) CFOSLS for 3D/4D hyperbolic equation
//                                  with adaptive refinement involving a div-free formulation
//
// Compile with: make
//
// Description:  This example code solves a simple 3D/4D hyperbolic problem over [0,1]^3(4)
//               corresponding to the saddle point system
//                                  sigma_1 = u * b
//							 		sigma_2 - u        = 0
//                                  div_(x,t) sigma    = f
//                       with b = vector function (~velocity),
//						 NO boundary conditions (which work only in case when b * n = 0 pointwise at the domain space boundary)
//						 and initial condition:
//                                  u(x,0)            = 0
//               Here, we use a given exact solution
//                                  u(xt) = uFun_ex(xt)
//               and compute the corresponding r.h.s.
//               We discretize with Raviart-Thomas finite elements (sigma), continuous H1 elements (u) and
//					  discontinuous polynomials (mu) for the lagrange multiplier.
//

#include "mfem.hpp"
#include <fstream>
#include <iostream>
#include <memory>
#include <iomanip>
#include <list>

// if passive, the mesh is simply uniformly refined at each iteration
#define AMR

#define PARTSOL_SETUP

#define DIVFREE_MINSOLVER

//#define NEWINTERFACE
//#define MG_DIVFREEPREC

#define RECOARSENING_AMR

// activates using the solution at the previous mesh as a starting guess for the next problem
#define CLEVER_STARTING_GUESS

// activates using a (simpler & cheaper) preconditioner for the problems, simple Gauss-Seidel
//#define USE_GS_PREC

#define MULTILEVEL_PARTSOL

// activates using the particular solution at the previous mesh as a starting guess
// when finding the next particular solution (i.e., particular solution on the next mesh)
#define CLEVER_STARTING_PARTSOL

// is wrong, because the inflow for a rotation when the space domain is [-1,1]^2 is actually two corners
// and one has to split bdr attributes for the faces, which is quite a pain
#define CYLINDER_CUBE_TEST

// only the finest level consideration, 0 starting guess, solved by minimization solver
#define APPROACH_1

using namespace std;
using namespace mfem;
using std::unique_ptr;
using std::shared_ptr;
using std::make_shared;

void DefineEstimatorComponents(FOSLSProblem * problem, int fosls_func_version, std::vector<std::pair<int,int> >& grfuns_descriptor,
                          Array<ParGridFunction*>& extra_grfuns, Array2D<BilinearFormIntegrator *> & integs, bool verbose);


int main(int argc, char *argv[])
{
    int num_procs, myid;
    bool visualization = 1;

    // 1. Initialize MPI
    MPI_Init(&argc, &argv);
    MPI_Comm comm = MPI_COMM_WORLD;
    MPI_Comm_size(comm, &num_procs);
    MPI_Comm_rank(comm, &myid);

    bool verbose = (myid == 0);

    int nDimensions     = 3;
    int numsol          = -33;

#ifdef CYLINDER_CUBE_TEST
    numsol = 8;
#endif

    int ser_ref_levels  = 2;
    int par_ref_levels  = 0;

    const char *formulation = "cfosls"; // "cfosls" or "fosls"
    const char *space_for_S = "L2";     // "H1" or "L2"
    const char *space_for_sigma = "Hdiv"; // "Hdiv" or "H1"

    /*
    // Hdiv-H1 case
    using FormulType = CFOSLSFormulation_HdivH1Hyper;
    using FEFormulType = CFOSLSFEFormulation_HdivH1Hyper;
    using BdrCondsType = BdrConditions_CFOSLS_HdivH1_Hyper;
    using ProblemType = FOSLSProblem_HdivH1L2hyp;
    */

    // Hdiv-L2 case
    using FormulType = CFOSLSFormulation_HdivL2Hyper;
    using FEFormulType = CFOSLSFEFormulation_HdivL2Hyper;
    using BdrCondsType = BdrConditions_CFOSLS_HdivL2_Hyper;
    using ProblemType = FOSLSProblem_HdivL2L2hyp;

    // solver options
    int prec_option = 1; //defines whether to use preconditioner or not, and which one

    const char *mesh_file = "../data/cube_3d_moderate.mesh";
    //const char *mesh_file = "../data/square_2d_moderate.mesh";

    //const char *mesh_file = "../data/cube4d_low.MFEM";

    //const char *mesh_file = "../data/cube4d.MFEM";
    //const char *mesh_file = "dsadsad";
    //const char *mesh_file = "../data/orthotope3D_moderate.mesh";
    //const char *mesh_file = "../data/sphere3D_0.1to0.2.mesh";
    //const char * mesh_file = "../data/orthotope3D_fine.mesh";

    //const char * meshbase_file = "../data/sphere3D_0.1to0.2.mesh";
    //const char * meshbase_file = "../data/sphere3D_0.05to0.1.mesh";
    //const char * meshbase_file = "../data/sphere3D_veryfine.mesh";
    //const char * meshbase_file = "../data/beam-tet.mesh";
    //const char * meshbase_file = "../data/escher-p3.mesh";
    //const char * meshbase_file = "../data/orthotope3D_moderate.mesh";
    //const char * meshbase_file = "../data/orthotope3D_fine.mesh";
    //const char * meshbase_file = "../data/square_2d_moderate.mesh";
    //const char * meshbase_file = "../data/square_2d_fine.mesh";
    //const char * meshbase_file = "../data/square-disc.mesh";
    //const char *meshbase_file = "dsadsad";
    //const char * meshbase_file = "../data/circle_fine_0.1.mfem";
    //const char * meshbase_file = "../data/circle_moderate_0.2.mfem";

    int feorder         = 0;

    if (verbose)
        cout << "Solving (С)FOSLS Transport equation with MFEM & hypre \n";

    OptionsParser args(argc, argv);
    //args.AddOption(&mesh_file, "-m", "--mesh",
    //               "Mesh file to use.");
    args.AddOption(&feorder, "-o", "--feorder",
                   "Finite element order (polynomial degree).");
    args.AddOption(&ser_ref_levels, "-sref", "--sref",
                   "Number of serial refinements 4d mesh.");
    args.AddOption(&par_ref_levels, "-pref", "--pref",
                   "Number of parallel refinements 4d mesh.");
    args.AddOption(&nDimensions, "-dim", "--whichD",
                   "Dimension of the space-time problem.");
    args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                   "--no-visualization",
                   "Enable or disable GLVis visualization.");
    args.AddOption(&prec_option, "-precopt", "--prec-option",
                   "Preconditioner choice (0, 1 or 2 for now).");
    args.AddOption(&formulation, "-form", "--formul",
                   "Formulation to use (cfosls or fosls).");
    args.AddOption(&space_for_S, "-spaceS", "--spaceS",
                   "Space for S (H1 or L2).");
    args.AddOption(&space_for_sigma, "-spacesigma", "--spacesigma",
                   "Space for sigma (Hdiv or H1).");
    args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                   "--no-visualization",
                   "Enable or disable GLVis visualization.");

    args.Parse();
    if (!args.Good())
    {
       if (verbose)
       {
          args.PrintUsage(cout);
       }
       MPI_Finalize();
       return 1;
    }
    if (verbose)
    {
       args.PrintOptions(cout);
    }

    if (verbose)
    {
        if (strcmp(formulation,"cfosls") == 0)
            std::cout << "formulation: CFOSLS \n";
        else
            std::cout << "formulation: FOSLS \n";

        if (strcmp(space_for_sigma,"Hdiv") == 0)
            std::cout << "Space for sigma: Hdiv \n";
        else
            std::cout << "Space for sigma: H1 \n";

        if (strcmp(space_for_S,"H1") == 0)
            std::cout << "Space for S: H1 \n";
        else
            std::cout << "Space for S: L2 \n";

        if (strcmp(space_for_S,"L2") == 0)
            std::cout << "S: is eliminated from the system \n";
    }

    if (verbose)
        std::cout << "Running tests for the paper: \n";

    //mesh_file = "../data/netgen_cylinder_mesh_0.1to0.2.mesh";
    //mesh_file = "../data/pmesh_cylinder_moderate_0.2.mesh";
    //mesh_file = "../data/pmesh_cylinder_fine_0.1.mesh";

    //mesh_file = "../data/pmesh_check.mesh";
    mesh_file = "../data/cube_3d_moderate.mesh";

#ifdef CYLINDER_CUBE_TEST
    if (verbose)
        std::cout << "WARNING: CYLINDER_CUBE_TEST works only when the domain is a cube [0,1]! \n";
#endif


    if (verbose)
        std::cout << "For the records: numsol = " << numsol
                  << ", mesh_file = " << mesh_file << "\n";

#ifdef AMR
    if (verbose)
        std::cout << "AMR active \n";
#else
    if (verbose)
        std::cout << "AMR passive \n";
#endif

#ifdef PARTSOL_SETUP
    if (verbose)
        std::cout << "PARTSOL_SETUP active \n";
#else
    if (verbose)
        std::cout << "PARTSOL_SETUP passive \n";
#endif

#if defined(PARTSOL_SETUP) && (!(defined(DIVFREE_HCURLSETUP) || defined(DIVFREE_MINSOLVER)))
    MFEM_ABORT("For PARTSOL_SETUP one of the divfree options must be active");
#endif

#ifdef DIVFREE_MINSOLVER
    if (verbose)
        std::cout << "DIVFREE_MINSOLVER active \n";
#else
    if (verbose)
        std::cout << "DIVFREE_MINSOLVER passive \n";
#endif

#if defined(DIVFREE_MINSOLVER) && defined(DIVFREE_HCURLSETUP)
    MFEM_ABORT("Cannot have both \n");
#endif

#ifdef CLEVER_STARTING_GUESS
    if (verbose)
        std::cout << "CLEVER_STARTING_GUESS active \n";
#else
    if (verbose)
        std::cout << "CLEVER_STARTING_GUESS passive \n";
#endif

#if defined(CLEVER_STARTING_PARTSOL) && !defined(MULTILEVEL_PARTSOL)
    MFEM_ABORT("CLEVER_STARTING_PARTSOL cannot be active if MULTILEVEL_PARTSOL is not \n");
#endif

#ifdef CLEVER_STARTING_PARTSOL
    if (verbose)
        std::cout << "CLEVER_STARTING_PARTSOL active \n";
#else
    if (verbose)
        std::cout << "CLEVER_STARTING_PARTSOL passive \n";
#endif

#ifdef USE_GS_PREC
    if (verbose)
        std::cout << "USE_GS_PREC active (overwrites the prec_option) \n";
#else
    if (verbose)
        std::cout << "USE_GS_PREC passive \n";
#endif

#ifdef MULTILEVEL_PARTSOL
    if (verbose)
        std::cout << "MULTILEVEL_PARTSOL active \n";
#else
    if (verbose)
        std::cout << "MULTILEVEL_PARTSOL passive \n";
#endif

#ifdef CYLINDER_CUBE_TEST
    if (verbose)
        std::cout << "CYLINDER_CUBE_TEST active \n";
#else
    if (verbose)
        std::cout << "CYLINDER_CUBE_TEST passive \n";
#endif

#ifdef NEWINTERFACE
    if (verbose)
        std::cout << "NEWINTERFACE active \n";
#else
    if (verbose)
        std::cout << "NEWINTERFACE passive \n";
#endif

#ifdef MG_DIVFREEPREC
    if (verbose)
        std::cout << "MG_DIVFREEPREC active \n";
#else
    if (verbose)
        std::cout << "MG_DIVFREEPREC passive \n";
#endif

#ifdef RECOARSENING_AMR
    if (verbose)
        std::cout << "RECOARSENING_AMR active \n";
#else
    if (verbose)
        std::cout << "RECOARSENING_AMR passive \n";
#endif


    MFEM_ASSERT(strcmp(formulation,"cfosls") == 0 || strcmp(formulation,"fosls") == 0, "Formulation must be cfosls or fosls!\n");
    MFEM_ASSERT(strcmp(space_for_S,"H1") == 0 || strcmp(space_for_S,"L2") == 0, "Space for S must be H1 or L2!\n");
    MFEM_ASSERT(strcmp(space_for_sigma,"Hdiv") == 0 || strcmp(space_for_sigma,"H1") == 0, "Space for sigma must be Hdiv or H1!\n");

    MFEM_ASSERT(!strcmp(space_for_sigma,"H1") == 0 || (strcmp(space_for_sigma,"H1") == 0 && strcmp(space_for_S,"H1") == 0), "Sigma from H1vec must be coupled with S from H1!\n");

    if (verbose)
        std::cout << "Number of mpi processes: " << num_procs << "\n";

    StopWatch chrono;

    Mesh *mesh = NULL;

    shared_ptr<ParMesh> pmesh;

    if (nDimensions == 3 || nDimensions == 4)
    {
        if (verbose)
            cout << "Reading a " << nDimensions << "d mesh from the file " << mesh_file << endl;
        ifstream imesh(mesh_file);
        if (!imesh)
        {
             std::cerr << "\nCan not open mesh file: " << mesh_file << '\n' << std::endl;
             MPI_Finalize();
             return -2;
        }
        else
        {
            mesh = new Mesh(imesh, 1, 1);
            imesh.close();
        }
    }
    else //if nDimensions is no 3 or 4
    {
        if (verbose)
            cerr << "Case nDimensions = " << nDimensions << " is not supported \n"
                 << flush;
        MPI_Finalize();
        return -1;

    }
    //mesh = new Mesh(2, 2, 2, Element::HEXAHEDRON, 1);

    if (mesh) // if only serial mesh was generated previously, parallel mesh is initialized here
    {
        for (int l = 0; l < ser_ref_levels; l++)
            mesh->UniformRefinement();

        if ( verbose )
            cout << "Creating parmesh(" << nDimensions <<
                    "d) from the serial mesh (" << nDimensions << "d)" << endl << flush;
        pmesh = make_shared<ParMesh>(comm, *mesh);
        delete mesh;
    }

    for (int l = 0; l < par_ref_levels; l++)
    {
       pmesh->UniformRefinement();
    }

    int dim = nDimensions;

#ifdef CYLINDER_CUBE_TEST
    Vector vert_coos;
    pmesh->GetVertices(vert_coos);
    int nv = pmesh->GetNV();
    for (int vind = 0; vind < nv; ++vind)
    {
        for (int j = 0; j < dim; ++j)
        {
            if (j < dim - 1) // shift only in space
            {
                // translation by -0.5 in space variables
                vert_coos(j*nv + vind) -= 0.5;
                // dilation so that the resulting mesh covers [-1,1] ^d in space
                vert_coos(j*nv + vind) *= 2.0;
            }
            // dilation in time so that final time interval is [0,2]
            if (j == dim - 1)
                vert_coos(j*nv + vind) *= 2.0;
        }
    }
    pmesh->SetVertices(vert_coos);

    /*
    std::stringstream fname;
    fname << "checkmesh.mesh";
    std::ofstream ofid(fname.str().c_str());
    ofid.precision(8);
    pmesh->Print(ofid);

    MPI_Finalize();
    return 0;
    */

#endif

    pmesh->PrintInfo(std::cout); if(verbose) cout << endl;

    // 6. Define a parallel finite element space on the parallel mesh. Here we
    //    use the Raviart-Thomas finite elements of the specified order.

    int numblocks = 1;

    if (strcmp(space_for_S,"H1") == 0)
        numblocks++;
    if (strcmp(formulation,"cfosls") == 0)
        numblocks++;

    if (verbose)
        std::cout << "Number of blocks in the formulation: " << numblocks << "\n";

   if (verbose)
       std::cout << "Running AMR ... \n";

   FOSLSFormulation * formulat = new FormulType (dim, numsol, verbose);
   FOSLSFEFormulation * fe_formulat = new FEFormulType(*formulat, feorder);
   BdrConditions * bdr_conds = new BdrCondsType(*pmesh);

#ifdef CYLINDER_CUBE_TEST
   delete bdr_conds;
   MFEM_ASSERT(pmesh->bdr_attributes.Max() == 6, "For CYLINDER_CUBE_TEST there must be"
                                                 " a bdr aittrbute for each face");

   std::vector<Array<int>* > bdr_attribs_data(formulat->Nblocks());
   for (int i = 0; i < formulat->Nblocks(); ++i)
       bdr_attribs_data[i] = new Array<int>(pmesh->bdr_attributes.Max());

   if (strcmp(space_for_S,"L2") == 0)
   {
       *bdr_attribs_data[0] = 1;
       (*bdr_attribs_data[0])[5] = 0;
   }
   else // S from H^1
   {
       *bdr_attribs_data[0] = 0;
       *bdr_attribs_data[1] = 1;
       (*bdr_attribs_data[1])[5] = 0;
   }
   *bdr_attribs_data[formulat->Nblocks() - 1] = 0;

   bdr_conds = new BdrConditions(*pmesh, formulat->Nblocks());
   bdr_conds->Set(bdr_attribs_data);
#endif

   /*
   // Hdiv-L2 case
   int numfoslsfuns = 1;

   std::vector<std::pair<int,int> > grfuns_descriptor(numfoslsfuns);
   // this works
   grfuns_descriptor[0] = std::make_pair<int,int>(1, 0);

   Array2D<BilinearFormIntegrator *> integs(numfoslsfuns, numfoslsfuns);
   for (int i = 0; i < integs.NumRows(); ++i)
       for (int j = 0; j < integs.NumCols(); ++j)
           integs(i,j) = NULL;

   integs(0,0) = new VectorFEMassIntegrator(*Mytest.Ktilda);

   FOSLSEstimator * estimator;

   estimator = new FOSLSEstimator(*problem, grfuns_descriptor, NULL, integs, verbose);
   */


//#if 0
   bool with_hcurl = true;

   GeneralHierarchy * hierarchy = new GeneralHierarchy(1, *pmesh, feorder, verbose, with_hcurl);
   hierarchy->ConstructDofTrueDofs();
   hierarchy->ConstructDivfreeDops();

   FOSLSProblHierarchy<ProblemType, GeneralHierarchy> * prob_hierarchy = new
           FOSLSProblHierarchy<ProblemType, GeneralHierarchy>
           (*hierarchy, 1, *bdr_conds, *fe_formulat, prec_option, verbose);

   ProblemType * problem = prob_hierarchy->GetProblem(0);

#ifdef MULTILEVEL_PARTSOL
   const Array<SpaceName>* space_names_funct = problem->GetFEformulation().GetFormulation()->
           GetFunctSpacesDescriptor();
#endif

#ifdef DIVFREE_MINSOLVER
   FOSLSProblem* problem_mgtools = hierarchy->BuildDynamicProblem<ProblemType>
           (*bdr_conds, *fe_formulat, prec_option, verbose);
   hierarchy->AttachProblem(problem_mgtools);

   ComponentsDescriptor * descriptor;
   {
       bool with_Schwarz = true;
       bool optimized_Schwarz = true;
       bool with_Hcurl = true;
       bool with_coarsest_partfinder = true;
       bool with_coarsest_hcurl = false;
       bool with_monolithic_GS = false;
       descriptor = new ComponentsDescriptor(with_Schwarz, optimized_Schwarz,
                                                     with_Hcurl, with_coarsest_partfinder,
                                                     with_coarsest_hcurl, with_monolithic_GS);
   }
   MultigridToolsHierarchy * mgtools_hierarchy =
           new MultigridToolsHierarchy(*hierarchy, problem_mgtools->GetAttachedIndex(), *descriptor);

   GeneralMinConstrSolver * NewSolver;
   {
       bool with_local_smoothers = true;
       bool optimized_localsolvers = true;
       bool with_hcurl_smoothers = true;

       int stopcriteria_type = 1;

       int numblocks_funct = numblocks - 1;

       int size_funct = problem_mgtools->GetTrueOffsetsFunc()[numblocks_funct];
       NewSolver = new GeneralMinConstrSolver(size_funct, *mgtools_hierarchy, with_local_smoothers,
                                        optimized_localsolvers, with_hcurl_smoothers, stopcriteria_type, verbose);
   }
#endif


#if defined(PARTSOL_SETUP) && defined(MULTILEVEL_PARTSOL)
   bool optimized_localsolvers = true;
   bool with_hcurl_smoothers = true;
   DivConstraintSolver * partsol_finder;

   partsol_finder = new DivConstraintSolver
           (*problem, *hierarchy, optimized_localsolvers, with_hcurl_smoothers, verbose);

   bool report_funct = true;
#endif // for #ifdef PARTSOL_SETUP or MULTILEVEL_PARTSOL

   int fosls_func_version = 1;
   if (verbose)
    std::cout << "fosls_func_version = " << fosls_func_version << "\n";

   int numblocks_funct = 1;
   if (strcmp(space_for_S,"H1") == 0)
       ++numblocks_funct;

   /// The descriptor describes the grid functions used in the error estimator
   /// each pair (which corresponds to a grid function used in the estimator)
   /// has the form <a,b>, where:
   /// 1) a pair of the form <1,b> means that the corresponding grid function
   /// is one of the grid functions inside the FOSLSProblem, and b
   /// equals its index in grfuns array
   /// 2) a pair of the for <-1,b> means that the grid function is in the extra
   /// grid functions (additional argument in the estimator construction)
   /// and b is its index inside the extra grfuns array.
   /// (*) The user should take care of updating the extra grfuns, if they
   /// are not a part of the problem (e.g., defined on a different pfespace)

   std::vector<std::pair<int,int> > grfuns_descriptor;
   Array<ParGridFunction*> extra_grfuns;
   Array2D<BilinearFormIntegrator *> integs;

   DefineEstimatorComponents(problem_mgtools, fosls_func_version, grfuns_descriptor, extra_grfuns, integs, verbose);

   FOSLSEstimator * estimator;
   estimator = new FOSLSEstimator(*problem_mgtools, grfuns_descriptor, NULL, integs, verbose);
   problem_mgtools->AddEstimator(*estimator);

   //ThresholdSmooRefiner refiner(*estimator); // 0.1, 0.001
   ThresholdRefiner refiner(*estimator);

   refiner.SetTotalErrorFraction(0.9); // 0.5

#ifdef PARTSOL_SETUP
   Array<Vector*> div_rhs_lvls(0);
   Array<BlockVector*> partsol_lvls(0);
   Array<BlockVector*> partsol_funct_lvls(0);
   Array<BlockVector*> initguesses_funct_lvls(0);
#endif

   Array<BlockVector*> problem_sols_lvls(0);

   // 12. The main AMR loop. In each iteration we solve the problem on the
   //     current mesh, visualize the solution, and refine the mesh.
   const int max_dofs = 300000;//1600000; 400000;

   HYPRE_Int global_dofs = problem->GlobalTrueProblemSize();
   std::cout << "starting n_el = " << hierarchy->GetFinestParMesh()->GetNE() << "\n";

   double fixed_rtol = 1.0e-12; // 1.0e-10
   double fixed_atol = 1.0e-15;
   double initial_res_norm = -1.0;

   bool compute_error = true;

   // Main loop (with AMR or uniform refinement depending on the predefined macro AMR)
   int max_iter_amr = 6;
   for (int it = 0; it < max_iter_amr; it++)
   {
       if (verbose)
       {
          cout << "\nAMR iteration " << it << "\n";
          cout << "Number of unknowns: " << global_dofs << "\n\n";
       }

       initguesses_funct_lvls.Prepend(new BlockVector(problem->GetTrueOffsetsFunc()));
       *initguesses_funct_lvls[0] = 0.0;

       problem_sols_lvls.Prepend(new BlockVector(problem->GetTrueOffsets()));
       *problem_sols_lvls[0] = 0.0;

       div_rhs_lvls.Prepend(new Vector(problem->GetRhs().GetBlock(numblocks - 1).Size()));
       *div_rhs_lvls[0] = problem->GetRhs().GetBlock(numblocks - 1);

#ifdef APPROACH_1
       int l = 0;
       ProblemType * problem_l = prob_hierarchy->GetProblem(l);

       *initguesses_funct_lvls[l] = 0.0;
       // setting correct bdr values
       problem_l->SetExactBndValues(*initguesses_funct_lvls[l]);

       MFEM_ABORT("Think carefully whether you need to move the boundary values into the "
                  "righthand side related to the functional part \n");
       partsol_finder->FindParticularSolution(partsol_guess, *partsol_funct_lvls[0], *div_rhs_lvls[0], verbose, report_funct);
#endif

       // recoarsening constraint rhsides from finest to coarsest level
       for (int l = 1; l < div_rhs_lvls.Size(); ++l)
           hierarchy->GetTruePspace(SpaceName::L2,l - 1)->MultTranspose(*div_rhs_lvls[l-1], *div_rhs_lvls[l]);

       // re-solving all the problems with coarsened rhs, from coarsest to finest
       // and using the previous soluition as a starting guess
#ifdef RECOARSENING_AMR
       int coarsest_lvl = hierarchy->Nlevels() - 1;
       for (int l = coarsest_lvl; l >= 0; --l) // l = 0 could be included actually after testing
#else
       for (int l = 0; l >= 0; --l) // only l = 0
#endif
       {
           if (verbose)
               std::cout << "level " << l << "\n";
           ProblemType * problem_l = prob_hierarchy->GetProblem(l);

           // solving the problem at level l

           *initguesses_funct_lvls[l] = 0.0;

#ifdef CLEVER_STARTING_GUESS
           // create a better initial guess
           if (l < coarsest_lvl)
           {
               for (int blk = 0; blk < numblocks_funct; ++blk)
                   hierarchy->GetTruePspace( (*space_names_funct)[blk], l)->Mult
                       (problem_sols_lvls[l + 1]->GetBlock(blk), initguesses_funct_lvls[l]->GetBlock(blk));

               std::cout << "check init norm before bnd = " << initguesses_funct_lvls[l]->Norml2()
                            / sqrt (initguesses_funct_lvls[l]->Size()) << "\n";
           }
#endif
           // setting correct bdr values
           problem_l->SetExactBndValues(*initguesses_funct_lvls[l]);

           //std::cout << "check init norm after bnd = " << initguesses_funct_lvls[l]->Norml2()
                        /// sqrt (initguesses_funct_lvls[l]->Size()) << "\n";

           // checking the initial guess
           {
               problem_l->ComputeBndError(*initguesses_funct_lvls[l]);

               HypreParMatrix & Constr = (HypreParMatrix&)(problem_l->GetOp_nobnd()->GetBlock(numblocks - 1, 0));
               Vector tempc(Constr.Height());
               Constr.Mult(initguesses_funct_lvls[l]->GetBlock(0), tempc);
               tempc -= *div_rhs_lvls[l];
               double res_constr_norm = ComputeMPIVecNorm(comm, tempc, "", false);
               MFEM_ASSERT (res_constr_norm < 1.0e-10, "");
           }

           // functional value for the initial guess
           CheckFunctValue(comm,*NewSolver->GetFunctOp_nobnd(l), NULL, *initguesses_funct_lvls[l],
                           "for the initial guess ", verbose);

           BlockVector zero_vec(problem_l->GetTrueOffsetsFunc());
           zero_vec = 0.0;
           NewSolver->SetInitialGuess(l, zero_vec);
           NewSolver->SetConstrRhs(*div_rhs_lvls[l]);

           //if (verbose)
               //NewSolver->PrintAllOptions();

           BlockVector NewRhs(problem_l->GetTrueOffsetsFunc());
           NewRhs = 0.0;

           // computing rhs = ...
           BlockVector padded_initguess(problem_l->GetTrueOffsets());
           padded_initguess = 0.0;
           for (int blk = 0; blk < numblocks_funct; ++blk)
               padded_initguess.GetBlock(blk) = initguesses_funct_lvls[l]->GetBlock(blk);

           BlockVector padded_rhs(problem_l->GetTrueOffsets());
           problem_l->GetOp_nobnd()->Mult(padded_initguess, padded_rhs);

           padded_rhs *= -1;
           for (int blk = 0; blk < numblocks_funct; ++blk)
               NewRhs.GetBlock(blk) = padded_rhs.GetBlock(blk);
           problem_l->ZeroBndValues(NewRhs);

           NewSolver->SetFunctRhs(NewRhs);

           HypreParMatrix & Constr_l = (HypreParMatrix&)(problem_l->GetOp()->GetBlock(numblocks - 1, 0));

           // solving for correction
           BlockVector correction(problem_l->GetTrueOffsetsFunc());
           correction = 0.0;
           //std::cout << "NewSolver size = " << NewSolver->Size() << "\n";
           //std::cout << "NewRhs norm = " << NewRhs.Norml2() / sqrt (NewRhs.Size()) << "\n";
           //if (l == 0)
               //NewSolver->SetPrintLevel(1);
           //else
               NewSolver->SetPrintLevel(1);

           NewSolver->Mult(l, &Constr_l, NewRhs, correction);

           for (int blk = 0; blk < numblocks_funct; ++blk)
           {
               problem_sols_lvls[l]->GetBlock(blk) = initguesses_funct_lvls[l]->GetBlock(blk);
               problem_sols_lvls[l]->GetBlock(blk) += correction.GetBlock(blk);
           }

           if (l == 0)
           {
               BlockVector tmp1(problem_l->GetTrueOffsetsFunc());
               for (int blk = 0; blk < numblocks_funct; ++blk)
                   tmp1.GetBlock(blk) = problem_sols_lvls[l]->GetBlock(blk);

               CheckFunctValue(comm,*NewSolver->GetFunctOp_nobnd(0), NULL, tmp1,
                               "for the finest level solution ", verbose);

               BlockVector tmp2(problem_l->GetTrueOffsetsFunc());
               for (int blk = 0; blk < numblocks_funct; ++blk)
                   tmp2.GetBlock(blk) = problem_l->GetExactSolProj()->GetBlock(blk);

               CheckFunctValue(comm,*NewSolver->GetFunctOp_nobnd(0), NULL, tmp2,
                               "for the projection of the exact solution ", verbose);
           }

       } // end of loop over levels

#ifdef RECOARSENING_AMR
       if (verbose)
           std::cout << "Re-coarsening (and re-solving if divfree problem in H(curl) is considered)"
                        " has been finished\n\n";
#endif

       if (compute_error)
           problem_mgtools->ComputeError(*problem_sols_lvls[0], verbose, true);

       // to make sure that problem has grfuns in correspondence with the problem_sol we compute here
       // though for now its coordination already happens in ComputeError()
       problem_mgtools->DistributeToGrfuns(*problem_sols_lvls[0]);


       // Send the solution by socket to a GLVis server.
       if (visualization && it == max_iter_amr - 1)
       {
           int ne = pmesh->GetNE();
           for (int elind = 0; elind < ne; ++elind)
               pmesh->SetAttribute(elind, elind);
           ParGridFunction * sigma = problem_mgtools->GetGrFun(0);
           ParGridFunction * S;
           S = problem_mgtools->GetGrFun(1);

           char vishost[] = "localhost";
           int  visport   = 19916;

           socketstream sigma_sock(vishost, visport);
           sigma_sock << "parallel " << num_procs << " " << myid << "\n";
           sigma_sock << "solution\n" << *pmesh << *sigma << "window_title 'sigma, AMR iter No."
                  << it <<"'" << flush;

           socketstream s_sock(vishost, visport);
           s_sock << "parallel " << num_procs << " " << myid << "\n";
           s_sock << "solution\n" << *pmesh << *S << "window_title 'S, AMR iter No."
                  << it <<"'" << flush;
       }

#ifdef AMR
       int nel_before = hierarchy->GetFinestParMesh()->GetNE();

       // testing with only 1 element marked for refinement
       //Array<int> els_to_refine(1);
       //els_to_refine = hierarchy->GetFinestParMesh()->GetNE() / 2;
       //hierarchy->GetFinestParMesh()->GeneralRefinement(els_to_refine);

       // true AMR
       refiner.Apply(*hierarchy->GetFinestParMesh());
       int nmarked_el = refiner.GetNumMarkedElements();
       if (verbose)
       {
           std::cout << "Marked elements percentage = " << 100 * nmarked_el * 1.0 / nel_before << " % \n";
           std::cout << "nmarked_el = " << nmarked_el << ", nel_before = " << nel_before << "\n";
           int nel_after = hierarchy->GetFinestParMesh()->GetNE();
           std::cout << "nel_after = " << nel_after << "\n";
           std::cout << "number of elements introduced = " << nel_after - nel_before << "\n";
           std::cout << "percentage (w.r.t to # before) of elements introduced = " <<
                        100.0 * (nel_after - nel_before) * 1.0 / nel_before << "% \n\n";
       }

       if (visualization && it == max_iter_amr - 1)
       {
           const Vector& local_errors = estimator->GetLastLocalErrors();
           if (feorder == 0)
               MFEM_ASSERT(local_errors.Size() == problem_mgtools->GetPfes(numblocks_funct)->TrueVSize(), "");

           FiniteElementCollection * l2_coll;
           if (feorder > 0)
               l2_coll = new L2_FECollection(0, dim);

           ParFiniteElementSpace * L2_space;
           if (feorder == 0)
               L2_space = problem_mgtools->GetPfes(numblocks_funct);
           else
               L2_space = new ParFiniteElementSpace(problem_mgtools->GetParMesh(), l2_coll);
           ParGridFunction * local_errors_pgfun = new ParGridFunction(L2_space);
           local_errors_pgfun->SetFromTrueDofs(local_errors);
           char vishost[] = "localhost";
           int  visport   = 19916;

           socketstream amr_sock(vishost, visport);
           amr_sock << "parallel " << num_procs << " " << myid << "\n";
           amr_sock << "solution\n" << *pmesh << *local_errors_pgfun <<
                         "window_title 'local errors, AMR iter No." << it <<"'" << flush;

           if (feorder > 0)
           {
               delete l2_coll;
               delete L2_space;
           }
       }

#else
       hierarchy->GetFinestParMesh()->UniformRefinement();
#endif

       if (refiner.Stop())
       {
          if (verbose)
             cout << "Stopping criterion satisfied. Stop. \n";
          break;
       }

       bool recoarsen = true;
       prob_hierarchy->Update(recoarsen);
       problem = prob_hierarchy->GetProblem(0);

       problem_mgtools->BuildSystem(verbose);
       mgtools_hierarchy->Update(recoarsen);
       NewSolver->UpdateProblem(*problem_mgtools);
       NewSolver->Update(recoarsen);

       //partsol_finder->UpdateProblem(*problem);
       //partsol_finder->Update(recoarsen);

       // checking #dofs after the refinement
       global_dofs = problem_mgtools->GlobalTrueProblemSize();

       if (global_dofs > max_dofs)
       {
          if (verbose)
             cout << "Reached the maximum number of dofs. Stop. \n";
          break;
       }

#if 0 // old code
#ifdef PARTSOL_SETUP
       // finding a particular solution
       partsol_lvls.Prepend(new BlockVector(problem->GetTrueOffsets()));
       *partsol_lvls[0] = 0.0;

#ifdef MULTILEVEL_PARTSOL
       partsol_funct_lvls.Prepend(new BlockVector(problem->GetTrueOffsetsFunc()));
#endif

#ifdef RECOARSENING_AMR
       if (verbose)
           std::cout << "Starting re-coarsening and re-solving part \n";

       // recoarsening constraint rhsides from finest to coarsest level
       for (int l = 1; l < div_rhs_lvls.Size(); ++l)
           hierarchy->GetTruePspace(SpaceName::L2,l - 1)->MultTranspose(*div_rhs_lvls[l-1], *div_rhs_lvls[l]);

       if (verbose)
       {
           std::cout << "norms of partsol_lvls before: \n";
           for (int l = 0; l < partsol_lvls.Size(); ++l)
               std::cout << "partsol norm = " << partsol_lvls[l]->Norml2() / sqrt(partsol_lvls[l]->Size()) << "\n";;
           for (int l = 0; l < div_rhs_lvls.Size(); ++l)
               std::cout << "rhs norm = " << div_rhs_lvls[l]->Norml2() / sqrt(div_rhs_lvls[l]->Size()) << "\n";;
       }

       // re-solving all the problems with coarsened rhs, from coarsest to finest
       // and using the previous soluition as a starting guess
       int coarsest_lvl = prob_hierarchy->Nlevels() - 1;
       for (int l = coarsest_lvl; l > 0; --l) // l = 0 could be included actually after testing
       {
           if (verbose)
               std::cout << "level " << l << "\n";
           ProblemType * problem_l = prob_hierarchy->GetProblem(l);

           // finding a new particular solution for the new rhs
#ifdef MULTILEVEL_PARTSOL
           Vector partsol_guess(partsol_funct_lvls[l]->Size());//partsol_finder->Size());
           partsol_guess = 0.0;

#ifdef      CLEVER_STARTING_PARTSOL
           if (l < coarsest_lvl)
           {
               BlockVector partsol_guess_viewer(partsol_guess.GetData(), problem_l->GetTrueOffsetsFunc());
               for (int blk = 0; blk < numblocks_funct; ++blk)
                   hierarchy->GetTruePspace((*space_names_funct)[blk], l)->Mult
                           (partsol_funct_lvls[l + 1]->GetBlock(blk), partsol_guess_viewer.GetBlock(blk));
           }
#endif
           HypreParMatrix& Constr_l = (HypreParMatrix&)problem_l->GetOp_nobnd()->GetBlock(numblocks_funct,0);
           // full V-cycle
           //partsol_finder->FindParticularSolution(l, Constr_l, partsol_guess,
                                                  //*partsol_funct_lvls[l], *div_rhs_lvls[l], verbose, report_funct);

           // finest available level update
           partsol_finder->UpdateParticularSolution(l, Constr_l, partsol_guess,
                                                  *partsol_funct_lvls[l], *div_rhs_lvls[l], verbose, report_funct);

           for (int blk = 0; blk < numblocks_funct; ++blk)
               partsol_lvls[l]->GetBlock(blk) = partsol_funct_lvls[l]->GetBlock(blk);
#else
           HypreParMatrix * B_hpmat = dynamic_cast<HypreParMatrix*>(&problem_l->GetOp()->GetBlock(numblocks - 1,0));
           ParGridFunction * partsigma = FindParticularSolution(problem_l->GetPfes(0), *B_hpmat, *div_rhs_lvls[l], verbose);
           partsigma->ParallelProject(partsol_lvls[l]->GetBlock(0));
           delete partsigma;
#endif // for #else for #ifdef MULTILEVEL_PARTSOL

           // a check that the particular solution does satisfy the divergence constraint after all
           HypreParMatrix & Constr = (HypreParMatrix&)(problem_l->GetOp()->GetBlock(numblocks - 1, 0));
           Vector tempc(Constr.Height());
           Constr.Mult(partsol_lvls[l]->GetBlock(0), tempc);
           tempc -= *div_rhs_lvls[l];
           double res_constr_norm = ComputeMPIVecNorm(comm, tempc, "", false);
           MFEM_ASSERT (res_constr_norm < 1.0e-10, "");

#ifdef NEW_INTERFACE
           MFEM_ABORT("Not ready yet \n");
#endif // end of #ifdef NEW_INTERFACE
       }

       if (verbose)
           std::cout << "Re-coarsening (and re-solving if divfree problem in H(curl) is considered)"
                        " has been finished\n";

       if (verbose)
       {
           std::cout << "norms of partsol_lvls after: \n";
           for (int l = 0; l < partsol_lvls.Size(); ++l)
               std::cout << "partsol norm = " << partsol_lvls[l]->Norml2() / sqrt(partsol_lvls[l]->Size()) << "\n";;
       }


#endif // end of #ifdef RECOARSENING_AMR

#ifdef MULTILEVEL_PARTSOL

       // define a starting guess for the particular solution finder
       Vector partsol_guess(partsol_finder->Size());
       partsol_guess = 0.0;

#ifdef CLEVER_STARTING_PARTSOL
       if (it > 0)
       {
           BlockVector partsol_guess_viewer(partsol_guess.GetData(), problem->GetTrueOffsetsFunc());
           for (int blk = 0; blk < numblocks_funct; ++blk)
               hierarchy->GetTruePspace((*space_names_funct)[blk], 0)
                       ->Mult(partsol_lvls[1]->GetBlock(blk), partsol_guess_viewer.GetBlock(blk));
       }
#endif

       // full V-cycle
       //partsol_finder->FindParticularSolution(partsol_guess, *partsol_funct_lvls[0], *div_rhs_lvls[0], verbose, report_funct);
       // only finest level update
       partsol_finder->UpdateParticularSolution(partsol_guess, *partsol_funct_lvls[0], *div_rhs_lvls[0], verbose, report_funct);

       for (int blk = 0; blk < numblocks_funct; ++blk)
           partsol_lvls[0]->GetBlock(blk) = partsol_funct_lvls[0]->GetBlock(blk);

#else // not a multilevel particular solution finder
       HypreParMatrix * B_hpmat = dynamic_cast<HypreParMatrix*>(&problem->GetOp()->GetBlock(numblocks - 1,0));
       ParGridFunction * partsigma = FindParticularSolution(problem->GetPfes(0), *B_hpmat, *div_rhs_lvls[0], verbose);
       partsigma->ParallelProject(partsol_lvls[0]->GetBlock(0));
       delete partsigma;
#endif // for #ifdef MULTILEVEL_PARTSOL

       // a check that the particular solution does satisfy the divergence constraint after all
       HypreParMatrix & Constr = (HypreParMatrix&)(problem->GetOp()->GetBlock(numblocks - 1, 0));
       Vector tempc(Constr.Height());
       Constr.Mult(partsol_lvls[0]->GetBlock(0), tempc);
       tempc -= *div_rhs_lvls[0];//problem->GetRhs().GetBlock(numblocks_funct);
       double res_constr_norm = ComputeMPIVecNorm(comm, tempc, "", false);
       MFEM_ASSERT (res_constr_norm < 1.0e-10, "");

#ifdef DIVFREE_MINSOLVER
       BlockVector& problem_sol = problem->GetSol();
       problem_sol = 0.0;

       double newsolver_reltol = 1.0e-6;

       if (verbose)
           std::cout << "newsolver_reltol = " << newsolver_reltol << "\n";

       NewSolver->SetRelTol(newsolver_reltol);
       NewSolver->SetMaxIter(200);
       NewSolver->SetPrintLevel(1);
       NewSolver->SetStopCriteriaType(0);

#ifdef CLEVER_STARTING_GUESS
       if (it > 0)
           for (int blk = 0; blk < numblocks_funct; ++blk)
               hierarchy->GetTruePspace( (*space_names_funct)[blk], 0)->Mult
                   (problem_sols_lvls[1]->GetBlock(blk), initguesses_funct_lvls[0]->GetBlock(blk));
#endif
       *initguesses_funct_lvls[0] += *partsol_funct_lvls[0];

       NewSolver->SetInitialGuess(*partsol_funct_lvls[0]);
       NewSolver->SetConstrRhs(*div_rhs_lvls[0]);
       //NewSolver.SetUnSymmetric();

       if (verbose)
           NewSolver->PrintAllOptions();

       Vector NewRhs(NewSolver->Size());
       NewRhs = 0.0;

       MFEM_ASSERT(strcmp(space_for_S,"L2") == 0, "Current implementation with GeneralMinConstrSolver works only when S is from L2 \n");

       BlockVector divfree_part(problem->GetTrueOffsetsFunc());
       NewSolver->Mult(NewRhs, divfree_part);

       for (int blk = 0; blk < numblocks_funct; ++blk)
           problem_sol.GetBlock(blk) = divfree_part.GetBlock(blk);
#endif

       problem_sol += *partsol_lvls[0];

       *problem_sols_lvls[0] = problem_sol;

       if (compute_error)
           problem->ComputeError(problem_sol, verbose, false);

       // to make sure that problem has grfuns in correspondence with the problem_sol we compute here
       // though for now its coordination already happens in ComputeError()
       problem->DistributeToGrfuns(problem_sol);
#else // the case when the original problem is solved, i.e., no particular solution is used

#ifdef CLEVER_STARTING_GUESS
       // if it's not the first iteration we reuse the previous solution as a starting guess
       if (it > 0)
           prob_hierarchy->GetTrueP(0)->Mult(*problem_sols_lvls[1], problem->GetSol());

       // checking the residual
       BlockVector res(problem->GetTrueOffsets());
       problem->GetOp()->Mult(problem->GetSol(), res);
       res -= problem->GetRhs();

       double res_norm = ComputeMPIVecNorm(comm, res, "", false);
       if (it == 0)
           initial_res_norm = res_norm;

       if (verbose)
           std::cout << "Initial res norm at iteration # " << it << " = " << res_norm << "\n";

       double adjusted_rtol = fixed_rtol * initial_res_norm / res_norm;
       if (verbose)
           std::cout << "adjusted rtol = " << adjusted_rtol << "\n";

       problem->SetRelTol(adjusted_rtol);
       problem->SetAbsTol(fixed_atol);
#ifdef USE_GS_PREC
       if (it > 0)
       {
           prec_option = 100;
           std::cout << "Resetting prec with the Gauss-Seidel preconditioners \n";
           problem->ResetPrec(prec_option);
       }
#endif

       //std::cout << "checking rhs norm for the first solve: " <<
                    //problem->GetRhs().Norml2() /  sqrt (problem->GetRhs().Size()) << "\n";

       problem->SolveProblem(problem->GetRhs(), problem->GetSol(), verbose, false);

       // checking the residual afterwards
       {
           BlockVector res(problem->GetTrueOffsets());
           problem->GetOp()->Mult(problem->GetSol(), res);
           res -= problem->GetRhs();

           double res_norm = ComputeMPIVecNorm(comm, res, "", false);
           if (verbose)
               std::cout << "Res norm after solving the problem at iteration # "
                         << it << " = " << res_norm << "\n";
       }

#else
       problem->Solve(verbose, false);
#endif

       *problem_sols_lvls[0] = problem->GetSol();
      if (compute_error)
      {
          problem->ComputeError(*problem_sols_lvls[0], verbose, true);
          problem->ComputeBndError(*problem_sols_lvls[0]);
      }

      // special testing cheaper preconditioners!
      /*
      if (verbose)
          std::cout << "Performing a special check for the preconditioners iteration counts! \n";

      prec_option = 100;
      problem->ResetPrec(prec_option);

      BlockVector special_guess(problem->GetTrueOffsets());
      special_guess = problem->GetSol();

      int special_num = 1;
      Array<int> el_indices(special_num);
      for (int i = 0; i < special_num; ++i)
          el_indices[i] = problem->GetParMesh()->GetNE() / 2 + i;

      std::cout << "Number of elements where the sol was changed: " <<
                   special_num << "(" <<  special_num * 100.0 /
                   problem->GetParMesh()->GetNE() << "%) \n";

      for (int blk = 0; blk < problem->GetFEformulation().Nblocks(); ++blk)
      {
          ParFiniteElementSpace * pfes = problem->GetPfes(blk);

          Array<int> dofs;
          MFEM_ASSERT(num_procs == 1, "This works only in serial");

          for (int elind = 0; elind < el_indices.Size(); ++elind)
          {
              pfes->GetElementDofs(el_indices[elind], dofs);

              for (int i = 0; i < dofs.Size(); ++i)
                  //special_guess.GetBlock(blk)[dofs[i]] = 0.0;
                  special_guess.GetBlock(blk)[dofs[i]] =
                    problem->GetSol().GetBlock(blk)[dofs[i]] * 0.9;
          }
      }

      BlockVector check_diff(problem->GetTrueOffsets());
      check_diff = special_guess;
      check_diff -= problem->GetSol();
      double check_diff_norm = ComputeMPIVecNorm(comm, check_diff, "", false);

      if (verbose)
          std::cout << "|| sol - special_guess || = " << check_diff_norm << "\n";

      int nnz_count = 0;
      for (int i = 0; i < check_diff.Size(); ++i)
          if (fabs(check_diff[i]) > 1.0e-8)
              ++nnz_count;

      if (verbose)
          std::cout << "nnz_count in the diff = " << nnz_count << "\n";

      {
          // checking the residual
          BlockVector res(problem->GetTrueOffsets());
          problem->GetOp()->Mult(special_guess, res);
          res -= problem->GetRhs();

          double res_norm = ComputeMPIVecNorm(comm, res, "", false);

          if (verbose)
              std::cout << "Initial res norm for the second solve = " << res_norm << "\n";

          double adjusted_rtol = fixed_rtol * initial_res_norm / res_norm;
          if (verbose)
              std::cout << "adjusted rtol = " << adjusted_rtol << "\n";

          problem->SetRelTol(adjusted_rtol);
          problem->SetAbsTol(fixed_atol);
      }


      std::cout << "checking rhs norm for the second solve: " <<
                   problem->GetRhs().Norml2() /  sqrt (problem->GetRhs().Size()) << "\n";
      problem->SolveProblem(problem->GetRhs(), special_guess, verbose, false);

      //problem_sol = problem->GetSol();
      //if (compute_error)
          //problem->ComputeError(problem_sol, verbose, true);

      MPI_Finalize();
      return 0;
      */

#endif

       // 17. Send the solution by socket to a GLVis server.
       if (visualization)
       {
           ParGridFunction * sigma = problem->GetGrFun(0);
           ParGridFunction * S;
           if (strcmp(space_for_S,"H1") == 0)
               S = problem->GetGrFun(1);
           else
               S = (dynamic_cast<FOSLSProblem_HdivL2L2hyp*>(problem))->RecoverS();

           char vishost[] = "localhost";
           int  visport   = 19916;

           socketstream sigma_sock(vishost, visport);
           sigma_sock << "parallel " << num_procs << " " << myid << "\n";
           sigma_sock << "solution\n" << *pmesh << *sigma << "window_title 'sigma, AMR iter No."
                  << it <<"'" << flush;

           socketstream s_sock(vishost, visport);
           s_sock << "parallel " << num_procs << " " << myid << "\n";
           s_sock << "solution\n" << *pmesh << *S << "window_title 'S, AMR iter No."
                  << it <<"'" << flush;
       }

       // 18. Call the refiner to modify the mesh. The refiner calls the error
       //     estimator to obtain element errors, then it selects elements to be
       //     refined and finally it modifies the mesh. The Stop() method can be
       //     used to determine if a stopping criterion was met.

#ifdef AMR
       int nel_before = hierarchy->GetFinestParMesh()->GetNE();

       // testing with only 1 element marked for refinement
       //Array<int> els_to_refine(1);
       //els_to_refine = hierarchy->GetFinestParMesh()->GetNE() / 2;
       //hierarchy->GetFinestParMesh()->GeneralRefinement(els_to_refine);

       // true AMR
       refiner.Apply(*hierarchy->GetFinestParMesh());
       int nmarked_el = refiner.GetNumMarkedElements();
       if (verbose)
       {
           std::cout << "Marked elements percentage = " << 100 * nmarked_el * 1.0 / nel_before << " % \n";
           std::cout << "nmarked_el = " << nmarked_el << ", nel_before = " << nel_before << "\n";
           int nel_after = hierarchy->GetFinestParMesh()->GetNE();
           std::cout << "nel_after = " << nel_after << "\n";
           std::cout << "number of elements introduced = " << nel_after - nel_before << "\n";
           std::cout << "percentage (w.r.t to # before) of elements introduced = " <<
                        100.0 * (nel_after - nel_before) * 1.0 / nel_before << "% \n\n";
       }

       if (visualization)
       {
           const Vector& local_errors = estimator->GetLocalErrors();
           if (feorder == 0)
               MFEM_ASSERT(local_errors.Size() == problem->GetPfes(numblocks_funct)->TrueVSize(), "");

           FiniteElementCollection * l2_coll;
           if (feorder > 0)
               l2_coll = new L2_FECollection(0, dim);

           ParFiniteElementSpace * L2_space;
           if (feorder == 0)
               L2_space = problem->GetPfes(numblocks_funct);
           else
               L2_space = new ParFiniteElementSpace(problem->GetParMesh(), l2_coll);
           ParGridFunction * local_errors_pgfun = new ParGridFunction(L2_space);
           local_errors_pgfun->SetFromTrueDofs(local_errors);
           char vishost[] = "localhost";
           int  visport   = 19916;

           socketstream amr_sock(vishost, visport);
           amr_sock << "parallel " << num_procs << " " << myid << "\n";
           amr_sock << "solution\n" << *pmesh << *local_errors_pgfun <<
                         "window_title 'local errors, AMR iter No." << it <<"'" << flush;

           if (feorder > 0)
           {
               delete l2_coll;
               delete L2_space;
           }
       }

#else
       hierarchy->GetFinestParMesh()->UniformRefinement();
#endif

       if (refiner.Stop())
       {
          if (verbose)
             cout << "Stopping criterion satisfied. Stop. \n";
          break;
       }

       bool recoarsen = true;
       prob_hierarchy->Update(recoarsen);
       problem = prob_hierarchy->GetProblem(0);

#ifdef PARTSOL_SETUP

#ifdef DIVFREE_MINSOLVER
       problem_mgtools->BuildSystem(verbose);
       mgtools_hierarchy->Update(recoarsen);
       NewSolver->UpdateProblem(*problem_mgtools);
       NewSolver->Update(recoarsen);
#endif

#ifdef      MULTILEVEL_PARTSOL

       // updating partsol_finder
       partsol_finder->UpdateProblem(*problem);

       partsol_finder->Update(recoarsen);
#endif // endif for MULTILEVEL_PARTSOL

#endif // for #ifdef PARTSOL_SETUP

       if (fosls_func_version == 2)
       {
           // first option is just to delete and re-construct the extra grid function
           // this is slightly different from the old approach when the pgfun was
           // updated (~ interpolated)
           /*
           delete extra_grfuns[0];
           extra_grfuns[0] = new ParGridFunction(problem->GetPfes(numblocks - 1));
           extra_grfuns[0]->ProjectCoefficient(*problem->GetFEformulation().
                                               GetFormulation()->GetTest()->GetRhs());
           */

           // second option is to project it (which is quiv. to Update() in the
           // old variant w/o hierarchies
           Vector true_temp1(prob_hierarchy->GetProblem(1)->GetPfes(numblocks - 1)->TrueVSize());
           extra_grfuns[0]->ParallelProject(true_temp1);

           Vector true_temp2(prob_hierarchy->GetProblem(0)->GetPfes(numblocks - 1)->TrueVSize());
           prob_hierarchy->GetHierarchy().GetTruePspace(SpaceName::L2, 0)->Mult(true_temp1, true_temp2);
           delete extra_grfuns[0];
           extra_grfuns[0] = new ParGridFunction(problem->GetPfes(numblocks - 1));
           extra_grfuns[0]->SetFromTrueDofs(true_temp2);
       }

       // checking #dofs after the refinement
       global_dofs = problem->GlobalTrueProblemSize();

       if (global_dofs > max_dofs)
       {
          if (verbose)
             cout << "Reached the maximum number of dofs. Stop. \n";
          break;
       }

#endif

   } // end of the main AMR loop

   MPI_Finalize();
   return 0;
//#endif
}

void DefineEstimatorComponents(FOSLSProblem * problem, int fosls_func_version, std::vector<std::pair<int,int> >& grfuns_descriptor,
                          Array<ParGridFunction*>& extra_grfuns, Array2D<BilinearFormIntegrator *> & integs, bool verbose)
{
    int numfoslsfuns = -1;

    if (verbose)
        std::cout << "fosls_func_version = " << fosls_func_version << "\n";

    if (fosls_func_version == 1)
        numfoslsfuns = 2;
    else if (fosls_func_version == 2)
        numfoslsfuns = 3;

    // extra_grfuns.SetSize(0); // must come by default
    if (fosls_func_version == 2)
        extra_grfuns.SetSize(1);

    grfuns_descriptor.resize(numfoslsfuns);

    integs.SetSize(numfoslsfuns, numfoslsfuns);
    for (int i = 0; i < integs.NumRows(); ++i)
        for (int j = 0; j < integs.NumCols(); ++j)
            integs(i,j) = NULL;

    Hyper_test* Mytest = dynamic_cast<Hyper_test*>
            (problem->GetFEformulation().GetFormulation()->GetTest());
    MFEM_ASSERT(Mytest, "Unsuccessful cast into Hyper_test* \n");

    const Array<SpaceName>* space_names_funct = problem->GetFEformulation().GetFormulation()->
            GetFunctSpacesDescriptor();

    // version 1, only || sigma - b S ||^2, or || K sigma ||^2
    if (fosls_func_version == 1)
    {
        // this works
        grfuns_descriptor[0] = std::make_pair<int,int>(1, 0);
        grfuns_descriptor[1] = std::make_pair<int,int>(1, 1);

        if ( (*space_names_funct)[0] == SpaceName::HDIV) // sigma is from Hdiv
            integs(0,0) = new VectorFEMassIntegrator;
        else // sigma is from H1vec
            integs(0,0) = new ImproperVectorMassIntegrator;

        integs(1,1) = new MassIntegrator(*Mytest->GetBtB());

        if ( (*space_names_funct)[0] == SpaceName::HDIV) // sigma is from Hdiv
            integs(1,0) = new VectorFEMassIntegrator(*Mytest->GetMinB());
        else // sigma is from H1
            integs(1,0) = new MixedVectorScalarIntegrator(*Mytest->GetMinB());
    }
    else if (fosls_func_version == 2)
    {
        // version 2, only || sigma - b S ||^2 + || div bS - f ||^2
        MFEM_ASSERT(problem->GetFEformulation().Nunknowns() == 2 && (*space_names_funct)[1] == SpaceName::H1,
                "Version 2 works only if S is from H1 \n");

        // this works
        grfuns_descriptor[0] = std::make_pair<int,int>(1, 0);
        grfuns_descriptor[1] = std::make_pair<int,int>(1, 1);
        grfuns_descriptor[2] = std::make_pair<int,int>(-1, 0);

        int numblocks = problem->GetFEformulation().Nblocks();

        extra_grfuns[0] = new ParGridFunction(problem->GetPfes(numblocks - 1));
        extra_grfuns[0]->ProjectCoefficient(*problem->GetFEformulation().GetFormulation()->GetTest()->GetRhs());

        if ( (*space_names_funct)[0] == SpaceName::HDIV) // sigma is from Hdiv
            integs(0,0) = new VectorFEMassIntegrator;
        else // sigma is from H1vec
            integs(0,0) = new ImproperVectorMassIntegrator;

        integs(1,1) = new H1NormIntegrator(*Mytest->GetBBt(), *Mytest->GetBtB());

        integs(1,0) = new VectorFEMassIntegrator(*Mytest->GetMinB());

        // integrators related to f (rhs side)
        integs(2,2) = new MassIntegrator;
        integs(1,2) = new MixedDirectionalDerivativeIntegrator(*Mytest->GetMinB());
    }
    else
    {
        MFEM_ABORT("Unsupported version of fosls functional \n");
    }
}





