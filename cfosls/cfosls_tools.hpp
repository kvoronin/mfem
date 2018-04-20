#include <iostream>
#include "testhead.hpp"

#ifndef MFEM_CFOSLS_TOOLS
#define MFEM_CFOSLS_TOOLS

using namespace std;
using namespace mfem;

namespace mfem
{
//HypreParMatrix * CopyRAPHypreParMatrix (HypreParMatrix& inputmat)
//HypreParMatrix * CopyHypreParMatrix (HypreParMatrix& inputmat)

SparseMatrix * RemoveZeroEntries(const SparseMatrix& in);

HypreParMatrix * CreateRestriction(const char * top_or_bot, ParFiniteElementSpace& pfespace,
                                   std::vector<std::pair<int,int> >& bot_to_top_tdofs_link);
std::vector<std::pair<int,int> >* CreateBotToTopDofsLink(const char * eltype, FiniteElementSpace& fespace,
                                                         std::vector<std::pair<int,int> > & bot_to_top_bels, bool verbose = false);

void Eliminate_ib_block(HypreParMatrix& Op_hpmat, const Array<int>& EssBdrTrueDofs_dom, const Array<int>& EssBdrTrueDofs_range );
void Eliminate_bb_block(HypreParMatrix& Op_hpmat, const Array<int>& EssBdrTrueDofs );

/// Conjugate gradient method which checks for boundary conditions (used for debugging)
class CGSolver_mod : public CGSolver
{
protected:
    Array<int>& check_indices;

    bool IndicesAreCorrect(const Vector& vec) const;

public:
   CGSolver_mod(Array<int>& Check_Indices) : CGSolver(), check_indices(Check_Indices) {}

#ifdef MFEM_USE_MPI
   CGSolver_mod(MPI_Comm _comm, Array<int>& Check_Indices) : CGSolver(_comm), check_indices(Check_Indices) { }
#endif

   virtual void Mult(const Vector &b, Vector &x) const;

};

// a class for square block operators where each block is given as a HypreParMatrix
// used as an interface to handle coarsened operators for multigrid
// FIXME: Who should delete the matrices?
class BlkHypreOperator : public Operator
{
protected:
    int numblocks;
    Array2D<HypreParMatrix*> hpmats;
    Array<int> block_offsets;
public:
    BlkHypreOperator(Array2D<HypreParMatrix*> & Hpmats)
        : numblocks(Hpmats.NumRows())
    {
        hpmats.SetSize(numblocks, numblocks);
        for (int i = 0; i < numblocks; ++i )
            for (int j = 0; j < numblocks; ++j )
                if (Hpmats(i,j))
                    hpmats(i,j) = Hpmats(i,j);
                else
                    hpmats(i,j) = NULL;


        block_offsets.SetSize(numblocks + 1);
        block_offsets[0] = 0;
        for (int i = 0; i < numblocks; ++i )
            block_offsets[i + 1] = hpmats(i,i)->Height();
        block_offsets.PartialSum();
    }

    virtual void Mult(const Vector &x, Vector &y) const;
    virtual void MultTranspose(const Vector &x, Vector &y) const;
};

// abstract structure for a (C)FOSLS formulation
// CFOSLS is considered to be a FOSLS formulation with constraint
struct FOSLSFormulation
{
protected:
    const int dim;
    const int numblocks;
    const int unknowns_number;
    const bool have_constraint;
    Array2D<BilinearFormIntegrator*> blfis;
    Array<LinearFormIntegrator*> lfis;
public:
    FOSLSFormulation(int dimension, int num_blocks, int num_unknowns, bool do_have_constraint);

    virtual void foo() = 0; // to make the class pure abstract

    int Dim() const {return dim;}
    int Nblocks() const {return numblocks;}
    int Numunknowns() const {return unknowns_number;}
};

struct CFOSLSFormulation_HdivL2Hyper : public FOSLSFormulation
{
protected:
    int numsol;
    Transport_test test;
public:
    CFOSLSFormulation_HdivL2Hyper(int dimension, int num_solution, bool verbose);

