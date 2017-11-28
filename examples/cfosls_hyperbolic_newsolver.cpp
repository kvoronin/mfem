//
//                        MFEM CFOSLS Transport equation with multigrid (debugging & testing of a new multilevel solver)
//

#include "mfem.hpp"
#include <fstream>
#include <iostream>
#include <memory>
#include <iomanip>
#include <list>

#include "cfosls_testsuite.hpp"

// (de)activates solving of the discrete global problem
#define OLD_CODE

// for the new multilevel solver's implementation
#define NEW_STUFF

// switches on/off usage of smoother in the new minimization solver
//#define WITH_SMOOTHER

// activates a test where new solver is used as a preconditioner
//#define USE_AS_A_PREC

// activates a check for the symmetry of the new solver
//#define CHECK_SPDSOLVER

// initializes sigma with exact solution
// if combined with COMPUTING_LAMBDA, it will
// be exact discrete solution,
// else a projection of the exact solution
//#define EXACTSOLH_INIT
//#define COMPUTING_LAMBDA

#include "divfree_solver_tools.hpp"

// must be always active
#define USE_CURLMATRIX

//#define BAD_TEST
//#define ONLY_DIVFREEPART
//#define K_IDENTITY

#define MYZEROTOL (1.0e-13)

using namespace std;
using namespace mfem;
using std::unique_ptr;
using std::shared_ptr;
using std::make_shared;

class VectorcurlDomainLFIntegrator : public LinearFormIntegrator
{
    DenseMatrix curlshape;
    DenseMatrix curlshape_dFadj;
    DenseMatrix curlshape_dFT;
    DenseMatrix dF_curlshape;
    VectorCoefficient &VQ;
    int oa, ob;
public:
    /// Constructs a domain integrator with a given Coefficient
    VectorcurlDomainLFIntegrator(VectorCoefficient &VQF, int a = 2, int b = 0)
        : VQ(VQF), oa(a), ob(b) { }

    /// Constructs a domain integrator with a given Coefficient
    VectorcurlDomainLFIntegrator(VectorCoefficient &VQF, const IntegrationRule *ir)
        : LinearFormIntegrator(ir), VQ(VQF), oa(1), ob(1) { }

    /** Given a particular Finite Element and a transformation (Tr)
       computes the element right hand side element vector, elvect. */
    virtual void AssembleRHSElementVect(const FiniteElement &el,
                                        ElementTransformation &Tr,
                                        Vector &elvect);

    using LinearFormIntegrator::AssembleRHSElementVect;
};

void VectorcurlDomainLFIntegrator::AssembleRHSElementVect(
        const FiniteElement &el, ElementTransformation &Tr, Vector &elvect)
{
    int dof = el.GetDof();

    int dim = el.GetDim();
    MFEM_ASSERT(dim == 3, "VectorcurlDomainLFIntegrator is working only in 3D currently \n");

    curlshape.SetSize(dof,3);           // matrix of size dof x 3, works only in 3D
    curlshape_dFadj.SetSize(dof,3);     // matrix of size dof x 3, works only in 3D
    curlshape_dFT.SetSize(dof,3);       // matrix of size dof x 3, works only in 3D
    dF_curlshape.SetSize(3,dof);        // matrix of size dof x 3, works only in 3D
    Vector vecval(3);
    //Vector vecval_new(3);
    //DenseMatrix invdfdx(3,3);

    const IntegrationRule *ir = IntRule;
    if (ir == NULL)
    {
        // ir = &IntRules.Get(el.GetGeomType(), oa * el.GetOrder() + ob + Tr.OrderW());
        ir = &IntRules.Get(el.GetGeomType(), oa * el.GetOrder() + ob);
        // int order = 2 * el.GetOrder() ; // <--- OK for RTk
        // ir = &IntRules.Get(el.GetGeomType(), order);
    }

    elvect.SetSize(dof);
    elvect = 0.0;

    for (int i = 0; i < ir->GetNPoints(); i++)
    {
        const IntegrationPoint &ip = ir->IntPoint(i);
        el.CalcCurlShape(ip, curlshape);

        Tr.SetIntPoint (&ip);

        VQ.Eval(vecval,Tr,ip);                  // plain evaluation

        MultABt(curlshape, Tr.Jacobian(), curlshape_dFT);

        curlshape_dFT.AddMult_a(ip.weight, vecval, elvect);
    }

}

class VectordivDomainLFIntegrator : public LinearFormIntegrator
{
    Vector divshape;
    Coefficient &Q;
    int oa, ob;
public:
    /// Constructs a domain integrator with a given Coefficient
    VectordivDomainLFIntegrator(Coefficient &QF, int a = 2, int b = 0)
    // the old default was a = 1, b = 1
    // for simple elliptic problems a = 2, b = -2 is ok
        : Q(QF), oa(a), ob(b) { }

    /// Constructs a domain integrator with a given Coefficient
    VectordivDomainLFIntegrator(Coefficient &QF, const IntegrationRule *ir)
        : LinearFormIntegrator(ir), Q(QF), oa(1), ob(1) { }

    /** Given a particular Finite Element and a transformation (Tr)
       computes the element right hand side element vector, elvect. */
    virtual void AssembleRHSElementVect(const FiniteElement &el,
                                        ElementTransformation &Tr,
                                        Vector &elvect);

    using LinearFormIntegrator::AssembleRHSElementVect;
};
//---------

//------------------
void VectordivDomainLFIntegrator::AssembleRHSElementVect(
        const FiniteElement &el, ElementTransformation &Tr, Vector &elvect)//don't need the matrix but the vector
{
    int dof = el.GetDof();

    divshape.SetSize(dof);       // vector of size dof
    elvect.SetSize(dof);
    elvect = 0.0;

    const IntegrationRule *ir = IntRule;
    if (ir == NULL)
    {
        // ir = &IntRules.Get(el.GetGeomType(), oa * el.GetOrder() + ob + Tr.OrderW());
        ir = &IntRules.Get(el.GetGeomType(), oa * el.GetOrder() + ob);
        // int order = 2 * el.GetOrder() ; // <--- OK for RTk
        // ir = &IntRules.Get(el.GetGeomType(), order);
    }

    for (int i = 0; i < ir->GetNPoints(); i++)
    {
        const IntegrationPoint &ip = ir->IntPoint(i);
        el.CalcDivShape(ip, divshape);

        Tr.SetIntPoint (&ip);
        //double val = Tr.Weight() * Q.Eval(Tr, ip);
        // Chak: Looking at how MFEM assembles in VectorFEDivergenceIntegrator, I think you dont need Tr.Weight() here
        // I think this is because the RT (or other vector FE) basis is scaled by the geometry of the mesh
        double val = Q.Eval(Tr, ip);

        add(elvect, ip.weight * val, divshape, elvect);
        //cout << "elvect = " << elvect << endl;
    }
}

class GradDomainLFIntegrator : public LinearFormIntegrator
{
    DenseMatrix dshape;
    DenseMatrix invdfdx;
    DenseMatrix dshapedxt;
    Vector bf;
    Vector bfdshapedxt;
    VectorCoefficient &Q;
    int oa, ob;
public:
    /// Constructs a domain integrator with a given Coefficient
    GradDomainLFIntegrator(VectorCoefficient &QF, int a = 2, int b = 0)
    // the old default was a = 1, b = 1
    // for simple elliptic problems a = 2, b = -2 is ok
        : Q(QF), oa(a), ob(b) { }

    /// Constructs a domain integrator with a given Coefficient
    GradDomainLFIntegrator(VectorCoefficient &QF, const IntegrationRule *ir)
        : LinearFormIntegrator(ir), Q(QF), oa(1), ob(1) { }

    /** Given a particular Finite Element and a transformation (Tr)
        computes the element right hand side element vector, elvect. */
    virtual void AssembleRHSElementVect(const FiniteElement &el,
                                        ElementTransformation &Tr,
                                        Vector &elvect);

    using LinearFormIntegrator::AssembleRHSElementVect;
};

void GradDomainLFIntegrator::AssembleRHSElementVect(
        const FiniteElement &el, ElementTransformation &Tr, Vector &elvect)
{
    int dof = el.GetDof();
    int dim  = el.GetDim();

    dshape.SetSize(dof,dim);       // vector of size dof
    elvect.SetSize(dof);
    elvect = 0.0;

    invdfdx.SetSize(dim,dim);
    dshapedxt.SetSize(dof,dim);
    bf.SetSize(dim);
    bfdshapedxt.SetSize(dof);
    double w;

    const IntegrationRule *ir = IntRule;
    if (ir == NULL)
    {
        //       ir = &IntRules.Get(el.GetGeomType(), oa * el.GetOrder() + ob
        //                          + Tr.OrderW());
        //      ir = &IntRules.Get(el.GetGeomType(), oa * el.GetOrder() + ob);
        // int order = 2 * el.GetOrder() ; // <--- OK for RTk
        int order = (Tr.OrderW() + el.GetOrder() + el.GetOrder());
        ir = &IntRules.Get(el.GetGeomType(), order);
    }

    for (int i = 0; i < ir->GetNPoints(); i++)
    {
        const IntegrationPoint &ip = ir->IntPoint(i);
        el.CalcDShape(ip, dshape);

        //double val = Tr.Weight() * Q.Eval(Tr, ip);

        Tr.SetIntPoint (&ip);
        w = ip.weight;// * Tr.Weight();
        CalcAdjugate(Tr.Jacobian(), invdfdx);
        Mult(dshape, invdfdx, dshapedxt);

        Q.Eval(bf, Tr, ip);

        dshapedxt.Mult(bf, bfdshapedxt);

        add(elvect, w, bfdshapedxt, elvect);
    }
}

/** Bilinear integrator for (curl u, v) for Nedelec and scalar finite element for v. If the trial and
    test spaces are switched, assembles the form (u, curl v). */
class VectorFECurlVQIntegrator: public BilinearFormIntegrator
{
private:
    VectorCoefficient *VQ;
#ifndef MFEM_THREAD_SAFE
    Vector shape;
    DenseMatrix curlshape;
    DenseMatrix curlshape_dFT;
    //old
    DenseMatrix curlshapeTrial;
    DenseMatrix vshapeTest;
    DenseMatrix curlshapeTrial_dFT;
#endif
    void Init(VectorCoefficient *vq)
    { VQ = vq; }
public:
    VectorFECurlVQIntegrator() { Init(NULL); }
    VectorFECurlVQIntegrator(VectorCoefficient &vq) { Init(&vq); }
    VectorFECurlVQIntegrator(VectorCoefficient *vq) { Init(vq); }
    virtual void AssembleElementMatrix(const FiniteElement &el,
                                       ElementTransformation &Trans,
                                       DenseMatrix &elmat) { }
    virtual void AssembleElementMatrix2(const FiniteElement &trial_fe,
                                        const FiniteElement &test_fe,
                                        ElementTransformation &Trans,
                                        DenseMatrix &elmat);
};

void VectorFECurlVQIntegrator::AssembleElementMatrix2(
        const FiniteElement &trial_fe, const FiniteElement &test_fe,
        ElementTransformation &Trans, DenseMatrix &elmat)
{
    int trial_nd = trial_fe.GetDof(), test_nd = test_fe.GetDof(), i;
    //int dim = trial_fe.GetDim();
    //int dimc = (dim == 3) ? 3 : 1;
    int dim;
    int vector_dof, scalar_dof;

    MFEM_ASSERT(trial_fe.GetMapType() == mfem::FiniteElement::H_CURL ||
                test_fe.GetMapType() == mfem::FiniteElement::H_CURL,
                "At least one of the finite elements must be in H(Curl)");

    //int curl_nd;
    int vec_nd;
    if ( trial_fe.GetMapType() == mfem::FiniteElement::H_CURL )
    {
        //curl_nd = trial_nd;
        vector_dof = trial_fe.GetDof();
        vec_nd  = test_nd;
        scalar_dof = test_fe.GetDof();
        dim = trial_fe.GetDim();
    }
    else
    {
        //curl_nd = test_nd;
        vector_dof = test_fe.GetDof();
        vec_nd  = trial_nd;
        scalar_dof = trial_fe.GetDof();
        dim = test_fe.GetDim();
    }

    MFEM_ASSERT(dim == 3, "VectorFECurlVQIntegrator is working only in 3D currently \n");

#ifdef MFEM_THREAD_SAFE
    DenseMatrix curlshapeTrial(curl_nd, dimc);
    DenseMatrix curlshapeTrial_dFT(curl_nd, dimc);
    DenseMatrix vshapeTest(vec_nd, dimc);
#else
    //curlshapeTrial.SetSize(curl_nd, dimc);
    //curlshapeTrial_dFT.SetSize(curl_nd, dimc);
    //vshapeTest.SetSize(vec_nd, dimc);
#endif
    //Vector shapeTest(vshapeTest.GetData(), vec_nd);

    curlshape.SetSize(vector_dof, dim);
    curlshape_dFT.SetSize(vector_dof, dim);
    shape.SetSize(scalar_dof);
    Vector D(vec_nd);

    elmat.SetSize(test_nd, trial_nd);

    const IntegrationRule *ir = IntRule;
    if (ir == NULL)
    {
        int order = trial_fe.GetOrder() + test_fe.GetOrder() - 1; // <--
        ir = &IntRules.Get(trial_fe.GetGeomType(), order);
    }

    elmat = 0.0;
    for (i = 0; i < ir->GetNPoints(); i++)
    {
        const IntegrationPoint &ip = ir->IntPoint(i);

        Trans.SetIntPoint(&ip);

        double w = ip.weight;
        VQ->Eval(D, Trans, ip);
        D *= w;

        if (dim == 3)
        {
            if ( trial_fe.GetMapType() == mfem::FiniteElement::H_CURL )
            {
                trial_fe.CalcCurlShape(ip, curlshape);
                test_fe.CalcShape(ip, shape);
            }
            else
            {
                test_fe.CalcCurlShape(ip, curlshape);
                trial_fe.CalcShape(ip, shape);
            }
            MultABt(curlshape, Trans.Jacobian(), curlshape_dFT);

            ///////////////////////////
            for (int d = 0; d < dim; d++)
            {
                for (int j = 0; j < scalar_dof; j++)
                {
                    for (int k = 0; k < vector_dof; k++)
                    {
                        elmat(j, k) += D[d] * shape(j) * curlshape_dFT(k, d);
                    }
                }
            }
            ///////////////////////////
        }
    }
}

double uFun1_ex(const Vector& x); // Exact Solution
double uFun1_ex_dt(const Vector& xt);
void uFun1_ex_gradx(const Vector& xt, Vector& grad);

void bFun_ex (const Vector& xt, Vector& b);
double  bFundiv_ex(const Vector& xt);

void bFunRect2D_ex(const Vector& xt, Vector& b );
double  bFunRect2Ddiv_ex(const Vector& xt);

void bFunCube3D_ex(const Vector& xt, Vector& b );
double  bFunCube3Ddiv_ex(const Vector& xt);

void bFunSphere3D_ex(const Vector& xt, Vector& b );
double  bFunSphere3Ddiv_ex(const Vector& xt);

void bFunCircle2D_ex (const Vector& xt, Vector& b);
double  bFunCircle2Ddiv_ex(const Vector& xt);

double uFun3_ex(const Vector& x); // Exact Solution
double uFun3_ex_dt(const Vector& xt);
void uFun3_ex_gradx(const Vector& xt, Vector& grad);

double uFun4_ex(const Vector& x); // Exact Solution
double uFun4_ex_dt(const Vector& xt);
void uFun4_ex_gradx(const Vector& xt, Vector& grad);

double uFun5_ex(const Vector& x); // Exact Solution
double uFun5_ex_dt(const Vector& xt);
void uFun5_ex_gradx(const Vector& xt, Vector& grad);

double uFun6_ex(const Vector& x); // Exact Solution
double uFun6_ex_dt(const Vector& xt);
void uFun6_ex_gradx(const Vector& xt, Vector& grad);

double uFunCylinder_ex(const Vector& x); // Exact Solution
double uFunCylinder_ex_dt(const Vector& xt);
void uFunCylinder_ex_gradx(const Vector& xt, Vector& grad);

double uFun66_ex(const Vector& x); // Exact Solution
double uFun66_ex_dt(const Vector& xt);
void uFun66_ex_gradx(const Vector& xt, Vector& grad);


double uFun2_ex(const Vector& x); // Exact Solution
double uFun2_ex_dt(const Vector& xt);
void uFun2_ex_gradx(const Vector& xt, Vector& grad);

double uFun33_ex(const Vector& x); // Exact Solution
double uFun33_ex_dt(const Vector& xt);
void uFun33_ex_gradx(const Vector& xt, Vector& grad);

