///                           MFEM(with 4D elements) CFOSLS for 3D/4D transport equation
///                       solved by geometric multigrid preconditioner in div-free setting
///                                   and also by a minimization solver (old interfaces)
///
/// ARCHIVED EXAMPLE
/// This code shows the old way of constructing multigrid preconditioners from explicitly built
/// hierarchy of meshes and f.e. spaces. Look in cfosls_hyperbolic_multigrid.cpp example
/// for a much cleaner way exploiting tools available from cfosls/
///
/// The problem considered in this example is
///                             du/dt + b * u = f (either 3D or 4D in space-time)
/// casted in the CFOSLS formulation
/// 1) either in Hdiv-L2 case:
///                             (K sigma, sigma) -> min
/// where sigma is from H(div), u is recovered (as an element of L^2) from sigma = b * u,
/// and K = (I - bbT / || b ||);
/// 2) or in Hdiv-H1-L2 case
///                             || sigma - b * u || ^2 -> min
/// where sigma is from H(div) and u is from H^1;
/// minimizing in all cases under the constraint
///                             div sigma = f.
///
/// The problem is discretized using RT, linear Lagrange and discontinuous constants in 3D/4D.
/// The current 3D tests are either in cube.
///
/// The problem is then solved by two different multigrid setups:
/// 1) In the div-free formulation, where we first find a particular solution to the
/// divergence constraint, and then search for the div-free correction, casting the system's
/// first component into H(curl) (in 3D).
/// Then, to find the div-free correction, CG is used, preconditioned by a geometric multigrid.
/// 2) With a minimization solver, where we first find a particular solution to the
/// divergence constraint and then minimize the functional over the correction subspace.
/// Unlike 1), we don't cast the problem explicitly into H(curl), but iterations of the
/// minimization solver keep the vectors in the corresponding subspace where first block component
/// is from H(div) and satisfies the prescribed divergence constraint.
///
/// (*) The code shows how multigrid preconditioners were constructed previously, without classes
/// from cfosls/. Hence it's big and has multiple blocks which do the same thing using different interfaces.
/// There was an attempt to modify this example, but eventually the code was fully rewritten in
/// cfosls_hyperbolic_multigrid.cpp.
///
/// (**) This code was tested in serial and in parallel for the report on CFOSLS.
///
/// (***) The example was tested for memory leaks with valgrind, in 3D, was neither cleaned
/// nor properly commented.
///
/// Typical run of this example: ./cfosls_hyperbolic_newsolver --whichD 3 --spaceS L2 -no-vis
/// If you want to use the Hdiv-H1-L2 formulation, you will need not only change --spaceS option but also
/// change the source code.
///
/// Other examples with geometric multigrid are cfosls_hyperbolic_anisoMG.cpp (look there
/// for a cleaner example with geometric MG only, using the newest interfaces) and
/// cfosls_hyperbolic_multigrid.cpp (recommended).

#include "mfem.hpp"
#include <fstream>
#include <iostream>
#include <memory>
#include <iomanip>
#include <list>
#include <unistd.h>

//#define NEW_INTERFACE
//#define NEW_INTERFACE2
// If you comment this, Coeus cluster cannot compile the example unlike my workstation
#define BRANDNEW_INTERFACE

// (de)activates solving of the discrete global problem
#define OLD_CODE

#define WITH_DIVCONSTRAINT_SOLVER

// switches on/off usage of smoother in the new minimization solver
// in parallel GS smoother works a little bit different from serial
#define WITH_SMOOTHERS

// activates a check for the symmetry of the new smoother setup
//#define CHECK_SPDSMOOTHER

// activates using the new interface to local problem solvers
// via a separated class called LocalProblemSolver
#define SOLVE_WITH_LOCALSOLVERS

// activates a test where new solver is used as a preconditioner
#define USE_AS_A_PREC

#define HCURL_COARSESOLVER

//#define CHECK_SPDCOARSESTSOLVER

// activates a check for the symmetry of the new solver
//#define CHECK_SPDSOLVER

// activates constraint residual check after each iteration of the minimization solver
#define CHECK_CONSTR

#define CHECK_BNDCND

//#define SPECIAL_COARSECHECK

//#define COMPARE_MG

#define BND_FOR_MULTIGRID

//#define COARSEPREC_AMS

#ifdef COMPARE_MG // options for multigrid, specific for detailed comparison of mg

#define NCOARSEITER 4

#define NO_COARSESOLVE
//#define NO_POSTSMOOTH
//#define NO_PRESMOOTH

//#define COMPARE_COARSE_SOLVERS
//#define COMPARE_SMOOTHERS
#endif // for ifdef COMPARE_MG

// activates more detailed timing of the new multigrid code
//#define TIMING

#ifdef TIMING
#undef CHECK_LOCALSOLVE
#undef CHECK_CONSTR
#undef CHECK_BNDCND
#endif

#define MYZEROTOL (1.0e-13)

//#define WITH_PENALTY

//#define ONLY_DIVFREEPART

//#define K_IDENTITY

using namespace std;
using namespace mfem;
using std::unique_ptr;
using std::shared_ptr;
using std::make_shared;

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
    int numsol          = 4;
    int numcurl         = 0;

    int ser_ref_levels  = 0;
    int par_ref_levels  = 1;

    const char *space_for_S = "L2";    // "H1" or "L2"

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
    using ProblemType = FOSLSProblem_HdivL2hyp;

    bool eliminateS = true;            // in case space_for_S = "L2" defines whether we eliminate S from the system

    bool aniso_refine = false;
    bool refine_t_first = false;

    bool with_multilevel = true;
    bool monolithicMG = false;

    bool useM_in_divpart = true;

    // solver options
    int prec_option = 3;        // defines whether to use preconditioner or not, and which one
    bool prec_is_MG;

    //const char *mesh_file = "../data/cube_3d_fine.mesh";
    const char *mesh_file = "../data/cube_3d_moderate.mesh";
    //const char *mesh_file = "../data/square_2d_moderate.mesh";

    //const char *mesh_file = "../data/cube4d_low.MFEM";
    //const char *mesh_file = "../data/cube4d_96.MFEM";
    //const char *mesh_file = "dsadsad";
    //const char *mesh_file = "../data/orthotope3D_moderate.mesh";
    //const char * mesh_file = "../data/orthotope3D_fine.mesh";

    int feorder         = 0;

    if (verbose)
        cout << "Solving CFOSLS Transport equation with MFEM & hypre, div-free approach, minimization solver \n";

    OptionsParser args(argc, argv);
    args.AddOption(&mesh_file, "-m", "--mesh",
                   "Mesh file to use.");
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
    args.AddOption(&eliminateS, "-elims", "--eliminateS", "-no-elims",
                   "--no-eliminateS",
                   "Turn on/off elimination of S in L2 formulation.");
    args.AddOption(&prec_option, "-precopt", "--prec-option",
                   "Preconditioner choice.");
    args.AddOption(&with_multilevel, "-ml", "--multilvl", "-no-ml",
                   "--no-multilvl",
                   "Enable or disable multilevel algorithm for finding a particular solution.");
    args.AddOption(&useM_in_divpart, "-useM", "--useM", "-no-useM", "--no-useM",
                   "Whether to use M to compute a partilar solution");
    args.AddOption(&aniso_refine, "-aniso", "--aniso-refine", "-iso",
                   "--iso-refine",
                   "Using anisotropic or isotropic refinement.");
    args.AddOption(&refine_t_first, "-refine-t-first", "--refine-time-first",
                   "-refine-x-first", "--refine-space-first",
                   "Refine time or space first in anisotropic refinement.");
    args.AddOption(&space_for_S, "-spaceS", "--spaceS",
                   "Space for S: L2 or H1.");
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

#ifdef WITH_SMOOTHERS
    if (verbose)
        std::cout << "WITH_SMOOTHERS active \n";
#else
    if (verbose)
        std::cout << "WITH_SMOOTHERS passive \n";
#endif

#ifdef SOLVE_WITH_LOCALSOLVERS
    if (verbose)
        std::cout << "SOLVE_WITH_LOCALSOLVERS active \n";
#else
    if (verbose)
        std::cout << "SOLVE_WITH_LOCALSOLVERS passive \n";
#endif

#ifdef HCURL_COARSESOLVER
    if (verbose)
        std::cout << "HCURL_COARSESOLVER active \n";
#else
    if (verbose)
        std::cout << "HCURL_COARSESOLVER passive \n";
#endif

#ifdef USE_AS_A_PREC
    if (verbose)
        std::cout << "USE_AS_A_PREC active \n";
#else
    if (verbose)
        std::cout << "USE_AS_A_PREC passive \n";
#endif

#ifdef OLD_CODE
    if (verbose)
        std::cout << "OLD_CODE active \n";
#else
    if (verbose)
        std::cout << "OLD_CODE passive \n";
#endif
#ifdef TIMING
    if (verbose)
        std::cout << "TIMING active \n";
#else
    if (verbose)
        std::cout << "TIMING passive \n";
#endif
#ifdef CHECK_BNDCND
    if (verbose)
        std::cout << "CHECK_BNDCND active \n";
#else
    if (verbose)
        std::cout << "CHECK_BNDCND passive \n";
#endif
#ifdef CHECK_CONSTR
    if (verbose)
        std::cout << "CHECK_CONSTR active \n";
#else
    if (verbose)
        std::cout << "CHECK_CONSTR passive \n";
#endif

#ifdef BND_FOR_MULTIGRID
    if (verbose)
        std::cout << "BND_FOR_MULTIGRID active \n";
#else
    if (verbose)
        std::cout << "BND_FOR_MULTIGRID passive \n";
#endif

#ifdef BLKDIAG_SMOOTHER
    if (verbose)
        std::cout << "BLKDIAG_SMOOTHER active \n";
#else
    if (verbose)
        std::cout << "BLKDIAG_SMOOTHER passive \n";
#endif

#ifdef COARSEPREC_AMS
    if (verbose)
        std::cout << "COARSEPREC_AMS active \n";
#else
    if (verbose)
        std::cout << "COARSEPREC_AMS passive \n";
#endif

#ifdef COMPARE_MG
    if (verbose)
        std::cout << "COMPARE_MG active \n";
#else
    if (verbose)
        std::cout << "COMPARE_MG passive \n";
#endif

#ifdef WITH_PENALTY
    if (verbose)
        std::cout << "WITH_PENALTY active \n";
#else
    if (verbose)
        std::cout << "WITH_PENALTY passive \n";
#endif

#ifdef K_IDENTITY
    if (verbose)
        std::cout << "K_IDENTITY active \n";
#else
    if (verbose)
        std::cout << "K_IDENTITY passive \n";
#endif

    std::cout << std::flush;
    MPI_Barrier(MPI_COMM_WORLD);

    MFEM_ASSERT(strcmp(space_for_S,"H1") == 0 || strcmp(space_for_S,"L2") == 0, "Space for S must be H1 or L2!\n");
    MFEM_ASSERT(!(strcmp(space_for_S,"L2") == 0 && !eliminateS), "Case: L2 space for S and S is not eliminated is working incorrectly, non pos.def. matrix. \n");

    if (verbose)
    {
        if (strcmp(space_for_S,"H1") == 0)
            std::cout << "Space for S: H1 \n";
        else
            std::cout << "Space for S: L2 \n";

        if (strcmp(space_for_S,"L2") == 0)
        {
            std::cout << "S is ";
            if (!eliminateS)
                std::cout << "not ";
            std::cout << "eliminated from the system \n";
        }
    }

    if (verbose)
        std::cout << "Running tests for the paper: \n";

    if (nDimensions == 3)
    {
        numsol = -3;
        mesh_file = "../data/cube_3d_moderate.mesh";
    }
    else // 4D case
    {
        numsol = -4;
        mesh_file = "../data/cube4d_96.MFEM";
    }

    if (verbose)
        std::cout << "For the records: numsol = " << numsol
                  << ", mesh_file = " << mesh_file << "\n";

    Hyper_test Mytest(nDimensions, numsol);

    if (verbose)
        cout << "Number of mpi processes: " << num_procs << endl << flush;

    bool with_prec;

    switch (prec_option)
    {
    case 1: // smth simple like AMS
        with_prec = true;
        prec_is_MG = false;
        break;
    case 2: // MG
        with_prec = true;
        prec_is_MG = true;
        monolithicMG = false;
        break;
    case 3: // block MG
        with_prec = true;
        prec_is_MG = true;
        monolithicMG = true;
        break;
    default: // no preconditioner (default)
        with_prec = false;
        prec_is_MG = false;
        monolithicMG = false;
        break;
    }

    if (verbose)
    {
        cout << "with_prec = " << with_prec << endl;
        cout << "prec_is_MG = " << prec_is_MG << endl;
        cout << flush;
    }

    StopWatch chrono;
    StopWatch chrono_total;

    chrono_total.Clear();
    chrono_total.Start();

    //DEFAULTED LINEAR SOLVER OPTIONS
    int max_num_iter = 2000;
    double rtol = 1e-12;//1e-7;//1e-9;
    double atol = 1e-14;//1e-9;//1e-12;

    Mesh *mesh = NULL;

    shared_ptr<ParMesh> pmesh;

    if (nDimensions == 3 || nDimensions == 4)
    {
        if (aniso_refine)
        {
            if (verbose)
                std::cout << "Anisotropic refinement is ON \n";
            if (nDimensions == 3)
            {
                if (verbose)
                    std::cout << "Using hexahedral mesh in 3D for anisotr. refinement code \n";
                mesh = new Mesh(2, 2, 2, Element::HEXAHEDRON, 1);
            }
            else // dim == 4
            {
                if (verbose)
                    cerr << "Anisotr. refinement is not implemented in 4D case with tesseracts \n" << std::flush;
                MPI_Finalize();
                return -1;
            }
        }
        else // no anisotropic refinement
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
    }
    else //if nDimensions is not 3 or 4
    {
        if (verbose)
            cerr << "Case nDimensions = " << nDimensions << " is not supported \n" << std::flush;
        MPI_Finalize();
        return -1;
    }

    if (mesh) // if only serial mesh was generated previously, parallel mesh is initialized here
    {
        if (aniso_refine)
        {
            // for anisotropic refinement, the serial mesh needs at least one
            // serial refine to turn the mesh into a nonconforming mesh
            MFEM_ASSERT(ser_ref_levels > 0, "need ser_ref_levels > 0 for aniso_refine");

            for (int l = 0; l < ser_ref_levels-1; l++)
                mesh->UniformRefinement();

            Array<Refinement> refs(mesh->GetNE());
            for (int i = 0; i < mesh->GetNE(); i++)
            {
                refs[i] = Refinement(i, 7);
            }
            mesh->GeneralRefinement(refs, -1, -1);

            par_ref_levels *= 2;
        }
        else
        {
            for (int l = 0; l < ser_ref_levels; l++)
                mesh->UniformRefinement();
        }

        if (verbose)
            cout << "Creating parmesh(" << nDimensions <<
                    "d) from the serial mesh (" << nDimensions << "d)" << endl << flush;
        pmesh = make_shared<ParMesh>(comm, *mesh);
        delete mesh;
    }

#ifdef WITH_PENALTY
    if (verbose)
        std::cout << "regularization is ON \n";
    double h_min, h_max, kappa_min, kappa_max;
    pmesh->GetCharacteristics(h_min, h_max, kappa_min, kappa_max);
    if (verbose)
        std::cout << "coarse mesh steps: min " << h_min << " max " << h_max << "\n";

    double reg_param;
    reg_param = 1.0 * h_min * h_min;
    reg_param *= 1.0 / (pow(2.0, par_ref_levels) * pow(2.0, par_ref_levels));
    if (verbose)
        std::cout << "regularization parameter: " << reg_param << "\n";
    ConstantCoefficient reg_coeff(reg_param);
#endif


    MFEM_ASSERT(!(aniso_refine && (with_multilevel || nDimensions == 4)),"Anisotropic refinement works only in 3D and without multilevel algorithm \n");

    ////////////////////////////////// new

    int dim = nDimensions;

    Array<int> ess_bdrSigma(pmesh->bdr_attributes.Max());
    ess_bdrSigma = 0;
    if (strcmp(space_for_S,"L2") == 0) // S is from L2, so we impose bdr condition for sigma at t = 0
    {
        ess_bdrSigma[0] = 1;
        //ess_bdrSigma = 1;
        //ess_bdrSigma[pmesh->bdr_attributes.Max()-1] = 0;
    }

    Array<int> ess_bdrS(pmesh->bdr_attributes.Max());
    ess_bdrS = 0;
    if (strcmp(space_for_S,"H1") == 0) // S is from H1
    {
        ess_bdrS[0] = 1; // t = 0
        //ess_bdrS = 1;
    }

    Array<int> all_bdrSigma(pmesh->bdr_attributes.Max());
    all_bdrSigma = 1;

    Array<int> all_bdrS(pmesh->bdr_attributes.Max());
    all_bdrS = 1;

    int ref_levels = par_ref_levels;

    int num_levels = ref_levels + 1;

    chrono.Clear();
    chrono.Start();

    Array<ParMesh*> pmesh_lvls(num_levels);
    Array<ParFiniteElementSpace*> R_space_lvls(num_levels);
    Array<ParFiniteElementSpace*> W_space_lvls(num_levels);
    Array<ParFiniteElementSpace*> C_space_lvls(num_levels);
    Array<ParFiniteElementSpace*> H_space_lvls(num_levels);

    FiniteElementCollection *hdiv_coll;
    ParFiniteElementSpace *R_space;
    FiniteElementCollection *l2_coll;
    ParFiniteElementSpace *W_space;

    if (dim == 4)
        hdiv_coll = new RT0_4DFECollection;
    else
        hdiv_coll = new RT_FECollection(feorder, dim);

    R_space = new ParFiniteElementSpace(pmesh.get(), hdiv_coll);

    l2_coll = new L2_FECollection(feorder, nDimensions);
    W_space = new ParFiniteElementSpace(pmesh.get(), l2_coll);

    FiniteElementCollection *hdivfree_coll;
    ParFiniteElementSpace *C_space;

    if (dim == 3)
        hdivfree_coll = new ND_FECollection(feorder + 1, nDimensions);
    else // dim == 4
        hdivfree_coll = new DivSkew1_4DFECollection;

    C_space = new ParFiniteElementSpace(pmesh.get(), hdivfree_coll);

    FiniteElementCollection *h1_coll;
    ParFiniteElementSpace *H_space;
    if (dim == 3)
        h1_coll = new H1_FECollection(feorder+1, nDimensions);
    else
    {
        if (feorder + 1 == 1)
            h1_coll = new LinearFECollection;
        else if (feorder + 1 == 2)
        {
            if (verbose)
                std::cout << "We have Quadratic FE for H1 in 4D, but are you sure? \n";
            h1_coll = new QuadraticFECollection;
        }
        else
            MFEM_ABORT("Higher-order H1 elements are not implemented in 4D \n");
    }
    H_space = new ParFiniteElementSpace(pmesh.get(), h1_coll);

#ifdef OLD_CODE
    ParFiniteElementSpace * S_space;
    if (strcmp(space_for_S,"H1") == 0)
        S_space = H_space;
    else // "L2"
        S_space = W_space;
#endif

    // For geometric multigrid
    Array<HypreParMatrix*> TrueP_C(par_ref_levels);
    Array<HypreParMatrix*> TrueP_H(par_ref_levels);

    Array<HypreParMatrix*> TrueP_R(par_ref_levels);

    Array< SparseMatrix*> P_W(ref_levels);
    Array< SparseMatrix*> P_R(ref_levels);
    //Array< SparseMatrix*> P_H(ref_levels);
    Array< SparseMatrix*> Element_dofs_R(ref_levels);
    Array< SparseMatrix*> Element_dofs_H(ref_levels);
    Array< SparseMatrix*> Element_dofs_W(ref_levels);

    const SparseMatrix* P_W_local;
    const SparseMatrix* P_R_local;
    const SparseMatrix* P_H_local;

    Array<SparseMatrix*> Mass_mat_lvls(num_levels);

    DivPart divp;

    int numblocks_funct = 1;
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
        numblocks_funct++;
    std::vector<std::vector<Array<int>* > > BdrDofs_Funct_lvls(num_levels, std::vector<Array<int>* >(numblocks_funct));
    std::vector<std::vector<Array<int>* > > EssBdrDofs_Funct_lvls(num_levels, std::vector<Array<int>* >(numblocks_funct));
    std::vector<std::vector<Array<int>* > > EssBdrTrueDofs_Funct_lvls(num_levels, std::vector<Array<int>* >(numblocks_funct));

#ifdef OLD_CODE
    std::vector<std::vector<Array<int>* > > EssBdrTrueDofs_HcurlFunct_lvls(num_levels, std::vector<Array<int>* >(numblocks_funct));
#endif

    Array< SparseMatrix* > P_C_lvls(num_levels - 1);
    Array<HypreParMatrix* > Dof_TrueDof_Hcurl_lvls(num_levels);
    std::vector<Array<int>* > EssBdrDofs_Hcurl(num_levels);
    std::vector<Array<int>* > EssBdrTrueDofs_Hcurl(num_levels);
    std::vector<Array<int>* > EssBdrTrueDofs_H1(num_levels);

    std::vector<Array<int>* > EssBdrDofs_H1(num_levels);
    Array< SparseMatrix* > P_H_lvls(num_levels - 1);
    Array<HypreParMatrix* > Dof_TrueDof_H1_lvls(num_levels);
    Array<HypreParMatrix* > Dof_TrueDof_Hdiv_lvls(num_levels);

    std::vector<std::vector<HypreParMatrix*> > Dof_TrueDof_Func_lvls(num_levels);
    std::vector<HypreParMatrix*> Dof_TrueDof_L2_lvls(num_levels);

    Array<SparseMatrix*> Divfree_mat_lvls(num_levels);
    std::vector<Array<int>*> Funct_mat_offsets_lvls(num_levels);
    Array<BlockMatrix*> Funct_mat_lvls(num_levels);
    Array<SparseMatrix*> Constraint_mat_lvls(num_levels);

    Array<HypreParMatrix*> Divfree_hpmat_mod_lvls(num_levels);
    std::vector<Array2D<HypreParMatrix*> *> Funct_hpmat_lvls(num_levels);

    BlockOperator* Funct_global;
    std::vector<Operator*> Funct_global_lvls(num_levels);
    BlockVector* Functrhs_global;
    Array<int> offsets_global(numblocks_funct + 1);

   for (int l = 0; l < num_levels; ++l)
   {
       Dof_TrueDof_Func_lvls[l].resize(numblocks_funct);
       BdrDofs_Funct_lvls[l][0] = new Array<int>;
       EssBdrDofs_Funct_lvls[l][0] = new Array<int>;
       EssBdrTrueDofs_Funct_lvls[l][0] = new Array<int>;
#ifdef OLD_CODE
       EssBdrTrueDofs_HcurlFunct_lvls[l][0] = new Array<int>;
#endif
       EssBdrDofs_Hcurl[l] = new Array<int>;
       EssBdrTrueDofs_Hcurl[l] = new Array<int>;
       if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
           EssBdrTrueDofs_H1[l] = new Array<int>;

       Funct_mat_offsets_lvls[l] = new Array<int>;
       if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
       {
           BdrDofs_Funct_lvls[l][1] = new Array<int>;
           EssBdrDofs_Funct_lvls[l][1] = new Array<int>;
           EssBdrTrueDofs_Funct_lvls[l][1] = new Array<int>;
#ifdef OLD_CODE
           EssBdrTrueDofs_HcurlFunct_lvls[l][1] = new Array<int>;
#endif
           EssBdrDofs_H1[l] = new Array<int>;
       }

       Funct_hpmat_lvls[l] = new Array2D<HypreParMatrix*>(numblocks_funct, numblocks_funct);
   }

   const SparseMatrix* P_C_local;

   //Actually this and LocalSolver_partfinder_lvls handle the same objects
   Array<Operator*>* LocalSolver_lvls;
   LocalSolver_lvls = new Array<Operator*>(num_levels - 1);

   Array<LocalProblemSolver*>* LocalSolver_partfinder_lvls;
   LocalSolver_partfinder_lvls = new Array<LocalProblemSolver*>(num_levels - 1);

   Array<Operator*> Smoothers_lvls(num_levels - 1);

   Operator* CoarsestSolver;
   CoarsestProblemSolver* CoarsestSolver_partfinder;

   Array<BlockMatrix*> Element_dofs_Func(num_levels - 1);
   std::vector<Array<int>*> row_offsets_El_dofs(num_levels - 1);
   std::vector<Array<int>*> col_offsets_El_dofs(num_levels - 1);

   Array<BlockMatrix*> P_Func(ref_levels);
   std::vector<Array<int>*> row_offsets_P_Func(num_levels - 1);
   std::vector<Array<int>*> col_offsets_P_Func(num_levels - 1);

   Array<BlockOperator*> TrueP_Func(ref_levels);
   std::vector<Array<int>*> row_offsets_TrueP_Func(num_levels - 1);
   std::vector<Array<int>*> col_offsets_TrueP_Func(num_levels - 1);

   for (int l = 0; l < num_levels; ++l)
       if (l < num_levels - 1)
       {
           row_offsets_El_dofs[l] = new Array<int>(numblocks_funct + 1);
           col_offsets_El_dofs[l] = new Array<int>(numblocks_funct + 1);
           row_offsets_P_Func[l] = new Array<int>(numblocks_funct + 1);
           col_offsets_P_Func[l] = new Array<int>(numblocks_funct + 1);
           row_offsets_TrueP_Func[l] = new Array<int>(numblocks_funct + 1);
           col_offsets_TrueP_Func[l] = new Array<int>(numblocks_funct + 1);
       }

   Array<SparseMatrix*> P_WT(num_levels - 1); //AE_e matrices

    chrono.Clear();
    chrono.Start();

    if (verbose)
        std::cout << "Creating a hierarchy of meshes by successive refinements "
                     "(with multilevel and multigrid prerequisites) \n";

    for (int l = num_levels - 1; l >= 0; --l)
    {
        // creating pmesh for level l
        if (l == num_levels - 1)
        {
            pmesh_lvls[l] = new ParMesh(*pmesh);
            //pmesh_lvls[l] = pmesh.get();
        }
        else
        {
            if (aniso_refine && refine_t_first)
            {
                Array<Refinement> refs(pmesh->GetNE());
                if (l < par_ref_levels/2+1)
                {
                    for (int i = 0; i < pmesh->GetNE(); i++)
                        refs[i] = Refinement(i, 4);
                }
                else
                {
                    for (int i = 0; i < pmesh->GetNE(); i++)
                        refs[i] = Refinement(i, 3);
                }
                pmesh->GeneralRefinement(refs, -1, -1);
            }
            else if (aniso_refine && !refine_t_first)
            {
                Array<Refinement> refs(pmesh->GetNE());
                if (l < par_ref_levels/2+1)
                {
                    for (int i = 0; i < pmesh->GetNE(); i++)
                        refs[i] = Refinement(i, 3);
                }
                else
                {
                    for (int i = 0; i < pmesh->GetNE(); i++)
                        refs[i] = Refinement(i, 4);
                }
                pmesh->GeneralRefinement(refs, -1, -1);
            }
            else
            {
                pmesh->UniformRefinement();
            }

            pmesh_lvls[l] = new ParMesh(*pmesh);
        }

        // creating pfespaces for level l
        R_space_lvls[l] = new ParFiniteElementSpace(pmesh_lvls[l], hdiv_coll);
        W_space_lvls[l] = new ParFiniteElementSpace(pmesh_lvls[l], l2_coll);
        C_space_lvls[l] = new ParFiniteElementSpace(pmesh_lvls[l], hdivfree_coll);
        if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
            H_space_lvls[l] = new ParFiniteElementSpace(pmesh_lvls[l], h1_coll);

        // getting boundary and essential boundary dofs
        R_space_lvls[l]->GetEssentialVDofs(all_bdrSigma, *BdrDofs_Funct_lvls[l][0]);
        R_space_lvls[l]->GetEssentialVDofs(ess_bdrSigma, *EssBdrDofs_Funct_lvls[l][0]);
        R_space_lvls[l]->GetEssentialTrueDofs(ess_bdrSigma, *EssBdrTrueDofs_Funct_lvls[l][0]);
        C_space_lvls[l]->GetEssentialVDofs(ess_bdrSigma, *EssBdrDofs_Hcurl[l]);
        C_space_lvls[l]->GetEssentialTrueDofs(ess_bdrSigma, *EssBdrTrueDofs_Hcurl[l]);
#ifdef OLD_CODE
        C_space_lvls[l]->GetEssentialTrueDofs(ess_bdrSigma, *EssBdrTrueDofs_HcurlFunct_lvls[l][0]);
#endif
        if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
            H_space_lvls[l]->GetEssentialTrueDofs(ess_bdrS, *EssBdrTrueDofs_H1[l]);

        if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
        {
            H_space_lvls[l]->GetEssentialVDofs(all_bdrS, *BdrDofs_Funct_lvls[l][1]);
            H_space_lvls[l]->GetEssentialVDofs(ess_bdrS, *EssBdrDofs_Funct_lvls[l][1]);
            H_space_lvls[l]->GetEssentialTrueDofs(ess_bdrS, *EssBdrTrueDofs_Funct_lvls[l][1]);
#ifdef OLD_CODE
            H_space_lvls[l]->GetEssentialTrueDofs(ess_bdrS, *EssBdrTrueDofs_HcurlFunct_lvls[l][1]);
#endif
            H_space_lvls[l]->GetEssentialVDofs(ess_bdrS, *EssBdrDofs_H1[l]);
        }

        ParBilinearForm mass_form(W_space_lvls[l]);
        mass_form.AddDomainIntegrator(new MassIntegrator);
        mass_form.Assemble();
        mass_form.Finalize();
        Mass_mat_lvls[l] = mass_form.LoseMat();

        // getting operators at level l
        // curl or divskew operator from C_space into R_space
        ParDiscreteLinearOperator Divfree_op(C_space_lvls[l], R_space_lvls[l]); // from Hcurl or HDivSkew(C_space) to Hdiv(R_space)
        if (dim == 3)
            Divfree_op.AddDomainInterpolator(new CurlInterpolator);
        else // dim == 4
            Divfree_op.AddDomainInterpolator(new DivSkewInterpolator);
        Divfree_op.Assemble();
        Divfree_op.Finalize();
        Divfree_mat_lvls[l] = Divfree_op.LoseMat();

        ParBilinearForm *Ablock(new ParBilinearForm(R_space_lvls[l]));
        //Ablock->AddDomainIntegrator(new VectorFEMassIntegrator);
        if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
            Ablock->AddDomainIntegrator(new VectorFEMassIntegrator);
        else
            Ablock->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.GetKtilda()));
#ifdef WITH_PENALTY
        Ablock->AddDomainIntegrator(new VectorFEMassIntegrator(reg_coeff));