    virtual void foo() override {}
};

struct FOSLSFEFormulation
{
protected:
    FOSLSFormulation& formul;
    Array<FiniteElementCollection*> fecolls;
public:
    FOSLSFEFormulation(FOSLSFormulation& formulation) : formul(formulation)
    {
        fecolls.SetSize(formul.Nblocks());
        for (int i = 0; i < formul.Nblocks(); ++i)
            fecolls[i] = NULL;
    }

    virtual void foo() = 0;
};

struct CFOSLSFEFormulation_HdivL2Hyper : FOSLSFEFormulation
{
protected:
    const int feorder;
public:
    CFOSLSFEFormulation_HdivL2Hyper(FOSLSFormulation& formulation, int fe_order);

    int Feorder() const {return feorder;}

    virtual void foo() override {}
};

/*
// class for CFOSLS problem
class CFOSLSProblem
{
protected:
    FOSLSFormulation& struct_formul;
    int feorder;
    bool spaces_initialized;
    bool forms_initialized;
    bool solver_initialized;

    FiniteElementCollection *hdiv_coll;
    FiniteElementCollection *h1_coll;
    FiniteElementCollection *l2_coll;
    ParFiniteElementSpace * Hdiv_space;
    ParFiniteElementSpace * H1_space;
    ParFiniteElementSpace * H1vec_space;
    ParFiniteElementSpace * L2_space;

    // FIXME: to be removed in the abstract base class
    ParFiniteElementSpace * Sigma_space;
    ParFiniteElementSpace * S_space;

    // all par grid functions which are relevant to the formulation
    // e.g., solution components and right hand sides
    Array<ParGridFunction*> grfuns;

    Array<ParFiniteElementSpace*> pfes;
    BlockProblemForms pbforms;
    Array<ParLinearForm*> plforms;


    Array<int> blkoffsets_true;
    Array<int> blkoffsets;
    Array2D<HypreParMatrix*> hpmats;
    BlockOperator *CFOSLSop;
    Array2D<HypreParMatrix*> hpmats_nobnd;
    BlockOperator *CFOSLSop_nobnd;
    BlockVector * trueRhs;
    BlockVector * trueX;
    BlockVector * trueBnd;
    BlockVector * x; // inital condition (~bnd conditions)
    BlockDiagonalPreconditioner *prec;
    IterativeSolver * solver;

    StopWatch chrono;

protected:
    void InitFEColls(bool verbose);
    void InitSpaces(ParMesh& pmesh);
    void InitForms();
    void AssembleSystem(bool verbose);
    void InitSolver(bool verbose);
    void InitPrec(int prec_option, bool verbose);
    BlockVector *  SetInitialCondition();
    BlockVector * SetTrueInitialCondition();
    void InitGrFuns();
    void DistributeSolution();
    void ComputeError(bool verbose, bool checkbnd);
public:
    CFOSLSHyperbolicProblem(CFOSLSHyperbolicFormulation& struct_formulation,
                            int fe_order, bool verbose);
    CFOSLSHyperbolicProblem(ParMesh& pmesh, CFOSLSHyperbolicFormulation& struct_formulation,
                            int fe_order, int prec_option, bool verbose);
    void BuildCFOSLSSystem(ParMesh& pmesh, bool verbose);
    void Solve(bool verbose);
    void Update();
    // deletes everything which was related to a specific mesh
    void Reset() {MFEM_ABORT("Not implemented \n");}
};
*/

struct CFOSLSHyperbolicFormulation
{
    friend class CFOSLSHyperbolicProblem;

protected:
    const int dim;
    const int numsol;
    const char * space_for_S;
    const char * space_for_sigma;
    bool have_constraint;
    const int bdrattrnum;
    int numblocks;
    int unknowns_number;
    const char * formulation;
    //bool keep_divdiv; unsupported because then we need additional integrators (sum of smth)
    Array2D<BilinearFormIntegrator*> blfis;
    Array<LinearFormIntegrator*> lfis;
    Array<Array<int>* > essbdr_attrs;
public:
    CFOSLSHyperbolicFormulation(int dimension, int solution_number,
                            const char * S_space, const char * sigma_space,
                            bool with_constraint, int number_of_bdrattribs, bool verbose)
        : dim(dimension), numsol(solution_number),
          space_for_S(S_space), space_for_sigma(sigma_space),
          have_constraint(with_constraint), bdrattrnum(number_of_bdrattribs)
          //, keep_divdiv(with_divdiv)
    {
        if (with_constraint)
            formulation = "cfosls";
        else
            formulation = "fosls";
        MFEM_ASSERT(strcmp(formulation,"cfosls") == 0 || strcmp(formulation,"fosls") == 0,
                    "Formulation must be cfosls or fosls!\n");
        MFEM_ASSERT(strcmp(space_for_S,"H1") == 0 || strcmp(space_for_S,"L2") == 0,
                    "Space for S must be H1 or L2!\n");
        MFEM_ASSERT(strcmp(space_for_sigma,"Hdiv") == 0 || strcmp(space_for_sigma,"H1") == 0,
                    "Space for sigma must be Hdiv or H1!\n");
        MFEM_ASSERT(!strcmp(space_for_sigma,"H1") == 0 || (strcmp(space_for_sigma,"H1") == 0
                                                           && strcmp(space_for_S,"H1") == 0),
                    "Sigma from H1vec must be coupled with S from H1!\n");

        Transport_test Mytest(dim,numsol);

        numblocks = 1;

        if (strcmp(space_for_S,"H1") == 0)
            numblocks++;

        unknowns_number = numblocks;

        if (strcmp(formulation,"cfosls") == 0)
            numblocks++;

        if (verbose)
            std::cout << "Number of blocks in the formulation: " << numblocks << "\n";

        //if (strcmp(formulation,"cfosls") == 0)
            //essbdr_attrs.SetSize(numblocks - 1);
        //else // fosls
            //essbdr_attrs.SetSize(numblocks);
        essbdr_attrs.SetSize(numblocks);

        for (int i = 0; i < essbdr_attrs.Size(); ++i)
        {
            essbdr_attrs[i] = new Array<int>(bdrattrnum);
            (*essbdr_attrs[i]) = 0;
        }

        // S is from H1, so we impose bdr condition for S at t = 0
        if (strcmp(space_for_S,"H1") == 0)
            (*essbdr_attrs[1])[0] = 1; // t = 0;

        // S is from L2, so we impose bdr condition for sigma at t = 0
        if (strcmp(space_for_S,"L2") == 0)
            (*essbdr_attrs[0])[0] = 1; // t = 0;

        if (verbose)
        {
            std::cout << "Boundary conditions: \n";
            std::cout << "ess bdr for sigma: \n";
            essbdr_attrs[0]->Print(std::cout, bdrattrnum);
            if (strcmp(space_for_S,"H1") == 0)
            {
                std::cout << "ess bdr for S: \n";
                essbdr_attrs[1]->Print(std::cout, bdrattrnum);
            }
        }

        // bilinear forms
        blfis.SetSize(numblocks, numblocks);
        for (int i = 0; i < numblocks; ++i)
            for (int j = 0; j < numblocks; ++j)
                blfis(i,j) = NULL;

        int blkcount = 0;
        if (strcmp(space_for_S,"H1") == 0) // S is from H1
        {
            if (strcmp(space_for_sigma,"Hdiv") == 0) // sigma is from Hdiv
                blfis(0,0) = new VectorFEMassIntegrator;
            else // sigma is from H1vec
                blfis(0,0) = new ImproperVectorMassIntegrator;
        }
        else // "L2"
            blfis(0,0) = new VectorFEMassIntegrator(*Mytest.Ktilda);
        ++blkcount;

        if (strcmp(space_for_S,"H1") == 0)
        {
            if (strcmp(space_for_sigma,"Hdiv") == 0)
                blfis(1,1) = new H1NormIntegrator(*Mytest.bbT, *Mytest.bTb);
            else
                blfis(1,1) = new MassIntegrator(*Mytest.bTb);
            ++blkcount;
        }

        if (strcmp(space_for_S,"H1") == 0) // S is present
        {
            if (strcmp(space_for_sigma,"Hdiv") == 0) // sigma is from Hdiv
            {
                //Bblock->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.b));
                blfis(1,0) = new VectorFEMassIntegrator(*Mytest.minb);
            }
            else // sigma is from H1
                blfis(1,0) = new MixedVectorScalarIntegrator(*Mytest.minb);
        }

        if (strcmp(formulation,"cfosls") == 0)
        {
           if (strcmp(space_for_sigma,"Hdiv") == 0) // sigma is from Hdiv
             blfis(blkcount,0) = new VectorFEDivergenceIntegrator;
           else // sigma is from H1vec
             blfis(blkcount,0) = new VectorDivergenceIntegrator;
        }

        // linear forms
        lfis.SetSize(numblocks);
        for (int i = 0; i < numblocks; ++i)
            lfis[i] = NULL;

        blkcount = 1;
        if (strcmp(space_for_S,"H1") == 0)
        {
            lfis[1] = new GradDomainLFIntegrator(*Mytest.bf);
            ++blkcount;
        }

        if (strcmp(formulation,"cfosls") == 0)
            lfis[blkcount] = new DomainLFIntegrator(*Mytest.scalardivsigma);
    }

};

class BlockProblemForms
{
    friend class CFOSLSHyperbolicProblem;
protected:
    const int numblocks;
    Array<ParBilinearForm*> diag_forms;
    Array2D<ParMixedBilinearForm*> offd_forms;
public:
    BlockProblemForms(int num_blocks) : numblocks(num_blocks)
    {
        diag_forms.SetSize(num_blocks);
        for (int i = 0; i < num_blocks; ++i)
            diag_forms[i] = NULL;
        offd_forms.SetSize(numblocks, num_blocks);
        for (int i = 0; i < num_blocks; ++i)
            for (int j = 0; j < num_blocks; ++j)
                offd_forms(i,j) = NULL;
    }
    ParBilinearForm* & diag(int i) {return diag_forms[i];}
    ParMixedBilinearForm* & offd(int i, int j) {return offd_forms(i,j);}
};

class CFOSLSHyperbolicProblem
{
protected:
    int feorder;
    CFOSLSHyperbolicFormulation& struct_formul;
    bool spaces_initialized;
    bool forms_initialized;
    bool solver_initialized;