void hcurlFun3D_ex(const Vector& xt, Vector& vecvalue);
void curlhcurlFun3D_ex(const Vector& xt, Vector& vecvalue);
void hcurlFun3D_2_ex(const Vector& xt, Vector& vecvalue);
void curlhcurlFun3D_2_ex(const Vector& xt, Vector& vecvalue);

void DivmatFun4D_ex(const Vector& xt, Vector& vecvalue);
void DivmatDivmatFun4D_ex(const Vector& xt, Vector& vecvalue);

double zero_ex(const Vector& xt);
void zerovec_ex(const Vector& xt, Vector& vecvalue);
void zerovecx_ex(const Vector& xt, Vector& zerovecx );
void zerovecMat4D_ex(const Vector& xt, Vector& vecvalue);

void vminusone_exact(const Vector &x, Vector &vminusone);
void vone_exact(const Vector &x, Vector &vone);

// Exact solution, E, and r.h.s., f. See below for implementation.
void E_exact(const Vector &, Vector &);
void curlE_exact(const Vector &x, Vector &curlE);
void f_exact(const Vector &, Vector &);
double freq = 1.0, kappa;

// 4d test from Martin's example
void E_exactMat_vec(const Vector &x, Vector &E);
void E_exactMat(const Vector &, DenseMatrix &);
void f_exactMat(const Vector &, DenseMatrix &);


template <double (*S)(const Vector&), void (*bvecfunc)(const Vector&, Vector& )>
void sigmaTemplate(const Vector& xt, Vector& sigma);
template <void (*bvecfunc)(const Vector&, Vector& )>
void KtildaTemplate(const Vector& xt, DenseMatrix& Ktilda);
template <void (*bvecfunc)(const Vector&, Vector& )>
void bbTTemplate(const Vector& xt, DenseMatrix& bbT);
template <void (*bvecfunc)(const Vector&, Vector& )>
double bTbTemplate(const Vector& xt);
template <double (*S)(const Vector&), void (*bvecfunc)(const Vector&, Vector& ), void (*opdivfreevec)(const Vector&, Vector& )>
void minKsigmahatTemplate(const Vector& xt, Vector& minKsigmahatv);
template<double (*S)(const Vector & xt), void(*bvec)(const Vector & x, Vector & vec), void (*opdivfreevec)(const Vector&, Vector& )>
double bsigmahatTemplate(const Vector& xt);
template<double (*S)(const Vector & xt), void (*bvecfunc)(const Vector&, Vector& ),
         void (*opdivfreevec)(const Vector&, Vector& )>
void sigmahatTemplate(const Vector& xt, Vector& sigmahatv);
template<double (*S)(const Vector & xt), void (*bvecfunc)(const Vector&, Vector& ),
         void (*opdivfreevec)(const Vector&, Vector& )>
void minsigmahatTemplate(const Vector& xt, Vector& sigmahatv);
template<double (*S)(const Vector & xt), double (*dSdt)(const Vector & xt), void(*Sgradxvec)(const Vector & x, Vector & gradx), \
         void(*bvec)(const Vector & x, Vector & vec), double (*divbfunc)(const Vector & xt) > \
double rhsideTemplate(const Vector& xt);

template<double (*S)(const Vector & xt), double (*dSdt)(const Vector & xt), void(*Sgradxvec)(const Vector & x, Vector & gradx), \
         void(*bvec)(const Vector & x, Vector & vec), double (*divbfunc)(const Vector & xt)> \
void bfTemplate(const Vector& xt, Vector& bf);

template<double (*S)(const Vector & xt), double (*dSdt)(const Vector & xt), void(*Sgradxvec)(const Vector & x, Vector & gradx), \
         void(*bvec)(const Vector & x, Vector & vec), double (*divbfunc)(const Vector & xt)> \
void bdivsigmaTemplate(const Vector& xt, Vector& bf);

template<void(*bvec)(const Vector & x, Vector & vec)>
void minbTemplate(const Vector& xt, Vector& minb);

template<double (*S)(const Vector & xt), double (*dSdt)(const Vector & xt), void(*Sgradxvec)(const Vector & x, Vector & gradx), \
         void(*bvec)(const Vector & x, Vector & vec), double (*divbfunc)(const Vector & xt) > \
double divsigmaTemplate(const Vector& xt);

template<double (*S)(const Vector & xt) > double SnonhomoTemplate(const Vector& xt);

class Transport_test_divfree
{
protected:
    int dim;
    int numsol;
    int numcurl;

public:
    FunctionCoefficient * scalarS;
    FunctionCoefficient * scalardivsigma;         // = dS/dt + div (bS) = div sigma
    FunctionCoefficient * bTb;                    // b^T * b
    FunctionCoefficient * bsigmahat;              // b * sigma_hat
    VectorFunctionCoefficient * sigma;
    VectorFunctionCoefficient * sigmahat;         // sigma_hat = sigma_exact - op divfreepart (curl hcurlpart in 3D)
    VectorFunctionCoefficient * b;
    VectorFunctionCoefficient * minb;
    VectorFunctionCoefficient * bf;
    VectorFunctionCoefficient * bdivsigma;        // b * div sigma = b * initial f (before modifying it due to inhomogenuity)
    MatrixFunctionCoefficient * Ktilda;
    MatrixFunctionCoefficient * bbT;
    VectorFunctionCoefficient * divfreepart;        // additional part added for testing div-free solver
    VectorFunctionCoefficient * opdivfreepart;    // curl of the additional part which is added to sigma_exact for testing div-free solver
    VectorFunctionCoefficient * minKsigmahat;     // - K * sigma_hat
    VectorFunctionCoefficient * minsigmahat;      // -sigma_hat
public:
    Transport_test_divfree (int Dim, int NumSol, int NumCurl);

    int GetDim() {return dim;}
    int GetNumSol() {return numsol;}
    int GetNumCurl() {return numcurl;}
    void SetDim(int Dim) { dim = Dim;}
    void SetNumSol(int NumSol) { numsol = NumSol;}
    void SetNumCurl(int NumCurl) { numcurl = NumCurl;}
    bool CheckTestConfig();

    ~Transport_test_divfree () {}
private:
    template<double (*S)(const Vector & xt), double (*dSdt)(const Vector & xt), void(*Sgradxvec)(const Vector & x, Vector & gradx), \
             void(*bvec)(const Vector & x, Vector & vec), double (*divbvec)(const Vector & xt), \
             void(*hcurlvec)(const Vector & x, Vector & vec), void(*opdivfreevec)(const Vector & x, Vector & vec)> \
    void SetTestCoeffs ( );

    void SetScalarSFun( double (*S)(const Vector & xt))
    { scalarS = new FunctionCoefficient(S);}

    template<void(*bvec)(const Vector & x, Vector & vec)>  \
    void SetScalarBtB()
    {
        bTb = new FunctionCoefficient(bTbTemplate<bvec>);
    }

    template<double (*S)(const Vector & xt), void(*bvec)(const Vector & x, Vector & vec)> \
    void SetSigmaVec()
    {
        sigma = new VectorFunctionCoefficient(dim, sigmaTemplate<S,bvec>);
    }

    template<void(*bvec)(const Vector & x, Vector & vec)> \
    void SetminbVec()
    { minb = new VectorFunctionCoefficient(dim, minbTemplate<bvec>);}

    void SetbVec( void(*bvec)(const Vector & x, Vector & vec) )
    { b = new VectorFunctionCoefficient(dim, bvec);}

    template< void(*bvec)(const Vector & x, Vector & vec)>  \
    void SetKtildaMat()
    {
        Ktilda = new MatrixFunctionCoefficient(dim, KtildaTemplate<bvec>);
    }

    template< void(*bvec)(const Vector & x, Vector & vec)>  \
    void SetBBtMat()
    {
        bbT = new MatrixFunctionCoefficient(dim, bbTTemplate<bvec>);
    }

    template<double (*S)(const Vector & xt), double (*dSdt)(const Vector & xt), void(*Sgradxvec)(const Vector & x, Vector & gradx), \
             void(*bvec)(const Vector & x, Vector & vec), double (*divbfunc)(const Vector & xt)> \
    void SetdivSigma()
    { scalardivsigma = new FunctionCoefficient(divsigmaTemplate<S, dSdt, Sgradxvec, bvec, divbfunc>);}

    template<double (*S)(const Vector & xt), double (*dSdt)(const Vector & xt), void(*Sgradxvec)(const Vector & x, Vector & gradx), \
             void(*bvec)(const Vector & x, Vector & vec), double (*divbfunc)(const Vector & xt) > \
    void SetbfVec()
    { bf = new VectorFunctionCoefficient(dim, bfTemplate<S,dSdt,Sgradxvec,bvec,divbfunc>);}

    template<double (*S)(const Vector & xt), double (*dSdt)(const Vector & xt), void(*Sgradxvec)(const Vector & x, Vector & gradx), \
             void(*bvec)(const Vector & x, Vector & vec), double (*divbfunc)(const Vector & xt) > \
    void SetbdivsigmaVec()
    { bdivsigma = new VectorFunctionCoefficient(dim, bdivsigmaTemplate<S,dSdt,Sgradxvec,bvec,divbfunc>);}

    void SetDivfreePart( void(*divfreevec)(const Vector & x, Vector & vec))
    { divfreepart = new VectorFunctionCoefficient(dim, divfreevec);}

    void SetOpDivfreePart( void(*opdivfreevec)(const Vector & x, Vector & vec))
    { opdivfreepart = new VectorFunctionCoefficient(dim, opdivfreevec);}

    template<double (*S)(const Vector & xt), void(*bvec)(const Vector & x, Vector & vec), void(*opdivfreevec)(const Vector & x, Vector & vec)> \
    void SetminKsigmahat()
    { minKsigmahat = new VectorFunctionCoefficient(dim, minKsigmahatTemplate<S, bvec, opdivfreevec>);}

    template<double (*S)(const Vector & xt), void(*bvec)(const Vector & x, Vector & vec), void(*opdivfreevec)(const Vector & x, Vector & vec)> \
    void Setbsigmahat()
    { bsigmahat = new FunctionCoefficient(bsigmahatTemplate<S, bvec, opdivfreevec>);}

    template<double (*S)(const Vector & xt), void (*bvec)(const Vector&, Vector& ),
             void(*opdivfreevec)(const Vector & x, Vector & vec)> \
    void Setsigmahat()
    { sigmahat = new VectorFunctionCoefficient(dim, sigmahatTemplate<S, bvec, opdivfreevec>);}

    template<double (*S)(const Vector & xt), void (*bvec)(const Vector&, Vector& ),
             void(*opdivfreevec)(const Vector & x, Vector & vec)> \
    void Setminsigmahat()
    { minsigmahat = new VectorFunctionCoefficient(dim, minsigmahatTemplate<S, bvec, opdivfreevec>);}

};

template<double (*S)(const Vector & xt), double (*dSdt)(const Vector & xt), void(*Sgradxvec)(const Vector & x, Vector & gradx),  \
         void(*bvec)(const Vector & x, Vector & vec), double (*divbfunc)(const Vector & xt), \
         void(*divfreevec)(const Vector & x, Vector & vec), void(*opdivfreevec)(const Vector & x, Vector & vec)> \
void Transport_test_divfree::SetTestCoeffs ()
{
    SetScalarSFun(S);
    SetminbVec<bvec>();
    SetbVec(bvec);
    SetbfVec<S, dSdt, Sgradxvec, bvec, divbfunc>();
    SetbdivsigmaVec<S, dSdt, Sgradxvec, bvec, divbfunc>();
    SetSigmaVec<S,bvec>();
    SetKtildaMat<bvec>();
    SetScalarBtB<bvec>();
    SetdivSigma<S, dSdt, Sgradxvec, bvec, divbfunc>();
    SetDivfreePart(divfreevec);
    SetOpDivfreePart(opdivfreevec);
    SetminKsigmahat<S, bvec, opdivfreevec>();
    Setbsigmahat<S, bvec, opdivfreevec>();
    Setsigmahat<S, bvec, opdivfreevec>();
    Setminsigmahat<S, bvec, opdivfreevec>();
    SetBBtMat<bvec>();
    return;
}


bool Transport_test_divfree::CheckTestConfig()
{
    if (dim == 4 || dim == 3)
    {
        if ( numsol == 0 && dim >= 3 )
            return true;
        if ( numsol == 1 && dim == 3 )
            return true;
        if ( numsol == 2 && dim == 3 )
            return true;
        if ( numsol == 4 && dim == 3 )
            return true;
        if ( numsol == -3 && dim == 3 )
            return true;
        if ( numsol == -4 && dim == 4 )
            return true;
        return false;
    }
    else
        return false;

}