#endif
        Ablock->Assemble();
        Ablock->EliminateEssentialBC(ess_bdrSigma);//, *sigma_exact_finest, *fform); // makes res for sigma_special happier
        Ablock->Finalize();

        // getting pointers to dof_truedof matrices
        Dof_TrueDof_Hcurl_lvls[l] = C_space_lvls[l]->Dof_TrueDof_Matrix();
        Dof_TrueDof_Func_lvls[l][0] = R_space_lvls[l]->Dof_TrueDof_Matrix();
        Dof_TrueDof_Hdiv_lvls[l] = Dof_TrueDof_Func_lvls[l][0];
        Dof_TrueDof_L2_lvls[l] = W_space_lvls[l]->Dof_TrueDof_Matrix();
        if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
        {
            Dof_TrueDof_H1_lvls[l] = H_space_lvls[l]->Dof_TrueDof_Matrix();
            Dof_TrueDof_Func_lvls[l][1] = Dof_TrueDof_H1_lvls[l];
        }

        if (l == 0)
        {
            ParBilinearForm *Cblock;
            ParMixedBilinearForm *BTblock;
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
            {
                MFEM_ASSERT(strcmp(space_for_S,"H1") == 0, "Case when S is from L2 but is not"
                                                           " eliminated is not supported currently! \n");

                // diagonal block for H^1
                Cblock = new ParBilinearForm(H_space_lvls[l]);
                Cblock->AddDomainIntegrator(new MassIntegrator(*Mytest.GetBtB()));
                Cblock->AddDomainIntegrator(new DiffusionIntegrator(*Mytest.GetBBt()));
                Cblock->Assemble();
                // FIXME: What about boundary conditons here?
                //Cblock->EliminateEssentialBC(ess_bdrS, xblks.GetBlock(1),*qform);
                Cblock->Finalize();

                // off-diagonal block for (H(div), Space_for_S) block
                // you need to create a new integrator here to swap the spaces
                BTblock = new ParMixedBilinearForm(R_space_lvls[l], H_space_lvls[l]);
                BTblock->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.GetMinB()));
                BTblock->Assemble();
                // FIXME: What about boundary conditons here?
                //BTblock->EliminateTrialDofs(ess_bdrSigma, *sigma_exact, *qform);
                //BTblock->EliminateTestDofs(ess_bdrS);
                BTblock->Finalize();
            }

            Funct_mat_offsets_lvls[l]->SetSize(numblocks_funct + 1);
            (*Funct_mat_offsets_lvls[l])[0] = 0;
            (*Funct_mat_offsets_lvls[l])[1] = Ablock->Height();
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
                (*Funct_mat_offsets_lvls[l])[2] = Cblock->Height();
            Funct_mat_offsets_lvls[l]->PartialSum();

            Funct_mat_lvls[l] = new BlockMatrix(*Funct_mat_offsets_lvls[l]);
            Funct_mat_lvls[l]->SetBlock(0,0,Ablock->LoseMat());
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
            {
                Funct_mat_lvls[l]->SetBlock(1,1,Cblock->LoseMat());
                Funct_mat_lvls[l]->SetBlock(1,0,BTblock->LoseMat());
                Funct_mat_lvls[l]->SetBlock(0,1,Transpose(Funct_mat_lvls[l]->GetBlock(1,0)));
            }

            ParMixedBilinearForm *Bblock = new ParMixedBilinearForm(R_space_lvls[l], W_space_lvls[l]);
            Bblock->AddDomainIntegrator(new VectorFEDivergenceIntegrator);
            Bblock->Assemble();
            Bblock->Finalize();
            Constraint_mat_lvls[l] = Bblock->LoseMat();

            // Creating global functional matrix
            offsets_global[0] = 0;
            for ( int blk = 0; blk < numblocks_funct; ++blk)
                offsets_global[blk + 1] = Dof_TrueDof_Func_lvls[l][blk]->Width();
            offsets_global.PartialSum();

            Funct_global = new BlockOperator(offsets_global);

            Functrhs_global = new BlockVector(offsets_global);

            Functrhs_global->GetBlock(0) = 0.0;

            ParLinearForm *secondeqn_rhs;
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS)
            {
                secondeqn_rhs = new ParLinearForm(H_space_lvls[l]);
                secondeqn_rhs->AddDomainIntegrator(new GradDomainLFIntegrator(*Mytest.GetBf()));
                secondeqn_rhs->Assemble();

                secondeqn_rhs->ParallelAssemble(Functrhs_global->GetBlock(1));
                for (int tdofind = 0; tdofind < EssBdrDofs_Funct_lvls[0][1]->Size(); ++tdofind)
                {
                    int tdof = (*EssBdrDofs_Funct_lvls[0][1])[tdofind];
                    Functrhs_global->GetBlock(1)[tdof] = 0.0;
                }
            }

            Ablock->Assemble();
            Ablock->EliminateEssentialBC(ess_bdrSigma);//, *sigma_exact_finest, *fform); // makes res for sigma_special happier
            Ablock->Finalize();
            Funct_global->SetBlock(0,0, Ablock->ParallelAssemble());

            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
            {
                Cblock->Assemble();
                {
                    Vector temp1(Cblock->Width());
                    temp1 = 0.0;
                    Vector temp2(Cblock->Height());
                    temp2 = 0.0;
                    Cblock->EliminateEssentialBC(ess_bdrS, temp1, temp2);
                }
                Cblock->Finalize();
                Funct_global->SetBlock(1,1, Cblock->ParallelAssemble());
                BTblock->Assemble();
                {
                    Vector temp1(BTblock->Width());
                    temp1 = 0.0;
                    Vector temp2(BTblock->Height());
                    temp2 = 0.0;
                    BTblock->EliminateTrialDofs(ess_bdrSigma, temp1, temp2);
                    BTblock->EliminateTestDofs(ess_bdrS);
                }
                BTblock->Finalize();
                HypreParMatrix * BT = BTblock->ParallelAssemble();
                Funct_global->SetBlock(1,0, BT);
                Funct_global->SetBlock(0,1, BT->Transpose());
            }

            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
            {
                delete Cblock;
                delete BTblock;
                delete secondeqn_rhs;
            }
            delete Bblock;
        }

        // for all but one levels we create projection matrices between levels
        // and projectors assembled on true dofs if MG preconditioner is used
        if (l < num_levels - 1)
        {
            C_space->Update();
            P_C_local = (SparseMatrix *)C_space->GetUpdateOperator();
            P_C_lvls[l] = RemoveZeroEntries(*P_C_local);

            W_space->Update();
            R_space->Update();

            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
            {
                H_space->Update();
                P_H_local = (SparseMatrix *)H_space->GetUpdateOperator();
                SparseMatrix* H_Element_to_dofs1 = new SparseMatrix();
                P_H_lvls[l] = RemoveZeroEntries(*P_H_local);
                divp.Elem2Dofs(*H_space, *H_Element_to_dofs1);
                Element_dofs_H[l] = H_Element_to_dofs1;
            }

            P_W_local = (SparseMatrix *)W_space->GetUpdateOperator();
            P_R_local = (SparseMatrix *)R_space->GetUpdateOperator();

            SparseMatrix* R_Element_to_dofs1 = new SparseMatrix();
            SparseMatrix* W_Element_to_dofs1 = new SparseMatrix();

            divp.Elem2Dofs(*R_space, *R_Element_to_dofs1);
            divp.Elem2Dofs(*W_space, *W_Element_to_dofs1);

            P_W[l] = RemoveZeroEntries(*P_W_local);
            P_R[l] = RemoveZeroEntries(*P_R_local);

            Element_dofs_R[l] = R_Element_to_dofs1;
            Element_dofs_W[l] = W_Element_to_dofs1;

            // computing projectors assembled on true dofs

            // TODO: Rewrite these computations
            auto d_td_coarse_R = R_space_lvls[l + 1]->Dof_TrueDof_Matrix();
            SparseMatrix * RP_R_local = Mult(*R_space_lvls[l]->GetRestrictionMatrix(), *P_R[l]);
            TrueP_R[l] = d_td_coarse_R->LeftDiagMult(
                        *RP_R_local, R_space_lvls[l]->GetTrueDofOffsets());
            TrueP_R[l]->CopyColStarts();
            TrueP_R[l]->CopyRowStarts();

            delete RP_R_local;

            if (prec_is_MG)
            {
                auto d_td_coarse_C = C_space_lvls[l + 1]->Dof_TrueDof_Matrix();
                SparseMatrix * RP_C_local = Mult(*C_space_lvls[l]->GetRestrictionMatrix(), *P_C_lvls[l]);
                TrueP_C[num_levels - 2 - l] = d_td_coarse_C->LeftDiagMult(
                            *RP_C_local, C_space_lvls[l]->GetTrueDofOffsets());
                TrueP_C[num_levels - 2 - l]->CopyColStarts();
                TrueP_C[num_levels - 2 - l]->CopyRowStarts();

                delete RP_C_local;
                //delete d_td_coarse_C;
            }

            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
            {
                auto d_td_coarse_H = H_space_lvls[l + 1]->Dof_TrueDof_Matrix();
                SparseMatrix * RP_H_local = Mult(*H_space_lvls[l]->GetRestrictionMatrix(), *P_H_lvls[l]);
                TrueP_H[num_levels - 2 - l] = d_td_coarse_H->LeftDiagMult(
                            *RP_H_local, H_space_lvls[l]->GetTrueDofOffsets());
                TrueP_H[num_levels - 2 - l]->CopyColStarts();
                TrueP_H[num_levels - 2 - l]->CopyRowStarts();

                delete RP_H_local;
                //delete d_td_coarse_H;
            }

        }

        // FIXME: TrueP_C and TrueP_H has different level ordering compared to TrueP_R

        // creating additional structures required for local problem solvers
        if (l < num_levels - 1)
        {
            (*row_offsets_El_dofs[l])[0] = 0;
            (*row_offsets_El_dofs[l])[1] = Element_dofs_R[l]->Height();
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
                (*row_offsets_El_dofs[l])[2] = Element_dofs_H[l]->Height();
            row_offsets_El_dofs[l]->PartialSum();

            (*col_offsets_El_dofs[l])[0] = 0;
            (*col_offsets_El_dofs[l])[1] = Element_dofs_R[l]->Width();
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
                (*col_offsets_El_dofs[l])[2] = Element_dofs_H[l]->Width();
            col_offsets_El_dofs[l]->PartialSum();

            Element_dofs_Func[l] = new BlockMatrix(*row_offsets_El_dofs[l], *col_offsets_El_dofs[l]);
            Element_dofs_Func[l]->SetBlock(0,0, Element_dofs_R[l]);
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
                Element_dofs_Func[l]->SetBlock(1,1, Element_dofs_H[l]);

            (*row_offsets_P_Func[l])[0] = 0;
            (*row_offsets_P_Func[l])[1] = P_R[l]->Height();
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
                (*row_offsets_P_Func[l])[2] = P_H_lvls[l]->Height();
            row_offsets_P_Func[l]->PartialSum();

            (*col_offsets_P_Func[l])[0] = 0;
            (*col_offsets_P_Func[l])[1] = P_R[l]->Width();
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
                (*col_offsets_P_Func[l])[2] = P_H_lvls[l]->Width();
            col_offsets_P_Func[l]->PartialSum();

            P_Func[l] = new BlockMatrix(*row_offsets_P_Func[l], *col_offsets_P_Func[l]);
            P_Func[l]->SetBlock(0,0, P_R[l]);
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
                P_Func[l]->SetBlock(1,1, P_H_lvls[l]);

            (*row_offsets_TrueP_Func[l])[0] = 0;
            (*row_offsets_TrueP_Func[l])[1] = TrueP_R[l]->Height();
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
                (*row_offsets_TrueP_Func[l])[2] = TrueP_H[num_levels - 2 - l]->Height();
            row_offsets_TrueP_Func[l]->PartialSum();

            (*col_offsets_TrueP_Func[l])[0] = 0;
            (*col_offsets_TrueP_Func[l])[1] = TrueP_R[l]->Width();
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
                (*col_offsets_TrueP_Func[l])[2] = TrueP_H[num_levels - 2 - l]->Width();
            col_offsets_TrueP_Func[l]->PartialSum();

            TrueP_Func[l] = new BlockOperator(*row_offsets_TrueP_Func[l], *col_offsets_TrueP_Func[l]);
            TrueP_Func[l]->SetBlock(0,0, TrueP_R[l]);
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
                TrueP_Func[l]->SetBlock(1,1, TrueP_H[num_levels - 2 - l]);

            P_WT[l] = Transpose(*P_W[l]);
        }

        delete Ablock;
    } // end of loop over all levels

    for ( int l = 0; l < num_levels - 1; ++l)
    {
        BlockMatrix * temp = mfem::Mult(*Funct_mat_lvls[l],*P_Func[l]);
        BlockMatrix * PT_temp = Transpose(*P_Func[l]);
        Funct_mat_lvls[l + 1] = mfem::Mult(*PT_temp, *temp);
        delete temp;
        delete PT_temp;

        SparseMatrix * temp_sp = mfem::Mult(*Constraint_mat_lvls[l], P_Func[l]->GetBlock(0,0));
        Constraint_mat_lvls[l + 1] = mfem::Mult(*P_WT[l], *temp_sp);
        delete temp_sp;
    }

    HypreParMatrix * Constraint_global;

    for (int l = 0; l < num_levels; ++l)
    {
        if (l == 0)
            Funct_global_lvls[l] = Funct_global;
        else
            Funct_global_lvls[l] = new RAPOperator(*TrueP_Func[l - 1], *Funct_global_lvls[l - 1], *TrueP_Func[l - 1]);
    }

    for (int l = 0; l < num_levels; ++l)
    {
        /* cannot do this
        if (l == 0)
        {
            ParDiscreteLinearOperator Divfree_op2(C_space_lvls[l], R_space_lvls[l]); // from Hcurl or HDivSkew(C_space) to Hdiv(R_space)
            if (dim == 3)
                Divfree_op2.AddDomainInterpolator(new CurlInterpolator);
            else // dim == 4
                Divfree_op2.AddDomainInterpolator(new DivSkewInterpolator);
            Divfree_op2.Assemble();
            Divfree_op2.Finalize();
            Divfree_hpmat_mod_lvls[l] = Divfree_op2.ParallelAssemble();
        }
        else
            Divfree_hpmat_mod_lvls[l] = RAP(TrueP_R[l-1], Divfree_hpmat_mod_lvls[l-1], TrueP_C[l-1]);
        */

        ParDiscreteLinearOperator Divfree_op2(C_space_lvls[l], R_space_lvls[l]); // from Hcurl or HDivSkew(C_space) to Hdiv(R_space)
        if (dim == 3)
            Divfree_op2.AddDomainInterpolator(new CurlInterpolator);
        else // dim == 4
            Divfree_op2.AddDomainInterpolator(new DivSkewInterpolator);
        Divfree_op2.Assemble();
        Divfree_op2.Finalize();
        Divfree_hpmat_mod_lvls[l] = Divfree_op2.ParallelAssemble();

        // modifying the divfree operator so that the block which connects internal dofs to boundary dofs is zero
        Eliminate_ib_block(*Divfree_hpmat_mod_lvls[l], *EssBdrTrueDofs_Hcurl[l], *EssBdrTrueDofs_Funct_lvls[l][0]);
    }

    for (int l = 0; l < num_levels; ++l)
    {
        if (l == 0)
        {
            ParBilinearForm *Ablock(new ParBilinearForm(R_space_lvls[l]));
            //Ablock->AddDomainIntegrator(new VectorFEMassIntegrator);
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
                Ablock->AddDomainIntegrator(new VectorFEMassIntegrator);
            else
                Ablock->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.GetKtilda()));
#ifdef WITH_PENALTY
            Ablock->AddDomainIntegrator(new VectorFEMassIntegrator(reg_coeff));
#endif
            Ablock->Assemble();
            Ablock->EliminateEssentialBC(ess_bdrSigma);//, *sigma_exact_finest, *fform); // makes res for sigma_special happier
            Ablock->Finalize();

            (*Funct_hpmat_lvls[l])(0,0) = Ablock->ParallelAssemble();

            delete Ablock;

            ParBilinearForm *Cblock;
            ParMixedBilinearForm *BTblock;
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
            {
                MFEM_ASSERT(strcmp(space_for_S,"H1") == 0, "Case when S is from L2 but is not"
                                                           " eliminated is not supported currently! \n");

                // diagonal block for H^1
                Cblock = new ParBilinearForm(H_space_lvls[l]);
                Cblock->AddDomainIntegrator(new MassIntegrator(*Mytest.GetBtB()));
                Cblock->AddDomainIntegrator(new DiffusionIntegrator(*Mytest.GetBBt()));
                Cblock->Assemble();
                {
                    Vector temp1(Cblock->Width());
                    temp1 = 0.0;
                    Vector temp2(Cblock->Height());
                    temp2 = 0.0;
                    Cblock->EliminateEssentialBC(ess_bdrS, temp1, temp2);
                }
                Cblock->Finalize();

                // off-diagonal block for (H(div), Space_for_S) block
                // you need to create a new integrator here to swap the spaces
                BTblock = new ParMixedBilinearForm(R_space_lvls[l], H_space_lvls[l]);
                BTblock->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.GetMinB()));
                BTblock->Assemble();
                {
                    Vector temp1(BTblock->Width());
                    temp1 = 0.0;
                    Vector temp2(BTblock->Height());
                    temp2 = 0.0;
                    BTblock->EliminateTrialDofs(ess_bdrSigma, temp1, temp2);
                    BTblock->EliminateTestDofs(ess_bdrS);
                }
                BTblock->Finalize();

                (*Funct_hpmat_lvls[l])(1,1) = Cblock->ParallelAssemble();
                HypreParMatrix * BT = BTblock->ParallelAssemble();
                (*Funct_hpmat_lvls[l])(1,0) = BT;
                (*Funct_hpmat_lvls[l])(0,1) = BT->Transpose();

                delete Cblock;
                delete BTblock;
            }
        }
        else // doing RAP for the Functional matrix as an Array2D<HypreParMatrix*>
        {
             // TODO: Rewrite this in a general form
            (*Funct_hpmat_lvls[l])(0,0) = RAP(TrueP_R[l-1], (*Funct_hpmat_lvls[l-1])(0,0), TrueP_R[l-1]);
            (*Funct_hpmat_lvls[l])(0,0)->CopyRowStarts();
            (*Funct_hpmat_lvls[l])(0,0)->CopyRowStarts();

            {
                const Array<int> *temp_dom = EssBdrTrueDofs_Funct_lvls[l][0];

                Eliminate_ib_block(*(*Funct_hpmat_lvls[l])(0,0), *temp_dom, *temp_dom );
                HypreParMatrix * temphpmat = (*Funct_hpmat_lvls[l])(0,0)->Transpose();
                temphpmat->CopyColStarts();
                temphpmat->CopyRowStarts();
                delete (*Funct_hpmat_lvls[l])(0,0);

                Eliminate_ib_block(*temphpmat, *temp_dom, *temp_dom );
                (*Funct_hpmat_lvls[l])(0,0) = temphpmat->Transpose();
                Eliminate_bb_block(*(*Funct_hpmat_lvls[l])(0,0), *temp_dom);
                SparseMatrix diag;
                (*Funct_hpmat_lvls[l])(0,0)->GetDiag(diag);
                diag.MoveDiagonalFirst();

                (*Funct_hpmat_lvls[l])(0,0)->CopyRowStarts();
                (*Funct_hpmat_lvls[l])(0,0)->CopyColStarts();
                delete temphpmat;
            }


            if (strcmp(space_for_S,"H1") == 0)
            {
                (*Funct_hpmat_lvls[l])(1,1) = RAP(TrueP_H[num_levels - 2 - (l-1)], (*Funct_hpmat_lvls[l-1])(1,1),
                        TrueP_H[num_levels - 2 - (l-1)]);
                //(*Funct_hpmat_lvls[l])(1,1)->CopyRowStarts();
                //(*Funct_hpmat_lvls[l])(1,1)->CopyRowStarts();

                {
                    const Array<int> *temp_dom = EssBdrTrueDofs_Funct_lvls[l][1];

                    Eliminate_ib_block(*(*Funct_hpmat_lvls[l])(1,1), *temp_dom, *temp_dom );
                    HypreParMatrix * temphpmat = (*Funct_hpmat_lvls[l])(1,1)->Transpose();
                    temphpmat->CopyColStarts();
                    temphpmat->CopyRowStarts();
                    delete (*Funct_hpmat_lvls[l])(1,1);

                    Eliminate_ib_block(*temphpmat, *temp_dom, *temp_dom );
                    (*Funct_hpmat_lvls[l])(1,1) = temphpmat->Transpose();
                    Eliminate_bb_block(*(*Funct_hpmat_lvls[l])(1,1), *temp_dom);
                    SparseMatrix diag;
                    (*Funct_hpmat_lvls[l])(1,1)->GetDiag(diag);
                    diag.MoveDiagonalFirst();

                    (*Funct_hpmat_lvls[l])(1,1)->CopyRowStarts();
                    (*Funct_hpmat_lvls[l])(1,1)->CopyColStarts();
                    delete temphpmat;
                }

                HypreParMatrix * P_R_T = TrueP_R[l-1]->Transpose();
                HypreParMatrix * temp1 = ParMult((*Funct_hpmat_lvls[l-1])(0,1), TrueP_H[num_levels - 2 - (l-1)]);
                (*Funct_hpmat_lvls[l])(0,1) = ParMult(P_R_T, temp1);
                //(*Funct_hpmat_lvls[l])(0,1)->CopyRowStarts();
                //(*Funct_hpmat_lvls[l])(0,1)->CopyRowStarts();

                {
                    const Array<int> *temp_range = EssBdrTrueDofs_Funct_lvls[l][0];
                    const Array<int> *temp_dom = EssBdrTrueDofs_Funct_lvls[l][1];

                    Eliminate_ib_block(*(*Funct_hpmat_lvls[l])(0,1), *temp_dom, *temp_range );
                    HypreParMatrix * temphpmat = (*Funct_hpmat_lvls[l])(0,1)->Transpose();
                    temphpmat->CopyColStarts();
                    temphpmat->CopyRowStarts();
                    delete (*Funct_hpmat_lvls[l])(0,1);

                    Eliminate_ib_block(*temphpmat, *temp_range, *temp_dom );
                    (*Funct_hpmat_lvls[l])(0,1) = temphpmat->Transpose();
                    (*Funct_hpmat_lvls[l])(0,1)->CopyRowStarts();
                    (*Funct_hpmat_lvls[l])(0,1)->CopyColStarts();
                    delete temphpmat;
                }



                (*Funct_hpmat_lvls[l])(1,0) = (*Funct_hpmat_lvls[l])(0,1)->Transpose();
                (*Funct_hpmat_lvls[l])(1,0)->CopyRowStarts();
                (*Funct_hpmat_lvls[l])(1,0)->CopyRowStarts();

                delete P_R_T;
                delete temp1;
            }

        } // end of else for if (l == 0)

    } // end of loop over levels which create Funct matrices at each level

#ifdef COMPARE_MG
    HypreParMatrix * Coarsened_Op = RAP(TrueP_R[0], (*Funct_hpmat_lvls[0])(0,0), TrueP_R[0] );
    HypreParMatrix * Coarse_Op = (*Funct_hpmat_lvls[1])(0,0);

    {
        SparseMatrix tempm1_diag;
        Coarsened_Op->GetDiag(tempm1_diag);

        SparseMatrix tempm2_diag;
        Coarse_Op->GetDiag(tempm2_diag);

        SparseMatrix tempm2_diag_copy(tempm2_diag);

        tempm2_diag_copy.Add(-1.0, tempm1_diag);

        /*
        for (int i = 0; i < tempm2_diag.Height(); ++i)
        {
            for (int j = 0; j < tempm2_diag.RowSize(i); ++j)
                if (fabs(tempm2_diag.GetData()[tempm2_diag.GetI()[i] + j]) > 1.0e-13)
                {
                    std::cout << "nonzero entry, (" << i << ", " << tempm2_diag.GetJ()[tempm2_diag.GetI()[i] + j] << ", " << tempm2_diag.GetData()[tempm2_diag.GetI()[i] + j] << "\n";
                }
        }
        */

        if (verbose)
            std::cout << "diag(Funct_coarse) - diag(PT Funct_fine P) norm = " << tempm2_diag_copy.MaxNorm() << "\n";
    }
#endif
    //MPI_Finalize();
    //return 0;

    for (int l = 0; l < num_levels; ++l)
    {
        if (l == 0)
        {
            ParMixedBilinearForm *Bblock = new ParMixedBilinearForm(R_space_lvls[l], W_space_lvls[l]);
            Bblock->AddDomainIntegrator(new VectorFEDivergenceIntegrator);
            Bblock->Assemble();
            Vector tempsol(Bblock->Width());
            tempsol = 0.0;
            Vector temprhs(Bblock->Height());
            temprhs = 0.0;
            //Bblock->EliminateTrialDofs(ess_bdrSigma, tempsol, temprhs);
            //Bblock->EliminateTestDofs(ess_bdrSigma);
            Bblock->Finalize();
            Constraint_global = Bblock->ParallelAssemble();

            delete Bblock;
        }
    }

    //MPI_Finalize();
    //return 0;

    for (int l = num_levels - 1; l >=0; --l)
    {
        if (l < num_levels - 1)
        {
#ifdef WITH_SMOOTHERS
            Array<int> SweepsNum(numblocks_funct);
            Array<int> offsets_global(numblocks_funct + 1);
            offsets_global[0] = 0;
            for ( int blk = 0; blk < numblocks_funct; ++blk)
                offsets_global[blk + 1] = Dof_TrueDof_Func_lvls[l][blk]->Width();
            offsets_global.PartialSum();
            SweepsNum = ipow(1, l);
            if (verbose)
            {
                std::cout << "Sweeps num: \n";
                SweepsNum.Print();
            }
            /*
            if (l == 0)
            {
                if (verbose)
                {
                    std::cout << "Sweeps num: \n";
                    SweepsNum.Print();
                }
            }
            */
            Smoothers_lvls[l] = new HcurlGSSSmoother(*Funct_hpmat_lvls[l], *Divfree_hpmat_mod_lvls[l],
                                                     *EssBdrTrueDofs_Hcurl[l],
                                                     EssBdrTrueDofs_Funct_lvls[l],
                                                     &SweepsNum, offsets_global);
#else // for #ifdef WITH_SMOOTHERS
            Smoothers_lvls[l] = NULL;
#endif

#ifdef CHECK_SPDSMOOTHER
            {
                if (num_procs == 1)
                {
                    Vector Vec1(Smoothers_lvls[l]->Height());
                    Vec1.Randomize(2000);
                    Vector Vec2(Smoothers_lvls[l]->Height());
                    Vec2.Randomize(-39);

                    Vector Tempy(Smoothers_lvls[l]->Height());

                    /*
                    for ( int i = 0; i < Vec1.Size(); ++i )
                    {
                        if ((*EssBdrDofs_R[0][0])[i] != 0 )
                        {
                            Vec1[i] = 0.0;
                            Vec2[i] = 0.0;
                        }
                    }
                    */

                    Vector VecDiff(Vec1.Size());
                    VecDiff = Vec1;

                    std::cout << "Norm of Vec1 = " << VecDiff.Norml2() / sqrt(VecDiff.Size())  << "\n";

                    VecDiff -= Vec2;

                    MFEM_ASSERT(VecDiff.Norml2() / sqrt(VecDiff.Size()) > 1.0e-10, "Vec1 equals Vec2 but they must be different");
                    //VecDiff.Print();
                    std::cout << "Norm of (Vec1 - Vec2) = " << VecDiff.Norml2() / sqrt(VecDiff.Size())  << "\n";

                    Smoothers_lvls[l]->Mult(Vec1, Tempy);
                    double scal1 = Tempy * Vec2;
                    double scal3 = Tempy * Vec1;
                    //std::cout << "A Vec1 norm = " << Tempy.Norml2() / sqrt (Tempy.Size()) << "\n";

                    Smoothers_lvls[l]->Mult(Vec2, Tempy);
                    double scal2 = Tempy * Vec1;
                    double scal4 = Tempy * Vec2;
                    //std::cout << "A Vec2 norm = " << Tempy.Norml2() / sqrt (Tempy.Size()) << "\n";

                    std::cout << "scal1 = " << scal1 << "\n";
                    std::cout << "scal2 = " << scal2 << "\n";

                    if ( fabs(scal1 - scal2) / fabs(scal1) > 1.0e-12)
                    {
                        std::cout << "Smoother is not symmetric on two random vectors: \n";
                        std::cout << "vec2 * (A * vec1) = " << scal1 << " != " << scal2 << " = vec1 * (A * vec2)" << "\n";
                        std::cout << "difference = " << scal1 - scal2 << "\n";
                    }
                    else
                    {
                        std::cout << "Smoother was symmetric on the given vectors: dot product = " << scal1 << "\n";
                    }

                    std::cout << "scal3 = " << scal3 << "\n";
                    std::cout << "scal4 = " << scal4 << "\n";

                    if (scal3 < 0 || scal4 < 0)
                    {
                        std::cout << "The operator (new smoother) is not s.p.d. \n";
                    }
                    else
                    {
                        std::cout << "The smoother is s.p.d. on the two random vectors: (Av,v) > 0 \n";
                    }
                }
                else
                    if (verbose)
                        std::cout << "Symmetry check for the smoother works correctly only in the serial case \n";

            }
#endif
        }

        // creating local problem solver hierarchy
        if (l < num_levels - 1)
        {
            int size = 0;
            for (int blk = 0; blk < numblocks_funct; ++blk)
                size += Dof_TrueDof_Func_lvls[l][blk]->GetNumCols();

            bool optimized_localsolve = true;
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
            {
                (*LocalSolver_partfinder_lvls)[l] = new LocalProblemSolverWithS(size, *Funct_mat_lvls[l],
                                                         *Constraint_mat_lvls[l],
                                                         Dof_TrueDof_Func_lvls[l],
                                                         *P_WT[l],
                                                         *Element_dofs_Func[l],
                                                         *Element_dofs_W[l],
                                                         BdrDofs_Funct_lvls[l],
                                                         EssBdrDofs_Funct_lvls[l],
                                                         optimized_localsolve);
            }
            else // no S
            {
                (*LocalSolver_partfinder_lvls)[l] = new LocalProblemSolver(size, *Funct_mat_lvls[l],
                                                         *Constraint_mat_lvls[l],
                                                         Dof_TrueDof_Func_lvls[l],
                                                         *P_WT[l],
                                                         *Element_dofs_Func[l],
                                                         *Element_dofs_W[l],
                                                         BdrDofs_Funct_lvls[l],
                                                         EssBdrDofs_Funct_lvls[l],
                                                         optimized_localsolve);
            }

            (*LocalSolver_lvls)[l] = (*LocalSolver_partfinder_lvls)[l];

        }
    }

    //MPI_Finalize();
    //return 0;

    // Creating the coarsest problem solver
    int size = 0;
    for (int blk = 0; blk < numblocks_funct; ++blk)
        size += Dof_TrueDof_Func_lvls[num_levels - 1][blk]->GetNumCols();
    size += Dof_TrueDof_L2_lvls[num_levels - 1]->GetNumCols();

    CoarsestSolver_partfinder = new CoarsestProblemSolver(size, *Funct_mat_lvls[num_levels - 1],
                                                     *Constraint_mat_lvls[num_levels - 1],
                                                     Dof_TrueDof_Func_lvls[num_levels - 1],
                                                     *Dof_TrueDof_L2_lvls[num_levels - 1],
                                                     EssBdrDofs_Funct_lvls[num_levels - 1],
                                                     EssBdrTrueDofs_Funct_lvls[num_levels - 1]);
#ifdef HCURL_COARSESOLVER
    if (verbose)
        std::cout << "Creating the new coarsest solver which works in the div-free subspace \n" << std::flush;

    int size_sp = 0;
    for (int blk = 0; blk < numblocks_funct; ++blk)
        size_sp += Dof_TrueDof_Func_lvls[num_levels - 1][blk]->GetNumCols();
    CoarsestSolver = new CoarsestProblemHcurlSolver(size_sp,
                                                     *Funct_hpmat_lvls[num_levels - 1],
                                                     *Divfree_hpmat_mod_lvls[num_levels - 1],
                                                     EssBdrTrueDofs_Funct_lvls[num_levels - 1],
                                                     *EssBdrTrueDofs_Hcurl[num_levels - 1]);

    ((CoarsestProblemHcurlSolver*)CoarsestSolver)->SetMaxIter(100);
    ((CoarsestProblemHcurlSolver*)CoarsestSolver)->SetAbsTol(1.0e-7);
    ((CoarsestProblemHcurlSolver*)CoarsestSolver)->SetRelTol(1.0e-7);
    ((CoarsestProblemHcurlSolver*)CoarsestSolver)->ResetSolverParams();
#else
    CoarsestSolver = CoarsestSolver_partfinder;
    CoarsestSolver_partfinder->SetMaxIter(1000);
    CoarsestSolver_partfinder->SetAbsTol(1.0e-12);
    CoarsestSolver_partfinder->SetRelTol(1.0e-12);
    CoarsestSolver_partfinder->ResetSolverParams();
