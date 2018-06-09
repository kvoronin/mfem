#include <iostream>
#include "testhead.hpp"

#ifndef MFEM_CFOSLS_ESTIMATORS
#define MFEM_CFOSLS_ESTIMATORS

using namespace std;
using namespace mfem;

namespace mfem
{

// old implementation, now it's a simplified form of the more general, blocked case
// here FOSLS functional is given as a bilinear form(sigma, sigma)
double FOSLSErrorEstimator(BilinearFormIntegrator &blfi,
                           GridFunction &sigma, Vector &error_estimates);


/// more general implementation of a class for FOSLS error estimator.
/// The FOSLS functional is given as a symmetric block matrix with bilinear forms for
/// different grid functions (for all solution and rhs components)
double FOSLSErrorEstimator(Array2D<BilinearFormIntegrator*> &blfis,
                           Array<ParGridFunction*> & grfuns, Vector &error_estimates);

class FOSLSEstimator : public ErrorEstimator
{
protected:
    MPI_Comm comm;
    const int numblocks;
    long current_sequence;
    Array<ParGridFunction*> grfuns; // these are not owned by the estimator
    Array2D<BilinearFormIntegrator*> integs; // those as well
    Vector error_estimates;
    double global_total_error;
    bool verbose;

    /// Checks if the mesh of the solution was modified.
    bool MeshIsModified();

    /// Main function. Computes the element error estimates.
    void ComputeEstimates();
public:
    // Constructor which explicitly takes all the grid functions as an input.
    // The local error estimator is using the locally assembled forms provided
    // as integrators in the input
    FOSLSEstimator(MPI_Comm Comm, Array<ParGridFunction*>& solutions,
                   Array2D<BilinearFormIntegrator*>& integrators, bool verbose_ = false);

    // Constructor which takes some of the grid functions from the given FOSLS problem
    // via grfuns descriptor and can additionally take extra grid functions (which are not present
    // in the problem)
    // The definition of grfuns_desciptor is given at the definition of this constructor
    FOSLSEstimator(FOSLSProblem& problem, std::vector<std::pair<int,int> > & grfuns_descriptor,
                   Array<ParGridFunction *> *extra_grfuns, Array2D<BilinearFormIntegrator*>& integrators,
                   bool verbose_ = false);

    virtual const Vector & GetLocalErrors () override;
    double GetEstimate() {ComputeEstimates(); return global_total_error;}
    virtual void Reset () override { current_sequence = -1; }

    // This routine is called by the FOSLSProblem::Update() if the
    // estimator was added via AddEstimator() to the problem
    void Update();
};

/// A templated class for a FOSLSEsrimator which lives on the hierarchy of problems(meshes)
/// The difference with the base class is that when more levels are added to the hierarchy,
/// the finest level problem is created on the fly, and thus one has to change the definition of the
/// grid functions invlolved in the estimator. This is done automatically via RedefineGrFuns()
/// With that, the user must update the extra grid functions (if used) manually
template <class Problem, class Hierarchy>
class FOSLSEstimatorOnHier : public FOSLSEstimator
{
protected:
    FOSLSProblHierarchy<Problem, Hierarchy> &prob_hierarchy;
    const int level;
    std::vector<std::pair<int,int> > & grfuns_descriptor;
    // unlike FOSLSEstimator, we have to store this because
    // the user has to delete and recreate extra grfuns
    // outside of this (and hierarchy) class(es)
    Array<ParGridFunction *> *extra_grfuns;
    int update_counter;
public:
    FOSLSEstimatorOnHier(FOSLSProblHierarchy<Problem, Hierarchy> & prob_hierarchy_, int level_,
                         std::vector<std::pair<int,int> > & grfuns_descriptor_,
                         Array<ParGridFunction *> *extra_grfuns_,
                         Array2D<BilinearFormIntegrator*>& integrators,
                         bool verbose_ = false);

    virtual const Vector & GetLocalErrors () override;

    void RedefineGrFuns();
};

template <class Problem, class Hierarchy>
FOSLSEstimatorOnHier<Problem, Hierarchy>::FOSLSEstimatorOnHier(FOSLSProblHierarchy<Problem, Hierarchy> & prob_hierarchy_,
                                                               int level_,
                                                               std::vector<std::pair<int,int> > & grfuns_descriptor_,
                                                               Array<ParGridFunction *> *extra_grfuns_,
                                                               Array2D<BilinearFormIntegrator*>& integrators,
                                                               bool verbose_)
    : FOSLSEstimator(*prob_hierarchy_.GetProblem(level_), grfuns_descriptor_,
                     extra_grfuns_, integrators, verbose_),
      prob_hierarchy(prob_hierarchy_), level(level_),
      grfuns_descriptor(grfuns_descriptor_),
      extra_grfuns(extra_grfuns_),
      update_counter(prob_hierarchy.GetUpdateCounter() - 1)
{}

template <class Problem, class Hierarchy>
const Vector & FOSLSEstimatorOnHier<Problem, Hierarchy>::GetLocalErrors()
{
    int hierarchy_upd_cnt = prob_hierarchy.GetUpdateCounter();
    if (update_counter != hierarchy_upd_cnt) // if hierarchy was updated but the estimator not yet
    {
        MFEM_ASSERT(update_counter == hierarchy_upd_cnt - 1,
                    "Current implementation allows the update counters to differ no more than by one");
        RedefineGrFuns();
        ComputeEstimates();
        update_counter = hierarchy_upd_cnt;
    }
    return error_estimates;
}

template <class Problem, class Hierarchy>
void FOSLSEstimatorOnHier<Problem, Hierarchy>::RedefineGrFuns()
{
    for (int i = 0; i < numblocks; ++i)
    {
        MFEM_ASSERT(grfuns_descriptor[i].first == 1 || grfuns_descriptor[i].first == -1,
                    "Values of grfuns_descriptor must be either 1 or -1");
        if (grfuns_descriptor[i].first == 1)
            grfuns[i] = prob_hierarchy.GetProblem(0)->GetGrFuns()[grfuns_descriptor[i].second];
        else
        {
            MFEM_ASSERT(extra_grfuns, "Trying to use extra_grfuns which is NULL \n");
            grfuns[i] = (*extra_grfuns)[grfuns_descriptor[i].second];
        }
    }
}

} // for namespace mfem


#endif