Transport_test_divfree::Transport_test_divfree (int Dim, int NumSol, int NumCurl)
{
    dim = Dim;
    numsol = NumSol;
    numcurl = NumCurl;

    if ( CheckTestConfig() == false )
        std::cout << "Inconsistent dim = " << dim << " and numsol = " << numsol <<  std::endl << std::flush;
    else
    {
        if (numsol == 0)
        {
            if (dim == 3)
            {
                if (numcurl == 1)
                    SetTestCoeffs<&zero_ex, &zero_ex, &zerovecx_ex, &bFunRect2D_ex, &bFunRect2Ddiv_ex, &hcurlFun3D_ex, &curlhcurlFun3D_ex>();
                else if (numcurl == 2)
                    SetTestCoeffs<&zero_ex, &zero_ex, &zerovecx_ex, &bFunRect2D_ex, &bFunRect2Ddiv_ex, &hcurlFun3D_2_ex, &curlhcurlFun3D_2_ex>();
                else
                    SetTestCoeffs<&zero_ex, &zero_ex, &zerovecx_ex, &bFunRect2D_ex, &bFunRect2Ddiv_ex, &zerovec_ex, &zerovec_ex>();
            }
            if (dim > 3)
            {
                if (numcurl == 1)
                    SetTestCoeffs<&zero_ex, &zero_ex, &zerovecx_ex, &bFunCube3D_ex, &bFunCube3Ddiv_ex, &DivmatFun4D_ex, &DivmatDivmatFun4D_ex>();
                else
                    SetTestCoeffs<&zero_ex, &zero_ex, &zerovecx_ex, &bFunCube3D_ex, &bFunCube3Ddiv_ex, &zerovec_ex, &zerovec_ex>();
            }
        }
        if (numsol == -3)
        {
            if (numcurl == 1)
                SetTestCoeffs<&uFunTest_ex, &uFunTest_ex_dt, &uFunTest_ex_gradx, &bFunRect2D_ex, &bFunRect2Ddiv_ex, &hcurlFun3D_ex, &curlhcurlFun3D_ex>();
            else if (numcurl == 2)
                SetTestCoeffs<&uFunTest_ex, &uFunTest_ex_dt, &uFunTest_ex_gradx, &bFunRect2D_ex, &bFunRect2Ddiv_ex, &hcurlFun3D_2_ex, &curlhcurlFun3D_2_ex>();
            else
                SetTestCoeffs<&uFunTest_ex, &uFunTest_ex_dt, &uFunTest_ex_gradx, &bFunRect2D_ex, &bFunRect2Ddiv_ex, &zerovec_ex, &zerovec_ex>();
        }
        if (numsol == -4)
        {
            //if (numcurl == 1) // actually wrong div-free guy in 4D but it is not used when withDiv = true
                //SetTestCoeffs<&uFunTest_ex, &uFunTest_ex_dt, &uFunTest_ex_gradx, &bFunCube3D_ex, &bFunCube3Ddiv_ex, &hcurlFun3D_ex, &curlhcurlFun3D_ex>();
            //else if (numcurl == 2)
                //SetTestCoeffs<&uFunTest_ex, &uFunTest_ex_dt, &uFunTest_ex_gradx, &bFunCube3D_ex, &bFunCube3Ddiv_ex, &hcurlFun3D_2_ex, &curlhcurlFun3D_2_ex>();
            if (numcurl == 1 || numcurl == 2)
            {
                std::cout << "Critical error: Explicit analytic div-free guy is not implemented in 4D \n";
            }
            else
                SetTestCoeffs<&uFunTest_ex, &uFunTest_ex_dt, &uFunTest_ex_gradx, &bFunCube3D_ex, &bFunCube3Ddiv_ex, &zerovecMat4D_ex, &zerovec_ex>();
        }
        if (numsol == 1)
        {
            if (numcurl == 1)
                SetTestCoeffs<&uFun1_ex, &uFun1_ex_dt, &uFun1_ex_gradx, &bFunRect2D_ex, &bFunRect2Ddiv_ex, &hcurlFun3D_ex, &curlhcurlFun3D_ex>();
            else if (numcurl == 2)
                SetTestCoeffs<&uFun1_ex, &uFun1_ex_dt, &uFun1_ex_gradx, &bFunRect2D_ex, &bFunRect2Ddiv_ex, &hcurlFun3D_2_ex, &curlhcurlFun3D_2_ex>();
            else
                SetTestCoeffs<&uFun1_ex, &uFun1_ex_dt, &uFun1_ex_gradx, &bFunRect2D_ex, &bFunRect2Ddiv_ex, &zerovec_ex, &zerovec_ex>();
        }
        if (numsol == 2)
        {
            //std::cout << "The domain must be a cylinder over a square" << std::endl << std::flush;
            if (numcurl == 1)
                SetTestCoeffs<&uFun2_ex, &uFun2_ex_dt, &uFun2_ex_gradx, &bFunRect2D_ex, &bFunRect2Ddiv_ex, &hcurlFun3D_ex, &curlhcurlFun3D_ex>();
            else if (numcurl == 2)
                SetTestCoeffs<&uFun2_ex, &uFun2_ex_dt, &uFun2_ex_gradx, &bFunRect2D_ex, &bFunRect2Ddiv_ex, &hcurlFun3D_2_ex, &curlhcurlFun3D_2_ex>();
            else
                SetTestCoeffs<&uFun2_ex, &uFun2_ex_dt, &uFun2_ex_gradx, &bFunRect2D_ex, &bFunRect2Ddiv_ex, &zerovec_ex, &zerovec_ex>();
        }
        if (numsol == 4)
        {
            //std::cout << "The domain must be a cylinder over a square" << std::endl << std::flush;
            if (numcurl == 1)
                SetTestCoeffs<&uFun4_ex, &uFun4_ex_dt, &uFun4_ex_gradx, &bFunRect2D_ex, &bFunRect2Ddiv_ex, &hcurlFun3D_ex, &curlhcurlFun3D_ex>();
            else if (numcurl == 2)
                SetTestCoeffs<&uFun4_ex, &uFun4_ex_dt, &uFun4_ex_gradx, &bFunRect2D_ex, &bFunRect2Ddiv_ex, &hcurlFun3D_2_ex, &curlhcurlFun3D_2_ex>();
            else
                SetTestCoeffs<&uFun4_ex, &uFun4_ex_dt, &uFun4_ex_gradx, &bFunRect2D_ex, &bFunRect2Ddiv_ex, &zerovec_ex, &zerovec_ex>();
        }
    } // end of setting test coefficients in correct case
}

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

    int ser_ref_levels  = 1;
    int par_ref_levels  = 1;

    const char *space_for_S = "L2";    // "H1" or "L2"
    bool eliminateS = true;            // in case space_for_S = "L2" defines whether we eliminate S from the system

    bool aniso_refine = false;
    bool refine_t_first = false;

    bool withDiv = true;
    bool with_multilevel = true;
    bool monolithicMG = false;

    bool useM_in_divpart = true;

    // solver options
    int prec_option = 2;        // defines whether to use preconditioner or not, and which one
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

    kappa = freq * M_PI;

    if (verbose)
        cout << "Solving CFOSLS Transport equation with MFEM & hypre, div-free approach \n";

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
    args.AddOption(&space_for_S, "-sspace", "--sspace",
                   "Space for S (H1 or L2).");
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

    Transport_test_divfree Mytest(nDimensions, numsol, numcurl);

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

    //DEFAULTED LINEAR SOLVER OPTIONS
    int max_num_iter = 150000;
    double rtol = 1e-12;//1e-7;//1e-9;
    double atol = 1e-12;//1e-9;//1e-12;

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

    /*
    // testing a bug
    FiniteElementCollection * hdivfree_coll = new ND_FECollection(feorder + 1, nDimensions);

    ParFiniteElementSpace * C_space = new ParFiniteElementSpace(pmesh.get(), hdivfree_coll);
    //ParFiniteElementSpace * coarseC_space = new ParFiniteElementSpace(pmesh.get(), hdivfree_coll);

    //coarseC_space->Update();

    pmesh->UniformRefinement();

    C_space->Update();

    //C_space->Dof_TrueDof_Matrix();
    //coarseC_space->Dof_TrueDof_Matrix();
    //coarseC_space->Lose_Dof_TrueDof_Matrix();
    //HypreParMatrix * mat11 = new HypreParMatrix(C_space->Dof_TrueDof_Matrix()->StealData());
    HypreParMatrix * mat1 = C_space->Dof_TrueDof_Matrix();
    //mat1->SetOwnerFlags(1,1,1);
    //C_space->Dof_TrueDof_Matrix()->SetOwnerFlags(1,1,1);
    //C_space->Lose_Dof_TrueDof_Matrix();
    //mat1 = new HypreParMatrix(C_space->Dof_TrueDof_Matrix()->StealData());
    //HypreParMatrix * mat1 = C_space->Dof_TrueDof_Matrix();
    //HypreParMatrix * mat12 = new HypreParMatrix(coarseC_space->Dof_TrueDof_Matrix()->StealData());

    //coarseC_space->Update();

    pmesh->UniformRefinement();

    C_space->Update();

    HypreParMatrix * mat2 = C_space->Dof_TrueDof_Matrix();
    C_space->Lose_Dof_TrueDof_Matrix();
    //HypreParMatrix * mat22 = coarseC_space->Dof_TrueDof_Matrix();


    MPI_Finalize();
    return 0;
    }
    */

    MFEM_ASSERT(!(aniso_refine && (with_multilevel || nDimensions == 4)),"Anisotropic refinement works only in 3D and without multilevel algorithm \n");

    int dim = nDimensions;

    Array<int> ess_bdrSigma(pmesh->bdr_attributes.Max());
    ess_bdrSigma = 0;
    if (strcmp(space_for_S,"L2") == 0) // S is from L2, so we impose bdr condition for sigma at t = 0
    {
        ess_bdrSigma[0] = 1;
        //ess_bdrSigma = 1;
        //ess_bdrSigma[pmesh->bdr_attributes.Max()-1] = 0;
    }

    int ref_levels = par_ref_levels;

#ifdef NEW_STUFF
    int num_levels = ref_levels + 1;
    Array<ParMesh*> pmesh_lvls(num_levels);
    Array<ParFiniteElementSpace*> R_space_lvls(num_levels);
    Array<ParFiniteElementSpace*> W_space_lvls(num_levels);
    Array<ParFiniteElementSpace*> C_space_lvls(num_levels);
    //Array<ParFiniteElementSpace*> H_space_lvls(num_levels);
#endif
    FiniteElementCollection *hdiv_coll;
    ParFiniteElementSpace *R_space;
    FiniteElementCollection *l2_coll;
    ParFiniteElementSpace *W_space;

    if (dim == 4)
        hdiv_coll = new RT0_4DFECollection;
    else
        hdiv_coll = new RT_FECollection(feorder, dim);

    R_space = new ParFiniteElementSpace(pmesh.get(), hdiv_coll);

    if (withDiv)
    {
        l2_coll = new L2_FECollection(feorder, nDimensions);
        W_space = new ParFiniteElementSpace(pmesh.get(), l2_coll);
    }

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
    Array<HypreParMatrix*> P_C(par_ref_levels);
    Array<HypreParMatrix*> P_H(par_ref_levels);

    Array<HypreParMatrix*> TrueP_R(par_ref_levels);

    Array< SparseMatrix*> P_W(ref_levels);
    Array< SparseMatrix*> P_R(ref_levels);
    Array< SparseMatrix*> Element_dofs_R(ref_levels);
    Array< SparseMatrix*> Element_dofs_W(ref_levels);

    const SparseMatrix* P_W_local;
    const SparseMatrix* P_R_local;

    DivPart divp;

#ifdef NEW_STUFF
    Array<int> all_bdrSigma(pmesh->bdr_attributes.Max());
    all_bdrSigma = 1;
    std::vector<std::vector<Array<int>* > > BdrDofs_R(1, std::vector<Array<int>* >(num_levels));
    std::vector<std::vector<Array<int>* > > EssBdrDofs_R(1, std::vector<Array<int>* >(num_levels));

    //std::cout << "num_levels - 1 = " << num_levels << "\n";
    std::vector<Array<int>* > EssBdrDofs_Hcurl(num_levels);

    Array< SparseMatrix* > Proj_Hcurl(num_levels - 1);
    Array<HypreParMatrix* > Dof_TrueDof_Hcurl(num_levels);

    std::vector<std::vector<HypreParMatrix*> > Dof_TrueDof_Func_lvls(num_levels);
    std::vector<HypreParMatrix*> Dof_TrueDof_L2_lvls(num_levels);

    Array<SparseMatrix*> Divfree_mat_lvls(num_levels);
    std::vector<Array<int>*> Funct_mat_offsets_lvls(num_levels);
    Array<BlockMatrix*> Funct_mat_lvls(num_levels);
    Array<SparseMatrix*> Constraint_mat_lvls(num_levels);

   for (int l = 0; l < num_levels; ++l)
   {
       Dof_TrueDof_Func_lvls[l].resize(1);
       BdrDofs_R[0][l] = new Array<int>;
       EssBdrDofs_R[0][l] = new Array<int>;
       EssBdrDofs_Hcurl[l] = new Array<int>;
       Funct_mat_offsets_lvls[l] = new Array<int>;
   }

   const SparseMatrix* Proj_Hcurl_local;
#endif

    /*
     * I will just leave it here in case I will revisit the issue with HypreParMatrix copying
    //ParMesh * pmesh_copy = new ParMesh(*(pmesh.get()));
    ParMesh * pmesh_copy;
    {
        ifstream imesh(mesh_file);
        mesh = new Mesh(imesh, 1, 1);
        imesh.close();

        for (int l = 0; l < ser_ref_levels; l++)
            mesh->UniformRefinement();
        pmesh_copy = new ParMesh(comm, *mesh);
    }

    ParFiniteElementSpace * testW_space = new ParFiniteElementSpace(pmesh_copy, hdivfree_coll);
    */
    //ParMesh * pmesh_copy = pmesh.get();
    //ParFiniteElementSpace * testW_space = C_space;
    /*
    // with vector of pointers for W space
    const SparseMatrix * test_local;
    ParFiniteElementSpace * testW_space = new ParFiniteElementSpace(pmesh.get(), hdivfree_coll);
    Array< HypreParMatrix* > goals(2);
    for ( int l = 0; l < 2; l++)
    {
        // refine
        pmesh->UniformRefinement();

        //update fe space
        testW_space->Update();

        test_local = (SparseMatrix *)testW_space->GetUpdateOperator();

        // get d_td
        HypreParMatrix * testmat = testW_space->Dof_TrueDof_Matrix();
        goals[1 - l] = testmat;
        testmat = NULL;

        SparseMatrix * test_local2 = RemoveZeroEntries(*test_local);
    }

    MPI_Finalize();
    return 0;
    */

    chrono.Clear();
    chrono.Start();

    //const int finest_level = 0;
    //const int coarsest_level = num_levels - 1;

    if (verbose)
        std::cout << "Creating a hierarchy of meshes by successive refinements "
                     "(with multilevel and multigrid prerequisites) \n";

    if (!withDiv && verbose)
        std::cout << "Multilevel code cannot be used without withDiv flag \n";

    for (int l = num_levels - 1; l >=0; --l)
    {
        // creating pmesh for level l
        if (l == num_levels - 1)
        {
            pmesh_lvls[l] = pmesh.get();
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

        // getting boundary and essential boundary dofs
        R_space_lvls[l]->GetEssentialVDofs(all_bdrSigma, *BdrDofs_R[0][l]);
        R_space_lvls[l]->GetEssentialVDofs(ess_bdrSigma, *EssBdrDofs_R[0][l]);
        C_space_lvls[l]->GetEssentialVDofs(ess_bdrSigma, *EssBdrDofs_Hcurl[l]);

        // getting operators at level l
        // curl or divskew operator from C_space into R_space
        ParDiscreteLinearOperator Divfree_op(C_space_lvls[l], R_space_lvls[l]); // from Hcurl or HDivSkew(C_space) to Hdiv(R_space)
        if (dim == 3)
            Divfree_op.AddDomainInterpolator(new CurlInterpolator());
        else // dim == 4
            Divfree_op.AddDomainInterpolator(new DivSkewInterpolator());
        Divfree_op.Assemble();
        Divfree_op.Finalize();
        Divfree_mat_lvls[l] = Divfree_op.LoseMat();

        ParBilinearForm *Ablock(new ParBilinearForm(R_space_lvls[l]));
        //Ablock->AddDomainIntegrator(new VectorFEMassIntegrator);
        Ablock->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.Ktilda));
        Ablock->Assemble();
        //Ablock->EliminateEssentialBC(ess_bdrSigma, *sigma_exact_finest, *fform); // makes res for sigma_special happier
        Ablock->Finalize();

        Funct_mat_offsets_lvls[l]->SetSize(2);
        //SparseMatrix Aloc = Ablock->SpMat();
        //Array<int> offsets(2);
        (*Funct_mat_offsets_lvls[l])[0] = 0;
        (*Funct_mat_offsets_lvls[l])[1] = Ablock->Height();

        Funct_mat_lvls[l] = new BlockMatrix(*Funct_mat_offsets_lvls[l]);
        Funct_mat_lvls[l]->SetBlock(0,0,Ablock->LoseMat());

        ParMixedBilinearForm *Bblock(new ParMixedBilinearForm(R_space_lvls[l], W_space_lvls[l]));
        Bblock->AddDomainIntegrator(new VectorFEDivergenceIntegrator);
        Bblock->Assemble();
        //Bblock->EliminateTrialDofs(ess_bdrSigma, *sigma_exact_finest, *constrfform); // // makes res for sigma_special happier
        Bblock->Finalize();
        Constraint_mat_lvls[l] = Bblock->LoseMat();

        // getting pointers to dof_truedof matrices
        Dof_TrueDof_Hcurl[l] = C_space_lvls[l]->Dof_TrueDof_Matrix();
        Dof_TrueDof_Func_lvls[l][0] = R_space_lvls[l]->Dof_TrueDof_Matrix();
        Dof_TrueDof_L2_lvls[l] = W_space_lvls[l]->Dof_TrueDof_Matrix();

        // for all but one levels we create projection matrices between levels
        // and projectors assembled on true dofs if MG preconditioner is used
        if (l < num_levels - 1)
        {
            C_space->Update();
            Proj_Hcurl_local = (SparseMatrix *)C_space->GetUpdateOperator();
            Proj_Hcurl[l] = RemoveZeroEntries(*Proj_Hcurl_local);

            W_space->Update();
            R_space->Update();

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
            HypreParMatrix * P_R_coarser_d_td = Dof_TrueDof_Func_lvls[l + 1][0]->
                    LeftDiagMult( *P_R[l],  R_space_lvls[l]->GetDofOffsets());
            TrueP_R[num_levels - 2 - l] = ParMult(Dof_TrueDof_Func_lvls[l][0]->Transpose(), P_R_coarser_d_td);
            TrueP_R[num_levels - 2 - l]->CopyColStarts();
            TrueP_R[num_levels - 2 - l]->CopyRowStarts();

            if (prec_is_MG)
            {
                HypreParMatrix * P_C_coarser_d_td = Dof_TrueDof_Hcurl[l + 1]->
                        LeftDiagMult( *Proj_Hcurl[l],  C_space_lvls[l]->GetDofOffsets());
                P_C[num_levels - 2 - l] = ParMult(Dof_TrueDof_Hcurl[l]->Transpose(), P_C_coarser_d_td);
                P_C[num_levels - 2 - l]->CopyColStarts();
                P_C[num_levels - 2 - l]->CopyRowStarts();
            }
        }

    }

    if (verbose)
        std::cout << "End of the creating a hierarchy of meshes AND pfespaces \n";

#ifdef NEW_STUFF
    ParGridFunction * sigma_exact_finest;
    sigma_exact_finest = new ParGridFunction(R_space_lvls[0]);
    sigma_exact_finest->ProjectCoefficient(*Mytest.sigma);
#endif
    //if(dim==3) pmesh->ReorientTetMesh();

    pmesh->PrintInfo(std::cout); if(verbose) cout << "\n";