#endif

    if (verbose)
    {
#ifdef HCURL_COARSESOLVER
        if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
            std::cout << "CoarseSolver size = " << Divfree_hpmat_mod_lvls[num_levels - 1]->M()
                    + (*Funct_hpmat_lvls[num_levels - 1])(1,1)->M() << "\n";
        else
            std::cout << "CoarseSolver size = " << Divfree_hpmat_mod_lvls[num_levels - 1]->M() << "\n";
#else
        if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
            std::cout << "CoarseSolver size = " << Dof_TrueDof_Func_lvls[num_levels - 1][0]->N()
                    + Dof_TrueDof_Func_lvls[num_levels - 1][1]->N() + Dof_TrueDof_L2_lvls[num_levels - 1]->N() << "\n";
        else
            std::cout << "CoarseSolver size = " << Dof_TrueDof_Func_lvls[num_levels - 1][0]->N() + Dof_TrueDof_L2_lvls[num_levels - 1]->N() << "\n";
#endif
    }

#ifdef CHECK_SPDCOARSESTSOLVER

#ifdef HCURL_COARSESOLVER
    ((CoarsestProblemHcurlSolver*)CoarsestSolver)->SetMaxIter(200);
    ((CoarsestProblemHcurlSolver*)CoarsestSolver)->SetAbsTol(sqrt(1.0e-14));
    ((CoarsestProblemHcurlSolver*)CoarsestSolver)->SetRelTol(sqrt(1.0e-14));
    ((CoarsestProblemHcurlSolver*)CoarsestSolver)->ResetSolverParams();
#else
    CoarsestSolver = CoarsestSolver_partfinder;
    CoarsestSolver_partfinder->SetMaxIter(1000);
    CoarsestSolver_partfinder->SetAbsTol(1.0e-12);
    CoarsestSolver_partfinder->SetRelTol(1.0e-12);
    CoarsestSolver_partfinder->ResetSolverParams();
#endif


    HypreParMatrix * temp = Divfree_hpmat_mod_lvls[num_levels - 1];
    HypreParMatrix * tempT = temp->Transpose();
    HypreParMatrix * CurlCurlT = ParMult(temp, tempT);

    SparseMatrix diag;
    CurlCurlT->GetDiag(diag);

    if (verbose)
        std::cout << "diag of CurlCurlT unsymmetry measure = " << diag.IsSymmetric() << "\n";

    {
        Vector Tempy(CoarsestSolver->Height());

        Vector Vec1(CoarsestSolver->Height());
        Vec1.Randomize(2000);
        Vector Vec2(CoarsestSolver->Height());
        Vec2.Randomize(-39);

        for ( int i = 0; i < EssBdrTrueDofs_Funct_lvls[num_levels - 1][0]->Size(); ++i )
        {
            int tdof = (*EssBdrTrueDofs_Funct_lvls[num_levels - 1][0])[i];
            //std::cout << "index = " << tdof << "\n";
            Vec1[tdof] = 0.0;
            Vec2[tdof] = 0.0;
        }

        Vector VecDiff(Vec1.Size());
        VecDiff = Vec1;

        std::cout << "Norm of Vec1 = " << VecDiff.Norml2() / sqrt(VecDiff.Size())  << "\n";

        VecDiff -= Vec2;

        MFEM_ASSERT(VecDiff.Norml2() / sqrt(VecDiff.Size()) > 1.0e-10, "Vec1 equals Vec2 but they must be different");
        //VecDiff.Print();
        std::cout << "Norm of (Vec1 - Vec2) = " << VecDiff.Norml2() / sqrt(VecDiff.Size())  << "\n";

        CoarsestSolver->Mult(Vec1, Tempy);
        //CurlCurlT->Mult(Vec1, Tempy);
        double scal1 = Tempy * Vec2;
        double scal3 = Tempy * Vec1;
        //std::cout << "A Vec1 norm = " << Tempy.Norml2() / sqrt (Tempy.Size()) << "\n";

        CoarsestSolver->Mult(Vec2, Tempy);
        //CurlCurlT->Mult(Vec2, Tempy);
        double scal2 = Tempy * Vec1;
        double scal4 = Tempy * Vec2;
        //std::cout << "A Vec2 norm = " << Tempy.Norml2() / sqrt (Tempy.Size()) << "\n";

        std::cout << "scal1 = " << scal1 << "\n";
        std::cout << "scal2 = " << scal2 << "\n";

        if ( fabs(scal1 - scal2) / fabs(scal1) > 1.0e-12)
        {
            std::cout << "CoarsestSolver is not symmetric on two random vectors: \n";
            std::cout << "vec2 * (S * vec1) = " << scal1 << " != " << scal2 << " = vec1 * (S * vec2)" << "\n";
            std::cout << "difference = " << scal1 - scal2 << "\n";
            std::cout << "relative difference = " << fabs(scal1 - scal2) / fabs(scal1) << "\n";
        }
        else
        {
            std::cout << "CoarsestSolver was symmetric on the given vectors: dot product = " << scal1 << "\n";
        }

        std::cout << "scal3 = " << scal3 << "\n";
        std::cout << "scal4 = " << scal4 << "\n";

        if (scal3 < 0 || scal4 < 0)
        {
            std::cout << "The operator (CoarsestSolver) is not s.p.d. \n";
        }
        else
        {
            std::cout << "The CoarsestSolver is s.p.d. on the two random vectors: (Sv,v) > 0 \n";
        }

        //MPI_Finalize();
        //return 0;
    }
#endif


    /*
    StopWatch chrono_debug;

    Vector testRhs(CoarsestSolver->Height());
    testRhs = 1.0;
    Vector testX(CoarsestSolver->Width());
    testX = 0.0;

    MPI_Barrier(comm);
    chrono_debug.Clear();
    chrono_debug.Start();
    for (int it = 0; it < 20; ++it)
    {
        CoarsestSolver->Mult(testRhs, testX);
        testRhs = testX;
    }

    MPI_Barrier(comm);
    chrono_debug.Stop();

    if (verbose)
       std::cout << "CoarsestSolver test run is finished in " << chrono_debug.RealTime() << " \n" << std::flush;

    //delete CoarsestSolver;
    //MPI_Finalize();
    //return 0;
    */

    /*
    // comparing Divfreehpmat with smth from the Divfree_spmat at level 0
    SparseMatrix d_td_Hdiv_diag;
    Dof_TrueDof_Func_lvls[0][0]->GetDiag(d_td_Hdiv_diag);

    SparseMatrix * d_td_Hdiv_diag_T = Transpose(d_td_Hdiv_diag);


    SparseMatrix * tempRA = mfem::Mult(*d_td_Hdiv_diag_T, *Divfree_mat_lvls[0]);
    HypreParMatrix * tempRAP = Dof_TrueDof_Hcurl_lvls[0]->LeftDiagMult(*tempRA, R_space_lvls[0]->GetTrueDofOffsets() );

    ParGridFunction * temppgrfunc = new ParGridFunction(C_space_lvls[0]);
    temppgrfunc->ProjectCoefficient(*Mytest.divfreepart);

    Vector testvec1(tempRAP->Width());
    temppgrfunc->ParallelAssemble(testvec1);
    Vector testvec2(tempRAP->Height());
    tempRAP->Mult(testvec1, testvec2);

    temppgrfunc->ParallelAssemble(testvec1);
    Vector testvec3(tempRAP->Height());
    Divfree_hpmat_mod_lvls[0]->Mult(testvec1, testvec3);

    Vector diffvec(tempRAP->Height());
    double diffnorm = diffvec.Norml2() / sqrt (diffvec.Size());
    MPI_Barrier(comm);
    std::cout << "diffnorm = " << diffnorm << "\n" << std::flush;
    MPI_Barrier(comm);
    */


//#ifdef NEW_INTERFACE

    FormulType * formulat = new FormulType (dim, numsol, verbose);
    FEFormulType * fe_formulat = new FEFormulType(*formulat, feorder);
    BdrCondsType * bdr_conds = new BdrCondsType(*pmesh);

    ProblemType * problem = new ProblemType(*pmesh, *bdr_conds,
                                             *fe_formulat, prec_option, verbose);

    int nlevels = ref_levels + 1;
    GeneralHierarchy * hierarchy = new GeneralHierarchy(nlevels, *pmesh_lvls[num_levels - 1], 0, verbose);
    hierarchy->ConstructDivfreeDops();
    hierarchy->ConstructDofTrueDofs();
    hierarchy->ConstructEl2Dofs();

    FOSLSProblem* problem_mgtools = hierarchy->BuildDynamicProblem<ProblemType>
            (*bdr_conds, *fe_formulat, prec_option, verbose);
    hierarchy->AttachProblem(problem_mgtools);

    ComponentsDescriptor * descriptor;
    {
        bool with_Schwarz = true;
        bool optimized_Schwarz = true;
        bool with_Hcurl = true;
        bool with_coarsest_partfinder = true;
        bool with_coarsest_hcurl = true;
        bool with_monolithic_GS = false;
        bool with_nobnd_op = true;
        descriptor = new ComponentsDescriptor(with_Schwarz, optimized_Schwarz,
                                              with_Hcurl, with_coarsest_partfinder,
                                              with_coarsest_hcurl, with_monolithic_GS,
                                              with_nobnd_op);
    }
    MultigridToolsHierarchy * mgtools_hierarchy =
            new MultigridToolsHierarchy(*hierarchy, 0, *descriptor);


    const Array<int> &essbdr_attribs_Hcurl = problem->GetBdrConditions().GetBdrAttribs(0);

    std::vector<Array<int>*>& essbdr_attribs = problem->GetBdrConditions().GetAllBdrAttribs();

    std::vector<Array<int>*> fullbdr_attribs(numblocks_funct);
    for (unsigned int i = 0; i < fullbdr_attribs.size(); ++i)
    {
        fullbdr_attribs[i] = new Array<int>(pmesh->bdr_attributes.Max());
        (*fullbdr_attribs[i]) = 1;
    }

    Array<SpaceName> space_names_funct(numblocks_funct);
    space_names_funct[0] = SpaceName::HDIV;
    if (strcmp(space_for_S,"H1") == 0)
        space_names_funct[1] = SpaceName::H1;

    Array<SpaceName> space_names_divfree(numblocks_funct);
    space_names_divfree[0] = SpaceName::HCURL;
    if (strcmp(space_for_S,"H1") == 0)
        space_names_divfree[1] = SpaceName::H1;

    std::vector< Array<int>* > coarsebnd_indces_divfree_lvls(num_levels);
    for (int l = 0; l < num_levels - 1; ++l)
    {
        std::vector<Array<int>* > essbdr_tdofs_hcurlfunct =
                hierarchy->GetEssBdrTdofsOrDofs("tdof", space_names_divfree, essbdr_attribs, l + 1);

        int ncoarse_bndtdofs = 0;
        for (int blk = 0; blk < numblocks_funct; ++blk)
        {
            ncoarse_bndtdofs += essbdr_tdofs_hcurlfunct[blk]->Size();
        }

        coarsebnd_indces_divfree_lvls[l] = new Array<int>(ncoarse_bndtdofs);

        int shift_bnd_indices = 0;
        int shift_tdofs_indices = 0;
        for (int blk = 0; blk < numblocks_funct; ++blk)
        {
            for (int j = 0; j < essbdr_tdofs_hcurlfunct[blk]->Size(); ++j)
                (*coarsebnd_indces_divfree_lvls[l])[j + shift_bnd_indices] =
                    (*essbdr_tdofs_hcurlfunct[blk])[j] + shift_tdofs_indices;

            shift_bnd_indices += essbdr_tdofs_hcurlfunct[blk]->Size();
            shift_tdofs_indices += hierarchy->GetSpace(space_names_divfree[blk], l + 1)->TrueVSize();
        }

        for (unsigned int i = 0; i < essbdr_tdofs_hcurlfunct.size(); ++i)
            delete essbdr_tdofs_hcurlfunct[i];

    }

    Array<BlockOperator*> BlockP_mg_nobnd(nlevels - 1);
    Array<Operator*> P_mg(nlevels - 1);
    Array<BlockOperator*> BlockOps_mg(nlevels);
    Array<Operator*> Ops_mg(nlevels);
    Array<Operator*> Smoo_mg(nlevels - 1);
    Operator* CoarseSolver_mg;

    std::vector<const Array<int> *> offsets(nlevels);
    offsets[0] = hierarchy->ConstructTrueOffsetsforFormul(0, space_names_divfree);

    BlockOperator * orig_op = problem->GetOp();
    const HypreParMatrix * divfree_dop = hierarchy->GetDivfreeDop(0);

    HypreParMatrix * divfree_dop_mod = CopyHypreParMatrix(*divfree_dop);

    Array<int> * essbdr_tdofs_Hcurl =
            hierarchy->GetEssBdrTdofsOrDofs("tdof", SpaceName::HCURL, essbdr_attribs_Hcurl, 0);

    Array<int> * essbdr_tdofs_Hdiv =
            hierarchy->GetEssBdrTdofsOrDofs("tdof", SpaceName::HDIV, *essbdr_attribs[0], 0);
    Eliminate_ib_block(*divfree_dop_mod,
                       *essbdr_tdofs_Hcurl,
                       *essbdr_tdofs_Hdiv);

    delete essbdr_tdofs_Hcurl;
    delete essbdr_tdofs_Hdiv;

    // transferring the first block of the functional oiperator from hdiv into hcurl
    BlockOperator * divfree_funct_op = new BlockOperator(*offsets[0]);

    HypreParMatrix * op_00 = dynamic_cast<HypreParMatrix*>(&(orig_op->GetBlock(0,0)));
    HypreParMatrix * A00 = RAP(divfree_dop_mod, op_00, divfree_dop_mod);
    A00->CopyRowStarts();
    A00->CopyColStarts();
    divfree_funct_op->SetBlock(0,0, A00);

    HypreParMatrix * A10, * A01, *op_11;
    if (strcmp(space_for_S,"H1") == 0)
    {
        op_11 = dynamic_cast<HypreParMatrix*>(&(orig_op->GetBlock(1,1)));

        HypreParMatrix * op_10 = dynamic_cast<HypreParMatrix*>(&(orig_op->GetBlock(1,0)));
        A10 = ParMult(op_10, divfree_dop_mod);
        A10->CopyRowStarts();
        A10->CopyColStarts();

        A01 = A10->Transpose();
        A01->CopyRowStarts();
        A01->CopyColStarts();

        divfree_funct_op->SetBlock(1,0, A10);
        divfree_funct_op->SetBlock(0,1, A01);
        divfree_funct_op->SetBlock(1,1, op_11);
    }

    // setting multigrid components from the older parts of the code
    for (int l = 0; l < num_levels; ++l)
    {
        if (l < num_levels - 1)
        {
            offsets[l + 1] = hierarchy->ConstructTrueOffsetsforFormul(l + 1, space_names_divfree);

            BlockP_mg_nobnd[l] = hierarchy->ConstructTruePforFormul(l, space_names_divfree,
                                                                    *offsets[l], *offsets[l + 1]);
            P_mg[l] = new BlkInterpolationWithBNDforTranspose(
                        *BlockP_mg_nobnd[l],
                        *coarsebnd_indces_divfree_lvls[l],
                        *offsets[l], *offsets[l + 1]);
        }

        if (l == 0)
            BlockOps_mg[l] = divfree_funct_op;
        else
        {
            BlockOps_mg[l] = new RAPBlockHypreOperator(*BlockP_mg_nobnd[l - 1],
                    *BlockOps_mg[l - 1], *BlockP_mg_nobnd[l - 1], *offsets[l]);

            std::vector<Array<int>* > essbdr_tdofs_hcurlfunct =
                    hierarchy->GetEssBdrTdofsOrDofs("tdof", space_names_divfree, essbdr_attribs, l);
            EliminateBoundaryBlocks(*BlockOps_mg[l], essbdr_tdofs_hcurlfunct);

            for (unsigned int i = 0; i < essbdr_tdofs_hcurlfunct.size(); ++i)
                delete essbdr_tdofs_hcurlfunct[i];
        }

        Ops_mg[l] = BlockOps_mg[l];

        if (l < num_levels - 1)
            Smoo_mg[l] = new MonolithicGSBlockSmoother( *BlockOps_mg[l],
                                                        *offsets[l], false, HypreSmoother::Type::l1GS, 1);


        //P_mg[l] = ((MonolithicMultigrid*)prec)->GetInterpolation(l);
        //P_mg[l] = new InterpolationWithBNDforTranspose(
                    //*((MonolithicMultigrid*)prec)->GetInterpolation
                    //(num_levels - 1 - 1 - l), *coarsebnd_indces_divfree_lvls[l]);
        //Ops_mg[l] = ((MonolithicMultigrid*)prec)->GetOp(num_levels - 1 - l);
        //Smoo_mg[l] = ((MonolithicMultigrid*)prec)->GetSmoother(num_levels - 1 - l);

    }
    //CoarseSolver_mg = ((MonolithicMultigrid*)prec)->GetCoarsestSolver();

    int coarsest_level = num_levels - 1;
    CoarseSolver_mg = new CGSolver(comm);
    ((CGSolver*)CoarseSolver_mg)->SetAbsTol(sqrt(1e-32));
    ((CGSolver*)CoarseSolver_mg)->SetRelTol(sqrt(1e-12));
    ((CGSolver*)CoarseSolver_mg)->SetMaxIter(100);
    ((CGSolver*)CoarseSolver_mg)->SetPrintLevel(0);
    ((CGSolver*)CoarseSolver_mg)->SetOperator(*Ops_mg[coarsest_level]);
    ((CGSolver*)CoarseSolver_mg)->iterative_mode = false;

    BlockDiagonalPreconditioner * CoarsePrec_mg =
            new BlockDiagonalPreconditioner(BlockOps_mg[coarsest_level]->ColOffsets());

    HypreParMatrix &blk00 = (HypreParMatrix&)BlockOps_mg[coarsest_level]->GetBlock(0,0);
    HypreSmoother * precU = new HypreSmoother(blk00, HypreSmoother::Type::l1GS, 1);
    ((BlockDiagonalPreconditioner*)CoarsePrec_mg)->SetDiagonalBlock(0, precU);

    if (strcmp(space_for_S,"H1") == 0)
    {
        HypreParMatrix &blk11 = (HypreParMatrix&)BlockOps_mg[coarsest_level]->GetBlock(1,1);

        HypreSmoother * precS = new HypreSmoother(blk11, HypreSmoother::Type::l1GS, 1);

        ((BlockDiagonalPreconditioner*)CoarsePrec_mg)->SetDiagonalBlock(1, precS);
    }
    ((BlockDiagonalPreconditioner*)CoarsePrec_mg)->owns_blocks = true;

    ((CGSolver*)CoarseSolver_mg)->SetPreconditioner(*CoarsePrec_mg);

    GeneralMultigrid * GeneralMGprec =
            new GeneralMultigrid(nlevels, P_mg, Ops_mg, *CoarseSolver_mg, Smoo_mg);
//#endif // for #ifdef NEW_INTERFACE

#ifdef TIMING
    //testing the smoother performance

#ifdef WITH_SMOOTHERS
    for (int l = 0; l < num_levels - 1; ++l)
    {
        StopWatch chrono_debug;

        Vector testRhs(Smoothers_lvls[l]->Height());
        testRhs = 1.0;
        Vector testX(Smoothers_lvls[l]->Width());
        testX = 0.0;

        MPI_Barrier(comm);
        chrono_debug.Clear();
        chrono_debug.Start();
        for (int it = 0; it < 1; ++it)
        {
            Smoothers_lvls[l]->Mult(testRhs, testX);
            testRhs += testX;
        }

        MPI_Barrier(comm);
        chrono_debug.Stop();

        if (verbose)
           std::cout << "Smoother at level " << l << "  has finished in " << chrono_debug.RealTime() << " \n" << std::flush;

        if (verbose)
        {
           std::cout << "Internal timing of the smoother at level " << l << ": \n";
           std::cout << "global mult time: " << ((HcurlGSSSmoother*)Smoothers_lvls[l])->GetGlobalMultTime() << " \n" << std::flush;
           std::cout << "internal mult time: " << ((HcurlGSSSmoother*)Smoothers_lvls[l])->GetInternalMultTime() << " \n" << std::flush;
           std::cout << "before internal mult time: " << ((HcurlGSSSmoother*)Smoothers_lvls[l])->GetBeforeIntMultTime() << " \n" << std::flush;
           std::cout << "after internal mult time: " << ((HcurlGSSSmoother*)Smoothers_lvls[l])->GetAfterIntMultTime() << " \n" << std::flush;
        }
        MPI_Barrier(comm);

    }
    for (int l = 0; l < num_levels - 1; ++l)
        ((HcurlGSSSmoother*)Smoothers_lvls[l])->ResetInternalTimings();
#endif
#endif

    if (verbose)
        std::cout << "End of the creating a hierarchy of meshes AND pfespaces \n";

    ParGridFunction * sigma_exact_finest;
    sigma_exact_finest = new ParGridFunction(R_space_lvls[0]);
    sigma_exact_finest->ProjectCoefficient(*Mytest.GetSigma());
    Vector sigma_exact_truedofs(R_space_lvls[0]->GetTrueVSize());
    sigma_exact_finest->ParallelProject(sigma_exact_truedofs);

    ParGridFunction * S_exact_finest;
    Vector S_exact_truedofs;
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
    {
        S_exact_finest = new ParGridFunction(H_space_lvls[0]);
        S_exact_finest->ProjectCoefficient(*Mytest.GetU());
        S_exact_truedofs.SetSize(H_space_lvls[0]->GetTrueVSize());
        S_exact_finest->ParallelProject(S_exact_truedofs);
    }

    chrono.Stop();
    if (verbose)
        std::cout << "Hierarchy of f.e. spaces and stuff was constructed in "<< chrono.RealTime() <<" seconds.\n";

    pmesh->PrintInfo(std::cout); if(verbose) cout << "\n";

    //////////////////////////////////////////////////

#if !defined (WITH_DIVCONSTRAINT_SOLVER) || defined (OLD_CODE)
    chrono.Clear();
    chrono.Start();
    ParGridFunction * Sigmahat = new ParGridFunction(R_space);
    HypreParMatrix *Bdiv;

    ParLinearForm *gform;
    Vector sigmahat_pau;

    if (with_multilevel)
    {
        if (verbose)
            std::cout << "Using multilevel algorithm for finding a particular solution \n";
#ifdef NEW_INTERFACE
        ConstantCoefficient k(1.0);

        SparseMatrix *M_local;
        if (useM_in_divpart)
        {
            ParBilinearForm *Massform = new ParBilinearForm(hierarchy->GetSpace(SpaceName::HDIV, 0));
            Massform->AddDomainIntegrator(new VectorFEMassIntegrator(k));
            Massform->Assemble();
            Massform->Finalize();
            M_local = Massform->LoseMat();
            delete Massform;
        }
        else
            M_local = NULL;

        ParMixedBilinearForm *DivForm = new ParMixedBilinearForm(hierarchy->GetSpace(SpaceName::HDIV, 0),
                                       hierarchy->GetSpace(SpaceName::L2, 0));
        DivForm->AddDomainIntegrator(new VectorFEDivergenceIntegrator);
        DivForm->Assemble();
        DivForm->Finalize();
        Bdiv = DivForm->ParallelAssemble();
        SparseMatrix *B_local = DivForm->LoseMat();

        //Right hand size
        gform = new ParLinearForm(hierarchy->GetSpace(SpaceName::L2, 0));
        gform->AddDomainIntegrator(new DomainLFIntegrator(*problem->GetFEformulation().
                                                          GetFormulation()->GetTest()->GetRhs()));
        gform->Assemble();

        Vector F_fine(hierarchy->GetSpace(SpaceName::L2, 0)->GetVSize());
        Vector G_fine(hierarchy->GetSpace(SpaceName::HDIV, 0)->GetVSize());

        F_fine = *gform;
        G_fine = .0;

        Array< SparseMatrix*> el2dofs_R(ref_levels);
        Array< SparseMatrix*> el2dofs_W(ref_levels);
        Array< SparseMatrix*> P_Hdiv_lvls(ref_levels);
        Array< SparseMatrix*> P_L2_lvls(ref_levels);
        Array< SparseMatrix*> e_AE_lvls(ref_levels);

        for (int l = 0; l < ref_levels; ++l)
        {
            el2dofs_R[l] = hierarchy->GetElementToDofs(SpaceName::HDIV, l);
            el2dofs_W[l] = hierarchy->GetElementToDofs(SpaceName::L2, l);

            P_Hdiv_lvls[l] = hierarchy->GetPspace(SpaceName::HDIV, l);
            P_L2_lvls[l] = hierarchy->GetPspace(SpaceName::L2, l);
            e_AE_lvls[l] = P_L2_lvls[l];
        }

        Array<int>* coarse_essbdr_dofs_Hdiv = hierarchy->GetEssBdrTdofsOrDofs
                ("dof", SpaceName::HDIV, *essbdr_attribs[0], num_levels - 1);

        divp.div_part(ref_levels,
                      M_local, B_local,
                      G_fine,
                      F_fine,
                      P_L2_lvls, P_Hdiv_lvls, e_AE_lvls,
                      el2dofs_R,
                      el2dofs_W,
                      hierarchy->GetDofTrueDof(SpaceName::HDIV, num_levels - 1),
                      hierarchy->GetDofTrueDof(SpaceName::L2, num_levels - 1),
                      hierarchy->GetSpace(SpaceName::HDIV, num_levels - 1)->GetDofOffsets(),
                      hierarchy->GetSpace(SpaceName::L2, num_levels - 1)->GetDofOffsets(),
                      //R_space_lvls[num_levels - 1]->GetDofOffsets(),
                      //W_space_lvls[num_levels - 1]->GetDofOffsets(),
                      sigmahat_pau,
                      *coarse_essbdr_dofs_Hdiv);

        delete coarse_essbdr_dofs_Hdiv;

        delete DivForm;

        if (useM_in_divpart)
            delete M_local;

        delete B_local;

#else
        Vector F_fine(P_W[0]->Height());
        Vector G_fine(P_R[0]->Height());

        ConstantCoefficient k(1.0);

        SparseMatrix *M_local;
        ParBilinearForm *mVarf;
        if (useM_in_divpart)
        {
            mVarf = new ParBilinearForm(R_space);
            mVarf->AddDomainIntegrator(new VectorFEMassIntegrator(k));
            mVarf->Assemble();
            mVarf->Finalize();
            SparseMatrix &M_fine(mVarf->SpMat());
            M_local = &M_fine;
        }
        else
        {
            M_local = NULL;
        }

        ParMixedBilinearForm *bVarf(new ParMixedBilinearForm(R_space, W_space));
        bVarf->AddDomainIntegrator(new VectorFEDivergenceIntegrator);
        bVarf->Assemble();
        bVarf->Finalize();
        Bdiv = bVarf->ParallelAssemble();
        SparseMatrix &B_fine = bVarf->SpMat();
        SparseMatrix *B_local = &B_fine;

        //Right hand size

        gform = new ParLinearForm(W_space);
        gform->AddDomainIntegrator(new DomainLFIntegrator(*Mytest.GetRhs()));
        gform->Assemble();

        F_fine = *gform;
        G_fine = .0;

        divp.div_part(ref_levels,
                      M_local, B_local,
                      G_fine,
                      F_fine,
                      P_W, P_R, P_W,
                      Element_dofs_R,
                      Element_dofs_W,
                      Dof_TrueDof_Func_lvls[num_levels - 1][0],
                      Dof_TrueDof_L2_lvls[num_levels - 1],
                      R_space_lvls[num_levels - 1]->GetDofOffsets(),
                      W_space_lvls[num_levels - 1]->GetDofOffsets(),
                      sigmahat_pau,
                      *EssBdrDofs_Funct_lvls[num_levels - 1][0]);

#ifdef MFEM_DEBUG
        Vector sth(F_fine.Size());
        B_fine.Mult(sigmahat_pau, sth);
        sth -= F_fine;
        std::cout << "sth.Norml2() = " << sth.Norml2() << "\n";
        MFEM_ASSERT(sth.Norml2()<1e-8, "The particular solution does not satisfy the divergence constraint");
#endif

        //delete M_local;
        //delete B_local;
        delete bVarf;
        delete mVarf;
#endif

        *Sigmahat = sigmahat_pau;
    }
    else
    {
        if (verbose)
            std::cout << "Solving Poisson problem for finding a particular solution \n";
        ParGridFunction *sigma_exact;
        ParMixedBilinearForm *Bblock;
        HypreParMatrix *BdivT;
        HypreParMatrix *BBT;
        HypreParVector *Rhs;

#ifdef NEW_INTERFACE
        sigma_exact = new ParGridFunction(hierarchy->GetSpace(SpaceName::HDIV, 0));
        sigma_exact->ProjectCoefficient(*problem->GetFEformulation().
                                        GetFormulation()->GetTest()->GetSigma());
        //sigma_exact->ProjectCoefficient(*Mytest.GetSigma());

        gform = new ParLinearForm(hierarchy->GetSpace(SpaceName::L2, 0));
        gform->AddDomainIntegrator(new DomainLFIntegrator(*problem->GetFEformulation().
                                                          GetFormulation()->GetTest()->GetRhs()));
        //gform->AddDomainIntegrator(new DomainLFIntegrator(*Mytest.GetRhs()));
        gform->Assemble();

        Bblock = new ParMixedBilinearForm(hierarchy->GetSpace(SpaceName::HDIV, 0),
                                          hierarchy->GetSpace(SpaceName::L2, 0));
        Bblock->AddDomainIntegrator(new VectorFEDivergenceIntegrator);
        Bblock->Assemble();
        Bblock->EliminateTrialDofs(*essbdr_attribs[0], *sigma_exact, *gform);

        Bblock->Finalize();
        Bdiv = Bblock->ParallelAssemble();
        BdivT = Bdiv->Transpose();
        BBT = ParMult(Bdiv, BdivT);
        Rhs = gform->ParallelAssemble();

        HypreBoomerAMG * invBBT = new HypreBoomerAMG(*BBT);
        invBBT->SetPrintLevel(0);

        mfem::CGSolver solver(comm);
        solver.SetPrintLevel(0);
        solver.SetMaxIter(70000);
        solver.SetRelTol(1.0e-12);
        solver.SetAbsTol(1.0e-14);
        solver.SetPreconditioner(*invBBT);
        solver.SetOperator(*BBT);

        Vector * Temphat = new Vector(hierarchy->GetSpace(SpaceName::L2, 0)->TrueVSize());
        *Temphat = 0.0;
        solver.Mult(*Rhs, *Temphat);

        Vector * Temp = new Vector(hierarchy->GetSpace(SpaceName::HDIV, 0)->TrueVSize());
        BdivT->Mult(*Temphat, *Temp);

        Sigmahat->Distribute(*Temp);
        //Sigmahat->SetFromTrueDofs(*Temp);

        delete sigma_exact;
        delete invBBT;
        delete BBT;
        delete Bblock;
        delete Rhs;
        delete Temphat;
        delete Temp;
#else
        sigma_exact = new ParGridFunction(R_space);
        sigma_exact->ProjectCoefficient(*Mytest.GetSigma());

        gform = new ParLinearForm(W_space);
        gform->AddDomainIntegrator(new DomainLFIntegrator(*Mytest.GetRhs()));
        gform->Assemble();

        Bblock = new ParMixedBilinearForm(R_space, W_space);
        Bblock->AddDomainIntegrator(new VectorFEDivergenceIntegrator);
        Bblock->Assemble();
        Bblock->EliminateTrialDofs(ess_bdrSigma, *sigma_exact, *gform);

        Bblock->Finalize();
        Bdiv = Bblock->ParallelAssemble();
        BdivT = Bdiv->Transpose();
        BBT = ParMult(Bdiv, BdivT);
        Rhs = gform->ParallelAssemble();

        HypreBoomerAMG * invBBT = new HypreBoomerAMG(*BBT);
        invBBT->SetPrintLevel(0);

        mfem::CGSolver solver(comm);
        solver.SetPrintLevel(0);
        solver.SetMaxIter(70000);
        solver.SetRelTol(1.0e-12);
        solver.SetAbsTol(1.0e-14);
        solver.SetPreconditioner(*invBBT);
        solver.SetOperator(*BBT);

        Vector * Temphat = new Vector(W_space->TrueVSize());
        *Temphat = 0.0;
        solver.Mult(*Rhs, *Temphat);

        Vector * Temp = new Vector(R_space->TrueVSize());
        BdivT->Mult(*Temphat, *Temp);

        Sigmahat->Distribute(*Temp);
        //Sigmahat->SetFromTrueDofs(*Temp);

        delete sigma_exact;
        delete invBBT;
        delete BBT;
        delete Bblock;
        delete Rhs;
        delete Temphat;
        delete Temp;
#endif

    }

    // in either way now Sigmahat is a function from H(div) s.t. div Sigmahat = div sigma = f

    chrono.Stop();
    if (verbose)
        cout << "Particular solution found in "<< chrono.RealTime() <<" seconds.\n";

    if (verbose)
        std::cout << "Checking that particular solution in parallel version satisfies the divergence constraint \n";

    {
        ParLinearForm * constrfform = new ParLinearForm(W_space);
        constrfform->AddDomainIntegrator(new DomainLFIntegrator(*Mytest.GetRhs()));
        constrfform->Assemble();

        Vector Floc(P_W[0]->Height());
        Floc = *constrfform;

        Vector Sigmahat_truedofs(R_space->TrueVSize());
        Sigmahat->ParallelProject(Sigmahat_truedofs);

        if (!CheckConstrRes(Sigmahat_truedofs, *Constraint_global, &Floc, "in the old code for the particular solution"))
        {
            std::cout << "Failure! \n";
        }
        else
            if (verbose)
                std::cout << "Success \n";

        delete constrfform;
    }