    FiniteElementCollection *hdiv_coll;
    FiniteElementCollection *h1_coll;
    FiniteElementCollection *l2_coll;
    ParFiniteElementSpace * Hdiv_space;
    ParFiniteElementSpace * H1_space;
    ParFiniteElementSpace * H1vec_space;
    ParFiniteElementSpace * L2_space;

    // FIXME: to be removed in the abstract base class
    ParFiniteElementSpace * Sigma_space;
    ParFiniteElementSpace * S_space;

    // all par grid functions which are relevant to the formulation
    // e.g., solution components and right hand sides
    Array<ParGridFunction*> grfuns;

    Array<ParFiniteElementSpace*> pfes;
    BlockProblemForms pbforms;
    Array<ParLinearForm*> plforms;


    Array<int> blkoffsets_true;
    Array<int> blkoffsets;
    Array2D<HypreParMatrix*> hpmats;
    BlockOperator *CFOSLSop;
    Array2D<HypreParMatrix*> hpmats_nobnd;
    BlockOperator *CFOSLSop_nobnd;
    BlockVector * trueRhs;
    BlockVector * trueX;
    BlockVector * trueBnd;
    BlockVector * x; // inital condition (~bnd conditions)
    BlockDiagonalPreconditioner *prec;
    IterativeSolver * solver;