#ifdef OLD_CODE
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
        if (withDiv)
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
    //VectorFunctionCoefficient E(dim, E_exact);
    //VectorFunctionCoefficient curlE(dim, curlE_exact);

    //----------------------------------------------------------
    // Setting boundary conditions.
    //----------------------------------------------------------

    Array<int> ess_tdof_listU, ess_bdrU(pmesh->bdr_attributes.Max());
    ess_bdrU = 0;
    if (strcmp(space_for_S,"L2") == 0) // S is from L2, so we impose bdr cnds on sigma
        ess_bdrU[0] = 1;


    C_space->GetEssentialTrueDofs(ess_bdrU, ess_tdof_listU);

    Array<int> ess_tdof_listS, ess_bdrS(pmesh->bdr_attributes.Max());
    ess_bdrS = 0;
    if (strcmp(space_for_S,"H1") == 0) // S is from H1
    {
        ess_bdrS[0] = 1; // t = 0
        //ess_bdrS = 1;
        S_space->GetEssentialTrueDofs(ess_bdrS, ess_tdof_listS);
    }

    if (verbose)
    {
        std::cout << "Boundary conditions: \n";
        std::cout << "all bdr Sigma: \n";
        all_bdrSigma.Print(std::cout, pmesh->bdr_attributes.Max());
        std::cout << "ess bdr Sigma: \n";
        ess_bdrSigma.Print(std::cout, pmesh->bdr_attributes.Max());
        std::cout << "ess bdr U: \n";
        ess_bdrU.Print(std::cout, pmesh->bdr_attributes.Max());
        std::cout << "ess bdr S: \n";
        ess_bdrS.Print(std::cout, pmesh->bdr_attributes.Max());
    }

    chrono.Clear();
    chrono.Start();
    ParGridFunction * Sigmahat = new ParGridFunction(R_space);
    ParLinearForm *gform;
    HypreParMatrix *Bdiv;

    SparseMatrix *M_local;
    SparseMatrix *B_local;
    Vector F_fine(P_W[0]->Height());
    Vector G_fine(P_R[0]->Height());
    Vector sigmahat_pau;
    if (withDiv)
    {
        if (with_multilevel)
        {
            if (verbose)
                std::cout << "Using multilevel algorithm for finding a particular solution \n";

            ConstantCoefficient k(1.0);

            //SparseMatrix *M_local;
            if (useM_in_divpart)
            {
                ParBilinearForm *mVarf(new ParBilinearForm(R_space));
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
            B_local = &B_fine;

            //Right hand size

            gform = new ParLinearForm(W_space);
            gform->AddDomainIntegrator(new DomainLFIntegrator(*Mytest.scalardivsigma));
            gform->Assemble();

            F_fine = *gform;
            G_fine = .0;

            //std::cout << "Looking at B_local \n";
            //B_local->Print();

            divp.div_part(ref_levels,
                          M_local, B_local,
                          G_fine,
                          F_fine,
                          P_W, P_R, P_W,
                          Element_dofs_R,
                          Element_dofs_W,
                          Dof_TrueDof_Func_lvls[num_levels - 1][0],
                          Dof_TrueDof_L2_lvls[num_levels - 1],
                          sigmahat_pau,
                          *EssBdrDofs_R[0][num_levels - 1]);

    #ifdef MFEM_DEBUG
            Vector sth(F_fine.Size());
            B_fine.Mult(sigmahat_pau, sth);
            sth -= F_fine;
            std::cout << "sth.Norml2() = " << sth.Norml2() << "\n";
            MFEM_ASSERT(sth.Norml2()<1e-8, "The particular solution does not satisfy the divergence constraint");
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

            sigma_exact = new ParGridFunction(R_space);
            sigma_exact->ProjectCoefficient(*Mytest.sigma);

            gform = new ParLinearForm(W_space);
            gform->AddDomainIntegrator(new DomainLFIntegrator(*Mytest.scalardivsigma));
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
        }

    }
    else // solving a div-free system with some analytical solution for the div-free part
    {
        if (verbose)
            std::cout << "Using exact sigma minus curl of a given function from H(curl,0) (in 3D) as a particular solution \n";
        Sigmahat->ProjectCoefficient(*Mytest.sigmahat);
    }
    if (verbose)
        cout<<"Particular solution found in "<< chrono.RealTime() <<" seconds.\n";
    // in either way now Sigmahat is a function from H(div) s.t. div Sigmahat = div sigma = f

    // the div-free part
    ParGridFunction *u_exact = new ParGridFunction(C_space);
    u_exact->ProjectCoefficient(*Mytest.divfreepart);

    ParGridFunction *S_exact;
    S_exact = new ParGridFunction(S_space);
    S_exact->ProjectCoefficient(*Mytest.scalarS);

    ParGridFunction * sigma_exact = new ParGridFunction(R_space);
    sigma_exact->ProjectCoefficient(*Mytest.sigma);

    if (withDiv)
        xblks.GetBlock(0) = 0.0;
    else
        xblks.GetBlock(0) = *u_exact;

    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S from H1 or (S from L2 and no elimination)
        xblks.GetBlock(1) = *S_exact;

    ConstantCoefficient zero(.0);

#ifdef USE_CURLMATRIX
    if (verbose)
        std::cout << "Creating div-free system using the explicit discrete div-free operator \n";

    ParGridFunction* rhside_Hdiv = new ParGridFunction(R_space);  // rhside for the first equation in the original cfosls system
    *rhside_Hdiv = 0.0;

    ParLinearForm *qform(new ParLinearForm);
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
    {
        qform->Update(S_space, rhsblks.GetBlock(1), 0);
        if (strcmp(space_for_S,"H1") == 0) // S is from H1
            qform->AddDomainIntegrator(new GradDomainLFIntegrator(*Mytest.bf));
        else // S is from L2
            qform->AddDomainIntegrator(new DomainLFIntegrator(zero));
        qform->Assemble();
    }

    BlockOperator *MainOp = new BlockOperator(block_trueOffsets);

    // curl or divskew operator from C_space into R_space
    ParDiscreteLinearOperator Divfree_op(C_space, R_space); // from Hcurl or HDivSkew(C_space) to Hdiv(R_space)
    if (dim == 3)
        Divfree_op.AddDomainInterpolator(new CurlInterpolator());
    else // dim == 4
        Divfree_op.AddDomainInterpolator(new DivSkewInterpolator());
    Divfree_op.Assemble();
    //Divfree_op.EliminateTestDofs(ess_bdrSigma); is it needed here? I think no, we have bdr conditions for sigma already applied to M
    //ParGridFunction* rhside_Hcurl = new ParGridFunction(C_space);
    //Divfree_op.EliminateTrialDofs(ess_bdrU, xblks.GetBlock(0), *rhside_Hcurl);
    //Divfree_op.EliminateTestDofs(ess_bdrU);
    Divfree_op.Finalize();
    HypreParMatrix * Divfree_dop = Divfree_op.ParallelAssemble(); // from Hcurl or HDivSkew(C_space) to Hdiv(R_space)
    HypreParMatrix * DivfreeT_dop = Divfree_dop->Transpose();

    // mass matrix for H(div)
    ParBilinearForm *Mblock(new ParBilinearForm(R_space));
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
    {
        Mblock->AddDomainIntegrator(new VectorFEMassIntegrator);
        //Mblock->AddDomainIntegrator(new DivDivIntegrator); //only for debugging, delete this
    }
    else // no S, hence we need the matrix weight
        Mblock->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.Ktilda));
    Mblock->Assemble();
    Mblock->EliminateEssentialBC(ess_bdrSigma, *sigma_exact, *rhside_Hdiv);
    Mblock->Finalize();

    HypreParMatrix *M = Mblock->ParallelAssemble();

    // curl-curl matrix for H(curl) in 3D
    // either as DivfreeT_dop * M * Divfree_dop
    auto tempmat = ParMult(DivfreeT_dop,M);
    auto A = ParMult(tempmat, Divfree_dop);

    HypreParMatrix *C, *CH, *CHT, *B, *BT;
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
    {
        // diagonal block for H^1
        ParBilinearForm *Cblock = new ParBilinearForm(S_space);
        if (strcmp(space_for_S,"H1") == 0) // S is from H1
        {
            Cblock->AddDomainIntegrator(new MassIntegrator(*Mytest.bTb));
            Cblock->AddDomainIntegrator(new DiffusionIntegrator(*Mytest.bbT));
        }
        else // S is from L2
        {
            Cblock->AddDomainIntegrator(new MassIntegrator(*Mytest.bTb));
        }
        Cblock->Assemble();
        Cblock->EliminateEssentialBC(ess_bdrS, xblks.GetBlock(1),*qform);
        Cblock->Finalize();
        C = Cblock->ParallelAssemble();

        // off-diagonal block for (H(div), Space_for_S) block
        // you need to create a new integrator here to swap the spaces
        ParMixedBilinearForm *BTblock(new ParMixedBilinearForm(R_space, S_space));
        BTblock->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.minb));
        BTblock->Assemble();
        BTblock->EliminateTrialDofs(ess_bdrSigma, *sigma_exact, *qform);
        BTblock->EliminateTestDofs(ess_bdrS);
        BTblock->Finalize();
        BT = BTblock->ParallelAssemble();
        B = BT->Transpose();

        CHT = ParMult(DivfreeT_dop, B);
        CH = CHT->Transpose();
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

    // setting block operator of the system
    MainOp->SetBlock(0,0, A);
    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
    {
        MainOp->SetBlock(0,1, CHT);
        MainOp->SetBlock(1,0, CH);
        MainOp->SetBlock(1,1, C);
    }
#else
    if (verbose)
        std::cout << "This case is not supported any more \n";
    MPI_Finalize();
    return -1;
#endif

    if (verbose)
        cout << "Discretized problem is assembled \n";

    chrono.Clear();
    chrono.Start();

    Solver *prec;
    Array<BlockOperator*> P;
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
                        P.SetSize(P_C.Size());

                        for (int l = 0; l < P.Size(); l++)
                        {
                            auto offsets_f  = new Array<int>(3);
                            auto offsets_c  = new Array<int>(3);
                            (*offsets_f)[0] = (*offsets_c)[0] = 0;
                            (*offsets_f)[1] = P_C[l]->Height();
                            (*offsets_c)[1] = P_C[l]->Width();
                            (*offsets_f)[2] = (*offsets_f)[1] + P_H[l]->Height();
                            (*offsets_c)[2] = (*offsets_c)[1] + P_H[l]->Width();

                            P[l] = new BlockOperator(*offsets_f, *offsets_c);
                            P[l]->SetBlock(0, 0, P_C[l]);
                            P[l]->SetBlock(1, 1, P_H[l]);
                        }
                        prec = new MonolithicMultigrid(*MainOp, P);
                    }
                    else
                    {
                        prec = new BlockDiagonalPreconditioner(block_trueOffsets);
                        Operator * precU = new Multigrid(*A, P_C);
                        Operator * precS = new Multigrid(*C, P_H);
                        ((BlockDiagonalPreconditioner*)prec)->SetDiagonalBlock(0, precU);
                        ((BlockDiagonalPreconditioner*)prec)->SetDiagonalBlock(1, precS);
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
                        Operator * precU = new Multigrid(*A, P_C);
                        ((BlockDiagonalPreconditioner*)prec)->SetDiagonalBlock(0, precU);
                    }

                    //mfem_error("MG is not implemented when there is no S in the system");
                }
            }
            else // prec is AMS-like for the div-free part (block-diagonal for the system with boomerAMG for S)
            {
                if (dim == 3)
                {
                    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
                    {
                        prec = new BlockDiagonalPreconditioner(block_trueOffsets);
                        /*
                        Operator * precU = new HypreAMS(*A, C_space);
                        ((HypreAMS*)precU)->SetSingularProblem();
                        */

                        // Why in this case, when S is even in H1 as in the paper,
                        // CG is saying that the operator is not pos.def.
                        // And I checked that this is precU block that causes the trouble
                        // For, example, the following works:
                        Operator * precU = new IdentityOperator(A->Height());

                        Operator * precS;
                        /*
                        if (strcmp(space_for_S,"H1") == 0) // S is from H1
                        {
                            precS = new HypreBoomerAMG(*C);
                            ((HypreBoomerAMG*)precS)->SetPrintLevel(0);

                            //FIXME: do we need to set iterative mode = false here and around this place?
                        }
                        else // S is from L2
                        {
                            precS = new HypreDiagScale(*C);
                            //precS->SetPrintLevel(0);
                        }
                        */

                        precS = new IdentityOperator(C->Height());

                        //auto precSmatrix = ((HypreDiagScale*)precS)->GetData();
                        //SparseMatrix precSdiag;
                        //precSmatrix->GetDiag(precSdiag);
                        //precSdiag.Print();

                        ((BlockDiagonalPreconditioner*)prec)->SetDiagonalBlock(0, precU);
                        ((BlockDiagonalPreconditioner*)prec)->SetDiagonalBlock(1, precS);
                    }
                    else // no S, i.e. only an equation in div-free subspace
                    {
                        prec = new BlockDiagonalPreconditioner(block_trueOffsets);
                        /*
                        Operator * precU = new HypreAMS(*A, C_space);
                        ((HypreAMS*)precU)->SetSingularProblem();
                        */

                        // See the remark below, for the case when S is present
                        // CG is saying that the operator is not pos.def.
                        // And I checked that this is precU block that causes the trouble
                        // For, example, the following works:
                        Operator * precU = new IdentityOperator(A->Height());

                        ((BlockDiagonalPreconditioner*)prec)->SetDiagonalBlock(0, precU);
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
            cout << "Preconditioner is ready \n";
    }
    else
        if (verbose)
            cout << "Using no preconditioner \n";

    IterativeSolver * solver;
    solver = new CGSolver(comm);
    if (verbose)
        cout << "Linear solver: CG \n";
    //solver = new MINRESSolver(comm);
    //if (verbose)
        //cout << "Linear solver: MINRES \n";

    solver->SetAbsTol(sqrt(atol));
    solver->SetRelTol(sqrt(rtol));
    solver->SetMaxIter(max_num_iter);
    solver->SetOperator(*MainOp);

    if (with_prec)
        solver->SetPreconditioner(*prec);
    solver->SetPrintLevel(0);
    trueX = 0.0;
    solver->Mult(trueRhs, trueX);

    chrono.Stop();

    if (verbose)
    {
        if (solver->GetConverged())
            std::cout << "Linear solver converged in " << solver->GetNumIterations()
                      << " iterations with a residual norm of " << solver->GetFinalNorm() << ".\n";
        else
            std::cout << "Linear solver did not converge in " << solver->GetNumIterations()
                      << " iterations. Residual norm is " << solver->GetFinalNorm() << ".\n";
        std::cout << "Linear solver took " << chrono.RealTime() << "s. \n";
    }

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

    double err_u, norm_u;

    if (!withDiv)
    {
        err_u = u->ComputeL2Error(*Mytest.divfreepart, irs);
        norm_u = ComputeGlobalLpNorm(2, *Mytest.divfreepart, *pmesh, irs);

        if (verbose)
        {
            if ( norm_u > MYZEROTOL )
            {
                //std::cout << "norm_u = " << norm_u << "\n";
                cout << "|| u - u_ex || / || u_ex || = " << err_u / norm_u << endl;
            }
            else
                cout << "|| u || = " << err_u << " (u_ex = 0)" << endl;
        }
    }

    ParGridFunction * opdivfreepart = new ParGridFunction(R_space);
    DiscreteLinearOperator Divfree_h(C_space, R_space);
    if (dim == 3)
        Divfree_h.AddDomainInterpolator(new CurlInterpolator());
    else // dim == 4
        Divfree_h.AddDomainInterpolator(new DivSkewInterpolator());
    Divfree_h.Assemble();
    Divfree_h.Mult(*u, *opdivfreepart);

    ParGridFunction * opdivfreepart_exact;
    double err_opdivfreepart, norm_opdivfreepart;

    if (!withDiv)
    {
        opdivfreepart_exact = new ParGridFunction(R_space);
        opdivfreepart_exact->ProjectCoefficient(*Mytest.opdivfreepart);

        err_opdivfreepart = opdivfreepart->ComputeL2Error(*Mytest.opdivfreepart, irs);
        norm_opdivfreepart = ComputeGlobalLpNorm(2, *Mytest.opdivfreepart, *pmesh, irs);

        if (verbose)
        {
            if (norm_opdivfreepart > MYZEROTOL )
            {
                //cout << "|| opdivfreepart_ex || = " << norm_opdivfreepart << endl;
                cout << "|| Divfree_h u_h - opdivfreepart_ex || / || opdivfreepart_ex || = " << err_opdivfreepart / norm_opdivfreepart << endl;
            }
            else
                cout << "|| Divfree_h u_h || = " << err_opdivfreepart << " (divfreepart_ex = 0)" << endl;
        }
    }

    ParGridFunction * sigma = new ParGridFunction(R_space);
    *sigma = *Sigmahat;         // particular solution
    *sigma += *opdivfreepart;   // plus div-free guy

    /*
    // checking the divergence of sigma
    {
        Vector trueSigma(R_space->TrueVSize());
        sigma->ParallelProject(trueSigma);

        ParMixedBilinearForm *Dblock(new ParMixedBilinearForm(R_space, W_space));
        Dblock->AddDomainIntegrator(new VectorFEDivergenceIntegrator);
        Dblock->Assemble();
        Dblock->EliminateTrialDofs(ess_bdrSigma, x.GetBlock(0), *gform);
        Dblock->Finalize();
        HypreParMatrix * D = Dblock->ParallelAssemble();

        Vector trueDivSigma(W_space->TrueVSize());
        D->Mult(trueSigma, trueDivsigma);

        Vector trueF(W_space->TrueVSize());
        ParLinearForm * gform = new ParLinearForm(W_space);
        gform->AddDomainIntegrator(new DomainLFIntegrator(*Mytest.scalardivsigma));
        gform->Assemble();
        gform->ParallelAssemble(trueF);

        trueDivsigma -= trueF; // now it is div sigma - f, on true dofs from L_2 space

        double local_divres =
    }
    */

    if (strcmp(space_for_S,"H1") == 0 || !eliminateS) // S is present
    {
        S = new ParGridFunction(S_space);
        S->Distribute(&(trueX.GetBlock(1)));
    }
    else // no S, then we compute S from sigma
    {
        // temporary for checking the computation of S below
        //sigma->ProjectCoefficient(*Mytest.sigma);

        S = new ParGridFunction(S_space);

        ParBilinearForm *Cblock(new ParBilinearForm(S_space));
        Cblock->AddDomainIntegrator(new MassIntegrator(*Mytest.bTb));
        Cblock->Assemble();
        Cblock->Finalize();
        HypreParMatrix * C = Cblock->ParallelAssemble();

        ParMixedBilinearForm *Bblock(new ParMixedBilinearForm(R_space, S_space));
        Bblock->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.b));
        Bblock->Assemble();
        Bblock->Finalize();
        HypreParMatrix * B = Bblock->ParallelAssemble();
        Vector bTsigma(C->Height());
        Vector trueSigma(R_space->TrueVSize());
        sigma->ParallelProject(trueSigma);

        B->Mult(trueSigma,bTsigma);

        Vector trueS(C->Height());

        CG(*C, bTsigma, trueS, 0, 5000, 1e-9, 1e-12);
        S->Distribute(trueS);

    }

    double err_sigma = sigma->ComputeL2Error(*Mytest.sigma, irs);
    double norm_sigma = ComputeGlobalLpNorm(2, *Mytest.sigma, *pmesh, irs);

    if (verbose)
        cout << "sigma_h = sigma_hat + div-free part, div-free part = curl u_h \n";

    if (verbose)
    {
        if ( norm_sigma > MYZEROTOL )
            cout << "|| sigma_h - sigma_ex || / || sigma_ex || = " << err_sigma / norm_sigma << endl;
        else
            cout << "|| sigma || = " << err_sigma << " (sigma_ex = 0)" << endl;
    }

    /*
    double err_sigmahat = Sigmahat->ComputeL2Error(*Mytest.sigma, irs);
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

    double err_div = DivSigma.ComputeL2Error(*Mytest.scalardivsigma,irs);
    double norm_div = ComputeGlobalLpNorm(2, *Mytest.scalardivsigma, *pmesh, irs);

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

    double norm_S;
    //if (withS)
    {
        S_exact = new ParGridFunction(S_space);
        S_exact->ProjectCoefficient(*Mytest.scalarS);

        double err_S = S->ComputeL2Error(*Mytest.scalarS, irs);
        norm_S = ComputeGlobalLpNorm(2, *Mytest.scalarS, *pmesh, irs);
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
            if (dim == 3)
                GradSpace = C_space;
            else // dim == 4
            {
                FiniteElementCollection *hcurl_coll;
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

        }

#ifdef USE_CURLMATRIX
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

            /*
            ParMixedBilinearForm *BTblock(new ParMixedBilinearForm(R_space, S_space));
            if (strcmp(space_for_S,"L2") == 0 && eliminateS) // S was not present in the system
            {
                BTblock->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.minb));
                BTblock->Assemble();
                BTblock->EliminateTrialDofs(ess_bdrSigma, xblks.GetBlock(0), *qform);
                BTblock->EliminateTestDofs(ess_bdrS);
                BTblock->Finalize();
                BT = BTblock->ParallelAssemble();
            }
            */

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
        }