#endif


#ifdef OLD_CODE
    chrono.Clear();
    chrono.Start();

    // 6. Define a parallel finite element space on the parallel mesh. Here we
    //    use the Raviart-Thomas finite elements of the specified order.

    int numblocks = 1;
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
        numblocks++;

    Array<int> block_offsets(numblocks + 1); // number of variables + 1
    block_offsets[0] = 0;
    block_offsets[1] = C_space->GetVSize();
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
        block_offsets[2] = S_space->GetVSize();
    block_offsets.PartialSum();

    Array<int> block_trueOffsets(numblocks + 1); // number of variables + 1
    block_trueOffsets[0] = 0;
    block_trueOffsets[1] = C_space->TrueVSize();
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
        block_trueOffsets[2] = S_space->TrueVSize();
    block_trueOffsets.PartialSum();

    HYPRE_Int dimC = C_space->GlobalTrueVSize();
    HYPRE_Int dimR = R_space->GlobalTrueVSize();
    HYPRE_Int dimS;
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
        dimS = S_space->GlobalTrueVSize();
    if (verbose)
    {
        std::cout << "***********************************************************\n";
        std::cout << "dim(C) = " << dimC << "\n";
        if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
        {
            std::cout << "dim(S) = " << dimS << ", ";
            std::cout << "dim(C+S) = " << dimC + dimS << "\n";
        }
        std::cout << "dim(R) = " << dimR << "\n";
        std::cout << "***********************************************************\n";
    }

    BlockVector xblks(block_offsets), rhsblks(block_offsets);
    BlockVector trueX(block_trueOffsets), trueRhs(block_trueOffsets);
    xblks = 0.0;
    rhsblks = 0.0;
    trueX = 0.0;
    trueRhs = 0.0;

    //VectorFunctionCoefficient f(dim, f_exact);
    //VectorFunctionCoefficient vone(dim, vone_exact);
    //VectorFunctionCoefficient vminusone(dim, vminusone_exact);
    //ConstantCoefficient minusone(-1.0);
    //VectorFunctionCoefficient E(dim, E_exact);
    //VectorFunctionCoefficient curlE(dim, curlE_exact);

    //----------------------------------------------------------
    // Setting boundary conditions.
    //----------------------------------------------------------

    if (verbose)
    {
        std::cout << "Boundary conditions: \n";
        std::cout << "all bdr Sigma: \n";
        all_bdrSigma.Print(std::cout, pmesh->bdr_attributes.Max());
        std::cout << "ess bdr Sigma: \n";
        ess_bdrSigma.Print(std::cout, pmesh->bdr_attributes.Max());
        std::cout << "ess bdr S: \n";
        ess_bdrS.Print(std::cout, pmesh->bdr_attributes.Max());
    }

    chrono.Stop();
    if (verbose)
        std::cout << "Small things in OLD_CODE were done in "<< chrono.RealTime() <<" seconds.\n";

    chrono.Clear();
    chrono.Start();

    // the div-free part
    ParGridFunction *S_exact = new ParGridFunction(S_space);
    S_exact->ProjectCoefficient(*Mytest.GetU());

    ParGridFunction * sigma_exact = new ParGridFunction(R_space);
    sigma_exact->ProjectCoefficient(*Mytest.GetSigma());

    {
        Vector Sigmahat_truedofs(R_space->TrueVSize());
        Sigmahat->ParallelProject(Sigmahat_truedofs);

        Vector sigma_exact_truedofs((R_space->TrueVSize()));
        sigma_exact->ParallelProject(sigma_exact_truedofs);

        MFEM_ASSERT(CheckBdrError(Sigmahat_truedofs, &sigma_exact_truedofs, *EssBdrTrueDofs_Funct_lvls[0][0], true),
                                  "for the particular solution Sigmahat in the old code");
    }

    {
        const Array<int> *temp = EssBdrDofs_Funct_lvls[0][0];

        for ( int tdof = 0; tdof < temp->Size(); ++tdof)
        {
            if ( (*temp)[tdof] != 0 && fabs( (*Sigmahat)[tdof]) > 1.0e-14 )
                std::cout << "bnd cnd is violated for Sigmahat! value = "
                          << (*Sigmahat)[tdof]
                          << "exact val = " << (*sigma_exact)[tdof] << ", index = " << tdof << "\n";
        }
    }

    xblks.GetBlock(0) = 0.0;

    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S from H1 or (S from L2 and no elimination)
        xblks.GetBlock(1) = *S_exact;

    ConstantCoefficient zero(.0);

    if (verbose)
        std::cout << "Creating div-free system using the explicit discrete div-free operator \n";

    ParGridFunction* rhside_Hdiv = new ParGridFunction(R_space);  // rhside for the first equation in the original cfosls system
    *rhside_Hdiv = 0.0;

    ParLinearForm *qform;
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
    {
        qform = new ParLinearForm(S_space);
        if (strcmp(space_for_S,"H1") == 0) // S is from H1
            qform->AddDomainIntegrator(new GradDomainLFIntegrator(*Mytest.GetBf()));
        else // S is from L2 but is not eliminated
            qform->AddDomainIntegrator(new DomainLFIntegrator(zero));
        qform->Assemble();
    }

    BlockOperator *MainOp = new BlockOperator(block_trueOffsets);

    // curl or divskew operator from C_space into R_space
    /*
    ParDiscreteLinearOperator Divfree_op(C_space, R_space); // from Hcurl or HDivSkew(C_space) to Hdiv(R_space)
    if (dim == 3)
        Divfree_op.AddDomainInterpolator(new CurlInterpolator());
    else // dim == 4
        Divfree_op.AddDomainInterpolator(new DivSkewInterpolator());
    Divfree_op.Assemble();
    Divfree_op.Finalize();
    HypreParMatrix * Divfree_dop = Divfree_op.ParallelAssemble(); // from Hcurl or HDivSkew(C_space) to Hdiv(R_space)
    HypreParMatrix * DivfreeT_dop = Divfree_dop->Transpose();
    */

    HypreParMatrix * Divfree_dop = Divfree_hpmat_mod_lvls[0];
    HypreParMatrix * DivfreeT_dop = Divfree_dop->Transpose();

    // mass matrix for H(div)
    ParBilinearForm *Mblock(new ParBilinearForm(R_space));
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
    {
        Mblock->AddDomainIntegrator(new VectorFEMassIntegrator);
        //Mblock->AddDomainIntegrator(new DivDivIntegrator); //only for debugging, delete this
    }
    else // no S, hence we need the matrix weight
        Mblock->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.GetKtilda()));
#ifdef WITH_PENALTY
    Mblock->AddDomainIntegrator(new VectorFEMassIntegrator(reg_coeff));
#endif
    Mblock->Assemble();
    Mblock->EliminateEssentialBC(ess_bdrSigma, *sigma_exact, *rhside_Hdiv);
    Mblock->Finalize();

    HypreParMatrix *M = Mblock->ParallelAssemble();

    // div-free operator matrix (curl in 3D, divskew in 4D)
    // either as DivfreeT_dop * M * Divfree_dop
    auto A = RAP(Divfree_dop, M, Divfree_dop);
    A->CopyRowStarts();
    A->CopyColStarts();

    /*
    // I think since we use a modified divfree operator, we don't need this anymore
    Eliminate_ib_block(*A, *EssBdrTrueDofs_Hcurl[0], *EssBdrTrueDofs_Hcurl[0] );
    HypreParMatrix * temphpmat = A->Transpose();
    Eliminate_ib_block(*temphpmat, *EssBdrTrueDofs_Hcurl[0], *EssBdrTrueDofs_Hcurl[0] );
    A = temphpmat->Transpose();
    A->CopyColStarts();
    A->CopyRowStarts();
    SparseMatrix diag;
    A->GetDiag(diag);
    diag.MoveDiagonalFirst();
    delete temphpmat;
    Eliminate_bb_block(*A, *EssBdrTrueDofs_Hcurl[0]);

    {
        SparseMatrix diag;
        A->GetDiag(diag);
        diag.MoveDiagonalFirst();
        diag.Print();
    }

    */

    /*
    ParBilinearForm *Checkblock(new ParBilinearForm(C_space_lvls[0]));
    //Checkblock->AddDomainIntegrator(new CurlCurlIntegrator);
    Checkblock->AddDomainIntegrator(new CurlCurlIntegrator(*Mytest.GetKtilda()));
#ifdef WITH_PENALTY
    Checkblock->AddDomainIntegrator(new VectorFEMassIntegrator(reg_coeff));
#endif
    Checkblock->Assemble();
    {
        Vector temp1(Checkblock->Width());
        temp1 = 0.0;
        Vector temp2(Checkblock->Width());
        Checkblock->EliminateEssentialBC(ess_bdrSigma, temp1, temp2);
    }
    Checkblock->Finalize();
    auto A = Checkblock->ParallelAssemble();
    */

    HypreParMatrix *C, *CH, *CHT, *B, *BT;
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
    {
        // diagonal block for H^1
        ParBilinearForm *Cblock = new ParBilinearForm(S_space);
        if (strcmp(space_for_S,"H1") == 0) // S is from H1
        {
            Cblock->AddDomainIntegrator(new MassIntegrator(*Mytest.GetBtB()));
            Cblock->AddDomainIntegrator(new DiffusionIntegrator(*Mytest.GetBBt()));
        }
        else // S is from L2
        {
            Cblock->AddDomainIntegrator(new MassIntegrator(*Mytest.GetBtB()));
        }
        Cblock->Assemble();
        Cblock->EliminateEssentialBC(ess_bdrS, xblks.GetBlock(1),*qform);
        Cblock->Finalize();
        C = Cblock->ParallelAssemble();

        // off-diagonal block for (H(div), Space_for_S) block
        // you need to create a new integrator here to swap the spaces
        ParMixedBilinearForm *BTblock(new ParMixedBilinearForm(R_space, S_space));
        BTblock->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.GetMinB()));
        BTblock->Assemble();
        BTblock->EliminateTrialDofs(ess_bdrSigma, *sigma_exact, *qform);
        BTblock->EliminateTestDofs(ess_bdrS);
        BTblock->Finalize();
        BT = BTblock->ParallelAssemble();
        B = BT->Transpose();

        CHT = ParMult(DivfreeT_dop, B);
        CHT->CopyColStarts();
        CHT->CopyRowStarts();
        CH = CHT->Transpose();

        delete Cblock;
        delete BTblock;
    }

    // additional temporary vectors on true dofs required for various matvec
    Vector tempHdiv_true(R_space->TrueVSize());
    Vector temp2Hdiv_true(R_space->TrueVSize());

    // assembling local rhs vectors from inhomog. boundary conditions
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
        qform->ParallelAssemble(trueRhs.GetBlock(1));
    rhside_Hdiv->ParallelAssemble(tempHdiv_true);
    DivfreeT_dop->Mult(tempHdiv_true, trueRhs.GetBlock(0));

    // subtracting from rhs a part from Sigmahat
    Sigmahat->ParallelProject(tempHdiv_true);
    M->Mult(tempHdiv_true, temp2Hdiv_true);
    //DivfreeT_dop->Mult(temp2Hdiv_true, tempHcurl_true);
    //trueRhs.GetBlock(0) -= tempHcurl_true;
    DivfreeT_dop->Mult(-1.0, temp2Hdiv_true, 1.0, trueRhs.GetBlock(0));

    // subtracting from rhs for S a part from Sigmahat
    //BT->Mult(tempHdiv_true, tempH1_true);
    //trueRhs.GetBlock(1) -= tempH1_true;
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
        BT->Mult(-1.0, tempHdiv_true, 1.0, trueRhs.GetBlock(1));

    for (int blk = 0; blk < numblocks; ++blk)
    {
        const Array<int> *temp;
        if (blk == 0)
            temp = EssBdrTrueDofs_Hcurl[0];
        else
            temp = EssBdrTrueDofs_Funct_lvls[0][blk];

        for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
        {
            int tdof = (*temp)[tdofind];
            trueRhs.GetBlock(blk)[tdof] = 0.0;
        }
    }
    /*
    {
        MFEM_ASSERT(CheckBdrError(trueRhs.GetBlock(0), NULL, *EssBdrTrueDofs_Hcurl[0], true),
                                  "for the rhside, block 0, for div-free system in the old code");
        if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
            MFEM_ASSERT(CheckBdrError(trueRhs.GetBlock(1), NULL, *EssBdrTrueDofs_Funct_lvls[0][1], true),
                                  "for the rhside, block 1, for div-free system in the old code");
    }
    */


    // setting block operator of the system
    MainOp->SetBlock(0,0, A);
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
    {
        MainOp->SetBlock(0,1, CHT);
        MainOp->SetBlock(1,0, CH);
        MainOp->SetBlock(1,1, C);
    }

    //delete Divfree_dop;
    //delete DivfreeT_dop;

    delete DivfreeT_dop;

    delete rhside_Hdiv;

    chrono.Stop();
    if (verbose)
        std::cout << "Discretized problem is assembled" << endl << flush;

    chrono.Clear();
    chrono.Start();

    Solver *prec;
    Array<BlockOperator*> P;
    std::vector<Array<int> *> offsets_f;
    std::vector<Array<int> *> offsets_c;

    if (with_prec)
    {
        if(dim<=4)
        {
            if (prec_is_MG)
            {
                if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
                {
                    if (monolithicMG)
                    {
                        P.SetSize(TrueP_C.Size());

                        offsets_f.resize(num_levels);
                        offsets_c.resize(num_levels);

                        for (int l = 0; l < P.Size(); l++)
                        {
                            offsets_f[l] = new Array<int>(3);
                            offsets_c[l] = new Array<int>(3);

                            (*offsets_f[l])[0] = (*offsets_c[l])[0] = 0;
                            (*offsets_f[l])[1] = TrueP_C[l]->Height();
                            (*offsets_c[l])[1] = TrueP_C[l]->Width();
                            (*offsets_f[l])[2] = (*offsets_f[l])[1] + TrueP_H[l]->Height();
                            (*offsets_c[l])[2] = (*offsets_c[l])[1] + TrueP_H[l]->Width();

                            P[l] = new BlockOperator(*offsets_f[l], *offsets_c[l]);
                            P[l]->SetBlock(0, 0, TrueP_C[l]);
                            P[l]->SetBlock(1, 1, TrueP_H[l]);
                        }

#ifdef BND_FOR_MULTIGRID
                        prec = new MonolithicMultigrid(*MainOp, P, EssBdrTrueDofs_HcurlFunct_lvls);
#else
                        prec = new MonolithicMultigrid(*MainOp, P);
#endif
                    }
                    else
                    {
                        prec = new BlockDiagonalPreconditioner(block_trueOffsets);
#ifdef BND_FOR_MULTIGRID
                        Operator * precU = new Multigrid(*A, TrueP_C, EssBdrTrueDofs_Hcurl);
                        Operator * precS = new Multigrid(*C, TrueP_H, EssBdrTrueDofs_H1);
#else
                        Operator * precU = new Multigrid(*A, TrueP_C);
                        Operator * precS = new Multigrid(*C, TrueP_H);
#endif
                        ((BlockDiagonalPreconditioner*)prec)->SetDiagonalBlock(0, precU);
                        ((BlockDiagonalPreconditioner*)prec)->SetDiagonalBlock(1, precS);
                        ((BlockDiagonalPreconditioner*)prec)->owns_blocks = true;
                    }
                }
                else // only equation in div-free subspace
                {
                    if (monolithicMG && verbose)
                        std::cout << "There is only one variable in the system because there is no S, \n"
                                     "So monolithicMG is the same as block-diagonal MG \n";
                    if (prec_is_MG)
                    {
                        prec = new BlockDiagonalPreconditioner(block_trueOffsets);
                        Operator * precU;
#ifdef BND_FOR_MULTIGRID

#ifdef COARSEPREC_AMS
                        precU = new Multigrid(*A, TrueP_C, EssBdrTrueDofs_Hcurl, C_space_lvls[num_levels - 1]);
#else
                        precU = new Multigrid(*A, TrueP_C, EssBdrTrueDofs_Hcurl);
#endif
#else
                        precU = new Multigrid(*A, TrueP_C);
#endif
                        //precU = new IdentityOperator(A->Height());

                        ((BlockDiagonalPreconditioner*)prec)->SetDiagonalBlock(0, precU);
                        ((BlockDiagonalPreconditioner*)prec)->owns_blocks = true;
                    }

                }
            }
            else // prec is AMS-like for the div-free part (block-diagonal for the system with boomerAMG for S)
            {
                if (dim == 3)
                {
                    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
                    {
                        prec = new BlockDiagonalPreconditioner(block_trueOffsets);
                        Operator * precU = new IdentityOperator(A->Height());

                        Operator * precS;
                        precS = new IdentityOperator(C->Height());

                        ((BlockDiagonalPreconditioner*)prec)->SetDiagonalBlock(0, precU);
                        ((BlockDiagonalPreconditioner*)prec)->SetDiagonalBlock(1, precS);
                        ((BlockDiagonalPreconditioner*)prec)->owns_blocks = true;
                    }
                    else // no S, i.e. only an equation in div-free subspace
                    {
                        //prec = new BlockDiagonalPreconditioner(block_trueOffsets);
                        //Operator * precU = new IdentityOperator(A->Height());

                        //Operator * precU = new HypreAMS(*A, C_space);
                        //((HypreAMS*)precU)->SetSingularProblem();

                        //((BlockDiagonalPreconditioner*)prec)->SetDiagonalBlock(0, precU);

                        prec = new HypreAMS(*A, C_space);
                        ((HypreAMS*)prec)->SetSingularProblem();
                        //((HypreAMS*)prec)->SetPrintLevel(2);

                        //prec = new BlockDiagonalPreconditioner(block_trueOffsets);
                        //Operator * precU = new IdentityOperator(A->Height());
                        //((BlockDiagonalPreconditioner*)prec)->SetDiagonalBlock(0, precU);

                    }

                }
                else // dim == 4
                {
                    if (verbose)
                        std::cout << "Aux. space prec is not implemented in 4D \n";
                    MPI_Finalize();
                    return 0;
                }
            }
        }

        if (verbose)
            cout << "Preconditioner is ready" << endl << flush;
    }
    else
        if (verbose)
            cout << "Using no preconditioner \n";

    chrono.Stop();
    if (verbose)
        std::cout << "Preconditioner was created in "<< chrono.RealTime() <<" seconds.\n";

#ifndef COMPARE_MG

    CGSolver solver(comm);
    if (verbose)
        cout << "Linear solver: CG" << endl << flush;

    solver.SetAbsTol(sqrt(atol));
    solver.SetRelTol(sqrt(rtol));
    solver.SetMaxIter(max_num_iter);

#ifdef NEW_INTERFACE
    solver.SetOperator(*divfree_funct_op);
    if (with_prec)
        solver.SetPreconditioner(*GeneralMGprec);
#else
    solver.SetOperator(*MainOp);
    if (with_prec)
        solver.SetPreconditioner(*prec);
#endif

    solver.SetPrintLevel(1);
    trueX = 0.0;

    chrono.Clear();
    chrono.Start();
    solver.Mult(trueRhs, trueX);
    chrono.Stop();

    MFEM_ASSERT(CheckBdrError(trueX.GetBlock(0), NULL, *EssBdrTrueDofs_Hcurl[0], true),
                              "for u_truedofs in the old code");

    for (int blk = 0; blk < numblocks; ++blk)
    {
        const Array<int> *temp;
        if (blk == 0)
            temp = EssBdrTrueDofs_Hcurl[0];
        else
            temp = EssBdrTrueDofs_Funct_lvls[0][blk];

        for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
        {
            int tdof = (*temp)[tdofind];
            trueX.GetBlock(blk)[tdof] = 0.0;
        }
    }

    //MFEM_ASSERT(CheckBdrError(trueX.GetBlock(0), NULL, *EssBdrTrueDofs_Hcurl[0], true),
                              //"for u_truedofs in the old code");
    //MFEM_ASSERT(CheckBdrError(trueX.GetBlock(1), NULL, *EssBdrTrueDofs_Funct_lvls[0][1], true),
                              //"for S_truedofs from trueX in the old code");

    if (verbose)
    {
        if (solver.GetConverged())
            std::cout << "Linear solver converged in " << solver.GetNumIterations()
                      << " iterations with a residual norm of " << solver.GetFinalNorm() << ".\n";
        else
            std::cout << "Linear solver did not converge in " << solver.GetNumIterations()
                      << " iterations. Residual norm is " << solver.GetFinalNorm() << ".\n";
        std::cout << "Linear solver took " << chrono.RealTime() << "s. \n";
    }

#ifdef SPECIAL_COARSECHECK
    {
        if (verbose)
            std::cout << "Performing a special coarsest problem convergence check \n";

        /*
        ParBilinearForm *Mblock = new ParBilinearForm(R_space_lvls[num_levels - 1]);
        Mblock->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.GetKtilda()));
#ifdef WITH_PENALTY
        Mblock->AddDomainIntegrator(new VectorFEMassIntegrator(reg_coeff));
#endif
        Mblock->Assemble();
        Vector temp1(Mblock->Width());
        temp1 = 0.0;
        Vector temp2(Mblock->Width());
        Mblock->EliminateEssentialBC(ess_bdrSigma, temp1, temp2);
        Mblock->Finalize();

        HypreParMatrix *M = Mblock->ParallelAssemble();
        */

        auto M = (*Funct_hpmat_lvls[num_levels - 1])(0,0);

        auto A = RAP(Divfree_hpmat_mod_lvls[num_levels - 1], M, Divfree_hpmat_mod_lvls[num_levels - 1]);

        Eliminate_ib_block(*A, *EssBdrTrueDofs_Hcurl[num_levels - 1], *EssBdrTrueDofs_Hcurl[num_levels - 1] );
        HypreParMatrix * temphpmat = A->Transpose();
        Eliminate_ib_block(*temphpmat, *EssBdrTrueDofs_Hcurl[num_levels - 1], *EssBdrTrueDofs_Hcurl[num_levels - 1] );
        A = temphpmat->Transpose();
        A->CopyColStarts();
        A->CopyRowStarts();
        SparseMatrix diag;
        A->GetDiag(diag);
        diag.MoveDiagonalFirst();
        delete temphpmat;
        //Eliminate_bb_block(*A, *EssBdrTrueDofs_Hcurl[num_levels - 1]);

        //A->CopyRowStarts();
        //A->CopyColStarts();

        Vector checkvec(Divfree_hpmat_mod_lvls[num_levels - 1]->Height());
        ParGridFunction * sigma_exact_tdofs_coarsest = new ParGridFunction(R_space_lvls[num_levels - 1]);
        sigma_exact_tdofs_coarsest->ProjectCoefficient(*Mytest.GetSigma());
        sigma_exact_tdofs_coarsest->ParallelProject(checkvec);

        Vector checkRhs(A->Height());
        Divfree_hpmat_mod_lvls[num_levels - 1]->MultTranspose(checkvec, checkRhs);
        Vector checkX(A->Width());

        //delete M;
        //delete Mblock;

        HypreSmoother * prec = new HypreSmoother(*A, HypreSmoother::Type::l1GS, 1);

        CGSolver solver(comm);
        if (verbose)
            cout << "Linear solver: CG" << endl << flush;

        if (verbose)
            std::cout << "Coarsest problem size = " << A->Height() << "\n";

        solver.SetAbsTol(sqrt(1.0e-64));
        solver.SetRelTol(sqrt(1.0e-30));
        solver.SetMaxIter(400);
        solver.SetOperator(*A);

        solver.SetPreconditioner(*prec);
        solver.SetPrintLevel(1);
        checkX = 0.0;

        chrono.Clear();
        chrono.Start();
        solver.Mult(checkRhs, checkX);
        chrono.Stop();

        if (verbose)
        {
            if (solver.GetConverged())
                std::cout << "Linear solver check converged in " << solver.GetNumIterations()
                          << " iterations with a residual norm of " << solver.GetFinalNorm() << ".\n";
            else
                std::cout << "Linear solver check did not converge in " << solver.GetNumIterations()
                          << " iterations. Residual norm is " << solver.GetFinalNorm() << ".\n";
            std::cout << "Linear solver check took " << chrono.RealTime() << "s. \n";
        }

    }