    StopWatch chrono;

protected:
    void InitFEColls(bool verbose);
    void InitSpaces(ParMesh& pmesh);
    void InitForms();
    void AssembleSystem(bool verbose);
    void InitSolver(bool verbose);
    void InitPrec(int prec_option, bool verbose);
    BlockVector *  SetInitialCondition();
    BlockVector * SetTrueInitialCondition();
    void InitGrFuns();
    void DistributeSolution();
    void ComputeError(bool verbose, bool checkbnd);
public:
    CFOSLSHyperbolicProblem(CFOSLSHyperbolicFormulation& struct_formulation,
                            int fe_order, bool verbose);
    CFOSLSHyperbolicProblem(ParMesh& pmesh, CFOSLSHyperbolicFormulation& struct_formulation,
                            int fe_order, int prec_option, bool verbose);
    void BuildCFOSLSSystem(ParMesh& pmesh, bool verbose);
    void Solve(bool verbose);
    void Update();
    // deletes everything which was related to a specific mesh
    void Reset() {MFEM_ABORT("Not implemented \n");}
};


// a class for hierarchy of spaces of finite element spaces based on a nested sequence of meshes
class GeneralHierarchy
{
protected:
    int num_lvls;
    std::vector<ParMesh*> pmesh_lvls;
    std::vector<ParFiniteElementSpace* > Hdiv_space_lvls;
    std::vector<ParFiniteElementSpace* > H1_space_lvls;
    std::vector<ParFiniteElementSpace* > L2_space_lvls;