#endif
    }

    if (verbose)
        cout << "Computing projection errors \n";

    if(verbose && !withDiv)
    {
        double projection_error_u = u_exact->ComputeL2Error(*Mytest.divfreepart, irs);
        if ( norm_u > MYZEROTOL )
        {
            //std::cout << "Debug: || u_ex || = " << norm_u << "\n";
            //std::cout << "Debug: proj error = " << projection_error_u << "\n";
            cout << "|| u_ex - Pi_h u_ex || / || u_ex || = " << projection_error_u / norm_u << endl;
        }
        else
            cout << "|| Pi_h u_ex || = " << projection_error_u << " (u_ex = 0) \n ";
    }

    double projection_error_sigma = sigma_exact->ComputeL2Error(*Mytest.sigma, irs);

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
        double projection_error_S = S_exact->ComputeL2Error(*Mytest.scalarS, irs);

        if(verbose)
        {
            if ( norm_S > MYZEROTOL )
                cout << "|| S_ex - Pi_h S_ex || / || S_ex || = " << projection_error_S / norm_S << endl;
            else
                cout << "|| Pi_h S_ex ||  = " << projection_error_S << " (S_ex = 0) \n";
        }
    }
#endif

#ifdef COMPUTING_LAMBDA
    ParGridFunction * sigma_special = new ParGridFunction(R_space);
    ParGridFunction * lambda_special = new ParGridFunction(W_space);
    SparseMatrix Adiag_check;
    SparseMatrix Bdiag_check;
    int numblocks_special = 2;
    Array<int> offsets_special(numblocks_special + 1);
    offsets_special[0] = 0;
    offsets_special[1] = R_space->GetVSize();
    offsets_special[2] = W_space->GetVSize();
    offsets_special.PartialSum();

    Array<int> trueOffsets_special(numblocks_special + 1);
    trueOffsets_special[0] = 0;
    trueOffsets_special[1] = R_space->GetTrueVSize();
    trueOffsets_special[2] = W_space->GetTrueVSize();
    trueOffsets_special.PartialSum();

    BlockVector x_special(offsets_special);
    BlockVector trueX_special(trueOffsets_special);
    BlockVector trueRhs_special(trueOffsets_special);
    x_special = 0.0;
    trueX_special = 0.0;
    trueRhs_special = 0.0;
    {
        if (verbose)
            std::cout << "COMPUTING_LAMBDA is activated \n";



        Transport_test_divfree Mytest(nDimensions, numsol, numcurl);

        ParGridFunction * sigma_exact = new ParGridFunction(R_space);
        sigma_exact->ProjectCoefficient(*Mytest.sigma);

        x_special.GetBlock(0) = *sigma_exact;

        ParLinearForm *fform = new ParLinearForm(R_space);
        ConstantCoefficient zero(.0);
        fform->AddDomainIntegrator(new VectordivDomainLFIntegrator(zero));
        fform->Assemble();

        ParLinearForm *gform;
        gform = new ParLinearForm(W_space);
        gform->AddDomainIntegrator(new DomainLFIntegrator(*Mytest.scalardivsigma));
        gform->Assemble();

        // 10. Assemble the finite element matrices for the CFOSLS operator  A
        //     where:

        ParBilinearForm *Ablock(new ParBilinearForm(R_space));
        HypreParMatrix *A;
        Ablock->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.Ktilda));
        Ablock->Assemble();
        Ablock->EliminateEssentialBC(ess_bdrSigma, x_special.GetBlock(0), *fform);
        Ablock->Finalize();
        A = Ablock->ParallelAssemble();
        A->GetDiag(Adiag_check);

        //----------------
        //  D Block:
        //-----------------

        HypreParMatrix *D;
        HypreParMatrix *DT;

        ParMixedBilinearForm *Dblock(new ParMixedBilinearForm(R_space, W_space));
        Dblock->AddDomainIntegrator(new VectorFEDivergenceIntegrator);
        Dblock->Assemble();
        Dblock->EliminateTrialDofs(ess_bdrSigma, x_special.GetBlock(0), *gform);
        Dblock->Finalize();
        D = Dblock->ParallelAssemble();
        D->GetDiag(Bdiag_check);
        DT = D->Transpose();

        fform->ParallelAssemble(trueRhs_special.GetBlock(0));
        gform->ParallelAssemble(trueRhs_special.GetBlock(1));

        BlockOperator *CFOSLSop = new BlockOperator(trueOffsets_special);
        CFOSLSop->SetBlock(0,0, A);
        CFOSLSop->SetBlock(0,1, DT);
        CFOSLSop->SetBlock(1,0, D);

        MINRESSolver solver(MPI_COMM_WORLD);
        solver.SetAbsTol(atol);
        solver.SetRelTol(rtol);
        solver.SetMaxIter(70000);
        solver.SetOperator(*CFOSLSop);
        solver.SetPrintLevel(0);

        trueX_special = 0.0;
        //trueRhs_special.GetBlock(0).Print();
        solver.Mult(trueRhs_special, trueX_special);
        chrono.Stop();

        if (verbose)
        {
           if (solver.GetConverged())
              std::cout << "MINRES converged in " << solver.GetNumIterations()
                        << " iterations with a residual norm of " << solver.GetFinalNorm() << ".\n";
           else
              std::cout << "MINRES did not converge in " << solver.GetNumIterations()
                        << " iterations. Residual norm is " << solver.GetFinalNorm() << ".\n";
           std::cout << "MINRES solver took " << chrono.RealTime() << "s. \n";
        }

        /*
        std::cout << "\nTrying to check residual for base of sigma_special and lambda_special aka trueX \n";

        BlockVector trueRes(trueOffsets_special);
        CFOSLSop->Mult(trueX_special, trueRes);
        trueRes -= trueRhs_special;
        double trueres_func_norm = trueRes.Norml2() / sqrt (trueRes.Size());
        std::cout << "trueres_func norm for sigma and lambda: " << trueres_func_norm << "\n\n";

        Vector Asigma(A->Height());
        A->Mult(trueX_special.GetBlock(0), Asigma);
        Vector DTlambda(D->Width());
        DT->Mult(trueX_special.GetBlock(1), DTlambda);
        MFEM_ASSERT(DTlambda.Size() == Asigma.Size(), "Dimensions of A and D mismatch!");
        Vector res(Asigma.Size());
        res = Asigma;
        res += DTlambda;
        res -= trueRhs_special.GetBlock(0);
        double res_func_norm = res.Norml2() / sqrt (res.Size());
        std::cout << "res_func norm for sigma and lambda: " << res_func_norm << "\n\n";
        */


        sigma_special->Distribute(&(trueX_special.GetBlock(0)));
        lambda_special->Distribute(&(trueX_special.GetBlock(1)));

        int order_quad = max(2, 2*feorder+1);
        const IntegrationRule *irs[Geometry::NumGeom];
        for (int i=0; i < Geometry::NumGeom; ++i)
        {
            irs[i] = &(IntRules.Get(i, order_quad));
        }

        double err_sigma = sigma_special->ComputeL2Error(*Mytest.sigma, irs);
        double norm_sigma = ComputeGlobalLpNorm(2, *Mytest.sigma, *pmesh, irs);

        if (verbose)
            cout << "sigma_h = sigma_hat + div-free part, div-free part = curl u_h \n";

        if (verbose)
        {
            if ( norm_sigma > MYZEROTOL )
                cout << "|| sigma_h - sigma_ex || / || sigma_ex || = " << err_sigma / norm_sigma << endl;
            else
                cout << "|| sigma || = " << err_sigma << " (sigma_ex = 0)" << endl;
        }

        DiscreteLinearOperator Div(R_space, W_space);
        Div.AddDomainInterpolator(new DivergenceInterpolator());
        ParGridFunction DivSigma(W_space);
        Div.Assemble();
        Div.Mult(*sigma_special, DivSigma);

        double err_div = DivSigma.ComputeL2Error(*Mytest.scalardivsigma,irs);
        double norm_div = ComputeGlobalLpNorm(2, *Mytest.scalardivsigma, *pmesh, irs);

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

        // checking that lambda special satisfies the correct essential boundary conditions
        for (int i = 0; i < sigma_exact_finest->Size(); ++i )
        {
            if ( fabs( (*sigma_special)[i] - (*sigma_exact_finest)[i] ) > 1.0e-13 && (*(EssBdrDofs_R[0][0]))[i] != 0)
                std::cout << "Weird! \n";
        }
        //std::cout << "? \n\n";

    }
#endif

#ifdef NEW_STUFF
    if (verbose)
        std::cout << "\nCreating an instance of the new Hcurl smoother \n";

    if (verbose)
        std::cout << "Calling constructor of the new solver \n";

    /*
    double smooth_abstol = 1.0e-12;
    double smooth_reltol = 1.0e-12;

    if (verbose)
    {
        std::cout << "smooth_abstol = " << smooth_abstol << "\n";
        std::cout << "smooth_reltol = " << smooth_reltol << "\n";
    }

    HCurlSmoother NewSmoother(num_levels - 1, Divfree_mat_lvls[0],
                   Proj_Hcurl, Dof_TrueDof_Hcurl,
                   EssBdrDofs_Hcurl);
    NewSmoother.SetAbsTol(smooth_abstol);//(1.0e-10);//(new_abstol);
    NewSmoother.SetRelTol(smooth_reltol);//(1.0e-10);//(new_reltol);
    NewSmoother.SetMaxIterInt(20000);
    NewSmoother.SetPrintLevel(0);
    */

    HCurlGSSmoother NewGSSmoother(num_levels - 1, Divfree_mat_lvls,
                   Proj_Hcurl, Dof_TrueDof_Hcurl,
                   EssBdrDofs_Hcurl);
    //NewGSSmoother.SetSweepsNumber(5*(num_levels-1));
    NewGSSmoother.SetSweepsNumber(5);
    NewGSSmoother.SetPrintLevel(0);

    if (verbose)
        std::cout << "Number of smoothing steps: " << NewGSSmoother.GetSweepsNumber() << "\n";

    if (verbose)
        std::cout << "\nCreating an instance of the new multilevel solver \n";

    Array<BlockMatrix*> Element_dofs_Func(ref_levels);

    Array<int>* row_offsets_El_dofs = new Array<int>[num_levels - 1];
    Array<int>* col_offsets_El_dofs = new Array<int>[num_levels - 1];
    for (int i = 0; i < num_levels - 1; ++i)
    {
        row_offsets_El_dofs[i].SetSize(2);
        row_offsets_El_dofs[i][0] = 0;
        row_offsets_El_dofs[i][1] = Element_dofs_R[i]->Height();
        col_offsets_El_dofs[i].SetSize(2);
        col_offsets_El_dofs[i][0] = 0;
        col_offsets_El_dofs[i][1] = Element_dofs_R[i]->Width();
        Element_dofs_Func[i] = new BlockMatrix(row_offsets_El_dofs[i], col_offsets_El_dofs[i]);
        Element_dofs_Func[i]->SetBlock(0,0, Element_dofs_R[i]);
    }

    Array<BlockMatrix*> P_Func(ref_levels);

    Array<int> * row_offsets_P_Func = new Array<int>[num_levels - 1];
    Array<int> * col_offsets_P_Func = new Array<int>[num_levels - 1];
    for (int i = 0; i < num_levels - 1; ++i)
    {
        row_offsets_P_Func[i].SetSize(2);
        row_offsets_P_Func[i][0] = 0;
        row_offsets_P_Func[i][1] = P_R[i]->Height();
        col_offsets_P_Func[i].SetSize(2);
        col_offsets_P_Func[i][0] = 0;
        col_offsets_P_Func[i][1] = P_R[i]->Width();
        P_Func[i] = new BlockMatrix(row_offsets_P_Func[i], col_offsets_P_Func[i]);
        P_Func[i]->SetBlock(0,0, P_R[i]);
    }


    Array<BlockOperator*> TrueP_Func(ref_levels);
    Array<int> * row_offsets_TrueP_Func = new Array<int>[num_levels - 1];
    Array<int> * col_offsets_TrueP_Func = new Array<int>[num_levels - 1];

    for (int i = 0; i < num_levels - 1; ++i)
    {
        row_offsets_TrueP_Func[i].SetSize(2);
        row_offsets_TrueP_Func[i][0] = 0;
        row_offsets_TrueP_Func[i][1] = TrueP_R[i]->Height();
        col_offsets_TrueP_Func[i].SetSize(2);
        col_offsets_TrueP_Func[i][0] = 0;
        col_offsets_TrueP_Func[i][1] = TrueP_R[i]->Width();
        TrueP_Func[i] = new BlockOperator(row_offsets_TrueP_Func[i], col_offsets_TrueP_Func[i]);
        TrueP_Func[i]->SetBlock(0,0, TrueP_R[i]);
    }



    Array<SparseMatrix*> P_WT(ref_levels); //AE_e matrices
    for (int i = 0; i < ref_levels; ++i)
    {
        P_WT[i] = Transpose(*P_W[i]);
    }

    //ParLinearForm *fform = new ParLinearForm(R_space);

    ParLinearForm * constrfform = new ParLinearForm(W_space);
    constrfform->AddDomainIntegrator(new DomainLFIntegrator(*Mytest.scalardivsigma));
    constrfform->Assemble();

    ParMixedBilinearForm *Bblock(new ParMixedBilinearForm(R_space, W_space));
    Bblock->AddDomainIntegrator(new VectorFEDivergenceIntegrator);
    Bblock->Assemble();
    //Bblock->EliminateTrialDofs(ess_bdrSigma, *sigma_exact_finest, *constrfform); // // makes res for sigma_special happier
    Bblock->Finalize();

    Vector Floc(P_W[0]->Height());
    Floc = *constrfform;

    BlockVector Xinit(Funct_mat_lvls[0]->ColOffsets());
    Xinit.GetBlock(0) = 0.0;
    MFEM_ASSERT(Xinit.GetBlock(0).Size() == sigma_exact_finest->Size(),
                "Xinit and sigma_exact_finest have different sizes! \n");

