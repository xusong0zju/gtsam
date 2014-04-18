/*
 * QPSolver.cpp
 * @brief:
 * @date: Apr 15, 2014
 * @author: thduynguyen
 */

#include <boost/foreach.hpp>
#include <gtsam/linear/QPSolver.h>

using namespace std;

namespace gtsam {

/* ************************************************************************* */
QPSolver::QPSolver(const GaussianFactorGraph& graph) :
          graph_(graph), fullFactorIndices_(graph) {

  // Split the original graph into unconstrained and constrained part
  // and collect indices of constrained factors
  for (size_t i = 0; i < graph.nrFactors(); ++i) {
    // obtain the factor and its noise model
    JacobianFactor::shared_ptr jacobian = toJacobian(graph.at(i));
    if (jacobian && jacobian->get_model()
        && jacobian->get_model()->isConstrained()) {
      constraintIndices_.push_back(i);
    }
  }

  // Collect constrained variable keys
  KeySet constrainedVars;
  BOOST_FOREACH(size_t index, constraintIndices_) {
    KeyVector keys = graph[index]->keys();
    constrainedVars.insert(keys.begin(), keys.end());
  }

  // Collect unconstrained hessians of constrained vars to build dual graph
  freeHessians_ = unconstrainedHessiansOfConstrainedVars(graph, constrainedVars);
  freeHessianFactorIndex_ = VariableIndex(*freeHessians_);
}


/* ************************************************************************* */
GaussianFactorGraph::shared_ptr QPSolver::unconstrainedHessiansOfConstrainedVars(
    const GaussianFactorGraph& graph, const KeySet& constrainedVars) const {
  VariableIndex variableIndex(graph);
  GaussianFactorGraph::shared_ptr hfg = boost::make_shared<GaussianFactorGraph>();

  // Collect all factors involving constrained vars
  FastSet<size_t> factors;
  BOOST_FOREACH(Key key, constrainedVars) {
    VariableIndex::Factors factorsOfThisVar = variableIndex[key];
    BOOST_FOREACH(size_t factorIndex, factorsOfThisVar) {
      factors.insert(factorIndex);
    }
  }

  // Convert each factor into Hessian
  BOOST_FOREACH(size_t factorIndex, factors) {
    if (!graph[factorIndex]) continue;
    // See if this is a Jacobian factor
    JacobianFactor::shared_ptr jf = toJacobian(graph[factorIndex]);
    if (jf) {
      // Dealing with mixed constrained factor
      if (jf->get_model() && jf->isConstrained()) {
        // Turn a mixed-constrained factor into a factor with 0 information on the constrained part
        Vector sigmas = jf->get_model()->sigmas();
        Vector newPrecisions(sigmas.size());
        bool mixed = false;
        for (size_t s=0; s<sigmas.size(); ++s) {
          if (sigmas[s] <= 1e-9) newPrecisions[s] = 0.0; // 0 info for constraints (both ineq and eq)
          else {
            newPrecisions[s] = 1.0/sigmas[s];
            mixed = true;
          }
        }
        if (mixed) {  // only add free hessians if it's mixed
          JacobianFactor::shared_ptr newJacobian = toJacobian(jf->clone());
          newJacobian->setModel(noiseModel::Diagonal::Precisions(newPrecisions));
          hfg->push_back(HessianFactor(*newJacobian));
        }
      }
      else {  // unconstrained Jacobian
        // Convert the original linear factor to Hessian factor
        hfg->push_back(HessianFactor(*graph[factorIndex]));
      }
    }
    else { // If it's not a Jacobian, it should be a hessian factor. Just add!
      hfg->push_back(graph[factorIndex]);
    }

  }
  return hfg;
}

/* ************************************************************************* */
GaussianFactorGraph QPSolver::buildDualGraph(const GaussianFactorGraph& graph,
    const VectorValues& x0, bool useLeastSquare) const {
  static const bool debug = false;

  // The dual graph to return
  GaussianFactorGraph dualGraph;

  // For each variable xi involving in some constraint, compute the unconstrained gradient
  // wrt xi from the prebuilt freeHessian graph
  // \grad f(xi) = \frac{\partial f}{\partial xi}' = \sum_j G_ij*xj - gi
  BOOST_FOREACH(const VariableIndex::value_type& xiKey_factors, freeHessianFactorIndex_) {
    Key xiKey = xiKey_factors.first;
    VariableIndex::Factors xiFactors = xiKey_factors.second;

    // Find xi's dim from the first factor on xi
    if (xiFactors.size() == 0) continue;
    GaussianFactor::shared_ptr xiFactor0 = freeHessians_->at(0);
    size_t xiDim = xiFactor0->getDim(xiFactor0->find(xiKey));
    if (debug) cout << "xiDim: " << xiDim << endl;

    //++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
    // Compute the b-vector for the dual factor Ax-b
    // b = gradf(xi) = \frac{\partial f}{\partial xi}' = \sum_j G_ij*xj - gi
    Vector gradf_xi = zero(xiDim);
    BOOST_FOREACH(size_t factorIx, xiFactors) {
      HessianFactor::shared_ptr factor = toHessian(freeHessians_->at(factorIx));
      Factor::const_iterator xi = factor->find(xiKey);
      // Sum over Gij*xj for all xj connecting to xi
      for (Factor::const_iterator xj = factor->begin(); xj != factor->end();
          ++xj) {
        // Obtain Gij from the Hessian factor
        // Hessian factor only stores an upper triangular matrix, so be careful when i>j
        Matrix Gij;
        if (xi > xj) {
          Matrix Gji = factor->info(xj, xi);
          Gij = Gji.transpose();
        }
        else {
          Gij = factor->info(xi, xj);
        }
        // Accumulate Gij*xj to gradf
        Vector x0_j = x0.at(*xj);
        gradf_xi += Gij * x0_j;
      }
      // Subtract the linear term gi
      gradf_xi += -factor->linearTerm(xi);
    }

    //++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
    // Compute the Jacobian A for the dual factor Ax-b
    // Obtain the jacobians for lambda variables from their corresponding constraints
    // A = gradc_k(xi) = \frac{\partial c_k}{\partial xi}'
    std::vector<std::pair<Key, Matrix> > lambdaTerms; // collection of lambda_k, and gradc_k
    typedef std::pair<size_t, size_t> FactorIx_SigmaIx;
    std::vector<FactorIx_SigmaIx> unconstrainedIndex; // pairs of factorIx,sigmaIx of unconstrained rows
    BOOST_FOREACH(size_t factorIndex, fullFactorIndices_[xiKey]) {
      JacobianFactor::shared_ptr factor = toJacobian(graph.at(factorIndex));
      if (!factor || !factor->isConstrained()) continue;
      // Gradient is the transpose of the Jacobian: A_k = gradc_k(xi) = \frac{\partial c_k}{\partial xi}'
      // Each column for each lambda_k corresponds to [the transpose of] each constrained row factor
      Matrix A_k = factor->getA(factor->find(xiKey)).transpose();
      if (debug) gtsam::print(A_k, "A_k = ");
      // Deal with mixed sigmas: no information if sigma != 0
      Vector sigmas = factor->get_model()->sigmas();
      for (size_t sigmaIx = 0; sigmaIx<sigmas.size(); ++sigmaIx) {
        // if it's either ineq (sigma<0) or unconstrained (sigma>0)
        // we have no information about it
        if (fabs(sigmas[sigmaIx]) > 1e-9) {
          A_k.col(sigmaIx) = zero(A_k.rows());
          // remember to add a zero prior on this lambda, otherwise the graph is under-determined
          unconstrainedIndex.push_back(make_pair(factorIndex, sigmaIx));
        }
      }
      // Use factorIndex as the lambda's key.
      lambdaTerms.push_back(make_pair(factorIndex, A_k));
    }

    //++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
    // Create and add factors to the dual graph
    // If least square approximation is desired, use unit noise model.
    if (useLeastSquare) {
      dualGraph.push_back(JacobianFactor(lambdaTerms, gradf_xi,
          noiseModel::Unit::Create(gradf_xi.size())));
    }
    else {
      // Enforce constrained noise model so lambdas are solved with QR
      // and should exactly satisfy all the equations
      dualGraph.push_back(JacobianFactor(lambdaTerms, gradf_xi,
          noiseModel::Constrained::All(gradf_xi.size())));
    }

    // Add 0 priors on all lambdas of the unconstrained rows to make sure the graph is solvable
    BOOST_FOREACH(FactorIx_SigmaIx factorIx_sigmaIx, unconstrainedIndex) {
      size_t factorIx = factorIx_sigmaIx.first;
      JacobianFactor::shared_ptr factor = toJacobian(graph.at(factorIx));
      size_t dim= factor->get_model()->dim();
      Matrix J = zeros(dim, dim);
      size_t sigmaIx = factorIx_sigmaIx.second;
      J(sigmaIx,sigmaIx) = 1.0;
      // Use factorIndex as the lambda's key.
      dualGraph.push_back(JacobianFactor(factorIx, J, zero(dim)));
    }
  }

  return dualGraph;
}

/* ************************************************************************* */
std::pair<int, int> QPSolver::findWorstViolatedActiveIneq(const VectorValues& lambdas) const {
  int worstFactorIx = -1, worstSigmaIx = -1;
  // preset the maxLambda to 0.0: if lambda is <= 0.0, the constraint is either
  // inactive or a good ineq constraint, so we don't care!
  double maxLambda = 0.0;
  BOOST_FOREACH(size_t factorIx, constraintIndices_) {
    Vector lambda = lambdas.at(factorIx);
    Vector orgSigmas = toJacobian(graph_.at(factorIx))->get_model()->sigmas();
    for (size_t j = 0; j<orgSigmas.size(); ++j)
      // If it is a BAD active inequality, and lambda is larger than the current max
      if (orgSigmas[j]<0 && lambda[j] > maxLambda) {
        worstFactorIx = factorIx;
        worstSigmaIx = j;
        maxLambda = lambda[j];
      }
  }
  return make_pair(worstFactorIx, worstSigmaIx);
}

/* ************************************************************************* */
bool QPSolver::updateWorkingSetInplace(GaussianFactorGraph& workingGraph,
    int factorIx, int sigmaIx, double newSigma) const {
  if (factorIx < 0 || sigmaIx < 0)
    return false;
  Vector sigmas = toJacobian(workingGraph.at(factorIx))->get_model()->sigmas();
  sigmas[sigmaIx] = newSigma; // removing it from the working set
  toJacobian(workingGraph.at(factorIx))->setModel(true, sigmas);
  return true;
}

/* ************************************************************************* */
boost::tuple<double, int, int> QPSolver::computeStepSize(const GaussianFactorGraph& workingGraph,
    const VectorValues& xk, const VectorValues& p) const {
  static bool debug = false;

  double minAlpha = 1.0;
  int closestFactorIx = -1, closestSigmaIx = -1;
  BOOST_FOREACH(size_t factorIx, constraintIndices_) {
    JacobianFactor::shared_ptr jacobian = toJacobian(workingGraph.at(factorIx));
    Vector sigmas = jacobian->get_model()->sigmas();
    Vector b = jacobian->getb();
    for (size_t s = 0; s<sigmas.size(); ++s) {
      // If it is an inactive inequality, compute alpha and update min
      if (sigmas[s]<0) {
        // Compute aj'*p
        double ajTp = 0.0;
        for (Factor::const_iterator xj = jacobian->begin(); xj != jacobian->end(); ++xj) {
          Vector pj = p.at(*xj);
          Vector aj = jacobian->getA(xj).row(s);
          ajTp += aj.dot(pj);
        }
        if (debug) cout << "s, ajTp: " << s << " " << ajTp << endl;

        // Check if  aj'*p >0. Don't care if it's not.
        if (ajTp<=0) continue;

        // Compute aj'*xk
        double ajTx = 0.0;
        for (Factor::const_iterator xj = jacobian->begin(); xj != jacobian->end(); ++xj) {
          Vector xkj = xk.at(*xj);
          Vector aj = jacobian->getA(xj).row(s);
          ajTx += aj.dot(xkj);
        }
        if (debug) cout << "b[s], ajTx: " << b[s] << " " << ajTx << " " << ajTp << endl;

        // alpha = (bj - aj'*xk) / (aj'*p)
        double alpha = (b[s] - ajTx)/ajTp;
        if (debug) cout << "alpha: " << alpha << endl;

        // We want the minimum of all those max alphas
        if (alpha < minAlpha) {
          closestFactorIx = factorIx;
          closestSigmaIx = s;
          minAlpha = alpha;
        }
      }
    }
  }
  return boost::make_tuple(minAlpha, closestFactorIx, closestSigmaIx);
}

/* ************************************************************************* */
bool QPSolver::iterateInPlace(GaussianFactorGraph& workingGraph, VectorValues& currentSolution) const {
  static bool debug = false;
  if (debug) workingGraph.print("workingGraph: ");
  // Obtain the solution from the current working graph
  VectorValues newSolution = workingGraph.optimize();
  if (debug) newSolution.print("New solution:");

  // If we CAN'T move further
  if (newSolution.equals(currentSolution, 1e-5)) {
    // Compute lambda from the dual graph
    if (debug) cout << "Building dual graph..." << endl;
    GaussianFactorGraph dualGraph = buildDualGraph(workingGraph, newSolution);
    if (debug) dualGraph.print("Dual graph: ");
    VectorValues lambdas = dualGraph.optimize();
    if (debug) lambdas.print("lambdas :");

    int factorIx, sigmaIx;
    boost::tie(factorIx, sigmaIx) = findWorstViolatedActiveIneq(lambdas);

    // Try to disactivate the weakest violated ineq constraints
    // if not successful, i.e. all ineq constraints are satisfied: We have the solution!!
    if (!updateWorkingSetInplace(workingGraph, factorIx, sigmaIx, -1.0))
      return true;
  }
  else {
    // If we CAN make some progress
    // Adapt stepsize if some inactive inequality constraints complain about this move
    if (debug) cout << "Computing stepsize..." << endl;
    double alpha;
    int factorIx, sigmaIx;
    VectorValues p = newSolution - currentSolution;
    boost::tie(alpha, factorIx, sigmaIx) = computeStepSize(workingGraph, currentSolution, p);
    if (debug) cout << "alpha, factorIx, sigmaIx: " << alpha << " " << factorIx << " " << sigmaIx << endl;
    // also add to the working set the one that complains the most
    updateWorkingSetInplace(workingGraph, factorIx, sigmaIx, 0.0);
    // step!
    currentSolution = currentSolution + alpha * p;
  }

  return false;
}

/* ************************************************************************* */
VectorValues QPSolver::optimize(const VectorValues& initials) const {
  GaussianFactorGraph workingGraph = graph_.clone();
  VectorValues currentSolution = initials;
  bool converged = false;
  while (!converged) {
    converged = iterateInPlace(workingGraph, currentSolution);
  }
  return currentSolution;
}


} /* namespace gtsam */