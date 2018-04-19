#include <iostream>
#include "testhead.hpp"

using namespace std;

namespace mfem
{

bool CGSolver_mod::IndicesAreCorrect(const Vector& vec) const
{
    bool res = true;

    for (int i = 0; i < check_indices.Size(); ++i)
        if (fabs(vec[check_indices[i]]) > 1.0e-14)
        {
            std::cout << "index " << i << "has a nonzero value: " << vec[check_indices[i]] << "\n";
            res = false;
        }

    return res;
}

void CGSolver_mod::Mult(const Vector &b, Vector &x) const
{
   int i;
   double r0, den, nom, nom0, betanom, alpha, beta;

   //std::cout << "look at b at the entrance \n";
   //b.Print();
   std::cout << "check for b: " << IndicesAreCorrect(b) << "\n";
   MFEM_ASSERT(IndicesAreCorrect(b), "Indices check fails for b \n");

   if (iterative_mode)
   {
      oper->Mult(x, r);
      subtract(b, r, r); // r = b - A x
   }
   else
   {
      r = b;
      x = 0.0;
   }

   MFEM_ASSERT(IndicesAreCorrect(r), "Indices check fails for r \n");
   std::cout << "check for initial r: " << IndicesAreCorrect(r) << "\n";
   check_indices.Print();
   for ( int i = 0; i < check_indices.Size(); ++i)
       std::cout << r[check_indices[i]] << " ";
   std::cout << "\n";
   //std::cout << "look at the initial residual\n";
   //r.Print();

   if (prec)
   {
      prec->Mult(r, z); // z = B r
      //std::cout << "look at preconditioned residual at the entrance \n";
      //z.Print();
      d = z;
   }
   else
   {
      d = r;
   }

   std::cout << "check for initial d: " << IndicesAreCorrect(d) << "\n";
   MFEM_ASSERT(IndicesAreCorrect(b), "Indices check fails for d \n");

   //std::cout << "look at residual at the entrance \n";
   //r.Print();

   nom0 = nom = Dot(d, r);
   MFEM_ASSERT(IsFinite(nom), "nom = " << nom);

   std::cout << "nom = " << nom << "\n";

   if (print_level == 1 || print_level == 3)
   {
      cout << "   Iteration : " << setw(3) << 0 << "  (B r, r) = "
           << nom << (print_level == 3 ? " ...\n" : "\n");
   }

   r0 = std::max(nom*rel_tol*rel_tol, abs_tol*abs_tol);
   if (nom <= r0)
   {
      converged = 1;
      final_iter = 0;
      final_norm = sqrt(nom);
      return;
   }

   oper->Mult(d, z);  // z = A d
   den = Dot(z, d);
   MFEM_ASSERT(IsFinite(den), "den = " << den);

   if (print_level >= 0 && den < 0.0)
   {
      cout << "Negative denominator in step 0 of PCG: " << den << '\n';
   }

   if (den == 0.0)
   {
      converged = 0;
      final_iter = 0;
      final_norm = sqrt(nom);
      return;
   }

   // start iteration
   converged = 0;
   final_iter = max_iter;
   for (i = 1; true; )
   {
      alpha = nom/den;
      add(x,  alpha, d, x);     //  x = x + alpha d
      add(r, -alpha, z, r);     //  r = r - alpha A d

      std::cout << "check for new r: " << IndicesAreCorrect(r) << ", i = " << i << " \n";

      if (prec)
      {
         prec->Mult(r, z);      //  z = B r
         std::cout << "check for new z: " << IndicesAreCorrect(z) << ", i = " << i << " \n";
         betanom = Dot(r, z);
      }
      else
      {
         betanom = Dot(r, r);
      }

      MFEM_ASSERT(IsFinite(betanom), "betanom = " << betanom);

      if (print_level == 1)
      {
         cout << "   Iteration : " << setw(3) << i << "  (B r, r) = "
              << betanom << '\n';
      }

      if (betanom < r0)
      {
         if (print_level == 2)
         {
            cout << "Number of PCG iterations: " << i << '\n';
         }
         else if (print_level == 3)
         {
            cout << "   Iteration : " << setw(3) << i << "  (B r, r) = "
                 << betanom << '\n';
         }
         converged = 1;
         final_iter = i;
         break;
      }

      if (++i > max_iter)
      {
         break;
      }

      beta = betanom/nom;
      if (prec)
      {
         add(z, beta, d, d);   //  d = z + beta d
         std::cout << "check for new d: " << IndicesAreCorrect(d) << ", i = " << i << " \n";
      }
      else
      {
         add(r, beta, d, d);
      }
      oper->Mult(d, z);       //  z = A d
      den = Dot(d, z);
      MFEM_ASSERT(IsFinite(den), "den = " << den);
      if (den <= 0.0)
      {
         if (print_level >= 0 && Dot(d, d) > 0.0)
            cout << "PCG: The operator is not positive definite. (Ad, d) = "
                 << den << '\n';
      }
      nom = betanom;
   }
   if (print_level >= 0 && !converged)
   {
      if (print_level != 1)
      {
         if (print_level != 3)
         {
            cout << "   Iteration : " << setw(3) << 0 << "  (B r, r) = "
                 << nom0 << " ...\n";
         }
         cout << "   Iteration : " << setw(3) << final_iter << "  (B r, r) = "
              << betanom << '\n';
      }
      cout << "PCG: No convergence!" << '\n';
   }
   if (print_level >= 1 || (print_level >= 0 && !converged))
   {
      cout << "Average reduction factor = "
           << pow (betanom/nom0, 0.5/final_iter) << '\n';
   }
   final_norm = sqrt(betanom);
}

void BlkHypreOperator::Mult(const Vector &x, Vector &y) const
{
    BlockVector x_viewer(x.GetData(), block_offsets);
    BlockVector y_viewer(y.GetData(), block_offsets);

    for (int i = 0; i < numblocks; ++i)
    {
        for (int j = 0; j < numblocks; ++j)
            if (hpmats(i,j))
                hpmats(i,j)->Mult(x_viewer.GetBlock(j), y_viewer.GetBlock(i));
    }
}

void BlkHypreOperator::MultTranspose(const Vector &x, Vector &y) const
{
    BlockVector x_viewer(x.GetData(), block_offsets);
    BlockVector y_viewer(y.GetData(), block_offsets);

    for (int i = 0; i < numblocks; ++i)
    {
        for (int j = 0; j < numblocks; ++j)
            if (hpmats(i,j))
                hpmats(i,j)->MultTranspose(x_viewer.GetBlock(j), y_viewer.GetBlock(i));
    }
}


CFOSLSHyperbolicProblem::CFOSLSHyperbolicProblem(CFOSLSHyperbolicFormulation &struct_formulation,
                                                 int fe_order, bool verbose)
    : feorder (fe_order), struct_formul(struct_formulation),
      spaces_initialized(false), forms_initialized(false), solver_initialized(false),
      pbforms(struct_formul.numblocks)
{
    InitFEColls(verbose);
}

CFOSLSHyperbolicProblem::CFOSLSHyperbolicProblem(ParMesh& pmesh, CFOSLSHyperbolicFormulation &struct_formulation,
                                                 int fe_order, int prec_option, bool verbose)
    : feorder (fe_order), struct_formul(struct_formulation), pbforms(struct_formul.numblocks)
{
    InitFEColls(verbose);
    InitSpaces(pmesh);
    spaces_initialized = true;
    InitForms();
    forms_initialized = true;
    AssembleSystem(verbose);
    InitPrec(prec_option, verbose);
    InitSolver(verbose);
    solver_initialized = true;
    InitGrFuns();
}

void CFOSLSHyperbolicProblem::InitFEColls(bool verbose)
{
    if ( struct_formul.dim == 4 )
    {
        hdiv_coll = new RT0_4DFECollection;
        if(verbose)
            cout << "RT: order 0 for 4D" << endl;
    }
    else
    {
        hdiv_coll = new RT_FECollection(feorder, struct_formul.dim);
        if(verbose)
            cout << "RT: order " << feorder << " for 3D" << endl;
    }

    if (struct_formul.dim == 4)
        MFEM_ASSERT(feorder == 0, "Only lowest order elements are support in 4D!");

    if (struct_formul.dim == 4)
    {
        h1_coll = new LinearFECollection;
        if (verbose)
            cout << "H1 in 4D: linear elements are used" << endl;
    }
    else
    {
        h1_coll = new H1_FECollection(feorder+1, struct_formul.dim);
        if(verbose)
            cout << "H1: order " << feorder + 1 << " for 3D" << endl;
    }
    l2_coll = new L2_FECollection(feorder, struct_formul.dim);
    if (verbose)
        cout << "L2: order " << feorder << endl;
}

void CFOSLSHyperbolicProblem::InitSpaces(ParMesh &pmesh)
{
    Hdiv_space = new ParFiniteElementSpace(&pmesh, hdiv_coll);
    H1_space = new ParFiniteElementSpace(&pmesh, h1_coll);
    L2_space = new ParFiniteElementSpace(&pmesh, l2_coll);
    H1vec_space = new ParFiniteElementSpace(&pmesh, h1_coll, struct_formul.dim, Ordering::byVDIM);

    pfes.SetSize(struct_formul.numblocks);

    int blkcount = 0;
    if (strcmp(struct_formul.space_for_sigma,"Hdiv") == 0)
        pfes[0] = Hdiv_space;
    else
        pfes[0] = H1vec_space;
    Sigma_space = pfes[0];
    ++blkcount;

    if (strcmp(struct_formul.space_for_S,"H1") == 0)
    {
        pfes[blkcount] = H1_space;
        S_space = pfes[blkcount];
        ++blkcount;
    }
    else // "L2"
    {
        S_space = L2_space;
    }

    if (struct_formul.have_constraint)
        pfes[blkcount] = L2_space;

}

void CFOSLSHyperbolicProblem::InitForms()
{
    MFEM_ASSERT(spaces_initialized, "Spaces must have been initialized by this moment!\n");

    plforms.SetSize(struct_formul.numblocks);
    for (int i = 0; i < struct_formul.numblocks; ++i)
    {
        plforms[i] = new ParLinearForm(pfes[i]);
        if (struct_formul.lfis[i])
            plforms[i]->AddDomainIntegrator(struct_formul.lfis[i]);
    }

    for (int i = 0; i < struct_formul.numblocks; ++i)
        for (int j = 0; j < struct_formul.numblocks; ++j)
        {
            if (i == j)
                pbforms.diag(i) = new ParBilinearForm(pfes[i]);
            else
                pbforms.offd(i,j) = new ParMixedBilinearForm(pfes[j], pfes[i]);

            if (struct_formul.blfis(i,j))
            {
                if (i == j)
                    pbforms.diag(i)->AddDomainIntegrator(struct_formul.blfis(i,j));
                else
                    pbforms.offd(i,j)->AddDomainIntegrator(struct_formul.blfis(i,j));
            }
        }

}

BlockVector * CFOSLSHyperbolicProblem::SetTrueInitialCondition()
{
    BlockVector * truebnd = new BlockVector(blkoffsets_true);
    *truebnd = 0.0;

    Transport_test Mytest(struct_formul.dim,struct_formul.numsol);

    ParGridFunction * sigma_exact = new ParGridFunction(Sigma_space);
    sigma_exact->ProjectCoefficient(*(Mytest.sigma));
    Vector sigma_exact_truedofs(Sigma_space->TrueVSize());
    sigma_exact->ParallelProject(sigma_exact_truedofs);

    Array<int> ess_tdofs_sigma;
    Sigma_space->GetEssentialTrueDofs(*struct_formul.essbdr_attrs[0], ess_tdofs_sigma);

    for (int j = 0; j < ess_tdofs_sigma.Size(); ++j)
    {
        int tdof = ess_tdofs_sigma[j];
        truebnd->GetBlock(0)[tdof] = sigma_exact_truedofs[tdof];
    }

    if (strcmp(struct_formul.space_for_S,"H1") == 0)
    {
        ParGridFunction *S_exact = new ParGridFunction(S_space);
        S_exact->ProjectCoefficient(*(Mytest.scalarS));
        Vector S_exact_truedofs(S_space->TrueVSize());
        S_exact->ParallelProject(S_exact_truedofs);

        Array<int> ess_tdofs_S;
        S_space->GetEssentialTrueDofs(*struct_formul.essbdr_attrs[1], ess_tdofs_S);

        for (int j = 0; j < ess_tdofs_S.Size(); ++j)
        {
            int tdof = ess_tdofs_S[j];
            truebnd->GetBlock(1)[tdof] = S_exact_truedofs[tdof];
        }

    }

    return truebnd;
}

BlockVector * CFOSLSHyperbolicProblem::SetInitialCondition()
{
    BlockVector * init_cond = new BlockVector(blkoffsets);
    *init_cond = 0.0;

    Transport_test Mytest(struct_formul.dim,struct_formul.numsol);

    ParGridFunction * sigma_exact = new ParGridFunction(Sigma_space);
    sigma_exact->ProjectCoefficient(*(Mytest.sigma));

    init_cond->GetBlock(0) = *sigma_exact;
    if (strcmp(struct_formul.space_for_S,"H1") == 0)
    {
        ParGridFunction *S_exact = new ParGridFunction(S_space);
        S_exact->ProjectCoefficient(*(Mytest.scalarS));
        init_cond->GetBlock(1) = *S_exact;
    }

    return init_cond;
}

void CFOSLSHyperbolicProblem::InitGrFuns()
{
    // + 1 for the f stored as a grid function from L2
    grfuns.SetSize(struct_formul.unknowns_number + 1);
    for (int i = 0; i < struct_formul.unknowns_number; ++i)
        grfuns[i] = new ParGridFunction(pfes[i]);
    grfuns[struct_formul.unknowns_number] = new ParGridFunction(L2_space);

    Transport_test Mytest(struct_formul.dim,struct_formul.numsol);
    grfuns[struct_formul.unknowns_number]->ProjectCoefficient(*Mytest.scalardivsigma);
}

void CFOSLSHyperbolicProblem::BuildCFOSLSSystem(ParMesh &pmesh, bool verbose)
{
    if (!spaces_initialized)
    {
        Hdiv_space = new ParFiniteElementSpace(&pmesh, hdiv_coll);
        H1_space = new ParFiniteElementSpace(&pmesh, h1_coll);
        L2_space = new ParFiniteElementSpace(&pmesh, l2_coll);

        if (strcmp(struct_formul.space_for_sigma,"H1") == 0)
            H1vec_space = new ParFiniteElementSpace(&pmesh, h1_coll, struct_formul.dim, Ordering::byVDIM);

        if (strcmp(struct_formul.space_for_sigma,"Hdiv") == 0)
            Sigma_space = Hdiv_space;
        else
            Sigma_space = H1vec_space;

        if (strcmp(struct_formul.space_for_S,"H1") == 0)
            S_space = H1_space;
        else // "L2"
            S_space = L2_space;

        MFEM_ASSERT(!forms_initialized, "Forms cannot have been already initialized by this moment!");

        InitForms();
    }

    AssembleSystem(verbose);
}

void CFOSLSHyperbolicProblem::Solve(bool verbose)
{
    *trueX = 0;

    chrono.Clear();
    chrono.Start();

    //trueRhs->Print();
    //SparseMatrix diag;
    //((HypreParMatrix&)(CFOSLSop->GetBlock(0,0))).GetDiag(diag);
    //diag.Print();

    solver->Mult(*trueRhs, *trueX);

    chrono.Stop();

    if (verbose)
    {
       if (solver->GetConverged())
          std::cout << "MINRES converged in " << solver->GetNumIterations()
                    << " iterations with a residual norm of " << solver->GetFinalNorm() << ".\n";
       else
          std::cout << "MINRES did not converge in " << solver->GetNumIterations()
                    << " iterations. Residual norm is " << solver->GetFinalNorm() << ".\n";
       std::cout << "MINRES solver took " << chrono.RealTime() << "s. \n";
    }

    DistributeSolution();

    ComputeError(verbose, true);
}

void CFOSLSHyperbolicProblem::DistributeSolution()
{
    for (int i = 0; i < struct_formul.unknowns_number; ++i)
        grfuns[i]->Distribute(&(trueX->GetBlock(i)));
}

void CFOSLSHyperbolicProblem::ComputeError(bool verbose, bool checkbnd)
{
    Transport_test Mytest(struct_formul.dim,struct_formul.numsol);

    ParMesh * pmesh = pfes[0]->GetParMesh();

    ParGridFunction * sigma = grfuns[0];

    int order_quad = max(2, 2*feorder+1);
    const IntegrationRule *irs[Geometry::NumGeom];
    for (int i=0; i < Geometry::NumGeom; ++i)
    {
       irs[i] = &(IntRules.Get(i, order_quad));
    }

    double err_sigma = sigma->ComputeL2Error(*(Mytest.sigma), irs);
    double norm_sigma = ComputeGlobalLpNorm(2, *(Mytest.sigma), *pmesh, irs);
    if (verbose)
        cout << "|| sigma - sigma_ex || / || sigma_ex || = " << err_sigma / norm_sigma << endl;

    ParGridFunction * S;
    if (strcmp(struct_formul.space_for_S,"H1") == 0)
    {
        //std::cout << "I am here \n";
        S = grfuns[1];
    }
    else
    {
        //std::cout << "I am there \n";
        ParBilinearForm *Cblock(new ParBilinearForm(S_space));
        Cblock->AddDomainIntegrator(new MassIntegrator(*(Mytest.bTb)));
        Cblock->Assemble();
        Cblock->Finalize();
        HypreParMatrix * C = Cblock->ParallelAssemble();

        ParMixedBilinearForm *Bblock(new ParMixedBilinearForm(Sigma_space, S_space));
        Bblock->AddDomainIntegrator(new VectorFEMassIntegrator(*(Mytest.b)));
        Bblock->Assemble();
        Bblock->Finalize();
        HypreParMatrix * B = Bblock->ParallelAssemble();
        Vector bTsigma(C->Height());
        B->Mult(trueX->GetBlock(0),bTsigma);

        Vector trueS(C->Height());

        CG(*C, bTsigma, trueS, 0, 5000, 1e-9, 1e-12);

        S = new ParGridFunction(S_space);
        S->Distribute(trueS);

        delete Cblock;
        delete Bblock;
        delete B;
        delete C;
    }

    //std::cout << "I compute S_h one way or another \n";

    double err_S = S->ComputeL2Error((*Mytest.scalarS), irs);
    double norm_S = ComputeGlobalLpNorm(2, (*Mytest.scalarS), *pmesh, irs);
    if (verbose)
    {
        std::cout << "|| S_h - S_ex || / || S_ex || = " <<
                     err_S / norm_S << "\n";
    }

    if (checkbnd)
    {
        ParGridFunction * sigma_exact = new ParGridFunction(Sigma_space);
        sigma_exact->ProjectCoefficient(*Mytest.sigma);
        Vector sigma_exact_truedofs(Sigma_space->TrueVSize());
        sigma_exact->ParallelProject(sigma_exact_truedofs);

        Array<int> EssBnd_tdofs_sigma;
        Sigma_space->GetEssentialTrueDofs(*struct_formul.essbdr_attrs[0], EssBnd_tdofs_sigma);

        for (int i = 0; i < EssBnd_tdofs_sigma.Size(); ++i)
        {
            int tdof = EssBnd_tdofs_sigma[i];
            double value_ex = sigma_exact_truedofs[tdof];
            double value_com = trueX->GetBlock(0)[tdof];

            if (fabs(value_ex - value_com) > MYZEROTOL)
            {
                std::cout << "bnd condition is violated for sigma, tdof = " << tdof << " exact value = "
                          << value_ex << ", value_com = " << value_com << ", diff = " << value_ex - value_com << "\n";
                std::cout << "rhs side at this tdof = " << trueRhs->GetBlock(0)[tdof] << "\n";
            }
        }

        if (strcmp(struct_formul.space_for_S,"H1") == 0) // S is present
        {
            ParGridFunction * S_exact = new ParGridFunction(S_space);
            S_exact->ProjectCoefficient(*Mytest.scalarS);

            Vector S_exact_truedofs(S_space->TrueVSize());
            S_exact->ParallelProject(S_exact_truedofs);

            Array<int> EssBnd_tdofs_S;
            S_space->GetEssentialTrueDofs(*struct_formul.essbdr_attrs[1], EssBnd_tdofs_S);

            for (int i = 0; i < EssBnd_tdofs_S.Size(); ++i)
            {
                int tdof = EssBnd_tdofs_S[i];
                double value_ex = S_exact_truedofs[tdof];
                double value_com = trueX->GetBlock(1)[tdof];

                if (fabs(value_ex - value_com) > MYZEROTOL)
                {
                    std::cout << "bnd condition is violated for S, tdof = " << tdof << " exact value = "
                              << value_ex << ", value_com = " << value_com << ", diff = " << value_ex - value_com << "\n";
                    std::cout << "rhs side at this tdof = " << trueRhs->GetBlock(1)[tdof] << "\n";
                }
            }
        }
    }
}


// works correctly only for problems with homogeneous initial conditions?
// see the times-stepping branch, think of how boundary conditions for off-diagonal blocks are imposed
// system is assumed to be symmetric
void CFOSLSHyperbolicProblem::AssembleSystem(bool verbose)
{
    int numblocks = struct_formul.numblocks;

    blkoffsets_true.SetSize(numblocks + 1);
    blkoffsets_true[0] = 0;
    for (int i = 0; i < numblocks; ++i)
        blkoffsets_true[i + 1] = pfes[i]->TrueVSize();
    blkoffsets_true.PartialSum();

    blkoffsets.SetSize(numblocks + 1);
    blkoffsets[0] = 0;
    for (int i = 0; i < numblocks; ++i)
        blkoffsets[i + 1] = pfes[i]->GetVSize();
    blkoffsets.PartialSum();

    x = SetInitialCondition();

    trueRhs = new BlockVector(blkoffsets_true);
    trueX = new BlockVector(blkoffsets_true);

    for (int i = 0; i < numblocks; ++i)
        plforms[i]->Assemble();

    hpmats_nobnd.SetSize(numblocks, numblocks);
    for (int i = 0; i < numblocks; ++i)
        for (int j = 0; j < numblocks; ++j)
            hpmats_nobnd(i,j) = NULL;
    for (int i = 0; i < numblocks; ++i)
        for (int j = 0; j < numblocks; ++j)
        {
            if (i == j)
            {
                if (pbforms.diag(i))
                {
                    pbforms.diag(i)->Assemble();
                    pbforms.diag(i)->Finalize();
                    hpmats_nobnd(i,j) = pbforms.diag(i)->ParallelAssemble();
                }
            }
            else // off-diagonal
            {
                if (pbforms.offd(i,j) || pbforms.offd(j,i))
                {
                    int exist_row, exist_col;
                    if (pbforms.offd(i,j))
                    {
                        exist_row = i;
                        exist_col = j;
                    }
                    else
                    {
                        exist_row = j;
                        exist_col = i;
                    }

                    pbforms.offd(exist_row,exist_col)->Assemble();

                    pbforms.offd(exist_row,exist_col)->Finalize();
                    hpmats_nobnd(exist_row,exist_col) = pbforms.offd(exist_row,exist_col)->ParallelAssemble();
                    hpmats_nobnd(exist_col, exist_row) = hpmats_nobnd(exist_row,exist_col)->Transpose();
                }
            }
        }

    for (int i = 0; i < numblocks; ++i)
        for (int j = 0; j < numblocks; ++j)
            if (i == j)
                pbforms.diag(i)->LoseMat();
            else
                if (pbforms.offd(i,j))
                    pbforms.offd(i,j)->LoseMat();

    hpmats.SetSize(numblocks, numblocks);
    for (int i = 0; i < numblocks; ++i)
        for (int j = 0; j < numblocks; ++j)
            hpmats(i,j) = NULL;

    for (int i = 0; i < numblocks; ++i)
        for (int j = 0; j < numblocks; ++j)
        {
            if (i == j)
            {
                if (pbforms.diag(i))
                {
                    pbforms.diag(i)->Assemble();

                    //pbforms.diag(i)->EliminateEssentialBC(*struct_formul.essbdr_attrs[i],
                            //x->GetBlock(i), *plforms[i]);
                    Vector dummy(pbforms.diag(i)->Height());
                    dummy = 0.0;
                    pbforms.diag(i)->EliminateEssentialBC(*struct_formul.essbdr_attrs[i],
                            x->GetBlock(i), dummy);
                    pbforms.diag(i)->Finalize();
                    hpmats(i,j) = pbforms.diag(i)->ParallelAssemble();

                    SparseMatrix diag;
                    hpmats(i,j)->GetDiag(diag);
                    Array<int> essbnd_tdofs;
                    pfes[i]->GetEssentialTrueDofs(*struct_formul.essbdr_attrs[i], essbnd_tdofs);
                    for (int i = 0; i < essbnd_tdofs.Size(); ++i)
                    {
                        int tdof = essbnd_tdofs[i];
                        diag.EliminateRow(tdof,1.0);
                    }

                }
            }
            else // off-diagonal
            {
                if (pbforms.offd(i,j) || pbforms.offd(j,i))
                {
                    int exist_row, exist_col;
                    if (pbforms.offd(i,j))
                    {
                        exist_row = i;
                        exist_col = j;
                    }
                    else
                    {
                        exist_row = j;
                        exist_col = i;
                    }

                    pbforms.offd(exist_row,exist_col)->Assemble();

                    //pbforms.offd(exist_row,exist_col)->EliminateTrialDofs(*struct_formul.essbdr_attrs[exist_col],
                                                                          //x->GetBlock(exist_col), *plforms[exist_row]);
                    //pbforms.offd(exist_row,exist_col)->EliminateTestDofs(*struct_formul.essbdr_attrs[exist_row]);

                    Vector dummy(pbforms.offd(exist_row,exist_col)->Height());
                    dummy = 0.0;
                    pbforms.offd(exist_row,exist_col)->EliminateTrialDofs(*struct_formul.essbdr_attrs[exist_col],
                                                                          x->GetBlock(exist_col), dummy);
                    pbforms.offd(exist_row,exist_col)->EliminateTestDofs(*struct_formul.essbdr_attrs[exist_row]);


                    pbforms.offd(exist_row,exist_col)->Finalize();
                    hpmats(exist_row,exist_col) = pbforms.offd(exist_row,exist_col)->ParallelAssemble();
                    hpmats(exist_col, exist_row) = hpmats(exist_row,exist_col)->Transpose();
                }
            }
        }

   CFOSLSop = new BlockOperator(blkoffsets_true);
   for (int i = 0; i < numblocks; ++i)
       for (int j = 0; j < numblocks; ++j)
           CFOSLSop->SetBlock(i,j, hpmats(i,j));

   CFOSLSop_nobnd = new BlockOperator(blkoffsets_true);
   for (int i = 0; i < numblocks; ++i)
       for (int j = 0; j < numblocks; ++j)
           CFOSLSop_nobnd->SetBlock(i,j, hpmats_nobnd(i,j));

   // assembling rhs forms without boundary conditions
   for (int i = 0; i < numblocks; ++i)
   {
       plforms[i]->ParallelAssemble(trueRhs->GetBlock(i));
   }

   //trueRhs->Print();

   trueBnd = SetTrueInitialCondition();

   // moving the contribution from inhomogenous bnd conditions
   // from the rhs
   BlockVector trueBndCor(blkoffsets_true);
   trueBndCor = 0.0;

   //trueBnd->Print();

   CFOSLSop_nobnd->Mult(*trueBnd, trueBndCor);

   //trueBndCor.Print();

   *trueRhs -= trueBndCor;

   // restoring correct boundary values for boundary tdofs
   for (int i = 0; i < numblocks; ++i)
   {
       Array<int> ess_bnd_tdofs;
       pfes[i]->GetEssentialTrueDofs(*struct_formul.essbdr_attrs[i], ess_bnd_tdofs);

       for (int j = 0; j < ess_bnd_tdofs.Size(); ++j)
       {
           int tdof = ess_bnd_tdofs[j];
           trueRhs->GetBlock(i)[tdof] = trueBnd->GetBlock(i)[tdof];
       }
   }

   if (verbose)
        cout << "Final saddle point matrix assembled \n";
    MPI_Comm comm = pfes[0]->GetComm();
    MPI_Barrier(comm);
}

void CFOSLSHyperbolicProblem::InitSolver(bool verbose)
{
    MPI_Comm comm = pfes[0]->GetComm();

    int max_iter = 100000;
    double rtol = 1e-12;//1e-7;//1e-9;
    double atol = 1e-14;//1e-9;//1e-12;

    solver = new MINRESSolver(comm);
    solver->SetAbsTol(atol);
    solver->SetRelTol(rtol);
    solver->SetMaxIter(max_iter);
    solver->SetOperator(*CFOSLSop);
    if (prec)
         solver->SetPreconditioner(*prec);
    solver->SetPrintLevel(0);

    if (verbose)
        std::cout << "Here you should print out parameters of the linear solver \n";
}

// this works only for hyperbolic case
// and should be a virtual function in the abstract base
void CFOSLSHyperbolicProblem::InitPrec(int prec_option, bool verbose)
{
    bool use_ADS;
    switch (prec_option)
    {
    case 1: // smth simple like AMS
        use_ADS = false;
        break;
    case 2: // MG
        use_ADS = true;
        break;
    default: // no preconditioner
        break;
    }

    HypreParMatrix & A = (HypreParMatrix&)CFOSLSop->GetBlock(0,0);
    HypreParMatrix * C;
    int blkcount = 1;
    if (strcmp(struct_formul.space_for_S,"H1") == 0) // S is from H1
    {
        C = &((HypreParMatrix&)CFOSLSop->GetBlock(1,1));
        ++blkcount;
    }
    HypreParMatrix & D = (HypreParMatrix&)CFOSLSop->GetBlock(blkcount,0);

    HypreParMatrix *Schur;
    if (struct_formul.have_constraint)
    {
       HypreParMatrix *AinvDt = D.Transpose();
       //FIXME: Do we actually need a hypreparvector here? Can't we just use a vector?
       //HypreParVector *Ad = new HypreParVector(comm, A.GetGlobalNumRows(),
                                            //A.GetRowStarts());
       //A.GetDiag(*Ad);
       //AinvDt->InvScaleRows(*Ad);
       Vector Ad;
       A.GetDiag(Ad);
       AinvDt->InvScaleRows(Ad);
       Schur = ParMult(&D, AinvDt);
    }

    Solver * invA;
    if (use_ADS)
        invA = new HypreADS(A, Sigma_space);
    else // using Diag(A);
         invA = new HypreDiagScale(A);

    invA->iterative_mode = false;

    Solver * invC;
    if (strcmp(struct_formul.space_for_S,"H1") == 0) // S is from H1
    {
        invC = new HypreBoomerAMG(*C);
        ((HypreBoomerAMG*)invC)->SetPrintLevel(0);
        ((HypreBoomerAMG*)invC)->iterative_mode = false;
    }

    Solver * invS;
    if (struct_formul.have_constraint)
    {
         invS = new HypreBoomerAMG(*Schur);
         ((HypreBoomerAMG *)invS)->SetPrintLevel(0);
         ((HypreBoomerAMG *)invS)->iterative_mode = false;
    }

    prec = new BlockDiagonalPreconditioner(blkoffsets_true);
    if (prec_option > 0)
    {
        int tempblknum = 0;
        prec->SetDiagonalBlock(tempblknum, invA);
        tempblknum++;
        if (strcmp(struct_formul.space_for_S,"H1") == 0) // S is present
        {
            prec->SetDiagonalBlock(tempblknum, invC);
            tempblknum++;
        }
        if (struct_formul.have_constraint)
             prec->SetDiagonalBlock(tempblknum, invS);

        if (verbose)
            std::cout << "Preconditioner built in " << chrono.RealTime() << "s. \n";
    }
    else
        if (verbose)
            cout << "No preconditioner is used. \n";
}


void CFOSLSHyperbolicProblem::Update()
{
    // update spaces
    Hdiv_space->Update();
    H1vec_space->Update();
    H1_space->Update();
    L2_space->Update();
    // this is not enough, better update all pfes as above
    //for (int i = 0; i < numblocks; ++i)
        //pfes[i]->Update();

    // update grid functions
    for (int i = 0; i < grfuns.Size(); ++i)
        grfuns[i]->Update();
}

GeneralHierarchy::GeneralHierarchy(int num_levels, ParMesh& pmesh, int feorder, bool verbose)
    : num_lvls (num_levels)
{
    int dim = pmesh.Dimension();

    FiniteElementCollection *hdiv_coll;
    FiniteElementCollection *l2_coll;

    if (dim == 4)
        hdiv_coll = new RT0_4DFECollection;
    else
        hdiv_coll = new RT_FECollection(feorder, dim);

    l2_coll = new L2_FECollection(feorder, dim);

    FiniteElementCollection *h1_coll;
    if (dim == 3)
        h1_coll = new H1_FECollection(feorder + 1, dim);
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

    ParFiniteElementSpace *Hdiv_space;
    Hdiv_space = new ParFiniteElementSpace(&pmesh, hdiv_coll);

    ParFiniteElementSpace *L2_space;
    L2_space = new ParFiniteElementSpace(&pmesh, l2_coll);

    ParFiniteElementSpace *H1_space;
    H1_space = new ParFiniteElementSpace(&pmesh, h1_coll);

    const SparseMatrix* P_Hdiv_local;
    const SparseMatrix* P_H1_local;
    const SparseMatrix* P_L2_local;

    pmesh_lvls.resize(num_lvls);
    Hdiv_space_lvls.resize(num_lvls);
    H1_space_lvls.resize(num_lvls);
    L2_space_lvls.resize(num_lvls);
    P_Hdiv_lvls.resize(num_lvls - 1);
    P_H1_lvls.resize(num_lvls - 1);
    P_L2_lvls.resize(num_lvls - 1);
    TrueP_Hdiv_lvls.resize(num_lvls - 1);
    TrueP_H1_lvls.resize(num_lvls - 1);
    TrueP_L2_lvls.resize(num_lvls - 1);

    //std::cout << "Checking test for dynamic cast \n";
    //if (dynamic_cast<testB*> (testA))
        //std::cout << "Unsuccessful cast \n";

    for (int l = num_lvls - 1; l >= 0; --l)
    {
        RefineAndCopy(l, &pmesh);

        // creating pfespaces for level l
        Hdiv_space_lvls[l] = new ParFiniteElementSpace(pmesh_lvls[l], hdiv_coll);
        L2_space_lvls[l] = new ParFiniteElementSpace(pmesh_lvls[l], l2_coll);
        H1_space_lvls[l] = new ParFiniteElementSpace(pmesh_lvls[l], h1_coll);

        // for all but one levels we create projection matrices between levels
        // and projectors assembled on true dofs if MG preconditioner is used
        if (l < num_lvls - 1)
        {
            Hdiv_space->Update();
            H1_space->Update();
            L2_space->Update();

            // TODO: Rewrite these computations

            P_Hdiv_local = (SparseMatrix *)Hdiv_space->GetUpdateOperator();
            P_Hdiv_lvls[l] = RemoveZeroEntries(*P_Hdiv_local);

            auto d_td_coarse_Hdiv = Hdiv_space_lvls[l + 1]->Dof_TrueDof_Matrix();
            SparseMatrix * RP_Hdiv_local = Mult(*Hdiv_space_lvls[l]->GetRestrictionMatrix(), *P_Hdiv_lvls[l]);
            TrueP_Hdiv_lvls[l] = d_td_coarse_Hdiv->LeftDiagMult(
                        *RP_Hdiv_local, Hdiv_space_lvls[l]->GetTrueDofOffsets());
            TrueP_Hdiv_lvls[l]->CopyColStarts();
            TrueP_Hdiv_lvls[l]->CopyRowStarts();

            delete RP_Hdiv_local;


            P_H1_local = (SparseMatrix *)H1_space->GetUpdateOperator();
            P_H1_lvls[l] = RemoveZeroEntries(*P_H1_local);

            auto d_td_coarse_H1 = H1_space_lvls[l + 1]->Dof_TrueDof_Matrix();
            SparseMatrix * RP_H1_local = Mult(*H1_space_lvls[l]->GetRestrictionMatrix(), *P_H1_lvls[l]);
            TrueP_H1_lvls[l] = d_td_coarse_H1->LeftDiagMult(
                        *RP_H1_local, H1_space_lvls[l]->GetTrueDofOffsets());
            TrueP_H1_lvls[l]->CopyColStarts();
            TrueP_H1_lvls[l]->CopyRowStarts();

            delete RP_H1_local;

            P_L2_local = (SparseMatrix *)L2_space->GetUpdateOperator();
            P_L2_lvls[l] = RemoveZeroEntries(*P_L2_local);

            auto d_td_coarse_L2 = L2_space_lvls[l + 1]->Dof_TrueDof_Matrix();
            SparseMatrix * RP_L2_local = Mult(*L2_space_lvls[l]->GetRestrictionMatrix(), *P_L2_lvls[l]);
            TrueP_L2_lvls[l] = d_td_coarse_L2->LeftDiagMult(
                        *RP_L2_local, L2_space_lvls[l]->GetTrueDofOffsets());
            TrueP_L2_lvls[l]->CopyColStarts();
            TrueP_L2_lvls[l]->CopyRowStarts();

            delete RP_L2_local;
        }

    } // end of loop over levels

}

/*
void GeneralCylHierarchy::RefineAndCopy(int lvl, ParMesh* pmesh)
{
    if (lvl == num_lvls - 1)
        pmesh_lvls[lvl] = new ParMesh(*pmesh);
    else
    {
        ParMeshCyl * pmeshcyl_view = dynamic_cast<ParMeshCyl*> (pmesh);
        if (!pmeshcyl_view)
        {
            MFEM_ABORT("Dynamic cast into ParMeshCyl returned NULL \n");
        }
        pmeshcyl_view->Refine(1);
        pmesh_lvls[lvl] = new ParMesh(*pmesh);
    }
}
*/

void GeneralCylHierarchy::ConstructRestrictions()
{
    Restrict_bot_H1_lvls.resize(num_lvls);
    Restrict_bot_Hdiv_lvls.resize(num_lvls);
    Restrict_top_H1_lvls.resize(num_lvls);
    Restrict_top_Hdiv_lvls.resize(num_lvls);

    for (int l = num_lvls - 1; l >= 0; --l)
    {
        Restrict_bot_H1_lvls[l] = CreateRestriction("bot", *H1_space_lvls[l], tdofs_link_H1_lvls[l]);
        Restrict_bot_Hdiv_lvls[l] = CreateRestriction("bot", *Hdiv_space_lvls[l], tdofs_link_Hdiv_lvls[l]);
        Restrict_top_H1_lvls[l] = CreateRestriction("top", *H1_space_lvls[l], tdofs_link_H1_lvls[l]);
        Restrict_top_Hdiv_lvls[l] = CreateRestriction("top", *Hdiv_space_lvls[l], tdofs_link_Hdiv_lvls[l]);
    }
}

void GeneralCylHierarchy::ConstructInterpolations()
{
    TrueP_bndbot_H1_lvls.resize(num_lvls - 1);
    TrueP_bndbot_Hdiv_lvls.resize(num_lvls - 1);
    TrueP_bndtop_H1_lvls.resize(num_lvls - 1);
    TrueP_bndtop_Hdiv_lvls.resize(num_lvls - 1);

    for (int l = num_lvls - 2; l >= 0; --l)
    {
        TrueP_bndbot_H1_lvls[l] = RAP(Restrict_bot_H1_lvls[l], TrueP_H1_lvls[l], Restrict_bot_H1_lvls[l + 1]);
        TrueP_bndbot_H1_lvls[l]->CopyColStarts();
        TrueP_bndbot_H1_lvls[l]->CopyRowStarts();

        TrueP_bndtop_H1_lvls[l] = RAP(Restrict_top_H1_lvls[l], TrueP_H1_lvls[l], Restrict_top_H1_lvls[l + 1]);
        TrueP_bndtop_H1_lvls[l]->CopyColStarts();
        TrueP_bndtop_H1_lvls[l]->CopyRowStarts();

        TrueP_bndbot_Hdiv_lvls[l] = RAP(Restrict_bot_Hdiv_lvls[l], TrueP_Hdiv_lvls[l], Restrict_bot_Hdiv_lvls[l + 1]);
        TrueP_bndbot_Hdiv_lvls[l]->CopyColStarts();
        TrueP_bndbot_Hdiv_lvls[l]->CopyRowStarts();

        TrueP_bndtop_Hdiv_lvls[l] = RAP(Restrict_top_Hdiv_lvls[l], TrueP_Hdiv_lvls[l], Restrict_top_Hdiv_lvls[l + 1]);
        TrueP_bndtop_Hdiv_lvls[l]->CopyColStarts();
        TrueP_bndtop_Hdiv_lvls[l]->CopyRowStarts();
    }
}

void GeneralCylHierarchy::ConstructTdofsLinks()
{
    //init_cond_size_lvls.resize(num_lvls);
    tdofs_link_H1_lvls.resize(num_lvls);
    tdofs_link_Hdiv_lvls.resize(num_lvls);

    for (int l = num_lvls - 1; l >= 0; --l)
    {
        std::vector<std::pair<int,int> > * dofs_link_H1 =
                CreateBotToTopDofsLink("linearH1",*H1_space_lvls[l], pmeshcyl_lvls[l]->bot_to_top_bels);
        std::cout << std::flush;

        tdofs_link_H1_lvls[l].reserve(dofs_link_H1->size());

        int count = 0;
        for ( unsigned int i = 0; i < dofs_link_H1->size(); ++i )
        {
            //std::cout << "<" << it->first << ", " << it->second << "> \n";
            int dof1 = (*dofs_link_H1)[i].first;
            int dof2 = (*dofs_link_H1)[i].second;
            int tdof1 = H1_space_lvls[l]->GetLocalTDofNumber(dof1);
            int tdof2 = H1_space_lvls[l]->GetLocalTDofNumber(dof2);
            //std::cout << "corr. dof pair: <" << dof1 << "," << dof2 << ">\n";
            //std::cout << "corr. tdof pair: <" << tdof1 << "," << tdof2 << ">\n";
            if (tdof1 * tdof2 < 0)
                MFEM_ABORT( "unsupported case: tdof1 and tdof2 belong to different processors! \n");

            if (tdof1 > -1)
            {
                tdofs_link_H1_lvls[l].push_back(std::pair<int,int>(tdof1, tdof2));
                ++count;
            }
            else
            {
                //std::cout << "Ignored dofs pair which are not own tdofs \n";
            }
        }

        std::vector<std::pair<int,int> > * dofs_link_RT0 =
                   CreateBotToTopDofsLink("RT0",*Hdiv_space_lvls[l], pmeshcyl_lvls[l]->bot_to_top_bels);
        std::cout << std::flush;

        tdofs_link_Hdiv_lvls[l].reserve(dofs_link_RT0->size());

        count = 0;
        //std::cout << "dof pairs for Hdiv: \n";
        for ( unsigned int i = 0; i < dofs_link_RT0->size(); ++i)
        {
            int dof1 = (*dofs_link_RT0)[i].first;
            int dof2 = (*dofs_link_RT0)[i].second;
            //std::cout << "<" << it->first << ", " << it->second << "> \n";
            int tdof1 = Hdiv_space_lvls[l]->GetLocalTDofNumber(dof1);
            int tdof2 = Hdiv_space_lvls[l]->GetLocalTDofNumber(dof2);
            //std::cout << "corr. tdof pair: <" << tdof1 << "," << tdof2 << ">\n";
            if ((tdof1 > 0 && tdof2 < 0) || (tdof1 < 0 && tdof2 > 0))
            {
                //std::cout << "Caught you! tdof1 = " << tdof1 << ", tdof2 = " << tdof2 << "\n";
                MFEM_ABORT( "unsupported case: tdof1 and tdof2 belong to different processors! \n");
            }

            if (tdof1 > -1)
            {
                tdofs_link_Hdiv_lvls[l].push_back(std::pair<int,int>(tdof1, tdof2));
                ++count;
            }
            else
            {
                //std::cout << "Ignored a dofs pair which are not own tdofs \n";
            }
        }
    }
}

HypreParMatrix * CreateRestriction(const char * top_or_bot, ParFiniteElementSpace& pfespace, std::vector<std::pair<int,int> >& bot_to_top_tdofs_link)
{
    if (strcmp(top_or_bot, "top") != 0 && strcmp(top_or_bot, "bot") != 0)
    {
        MFEM_ABORT ("In num_lvls() top_or_bot must be 'top' or 'bot'!\n");
    }

    MPI_Comm comm = pfespace.GetComm();

    int m = bot_to_top_tdofs_link.size();
    int n = pfespace.TrueVSize();
    int * ia = new int[m + 1];
    ia[0] = 0;
    for (int i = 0; i < m; ++i)
        ia[i + 1] = ia[i] + 1;
    int * ja = new int [ia[m]];
    double * data = new double [ia[m]];
    int count = 0;
    for (int row = 0; row < m; ++row)
    {
        if (strcmp(top_or_bot, "bot") == 0)
            ja[count] = bot_to_top_tdofs_link[row].first;
        else
            ja[count] = bot_to_top_tdofs_link[row].second;
        data[count] = 1.0;
        count++;
    }
    SparseMatrix * diag = new SparseMatrix(ia, ja, data, m, n);

    int local_size = bot_to_top_tdofs_link.size();
    int global_marked_tdofs = 0;
    MPI_Allreduce(&local_size, &global_marked_tdofs, 1, MPI_INT, MPI_SUM, comm);

    //std::cout << "Got after Allreduce \n";

    int global_num_rows = global_marked_tdofs;
    int global_num_cols = pfespace.GlobalTrueVSize();

    int num_procs;
    MPI_Comm_size(comm, &num_procs);

    int myid;
    MPI_Comm_rank(comm, &myid);

    int * local_row_offsets = new int[num_procs + 1];
    local_row_offsets[0] = 0;
    MPI_Allgather(&m, 1, MPI_INT, local_row_offsets + 1, 1, MPI_INT, comm);

    int * local_col_offsets = new int[num_procs + 1];
    local_col_offsets[0] = 0;
    MPI_Allgather(&n, 1, MPI_INT, local_col_offsets + 1, 1, MPI_INT, comm);

    for (int j = 1; j < num_procs + 1; ++j)
        local_row_offsets[j] += local_row_offsets[j - 1];

    for (int j = 1; j < num_procs + 1; ++j)
        local_col_offsets[j] += local_col_offsets[j - 1];

    int * row_starts = new int[3];
    row_starts[0] = local_row_offsets[myid];
    row_starts[1] = local_row_offsets[myid + 1];
    row_starts[2] = local_row_offsets[num_procs];
    int * col_starts = new int[3];
    col_starts[0] = local_col_offsets[myid];
    col_starts[1] = local_col_offsets[myid + 1];
    col_starts[2] = local_col_offsets[num_procs];

    /*
    for (int i = 0; i < num_procs; ++i)
    {
        if (myid == i)
        {
            std::cout << "I am " << myid << "\n";
            std::cout << "my local_row_offsets not summed: \n";
            for (int j = 0; j < num_procs + 1; ++j)
                std::cout << local_row_offsets[j] << " ";
            std::cout << "\n";

            std::cout << "my local_col_offsets not summed: \n";
            for (int j = 0; j < num_procs + 1; ++j)
                std::cout << local_col_offsets[j] << " ";
            std::cout << "\n";
            std::cout << "\n";

            for (int j = 1; j < num_procs + 1; ++j)
                local_row_offsets[j] += local_row_offsets[j - 1];

            for (int j = 1; j < num_procs + 1; ++j)
                local_col_offsets[j] += local_col_offsets[j - 1];

            std::cout << "my local_row_offsets: \n";
            for (int j = 0; j < num_procs + 1; ++j)
                std::cout << local_row_offsets[j] << " ";
            std::cout << "\n";

            std::cout << "my local_col_offsets: \n";
            for (int j = 0; j < num_procs + 1; ++j)
                std::cout << local_row_offsets[j] << " ";
            std::cout << "\n";
            std::cout << "\n";

            int * row_starts = new int[3];
            row_starts[0] = local_row_offsets[myid];
            row_starts[1] = local_row_offsets[myid + 1];
            row_starts[2] = local_row_offsets[num_procs];
            int * col_starts = new int[3];
            col_starts[0] = local_col_offsets[myid];
            col_starts[1] = local_col_offsets[myid + 1];
            col_starts[2] = local_col_offsets[num_procs];

            std::cout << "my computed row starts: \n";
            std::cout << row_starts[0] << " " <<  row_starts[1] << " " << row_starts[2];
            std::cout << "\n";

            std::cout << "my computed col starts: \n";
            std::cout << col_starts[0] << " " <<  col_starts[1] << " " << col_starts[2];
            std::cout << "\n";

            std::cout << std::flush;
        }

        MPI_Barrier(comm);
    } // end fo loop over all processors, one after another
    */


    // FIXME:
    // MFEM_ABORT("Don't know how to create row_starts and col_starts \n");

    //std::cout << "Creating resT \n";

    HypreParMatrix * resT = new HypreParMatrix(comm, global_num_rows, global_num_cols, row_starts, col_starts, diag);

    //std::cout << "resT created \n";


    HypreParMatrix * res = resT->Transpose();
    res->CopyRowStarts();
    res->CopyColStarts();

    //std::cout << "Got after resT creation \n";

    return res;
}

// eltype must be "linearH1" or "RT0", for any other finite element the code doesn't work
// the fespace must correspond to the eltype provided
// bot_to_top_bels is the link between boundary elements (at the bottom and at the top)
// which can be taken out of ParMeshCyl

std::vector<std::pair<int,int> >* CreateBotToTopDofsLink(const char * eltype, FiniteElementSpace& fespace,
                                                         std::vector<std::pair<int,int> > & bot_to_top_bels, bool verbose)
{
    if (strcmp(eltype, "linearH1") != 0 && strcmp(eltype, "RT0") != 0)
    {
        MFEM_ABORT ("Provided eltype is not supported in CreateBotToTopDofsLink: must be linearH1 or RT0 strictly! \n");
    }

    int nbelpairs = bot_to_top_bels.size();
    // estimating the maximal memory size required
    Array<int> dofs;
    fespace.GetBdrElementDofs(0, dofs);
    int ndofpairs_max = nbelpairs * dofs.Size();

    if (verbose)
        std::cout << "nbelpairs = " << nbelpairs << ", estimated ndofpairs_max = " << ndofpairs_max << "\n";

    std::vector<std::pair<int,int> > * res = new std::vector<std::pair<int,int> >;
    res->reserve(ndofpairs_max);

    std::set<std::pair<int,int> > res_set;

    Mesh * mesh = fespace.GetMesh();

    for (int i = 0; i < nbelpairs; ++i)
    {
        if (verbose)
            std::cout << "pair " << i << ": \n";

        if (strcmp(eltype, "RT0") == 0)
        {
            int belind_first = bot_to_top_bels[i].first;
            Array<int> bel_dofs_first;
            fespace.GetBdrElementDofs(belind_first, bel_dofs_first);

            int belind_second = bot_to_top_bels[i].second;
            Array<int> bel_dofs_second;
            fespace.GetBdrElementDofs(belind_second, bel_dofs_second);

            if (verbose)
            {
                std::cout << "belind1: " << belind_first << ", bel_dofs_first: \n";
                bel_dofs_first.Print();
                std::cout << "belind2: " << belind_second << ", bel_dofs_second: \n";
                bel_dofs_second.Print();
            }


            if (bel_dofs_first.Size() != 1 || bel_dofs_second.Size() != 1)
            {
                MFEM_ABORT("For RT0 exactly one dof must correspond to each boundary element \n");
            }

            if (res_set.find(std::pair<int,int>(bel_dofs_first[0], bel_dofs_second[0])) == res_set.end())
            {
                res_set.insert(std::pair<int,int>(bel_dofs_first[0], bel_dofs_second[0]));
                res->push_back(std::pair<int,int>(bel_dofs_first[0], bel_dofs_second[0]));
            }

        }

        if (strcmp(eltype, "linearH1") == 0)
        {
            int belind_first = bot_to_top_bels[i].first;
            Array<int> bel_dofs_first;
            fespace.GetBdrElementDofs(belind_first, bel_dofs_first);

            Array<int> belverts_first;
            mesh->GetBdrElementVertices(belind_first, belverts_first);

            int nverts = mesh->GetBdrElement(belind_first)->GetNVertices();

            int belind_second = bot_to_top_bels[i].second;
            Array<int> bel_dofs_second;
            fespace.GetBdrElementDofs(belind_second, bel_dofs_second);

            if (verbose)
            {
                std::cout << "belind1: " << belind_first << ", bel_dofs_first: \n";
                bel_dofs_first.Print();
                std::cout << "belind2: " << belind_second << ", bel_dofs_second: \n";
                bel_dofs_second.Print();
            }

            Array<int> belverts_second;
            mesh->GetBdrElementVertices(belind_second, belverts_second);


            if (bel_dofs_first.Size() != nverts || bel_dofs_second.Size() != nverts)
            {
                MFEM_ABORT("For linearH1 exactly #bel.vertices of dofs must correspond to each boundary element \n");
            }

            /*
            Array<int> P, Po;
            fespace.GetMesh()->GetBdrElementPlanars(i, P, Po);

            std::cout << "P: \n";
            P.Print();
            std::cout << "Po: \n";
            Po.Print();

            Array<int> belverts_first;
            mesh->GetBdrElementVertices(belind_first, belverts_first);
            */

            std::vector<std::vector<double> > vertscoos_first(nverts);
            if (verbose)
                std::cout << "verts of first bdr el \n";
            for (int vert = 0; vert < nverts; ++vert)
            {
                vertscoos_first[vert].resize(mesh->Dimension());
                double * vertcoos = mesh->GetVertex(belverts_first[vert]);
                if (verbose)
                    std::cout << "vert = " << vert << ": ";
                for (int j = 0; j < mesh->Dimension(); ++j)
                {
                    vertscoos_first[vert][j] = vertcoos[j];
                    if (verbose)
                        std::cout << vertcoos[j] << " ";
                }
                if (verbose)
                    std::cout << "\n";
            }

            int * verts_permutation_first = new int[nverts];
            sortingPermutationNew(vertscoos_first, verts_permutation_first);

            if (verbose)
            {
                std::cout << "permutation first: ";
                for (int i = 0; i < mesh->Dimension(); ++i)
                    std::cout << verts_permutation_first[i] << " ";
                std::cout << "\n";
            }

            std::vector<std::vector<double> > vertscoos_second(nverts);
            if (verbose)
                std::cout << "verts of second bdr el \n";
            for (int vert = 0; vert < nverts; ++vert)
            {
                vertscoos_second[vert].resize(mesh->Dimension());
                double * vertcoos = mesh->GetVertex(belverts_second[vert]);
                if (verbose)
                    std::cout << "vert = " << vert << ": ";
                for (int j = 0; j < mesh->Dimension(); ++j)
                {
                    vertscoos_second[vert][j] = vertcoos[j];
                    if (verbose)
                        std::cout << vertcoos[j] << " ";
                }
                if (verbose)
                    std::cout << "\n";
            }

            int * verts_permutation_second = new int[nverts];
            sortingPermutationNew(vertscoos_second, verts_permutation_second);

            if (verbose)
            {
                std::cout << "permutation second: ";
                for (int i = 0; i < mesh->Dimension(); ++i)
                    std::cout << verts_permutation_second[i] << " ";
                std::cout << "\n";
            }

            /*
            int * verts_perm_second_inverse = new int[nverts];
            invert_permutation(verts_permutation_second, nverts, verts_perm_second_inverse);

            if (verbose)
            {
                std::cout << "inverted permutation second: ";
                for (int i = 0; i < mesh->Dimension(); ++i)
                    std::cout << verts_perm_second_inverse[i] << " ";
                std::cout << "\n";
            }
            */

            int * verts_perm_first_inverse = new int[nverts];
            invert_permutation(verts_permutation_first, nverts, verts_perm_first_inverse);

            if (verbose)
            {
                std::cout << "inverted permutation first: ";
                for (int i = 0; i < mesh->Dimension(); ++i)
                    std::cout << verts_perm_first_inverse[i] << " ";
                std::cout << "\n";
            }


            for (int dofno = 0; dofno < bel_dofs_first.Size(); ++dofno)
            {
                //int dofno_second = verts_perm_second_inverse[verts_permutation_first[dofno]];
                int dofno_second = verts_permutation_second[verts_perm_first_inverse[dofno]];

                if (res_set.find(std::pair<int,int>(bel_dofs_first[dofno], bel_dofs_second[dofno_second])) == res_set.end())
                {
                    res_set.insert(std::pair<int,int>(bel_dofs_first[dofno], bel_dofs_second[dofno_second]));
                    res->push_back(std::pair<int,int>(bel_dofs_first[dofno], bel_dofs_second[dofno_second]));
                }
                //res_set.insert(std::pair<int,int>(bel_dofs_first[dofno],
                                                  //bel_dofs_second[dofno_second]));

                if (verbose)
                    std::cout << "matching dofs pair: <" << bel_dofs_first[dofno] << ","
                          << bel_dofs_second[dofno_second] << "> \n";
            }

            if (verbose)
               std::cout << "\n";
        }

    } // end of loop over all pairs of boundary elements

    if (verbose)
    {
        if (strcmp(eltype,"RT0") == 0)
            std::cout << "dof pairs for Hdiv: \n";
        if (strcmp(eltype,"linearH1") == 0)
            std::cout << "dof pairs for H1: \n";
        std::set<std::pair<int,int> >::iterator it;
        for ( unsigned int i = 0; i < res->size(); ++i )
        {
            std::cout << "<" << (*res)[i].first << ", " << (*res)[i].second << "> \n";
        }
    }


    return res;
}

SparseMatrix * RemoveZeroEntries(const SparseMatrix& in)
{
    int * I = in.GetI();
    int * J = in.GetJ();
    double * Data = in.GetData();
    double * End = Data+in.NumNonZeroElems();

    int nnz = 0;
    for (double * data_ptr = Data; data_ptr != End; data_ptr++)
    {
        if (*data_ptr != 0)
            nnz++;
    }

    int * outI = new int[in.Height()+1];
    int * outJ = new int[nnz];
    double * outData = new double[nnz];
    nnz = 0;
    for (int i = 0; i < in.Height(); i++)
    {
        outI[i] = nnz;
        for (int j = I[i]; j < I[i+1]; j++)
        {
            if (Data[j] !=0)
            {
                outJ[nnz] = J[j];
                outData[nnz++] = Data[j];
            }
        }
    }
    outI[in.Height()] = nnz;

    return new SparseMatrix(outI, outJ, outData, in.Height(), in.Width());
}

// Eliminates all entries in the Operator acting in a pair of spaces,
// assembled as a HypreParMatrix, which connect internal dofs to boundary dofs
// Used to modife the Curl and Divskew operator for the new multigrid solver
void Eliminate_ib_block(HypreParMatrix& Op_hpmat, const Array<int>& EssBdrTrueDofs_dom, const Array<int>& EssBdrTrueDofs_range )
{
    MPI_Comm comm = Op_hpmat.GetComm();

    int ntdofs_dom = Op_hpmat.Width();
    Array<int> btd_flags(ntdofs_dom);
    btd_flags = 0;
    //if (verbose)
        //std::cout << "EssBdrTrueDofs_dom \n";
    //EssBdrTrueDofs_dom.Print();

    for ( int i = 0; i < EssBdrTrueDofs_dom.Size(); ++i )
    {
        int tdof = EssBdrTrueDofs_dom[i];
        btd_flags[tdof] = 1;
    }

    int * td_btd_i = new int[ ntdofs_dom + 1];
    td_btd_i[0] = 0;
    for (int i = 0; i < ntdofs_dom; ++i)
        td_btd_i[i + 1] = td_btd_i[i] + 1;

    int * td_btd_j = new int [td_btd_i[ntdofs_dom]];
    double * td_btd_data = new double [td_btd_i[ntdofs_dom]];
    for (int i = 0; i < ntdofs_dom; ++i)
    {
        td_btd_j[i] = i;
        if (btd_flags[i] != 0)
            td_btd_data[i] = 1.0;
        else
            td_btd_data[i] = 0.0;
    }

    SparseMatrix * td_btd_diag = new SparseMatrix(td_btd_i, td_btd_j, td_btd_data, ntdofs_dom, ntdofs_dom);

    HYPRE_Int * row_starts = Op_hpmat.GetColStarts();

    HypreParMatrix * td_btd_hpmat = new HypreParMatrix(comm, Op_hpmat.N(),
            row_starts, td_btd_diag);
    td_btd_hpmat->CopyColStarts();
    td_btd_hpmat->CopyRowStarts();

    HypreParMatrix * C_td_btd = ParMult(&Op_hpmat, td_btd_hpmat);

    // processing local-to-process block of the Divfree matrix
    SparseMatrix C_td_btd_diag;
    C_td_btd->GetDiag(C_td_btd_diag);

    //C_td_btd_diag.Print();

    SparseMatrix C_diag;
    Op_hpmat.GetDiag(C_diag);

    //C_diag.Print();

    int ntdofs_range = Op_hpmat.Height();

    //std::cout << "Op_hpmat = " << Op_hpmat.Height() << " x " << Op_hpmat.Width() << "\n";
    Array<int> btd_flags_range(ntdofs_range);
    btd_flags_range = 0;
    for ( int i = 0; i < EssBdrTrueDofs_range.Size(); ++i )
    {
        int tdof = EssBdrTrueDofs_range[i];
        btd_flags_range[tdof] = 1;
    }

    //if (verbose)
        //std::cout << "EssBdrTrueDofs_range \n";
    //EssBdrTrueDofs_range.Print();

    for (int row = 0; row < C_td_btd_diag.Height(); ++row)
    {
        if (btd_flags_range[row] == 0)
        {
            for (int colind = 0; colind < C_td_btd_diag.RowSize(row); ++colind)
            {
                int nnz_ind = C_td_btd_diag.GetI()[row] + colind;
                int col = C_td_btd_diag.GetJ()[nnz_ind];
                double fabs_entry = fabs(C_td_btd_diag.GetData()[nnz_ind]);

                if (fabs_entry > 1.0e-14)
                {
                    for (int j = 0; j < C_diag.RowSize(row); ++j)
                    {
                        int colorig = C_diag.GetJ()[C_diag.GetI()[row] + j];
                        if (colorig == col)
                        {
                            //std::cout << "Changes made in row = " << row << ", col = " << colorig << "\n";
                            C_diag.GetData()[C_diag.GetI()[row] + j] = 0.0;

                        }
                    }
                } // else of if fabs_entry is large enough

            }
        } // end of if row corresponds to the non-boundary range dof
    }

    //C_diag.Print();

    // processing the off-diagonal block of the Divfree matrix
    SparseMatrix C_td_btd_offd;
    HYPRE_Int * C_td_btd_cmap;
    C_td_btd->GetOffd(C_td_btd_offd, C_td_btd_cmap);

    SparseMatrix C_offd;
    HYPRE_Int * C_cmap;
    Op_hpmat.GetOffd(C_offd, C_cmap);

    //int * row_starts = Op_hpmat.GetRowStarts();

    for (int row = 0; row < C_td_btd_offd.Height(); ++row)
    {
        if (btd_flags_range[row] == 0)
        {
            for (int colind = 0; colind < C_td_btd_offd.RowSize(row); ++colind)
            {
                int nnz_ind = C_td_btd_offd.GetI()[row] + colind;
                int truecol = C_td_btd_cmap[C_td_btd_offd.GetJ()[nnz_ind]];
                double fabs_entry = fabs(C_td_btd_offd.GetData()[nnz_ind]);

                if (fabs_entry > 1.0e-14)
                {
                    for (int j = 0; j < C_offd.RowSize(row); ++j)
                    {
                        int col = C_offd.GetJ()[C_offd.GetI()[row] + j];
                        int truecolorig = C_cmap[col];
                        /*
                        int tdof_for_truecolorig;
                        if (truecolorig < row_starts[0])
                            tdof_for_truecolorig = truecolorig;
                        else
                            tdof_for_truecolorig = truecolorig - row_starts[1];
                        */
                        if (truecolorig == truecol)
                        {
                            //std::cout << "Changes made in off-d: row = " << row << ", col = " << col << ", truecol = " << truecolorig << "\n";
                            C_offd.GetData()[C_offd.GetI()[row] + j] = 0.0;

                        }
                    }
                } // else of if fabs_entry is large enough

            }
        } // end of if row corresponds to the non-boundary range dof

    }
}


// Replaces "bb" block in the Operator acting in the same space,
// assembled as a HypreParMatrix, which connects boundary dofs to boundary dofs by identity
void Eliminate_bb_block(HypreParMatrix& Op_hpmat, const Array<int>& EssBdrTrueDofs )
{
    MFEM_ASSERT(Op_hpmat.Width() == Op_hpmat.Height(), "The matrix must be square in Eliminate_bb_block()! \n");

    int ntdofs = Op_hpmat.Width();

    Array<int> btd_flags(ntdofs);
    btd_flags = 0;
    //if (verbose)
        //std::cout << "EssBdrTrueDofs \n";
    //EssBdrTrueDofs.Print();

    for ( int i = 0; i < EssBdrTrueDofs.Size(); ++i )
    {
        int tdof = EssBdrTrueDofs[i];
        btd_flags[tdof] = 1;
    }

    SparseMatrix C_diag;
    Op_hpmat.GetDiag(C_diag);

    // processing local-to-process block of the matrix
    for (int row = 0; row < C_diag.Height(); ++row)
    {
        if (btd_flags[row] != 0) // if the row tdof is at the boundary
        {
            for (int j = 0; j < C_diag.RowSize(row); ++j)
            {
                int col = C_diag.GetJ()[C_diag.GetI()[row] + j];
                if  (col == row)
                    C_diag.GetData()[C_diag.GetI()[row] + j] = 1.0;
                else
                    C_diag.GetData()[C_diag.GetI()[row] + j] = 0.0;
            }
        } // end of if row corresponds to the boundary tdof
    }

    //C_diag.Print();

    SparseMatrix C_offd;
    HYPRE_Int * C_cmap;
    Op_hpmat.GetOffd(C_offd, C_cmap);

    // processing the off-diagonal block of the matrix
    for (int row = 0; row < C_offd.Height(); ++row)
    {
        if (btd_flags[row] != 0) // if the row tdof is at the boundary
        {
            for (int j = 0; j < C_offd.RowSize(row); ++j)
            {
                C_offd.GetData()[C_offd.GetI()[row] + j] = 0.0;
            }

        } // end of if row corresponds to the boundary tdof

    }
}

/*
// self-written copy routine for HypreParMatrices
// faces the issues with LeftDiagMult and ParMult combination
// My guess is that offd.num_rownnz != 0 is the bug
// but no proof for now
HypreParMatrix * CopyHypreParMatrix (HypreParMatrix& inputmat)
{
    MPI_Comm comm = inputmat.GetComm();
    int num_procs;
    MPI_Comm_size(comm, &num_procs);

    HYPRE_Int global_num_rows = inputmat.M();
    HYPRE_Int global_num_cols = inputmat.N();

    int size_starts = num_procs;
    if (num_procs > 1) // in thi case offd exists
    {
        //int myid;
        //MPI_Comm_rank(comm,&myid);

        HYPRE_Int * row_starts_in = inputmat.GetRowStarts();
        HYPRE_Int * col_starts_in = inputmat.GetColStarts();

        HYPRE_Int * row_starts = new HYPRE_Int[num_procs];
        memcpy(row_starts, row_starts_in, size_starts * sizeof(HYPRE_Int));
        HYPRE_Int * col_starts = new HYPRE_Int[num_procs];
        memcpy(col_starts, col_starts_in, size_starts * sizeof(HYPRE_Int));

        //std::cout << "memcpy calls finished \n";

        SparseMatrix diag_in;
        inputmat.GetDiag(diag_in);
        SparseMatrix * diag_out = new SparseMatrix(diag_in);

        //std::cout << "diag copied \n";

        SparseMatrix offdiag_in;
        HYPRE_Int * offdiag_cmap_in;
        inputmat.GetOffd(offdiag_in, offdiag_cmap_in);

        int size_offdiag_cmap = offdiag_in.Width();

        SparseMatrix * offdiag_out = new SparseMatrix(offdiag_in);
        HYPRE_Int * offdiag_cmap_out = new HYPRE_Int[size_offdiag_cmap];

        memcpy(offdiag_cmap_out, offdiag_cmap_in, size_offdiag_cmap * sizeof(int));


        return new HypreParMatrix(comm, global_num_rows, global_num_cols,
                                  row_starts, col_starts,
                                  diag_out, offdiag_out, offdiag_cmap_out);

        //std::cout << "constructor called \n";
    }
    else // in this case offd doesn't exist and we have to use a different constructor
    {
        HYPRE_Int * row_starts = new HYPRE_Int[2];
        row_starts[0] = 0;
        row_starts[1] = global_num_rows;
        HYPRE_Int * col_starts = new HYPRE_Int[2];
        col_starts[0] = 0;
        col_starts[1] = global_num_cols;

        SparseMatrix diag_in;
        inputmat.GetDiag(diag_in);
        SparseMatrix * diag_out = new SparseMatrix(diag_in);

        return new HypreParMatrix(comm, global_num_rows, global_num_cols,
                                  row_starts, col_starts, diag_out);
    }

}

// faces the same issues as CopyHypreParMatrix
HypreParMatrix * CopyRAPHypreParMatrix (HypreParMatrix& inputmat)
{
    MPI_Comm comm = inputmat.GetComm();
    int num_procs;
    MPI_Comm_size(comm, &num_procs);

    HYPRE_Int global_num_rows = inputmat.M();
    HYPRE_Int global_num_cols = inputmat.N();

    int size_starts = 2;

    HYPRE_Int * row_starts_in = inputmat.GetRowStarts();
    HYPRE_Int * col_starts_in = inputmat.GetColStarts();

    HYPRE_Int * row_starts = new HYPRE_Int[num_procs];
    memcpy(row_starts, row_starts_in, size_starts * sizeof(HYPRE_Int));
    HYPRE_Int * col_starts = new HYPRE_Int[num_procs];
    memcpy(col_starts, col_starts_in, size_starts * sizeof(HYPRE_Int));

    int num_local_rows = row_starts[1] - row_starts[0];
    int num_local_cols = col_starts[1] - col_starts[0];
    int * ia_id = new int[num_local_rows + 1];
    ia_id[0] = 0;
    for ( int i = 0; i < num_local_rows; ++i)
        ia_id[i + 1] = ia_id[i] + 1;

    int id_nnz = num_local_rows;
    int * ja_id = new int[id_nnz];
    double * a_id = new double[id_nnz];
    for ( int i = 0; i < id_nnz; ++i)
    {
        ja_id[i] = i;
        a_id[i] = 1.0;
    }

    SparseMatrix * id_diag = new SparseMatrix(ia_id, ja_id, a_id, num_local_rows, num_local_cols);

    HypreParMatrix * id = new HypreParMatrix(comm, global_num_rows, global_num_cols,
                                             row_starts, col_starts, id_diag);

    return RAP(&inputmat,id);
}
*/

} // for namespace mfem