#ifdef EXACTSOLH_INIT
    if (verbose)
        std::cout << "EXACTSOLH_INIT is activated \n";
#endif

    for (int i = 0; i < sigma_exact_finest->Size(); ++i )
    {
        // just setting Xinit to store correct boundary values at essential boundary
        if ( (*EssBdrDofs_R[0][0])[i] != 0)
            Xinit.GetBlock(0)[i] = (*sigma_exact_finest)[i];

        //probably doing something better than just essential boundary values
#ifdef EXACTSOLH_INIT
    #ifdef COMPUTING_LAMBDA
        Xinit.GetBlock(0)[i] = (*sigma_special)[i];
        if ( fabs ((*sigma_special)[i] - (*sigma_exact_finest)[i]) > 1.0e-10 && (*EssBdrDofs_R[0][0])[i] != 0)
            std::cout << "Weird! Mismatching essential boundary values for sigma_h,exact and sigma_exact,h \n";
    #else
        //Xinit.GetBlock(0)[i] = sigmahat_pau[i];
    #endif
#endif
    }

#ifdef EXACTSOLH_INIT
    BlockVector res_func (Funct_mat_lvls[0]->ColOffsets());
    Funct_mat_lvls[0]->Mult(Xinit, res_func);

    ParGridFunction * Ax_check = new ParGridFunction(R_space);
    Vector trueAx_check(Adiag_check.Height());
    Adiag_check.Mult(trueX_special.GetBlock(0), trueAx_check);
    Ax_check->Distribute(&trueAx_check);
    *Ax_check -= res_func.GetBlock(0);
    double Ax_diff_norm = Ax_check->Norml2() / sqrt (Ax_check->Size());
    if (verbose)
        std::cout << "(Adiag sigma_special - A\' Xinit ) norm: " << Ax_diff_norm << "\n";

    Vector BTlambda(Constraint_mat_lvls[0]->Width());
    Constraint_mat_lvls[0]->MultTranspose(*lambda_special, BTlambda);

    ParGridFunction * BTlam_check = new ParGridFunction(R_space);
    Vector trueBTlam_check(Bdiag_check.Width());
    Bdiag_check.MultTranspose(trueX_special.GetBlock(1), trueBTlam_check);
    BTlam_check->Distribute(&trueBTlam_check);
    *BTlam_check -= BTlambda;
    double BTlam_diff_norm = BTlam_check->Norml2() / sqrt (BTlam_check->Size());
    if (verbose)
        std::cout << "Bdiag sigma_special - B\' Xinit ) norm: " << BTlam_diff_norm << "\n";

    res_func.GetBlock(0) += BTlambda;
    //res_func.GetBlock(0).Print();
    for ( int i = 0; i < res_func.GetBlock(0).Size(); ++i)
    {
        double value = fabs(res_func.GetBlock(0)[i]);
        if ( value > 1.0e-6 && value < 1.0e-4 )
        {
            std::cout << "i = " << i << "between 1.0e-4 and 1.0e-6, val = " << value << "\n";
            if ( (*EssBdrDofs_R[0][0])[i] != 0 )
                std::cout << "It belongs to the essential boundary! \n";
            else if ( (*BdrDofs_R[0][0])[i] != 0 )
                std::cout << "It belongs to the nonessential boundary! \n";
        }
    }

    double res_func_norm = res_func.GetBlock(0).Norml2() / sqrt (res_func.GetBlock(0).Size());
    if (verbose)
        std::cout << "residual norm in functional for correct sigma_h: " << res_func_norm << "\n";

    Vector res_constr (Constraint_mat_lvls[0]->Height());
    Constraint_mat_lvls[0]->Mult(Xinit, res_constr);
    res_constr -= Floc;
    double res_constr_norm = res_constr.Norml2() / sqrt (res_constr.Size());
    if (verbose)
        std::cout << "residual norm in constraint for correct sigma_h: " << res_constr_norm << "\n";

#endif

    //MPI_Finalize();
    //return 0;

    // testing some matrix properties:
    // realizing that we miss canonical projections to have
    // coarsened curl orthogonal to coarsened divergence
    /*
    // 1. looking at (P_RT)^T * P_RT - it is not diagonal!
    SparseMatrix * temppp = Transpose(P_Func[0]->GetBlock(0,0));
    SparseMatrix * testtt = Mult(*temppp, P_Func[0]->GetBlock(0,0));

    //testtt->Print();

    // 2. looking at B_0 * C_0
    SparseMatrix * testtt2 = Mult(Bloc,Divfree_op_sp);
    //testtt2->Print();

    // 3. looking at B_0 * (P_RT)^T * P_RT * C_0
    SparseMatrix * temppp2 = Mult(Bloc,P_Func[0]->GetBlock(0,0));
    SparseMatrix * temppp3 = Mult(*temppp,Divfree_op_sp);

    SparseMatrix * testtt3 = Mult(*temppp2,*temppp3);
    //testtt3->Print();
    */


    if (verbose)
        std::cout << "Calling constructor of the new solver \n";

    chrono.Clear();
    chrono.Start();

    const bool higher_order = false;
    const bool construct_coarseops = true;
    MultilevelSmoother * Smoother;
#ifdef WITH_SMOOTHER
    Smoother = &NewGSSmoother;
    //Smoother = &NewSmoother;
#else
    Smoother = NULL;
#endif

    MinConstrSolver NewSolver(num_levels, P_WT,
                     Element_dofs_Func, Element_dofs_W, Dof_TrueDof_Func_lvls, Dof_TrueDof_L2_lvls,
                     P_Func, TrueP_Func, P_W, BdrDofs_R, EssBdrDofs_R,
                     Funct_mat_lvls, Constraint_mat_lvls, Floc, Xinit,
#ifdef COMPUTING_LAMBDA
                     *sigma_special, *lambda_special,
#endif
                     Smoother, higher_order, construct_coarseops);

    double newsolver_reltol = 1.0e-6;

    if (verbose)
    {
        std::cout << "newsolver_reltol = " << newsolver_reltol << "\n";
    }

    NewSolver.SetRelTol(newsolver_reltol);
    NewSolver.SetMaxIter(30);
    NewSolver.SetPrintLevel(1);
    NewSolver.SetStopCriteriaType(0);
    NewSolver.SetOptimizedLocalSolve(true);

    Vector ParticSol(*(NewSolver.ParticularSolution()));

    /*
    if (verbose)
        std::cout << "Checking that particular solution in parallel version satisfies the divergence constraint \n";

    ParGridFunction * PartSolDofs = new ParGridFunction(R_space_lvls[0]);
    PartSolDofs->Distribute(ParticSol);

    MFEM_ASSERT(CheckConstrRes(PartSolDofs, Constraint_mat_lvls[0], ConstrRhs, "in the main code for poarticular solution"), "Failure");

    if (verbose)
        std::cout << "Success \n";
    MPI_Finalize();
    return 0;
    */

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

    if (verbose)
        std::cout << "New solver was set up in " << chrono.RealTime() << " seconds.\n";

    if (verbose)
        std::cout << "\nCalling the new multilevel solver for the first iteration \n";

    ParGridFunction * NewSigmahat = new ParGridFunction(R_space_lvls[0]);

    //Vector Tempx(sigma_exact_finest->Size());
    //Tempx = 0.0;
    //Vector Tempy(Tempx.Size());
    Vector Tempy(ParticSol.Size());
    Tempy = 0.0;

#ifdef CHECK_SPDSOLVER

    // checking that for unsymmetric version the symmetry check does
    // provide the negative answer
    //NewSolver.SetUnSymmetric();

    Vector Vec1(Funct_mat_lvls[0]->Height());
    Vec1.Randomize(2000);
    Vector Vec2(Funct_mat_lvls[0]->Height());
    Vec2.Randomize(-39);

    for ( int i = 0; i < Vec1.Size(); ++i )
    {
        if ((*EssBdrDofs_R[0][0])[i] != 0 )
        {
            Vec1[i] = 0.0;
            Vec2[i] = 0.0;
        }
    }

    Vector VecDiff(Vec1.Size());
    VecDiff = Vec1;

    std::cout << "Norm of Vec1 = " << VecDiff.Norml2() / sqrt(VecDiff.Size())  << "\n";

    VecDiff -= Vec2;

    MFEM_ASSERT(VecDiff.Norml2() / sqrt(VecDiff.Size()) > 1.0e-10, "Vec1 equals Vec2 but they must be different");
    //VecDiff.Print();
    std::cout << "Norm of (Vec1 - Vec2) = " << VecDiff.Norml2() / sqrt(VecDiff.Size())  << "\n";

    NewSolver.SetAsPreconditioner(true);
    NewSolver.SetMaxIter(5);

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


    ParLinearForm *fform = new ParLinearForm(R_space_lvls[0]);
    ConstantCoefficient zerotest(.0);
    fform->AddDomainIntegrator(new VectordivDomainLFIntegrator(zerotest));
    fform->Assemble();


    ParBilinearForm *Ablocktest(new ParBilinearForm(R_space_lvls[0]));
    HypreParMatrix *Atest;
    Ablocktest->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.Ktilda));
    Ablocktest->Assemble();
    Ablocktest->EliminateEssentialBC(ess_bdrSigma, *sigma_exact_finest, *fform);
    Ablocktest->Finalize();
    Atest = Ablocktest->ParallelAssemble();

    Vector trueXtest(Atest->Width());
    Vector trueRhstest(Atest->Height());
    trueRhstest = 0.0;

    fform->ParallelAssemble(trueRhstest);

    int TestmaxIter(50);

    CGSolver Testsolver(MPI_COMM_WORLD);
    Testsolver.SetAbsTol(sqrt(atol));
    Testsolver.SetRelTol(sqrt(rtol));
    Testsolver.SetMaxIter(TestmaxIter);
    Testsolver.SetOperator(*Atest);
    Testsolver.SetPrintLevel(1);

    NewSolver.SetAsPreconditioner(true);
    NewSolver.SetPrintLevel(0);
    NewSolver.PrintAllOptions();
    Testsolver.SetPreconditioner(NewSolver);

    trueXtest = 0.0;
    // trueRhstest = - M * particular solution, on true dofs
    Atest->Mult(trueParticSol, trueRhstest);
    trueRhstest *= -1.0;

    chrono.Clear();
    chrono.Start();

    Testsolver.Mult(trueRhstest, trueXtest);

    chrono.Stop();

    if (verbose)
    {
        if (Testsolver.GetConverged())
            std::cout << "Linear solver converged in " << Testsolver.GetNumIterations()
                      << " iterations with a residual norm of " << Testsolver.GetFinalNorm() << ".\n";
        else
            std::cout << "Linear solver did not converge in " << Testsolver.GetNumIterations()
                      << " iterations. Residual norm is " << Testsolver.GetFinalNorm() << ".\n";
        std::cout << "Linear solver took " << chrono.RealTime() << "s. \n";
    }

    trueXtest += trueParticSol;
    NewSigmahat->Distribute(trueXtest);

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
    gformtest->AddDomainIntegrator(new DomainLFIntegrator(*Mytest.scalardivsigma));
    gformtest->Assemble();

    ParBilinearForm *Ablock(new ParBilinearForm(R_space));
    HypreParMatrix *Atest;
    Ablock->AddDomainIntegrator(new VectorFEMassIntegrator(*Mytest.Ktilda));
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

        double norm_sigma = ComputeGlobalLpNorm(2, *Mytest.sigma, *pmesh, irs);
        double err_newsigmahat = NewSigmahat->ComputeL2Error(*Mytest.sigma, irs);
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

        double err_div = DivSigma.ComputeL2Error(*Mytest.scalardivsigma,irs);
        double norm_div = ComputeGlobalLpNorm(2, *Mytest.scalardivsigma, *pmesh, irs);

        if (verbose)
        {
            cout << "|| div (new sigma_h - sigma_ex) || / ||div (sigma_ex)|| = "
                      << err_div/norm_div  << "\n";
        }
    }

    MPI_Finalize();
    return 0;
#endif

    chrono.Clear();
    chrono.Start();

    Vector NewRhs(ParticSol.Size());
    NewRhs = 0.0;
    Vector NewX(NewRhs.Size());
    NewX = 0.0;

    //NewRhs = 0.02;
    NewSolver.SetInitialGuess(ParticSol);
    //NewSolver.SetUnSymmetric(); // FIXME: temporarily, while debugging parallel version!!!

    NewSolver.Mult(NewRhs, NewX);



    /*
    MPI_Barrier(comm);
    if (myid == 0)
    {
        std::cout << "Tempy.Size = " << Tempy.Size() << "\n";
        std::cout << "NewSigmahat.Size = " << NewSigmahat->Size() << "\n";
    }
    MPI_Barrier(comm);
    if (myid == 1)
    {
        std::cout << "Tempy.Size = " << Tempy.Size() << "\n";
        std::cout << "NewSigmahat.Size = " << NewSigmahat->Size() << "\n";
    }
    MPI_Barrier(comm);
    */

    /*
    //NewX = 1.0;

    //NewSigmahat->Distribute(&NewX);

    //NewX = 0.002;

    //Vector temp(NewX.Size());
    //temp = 0.002;
    //sigma_exact_finest->Distribute(&temp);
    //double debugg_norm = sigma_exact_finest->Norml2() / sqrt (sigma_exact_finest->Size());
    //std::cout << "debugg norm outside = " << debugg_norm << "\n";

    Vector trueexact(NewX.Size());
    //trueexact = *(sigma_exact_finest->GetTrueDofs());
    trueexact = NewX;
    //sigma_exact_finest->ParallelAssemble(trueexact);
    //sigma_exact_finest->ParallelProject(trueexact);

    //if (myid == 0)
        //sigma_exact_finest->Print(std::cout,1);

    //NewX.Print();

    Vector error(NewX.Size());
    error = 0.0;
    //error = NewX;
    error = ParticSol;
    //error -= trueexact;
    //error = 1.0;

    int local_size = error.Size();
    int global_size = 0;
    MPI_Allreduce(&local_size, &global_size, 1, MPI_INT, MPI_SUM, comm);

    std::cout << std::flush;
    MPI_Barrier(comm);
    if (myid == 0)
    {
        std::cout << "myid = 0 \n";
        std::cout << "local_size = " << local_size << "\n";
        std::cout << "global_size = " << global_size << "\n";
    }
    MPI_Barrier(comm);
    if (myid == 1)
    {
        std::cout << "myid = 1 \n";
        std::cout << "local_size = " << local_size << "\n";
        std::cout << "global_size = " << global_size << "\n";
    }
    MPI_Barrier(comm);

    double local_normsq = error * error;
    double global_norm = 0.0;
    MPI_Allreduce(&local_normsq, &global_norm, 1, MPI_DOUBLE, MPI_SUM, comm);
    global_norm = sqrt (global_norm / global_size);

    MPI_Barrier(comm);
    if (myid == 0)
    {
        std::cout << "myid = 0 \n";
        std::cout << "local_normsq = " << local_normsq << "\n";
        std::cout << "global_norm = " << global_norm << "\n";
        std::cout << "error norml2 = " << sqrt( error.Norml2() * error.Norml2() / error.Size() ) << "\n";
    }
    MPI_Barrier(comm);
    if (myid == 1)
    {
        std::cout << "myid = 1 \n";
        std::cout << "local_normsq = " << local_normsq << "\n";
        std::cout << "global_norm = " << global_norm << "\n";
        std::cout << "error norml2 = " << sqrt( error.Norml2() * error.Norml2() / error.Size() ) << "\n";
    }
    MPI_Barrier(comm);

    std::cout << std::flush;
    MPI_Barrier(comm);

    if (verbose)
        std::cout << "error norm special = " << global_norm << "\n";

    MPI_Finalize();
    return 0;
    */

    //*NewSigmahat = 1.0;

    NewSigmahat->Distribute(&NewX);

    std::cout << "Solution computed via the new solver \n";

    double max_bdr_error = 0;
    for ( int dof = 0; dof < Xinit.Size(); ++dof)
    {
        if ( (*EssBdrDofs_R[0][0])[dof] != 0.0)
        {
            //std::cout << "ess dof index: " << dof << "\n";
            double bdr_error_dof = fabs(Xinit[dof] - (*NewSigmahat)[dof]);
            if ( bdr_error_dof > max_bdr_error )
                max_bdr_error = bdr_error_dof;
        }
    }

    if (max_bdr_error > 1.0e-14)
        std::cout << "Error, boundary values for the solution are wrong:"
                     " max_bdr_error = " << max_bdr_error << "\n";
    //else
        //std::cout << "After iter " << i << " of the new solver bdr values at ess bdr are correct \n";

    {
        int order_quad = max(2, 2*feorder+1);
        const IntegrationRule *irs[Geometry::NumGeom];
        for (int i = 0; i < Geometry::NumGeom; ++i)
        {
            irs[i] = &(IntRules.Get(i, order_quad));
        }

        double norm_sigma = ComputeGlobalLpNorm(2, *Mytest.sigma, *pmesh, irs);
        double err_newsigmahat = NewSigmahat->ComputeL2Error(*Mytest.sigma, irs);
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

        double err_div = DivSigma.ComputeL2Error(*Mytest.scalardivsigma,irs);
        double norm_div = ComputeGlobalLpNorm(2, *Mytest.scalardivsigma, *pmesh, irs);

        if (verbose)
        {
            cout << "|| div (new sigma_h - sigma_ex) || / ||div (sigma_ex)|| = "
                      << err_div/norm_div  << "\n";
        }
    }

    chrono.Stop();


    if (verbose)
        std::cout << "\n";

    MPI_Finalize();
    return 0;