#endif


    chrono.Clear();
    chrono.Start();

    ParGridFunction * u = new ParGridFunction(C_space);
    ParGridFunction * S;

    u->Distribute(&(trueX.GetBlock(0)));

    // 13. Extract the parallel grid function corresponding to the finite element
    //     approximation X. This is the local solution on each processor. Compute
    //     L2 error norms.

    int order_quad = max(2, 2*feorder+1);
    const IntegrationRule *irs[Geometry::NumGeom];
    for (int i=0; i < Geometry::NumGeom; ++i)
    {
        irs[i] = &(IntRules.Get(i, order_quad));
    }

    ParGridFunction * opdivfreepart = new ParGridFunction(R_space);
    Vector u_truedofs(Divfree_hpmat_mod_lvls[0]->Width());
    u->ParallelProject(u_truedofs);

    Vector opdivfree_truedofs(Divfree_hpmat_mod_lvls[0]->Height());
    Divfree_hpmat_mod_lvls[0]->Mult(u_truedofs, opdivfree_truedofs);
    opdivfreepart->Distribute(opdivfree_truedofs);

    {
        const Array<int> *temp = EssBdrDofs_Funct_lvls[0][0];

        for ( int tdof = 0; tdof < temp->Size(); ++tdof)
        {
            if ( (*temp)[tdof] != 0 && fabs( (*opdivfreepart)[tdof]) > 1.0e-14 )
            {
                std::cout << "bnd cnd is violated for opdivfreepart! value = "
                          << (*opdivfreepart)[tdof]
                          << ", index = " << tdof << "\n";
            }
        }
    }

    ParGridFunction * sigma = new ParGridFunction(R_space);
    *sigma = *Sigmahat;         // particular solution
    *sigma += *opdivfreepart;   // plus div-free guy

    {
        const Array<int> *temp = EssBdrDofs_Funct_lvls[0][0];

        for ( int tdof = 0; tdof < temp->Size(); ++tdof)
        {
            if ( (*temp)[tdof] != 0 && fabs( (*sigma)[tdof]) > 1.0e-14 )
                std::cout << "bnd cnd is violated for sigma! value = "
                          << (*sigma)[tdof]
                          << "exact val = " << (*sigma_exact)[tdof] << ", index = " << tdof << "\n";
        }
    }

    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
    {
        S = new ParGridFunction(S_space);
        S->Distribute(&(trueX.GetBlock(1)));
    }
    else // no S, then we compute S from sigma
    {
        // temporary for checking the computation of S below
        //sigma->ProjectCoefficient(*Mytest.GetSigma());

        S = new ParGridFunction(S_space);

        ParBilinearForm *Cblock(new ParBilinearForm(S_space));
        Cblock->AddDomainIntegrator(new MassIntegrator(*Mytest.GetBtB()));
        Cblock->Assemble();
        Cblock->Finalize();
        HypreParMatrix * C = Cblock->ParallelAssemble();

        ParMixedBilinearForm *Bblock(new ParMixedBilinearForm(R_space, S_space));
        Bblock->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.GetB()));
        Bblock->Assemble();
        Bblock->Finalize();
        HypreParMatrix * B = Bblock->ParallelAssemble();
        Vector bTsigma(C->Height());
        Vector trueSigma(R_space->TrueVSize());
        sigma->ParallelProject(trueSigma);

        B->Mult(trueSigma,bTsigma);

        Vector trueS(C->Height());
        trueS = 0.0;

        CGSolver cg(comm);
        cg.SetPrintLevel(0);
        cg.SetMaxIter(5000);
        cg.SetRelTol(sqrt(1.0e-9));
        cg.SetAbsTol(sqrt(1.0e-12));
        cg.SetOperator(*C);
        cg.Mult(bTsigma, trueS);

        //CG(*C, bTsigma, trueS, 0, 5000, 1e-9, 1e-12);
        S->Distribute(trueS);

        delete B;
        delete C;
        delete Bblock;
        delete Cblock;
    }

    double err_sigma = sigma->ComputeL2Error(*Mytest.GetSigma(), irs);
    double norm_sigma = ComputeGlobalLpNorm(2, *Mytest.GetSigma(), *pmesh, irs);

    if (verbose)
        cout << "sigma_h = sigma_hat + div-free part, div-free part = curl u_h \n";

    if (verbose)
    {
        std::cout << "err_sigma = " << err_sigma << ", norm_sigma = " << norm_sigma << "\n";
        if ( norm_sigma > MYZEROTOL )
            cout << "|| sigma_h - sigma_ex || / || sigma_ex || = " << err_sigma / norm_sigma << endl;
        else
            cout << "|| sigma || = " << err_sigma << " (sigma_ex = 0)" << endl;
    }

    /*
    double err_sigmahat = Sigmahat->ComputeL2Error(*Mytest.GetSigma(), irs);
    if (verbose && !withDiv)
        if ( norm_sigma > MYZEROTOL )
            cout << "|| sigma_hat - sigma_ex || / || sigma_ex || = " << err_sigmahat / norm_sigma << endl;
        else
            cout << "|| sigma_hat || = " << err_sigmahat << " (sigma_ex = 0)" << endl;
    */

    DiscreteLinearOperator Div(R_space, W_space);
    Div.AddDomainInterpolator(new DivergenceInterpolator());
    ParGridFunction DivSigma(W_space);
    Div.Assemble();
    Div.Mult(*sigma, DivSigma);

    double err_div = DivSigma.ComputeL2Error(*Mytest.GetRhs(),irs);
    double norm_div = ComputeGlobalLpNorm(2, *Mytest.GetRhs(), *pmesh, irs);

    if (verbose)
    {
        cout << "|| div (sigma_h - sigma_ex) || / ||div (sigma_ex)|| = "
                  << err_div/norm_div  << "\n";
    }

    if (verbose)
    {
        //cout << "Actually it will be ~ continuous L2 + discrete L2 for divergence" << endl;
        cout << "|| sigma_h - sigma_ex ||_Hdiv / || sigma_ex ||_Hdiv = "
                  << sqrt(err_sigma*err_sigma + err_div * err_div)/sqrt(norm_sigma*norm_sigma + norm_div * norm_div)  << "\n";
    }

    double norm_S = 0.0;
    //if (withS)
    {
        ParGridFunction * S_exact = new ParGridFunction(S_space);
        S_exact->ProjectCoefficient(*Mytest.GetU());

        double err_S = S->ComputeL2Error(*Mytest.GetU(), irs);
        norm_S = ComputeGlobalLpNorm(2, *Mytest.GetU(), *pmesh, irs);
        if (verbose)
        {
            if ( norm_S > MYZEROTOL )
                std::cout << "|| S_h - S_ex || / || S_ex || = " <<
                             err_S / norm_S << "\n";
            else
                std::cout << "|| S_h || = " << err_S << " (S_ex = 0) \n";
        }

        if (strcmp(space_for_S,"H1") == 0)
        {
            ParFiniteElementSpace * GradSpace;
            FiniteElementCollection *hcurl_coll;
            if (dim == 3)
                GradSpace = C_space;
            else // dim == 4
            {
                hcurl_coll = new ND1_4DFECollection;
                GradSpace = new ParFiniteElementSpace(pmesh.get(), hcurl_coll);
            }
            DiscreteLinearOperator Grad(S_space, GradSpace);
            Grad.AddDomainInterpolator(new GradientInterpolator());
            ParGridFunction GradS(GradSpace);
            Grad.Assemble();
            Grad.Mult(*S, GradS);

            if (numsol != -34 && verbose)
                std::cout << "For this norm we are grad S for S from numsol = -34 \n";
            VectorFunctionCoefficient GradS_coeff(dim, uFunTest_ex_gradxt);
            double err_GradS = GradS.ComputeL2Error(GradS_coeff, irs);
            double norm_GradS = ComputeGlobalLpNorm(2, GradS_coeff, *pmesh, irs);
            if (verbose)
            {
                std::cout << "|| Grad_h (S_h - S_ex) || / || Grad S_ex || = " <<
                             err_GradS / norm_GradS << "\n";
                std::cout << "|| S_h - S_ex ||_H^1 / || S_ex ||_H^1 = " <<
                             sqrt(err_S*err_S + err_GradS*err_GradS) / sqrt(norm_S*norm_S + norm_GradS*norm_GradS) << "\n";
            }

            if (dim != 3)
            {
                delete GradSpace;
                delete hcurl_coll;
            }

        }

        // Check value of functional and mass conservation
        if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
        {
            Vector trueSigma(R_space->TrueVSize());
            trueSigma = 0.0;
            sigma->ParallelProject(trueSigma);

            Vector MtrueSigma(R_space->TrueVSize());
            MtrueSigma = 0.0;
            M->Mult(trueSigma, MtrueSigma);
            double localFunctional = trueSigma*MtrueSigma;

            Vector GtrueSigma(S_space->TrueVSize());
            GtrueSigma = 0.0;

            BT->Mult(trueSigma, GtrueSigma);
            localFunctional += 2.0*(trueX.GetBlock(1)*GtrueSigma);

            Vector XtrueS(S_space->TrueVSize());
            XtrueS = 0.0;
            C->Mult(trueX.GetBlock(1), XtrueS);
            localFunctional += trueX.GetBlock(1)*XtrueS;

            double globalFunctional;
            MPI_Reduce(&localFunctional, &globalFunctional, 1,
                       MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
            if (verbose)
            {
                cout << "|| sigma_h - L(S_h) ||^2 + || div_h sigma_h - f ||^2 = "
                     << globalFunctional+err_div*err_div<< "\n";
                cout << "|| f ||^2 = " << norm_div*norm_div  << "\n";
                cout << "Relative Energy Error = "
                     << sqrt(globalFunctional+err_div*err_div)/norm_div<< "\n";
            }

            auto trueRhs_part = gform->ParallelAssemble();
            double mass_loc = trueRhs_part->Norml1();
            double mass;
            MPI_Reduce(&mass_loc, &mass, 1,
                       MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
            if (verbose)
                cout << "Sum of local mass = " << mass<< "\n";

            Vector DtrueSigma(W_space->TrueVSize());
            DtrueSigma = 0.0;
            Bdiv->Mult(trueSigma, DtrueSigma);
            DtrueSigma -= *trueRhs_part;
            double mass_loss_loc = DtrueSigma.Norml1();
            double mass_loss;
            MPI_Reduce(&mass_loss_loc, &mass_loss, 1,
                       MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
            if (verbose)
                cout << "Sum of local mass loss = " << mass_loss<< "\n";

            delete trueRhs_part;
        }

        delete S_exact;
    }

    if (verbose)
        cout << "Computing projection errors \n";

    double projection_error_sigma = sigma_exact->ComputeL2Error(*Mytest.GetSigma(), irs);

    if(verbose)
    {
        if ( norm_sigma > MYZEROTOL )
        {
            cout << "|| sigma_ex - Pi_h sigma_ex || / || sigma_ex || = " << projection_error_sigma / norm_sigma << endl;
        }
        else
            cout << "|| Pi_h sigma_ex || = " << projection_error_sigma << " (sigma_ex = 0) \n ";
    }

    //if (withS)
    {
        double projection_error_S = S_exact->ComputeL2Error(*Mytest.GetU(), irs);

        if(verbose)
        {
            if ( norm_S > MYZEROTOL )
                cout << "|| S_ex - Pi_h S_ex || / || S_ex || = " << projection_error_S / norm_S << endl;
            else
                cout << "|| Pi_h S_ex ||  = " << projection_error_S << " (S_ex = 0) \n";
        }
    }

    chrono.Stop();
    if (verbose)
        std::cout << "Errors in the MG code were computed in "<< chrono.RealTime() <<" seconds.\n";

    //MPI_Finalize();
    //return 0;
#endif // for #ifndef COMPARE_MG

#endif // for #ifdef OLD_CODE

    chrono.Clear();
    chrono.Start();

//#ifdef NEW_INTERFACE2
    std::vector<const Array<int> *> offsets_hdivh1(nlevels);
    offsets_hdivh1[0] = hierarchy->ConstructTrueOffsetsforFormul(0, space_names_funct);

    std::vector<const Array<int> *> offsets_sp_hdivh1(nlevels);
    offsets_sp_hdivh1[0] = hierarchy->ConstructOffsetsforFormul(0, space_names_funct);

    //Array<int> offsets_funct_hdivh1;

    // manually truncating the original problem's operator into hdiv-h1 operator
    BlockOperator * hdivh1_op = new BlockOperator(*offsets_hdivh1[0]);

    HypreParMatrix * hdivh1_op_00 = dynamic_cast<HypreParMatrix*>(&(orig_op->GetBlock(0,0)));
    hdivh1_op->SetBlock(0,0, hdivh1_op_00);

    HypreParMatrix * hdivh1_op_01, *hdivh1_op_10, *hdivh1_op_11;
    if (strcmp(space_for_S,"H1") == 0)
    {
        hdivh1_op_01 = dynamic_cast<HypreParMatrix*>(&(orig_op->GetBlock(0,1)));
        hdivh1_op_10 = dynamic_cast<HypreParMatrix*>(&(orig_op->GetBlock(1,0)));
        hdivh1_op_11 = dynamic_cast<HypreParMatrix*>(&(orig_op->GetBlock(1,1)));

        hdivh1_op->SetBlock(0,1, hdivh1_op_01);
        hdivh1_op->SetBlock(1,0, hdivh1_op_10);
        hdivh1_op->SetBlock(1,1, hdivh1_op_11);
    }


    // setting multigrid components from the older parts of the code
    Array<BlockOperator*> BlockP_mg_nobnd_plus(nlevels - 1);
    Array<Operator*> P_mg_plus(nlevels - 1);
    Array<BlockOperator*> BlockOps_mg_plus(nlevels);
    Array<Operator*> Ops_mg_plus(nlevels);
    Array<Operator*> HcurlSmoothers_lvls(nlevels - 1);
    Array<Operator*> SchwarzSmoothers_lvls(nlevels - 1);
    Array<Operator*> Smoo_mg_plus(nlevels - 1);
    Operator* CoarseSolver_mg_plus;

    Array<SparseMatrix*> AE_e_lvls(nlevels - 1);

    Array<BlockMatrix*> el2dofs_funct_lvls(num_levels - 1);
    std::deque<std::vector<HypreParMatrix*> > d_td_Funct_lvls(num_levels - 1);

    std::vector<Operator*> Ops_mg_special(nlevels - 1);

    std::vector< Array<int>* > coarsebnd_indces_funct_lvls(num_levels);

    for (int l = 0; l < num_levels - 1; ++l)
    {
        std::vector<Array<int>* > essbdr_tdofs_funct =
                hierarchy->GetEssBdrTdofsOrDofs("tdof", space_names_funct, essbdr_attribs, l + 1);

        int ncoarse_bndtdofs = 0;
        for (int blk = 0; blk < numblocks; ++blk)
        {

            ncoarse_bndtdofs += essbdr_tdofs_funct[blk]->Size();
        }

        coarsebnd_indces_funct_lvls[l] = new Array<int>(ncoarse_bndtdofs);

        int shift_bnd_indices = 0;
        int shift_tdofs_indices = 0;

        for (int blk = 0; blk < numblocks; ++blk)
        {
            for (int j = 0; j < essbdr_tdofs_funct[blk]->Size(); ++j)
                (*coarsebnd_indces_funct_lvls[l])[j + shift_bnd_indices] =
                    (*essbdr_tdofs_funct[blk])[j] + shift_tdofs_indices;

            shift_bnd_indices += essbdr_tdofs_funct[blk]->Size();
            shift_tdofs_indices += hierarchy->GetSpace(space_names_funct[blk], l + 1)->TrueVSize();
        }

        for (unsigned int i = 0; i < essbdr_tdofs_funct.size(); ++i)
            delete essbdr_tdofs_funct[i];
    }

    std::vector<Array<int>* > dtd_row_offsets(num_levels);
    std::vector<Array<int>* > dtd_col_offsets(num_levels);

    std::vector<Array<int>* > el2dofs_row_offsets(num_levels);
    std::vector<Array<int>* > el2dofs_col_offsets(num_levels);

    Array<SparseMatrix*> Constraint_mat_lvls_mg(num_levels);
    Array<BlockMatrix*> Funct_mat_lvls_mg(num_levels);

    for (int l = 0; l < num_levels; ++l)
    {
        dtd_row_offsets[l] = new Array<int>();
        dtd_col_offsets[l] = new Array<int>();

        el2dofs_row_offsets[l] = new Array<int>();
        el2dofs_col_offsets[l] = new Array<int>();

        if (l < num_levels - 1)
            el2dofs_funct_lvls[l] = hierarchy->GetElementToDofs(space_names_funct, l, el2dofs_row_offsets[l],
                                                            el2dofs_col_offsets[l]);

        if (l < num_levels - 1)
        {
            offsets_hdivh1[l + 1] = hierarchy->ConstructTrueOffsetsforFormul(l + 1, space_names_funct);
            BlockP_mg_nobnd_plus[l] = hierarchy->ConstructTruePforFormul(l, space_names_funct,
                                                                         *offsets_hdivh1[l], *offsets_hdivh1[l + 1]);
            P_mg_plus[l] = new BlkInterpolationWithBNDforTranspose(*BlockP_mg_nobnd_plus[l],
                                                              *coarsebnd_indces_funct_lvls[l],
                                                              *offsets_hdivh1[l], *offsets_hdivh1[l + 1]);
        }

        if (l == 0)
            BlockOps_mg_plus[l] = hdivh1_op;
        else
        {
            BlockOps_mg_plus[l] = new RAPBlockHypreOperator(*BlockP_mg_nobnd_plus[l - 1],
                    *BlockOps_mg_plus[l - 1], *BlockP_mg_nobnd_plus[l - 1], *offsets_hdivh1[l]);

            std::vector<Array<int>* > essbdr_tdofs_funct = hierarchy->GetEssBdrTdofsOrDofs("tdof", space_names_funct, essbdr_attribs, l);
            EliminateBoundaryBlocks(*BlockOps_mg_plus[l], essbdr_tdofs_funct);

            for (unsigned int i = 0; i < essbdr_tdofs_funct.size(); ++i)
                delete essbdr_tdofs_funct[i];
        }

        Ops_mg_plus[l] = BlockOps_mg_plus[l];

        if (l == 0)
        {
            ParMixedBilinearForm *Divblock = new ParMixedBilinearForm(hierarchy->GetSpace(SpaceName::HDIV, 0),
                                                                    hierarchy->GetSpace(SpaceName::L2, 0));
            Divblock->AddDomainIntegrator(new VectorFEDivergenceIntegrator);
            Divblock->Assemble();
            Divblock->Finalize();
            Constraint_mat_lvls_mg[0] = Divblock->LoseMat();
            delete Divblock;

            //offsets_sp_hdivh1[l + 1] = &hierarchy->ConstructOffsetsforFormul(l + 1, space_names_funct);

            Funct_mat_lvls_mg[0] = problem->ConstructFunctBlkMat(*offsets_sp_hdivh1[0]/* offsets_funct_hdivh1*/);
        }
        else
        {
            offsets_sp_hdivh1[l] = hierarchy->ConstructOffsetsforFormul(l, space_names_funct);

            Constraint_mat_lvls_mg[l] = RAP(*hierarchy->GetPspace(SpaceName::L2, l - 1),
                                            *Constraint_mat_lvls_mg[l - 1], *hierarchy->GetPspace(SpaceName::HDIV, l - 1));

            BlockMatrix * P_Funct = hierarchy->ConstructPforFormul(l - 1, space_names_funct,
                                                                       *offsets_sp_hdivh1[l - 1], *offsets_sp_hdivh1[l]);
            Funct_mat_lvls_mg[l] = RAP(*P_Funct, *Funct_mat_lvls_mg[l - 1], *P_Funct);

            delete P_Funct;
        }

        if (l < num_levels - 1)
        {
            Array<int> SweepsNum(numblocks_funct);
            SweepsNum = ipow(1, l);
            if (verbose)
            {
                std::cout << "Sweeps num: \n";
                SweepsNum.Print();
            }
            // getting smoothers from the older mg setup
            //HcurlSmoothers_lvls[l] = Smoothers_lvls[l];
            //SchwarzSmoothers_lvls[l] = (*LocalSolver_lvls)[l];

            Array<int> * essbdr_tdofs_hcurl = hierarchy->GetEssBdrTdofsOrDofs("tdof", SpaceName::HCURL,
                                                                              essbdr_attribs_Hcurl, l);

            std::vector<Array<int>*> essbdr_tdofs_funct = hierarchy->GetEssBdrTdofsOrDofs
                    ("tdof", space_names_funct, essbdr_attribs, l);

            std::vector<Array<int>*> essbdr_dofs_funct = hierarchy->GetEssBdrTdofsOrDofs
                    ("dof", space_names_funct, essbdr_attribs, l);

            std::vector<Array<int>*> fullbdr_dofs_funct = hierarchy->GetEssBdrTdofsOrDofs
                    ("dof", space_names_funct, fullbdr_attribs, l);

            HcurlSmoothers_lvls[l] = new HcurlGSSSmoother(*BlockOps_mg_plus[l],
                                                     *hierarchy->GetDivfreeDop(l),
                                                     //*hierarchy->GetEssBdrTdofsOrDofs("tdof", SpaceName::HCURL,
                                                                               //essbdr_attribs_Hcurl, l),
                                                     *essbdr_tdofs_hcurl,
                                                     //hierarchy->GetEssBdrTdofsOrDofs("tdof",
                                                                               //space_names_funct,
                                                                               //essbdr_attribs, l),
                                                     essbdr_tdofs_funct,
                                                     &SweepsNum, *offsets_hdivh1[l]);

            delete essbdr_tdofs_hcurl;

            int size = BlockOps_mg_plus[l]->Height();


            bool optimized_localsolve = true;

            d_td_Funct_lvls[l] = hierarchy->GetDofTrueDof(space_names_funct, l);

            AE_e_lvls[l] = Transpose(*hierarchy->GetPspace(SpaceName::L2, l));
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
            {
                SchwarzSmoothers_lvls[l] = new LocalProblemSolverWithS(size, *Funct_mat_lvls_mg[l],
                                                         *Constraint_mat_lvls_mg[l],
                                                         //hierarchy->GetDofTrueDof(space_names_funct, l),
                                                         d_td_Funct_lvls[l],
                                                         *AE_e_lvls[l],
                                                         //*hierarchy->GetElementToDofs(space_names_funct, l,
                                                                                     //el2dofs_row_offsets[l],
                                                                                     //el2dofs_col_offsets[l]),
                                                         *el2dofs_funct_lvls[l],
                                                         *hierarchy->GetElementToDofs(SpaceName::L2, l),
                                                         //hierarchy->GetEssBdrTdofsOrDofs("dof", space_names_funct,
                                                                                  //fullbdr_attribs, l),
                                                         fullbdr_dofs_funct,
                                                         //hierarchy->GetEssBdrTdofsOrDofs("dof", space_names_funct,
                                                                                  //essbdr_attribs, l),
                                                         essbdr_dofs_funct,
                                                         optimized_localsolve);
            }
            else // no S
            {
                SchwarzSmoothers_lvls[l] = new LocalProblemSolver(size, *Funct_mat_lvls_mg[l],
                                                                  *Constraint_mat_lvls_mg[l],
                                                                  //hierarchy->GetDofTrueDof(space_names_funct, l),
                                                                  d_td_Funct_lvls[l],
                                                                  *AE_e_lvls[l],
                                                                  //*hierarchy->GetElementToDofs(space_names_funct, l,
                                                                                              //el2dofs_row_offsets[l],
                                                                                              //el2dofs_col_offsets[l]),
                                                                  *el2dofs_funct_lvls[l],
                                                                  *hierarchy->GetElementToDofs(SpaceName::L2, l),
                                                                  //hierarchy->GetEssBdrTdofsOrDofs("dof", space_names_funct,
                                                                                           //fullbdr_attribs, l),
                                                                  fullbdr_dofs_funct,
                                                                  //hierarchy->GetEssBdrTdofsOrDofs("dof", space_names_funct,
                                                                                           //essbdr_attribs, l),
                                                                  essbdr_dofs_funct,
                                                                  optimized_localsolve);
            }

            for (unsigned int i = 0; i < essbdr_tdofs_funct.size(); ++i)
                delete essbdr_tdofs_funct[i];

            for (unsigned int i = 0; i < fullbdr_dofs_funct.size(); ++i)
                delete fullbdr_dofs_funct[i];

            for (unsigned int i = 0; i < essbdr_dofs_funct.size(); ++i)
                delete essbdr_dofs_funct[i];

            //delete P_L2_T;

#ifdef SOLVE_WITH_LOCALSOLVERS
            Smoo_mg_plus[l] = new SmootherSum(*SchwarzSmoothers_lvls[l], *HcurlSmoothers_lvls[l], *Ops_mg_plus[l]);
#else
            Smoo_mg_plus[l] = HcurlSmoothers_lvls[l];
#endif
        }
    }

    for (int l = 0; l < nlevels - 1; ++l)
        Ops_mg_special[l] = Ops_mg_plus[l];

    if (verbose)
        std::cout << "Creating the new coarsest solver which works in the div-free subspace \n" << std::flush;

    //CoarseSolver_mg_plus = CoarsestSolver;

    std::vector<Array<int> * > essbdr_tdofs_funct_coarse =
            hierarchy->GetEssBdrTdofsOrDofs("tdof", space_names_funct, essbdr_attribs, num_levels - 1);

    Array<int> * essbdr_hcurl_coarse = hierarchy->GetEssBdrTdofsOrDofs("tdof", SpaceName::HCURL,
                                                                       essbdr_attribs_Hcurl, num_levels - 1);

    CoarseSolver_mg_plus = new CoarsestProblemHcurlSolver(Ops_mg_plus[num_levels - 1]->Height(),
                                                     *BlockOps_mg_plus[num_levels - 1],
                                                     *hierarchy->GetDivfreeDop(num_levels - 1),
                                                     //hierarchy->GetEssBdrTdofsOrDofs("tdof",
                                                                                     //space_names_funct,
                                                                                     //essbdr_attribs, num_levels - 1),
                                                     essbdr_tdofs_funct_coarse,
                                                     //*hierarchy->GetEssBdrTdofsOrDofs("tdof", SpaceName::HCURL,
                                                                                     //essbdr_attribs_Hcurl, num_levels - 1));
                                                     *essbdr_hcurl_coarse);

    delete essbdr_hcurl_coarse;

    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
    {
        ((CoarsestProblemHcurlSolver*)CoarseSolver_mg_plus)->SetMaxIter(100);
        ((CoarsestProblemHcurlSolver*)CoarseSolver_mg_plus)->SetAbsTol(sqrt(1.0e-32));
        ((CoarsestProblemHcurlSolver*)CoarseSolver_mg_plus)->SetRelTol(sqrt(1.0e-12));
        ((CoarsestProblemHcurlSolver*)CoarseSolver_mg_plus)->ResetSolverParams();
    }
    else // L2 case requires more iterations
    {
        ((CoarsestProblemHcurlSolver*)CoarseSolver_mg_plus)->SetMaxIter(100);
        ((CoarsestProblemHcurlSolver*)CoarseSolver_mg_plus)->SetAbsTol(sqrt(1.0e-32));
        ((CoarsestProblemHcurlSolver*)CoarseSolver_mg_plus)->SetRelTol(sqrt(1.0e-12));
        ((CoarsestProblemHcurlSolver*)CoarseSolver_mg_plus)->ResetSolverParams();
    }

    GeneralMultigrid * GeneralMGprec_plus =
            new GeneralMultigrid(nlevels, P_mg_plus, Ops_mg_plus, *CoarseSolver_mg_plus, Smoo_mg_plus);
//#endif

    if (verbose)
        std::cout << "\nCreating an instance of the new Hcurl smoother and the minimization solver \n";

    //ParLinearForm *fform = new ParLinearForm(R_space);

    ParLinearForm * constrfform = new ParLinearForm(W_space_lvls[0]);
    constrfform->AddDomainIntegrator(new DomainLFIntegrator(*Mytest.GetRhs()));
    constrfform->Assemble();

    /*
    ParMixedBilinearForm *Bblock(new ParMixedBilinearForm(R_space, W_space));
    Bblock->AddDomainIntegrator(new VectorFEDivergenceIntegrator);
    Bblock->Assemble();
    //Bblock->EliminateTrialDofs(ess_bdrSigma, *sigma_exact_finest, *constrfform); // // makes res for sigma_special happier
    Bblock->Finalize();
    */

    Vector Floc(P_W[0]->Height());
    Floc = *constrfform;

    delete constrfform;

    BlockVector Xinit(Funct_mat_lvls[0]->ColOffsets());
    Xinit.GetBlock(0) = 0.0;
    MFEM_ASSERT(Xinit.GetBlock(0).Size() == sigma_exact_finest->Size(),
                "Xinit and sigma_exact_finest have different sizes! \n");

    for (int i = 0; i < sigma_exact_finest->Size(); ++i )
    {
        // just setting Xinit to store correct boundary values at essential boundary
        if ( (*EssBdrDofs_Funct_lvls[0][0])[i] != 0)
            Xinit.GetBlock(0)[i] = (*sigma_exact_finest)[i];
    }

    Array<int> new_trueoffsets(numblocks_funct + 1);
    new_trueoffsets[0] = 0;
    for ( int blk = 0; blk < numblocks_funct; ++blk)
        new_trueoffsets[blk + 1] = Dof_TrueDof_Func_lvls[0][blk]->Width();
    new_trueoffsets.PartialSum();
    BlockVector Xinit_truedofs(new_trueoffsets);
    Xinit_truedofs = 0.0;

    for (int i = 0; i < EssBdrTrueDofs_Funct_lvls[0][0]->Size(); ++i )
    {
        int tdof = (*EssBdrTrueDofs_Funct_lvls[0][0])[i];
        Xinit_truedofs.GetBlock(0)[tdof] = sigma_exact_truedofs[tdof];
    }

    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
    {
        for (int i = 0; i < S_exact_finest->Size(); ++i )
        {
            // just setting Xinit to store correct boundary values at essential boundary
            if ( (*EssBdrDofs_Funct_lvls[0][1])[i] != 0)
                Xinit.GetBlock(1)[i] = (*S_exact_finest)[i];
        }

        for (int i = 0; i < EssBdrTrueDofs_Funct_lvls[0][1]->Size(); ++i )
        {
            int tdof = (*EssBdrTrueDofs_Funct_lvls[0][1])[i];
            Xinit_truedofs.GetBlock(1)[tdof] = S_exact_truedofs[tdof];
        }
    }

    chrono.Stop();
    if (verbose)
        std::cout << "Intermediate allocations for the new solver were done in "<< chrono.RealTime() <<" seconds.\n";
    chrono.Clear();
    chrono.Start();

    if (verbose)
        std::cout << "Calling constructor of the new solver \n";

    int stopcriteria_type = 1;

#ifdef TIMING
    std::list<double>* Times_mult = new std::list<double>;
    std::list<double>* Times_solve = new std::list<double>;
    std::list<double>* Times_localsolve = new std::list<double>;
    std::list<double>* Times_localsolve_lvls = new std::list<double>[num_levels - 1];
    std::list<double>* Times_smoother = new std::list<double>;
    std::list<double>* Times_smoother_lvls = new std::list<double>[num_levels - 1];
    std::list<double>* Times_coarsestproblem = new std::list<double>;
    std::list<double>* Times_resupdate = new std::list<double>;
    std::list<double>* Times_fw = new std::list<double>;
    std::list<double>* Times_up = new std::list<double>;
#endif

#ifdef WITH_DIVCONSTRAINT_SOLVER

    Array<LocalProblemSolver*> LocalSolver_partfinder_lvls_new(num_levels - 1);
    for (int l = 0; l < num_levels - 1; ++l)
    {
        if (strcmp(space_for_S,"H1") == 0)
            LocalSolver_partfinder_lvls_new[l] = dynamic_cast<LocalProblemSolverWithS*>(SchwarzSmoothers_lvls[l]);
        else
            LocalSolver_partfinder_lvls_new[l] = dynamic_cast<LocalProblemSolver*>(SchwarzSmoothers_lvls[l]);
        MFEM_ASSERT(LocalSolver_partfinder_lvls_new[l], "*Unsuccessful cast of the Schwars smoother \n");
    }

    // Creating the coarsest problem solver
    FOSLSFormulation * formulation_alias = problem->GetFEformulation().GetFormulation();
    const Array<SpaceName>* space_names_problem = formulation_alias->GetSpacesDescriptor();
    int coarse_size = 0;
    for (int i = 0; i < space_names_problem->Size(); ++i)
        coarse_size += hierarchy->GetSpace(formulation_alias->GetSpaceName(i), num_levels - 1)->TrueVSize();

    //Array<int> row_offsets_coarse, col_offsets_coarse;

    std::vector<Array<int>* > essbdr_dofs_funct_coarse =
            hierarchy->GetEssBdrTdofsOrDofs("dof", space_names_funct, essbdr_attribs, num_levels - 1);

    BlockOperator * d_td_Funct_coarsest;
    Array<int> d_td_coarsest_row_offsets;
    Array<int> d_td_coarsest_col_offsets;

    d_td_Funct_coarsest = hierarchy->GetDofTrueDof(space_names_funct, nlevels - 1,
                                                  d_td_coarsest_row_offsets, d_td_coarsest_col_offsets);

    CoarsestProblemSolver* CoarsestSolver_partfinder_new =
            new CoarsestProblemSolver(coarse_size,
                                      *Funct_mat_lvls_mg[num_levels - 1],
            *Constraint_mat_lvls_mg[num_levels - 1],
            //hierarchy->GetDofTrueDof(space_names_funct, num_levels - 1, row_offsets_coarse, col_offsets_coarse),
            d_td_Funct_coarsest,
            *hierarchy->GetDofTrueDof(SpaceName::L2, num_levels - 1),
            essbdr_dofs_funct_coarse,
            essbdr_tdofs_funct_coarse);

    CoarsestSolver_partfinder_new->SetMaxIter(70000);
    CoarsestSolver_partfinder_new->SetAbsTol(1.0e-18);
    CoarsestSolver_partfinder_new->SetRelTol(1.0e-18);
    CoarsestSolver_partfinder_new->ResetSolverParams();

    for (unsigned int i = 0; i < essbdr_dofs_funct_coarse.size(); ++i)
        delete essbdr_dofs_funct_coarse[i];

    for (unsigned int i = 0; i < essbdr_tdofs_funct_coarse.size(); ++i)
        delete essbdr_tdofs_funct_coarse[i];

#ifdef NEW_INTERFACE
    //std::vector<Array<int>* > &essbdr_tdofs_funct =
            //hierarchy->GetEssBdrTdofsOrDofs("tdof", space_names, essbdr_attribs, l + 1);

    //Array< SparseMatrix*> el2dofs_R(ref_levels);
    //Array< SparseMatrix*> el2dofs_W(ref_levels);
    //Array< SparseMatrix*> P_Hdiv_lvls(ref_levels);
    Array< SparseMatrix*> P_L2_lvls(ref_levels);
    //Array< SparseMatrix*> AE_e_lvls(ref_levels);

    for (int l = 0; l < ref_levels; ++l)
    {
        //el2dofs_R[l] = hierarchy->GetElementToDofs(SpaceName::HDIV, l);
        //el2dofs_W[l] = hierarchy->GetElementToDofs(SpaceName::L2, l);
        //P_Hdiv_lvls[l] = hierarchy->GetPspace(SpaceName::HDIV, l);
        P_L2_lvls[l] = hierarchy->GetPspace(SpaceName::L2, l);
        //AE_e_lvls[l] = Transpose(*P_L2_lvls[l]);
    }

    std::vector< std::vector<Array<int>* > > essbdr_tdofs_funct_lvls(num_levels);
    for (int l = 0; l < num_levels; ++l)
    {
        essbdr_tdofs_funct_lvls[l] = hierarchy->GetEssBdrTdofsOrDofs
                ("tdof", space_names_funct, essbdr_attribs, l);
    }

    BlockVector * xinit_new = problem->GetTrueInitialConditionFunc();

    FunctionCoefficient * rhs_coeff = problem->GetFEformulation().GetFormulation()->GetTest()->GetRhs();
    ParLinearForm * constrfform_new = new ParLinearForm(hierarchy->GetSpace(SpaceName::L2, 0));
    constrfform_new->AddDomainIntegrator(new DomainLFIntegrator(*rhs_coeff));
    constrfform_new->Assemble();

    DivConstraintSolver PartsolFinder(comm, num_levels,
                                      AE_e_lvls,
                                      BlockP_mg_nobnd_plus,
                                      P_L2_lvls,
                                      Mass_mat_lvls,
                                      essbdr_tdofs_funct_lvls,
                                      Ops_mg_special,
                                      (HypreParMatrix&)(problem->GetOp_nobnd()->GetBlock(numblocks_funct,0)),
                                      *constrfform_new, //Floc,
                                      HcurlSmoothers_lvls,
                                      &LocalSolver_partfinder_lvls_new,
                                      CoarsestSolver_partfinder_new, verbose);

#else
    DivConstraintSolver PartsolFinder(comm, num_levels, P_WT,
                                      TrueP_Func, P_W,
                                      Mass_mat_lvls,
                                      EssBdrTrueDofs_Funct_lvls,
                                      Ops_mg_special,
                                      //Funct_global_lvls,
                                      *Constraint_global,
                                      Floc,
                                      HcurlSmoothers_lvls,
                                      //Smoothers_lvls,
                                      &LocalSolver_partfinder_lvls_new,
                                      //LocalSolver_partfinder_lvls,
                                      CoarsestSolver_partfinder_new, verbose);
                                      //CoarsestSolver_partfinder, verbose);
#endif
    CoarsestSolver_partfinder->SetMaxIter(70000);
    CoarsestSolver_partfinder->SetAbsTol(1.0e-18);
    CoarsestSolver_partfinder->SetRelTol(1.0e-18);
    CoarsestSolver_partfinder->ResetSolverParams();
#endif

#ifdef BRANDNEW_INTERFACE
    bool with_local_smoothers = true;
    bool optimized_localsolvers = true;
    bool with_hcurl_smoothers = true;

    int size_funct = problem_mgtools->GetTrueOffsetsFunc()[numblocks_funct];
    GeneralMinConstrSolver NewSolver(size_funct, *mgtools_hierarchy, with_local_smoothers,
                                     optimized_localsolvers, with_hcurl_smoothers, stopcriteria_type, verbose);
#else

    GeneralMinConstrSolver NewSolver( comm, num_levels,
                                      BlockP_mg_nobnd_plus,
                                      //TrueP_Func,
                                      EssBdrTrueDofs_Funct_lvls,
                                      *Functrhs_global,
                                      HcurlSmoothers_lvls, //Smoothers_lvls,
                                      //Xinit_truedofs, Funct_global_lvls,
                                      Ops_mg_special,
#ifdef CHECK_CONSTR
                                      *Constraint_global, Floc,
#endif
#ifdef TIMING
                                     Times_mult, Times_solve, Times_localsolve, Times_localsolve_lvls, Times_smoother, Times_smoother_lvls, Times_coarsestproblem, Times_resupdate, Times_fw, Times_up,
#endif
#ifdef SOLVE_WITH_LOCALSOLVERS
                                      &SchwarzSmoothers_lvls, //LocalSolver_lvls,
#else
                     NULL,
#endif
                                      CoarseSolver_mg_plus, //CoarsestSolver,
                                      stopcriteria_type);
#endif

    double newsolver_reltol = 1.0e-6;

    if (verbose)
        std::cout << "newsolver_reltol = " << newsolver_reltol << "\n";

    NewSolver.SetRelTol(newsolver_reltol);
    NewSolver.SetMaxIter(200);
    NewSolver.SetPrintLevel(0);
    NewSolver.SetStopCriteriaType(0);

    BlockVector ParticSol(new_trueoffsets);
    ParticSol = 0.0;

    chrono.Stop();
    if (verbose)
        std::cout << "New solver and PartSolFinder were created in "<< chrono.RealTime() <<" seconds.\n";
    chrono.Clear();
    chrono.Start();

#ifdef WITH_DIVCONSTRAINT_SOLVER
    if (verbose)
    {
        std::cout << "CoarsestSolver parameters for the PartSolFinder: \n" << std::flush;
        CoarsestSolver_partfinder->PrintSolverParams();
    }

    PartsolFinder.FindParticularSolution(Xinit_truedofs, ParticSol, Floc, verbose);

    //std::cout << "partic sol norm = " << ParticSol.Norml2() / sqrt (ParticSol.Size()) << "\n";
    //MPI_Finalize();
    //return 0;
#else
    Sigmahat->ParallelProject(ParticSol.GetBlock(0));
#endif

    chrono.Stop();

#ifdef TIMING
#ifdef WITH_SMOOTHERS
    for (int l = 0; l < num_levels - 1; ++l)
        ((HcurlGSSSmoother*)Smoothers_lvls[l])->ResetInternalTimings();
#endif
#endif

#ifndef HCURL_COARSESOLVER
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
    {
        CoarsestSolver_partfinder->SetMaxIter(200);
        CoarsestSolver_partfinder->SetAbsTol(1.0e-9); // -9
        CoarsestSolver_partfinder->SetRelTol(1.0e-9); // -9 for USE_AS_A_PREC
        CoarsestSolver_partfinder->ResetSolverParams();
    }
    else
    {
        CoarsestSolver_partfinder->SetMaxIter(400);
        CoarsestSolver_partfinder->SetAbsTol(1.0e-15); // -9
        CoarsestSolver_partfinder->SetRelTol(1.0e-15); // -9 for USE_AS_A_PREC
        CoarsestSolver_partfinder->ResetSolverParams();
    }
#else
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
    {
#ifdef COMPARE_MG
        ((CoarsestProblemHcurlSolver*)CoarsestSolver)->SetMaxIter(NCOARSEITER);
#else
        ((CoarsestProblemHcurlSolver*)CoarsestSolver)->SetMaxIter(100);
#endif
        ((CoarsestProblemHcurlSolver*)CoarsestSolver)->SetAbsTol(sqrt(1.0e-32));
        ((CoarsestProblemHcurlSolver*)CoarsestSolver)->SetRelTol(sqrt(1.0e-12));
        ((CoarsestProblemHcurlSolver*)CoarsestSolver)->ResetSolverParams();
    }
    else // L2 case requires more iterations
    {
#ifdef COMPARE_MG
        ((CoarsestProblemHcurlSolver*)CoarsestSolver)->SetMaxIter(NCOARSEITER);
#else
        ((CoarsestProblemHcurlSolver*)CoarsestSolver)->SetMaxIter(100);
#endif
        ((CoarsestProblemHcurlSolver*)CoarsestSolver)->SetAbsTol(sqrt(1.0e-32));
        ((CoarsestProblemHcurlSolver*)CoarsestSolver)->SetRelTol(sqrt(1.0e-12));
        ((CoarsestProblemHcurlSolver*)CoarsestSolver)->ResetSolverParams();
    }
#endif
    if (verbose)
    {
        std::cout << "CoarsestSolver parameters for the new solver: \n" << std::flush;
#ifndef HCURL_COARSESOLVER
        CoarsestSolver_partfinder->PrintSolverParams();
#else
        ((CoarsestProblemHcurlSolver*)CoarsestSolver)->PrintSolverParams();
#endif
    }
    if (verbose)
        std::cout << "Particular solution was found in " << chrono.RealTime() <<" seconds.\n";
    chrono.Clear();
    chrono.Start();

    // checking that the computed particular solution satisfies essential boundary conditions
    for ( int blk = 0; blk < numblocks_funct; ++blk)
    {
        MFEM_ASSERT(CheckBdrError(ParticSol.GetBlock(blk), &(Xinit_truedofs.GetBlock(blk)), *EssBdrTrueDofs_Funct_lvls[0][blk], true),
                                  "for the particular solution");
    }

    // checking that the boundary conditions are not violated for the initial guess
    for ( int blk = 0; blk < numblocks_funct; ++blk)
    {
        for (int i = 0; i < EssBdrTrueDofs_Funct_lvls[0][blk]->Size(); ++i)
        {
            int tdofind = (*EssBdrTrueDofs_Funct_lvls[0][blk])[i];
            if ( fabs(ParticSol.GetBlock(blk)[tdofind]) > 1.0e-14 )
            {
                std::cout << "blk = " << blk << ": bnd cnd is violated for the ParticSol! \n";
                std::cout << "tdofind = " << tdofind << ", value = " << ParticSol.GetBlock(blk)[tdofind] << "\n";
            }
        }
    }

    // checking that the particular solution satisfies the divergence constraint
    BlockVector temp_dofs(Funct_mat_lvls[0]->RowOffsets());
    for ( int blk = 0; blk < numblocks_funct; ++blk)
    {
        Dof_TrueDof_Func_lvls[0][blk]->Mult(ParticSol.GetBlock(blk), temp_dofs.GetBlock(blk));
    }

    Vector temp_constr(Constraint_mat_lvls[0]->Height());
    Constraint_mat_lvls[0]->Mult(temp_dofs.GetBlock(0), temp_constr);
    temp_constr -= Floc;

    // 3.1 if not, abort
    if ( ComputeMPIVecNorm(comm, temp_constr,"", verbose) > 1.0e-13 )
    {
        std::cout << "Initial vector does not satisfies divergence constraint. \n";
        double temp = ComputeMPIVecNorm(comm, temp_constr,"", verbose);
        //temp_constr.Print();
        if (verbose)
            std::cout << "Constraint residual norm: " << temp << "\n";
        MFEM_ABORT("");
    }

    for (int blk = 0; blk < numblocks_funct; ++blk)
    {
        MFEM_ASSERT(CheckBdrError(ParticSol.GetBlock(blk), &(Xinit_truedofs.GetBlock(blk)), *EssBdrTrueDofs_Funct_lvls[0][blk], true),
                                  "for the particular solution");
    }


    Vector error3(ParticSol.Size());
    error3 = ParticSol;

    int local_size3 = error3.Size();
    int global_size3 = 0;
    MPI_Allreduce(&local_size3, &global_size3, 1, MPI_INT, MPI_SUM, comm);

    double local_normsq3 = error3 * error3;
    double global_norm3 = 0.0;
    MPI_Allreduce(&local_normsq3, &global_norm3, 1, MPI_DOUBLE, MPI_SUM, comm);
    global_norm3 = sqrt (global_norm3 / global_size3);

    if (verbose)
        std::cout << "error3 norm special = " << global_norm3 << "\n";

    if (verbose)
        std::cout << "Checking that particular solution in parallel version satisfies the divergence constraint \n";

    //MFEM_ASSERT(CheckConstrRes(*PartSolDofs, *Constraint_mat_lvls[0], &Floc, "in the main code for the particular solution"), "Failure");
    //if (!CheckConstrRes(*PartSolDofs, *Constraint_mat_lvls[0], &Floc, "in the main code for the particular solution"))
    if (!CheckConstrRes(ParticSol.GetBlock(0), *Constraint_global, &Floc, "in the main code for the particular solution"))
    {
        std::cout << "Failure! \n";
    }
    else
        if (verbose)
            std::cout << "Success \n";
    //MPI_Finalize();
    //return 0;

    /*
    Vector tempp(sigma_exact_finest->Size());
    tempp = *sigma_exact_finest;
    tempp -= Xinit;

    std::cout << "norm of sigma_exact = " << sigma_exact_finest->Norml2() / sqrt (sigma_exact_finest->Size()) << "\n";
    std::cout << "norm of sigma_exact - Xinit = " << tempp.Norml2() / sqrt (tempp.Size()) << "\n";

    Vector res(Funct_mat_lvls[0]->GetBlock(0,0).Height());
    Funct_mat_lvls[0]->GetBlock(0,0).Mult(*sigma_exact_finest, res);
    double func_norm = res.Norml2() / sqrt (res.Size());
    std::cout << "Functional norm for sigma_exact projection:  = " << func_norm << " ... \n";

#ifdef OLD_CODE
    res = 0.0;
    Funct_mat_lvls[0]->GetBlock(0,0).Mult(*sigma, res);
    func_norm = res.Norml2() / sqrt (res.Size());
    std::cout << "Functional norm for exact sigma_h:  = " << func_norm << " ... \n";
#endif
    */

    chrono.Stop();
    if (verbose)
        std::cout << "Intermediate things were done in " << chrono.RealTime() <<" seconds.\n";
    chrono.Clear();
    chrono.Start();

    ParGridFunction * NewSigmahat = new ParGridFunction(R_space_lvls[0]);

    ParGridFunction * NewS;
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
        NewS = new ParGridFunction(H_space_lvls[0]);

    //Vector Tempx(sigma_exact_finest->Size());
    //Tempx = 0.0;
    //Vector Tempy(Tempx.Size());
    Vector Tempy(ParticSol.Size());
    Tempy = 0.0;

#ifdef CHECK_SPDSOLVER

    // checking that for unsymmetric version the symmetry check does
    // provide the negative answer
    //NewSolver.SetUnSymmetric();

    Vector Vec1(NewSolver.Height());
    Vec1.Randomize(2000);
    Vector Vec2(NewSolver.Height());
    Vec2.Randomize(-39);

    for ( int i = 0; i < EssBdrTrueDofs_Funct_lvls[0][0]->Size(); ++i )
    {
        int tdof = (*EssBdrTrueDofs_Funct_lvls[0][0])[i];
        Vec1[tdof] = 0.0;
        Vec2[tdof] = 0.0;
    }

    Vector VecDiff(Vec1.Size());
    VecDiff = Vec1;

    std::cout << "Norm of Vec1 = " << VecDiff.Norml2() / sqrt(VecDiff.Size())  << "\n";

    VecDiff -= Vec2;

    MFEM_ASSERT(VecDiff.Norml2() / sqrt(VecDiff.Size()) > 1.0e-10, "Vec1 equals Vec2 but they must be different");
    //VecDiff.Print();
    std::cout << "Norm of (Vec1 - Vec2) = " << VecDiff.Norml2() / sqrt(VecDiff.Size())  << "\n";

    NewSolver.SetAsPreconditioner(true);
    NewSolver.SetMaxIter(1);

    NewSolver.Mult(Vec1, Tempy);
    double scal1 = Tempy * Vec2;
    double scal3 = Tempy * Vec1;
    //std::cout << "A Vec1 norm = " << Tempy.Norml2() / sqrt (Tempy.Size()) << "\n";

    NewSolver.Mult(Vec2, Tempy);
    double scal2 = Tempy * Vec1;
    double scal4 = Tempy * Vec2;
    //std::cout << "A Vec2 norm = " << Tempy.Norml2() / sqrt (Tempy.Size()) << "\n";

    std::cout << "scal1 = " << scal1 << "\n";
    std::cout << "scal2 = " << scal2 << "\n";

    if ( fabs(scal1 - scal2) / fabs(scal1) > 1.0e-12)
    {
        std::cout << "Solver is not symmetric on two random vectors: \n";
        std::cout << "vec2 * (A * vec1) = " << scal1 << " != " << scal2 << " = vec1 * (A * vec2)" << "\n";
        std::cout << "difference = " << scal1 - scal2 << "\n";
        std::cout << "relative difference = " << fabs(scal1 - scal2) / fabs(scal1) << "\n";
    }
    else
    {
        std::cout << "Solver was symmetric on the given vectors: dot product = " << scal1 << "\n";
    }

    std::cout << "scal3 = " << scal3 << "\n";
    std::cout << "scal4 = " << scal4 << "\n";

    if (scal3 < 0 || scal4 < 0)
    {
        std::cout << "The operator (new solver) is not s.p.d. \n";
    }
    else
    {
        std::cout << "The solver is s.p.d. on the two random vectors: (Av,v) > 0 \n";
    }

    MPI_Finalize();
    return 0;

#endif


#ifdef USE_AS_A_PREC
    if (verbose)
        std::cout << "Using the new solver as a preconditioner for CG for the correction \n";

    chrono.Clear();
    chrono.Start();

    ParLinearForm *fformtest = new ParLinearForm(R_space_lvls[0]);
    ConstantCoefficient zerotest(.0);
    fformtest->AddDomainIntegrator(new VectordivDomainLFIntegrator(zerotest));
    fformtest->Assemble();

    ParLinearForm *qformtest;
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS)
    {
        qformtest = new ParLinearForm(H_space_lvls[0]);
        qformtest->AddDomainIntegrator(new GradDomainLFIntegrator(*Mytest.GetBf()));
        qformtest->Assemble();
    }

    ParBilinearForm *Ablocktest(new ParBilinearForm(R_space_lvls[0]));
    HypreParMatrix *Atest;
    if (strcmp(space_for_S,"H1") == 0)
        Ablocktest->AddDomainIntegrator(new VectorFEMassIntegrator);
    else
        Ablocktest->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.GetKtilda()));