    std::vector<SparseMatrix*> P_H1_lvls;
    std::vector<SparseMatrix*> P_Hdiv_lvls;
    std::vector<SparseMatrix*> P_L2_lvls;
    std::vector<HypreParMatrix*> TrueP_H1_lvls;
    std::vector<HypreParMatrix*> TrueP_Hdiv_lvls;
    std::vector<HypreParMatrix*> TrueP_L2_lvls;

public:
    GeneralHierarchy(int num_levels, ParMesh& pmesh, int feorder, bool verbose);

    void RefineAndCopy(int lvl, ParMesh* pmesh)
    {
        //if (!dynamic_cast<ParMeshCyl*> (pmesh))
            //std::cout << "Unsuccessful cast \n";
        ParMeshCyl * pmeshcyl_view = dynamic_cast<ParMeshCyl*> (pmesh);

        if (lvl == num_lvls - 1)
            if (pmeshcyl_view)
            {
                //ParMesh * temp = new ParMeshCyl(*pmeshcyl_view);
                //pmesh_lvls[lvl] = dynamic_cast<ParMesh*>(temp);
                pmesh_lvls[lvl] = new ParMeshCyl(*pmeshcyl_view);
            }
            else
                pmesh_lvls[lvl] = new ParMesh(*pmesh);
        else
        {
            if (pmeshcyl_view)
            {
                pmeshcyl_view->Refine(1);
                pmesh_lvls[lvl] = new ParMeshCyl(*pmeshcyl_view);
            }
            else
            {
                pmesh->UniformRefinement();
                pmesh_lvls[lvl] = new ParMesh(*pmesh);
            }
            //pmesh->UniformRefinement();
        }
    }

    ParMesh * GetPmesh(int l) {return pmesh_lvls[l];}

    ParFiniteElementSpace * GetHdiv_space(int l) {return Hdiv_space_lvls[l];}
    ParFiniteElementSpace * GetH1_space(int l) {return H1_space_lvls[l];}
    ParFiniteElementSpace * GetL2_space(int l) {return L2_space_lvls[l];}

    SparseMatrix * GetP_Hdiv(int l) {return P_Hdiv_lvls[l];}
    SparseMatrix * GetP_H1(int l) {return P_H1_lvls[l];}
    SparseMatrix * GetP_L2(int l) {return P_L2_lvls[l];}