#endif

#ifdef VISUALIZATION
    if (visualization && nDimensions < 4)
    {
        char vishost[] = "localhost";
        int  visport   = 19916;

        if (!withDiv)
        {
            socketstream uex_sock(vishost, visport);
            uex_sock << "parallel " << num_procs << " " << myid << "\n";
            uex_sock.precision(8);
            MPI_Barrier(pmesh->GetComm());
            uex_sock << "solution\n" << *pmesh << *u_exact << "window_title 'u_exact'"
                   << endl;

            socketstream uh_sock(vishost, visport);
            uh_sock << "parallel " << num_procs << " " << myid << "\n";
            uh_sock.precision(8);
            MPI_Barrier(pmesh->GetComm());
            uh_sock << "solution\n" << *pmesh << *u << "window_title 'u_h'"
                   << endl;

            *u -= *u_exact;
            socketstream udiff_sock(vishost, visport);
            udiff_sock << "parallel " << num_procs << " " << myid << "\n";
            udiff_sock.precision(8);
            MPI_Barrier(pmesh->GetComm());
            udiff_sock << "solution\n" << *pmesh << *u << "window_title 'u_h - u_exact'"
                   << endl;

            socketstream opdivfreepartex_sock(vishost, visport);
            opdivfreepartex_sock << "parallel " << num_procs << " " << myid << "\n";
            opdivfreepartex_sock.precision(8);
            MPI_Barrier(pmesh->GetComm());
            opdivfreepartex_sock << "solution\n" << *pmesh << *opdivfreepart_exact << "window_title 'curl u_exact'"
                   << endl;

            socketstream opdivfreepart_sock(vishost, visport);
            opdivfreepart_sock << "parallel " << num_procs << " " << myid << "\n";
            opdivfreepart_sock.precision(8);
            MPI_Barrier(pmesh->GetComm());
            opdivfreepart_sock << "solution\n" << *pmesh << *opdivfreepart << "window_title 'curl u_h'"
                   << endl;

            *opdivfreepart -= *opdivfreepart_exact;
            socketstream opdivfreepartdiff_sock(vishost, visport);
            opdivfreepartdiff_sock << "parallel " << num_procs << " " << myid << "\n";
            opdivfreepartdiff_sock.precision(8);
            MPI_Barrier(pmesh->GetComm());
            opdivfreepartdiff_sock << "solution\n" << *pmesh << *opdivfreepart << "window_title 'curl u_h - curl u_exact'"
                   << endl;
        }

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

    // 17. Free the used memory.

    delete C_space;
    delete hdivfree_coll;
    delete R_space;
    delete hdiv_coll;
    delete H_space;
    delete h1_coll;

    if (withDiv)
    {
        delete W_space;
        delete l2_coll;
    }

    MPI_Finalize();
    return 0;
}

template <void (*bvecfunc)(const Vector&, Vector& )> \
void KtildaTemplate(const Vector& xt, DenseMatrix& Ktilda)
{
    int nDimensions = xt.Size();
    Ktilda.SetSize(nDimensions);
    Vector b;
    bvecfunc(xt,b);
    double bTbInv = (-1./(b*b));
    Ktilda.Diag(1.0,nDimensions);
#ifndef K_IDENTITY
    AddMult_a_VVt(bTbInv,b,Ktilda);
#endif
}

template <void (*bvecfunc)(const Vector&, Vector& )> \
void bbTTemplate(const Vector& xt, DenseMatrix& bbT)
{
    Vector b;
    bvecfunc(xt,b);
    MultVVt(b, bbT);
}


template <double (*S)(const Vector&), void (*bvecfunc)(const Vector&, Vector& )> \
void sigmaTemplate(const Vector& xt, Vector& sigma)
{
    Vector b;
    bvecfunc(xt, b);
    sigma.SetSize(xt.Size());
    sigma(xt.Size()-1) = S(xt);
    for (int i = 0; i < xt.Size()-1; i++)
        sigma(i) = b(i) * sigma(xt.Size()-1);

    return;
}

template <void (*bvecfunc)(const Vector&, Vector& )> \
double bTbTemplate(const Vector& xt)
{
    Vector b;
    bvecfunc(xt,b);
    return b*b;
}

template <double (*S)(const Vector&), void (*bvecfunc)(const Vector&, Vector& )> \
double minbTbSnonhomoTemplate(const Vector& xt)
{
    Vector xt0(xt.Size());
    xt0 = xt;
    xt0 (xt0.Size() - 1) = 0;

    return - bTbTemplate<bvecfunc>(xt) * S(xt0);
}



template<double (*S)(const Vector & xt), double (*dSdt)(const Vector & xt), void(*Sgradxvec)(const Vector & x, Vector & gradx), \
         void(*bvec)(const Vector & x, Vector & vec), double (*divbfunc)(const Vector & xt) > \
double divsigmaTemplate(const Vector& xt)
{
    Vector b;
    bvec(xt,b);

    Vector gradS;
    Sgradxvec(xt,gradS);

    double res = 0.0;

    res += dSdt(xt);
    for ( int i= 0; i < xt.Size() - 1; ++i )
        res += b(i) * gradS(i);
    res += divbfunc(xt) * S(xt);

    return res;
}

template<double (*S)(const Vector & xt), double (*dSdt)(const Vector & xt), void(*Sgradxvec)(const Vector & x, Vector & gradx), \
         void(*bvec)(const Vector & x, Vector & vec), double (*divbfunc)(const Vector & xt)> \
void bfTemplate(const Vector& xt, Vector& bf)
{
    bf.SetSize(xt.Size());

    Vector b;
    bvec(xt,b);

    double f = divsigmaTemplate<S, dSdt, Sgradxvec, bvec, divbfunc>(xt);

    for (int i = 0; i < bf.Size(); ++i)
        bf(i) = f * b(i);
}

template<double (*S)(const Vector & xt), double (*dSdt)(const Vector & xt), void(*Sgradxvec)(const Vector & x, Vector & gradx), \
         void(*bvec)(const Vector & x, Vector & vec), double (*divbfunc)(const Vector & xt)> \
void bdivsigmaTemplate(const Vector& xt, Vector& bdivsigma)
{
    bdivsigma.SetSize(xt.Size());

    Vector b;
    bvec(xt,b);

    double divsigma = divsigmaTemplate<S, dSdt, Sgradxvec, bvec, divbfunc>(xt);

    for (int i = 0; i < bdivsigma.Size(); ++i)
        bdivsigma(i) = divsigma * b(i);
}

template<void(*bvec)(const Vector & x, Vector & vec)>
void minbTemplate(const Vector& xt, Vector& minb)
{
    minb.SetSize(xt.Size());

    bvec(xt,minb);

    minb *= -1;
}

template<double (*S)(const Vector & xt) > double SnonhomoTemplate(const Vector& xt)
{
    Vector xt0(xt.Size());
    xt0 = xt;
    xt0 (xt0.Size() - 1) = 0;

    return S(xt0);
}



double uFun_ex(const Vector& xt)
{
    double t = xt(xt.Size()-1);
    //return t;
    ////setback
    return sin(t)*exp(t);
}

double uFun_ex_dt(const Vector& xt)
{
    double t = xt(xt.Size()-1);
    return (cos(t) + sin(t)) * exp(t);
}

void uFun_ex_gradx(const Vector& xt, Vector& gradx )
{
    gradx.SetSize(xt.Size() - 1);
    gradx = 0.0;
}

void bFun_ex(const Vector& xt, Vector& b )
{
    b.SetSize(xt.Size());

    //for (int i = 0; i < xt.Size()-1; i++)
    //b(i) = xt(i) * (1 - xt(i));

    //if (xt.Size() == 4)
    //b(2) = 1-cos(2*xt(2)*M_PI);
    //b(2) = sin(xt(2)*M_PI);
    //b(2) = 1-cos(xt(2)*M_PI);

    b(0) = sin(xt(0)*2*M_PI)*cos(xt(1)*M_PI);
    b(1) = sin(xt(1)*M_PI)*cos(xt(0)*M_PI);
    b(2) = 1-cos(2*xt(2)*M_PI);

    b(xt.Size()-1) = 1.;

}

double bFundiv_ex(const Vector& xt)
{
    double x = xt(0);
    double y = xt(1);
    double z = xt(2);
    //double t = xt(xt.Size()-1);
    if (xt.Size() == 4)
        return 2*M_PI * cos(x*2*M_PI)*cos(y*M_PI) + M_PI * cos(y*M_PI)*cos(x*M_PI) + 2*M_PI * sin(2*z*M_PI);
    if (xt.Size() == 3)
        return 2*M_PI * cos(x*2*M_PI)*cos(y*M_PI) + M_PI * cos(y*M_PI)*cos(x*M_PI);
    return 0.0;
}

double uFun2_ex(const Vector& xt)
{
    double x = xt(0);
    double y = xt(1);
    double t = xt(xt.Size()-1);

    return t * exp(t) * sin (((x - 0.5)*(x - 0.5) + y*y));
}

double uFun2_ex_dt(const Vector& xt)
{
    double x = xt(0);
    double y = xt(1);
    double t = xt(xt.Size()-1);
    return (1.0 + t) * exp(t) * sin (((x - 0.5)*(x - 0.5) + y*y));
}

void uFun2_ex_gradx(const Vector& xt, Vector& gradx )
{
    double x = xt(0);
    double y = xt(1);
    double t = xt(xt.Size()-1);

    gradx.SetSize(xt.Size() - 1);

    gradx(0) = t * exp(t) * 2.0 * (x - 0.5) * cos (((x - 0.5)*(x - 0.5) + y*y));
    gradx(1) = t * exp(t) * 2.0 * y         * cos (((x - 0.5)*(x - 0.5) + y*y));
}

/*
double fFun2(const Vector& xt)
{
    double x = xt(0);
    double y = xt(1);
    double t = xt(xt.Size()-1);
    Vector b(3);
    bFunCircle2D_ex(xt,b);
    return (t + 1) * exp(t) * sin (((x - 0.5)*(x - 0.5) + y*y)) +
             t * exp(t) * 2.0 * (x - 0.5) * cos (((x - 0.5)*(x - 0.5) + y*y)) * b(0) +
             t * exp(t) * 2.0 * y         * cos (((x - 0.5)*(x - 0.5) + y*y)) * b(1);
}
*/

double uFun3_ex(const Vector& xt)
{
    double x = xt(0);
    double y = xt(1);
    double z = xt(2);
    double t = xt(xt.Size()-1);
    return sin(t)*exp(t) * sin ( M_PI * (x + y + z));
}

double uFun3_ex_dt(const Vector& xt)
{
    double x = xt(0);
    double y = xt(1);
    double z = xt(2);
    double t = xt(xt.Size()-1);
    return (sin(t) + cos(t)) * exp(t) * sin ( M_PI * (x + y + z));
}

void uFun3_ex_gradx(const Vector& xt, Vector& gradx )
{
    double x = xt(0);
    double y = xt(1);
    double z = xt(2);
    double t = xt(xt.Size()-1);

    gradx.SetSize(xt.Size() - 1);

    gradx(0) = sin(t) * exp(t) * M_PI * cos ( M_PI * (x + y + z));
    gradx(1) = sin(t) * exp(t) * M_PI * cos ( M_PI * (x + y + z));
    gradx(2) = sin(t) * exp(t) * M_PI * cos ( M_PI * (x + y + z));
}


/*
double fFun3(const Vector& xt)
{
    double x = xt(0);
    double y = xt(1);
    double z = xt(2);
    double t = xt(xt.Size()-1);
    Vector b(4);
    bFun_ex(xt,b);

    return (cos(t)*exp(t)+sin(t)*exp(t)) * sin ( M_PI * (x + y + z)) +
            sin(t)*exp(t) * M_PI * cos ( M_PI * (x + y + z)) * b(0) +
            sin(t)*exp(t) * M_PI * cos ( M_PI * (x + y + z)) * b(1) +
            sin(t)*exp(t) * M_PI * cos ( M_PI * (x + y + z)) * b(2) +
            (2*M_PI*cos(x*2*M_PI)*cos(y*M_PI) +
             M_PI*cos(y*M_PI)*cos(x*M_PI)+
             + 2*M_PI*sin(z*2*M_PI)) * uFun3_ex(xt);
}
*/

double uFun4_ex(const Vector& xt)
{
    double x = xt(0);
    double y = xt(1);
    double t = xt(xt.Size()-1);

    return exp(t) * sin (((x - 0.5)*(x - 0.5) + y*y));
    //return t * exp(t) * sin (((x - 0.5)*(x - 0.5) + y*y)) + 5.0 * (x + y);
}

double uFun4_ex_dt(const Vector& xt)
{
    return uFun4_ex(xt);
}

void uFun4_ex_gradx(const Vector& xt, Vector& gradx )
{
    double x = xt(0);
    double y = xt(1);
    double t = xt(xt.Size()-1);

    gradx.SetSize(xt.Size() - 1);

    gradx(0) = exp(t) * 2.0 * (x - 0.5) * cos (((x - 0.5)*(x - 0.5) + y*y));
    gradx(1) = exp(t) * 2.0 * y         * cos (((x - 0.5)*(x - 0.5) + y*y));
    //gradx(0) = t * exp(t) * 2.0 * (x - 0.5) * cos (((x - 0.5)*(x - 0.5) + y*y)) + 5.0;
    //gradx(1) = t * exp(t) * 2.0 * y         * cos (((x - 0.5)*(x - 0.5) + y*y)) + 5.0;
}

double uFun33_ex(const Vector& xt)
{
    double x = xt(0);
    double y = xt(1);
    double z = xt(2);
    double t = xt(xt.Size()-1);

    return exp(t) * sin (((x - 0.5)*(x - 0.5) + y*y + (z -0.25)*(z-0.25) ));
}

double uFun33_ex_dt(const Vector& xt)
{
    return uFun33_ex(xt);
}

void uFun33_ex_gradx(const Vector& xt, Vector& gradx )
{
    double x = xt(0);
    double y = xt(1);
    double z = xt(2);
    double t = xt(xt.Size()-1);

    gradx.SetSize(xt.Size() - 1);

    gradx(0) = exp(t) * 2.0 * (x - 0.5) * cos (((x - 0.5)*(x - 0.5) + y*y + (z -0.25)*(z-0.25)));
    gradx(1) = exp(t) * 2.0 * y         * cos (((x - 0.5)*(x - 0.5) + y*y + (z -0.25)*(z-0.25)));
    gradx(2) = exp(t) * 2.0 * (z -0.25) * cos (((x - 0.5)*(x - 0.5) + y*y + (z -0.25)*(z-0.25)));
}

double uFun5_ex(const Vector& xt)
{
    double x = xt(0);
    double y = xt(1);
    double t = xt(xt.Size()-1);
    if ( t < MYZEROTOL)
        return exp(-100.0 * ((x - 0.5) * (x - 0.5) + y * y));
    else
        return 0.0;
}

double uFun5_ex_dt(const Vector& xt)
{
    return 0.0;
}