#ifdef WITH_PENALTY
    Ablocktest->AddDomainIntegrator(new VectorFEMassIntegrator(reg_coeff));
#endif
    Ablocktest->Assemble();
    Ablocktest->EliminateEssentialBC(ess_bdrSigma, *sigma_exact_finest, *fformtest);
    Ablocktest->Finalize();
    Atest = Ablocktest->ParallelAssemble();

    delete Ablocktest;

    HypreParMatrix *Ctest;
    if (strcmp(space_for_S,"H1") == 0)
    {
        ParBilinearForm * Cblocktest = new ParBilinearForm(H_space_lvls[0]);
        if (strcmp(space_for_S,"H1") == 0)
        {
            Cblocktest->AddDomainIntegrator(new MassIntegrator(*Mytest.GetBtB()));
            Cblocktest->AddDomainIntegrator(new DiffusionIntegrator(*Mytest.GetBBt()));
        }
        Cblocktest->Assemble();
        Cblocktest->EliminateEssentialBC(ess_bdrS, *S_exact_finest, *qformtest);
        Cblocktest->Finalize();

        Ctest = Cblocktest->ParallelAssemble();

        delete Cblocktest;
    }

    HypreParMatrix *Btest;
    HypreParMatrix *BTtest;
    if (strcmp(space_for_S,"H1") == 0)
    {
        ParMixedBilinearForm *Bblocktest = new ParMixedBilinearForm(R_space_lvls[0], H_space_lvls[0]);
        Bblocktest->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.GetMinB()));
        Bblocktest->Assemble();
        Bblocktest->EliminateTrialDofs(ess_bdrSigma, *sigma_exact_finest, *qformtest);
        Bblocktest->EliminateTestDofs(ess_bdrS);
        Bblocktest->Finalize();

        Btest = Bblocktest->ParallelAssemble();
        BTtest = Btest->Transpose();

        delete Bblocktest;
    }

    Array<int> blocktest_offsets(numblocks_funct + 1);
    blocktest_offsets[0] = 0;
    blocktest_offsets[1] = Atest->Height();
    if (strcmp(space_for_S,"H1") == 0)
        blocktest_offsets[2] = Ctest->Height();
    blocktest_offsets.PartialSum();

    BlockVector trueXtest(blocktest_offsets);
    BlockVector trueRhstest(blocktest_offsets);
    trueRhstest = 0.0;

    fformtest->ParallelAssemble(trueRhstest.GetBlock(0));
    if (strcmp(space_for_S,"H1") == 0)
        qformtest->ParallelAssemble(trueRhstest.GetBlock(1));

    delete fformtest;
    if (strcmp(space_for_S,"H1") == 0)
        delete qformtest;

    BlockOperator *BlockMattest = new BlockOperator(blocktest_offsets);
    BlockMattest->SetBlock(0,0, Atest);
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
    {
        BlockMattest->SetBlock(0,1, BTtest);
        BlockMattest->SetBlock(1,0, Btest);
        BlockMattest->SetBlock(1,1, Ctest);
    }

    NewSolver.SetAsPreconditioner(true);
    NewSolver.SetPrintLevel(0);
    if (verbose)
        NewSolver.PrintAllOptions();

#ifdef  COMPARE_MG
    if (verbose)
        std::cout << "\nComparing geometric MG with modified new MG (w/o Schwarz smoother) \n";

    //MFEM_ASSERT(strcmp(space_for_S,"L2") == 0, "Right now the check works only for S in L2 case!\n");
    //MFEM_ASSERT(num_procs == 1, "Right now the check operates only in serial case \n");
    //MFEM_ASSERT(num_levels == 2, "Check works only for 2-level case \n");

    Array<int> offsets_new(numblocks_funct + 1);
    offsets_new = 0;
    for (int blk = 0; blk < numblocks_funct; ++blk)
        offsets_new[blk + 1] = (*Funct_hpmat_lvls[0])(blk,blk)->Height();
    offsets_new.PartialSum();

    BlockVector inFunctvec(offsets_new);
    inFunctvec.GetBlock(0) = sigma_exact_truedofs;
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
        inFunctvec.GetBlock(1) = S_exact_truedofs;

    for (int blk = 0; blk < numblocks_funct; ++blk)
    {
        const Array<int> *temp = EssBdrTrueDofs_Funct_lvls[0][blk];
        for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
        {
            if ( fabs(inFunctvec.GetBlock(blk)[(*temp)[tdofind]]) > 1.0e-14 )
                std::cout << "bnd cnd is violated for inFunctvec, blk = " << blk << ",  value = "
                          << inFunctvec.GetBlock(blk)[(*temp)[tdofind]]
                          << ", index = " << (*temp)[tdofind] << "\n";
        }
    }
    /*
    Vector inHdivvec(NewSolver.Width());
    inHdivvec = sigma_exact_truedofs;

#ifdef CHECK_BNDCND
    {
        const Array<int> *temp = EssBdrTrueDofs_Funct_lvls[0][0];
        for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
        {
            if ( fabs(inHdivvec[(*temp)[tdofind]]) > 1.0e-14 )
                std::cout << "bnd cnd is violated for inHdivvec, value = "
                          << inHdivvec[(*temp)[tdofind]]
                          << ", index = " << (*temp)[tdofind] << "\n";
        }
    }
#endif
    */

    Array<int> offsets_hcurlfunct_new(numblocks_funct + 1);
    offsets_hcurlfunct_new = 0;
    for (int blk = 0; blk < numblocks_funct; ++blk)
        if (blk == 0)
            offsets_hcurlfunct_new[blk + 1] = Divfree_hpmat_mod_lvls[0]->Width();
        else
            offsets_hcurlfunct_new[blk + 1] = (*Funct_hpmat_lvls[0])(blk,blk)->Height();
    offsets_hcurlfunct_new.PartialSum();

    BlockVector inFunctHcurlvec(offsets_hcurlfunct_new);
    for (int blk = 0; blk < numblocks_funct; ++blk)
        if (blk == 0)
            Divfree_hpmat_mod_lvls[0]->MultTranspose(inFunctvec.GetBlock(0), inFunctHcurlvec.GetBlock(0));
        else
            inFunctHcurlvec.GetBlock(blk) = inFunctvec.GetBlock(blk);

    for (int blk = 0; blk < numblocks_funct; ++blk)
    {
        const Array<int> *temp;
        if (blk == 0)
            temp = EssBdrTrueDofs_Hcurl[0];
        else
            temp = EssBdrTrueDofs_Funct_lvls[0][blk];
        for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
        {
            if ( fabs(inFunctHcurlvec.GetBlock(blk)[(*temp)[tdofind]]) > 1.0e-14 )
                std::cout << "bnd cnd is violated for inFunctHcurlvec, blk = " << blk << ",  value = "
                          << inFunctHcurlvec.GetBlock(blk)[(*temp)[tdofind]]
                          << ", index = " << (*temp)[tdofind] << "\n";
        }
    }
    /*
     * checking the Divfree and Divfree_T operators
#ifdef CHECK_BNDCND
    MFEM_ASSERT(strcmp(space_for_S,"L2") == 0, "Right now the check works only for S in L2 case!\n");
    auto Divfree_T = Divfree_hpmat_mod_lvls[0]->Transpose();
    MPI_Barrier(comm);
    for (int i = 0; i < num_procs; ++i)
    {
        if (myid == i)
        {
            std::cout << "I am " << myid << "\n";

            const Array<int> *temp2 = EssBdrTrueDofs_Funct_lvls[0][0];

            Array<int> bndtdofs_Hdiv(R_space_lvls[0]->TrueVSize());
            bndtdofs_Hdiv = 0;
            //std::cout << "bnd tdofs Hdiv \n";
            for ( int tdofind = 0; tdofind < temp2->Size(); ++tdofind)
            {
                //std::cout << (*temp2)[tdofind] << " ";
                bndtdofs_Hdiv[(*temp2)[tdofind]] = 1;
            }
            //std::cout << "\n";


            const Array<int> *temp = EssBdrTrueDofs_Hcurl[0];

            Array<int> bndtdofs_Hcurl(C_space_lvls[0]->TrueVSize());
            bndtdofs_Hcurl = 0;
            //std::cout << "bnd tdofs Hcurl \n";
            for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
            {
                //std::cout << (*temp)[tdofind] << " ";
                bndtdofs_Hcurl[(*temp)[tdofind]] = 1;
            }
            //std::cout << "\n";

            int special_row;
            bool found = false;

            for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
            {
                if ( fabs(inHcurlvec[(*temp)[tdofind]]) > 1.0e-14 )
                {
                    std::cout << "bnd cnd is violated for inHcurlvec, value = "
                              << inHcurlvec[(*temp)[tdofind]]
                              << ", index = " << (*temp)[tdofind] << "\n";
                    std::cout << " ... was corrected \n";
                    if (found == false)
                    {
                        special_row = (*temp)[tdofind];
                        found = true;
                    }
                }
                inHcurlvec[(*temp)[tdofind]] = 0.0;
            }

            if (found)
            {
                int special_col2;
                int special_col3;
                bool found2 = false;
                bool found3 = false;

                SparseMatrix spmat;
                Divfree_T->GetDiag(spmat);

                {
                    std::cout << "Looking for incorrect values in the diag part of Divfree_T \n";
                    int row = special_row;
                    int row_shift = spmat.GetI()[row];
                    std::cout << "row = " << row << "\n";
                    for (int j = 0; j < spmat.RowSize(row); ++j)
                    {
                        int col = spmat.GetJ()[row_shift + j];
                        double value = spmat.GetData()[row_shift + j];
                        if (fabs(value) > 1.0e-14)
                        {
                            std::cout << "(" << col << ", " << value << ") ";
                            std::cout << "for hdivvec value = " << inHdivvec[col] << " ";
                            if (bndtdofs_Hdiv[col] != 0)
                                std::cout << " at the boundary! ";
                            else
                            {
                                std::cout << "not at the boundary! ";
                                found2 = true;
                                special_col2 = col;
                            }

                        }
                    }
                    std::cout << "\n";
                }

                SparseMatrix spmat_offd;
                int * cmap_offd;
                Divfree_T->GetOffd(spmat_offd, cmap_offd);

                {
                    std::cout << "Looking for incorrect values in the offd part of Divfree_T \n";
                    int row = special_row;
                    int row_shift = spmat_offd.GetI()[row];
                    std::cout << "row = " << row << "\n";
                    for (int j = 0; j < spmat_offd.RowSize(row); ++j)
                    {
                        int col = spmat_offd.GetJ()[row_shift + j];
                        int truecol = cmap_offd[col];
                        double value = spmat_offd.GetData()[row_shift + j];
                        if (fabs(value) > 1.0e-14)
                        {
                            std::cout << "col = " << col << ": (" << truecol << ", " << value << ") ";
                            //std::cout << "for hdivvec value = " << inHdivvec[col] << " ";
                            if (bndtdofs_Hdiv[col] != 0)
                                std::cout << " at the boundary! ";
                            else
                            {
                                std::cout << "not at the boundary! ";
                                found3 = true;
                                special_col3 = truecol;
                            }

                        }
                    }
                    std::cout << "\n";
                }

            }

        }
        MPI_Barrier(comm);
    }
    delete Divfree_T;

#endif // for CHECK_BND
    */

    //MPI_Finalize();
    //return 0;

    // studying why inHcurlvec has nonzeros at the boundary
    /*
    {
        std::cout << "bnd indices for Hdiv \n";
        const Array<int> *temp = EssBdrTrueDofs_Funct_lvls[0][0];
        for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
        {
            std::cout << (*temp)[tdofind] << " ";
        }
        std::cout << "\n";
    }

    {
        std::cout << "bnd indices for Hcurl \n";
        const Array<int> *temp = EssBdrTrueDofs_Hcurl[0];
        for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
        {
            std::cout << (*temp)[tdofind] << " ";
        }
        std::cout << "\n";
    }


    SparseMatrix spmat;
    Divfree_hpmat_mod_lvls[0]->Transpose()->GetDiag(spmat);

    {
        int row = 2;
        int row_shift = spmat.GetI()[row];
        std::cout << "row = " << row << "\n";
        for (int j = 0; j < spmat.RowSize(row); ++j)
        {
            std::cout << "(" << spmat.GetJ()[row_shift + j] << ", " << spmat.GetData()[row_shift + j] << ") ";
            std::cout << "for hdivvec value = " << inHdivvec[spmat.GetJ()[row_shift + j]] << " ";
        }
        std::cout << "\n";
    }

    SparseMatrix spmat2;
    Divfree_hpmat_mod_lvls[0]->GetDiag(spmat2);
    {
        int row = 2;
        int row_shift = spmat2.GetI()[row];
        std::cout << "row = " << row << "\n";
        for (int j = 0; j < spmat2.RowSize(row); ++j)
        {
            std::cout << "(" << spmat2.GetJ()[row_shift + j] << ", " << spmat2.GetData()[row_shift + j] << ") ";
        }
        std::cout << "\n";
    }


    MPI_Finalize();
    return 0;
    */

    //HypreParMatrix * A_coarse = ((Multigrid*) (&(((BlockDiagonalPreconditioner*)prec)->GetDiagonalBlock(0))))->GetCoarseOp();
    //Array2D<HypreParMatrix*> CoarseOperator(numblocks_funct, numblocks_funct);
    //CoarseOperator(0,0) = A_coarse;
    //((CoarsestProblemHcurlSolver*)CoarsestSolver)->SetCoarseOperator(CoarseOperator);

    BlockVector outFunctvec(offsets_new);

    if (verbose)
        std::cout << "Computing action for the new MG ... \n";
    NewSolver.Mult(inFunctvec, outFunctvec);

    for (int blk = 0; blk < numblocks_funct; ++blk)
    {
        const Array<int> *temp;
        temp = EssBdrTrueDofs_Funct_lvls[0][blk];
        for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
        {
            if ( fabs(outFunctvec.GetBlock(blk)[(*temp)[tdofind]]) > 1.0e-14 )
                std::cout << "bnd cnd is violated for outFunctvec, blk = " << blk << ",  value = "
                          << outFunctvec.GetBlock(blk)[(*temp)[tdofind]]
                          << ", index = " << (*temp)[tdofind] << "\n";
        }
    }

    BlockVector outFunctHcurlvec(offsets_hcurlfunct_new);
    outFunctHcurlvec = 0.0;
    if (verbose)
        std::cout << "Computing action for the geometric MG ... \n";
    prec->Mult(inFunctHcurlvec, outFunctHcurlvec);

    for (int blk = 0; blk < numblocks_funct; ++blk)
    {
        const Array<int> *temp;
        if (blk == 0)
            temp = EssBdrTrueDofs_Hcurl[0];
        else
            temp = EssBdrTrueDofs_Funct_lvls[0][blk];
        for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
        {
            if ( fabs(outFunctHcurlvec.GetBlock(blk)[(*temp)[tdofind]]) > 1.0e-14 )
                std::cout << "bnd cnd is violated for outFunctHcurlvec, blk = " << blk << ",  value = "
                          << outFunctHcurlvec.GetBlock(blk)[(*temp)[tdofind]]
                          << ", index = " << (*temp)[tdofind] << "\n";
        }
    }

    BlockVector out2Functvec(offsets_new);
    for (int blk = 0; blk < numblocks_funct; ++blk)
        if (blk == 0)
            Divfree_hpmat_mod_lvls[0]->Mult(outFunctHcurlvec.GetBlock(0), out2Functvec.GetBlock(0));
        else
            out2Functvec.GetBlock(blk) = outFunctHcurlvec.GetBlock(blk);

    BlockVector diff(offsets_new);
    diff = outFunctvec;
    diff -= out2Functvec;

    /*
    std::cout << "blk 0 \n";
    diff.GetBlock(0).Print();
    if (numblocks_funct > 1)
    {
        std::cout << "blk 1 \n";
        diff.GetBlock(1).Print();
    }
    */

    double diff_norm = diff.Norml2() / sqrt (diff.Size());
    double geommg_norm = out2Functvec.Norml2() / sqrt(out2Functvec.Size());
    if (verbose)
    {
        std::cout << "|| NewMG * vec - C MG * C^T vec || = " << diff_norm << "\n";
        std::cout << "|| NewMG * vec - C MG * C^T vec || / || C MG * C^T vec || = " << diff_norm / geommg_norm << "\n";
    }

    for (int blk = 0; blk < numblocks_funct; ++blk)
    {
        double diffblk_norm = diff.GetBlock(blk).Norml2() / sqrt (diff.GetBlock(blk).Size());
        if (verbose)
        {
            std::cout << "|| NewMG * vec - C MG * C^T vec ||, block " << blk << " = " << diffblk_norm << "\n";
        }
    }

    // checking that A is exactly CT Funct_0 C in serial
    SparseMatrix diag1;
    A->GetDiag(diag1);

    HypreParMatrix * A_Funct = RAP(Divfree_hpmat_mod_lvls[0], (*Funct_hpmat_lvls[0])(0,0), Divfree_hpmat_mod_lvls[0] );

    SparseMatrix diag2;
    A_Funct->GetDiag(diag2);

    SparseMatrix diag2_copy(diag2);
    diag2_copy.Add(-1.0, diag1);

    std::cout << "diag(A) - diag(CT Funct_0 C) norm = " << diag2_copy.MaxNorm() << "\n";

    // checking that A has 1's on the diagonal and 0's for other columns for boundary entries
    /*
    MPI_Barrier(comm);
    for (int i = 0; i < num_procs; ++i)
    {
        if (myid == i)
        {
            std::cout << "I am " << myid << "\n";

            const Array<int> *temp = EssBdrTrueDofs_Hcurl[0];

            Array<int> bndtdofs(C_space_lvls[0]->TrueVSize());
            bndtdofs = 0;
            for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
            {
                //std::cout << (*temp)[tdofind] << " ";
                bndtdofs[(*temp)[tdofind]] = 1;
            }
            //std::cout << "\n";

            //if (verbose)
                //bndtdofs.Print();

            int tdof_offset = C_space_lvls[0]->GetMyTDofOffset();

            SparseMatrix diag;
            A->GetDiag(diag);

            std::cout << "Checking diagonal part of A in geom mg \n";
            for (int row = 0; row < diag.Height(); ++row)
            {
                if ( bndtdofs[row + tdof_offset] != 0)
                {
                    int nnz_shift = diag.GetI()[row];
                    for (int j = 0; j < diag.RowSize(row); ++j)
                    {
                        int col = diag.GetJ()[nnz_shift + j];
                        if ( col != row && fabs(diag.GetData()[nnz_shift + j]) > 1.0e-14 )
                        {
                            if (bndtdofs[col + tdof_offset] == 0)
                            {
                                std::cout << "Found nonzero for the boundary row = " << row << "(" << col << ", " << diag.GetData()[nnz_shift + j] << ") \n";
                                std::cout << "which lives not on the boundary! \n";
                            }
                        }
                    }
                } // end of if row is for the boundary tdof
            }// end of loop over rows
        }
        MPI_Barrier(comm);
    }
    MPI_Finalize();
    return 0;
    */