    HypreParMatrix * GetTrueP_Hdiv(int l) {return TrueP_Hdiv_lvls[l];}
    HypreParMatrix * GetTrueP_H1(int l) {return TrueP_H1_lvls[l];}
    HypreParMatrix * GetTrueP_L2(int l) {return TrueP_L2_lvls[l];}
};

class GeneralCylHierarchy : public GeneralHierarchy
{
protected:
    std::vector<ParMeshCyl*> pmeshcyl_lvls;

    std::vector<std::vector<std::pair<int,int> > > tdofs_link_H1_lvls;
    std::vector<std::vector<std::pair<int,int> > > tdofs_link_Hdiv_lvls;

    std::vector<HypreParMatrix*> TrueP_bndbot_H1_lvls;
    std::vector<HypreParMatrix*> TrueP_bndbot_Hdiv_lvls;
    std::vector<HypreParMatrix*> TrueP_bndtop_H1_lvls;
    std::vector<HypreParMatrix*> TrueP_bndtop_Hdiv_lvls;
    std::vector<HypreParMatrix*> Restrict_bot_H1_lvls;
    std::vector<HypreParMatrix*> Restrict_bot_Hdiv_lvls;
    std::vector<HypreParMatrix*> Restrict_top_H1_lvls;
    std::vector<HypreParMatrix*> Restrict_top_Hdiv_lvls;
protected:
    void ConstructRestrictions();
    void ConstructInterpolations();
    void ConstructTdofsLinks();

public:
    GeneralCylHierarchy(int num_levels, ParMeshCyl& pmesh, int feorder, bool verbose)
        : GeneralHierarchy(num_levels, pmesh, feorder, verbose)
    {
        pmeshcyl_lvls.resize(num_lvls);
        for (int l = 0; l < num_lvls; ++l)
        {
            ParMeshCyl * temp = dynamic_cast<ParMeshCyl*>(pmesh_lvls[l]);
            if (temp)
                pmeshcyl_lvls[l] = temp;
            else
            {
                MFEM_ABORT ("Unsuccessful cast \n");
            }
        }

        // don't change the order of these calls
        ConstructTdofsLinks();
        ConstructRestrictions();
        ConstructInterpolations();
    }

    ParMeshCyl * GetPmeshcyl(int l) {return pmeshcyl_lvls[l];}

    std::vector<std::pair<int,int> > * GetTdofs_Hdiv_link(int l) {return &(tdofs_link_Hdiv_lvls[l]);}
    std::vector<std::pair<int,int> > * GetTdofs_H1_link(int l) {return &(tdofs_link_H1_lvls[l]);}

    HypreParMatrix * GetTrueP_bndbot_Hdiv (int l) {return TrueP_bndbot_Hdiv_lvls[l];}
    HypreParMatrix * GetTrueP_bndtop_Hdiv (int l) {return TrueP_bndtop_Hdiv_lvls[l];}
    HypreParMatrix * GetTrueP_bndbot_H1 (int l) {return TrueP_bndtop_H1_lvls[l];}
    HypreParMatrix * GetTrueP_bndtop_H1 (int l) {return TrueP_bndtop_H1_lvls[l];}

    HypreParMatrix * GetRestrict_bot_Hdiv (int l) {return Restrict_bot_Hdiv_lvls[l];}
    HypreParMatrix * GetRestrict_top_Hdiv (int l) {return Restrict_top_Hdiv_lvls[l];}
    HypreParMatrix * GetRestrict_bot_H1 (int l) {return Restrict_bot_H1_lvls[l];}
    HypreParMatrix * GetRestrict_top_H1 (int l) {return Restrict_top_H1_lvls[l];}

    int GetLinksize_Hdiv(int l) const {return tdofs_link_Hdiv_lvls[l].size();}
    int GetLinksize_H1(int l) const {return tdofs_link_H1_lvls[l].size();}

};

} // for namespace mfem


#endif