void uFun5_ex_gradx(const Vector& xt, Vector& gradx )
{
    double x = xt(0);
    double y = xt(1);
    //double t = xt(xt.Size()-1);

    gradx.SetSize(xt.Size() - 1);

    gradx(0) = -100.0 * 2.0 * (x - 0.5) * uFun5_ex(xt);
    gradx(1) = -100.0 * 2.0 * y * uFun5_ex(xt);
}


double uFun6_ex(const Vector& xt)
{
    double x = xt(0);
    double y = xt(1);
    double t = xt(xt.Size()-1);
    return exp(-100.0 * ((x - 0.5) * (x - 0.5) + y * y)) * exp(-10.0*t);
}

double uFun6_ex_dt(const Vector& xt)
{
    return -10.0 * uFun6_ex(xt);
}

void uFun6_ex_gradx(const Vector& xt, Vector& gradx )
{
    double x = xt(0);
    double y = xt(1);
    //double t = xt(xt.Size()-1);

    gradx.SetSize(xt.Size() - 1);

    gradx(0) = -100.0 * 2.0 * (x - 0.5) * uFun6_ex(xt);
    gradx(1) = -100.0 * 2.0 * y * uFun6_ex(xt);
}


double GaussianHill(const Vector&xvec)
{
    double x = xvec(0);
    double y = xvec(1);
    return exp(-100.0 * ((x - 0.5) * (x - 0.5) + y * y));
}

double uFunCylinder_ex(const Vector& xt)
{
    double x = xt(0);
    double y = xt(1);
    double r = sqrt(x*x + y*y);
    double teta = atan(y/x);
    /*
    if (fabs(x) < MYZEROTOL && y > 0)
        teta = M_PI / 2.0;
    else if (fabs(x) < MYZEROTOL && y < 0)
        teta = - M_PI / 2.0;
    else
        teta = atan(y,x);
    */
    double t = xt(xt.Size()-1);
    Vector xvec(2);
    xvec(0) = r * cos (teta - t);
    xvec(1) = r * sin (teta - t);
    return GaussianHill(xvec);
}

double uFunCylinder_ex_dt(const Vector& xt)
{
    return 0.0;
}

void uFunCylinder_ex_gradx(const Vector& xt, Vector& gradx )
{
    gradx.SetSize(xt.Size() - 1);

    gradx(0) = 0.0;
    gradx(1) = 0.0;
}


double uFun66_ex(const Vector& xt)
{
    double x = xt(0);
    double y = xt(1);
    double z = xt(2);
    double t = xt(xt.Size()-1);
    return exp(-100.0 * ((x - 0.5) * (x - 0.5) + y * y + (z - 0.25)*(z - 0.25))) * exp(-10.0*t);
}

double uFun66_ex_dt(const Vector& xt)
{
    return -10.0 * uFun6_ex(xt);
}

void uFun66_ex_gradx(const Vector& xt, Vector& gradx )
{
    double x = xt(0);
    double y = xt(1);
    double z = xt(2);
    //double t = xt(xt.Size()-1);

    gradx.SetSize(xt.Size() - 1);

    gradx(0) = -100.0 * 2.0 * (x - 0.5)  * uFun6_ex(xt);
    gradx(1) = -100.0 * 2.0 * y          * uFun6_ex(xt);
    gradx(2) = -100.0 * 2.0 * (z - 0.25) * uFun6_ex(xt);
}

void zerovecx_ex(const Vector& xt, Vector& zerovecx )
{
    zerovecx.SetSize(xt.Size() - 1);
    zerovecx = 0.0;
}

void zerovec_ex(const Vector& xt, Vector& vecvalue)
{
    vecvalue.SetSize(xt.Size());
    vecvalue = 0.0;
    return;
}

void zerovecMat4D_ex(const Vector& xt, Vector& vecvalue)
{
    vecvalue.SetSize(6);
    vecvalue = 0.0;
    return;
}

double zero_ex(const Vector& xt)
{
    return 0.0;
}

////////////////
void hcurlFun3D_ex(const Vector& xt, Vector& vecvalue)
{
    double x = xt(0);
    double y = xt(1);
    double t = xt(xt.Size()-1);

    vecvalue.SetSize(xt.Size());

    //vecvalue(0) = -y * (1 - t);
    //vecvalue(1) = x * (1 - t);
    //vecvalue(2) = 0;
    //vecvalue(0) = x * (1 - x);
    //vecvalue(1) = y * (1 - y);
    //vecvalue(2) = t * (1 - t);

    // Martin's function
    vecvalue(0) = sin(kappa * y);
    vecvalue(1) = sin(kappa * t);
    vecvalue(2) = sin(kappa * x);

    return;
}

void curlhcurlFun3D_ex(const Vector& xt, Vector& vecvalue)
{
    double x = xt(0);
    double y = xt(1);
    double t = xt(xt.Size()-1);

    vecvalue.SetSize(xt.Size());

    //vecvalue(0) = 0.0;
    //vecvalue(1) = 0.0;
    //vecvalue(2) = -2.0 * (1 - t);

    // Martin's function's curl
    vecvalue(0) = - kappa * cos(kappa * t);
    vecvalue(1) = - kappa * cos(kappa * x);
    vecvalue(2) = - kappa * cos(kappa * y);

    return;
}

////////////////
void DivmatFun4D_ex(const Vector& xt, Vector& vecvalue)
{
    double x = xt(0);
    double y = xt(1);
    double z = xt(2);
    double t = xt(xt.Size()-1);

    vecvalue.SetSize(xt.Size());

    // 4D counterpart of the Martin's 3D function
    //std::cout << "Error: DivmatFun4D_ex is incorrect \n";
    vecvalue(0) = sin(kappa * y);
    vecvalue(1) = sin(kappa * z);
    vecvalue(2) = sin(kappa * t);
    vecvalue(3) = sin(kappa * x);

    return;
}

void DivmatDivmatFun4D_ex(const Vector& xt, Vector& vecvalue)
{
    double x = xt(0);
    double y = xt(1);
    double z = xt(2);
    double t = xt(xt.Size()-1);

    vecvalue.SetSize(xt.Size());

    //vecvalue(0) = 0.0;
    //vecvalue(1) = 0.0;
    //vecvalue(2) = -2.0 * (1 - t);

    // Divmat of the 4D counterpart of the Martin's 3D function
    std::cout << "Error: DivmatDivmatFun4D_ex is incorrect \n";
    vecvalue(0) = - kappa * cos(kappa * t);
    vecvalue(1) = - kappa * cos(kappa * x);
    vecvalue(2) = - kappa * cos(kappa * y);
    vecvalue(3) = z;

    return;
}

////////////////
void hcurlFun3D_2_ex(const Vector& xt, Vector& vecvalue)
{
    double x = xt(0);
    double y = xt(1);
    double t = xt(xt.Size()-1);

    vecvalue.SetSize(xt.Size());

    //
    vecvalue(0) = 100.0 * x * x * (1-x) * (1-x) * y * y * (1-y) * (1-y) * t * t * (1-t) * (1-t);
    vecvalue(1) = 0.0;
    vecvalue(2) = 0.0;

    return;
}

void curlhcurlFun3D_2_ex(const Vector& xt, Vector& vecvalue)
{
    double x = xt(0);
    double y = xt(1);
    double t = xt(xt.Size()-1);

    vecvalue.SetSize(xt.Size());

    //
    vecvalue(0) = 0.0;
    vecvalue(1) = 100.0 * ( 2.0) * t * (1-t) * (1.-2.*t) * x * x * (1-x) * (1-x) * y * y * (1-y) * (1-y);
    vecvalue(2) = 100.0 * (-2.0) * y * (1-y) * (1.-2.*y) * x * x * (1-x) * (1-x) * t * t * (1-t) * (1-t);

    return;
}

template <double (*S)(const Vector & xt), void (*bvecfunc)(const Vector&, Vector& ),
          void (*opdivfreevec)(const Vector&, Vector& )> \
void minKsigmahatTemplate(const Vector& xt, Vector& minKsigmahatv)
{
    minKsigmahatv.SetSize(xt.Size());

    Vector b;
    bvecfunc(xt, b);

    Vector sigmahatv;
    sigmahatTemplate<S, bvecfunc, opdivfreevec>(xt, sigmahatv);

    DenseMatrix Ktilda;
    KtildaTemplate<bvecfunc>(xt, Ktilda);

    Ktilda.Mult(sigmahatv, minKsigmahatv);

    minKsigmahatv *= -1.0;
    return;
}

template <double (*S)(const Vector & xt), void (*bvecfunc)(const Vector&, Vector& ),
          void (*opdivfreevec)(const Vector&, Vector& )> \
double bsigmahatTemplate(const Vector& xt)
{
    Vector b;
    bvecfunc(xt, b);

    Vector sigmahatv;
    sigmahatTemplate<S, bvecfunc, opdivfreevec>(xt, sigmahatv);

    return b * sigmahatv;
}

template <double (*S)(const Vector & xt), void (*bvecfunc)(const Vector&, Vector& ),
          void (*opdivfreevec)(const Vector&, Vector& )> \
void sigmahatTemplate(const Vector& xt, Vector& sigmahatv)
{
    sigmahatv.SetSize(xt.Size());

    Vector b;
    bvecfunc(xt, b);

    Vector sigma(xt.Size());
    sigma(xt.Size()-1) = S(xt);
    for (int i = 0; i < xt.Size()-1; i++)
        sigma(i) = b(i) * sigma(xt.Size()-1);

    Vector opdivfree;
    opdivfreevec(xt, opdivfree);

    sigmahatv = 0.0;
    sigmahatv -= opdivfree;
#ifndef ONLY_DIVFREEPART
    sigmahatv += sigma;
#endif
    return;
}

template <double (*S)(const Vector & xt), void (*bvecfunc)(const Vector&, Vector& ),
          void (*opdivfreevec)(const Vector&, Vector& )> \
void minsigmahatTemplate(const Vector& xt, Vector& minsigmahatv)
{
    minsigmahatv.SetSize(xt.Size());
    sigmahatTemplate<S, bvecfunc, opdivfreevec>(xt, minsigmahatv);
    minsigmahatv *= -1;

    return;
}

void E_exact(const Vector &xt, Vector &E)
{
    if (xt.Size() == 3)
    {

        E(0) = sin(kappa * xt(1));
        E(1) = sin(kappa * xt(2));
        E(2) = sin(kappa * xt(0));
#ifdef BAD_TEST
        double x = xt(0);
        double y = xt(1);
        double t = xt(2);

        E(0) = x * x * (1-x) * (1-x) * y * y * (1-y) * (1-y) * t * t * (1-t) * (1-t);
        E(1) = 0.0;
        E(2) = 0.0;
#endif
    }
}


void curlE_exact(const Vector &xt, Vector &curlE)
{
    if (xt.Size() == 3)
    {
        curlE(0) = - kappa * cos(kappa * xt(2));
        curlE(1) = - kappa * cos(kappa * xt(0));
        curlE(2) = - kappa * cos(kappa * xt(1));
#ifdef BAD_TEST
        double x = xt(0);
        double y = xt(1);
        double t = xt(2);

        curlE(0) = 0.0;
        curlE(1) =  2.0 * t * (1-t) * (1.-2.*t) * x * x * (1-x) * (1-x) * y * y * (1-y) * (1-y);
        curlE(2) = -2.0 * y * (1-y) * (1.-2.*y) * x * x * (1-x) * (1-x) * t * t * (1-t) * (1-t);
#endif
    }
}


void vminusone_exact(const Vector &x, Vector &vminusone)
{
    vminusone.SetSize(x.Size());
    vminusone = -1.0;
}

void vone_exact(const Vector &x, Vector &vone)
{
    vone.SetSize(x.Size());
    vone = 1.0;
}


void f_exact(const Vector &xt, Vector &f)
{
    if (xt.Size() == 3)
    {


        //f(0) = sin(kappa * x(1));
        //f(1) = sin(kappa * x(2));
        //f(2) = sin(kappa * x(0));
        //f(0) = (1. + kappa * kappa) * sin(kappa * x(1));
        //f(1) = (1. + kappa * kappa) * sin(kappa * x(2));
        //f(2) = (1. + kappa * kappa) * sin(kappa * x(0));

        f(0) = kappa * kappa * sin(kappa * xt(1));
        f(1) = kappa * kappa * sin(kappa * xt(2));
        f(2) = kappa * kappa * sin(kappa * xt(0));

        /*

       double x = xt(0);
       double y = xt(1);
       double t = xt(2);

       f(0) =  -1.0 * (2 * (1-y)*(1-y) + 2*y*y - 2.0 * 2 * y * 2 * (1-y)) * x * x * (1-x) * (1-x) * t * t * (1-t) * (1-t);
       f(0) += -1.0 * (2 * (1-t)*(1-t) + 2*t*t - 2.0 * 2 * t * 2 * (1-t)) * x * x * (1-x) * (1-x) * y * y * (1-y) * (1-y);
       f(1) = 2.0 * y * (1-y) * (1-2*y) * 2.0 * x * (1-x) * (1-2*x) * t * t * (1-t) * (1-t);
       f(2) = 2.0 * t * (1-t) * (1-2*t) * 2.0 * x * (1-x) * (1-2*x) * y * y * (1-y) * (1-y);
       */


    }
}


void E_exactMat_vec(const Vector &x, Vector &E)
{
    int dim = x.Size();

    if (dim==4)
    {
        E.SetSize(6);

        double s0 = sin(M_PI*x(0)), s1 = sin(M_PI*x(1)), s2 = sin(M_PI*x(2)),
                s3 = sin(M_PI*x(3));
        double c0 = cos(M_PI*x(0)), c1 = cos(M_PI*x(1)), c2 = cos(M_PI*x(2)),
                c3 = cos(M_PI*x(3));

        E(0) =  c0*c1*s2*s3;
        E(1) = -c0*s1*c2*s3;
        E(2) =  c0*s1*s2*c3;
        E(3) =  s0*c1*c2*s3;
        E(4) = -s0*c1*s2*c3;
        E(5) =  s0*s1*c2*c3;
    }
}

void E_exactMat(const Vector &x, DenseMatrix &E)
{
    int dim = x.Size();

    E.SetSize(dim*dim);

    if (dim==4)
    {
        Vector vecE;
        E_exactMat_vec(x, vecE);

        E = 0.0;

        E(0,1) = vecE(0);
        E(0,2) = vecE(1);
        E(0,3) = vecE(2);
        E(1,2) = vecE(3);
        E(1,3) = vecE(4);
        E(2,3) = vecE(5);

        E(1,0) =  -E(0,1);
        E(2,0) =  -E(0,2);
        E(3,0) =  -E(0,3);
        E(2,1) =  -E(1,2);
        E(3,1) =  -E(1,3);
        E(3,2) =  -E(2,3);
    }
}



//f_exact = E + 0.5 * P( curl DivSkew E ), where P is the 4d permutation operator
void f_exactMat(const Vector &x, DenseMatrix &f)
{
    int dim = x.Size();

    f.SetSize(dim,dim);

    if (dim==4)
    {
        f = 0.0;

        double s0 = sin(M_PI*x(0)), s1 = sin(M_PI*x(1)), s2 = sin(M_PI*x(2)),
                s3 = sin(M_PI*x(3));
        double c0 = cos(M_PI*x(0)), c1 = cos(M_PI*x(1)), c2 = cos(M_PI*x(2)),
                c3 = cos(M_PI*x(3));

        f(0,1) =  (1.0 + 1.0  * M_PI*M_PI)*c0*c1*s2*s3;
        f(0,2) = -(1.0 + 0.0  * M_PI*M_PI)*c0*s1*c2*s3;
        f(0,3) =  (1.0 + 1.0  * M_PI*M_PI)*c0*s1*s2*c3;
        f(1,2) =  (1.0 - 1.0  * M_PI*M_PI)*s0*c1*c2*s3;
        f(1,3) = -(1.0 + 0.0  * M_PI*M_PI)*s0*c1*s2*c3;
        f(2,3) =  (1.0 + 1.0  * M_PI*M_PI)*s0*s1*c2*c3;

        f(1,0) =  -f(0,1);
        f(2,0) =  -f(0,2);
        f(3,0) =  -f(0,3);
        f(2,1) =  -f(1,2);
        f(3,1) =  -f(1,3);
        f(3,2) =  -f(2,3);
    }
}

double uFun1_ex(const Vector& xt)
{
    double t = xt(xt.Size()-1);
    //return t;
    ////setback
    return sin(t)*exp(t);
}

double uFun1_ex_dt(const Vector& xt)
{
    double t = xt(xt.Size()-1);
    return (cos(t) + sin(t)) * exp(t);
}

void uFun1_ex_gradx(const Vector& xt, Vector& gradx )
{
    gradx.SetSize(xt.Size() - 1);
    gradx = 0.0;
}