#ifdef COMPARE_SMOOTHERS

    if (verbose)
        std::cout << " \nComparing separately smoothers \n";

    for (int l = 0; l < num_levels - 1; ++l)
    {
        if (verbose)
            std::cout << "level: " << l << "\n";

        BlockVector outSmooHdivvec(offsets_new);
        Smoothers_lvls[0]->Mult(inFunctvec, outSmooHdivvec);

        //std::cout << "inFunctvec \n";
        //inFunctvec.Print();

        //std::cout << "outSmooHdivvec\n";
        //outSmooHdivvec.Print();

        /*
        inFunctvec = outSmooHdivvec; // iter no 2

        //std::cout << "outSmooHdivvec after the 1st iteration \n";
        //outSmooHdivvec.Print();

        Smoothers_lvls[0]->Mult(inFunctvec, outSmooHdivvec);
        */

        //Vector outSmooHdivvec(Smoothers_lvls[0]->Height());
        //Smoothers_lvls[0]->Mult(inHdivvec, outSmooHdivvec);

        HypreSmoother * Smoothers_fromMG_0 = new HypreSmoother(*A, HypreSmoother::Type::l1GS, 1);
        HypreSmoother * Smoothers_fromMG_1;
        if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
            Smoothers_fromMG_1 = new HypreSmoother(*C, HypreSmoother::Type::l1GS, 1);

        BlockVector outSmooHcurlvec(offsets_hcurlfunct_new);
        Smoothers_fromMG_0->Mult(inFunctHcurlvec.GetBlock(0), outSmooHcurlvec.GetBlock(0));
        if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
            Smoothers_fromMG_1->Mult(inFunctHcurlvec.GetBlock(1), outSmooHcurlvec.GetBlock(1));

        /*
        //std::cout << "outSmooHdivvec after the 1st iteration \n";
        //outSmooHdivvec.Print();

        inFunctHcurlvec = outSmooHcurlvec; // iter no 2
        Smoothers_fromMG_0->Mult(inFunctHcurlvec.GetBlock(0), outSmooHcurlvec.GetBlock(0));
        if (strcmp(space_for_S,"H1") == 0 || !eliminateS)
            Smoothers_fromMG_1->Mult(inFunctHcurlvec.GetBlock(1), outSmooHcurlvec.GetBlock(1));
        */

        //Vector outSmooHcurlvec(Smoothers_fromMG_0->Height());
        //Smoothers_fromMG_0->Mult(inHcurlvec, outSmooHcurlvec);

#ifdef CHECK_BNDCND
        for (int blk = 0; blk < numblocks_funct; ++blk)
        {
            const Array<int> *temp;
            if (blk == 0)
                temp = EssBdrTrueDofs_Hcurl[0];
            else
                temp = EssBdrTrueDofs_Funct_lvls[0][blk];

            for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
            {
                //std::cout << "index = " << (*temp)[tdofind] << "\n";
                if ( fabs(outSmooHcurlvec.GetBlock(blk)[(*temp)[tdofind]]) > 1.0e-14 )
                {
                    std::cout << "bnd cnd is violated for outSmooHcurlvec, blk = " << blk << ", value = "
                              << outSmooHcurlvec.GetBlock(blk)[(*temp)[tdofind]]
                              << ", index = " << (*temp)[tdofind] << "\n";
                    //std::cout << "... was corrected \n";
                    //outSmooHcurlvec.GetBlock(blk)[(*temp)[tdofind]] = 0.0;
                }
            }
        }
#endif

        BlockVector out2SmooHdivvec(offsets_new);
        for (int blk = 0; blk < numblocks_funct; ++blk)
        {
            if (blk == 0)
                Divfree_hpmat_mod_lvls[0]->Mult(outSmooHcurlvec.GetBlock(0), out2SmooHdivvec.GetBlock(0));
            else
                out2SmooHdivvec.GetBlock(blk) = outSmooHcurlvec.GetBlock(blk);
        }

        //Vector out2SmooHdivvec(Divfree_hpmat_mod_lvls[0]->Height());
        //Divfree_hpmat_mod_lvls[0]->Mult(outSmooHcurlvec, out2SmooHdivvec);

        BlockVector diffsmoo(offsets_new);
        //Vector diffsmoo(R_space_lvls[0]->TrueVSize());
        diffsmoo = outSmooHdivvec;
        diffsmoo -= out2SmooHdivvec;

        MPI_Barrier(comm);
        for (int i = 0; i < num_procs; ++i)
        {
            if (myid == i)
            {
                std::cout << "I am " << myid << "\n";

                double diffsmoo_norm = diffsmoo.Norml2() / sqrt (diffsmoo.Size());
                double geommgsmoo_norm = out2SmooHdivvec.Norml2() / sqrt(out2SmooHdivvec.Size());
                std::cout << "|| diff of smoothers action || = " << diffsmoo_norm << "\n";
                std::cout << "|| diff of smoothers action || / || geommg smoother action || = " << diffsmoo_norm / geommgsmoo_norm << "\n";
            }
            MPI_Barrier(comm);
        }
    }

    //MPI_Finalize();
    //return 0;

#endif

#if 0
    HypreParMatrix * prod1 = ParMult(Divfree_hpmat_mod_lvls[0], TrueP_C[0]);
    SparseMatrix diag_prod1;
    prod1->GetDiag(diag_prod1);

    HypreParMatrix * prod2 = ParMult(TrueP_R[0], Divfree_hpmat_mod_lvls[1]);
    SparseMatrix diag_prod2;
    prod2->GetDiag(diag_prod2);

    SparseMatrix diag_prod2_copy(diag_prod2);
    diag_prod2_copy.Add(-1.0, diag_prod1);

    //diag_prod2.Print();
    MPI_Barrier(comm);
    for (int i = 0; i < num_procs; ++i)
    {
        if (myid == i)
        {
            const Array<int> *temp2 = EssBdrTrueDofs_Funct_lvls[0][0];

            Array<int> bndtdofs_Hdiv(R_space_lvls[0]->TrueVSize());
            bndtdofs_Hdiv = 0;
            //std::cout << "bnd tdofs Hdiv \n";
            for ( int tdofind = 0; tdofind < temp2->Size(); ++tdofind)
            {
                //std::cout << (*temp2)[tdofind] << " ";
                bndtdofs_Hdiv[(*temp2)[tdofind]] = 1;
            }
            //std::cout << "\n";


            const Array<int> *temp = EssBdrTrueDofs_Hcurl[1];

            Array<int> bndtdofs_Hcurl(C_space_lvls[1]->TrueVSize());
            bndtdofs_Hcurl = 0;
            //std::cout << "bnd tdofs Hcurl \n";
            for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
            {
                bndtdofs_Hcurl[(*temp)[tdofind]] = 1;
            }

            std::cout << "I am " << myid << "\n";

            for (int i = 0; i < diag_prod2_copy.Height(); ++i)
            {
                for (int j = 0; j < diag_prod2_copy.RowSize(i); ++j)
                {
                    int col = diag_prod2_copy.GetJ()[diag_prod2_copy.GetI()[i] + j];
                    if (fabs(diag_prod2_copy.GetData()[diag_prod2_copy.GetI()[i] + j]) > 1.0e-13)
                    {
                        if (!(bndtdofs_Hdiv[i] != 0 && bndtdofs_Hcurl[col] != 0) )
                        {
                            std::cout << "nonzero entry of type ";
                            if (bndtdofs_Hdiv[i] != 0)
                                std::cout << "b-";
                            else
                                std::cout << "i-";
                            if (bndtdofs_Hcurl[col] != 0)
                                std::cout << "-b ";
                            else
                                std::cout << "-i ";
                            std::cout << ": (" << i << ", " << col << ", " << diag_prod1.GetData()[diag_prod1.GetI()[i] + j] << ") vs ";
                            std::cout << " (" << i << ", " << col << ", " << diag_prod2.GetData()[diag_prod2.GetI()[i] + j] << ") \n";
                        }
                        else
                        {
                            std::cout << "for bb nonzero entry, (" << i << ", " << col << ", " << diag_prod1.GetData()[diag_prod1.GetI()[i] + j] << ") vs ";
                            std::cout << " (" << i << ", " << col << ", " << diag_prod2.GetData()[diag_prod2.GetI()[i] + j] << ") \n";
                        }
                    }
                }
            }
        }
        MPI_Barrier(comm);
    }

    if (verbose)
        std::cout << "diag(P_R C1) - diag(C_0 P_C) norm = " << diag_prod2_copy.MaxNorm() << "\n";
#endif // for #if 0

#ifdef COMPARE_COARSE_SOLVERS
    if (verbose)
        std::cout << " \nComparing separately coarse level solvers \n";

    /*
    if (verbose)
        std::cout << " \nComparing coarsest level matrices \n";
    {
        SparseMatrix diag1;
        //A->GetDiag(diag1);
        HypreParMatrix * A_coarse = ((Multigrid*) (&(((BlockDiagonalPreconditioner*)prec)->GetDiagonalBlock(0))))->GetCoarseOp();
        A_coarse->GetDiag(diag1);

        SparseMatrix diag2;
        //std::cout << "size of Divfree_hpmat_mod_lvls[1] = " << Divfree_hpmat_mod_lvls[0]->Height() << " x " << Divfree_hpmat_mod_lvls[0]->Width() << "\n";
        //std::cout << "size of (*Funct_hpmat_lvls[1])(0,0)] = " << (*Funct_hpmat_lvls[1])(0,0)->Height() << " x " << (*Funct_hpmat_lvls[1])(0,0)->Width() << "\n";
        HypreParMatrix * HcurlOp = RAP(Divfree_hpmat_mod_lvls[1], (*Funct_hpmat_lvls[1])(0,0), Divfree_hpmat_mod_lvls[1]);
        HcurlOp->GetDiag(diag2);

        diag2.Add(-1.0, diag1);

        if (verbose)
            std::cout << "diag2 - diag1 norm = " << diag2.MaxNorm() << "\n";

        HypreParMatrix * tempm = RAP(TrueP_R[0], (*Funct_hpmat_lvls[0])(0,0), TrueP_R[0] );

        HypreParMatrix * HcurlOp_2 = RAP(Divfree_hpmat_mod_lvls[1], tempm, Divfree_hpmat_mod_lvls[1]);

        SparseMatrix diag3;
        HcurlOp_2->GetDiag(diag3);

        diag3.Add(-1.0, diag1);

        if (verbose)
            std::cout << "diag3 - diag1 norm = " << diag3.MaxNorm() << "\n";

        HypreParMatrix * tempmm = RAP(Divfree_hpmat_mod_lvls[0], (*Funct_hpmat_lvls[0])(0,0), Divfree_hpmat_mod_lvls[0]);
        HypreParMatrix * HcurlOp_3 = RAP(TrueP_C[0], tempmm, TrueP_C[0] );

        SparseMatrix diag4;
        HcurlOp_3->GetDiag(diag4);

        diag4.Add(-1.0, diag1);

        if (verbose)
            std::cout << "diag4 - diag1 norm = " << diag4.MaxNorm() << "\n";

        MPI_Finalize();
        return 0;
    }
    */

    // comparison at the coarsest level
    /*
    Vector inCoarseHdivvec(CoarsestSolver->Width());
    TrueP_R[0]->MultTranspose(inHdivvec, inCoarseHdivvec); // project

    {
        const Array<int> *temp = EssBdrTrueDofs_Funct_lvls[1][0];
        for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
        {
            inCoarseHdivvec[(*temp)[tdofind]] = 0.0;
        }

    }

#ifdef CHECK_BNDCND
    {
        const Array<int> *temp = EssBdrTrueDofs_Funct_lvls[1][0];
        for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
        {
            if ( fabs(inCoarseHdivvec[(*temp)[tdofind]]) > 1.0e-14 )
            {
                std::cout << "bnd cnd is violated for inCoarseHdivvec, value = "
                          << inCoarseHdivvec[(*temp)[tdofind]]
                          << ", index = " << (*temp)[tdofind] << "\n";
                std::cout << " ... was corrected \n";
            }
            inCoarseHdivvec[(*temp)[tdofind]] = 0.0;
        }

    }
#endif

    Vector outCoarseHdivvec(CoarsestSolver->Height());
    CoarsestSolver->Mult(inCoarseHdivvec, outCoarseHdivvec); // coarse solve

    Vector inCoarseHcurlvec( Divfree_hpmat_mod_lvls[1]->Width());
    Divfree_hpmat_mod_lvls[1]->MultTranspose(inCoarseHdivvec, inCoarseHcurlvec); // move to coarse Hcurl

#ifdef CHECK_BNDCND
    {
        const Array<int> *temp = EssBdrTrueDofs_Hcurl[1];
        for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
        {
            if ( fabs(inCoarseHcurlvec[(*temp)[tdofind]]) > 1.0e-14 )
            {
                std::cout << "bnd cnd is violated for inCoarseHcurlvec, value = "
                          << inCoarseHcurlvec[(*temp)[tdofind]]
                          << ", index = " << (*temp)[tdofind] << "\n";
                std::cout << " ... was corrected \n";
            }
            inCoarseHcurlvec[(*temp)[tdofind]] = 0.0;
        }
    }
#endif
    CGSolver * Geommg_Coarsesolver = ((Multigrid*) (&(((BlockDiagonalPreconditioner*)prec)->GetDiagonalBlock(0))))->GetCoarseSolver();
    Vector outCoarseHcurlvec(Geommg_Coarsesolver->Height());
    Geommg_Coarsesolver->Mult(inCoarseHcurlvec, outCoarseHcurlvec); // solve in coarse Hcurl

    Vector out2CoarseHdivvec(Divfree_hpmat_mod_lvls[1]->Height());
    Divfree_hpmat_mod_lvls[1]->Mult(outCoarseHcurlvec, out2CoarseHdivvec); // move to coarse Hdiv back

    Vector diffcoarse(R_space_lvls[1]->TrueVSize());
    diffcoarse = outCoarseHdivvec;
    diffcoarse -= out2CoarseHdivvec;

    for (int i = 0; i < diffcoarse.Size(); ++i)
        if (fabs(diffcoarse[i] > 1.0e-13))
            std::cout << "nonzero entry: (" << i << ", " << diffcoarse[i] << ") \n";

    double diffcoarse_norm = diffcoarse.Norml2() / sqrt (diffcoarse.Size());
    double geommgcoarse_norm = out2CoarseHdivvec.Norml2() / sqrt(out2CoarseHdivvec.Size());
    if (verbose)
    {
        std::cout << "|| diff of coarse solvers action || = " << diffcoarse_norm << "\n";
        std::cout << "|| diff of coarse solvers action || / || geommg coarse solver action || = " << diffcoarse_norm / geommgcoarse_norm << "\n";
    }
    */

    // comparison with transfers from and to the finest level

#ifdef CHECK_BNDCND
    {
        const Array<int> *temp = EssBdrTrueDofs_Hcurl[0];
        for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
        {
            if ( fabs(inHcurlvec[(*temp)[tdofind]]) > 1.0e-14 )
            {
                std::cout << "bnd cnd is violated for inHcurlvec, value = "
                          << inHcurlvec[(*temp)[tdofind]]
                          << ", index = " << (*temp)[tdofind] << "\n";
                //std::cout << " ... was corrected \n";
            }
            //inHcurlvec[(*temp)[tdofind]] = 0.0;
        }

    }
#endif

    Vector inCoarseHdivvec(CoarsestSolver->Width());
    TrueP_R[0]->MultTranspose(inHdivvec, inCoarseHdivvec); // project

    Vector outCoarseHdivvec(CoarsestSolver->Height());
    CoarsestSolver->Mult(inCoarseHdivvec, outCoarseHdivvec); // coarse solve

#ifdef CHECK_BNDCND
    {
        const Array<int> *temp = EssBdrTrueDofs_Funct_lvls[1][0];
        for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
        {
            if ( fabs(outCoarseHdivvec[(*temp)[tdofind]]) > 1.0e-14 )
            {
                std::cout << "bnd cnd is violated for outCoarseHdivvec, value = "
                          << outCoarseHdivvec[(*temp)[tdofind]]
                          << ", index = " << (*temp)[tdofind] << "\n";
                //std::cout << " ... was corrected \n";
            }
            //outCoarseHdivvec[(*temp)[tdofind]] = 0.0;
        }

    }
#endif

    Vector outFineCoarseHdivvec(TrueP_R[0]->Height());
    TrueP_R[0]->Mult(outCoarseHdivvec, outFineCoarseHdivvec); // interpolate back

#ifdef CHECK_BNDCND
    {
        const Array<int> *temp = EssBdrTrueDofs_Funct_lvls[0][0];
        for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
        {
            if ( fabs(outFineCoarseHdivvec[(*temp)[tdofind]]) > 1.0e-14 )
            {
                std::cout << "bnd cnd is violated for outFineCoarseHdivvec, value = "
                          << outFineCoarseHdivvec[(*temp)[tdofind]]
                          << ", index = " << (*temp)[tdofind] << "\n";
                //std::cout << " ... was corrected \n";
            }
            //outFoneCoarseHdivvec[(*temp)[tdofind]] = 0.0;
        }

    }
#endif
    Vector inCoarseHcurlvec(TrueP_C[0]->Width());
    TrueP_C[0]->MultTranspose(inHcurlvec, inCoarseHcurlvec); // project after moving from Hcurl

#ifdef CHECK_BNDCND
    {
        const Array<int> *temp = EssBdrTrueDofs_Hcurl[1];
        for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
        {
            if ( fabs(inCoarseHcurlvec[(*temp)[tdofind]]) > 1.0e-14 )
            {
                std::cout << "bnd cnd is violated for inCoarseHcurlvec, value = "
                          << inCoarseHcurlvec[(*temp)[tdofind]]
                          << ", index = " << (*temp)[tdofind] << "\n";
                std::cout << " ... was corrected \n";
            }
            inCoarseHcurlvec[(*temp)[tdofind]] = 0.0;
        }

    }
#endif

    // checking that at coarse level Curl^T * Hdivvec = Hcurlvec
    Vector check(Divfree_hpmat_mod_lvls[1]->Width());
    Divfree_hpmat_mod_lvls[1]->MultTranspose(inCoarseHdivvec, check);
    check -= inCoarseHcurlvec;
    std::cout << "check_norm = " << check.Norml2() / sqrt (check.Size()) << "\n";

    MPI_Barrier(comm);
    std::cout << std::flush;
    MPI_Barrier(comm);

    CGSolver * Geommg_Coarsesolver = ((Multigrid*) (&(((BlockDiagonalPreconditioner*)prec)->GetDiagonalBlock(0))))->GetCoarseSolver();
    Vector outCoarseHcurlvec(Geommg_Coarsesolver->Height());
    Geommg_Coarsesolver->Mult(inCoarseHcurlvec, outCoarseHcurlvec); // solve

#ifdef CHECK_BNDCND
    {
        const Array<int> *temp = EssBdrTrueDofs_Hcurl[1];
        for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
        {
            if ( fabs(outCoarseHcurlvec[(*temp)[tdofind]]) > 1.0e-14 )
            {
                std::cout << "bnd cnd is violated for outCoarseHcurlvec, value = "
                          << outCoarseHcurlvec[(*temp)[tdofind]]
                          << ", index = " << (*temp)[tdofind] << "\n";
                std::cout << " ... was corrected \n";
            }
            outCoarseHcurlvec[(*temp)[tdofind]] = 0.0;
        }

    }
#endif

    Vector outFineCoarseHcurlvec(TrueP_C[0]->Height());
    TrueP_C[0]->Mult(outCoarseHcurlvec, outFineCoarseHcurlvec);   // interpolate back

#ifdef CHECK_BNDCND
    {
        const Array<int> *temp = EssBdrTrueDofs_Hcurl[0];
        for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
        {
            if ( fabs(outFineCoarseHcurlvec[(*temp)[tdofind]]) > 1.0e-14 )
            {
                std::cout << "bnd cnd is violated for outFineCoarseHcurlvec, value = "
                          << outCoarseHcurlvec[(*temp)[tdofind]]
                          << ", index = " << (*temp)[tdofind] << "\n";
                //std::cout << " ... was corrected \n";
            }
            //outFineCoarseHcurlvec[(*temp)[tdofind]] = 0.0;
        }

    }
#endif
    Vector out2FineCoarseHdivvec(Divfree_hpmat_mod_lvls[0]->Height());
    Divfree_hpmat_mod_lvls[0]->Mult(outFineCoarseHcurlvec, out2FineCoarseHdivvec); // move to Hdiv back

#ifdef CHECK_BNDCND
    {
        const Array<int> *temp = EssBdrTrueDofs_Funct_lvls[0][0];
        for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
        {
            if ( fabs(out2FineCoarseHdivvec[(*temp)[tdofind]]) > 1.0e-14 )
            {
                std::cout << "bnd cnd is violated for out2FineCoarseHdivvec, value = "
                          << out2FineCoarseHdivvec[(*temp)[tdofind]]
                          << ", index = " << (*temp)[tdofind] << "\n";
                //std::cout << " ... was corrected \n";
            }
            //out2FineCoarseHdivvec[(*temp)[tdofind]] = 0.0;
        }

    }
#endif

    Vector diffcoarse(R_space_lvls[0]->TrueVSize());
    diffcoarse = outFineCoarseHdivvec;
    diffcoarse -= out2FineCoarseHdivvec;

    //diffcoarse.Print();

    MPI_Barrier(comm);
    for (int i = 0; i < num_procs; ++i)
    {
        if (myid == i)
        {
            std::cout << "I am " << myid << "\n";

            double diffcoarse_norm = diffcoarse.Norml2() / sqrt (diffcoarse.Size());
            double geommgcoarse_norm = out2FineCoarseHdivvec.Norml2() / sqrt(out2FineCoarseHdivvec.Size());
            std::cout << "|| diff of coarse solvers action || = " << diffcoarse_norm << "\n";
            std::cout << "|| diff of coarse solvers action || / || geommg coarse solver action || = " << diffcoarse_norm / geommgcoarse_norm << "\n";
            std::cout << "\n" << std::flush;
        }
        MPI_Barrier(comm);
    }

    if (verbose)
        std::cout << " \nChecking the coarsest level matrix in geometric MG \n";

    {
        HypreParMatrix * A_coarse = ((Multigrid*) (&(((BlockDiagonalPreconditioner*)prec)->GetDiagonalBlock(0))))->GetCoarseOp();

        Vector testinCoarseHcurlvec(A_coarse->Width());
        testinCoarseHcurlvec = 1.0;
        {
            const Array<int> *temp = EssBdrTrueDofs_Hcurl[1];
            for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
            {
                std::cout << (*temp)[tdofind] << " ";
                testinCoarseHcurlvec[(*temp)[tdofind]] = 0.0;
            }
            std::cout << "\n";

        }

        Vector testCoarseHcurlvec(A_coarse->Height());
        A_coarse->Mult(inCoarseHcurlvec, testCoarseHcurlvec);
        {
            const Array<int> *temp = EssBdrTrueDofs_Hcurl[1];
            for ( int tdofind = 0; tdofind < temp->Size(); ++tdofind)
            {
                if ( fabs(testCoarseHcurlvec[(*temp)[tdofind]]) > 1.0e-14 )
                {
                    std::cout << "bnd cnd is violated for testCoarseHcurlvec, value = "
                              << testCoarseHcurlvec[(*temp)[tdofind]]
                              << ", index = " << (*temp)[tdofind] << "\n";
                }
            }

        }
    }

#endif // for #ifdef COMPARE_COARSE_SOLVERS

    MPI_Finalize();
    return 0;
#else
    int TestmaxIter(400);

    CGSolver Testsolver(MPI_COMM_WORLD);
    Testsolver.SetAbsTol(sqrt(atol));
    Testsolver.SetRelTol(sqrt(rtol));
    Testsolver.SetMaxIter(TestmaxIter);

    Testsolver.SetOperator(*hdivh1_op);
#ifdef NEW_INTERFACE2
    //Testsolver.SetOperator(*hdivh1_op);
    Testsolver.SetPreconditioner(*GeneralMGprec_plus);
#else
    //Testsolver.SetOperator(*BlockMattest);
    Testsolver.SetPreconditioner(NewSolver);
#endif // for ifdef NEW_INTERFACE2

    Testsolver.SetPrintLevel(1);

    trueXtest = 0.0;

    BlockVector trueRhstest_funct(blocktest_offsets);
    trueRhstest_funct = trueRhstest;

    // trueRhstest = F - Funct * particular solution (= residual), on true dofs
    BlockVector truetemp(blocktest_offsets);
    BlockMattest->Mult(ParticSol, truetemp);
    trueRhstest -= truetemp;

    chrono.Stop();
    if (verbose)
        std::cout << "Global system for the CG was built in " << chrono.RealTime() <<" seconds.\n";
    chrono.Clear();
    chrono.Start();

    Testsolver.Mult(trueRhstest, trueXtest);

    //trueXtest.Print();

    chrono.Stop();

    if (verbose)
    {
        if (Testsolver.GetConverged())
            std::cout << "Linear solver converged in " << Testsolver.GetNumIterations()
                      << " iterations with a residual norm of " << Testsolver.GetFinalNorm() << ".\n";
        else
            std::cout << "Linear solver did not converge in " << Testsolver.GetNumIterations()
                      << " iterations. Residual norm is " << Testsolver.GetFinalNorm() << ".\n";
        std::cout << "Linear solver (CG + new solver) took " << chrono.RealTime() << "s. \n";
        if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
            std::cout << "System size: " << Atest->M() + Ctest->M() << "\n" << std::flush;
        else
            std::cout << "System size: " << Atest->M() << "\n" << std::flush;
    }

    chrono.Clear();

#ifdef TIMING
    double temp_sum;

    temp_sum = 0.0;
    for (list<double>::iterator i = Times_mult->begin(); i != Times_mult->end(); ++i)
        temp_sum += *i;
    if (verbose)
        std::cout << "time_mult = " << temp_sum << "\n";
    delete Times_mult;
    temp_sum = 0.0;
    for (list<double>::iterator i = Times_solve->begin(); i != Times_solve->end(); ++i)
        temp_sum += *i;
    if (verbose)
        std::cout << "time_solve = " << temp_sum << "\n";
    delete Times_solve;
    temp_sum = 0.0;
    for (list<double>::iterator i = Times_localsolve->begin(); i != Times_localsolve->end(); ++i)
        temp_sum += *i;
    if (verbose)
        std::cout << "time_localsolve = " << temp_sum << "\n";
    delete Times_localsolve;

    for (int l = 0; l < num_levels - 1; ++l)
    {
        temp_sum = 0.0;
        for (list<double>::iterator i = Times_localsolve_lvls[l].begin(); i != Times_localsolve_lvls[l].end(); ++i)
            temp_sum += *i;
        if (verbose)
            std::cout << "time_localsolve lvl " << l << " = " << temp_sum << "\n";
    }
    //delete Times_localsolve_lvls;

    temp_sum = 0.0;
    for (list<double>::iterator i = Times_smoother->begin(); i != Times_smoother->end(); ++i)
        temp_sum += *i;
    if (verbose)
        std::cout << "time_smoother = " << temp_sum << "\n";
    delete Times_smoother;

    for (int l = 0; l < num_levels - 1; ++l)
    {
        temp_sum = 0.0;
        for (list<double>::iterator i = Times_smoother_lvls[l].begin(); i != Times_smoother_lvls[l].end(); ++i)
            temp_sum += *i;
        if (verbose)
            std::cout << "time_smoother lvl " << l << " = " << temp_sum << "\n";
    }
    //delete Times_smoother_lvls;
#ifdef WITH_SMOOTHERS
    for (int l = 0; l < num_levels - 1; ++l)
    {
        if (verbose)
        {
           std::cout << "Internal timing of the smoother at level " << l << ": \n";
           std::cout << "global mult time: " << ((HcurlGSSSmoother*)Smoothers_lvls[l])->GetGlobalMultTime() << " \n" << std::flush;
           std::cout << "internal mult time: " << ((HcurlGSSSmoother*)Smoothers_lvls[l])->GetInternalMultTime() << " \n" << std::flush;
           std::cout << "before internal mult time: " << ((HcurlGSSSmoother*)Smoothers_lvls[l])->GetBeforeIntMultTime() << " \n" << std::flush;
           std::cout << "after internal mult time: " << ((HcurlGSSSmoother*)Smoothers_lvls[l])->GetAfterIntMultTime() << " \n" << std::flush;
        }
    }
#endif
    temp_sum = 0.0;
    for (list<double>::iterator i = Times_coarsestproblem->begin(); i != Times_coarsestproblem->end(); ++i)
        temp_sum += *i;
    if (verbose)
        std::cout << "time_coarsestproblem = " << temp_sum << "\n";
    delete Times_coarsestproblem;

    MPI_Barrier(comm);
    temp_sum = 0.0;
    for (list<double>::iterator i = Times_resupdate->begin(); i != Times_resupdate->end(); ++i)
        temp_sum += *i;
    if (verbose)
        std::cout << "time_resupdate = " << temp_sum << "\n";
    delete Times_resupdate;

    temp_sum = 0.0;
    for (list<double>::iterator i = Times_fw->begin(); i != Times_fw->end(); ++i)
        temp_sum += *i;
    if (verbose)
        std::cout << "time_fw = " << temp_sum << "\n";
    delete Times_fw;
    temp_sum = 0.0;
    for (list<double>::iterator i = Times_up->begin(); i != Times_up->end(); ++i)
        temp_sum += *i;
    if (verbose)
        std::cout << "time_up = " << temp_sum << "\n";
    delete Times_up;
#endif

    chrono.Start();

    trueXtest += ParticSol;
    NewSigmahat->Distribute(trueXtest.GetBlock(0));

    if (strcmp(space_for_S,"H1") == 0)
        NewS->Distribute(trueXtest.GetBlock(1));

    /*
#ifdef OLD_CODE

    if (verbose)
        std::cout << "Using the new solver as a preconditioner for CG applied"
                     " to a saddle point problem for sigma and lambda \n";

    Array<int> block_Offsetstest(numblocks + 2); // number of variables + 1
    block_Offsetstest[0] = 0;
    block_Offsetstest[1] = R_space_lvls[0]->GetVSize();
    block_Offsetstest[2] = W_space_lvls[0]->GetVSize();
    block_Offsetstest.PartialSum();

    Array<int> block_trueOffsetstest(numblocks + 2); // number of variables + 1
    block_trueOffsetstest[0] = 0;
    block_trueOffsetstest[1] = R_space_lvls[0]->TrueVSize();
    block_trueOffsetstest[2] = W_space_lvls[0]->TrueVSize();
    block_trueOffsetstest.PartialSum();

    BlockVector trueXtest(block_trueOffsetstest), trueRhstest(block_trueOffsetstest);

    ConstantCoefficient zerostest(.0);

    ParLinearForm *fform = new ParLinearForm(R_space_lvls[0]);
    fform->AddDomainIntegrator(new VectordivDomainLFIntegrator(zerostest));
    fform->Assemble();

    ParLinearForm *gformtest;
    gformtest = new ParLinearForm(W_space_lvls[0]);
    gformtest->AddDomainIntegrator(new DomainLFIntegrator(*Mytest.GetRhs()));
    gformtest->Assemble();

    ParBilinearForm *Ablock(new ParBilinearForm(R_space));
    HypreParMatrix *Atest;
    Ablock->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.GetKtilda()));
    Ablock->Assemble();
    Ablock->EliminateEssentialBC(ess_bdrSigma, *sigma_exact_finest, *fform);
    Ablock->Finalize();
    Atest = Ablock->ParallelAssemble();

    HypreParMatrix *D;
    HypreParMatrix *DT;

    ParMixedBilinearForm *Dblock(new ParMixedBilinearForm(R_space_lvls[0], W_space_lvls[0]));
    Dblock->AddDomainIntegrator(new VectorFEDivergenceIntegrator);
    Dblock->Assemble();
    Dblock->EliminateTrialDofs(ess_bdrSigma, *sigma_exact_finest, *gformtest);
    Dblock->Finalize();
    D = Dblock->ParallelAssemble();
    DT = D->Transpose();

    fform->ParallelAssemble(trueRhstest.GetBlock(0));
    gformtest->ParallelAssemble(trueRhstest.GetBlock(1));

    Solver *prectest;
    prectest = new BlockDiagonalPreconditioner(block_trueOffsetstest);
    NewSolver.SetAsPreconditioner(true);
    NewSolver.SetPrintLevel(1);
    ((BlockDiagonalPreconditioner*)prectest)->SetDiagonalBlock(0, &NewSolver);

    HypreParMatrix *Schur;
    {
        HypreParMatrix *AinvDt = D->Transpose();
        HypreParVector *Ad = new HypreParVector(MPI_COMM_WORLD, Atest->GetGlobalNumRows(),
                                             Atest->GetRowStarts());

        Atest->GetDiag(*Ad);
        AinvDt->InvScaleRows(*Ad);
        Schur = ParMult(D, AinvDt);
    }

    Solver * precS;
    precS = new HypreBoomerAMG(*Schur);
    ((HypreBoomerAMG *)precS)->SetPrintLevel(0);
    ((HypreBoomerAMG *)precS)->iterative_mode = false;
    ((BlockDiagonalPreconditioner*)prectest)->SetDiagonalBlock(1, precS);


    BlockOperator *CFOSLSop = new BlockOperator(block_trueOffsetstest);
    CFOSLSop->SetBlock(0,0, Atest);
    CFOSLSop->SetBlock(0,1, DT);
    CFOSLSop->SetBlock(1,0, D);

    IterativeSolver * solvertest;
    solvertest = new CGSolver(comm);

    solvertest->SetAbsTol(atol);
    solvertest->SetRelTol(rtol);
    solvertest->SetMaxIter(max_num_iter);
    solvertest->SetOperator(*MainOp);

    solvertest->SetPrintLevel(0);
    solvertest->SetOperator(*CFOSLSop);
    solvertest->SetPreconditioner(*prectest);
    solvertest->SetPrintLevel(1);

    trueXtest = 0.0;
    trueXtest.GetBlock(0) = trueParticSol;

    chrono.Clear();
    chrono.Start();

    solvertest->Mult(trueRhstest, trueXtest);

    chrono.Stop();

    NewSigmahat->Distribute(&(trueXtest.GetBlock(0)));


    if (verbose)
    {
        if (solvertest->GetConverged())
            std::cout << "Linear solver converged in " << solvertest->GetNumIterations()
                      << " iterations with a residual norm of " << solvertest->GetFinalNorm() << ".\n";
        else
            std::cout << "Linear solver did not converge in " << solvertest->GetNumIterations()
                      << " iterations. Residual norm is " << solvertest->GetFinalNorm() << ".\n";
        std::cout << "Linear solver took " << chrono.RealTime() << "s. \n";
    }
#else
    *NewSigmahat = 0.0;
    std::cout << "OLD_CODE must be defined for using the new solver as a preconditioner \n";
#endif
    */

    {
        int order_quad = max(2, 2*feorder+1);
        const IntegrationRule *irs[Geometry::NumGeom];
        for (int i = 0; i < Geometry::NumGeom; ++i)
        {
            irs[i] = &(IntRules.Get(i, order_quad));
        }

        double norm_sigma = ComputeGlobalLpNorm(2, *Mytest.GetSigma(), *pmesh, irs);
        double err_newsigmahat = NewSigmahat->ComputeL2Error(*Mytest.GetSigma(), irs);
        if (verbose)
        {
            if ( norm_sigma > MYZEROTOL )
                cout << "|| new sigma_h - sigma_ex || / || sigma_ex || = " << err_newsigmahat / norm_sigma << endl;
            else
                cout << "|| new sigma_h || = " << err_newsigmahat << " (sigma_ex = 0)" << endl;
        }

        DiscreteLinearOperator Div(R_space, W_space);
        Div.AddDomainInterpolator(new DivergenceInterpolator());
        ParGridFunction DivSigma(W_space);
        Div.Assemble();
        Div.Mult(*NewSigmahat, DivSigma);

        double err_div = DivSigma.ComputeL2Error(*Mytest.GetRhs(),irs);
        double norm_div = ComputeGlobalLpNorm(2, *Mytest.GetRhs(), *pmesh, irs);

        if (verbose)
        {
            cout << "|| div (new sigma_h - sigma_ex) || / ||div (sigma_ex)|| = "
                      << err_div/norm_div  << "\n";
        }

        //////////////////////////////////////////////////////
        if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
        {
            double max_bdr_error = 0;
            for ( int dof = 0; dof < Xinit.GetBlock(1).Size(); ++dof)
            {
                if ( (*EssBdrDofs_Funct_lvls[0][1])[dof] != 0.0)
                {
                    //std::cout << "ess dof index: " << dof << "\n";
                    double bdr_error_dof = fabs(Xinit.GetBlock(1)[dof] - (*NewS)[dof]);
                    if ( bdr_error_dof > max_bdr_error )
                        max_bdr_error = bdr_error_dof;
                }
            }

            if (max_bdr_error > 1.0e-14)
                std::cout << "Error, boundary values for the solution (S) are wrong:"
                             " max_bdr_error = " << max_bdr_error << "\n";

            // 13. Extract the parallel grid function corresponding to the finite element
            //     approximation X. This is the local solution on each processor. Compute
            //     L2 error norms.

            int order_quad = max(2, 2*feorder+1);
            const IntegrationRule *irs[Geometry::NumGeom];
            for (int i=0; i < Geometry::NumGeom; ++i)
            {
               irs[i] = &(IntRules.Get(i, order_quad));
            }

            // Computing error for S

            double err_S = NewS->ComputeL2Error((*Mytest.GetU()), irs);
            double norm_S = ComputeGlobalLpNorm(2, (*Mytest.GetU()), *pmesh, irs);
            if (verbose)
            {
                std::cout << "|| S_h - S_ex || / || S_ex || = " <<
                             err_S / norm_S << "\n";
            }
        }
        /////////////////////////////////////////////////////////

        double localFunctional = -2.0 * (trueXtest * trueRhstest_funct); //0.0;//-2.0*(trueX.GetBlock(0)*trueRhs.GetBlock(0));
        BlockMattest->Mult(trueXtest, trueRhstest_funct);
        localFunctional += trueXtest * trueRhstest_funct;

        double globalFunctional;
        MPI_Reduce(&localFunctional, &globalFunctional, 1,
                   MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
        if (verbose)
        {
            //cout << "|| sigma_h - L(S_h) ||^2 + || div_h (bS_h) - f ||^2 = " << globalFunctional+err_div*err_div << "\n";
            //cout << "|| f ||^2 = " << norm_div*norm_div  << "\n";
            //cout << "Relative Energy Error = " << sqrt(globalFunctional+err_div*err_div)/norm_div << "\n";

            if (strcmp(space_for_S,"H1") == 0) // S is present
            {
                cout << "|| sigma_h - L(S_h) ||^2 + || div_h (bS_h) - f ||^2 = " << globalFunctional+err_div*err_div << "\n";
                cout << "|| f ||^2 = " << norm_div*norm_div  << "\n";
                cout << "Relative Energy Error = " << sqrt(globalFunctional+err_div*err_div)/norm_div << "\n";
            }
            else // if S is from L2
            {
                cout << "|| sigma_h - L(S_h) ||^2 + || div_h (sigma_h) - f ||^2 = " << globalFunctional+err_div*err_div << "\n";
                cout << "Energy Error = " << sqrt(globalFunctional+err_div*err_div) << "\n";
            }
        }
    }

    chrono.Stop();
    if (verbose)
        std::cout << "Errors in USE_AS_A_PREC were computed in " << chrono.RealTime() <<" seconds.\n";
    chrono.Clear();
    chrono.Start();
#endif

#else // for USE_AS_A_PREC

    if (verbose)
        std::cout << "\nCalling the new multilevel solver \n";

    chrono.Clear();
    chrono.Start();

    BlockVector NewRhs(new_trueoffsets);
    NewRhs = 0.0;

    if (numblocks_funct > 1)
    {
        if (verbose)
            std::cout << "This place works only for homogeneous boundary conditions \n";
        ParLinearForm *secondeqn_rhs;
        if (strcmp(space_for_S,"H1") == 0 || !eliminateS)
        {
            secondeqn_rhs = new ParLinearForm(H_space_lvls[0]);
            secondeqn_rhs->AddDomainIntegrator(new GradDomainLFIntegrator(*Mytest.GetBf()));
            secondeqn_rhs->Assemble();
            secondeqn_rhs->ParallelAssemble(NewRhs.GetBlock(1));

            delete secondeqn_rhs;

            for ( int i = 0; i < EssBdrTrueDofs_Funct_lvls[0][1]->Size(); ++i)
            {
                int bdrtdof = (*EssBdrTrueDofs_Funct_lvls[0][1])[i];
                NewRhs.GetBlock(1)[bdrtdof] = 0.0;
            }

        }
    }

    BlockVector NewX(new_trueoffsets);
    NewX = 0.0;

    MFEM_ASSERT(CheckConstrRes(ParticSol.GetBlock(0), *Constraint_global, &Floc, "in the main code for the ParticSol"), "blablabla");

    NewSolver.SetInitialGuess(ParticSol);
    //NewSolver.SetUnSymmetric(); // FIXME: temporarily, for debugging purposes!

    if (verbose)
        NewSolver.PrintAllOptions();

    chrono.Stop();
    if (verbose)
        std::cout << "NewSolver was prepared for solving in " << chrono.RealTime() <<" seconds.\n";
    chrono.Clear();
    chrono.Start();

    NewSolver.Mult(NewRhs, NewX);

    chrono.Stop();

    if (verbose)
    {
        std::cout << "Linear solver (new solver only) took " << chrono.RealTime() << "s. \n";
    }



#ifdef TIMING
    double temp_sum;
    /*
    for (int i = 0; i < num_procs; ++i)
    {
        if (myid == i && myid % 10 == 0)
        {
            std::cout << "I am " << myid << "\n";
            std::cout << "Look at my list for mult timings: \n";

            for (list<double>::iterator i = Times_mult->begin(); i != Times_mult->end(); ++i)
                std::cout << *i << " ";
            std::cout << "\n" << std::flush;
        }
        MPI_Barrier(comm);
    }
    */
    temp_sum = 0.0;
    for (list<double>::iterator i = Times_mult->begin(); i != Times_mult->end(); ++i)
        temp_sum += *i;
    if (verbose)
        std::cout << "time_mult = " << temp_sum << "\n";
    delete Times_mult;
    temp_sum = 0.0;
    for (list<double>::iterator i = Times_solve->begin(); i != Times_solve->end(); ++i)
        temp_sum += *i;
    if (verbose)
        std::cout << "time_solve = " << temp_sum << "\n";
    delete Times_solve;
    temp_sum = 0.0;
    for (list<double>::iterator i = Times_localsolve->begin(); i != Times_localsolve->end(); ++i)
        temp_sum += *i;
    if (verbose)
        std::cout << "time_localsolve = " << temp_sum << "\n";
    delete Times_localsolve;
    for (int l = 0; l < num_levels - 1; ++l)
    {
        temp_sum = 0.0;
        for (list<double>::iterator i = Times_localsolve_lvls[l].begin(); i != Times_localsolve_lvls[l].end(); ++i)
            temp_sum += *i;
        if (verbose)
            std::cout << "time_localsolve lvl " << l << " = " << temp_sum << "\n";
    }
    //delete Times_localsolve_lvls;

    temp_sum = 0.0;
    for (list<double>::iterator i = Times_smoother->begin(); i != Times_smoother->end(); ++i)
        temp_sum += *i;
    if (verbose)
        std::cout << "time_smoother = " << temp_sum << "\n";
    delete Times_smoother;

    for (int l = 0; l < num_levels - 1; ++l)
    {
        temp_sum = 0.0;
        for (list<double>::iterator i = Times_smoother_lvls[l].begin(); i != Times_smoother_lvls[l].end(); ++i)
            temp_sum += *i;
        if (verbose)
            std::cout << "time_smoother lvl " << l << " = " << temp_sum << "\n";
    }
    if (verbose)
        std::cout << "\n";
    //delete Times_smoother_lvls;
#ifdef WITH_SMOOTHERS
    for (int l = 0; l < num_levels - 1; ++l)
    {
        if (verbose)
        {
           std::cout << "Internal timing of the smoother at level " << l << ": \n";
           std::cout << "global mult time: " << ((HcurlGSSSmoother*)Smoothers_lvls[l])->GetGlobalMultTime() << " \n" << std::flush;
           std::cout << "internal mult time: " << ((HcurlGSSSmoother*)Smoothers_lvls[l])->GetInternalMultTime() << " \n" << std::flush;
           std::cout << "before internal mult time: " << ((HcurlGSSSmoother*)Smoothers_lvls[l])->GetBeforeIntMultTime() << " \n" << std::flush;
           std::cout << "after internal mult time: " << ((HcurlGSSSmoother*)Smoothers_lvls[l])->GetAfterIntMultTime() << " \n" << std::flush;
        }
    }
#endif
    temp_sum = 0.0;
    for (list<double>::iterator i = Times_coarsestproblem->begin(); i != Times_coarsestproblem->end(); ++i)
        temp_sum += *i;
    if (verbose)
        std::cout << "time_coarsestproblem = " << temp_sum << "\n";
    delete Times_coarsestproblem;

    MPI_Barrier(comm);
    temp_sum = 0.0;
    for (list<double>::iterator i = Times_resupdate->begin(); i != Times_resupdate->end(); ++i)
        temp_sum += *i;
    if (verbose)
        std::cout << "time_resupdate = " << temp_sum << "\n";
    delete Times_resupdate;

    temp_sum = 0.0;
    for (list<double>::iterator i = Times_fw->begin(); i != Times_fw->end(); ++i)
        temp_sum += *i;
    if (verbose)
        std::cout << "time_fw = " << temp_sum << "\n";
    delete Times_fw;
    temp_sum = 0.0;
    for (list<double>::iterator i = Times_up->begin(); i != Times_up->end(); ++i)
        temp_sum += *i;
    if (verbose)
        std::cout << "time_up = " << temp_sum << "\n";
    delete Times_up;
#endif

    NewSigmahat->Distribute(&(NewX.GetBlock(0)));

    // FIXME: remove this
    {
        const Array<int> *temp = EssBdrDofs_Funct_lvls[0][0];

        for ( int tdof = 0; tdof < temp->Size(); ++tdof)
        {
            if ( (*temp)[tdof] != 0 && fabs( (*NewSigmahat)[tdof]) > 1.0e-14 )
                std::cout << "bnd cnd is violated for NewSigmahat! value = "
                          << (*NewSigmahat)[tdof]
                          << "exact val = " << (*sigma_exact_finest)[tdof] << ", index = " << tdof << "\n";
        }
    }

    if (verbose)
        std::cout << "Solution computed via the new solver \n";

    double max_bdr_error = 0;
    for ( int dof = 0; dof < Xinit.GetBlock(0).Size(); ++dof)
    {
        if ( (*EssBdrDofs_Funct_lvls[0][0])[dof] != 0.0)
        {
            //std::cout << "ess dof index: " << dof << "\n";
            double bdr_error_dof = fabs(Xinit.GetBlock(0)[dof] - (*NewSigmahat)[dof]);
            if ( bdr_error_dof > max_bdr_error )
                max_bdr_error = bdr_error_dof;
        }
    }

    if (max_bdr_error > 1.0e-14)
        std::cout << "Error, boundary values for the solution (sigma) are wrong:"
                     " max_bdr_error = " << max_bdr_error << "\n";
    {
        int order_quad = max(2, 2*feorder+1);
        const IntegrationRule *irs[Geometry::NumGeom];
        for (int i = 0; i < Geometry::NumGeom; ++i)
        {
            irs[i] = &(IntRules.Get(i, order_quad));
        }

        double norm_sigma = ComputeGlobalLpNorm(2, *Mytest.GetSigma(), *pmesh, irs);
        double err_newsigmahat = NewSigmahat->ComputeL2Error(*Mytest.GetSigma(), irs);
        if (verbose)
        {
            if ( norm_sigma > MYZEROTOL )
                cout << "|| new sigma_h - sigma_ex || / || sigma_ex || = " << err_newsigmahat / norm_sigma << endl;
            else
                cout << "|| new sigma_h || = " << err_newsigmahat << " (sigma_ex = 0)" << endl;
        }

        DiscreteLinearOperator Div(R_space, W_space);
        Div.AddDomainInterpolator(new DivergenceInterpolator());
        ParGridFunction DivSigma(W_space);
        Div.Assemble();
        Div.Mult(*NewSigmahat, DivSigma);

        double err_div = DivSigma.ComputeL2Error(*Mytest.GetRhs(),irs);
        double norm_div = ComputeGlobalLpNorm(2, *Mytest.GetRhs(), *pmesh, irs);

        if (verbose)
        {
            cout << "|| div (new sigma_h - sigma_ex) || / ||div (sigma_ex)|| = "
                      << err_div/norm_div  << "\n";
        }
    }

    //////////////////////////////////////////////////////
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
    {
        NewS->Distribute(&(NewX.GetBlock(1)));

        double max_bdr_error = 0;
        for ( int dof = 0; dof < Xinit.GetBlock(1).Size(); ++dof)
        {
            if ( (*EssBdrDofs_Funct_lvls[0][1])[dof] != 0.0)
            {
                //std::cout << "ess dof index: " << dof << "\n";
                double bdr_error_dof = fabs(Xinit.GetBlock(1)[dof] - (*NewS)[dof]);
                if ( bdr_error_dof > max_bdr_error )
                    max_bdr_error = bdr_error_dof;
            }
        }

        if (max_bdr_error > 1.0e-14)
            std::cout << "Error, boundary values for the solution (S) are wrong:"
                         " max_bdr_error = " << max_bdr_error << "\n";

        // 13. Extract the parallel grid function corresponding to the finite element
        //     approximation X. This is the local solution on each processor. Compute
        //     L2 error norms.

        int order_quad = max(2, 2*feorder+1);
        const IntegrationRule *irs[Geometry::NumGeom];
        for (int i=0; i < Geometry::NumGeom; ++i)
        {
           irs[i] = &(IntRules.Get(i, order_quad));
        }

        // Computing error for S

        double err_S = NewS->ComputeL2Error((*Mytest.GetU()), irs);
        double norm_S = ComputeGlobalLpNorm(2, (*Mytest.GetU()), *pmesh, irs);
        if (verbose)
        {
            std::cout << "|| S_h - S_ex || / || S_ex || = " <<
                         err_S / norm_S << "\n";
        }
    }
    /////////////////////////////////////////////////////////

    chrono.Stop();


    if (verbose)
        std::cout << "\n";
#endif // for else for USE_AS_A_PREC

#ifdef VISUALIZATION
    if (visualization && nDimensions < 4)
    {
        char vishost[] = "localhost";
        int  visport   = 19916;

        //if (withS)
        {
            socketstream S_ex_sock(vishost, visport);
            S_ex_sock << "parallel " << num_procs << " " << myid << "\n";
            S_ex_sock.precision(8);
            MPI_Barrier(pmesh->GetComm());
            S_ex_sock << "solution\n" << *pmesh << *S_exact << "window_title 'S_exact'"
                   << endl;

            socketstream S_h_sock(vishost, visport);
            S_h_sock << "parallel " << num_procs << " " << myid << "\n";
            S_h_sock.precision(8);
            MPI_Barrier(pmesh->GetComm());
            S_h_sock << "solution\n" << *pmesh << *S << "window_title 'S_h'"
                   << endl;

            *S -= *S_exact;
            socketstream S_diff_sock(vishost, visport);
            S_diff_sock << "parallel " << num_procs << " " << myid << "\n";
            S_diff_sock.precision(8);
            MPI_Barrier(pmesh->GetComm());
            S_diff_sock << "solution\n" << *pmesh << *S << "window_title 'S_h - S_exact'"
                   << endl;
        }

        socketstream sigma_sock(vishost, visport);
        sigma_sock << "parallel " << num_procs << " " << myid << "\n";
        sigma_sock.precision(8);
        MPI_Barrier(pmesh->GetComm());
        sigma_sock << "solution\n" << *pmesh << *sigma_exact
               << "window_title 'sigma_exact'" << endl;
        // Make sure all ranks have sent their 'u' solution before initiating
        // another set of GLVis connections (one from each rank):

        socketstream sigmah_sock(vishost, visport);
        sigmah_sock << "parallel " << num_procs << " " << myid << "\n";
        sigmah_sock.precision(8);
        MPI_Barrier(pmesh->GetComm());
        sigmah_sock << "solution\n" << *pmesh << *sigma << "window_title 'sigma'"
                << endl;

        *sigma_exact -= *sigma;
        socketstream sigmadiff_sock(vishost, visport);
        sigmadiff_sock << "parallel " << num_procs << " " << myid << "\n";
        sigmadiff_sock.precision(8);
        MPI_Barrier(pmesh->GetComm());
        sigmadiff_sock << "solution\n" << *pmesh << *sigma_exact
                 << "window_title 'sigma_ex - sigma_h'" << endl;

        MPI_Barrier(pmesh->GetComm());
    }
#endif

    //MPI_Finalize();
    //return 0;

#ifndef COMPARE_MG

    chrono.Stop();
    if (verbose)
        std::cout << "Deallocating memory \n";
    chrono.Clear();
    chrono.Start();

    for (int l = 0; l < num_levels; ++l)
    {
        delete BdrDofs_Funct_lvls[l][0];
        delete EssBdrDofs_Funct_lvls[l][0];
        delete EssBdrTrueDofs_Funct_lvls[l][0];
        delete EssBdrDofs_Hcurl[l];
        delete EssBdrTrueDofs_Hcurl[l];

        if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
        {
            delete BdrDofs_Funct_lvls[l][1];
            delete EssBdrDofs_Funct_lvls[l][1];
            delete EssBdrTrueDofs_Funct_lvls[l][1];
            delete EssBdrDofs_H1[l];
        }

        if (l < num_levels - 1)
        {
            if (LocalSolver_partfinder_lvls)
                if ((*LocalSolver_partfinder_lvls)[l])
                    delete (*LocalSolver_partfinder_lvls)[l];
        }

#ifdef WITH_SMOOTHERS
        if (l < num_levels - 1)
            if (Smoothers_lvls[l])
                delete Smoothers_lvls[l];
#endif
        delete Divfree_hpmat_mod_lvls[l];

        for (int blk1 = 0; blk1 < Funct_hpmat_lvls[l]->NumRows(); ++blk1)
            for (int blk2 = 0; blk2 < Funct_hpmat_lvls[l]->NumCols(); ++blk2)
                if ((*Funct_hpmat_lvls[l])(blk1,blk2))
                    delete (*Funct_hpmat_lvls[l])(blk1,blk2);
        delete Funct_hpmat_lvls[l];

        if (l < num_levels - 1)
        {
            delete Element_dofs_Func[l];
            delete P_Func[l];
            delete TrueP_Func[l];
        }

        if (l == 0)
            // this happens because for l = 0 object is created in a different way,
            // thus it doesn't own the blocks and cannot delete it from destructor
            for (int blk1 = 0; blk1 < Funct_mat_lvls[l]->NumRowBlocks(); ++blk1)
                for (int blk2 = 0; blk2 < Funct_mat_lvls[l]->NumColBlocks(); ++blk2)
                    delete &(Funct_mat_lvls[l]->GetBlock(blk1,blk2));
        delete Funct_mat_lvls[l];
        delete Funct_mat_offsets_lvls[l];

        delete Constraint_mat_lvls[l];

        delete Divfree_mat_lvls[l];

        delete R_space_lvls[l];
        delete W_space_lvls[l];
        delete C_space_lvls[l];
        if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
            delete H_space_lvls[l];

        if (l < num_levels - 1)
        {
            delete P_W[l];
            delete P_WT[l];
            delete P_R[l];
            delete P_C_lvls[l];
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
                delete P_H_lvls[l];
            delete TrueP_R[l];
            if (prec_is_MG)
                delete TrueP_C[l];
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
                delete TrueP_H[l];

            delete Element_dofs_R[l];
            delete Element_dofs_W[l];
            if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
                delete Element_dofs_H[l];
        }

        if (l < num_levels - 1)
        {
            delete row_offsets_El_dofs[l];
            delete col_offsets_El_dofs[l];
            delete row_offsets_P_Func[l];
            delete col_offsets_P_Func[l];
            delete row_offsets_TrueP_Func[l];
            delete col_offsets_TrueP_Func[l];
        }

    }

    delete LocalSolver_partfinder_lvls;
    delete LocalSolver_lvls;

    for (int blk1 = 0; blk1 < Funct_global->NumRowBlocks(); ++blk1)
        for (int blk2 = 0; blk2 < Funct_global->NumColBlocks(); ++blk2)
            if (Funct_global->IsZeroBlock(blk1, blk2) == false)
                delete &(Funct_global->GetBlock(blk1,blk2));
    delete Funct_global;

    delete Functrhs_global;

    delete hdiv_coll;
    delete R_space;
    delete l2_coll;
    delete W_space;
    delete hdivfree_coll;
    delete C_space;

    delete h1_coll;
    delete H_space;

    delete CoarsestSolver_partfinder;
    delete CoarsestSolver;

    delete sigma_exact_finest;
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
        delete S_exact_finest;

    delete NewSigmahat;
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
        delete NewS;

#ifdef USE_AS_A_PREC
    delete Atest;
    if (strcmp(space_for_S,"H1") == 0)
    {
        delete Ctest;
        delete Btest;
        delete BTtest;
    }
    delete BlockMattest;
#endif

#ifdef OLD_CODE
    delete gform;
    delete Bdiv;

    delete S_exact;
    delete sigma_exact;
    delete opdivfreepart;
    delete sigma;
    delete S;

    delete Sigmahat;
    delete u;

    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
        delete qform;
    delete MainOp;
    delete Mblock;
    delete M;
    delete A;
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
    {
        delete C;
        delete CHT;
        delete CH;
        delete B;
        delete BT;
    }

    if( dim<= 4)
    {
        delete prec;
        if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
        {
            if (monolithicMG)
                for (int l = 0; l < num_levels; ++l)
                {
                    delete offsets_f[l];
                    delete offsets_c[l];
                }
        }
    }

    for (int i = 0; i < P.Size(); ++i)
        delete P[i];

#endif // end of #ifdef OLD_CODE in the memory deallocating

    chrono.Stop();
    if (verbose)
        std::cout << "Deallocation of memory was done in " << chrono.RealTime() <<" seconds.\n";
    chrono.Clear();
    chrono.Start();

    chrono_total.Stop();
    if (verbose)
        std::cout << "Total time consumed was " << chrono_total.RealTime() <<" seconds.\n";
#endif

    delete divfree_dop_mod;

    delete hierarchy;
    delete problem;

    delete problem_mgtools;

    delete Constraint_global;

    delete descriptor;
    delete mgtools_hierarchy;

    for (unsigned int i = 0; i < Funct_global_lvls.size(); ++i)
        if (i > 0)
            delete Funct_global_lvls[i];

    for (unsigned int i = 0; i < fullbdr_attribs.size(); ++i)
        delete fullbdr_attribs[i];

    for (unsigned int i = 0; i < coarsebnd_indces_funct_lvls.size(); ++i)
        delete coarsebnd_indces_funct_lvls[i];

    for (unsigned int i = 0; i < coarsebnd_indces_divfree_lvls.size(); ++i)
        delete coarsebnd_indces_divfree_lvls[i];

    for (int i = 0; i < P_mg.Size(); ++i)
        delete P_mg[i];

    for (int i = 0; i < BlockP_mg_nobnd.Size(); ++i)
        delete BlockP_mg_nobnd[i];

    for (int i = 0; i < BlockOps_mg.Size(); ++i)
        delete BlockOps_mg[i];

    delete A00;
    if (strcmp(space_for_S,"H1") == 0) // S is present
    {
        delete A01;
        delete A10;
    }

    for (int i = 0; i < Smoo_mg.Size(); ++i)
        delete Smoo_mg[i];

    for (int i = 0; i < Smoo_mg_plus.Size(); ++i)
        delete Smoo_mg_plus[i];

    for (int i = 0; i < P_mg_plus.Size(); ++i)
        delete P_mg_plus[i];

    for (int i = 0; i < BlockP_mg_nobnd_plus.Size(); ++i)
        delete BlockP_mg_nobnd_plus[i];

    for (int i = 0; i < BlockOps_mg_plus.Size(); ++i)
        delete BlockOps_mg_plus[i];

    for (int i = 0; i < Funct_mat_lvls_mg.Size(); ++i)
        delete Funct_mat_lvls_mg[i];

    for (int i = 0; i < Constraint_mat_lvls_mg.Size(); ++i)
        delete Constraint_mat_lvls_mg[i];

    for (int i = 0; i < HcurlSmoothers_lvls.Size(); ++i)
        delete HcurlSmoothers_lvls[i];

    for (int i = 0; i < SchwarzSmoothers_lvls.Size(); ++i)
        delete SchwarzSmoothers_lvls[i];

    for (int i = 0; i < el2dofs_funct_lvls.Size(); ++i)
        delete el2dofs_funct_lvls[i];

    for (int i = 0; i < AE_e_lvls.Size(); ++i)
        delete AE_e_lvls[i];

    for (int i = 0; i < Mass_mat_lvls.Size(); ++i)
        delete Mass_mat_lvls[i];

    delete CoarseSolver_mg;
    delete CoarsePrec_mg;
    delete CoarseSolver_mg_plus;

#ifdef WITH_DIVCONSTRAINT_SOLVER
    delete CoarsestSolver_partfinder_new;
#endif

    delete GeneralMGprec;
    delete GeneralMGprec_plus;

#ifdef OLD_CODE
    for (unsigned int i = 0; i < EssBdrTrueDofs_HcurlFunct_lvls.size(); ++i)
        for (unsigned int j = 0; j < EssBdrTrueDofs_HcurlFunct_lvls[i].size(); ++j)
            delete EssBdrTrueDofs_HcurlFunct_lvls[i][j];
#endif

    for (int i = 0; i < pmesh_lvls.Size(); ++i)
         delete pmesh_lvls[i];

    for (unsigned int i = 0; i < offsets.size(); ++i)
        delete offsets[i];

    for (unsigned int i = 0; i < dtd_row_offsets.size(); ++i)
        delete dtd_row_offsets[i];

    for (unsigned int i = 0; i < dtd_col_offsets.size(); ++i)
        delete dtd_col_offsets[i];

    for (unsigned int i = 0; i < el2dofs_row_offsets.size(); ++i)
        delete el2dofs_row_offsets[i];

    for (unsigned int i = 0; i < el2dofs_col_offsets.size(); ++i)
        delete el2dofs_col_offsets[i];

    for (unsigned int i = 0; i < offsets_hdivh1.size(); ++i)
        delete offsets_hdivh1[i];

    for (unsigned int i = 0; i < offsets_sp_hdivh1.size(); ++i)
        delete offsets_sp_hdivh1[i];

#ifdef NEW_INTERFACE
    for (unsigned int i = 0; i < essbdr_tdofs_funct_lvls.size(); ++i)
        for (unsigned int j = 0; j < essbdr_tdofs_funct_lvls[i].size(); ++j)
            delete essbdr_tdofs_funct_lvls[i][j];

    delete xinit_new;
    delete constrfform_new;
#endif

    delete d_td_Funct_coarsest;

    delete bdr_conds;
    delete formulat;
    delete fe_formulat;

    MPI_Finalize();
    return 0;
}

