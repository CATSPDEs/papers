﻿#include "FEMt.hpp"
#include "SymmetricCSlRMatrix.hpp" // for final linear system matrix
#include "krylov.hpp" // conjugate gradients
#include "array.hpp" // utility for array operations

vector<vector<double>> FEMt::CN3(HyperbolicPDE const & PDE,
                                 InitialBoundaryConditions& IBCs,
                                 vector<double> const & t, // vector of time frames
                                 Triangulation& Omega,
	                             TimeFunction u) { // exact soln (if we are dealing w/ model problem)
	if (t.size() < 2) throw invalid_argument("CN3() needs at least 2 time frames to compute soln");
	// data structures for final linear system A.xi[m] = b:
	SymmetricCSlRMatrix A(Omega.generateAdjList()); // build final matrix portrait
	vector<double> b(Omega.numbOfNodes(), 0.); // load vector
	vector<vector<double>> xi(t.size(), b); // discrete solution---$xi_{m\,i}$ is our solution at time = $t_m$ and at $node_i$
	// data structures for assemby of A and b:
	SymmetricContainer<double> localMassMatrixChi(3), localMassMatrixSigma(3), // for hat functions on triangles 
	                           localStiffnessMatrix(3), // we have 3 $\times$ 3 element matricies
	                           localRobinMatrix_m(2), // and 2 $\times$ 2 element matricies for Robin BCs (just like element matrix in 1D)
	                           localRobinMatrix_m_2(2); // computed on $t_m$ and $t_{m-2}$---for CN3--scheme
	array<double, 3> localLoadVector_m, localLoadVector_m_2; // and their
	array<double, 2> localRobinVector_m, localRobinVector_m_2; // friends, element vectors
	array<Node, 3> elementNodes, // nodes of the current triangle
	               elementMiddleNodes; // and nodes on the middle of edges
	array<Node, 2> edgeNodes; // nodes spanning an edge of the current triangle that is part of bndry
	double measure; // area of ith triangle / length of bndry edge of ith thiangle
	array<size_t, 3> l2g_elem; // local to global mapping of nodes on the element
	array<size_t, 2> l2g_edge; // and on the edge
	localIndex j, k, leftNodeIndex, rightNodeIndex; // dummy indicies
	array<array<double, 2>, 3> eta; // time-basis
	// stencil:
	// $t_{-2}-----t_{-1}---t_0$
	// $eta_i$ = unity at $t_{-i}$, zero elsewhere, $eta_i = a t^2 + b t + c$
	// $eta{m\,i}$ means (j+1)'s derivative of $eta_i$ computed at $t_0$
	// we will need these derivatives to approximate differential operators
	// (I) apply initial conditions to get $xi_0$ and $xi_1$
	for (size_t i = 0; i < Omega.numbOfNodes(); ++i) 
		xi[0][i] = IBCs.initialPosition(Omega.getNode(i));
	if (u == emptyTimeFunc) for (size_t i = 0; i < Omega.numbOfNodes(); ++i)
		// normally, we have to use initial velocity to approximate $xi_1$ \dots
		xi[1][i] = xi[0][i] + IBCs.initialVelocity(Omega.getNode(i)) * (t[1] - t[0]);
	else for (size_t i = 0; i < Omega.numbOfNodes(); ++i)
		// \dots but for model problems we have exact soln, so
		xi[1][i] = u(Omega.getNode(i), t[1]);
	// (II) solve for $xi_m$, m = 2, 3, \dots w/ Crank--Nicolson scheme
	for (size_t m = 2; m < t.size(); ++m) {
		// compute derivatives
		eta[2][0] = (t[m] - t[m - 1])                 / (t[m - 2] - t[m - 1]) / (t[m - 2] - t[m]);
		eta[2][1] = 2.                                / (t[m - 2] - t[m - 1]) / (t[m - 2] - t[m]);
		eta[1][0] = (t[m - 2] - t[m])                 / (t[m - 2] - t[m - 1]) / (t[m - 1] - t[m]);
		eta[1][1] = - 2.                              / (t[m - 2] - t[m - 1]) / (t[m - 1] - t[m]);
		eta[0][0] = (2. * t[m] - t[m - 2] - t[m - 1]) / (t[m - 2] - t[m]) / (t[m - 1] - t[m]);
		eta[0][1] = 2.                                / (t[m - 2] - t[m]) / (t[m - 1] - t[m]);
		// assemble SLDE
		for (size_t i = 0; i < Omega.numbOfTriangles(); ++i) {
			// (1) quadratures over elements
			// in order to assemble A and b,
			// it is convenient to iterate over mesh elements (i.e. triangles)
			elementNodes = Omega.getNodes(i); // get nodes of ith triangle
			for (j = 0; j < 3; ++j) // and middle nodes of its edges
				elementMiddleNodes[j] = elementNodes[k = nextIndex(j)].midPoint(elementNodes[nextIndex(k)]);
			measure = Omega.area(i); // compute area of ith triangle
			l2g_elem = Omega.l2g(i); // local to global mapping of nodes of ith element
			// local matrices and vectors
			localMassMatrixChi = computeLocalMassMatrix(PDE.chi(), elementNodes, measure);
			localMassMatrixSigma = computeLocalMassMatrix(PDE.sigma(), elementNodes, measure);
			localStiffnessMatrix = computeLocalStiffnessMatrix(PDE.diffusionTerm(), elementNodes, elementMiddleNodes, measure);
			localLoadVector_m = computeLocalLoadVector(PDE.forceTerm(), t[m], elementNodes, elementMiddleNodes, measure);
			localLoadVector_m_2 = computeLocalLoadVector(PDE.forceTerm(), t[m - 2], elementNodes, elementMiddleNodes, measure);
			// assemble contributions
			for (j = 0; j < 3; ++j) {
				for (k = 0; k < 3; ++k) {
					if (k >= j) // symmetric
						A(l2g_elem[j], l2g_elem[k]) +=
							eta[0][0] * localMassMatrixSigma(j, k) +
							eta[0][1] * localMassMatrixChi(j, k) +
							localStiffnessMatrix(j, k) / 2.;
					b[l2g_elem[j]] -= (
						localStiffnessMatrix(j, k) * xi[m - 2][l2g_elem[k]] / 2. +
						eta[1][0] * localMassMatrixSigma(j, k) * xi[m - 1][l2g_elem[k]] + // parabolic
						eta[2][0] * localMassMatrixSigma(j, k) * xi[m - 2][l2g_elem[k]] +
						eta[1][1] * localMassMatrixChi(j, k)   * xi[m - 1][l2g_elem[k]] + // hyperbolic
						eta[2][1] * localMassMatrixChi(j, k)   * xi[m - 2][l2g_elem[k]]
					);
				}
				b[l2g_elem[j]] += (localLoadVector_m[j] + localLoadVector_m_2[j]) / 2.;
			}
			// (2) quadratures over edges
			// iterate over list of local indicies of boundary nodes
			for (localIndex edgeIndex : Omega.getBoundaryIndicies(i)) {
				// if edgeIndex = 2, then the edge against second node of ith triangle
				// is part of the boundary
				// so we need to assemble BCs here
				leftNodeIndex = nextIndex(edgeIndex); // local indicies of nodes that
				rightNodeIndex = nextIndex(leftNodeIndex); // define the edge
				edgeNodes = { elementNodes[leftNodeIndex], elementNodes[rightNodeIndex] }; // and the nodes themselves
				l2g_edge[0] = l2g_elem[leftNodeIndex]; // local to global nodes
				l2g_edge[1] = l2g_elem[rightNodeIndex]; // numeration mapping 
				measure = Omega.length(i, edgeIndex);
				// compute
				// (2.1) local Robin matrix
				// (2.2) local Robin vector
				localRobinMatrix_m = FEMt::computeLocalRobinMatrix(IBCs, t[m], edgeNodes, measure);
				localRobinMatrix_m_2 = FEMt::computeLocalRobinMatrix(IBCs, t[m - 2], edgeNodes, measure);
				localRobinVector_m = FEMt::computeLocalRobinVector(IBCs, t[m], edgeNodes, measure);
				localRobinVector_m_2 = FEMt::computeLocalRobinVector(IBCs, t[m - 2], edgeNodes, measure);
				// (2.3) assemble contributions
				for (j = 0; j < 2; ++j) {
					for (k = 0; k < 2; ++k) {
						if (k >= j) // symmetric
							A(l2g_edge[j], l2g_edge[k]) += localRobinMatrix_m(j, k) / 2.;
						b[l2g_edge[j]] -= localRobinMatrix_m_2(j, k) * xi[m - 2][l2g_edge[k]] / 2.;
					}
					b[l2g_edge[j]] += (localRobinVector_m[j] + localRobinVector_m_2[j]) / 2.;
				}
			}
		}
		// now we are ready to compute xi[m], A.xi[m] = b
		xi[m] = CG(A, b, xi[m - 1], 10e-70);
		// clear A and b 
		A.setZero();
		fill(b.begin(), b.end(), 0.);
	}
	return xi;
}

vector<vector<double>> FEMt::BDF3(HyperbolicPDE const & PDE,
	                              InitialBoundaryConditions& IBCs,
	                              vector<double> const & t, // vector of time frames
	                              Triangulation& Omega,
	                              TimeFunction u) { // exact soln (if we are dealing w/ model problem)
	// . . .
}


SymmetricContainer<double> FEMt::computeLocalMassMatrix(Function reactionTerm,
                                                        array<Node, 3>& nodes,
                                                        double area) {
	SymmetricContainer<double> m(3);
	m(0, 0) = area * (6. * reactionTerm(nodes[0]) + 2. * reactionTerm(nodes[1]) + 2. * reactionTerm(nodes[2])) / 60.;
	m(0, 1) = area * (2. * reactionTerm(nodes[0]) + 2. * reactionTerm(nodes[1]) + reactionTerm(nodes[2])) / 60.;
	m(0, 2) = area * (2. * reactionTerm(nodes[0]) + reactionTerm(nodes[1]) + 2. * reactionTerm(nodes[2])) / 60.;
	m(1, 1) = area * (2. * reactionTerm(nodes[0]) + 6. * reactionTerm(nodes[1]) + 2. * reactionTerm(nodes[2])) / 60.;
	m(1, 2) = area * (reactionTerm(nodes[0]) + 2. * reactionTerm(nodes[1]) + 2. * reactionTerm(nodes[2])) / 60.;
	m(2, 2) = area * (2. * reactionTerm(nodes[0]) + 2. * reactionTerm(nodes[1]) + 6. * reactionTerm(nodes[2])) / 60.;
	return m;
}

SymmetricContainer<double> FEMt::computeLocalStiffnessMatrix(Function diffusionTerm,
                                                             array<Node, 3>& nodes,
                                                             array<Node, 3>& middleNodes,
                                                             double area) {
	SymmetricContainer<double> s(3);
	s(0, 0) = (nodes[1].x() - nodes[2].x()) * (nodes[1].x() - nodes[2].x()) +
		(nodes[1].y() - nodes[2].y()) * (nodes[1].y() - nodes[2].y());
	s(0, 1) = (nodes[0].x() - nodes[2].x()) * (nodes[2].x() - nodes[1].x()) +
		(nodes[0].y() - nodes[2].y()) * (nodes[2].y() - nodes[1].y());
	s(0, 2) = (nodes[0].x() - nodes[1].x()) * (nodes[1].x() - nodes[2].x()) +
		(nodes[0].y() - nodes[1].y()) * (nodes[1].y() - nodes[2].y());
	s(1, 1) = (nodes[0].x() - nodes[2].x()) * (nodes[0].x() - nodes[2].x()) +
		(nodes[0].y() - nodes[2].y()) * (nodes[0].y() - nodes[2].y());
	s(1, 2) = (nodes[1].x() - nodes[0].x()) * (nodes[0].x() - nodes[2].x()) +
		(nodes[1].y() - nodes[0].y()) * (nodes[0].y() - nodes[2].y());
	s(2, 2) = (nodes[0].x() - nodes[1].x()) * (nodes[0].x() - nodes[1].x()) +
		(nodes[0].y() - nodes[1].y()) * (nodes[0].y() - nodes[1].y());
	for (localIndex i = 0; i < 3; ++i)
		for (localIndex j = i; j < 3; ++j)
			s(i, j) *= (diffusionTerm(middleNodes[0]) + diffusionTerm(middleNodes[1]) + diffusionTerm(middleNodes[2])) / area / 12.;
	return s;
}

array<double, 3> FEMt::computeLocalLoadVector(TimeFunction forceTerm,
                                              double t,
                                              array<Node, 3>& nodes,
                                              array<Node, 3>& middleNodes,
                                              double area) {
	array<double, 3> f;
	return (f = {
		2. * forceTerm(nodes[0], t) - forceTerm(nodes[1], t) - forceTerm(nodes[2], t) +
		4. * forceTerm(middleNodes[0], t) + 8. * (forceTerm(middleNodes[1], t) + forceTerm(middleNodes[2], t)),
		2. * forceTerm(nodes[1], t) - forceTerm(nodes[0], t) - forceTerm(nodes[2], t) +
		4. * (2. * (forceTerm(middleNodes[0], t) + forceTerm(middleNodes[2], t)) + forceTerm(middleNodes[1], t)),
		2. * (forceTerm(nodes[2], t) + 4. * (forceTerm(middleNodes[0], t) + forceTerm(middleNodes[1], t)) + 2. * forceTerm(middleNodes[2], t)) -
		forceTerm(nodes[0], t) - forceTerm(nodes[1], t)
	}) *= area / 60.;
}


SymmetricContainer<double> FEMt::computeLocalRobinMatrix(InitialBoundaryConditions& IBCs,
                                                         double t,
                                                         array<Node, 2>& nodes,
	                                                     double length) {
	IBCs.computeBoundaryType(nodes);
	double RobinCoefficientLeft  = IBCs.RobinCoefficient(nodes[0], t),
		   RobinCoefficientRight = IBCs.RobinCoefficient(nodes[1], t);
	SymmetricContainer<double> r(2);
	r(0, 0) = length * (3. * RobinCoefficientLeft + RobinCoefficientRight) / 12.;
	r(0, 1) = length * (RobinCoefficientLeft + RobinCoefficientRight) / 12.;
	r(1, 1) = length * (RobinCoefficientLeft + 3. * RobinCoefficientRight) / 12.;
	return r;
}

array<double, 2> FEMt::computeLocalRobinVector(InitialBoundaryConditions & IBCs,
                                               double t,
                                               array<Node, 2>& nodes,
                                               double length) {
	IBCs.computeBoundaryType(nodes);
	double RobinCoefficientLeft    = IBCs.RobinCoefficient(nodes[0], t),
		   RobinCoefficientRight   = IBCs.RobinCoefficient(nodes[1], t),
		   DirichletConditionLeft  = IBCs.DirichletCondition(nodes[0], t),
		   DirichletConditionRight = IBCs.DirichletCondition(nodes[1], t);
	array<double, 2> r;
	return (r = {
		4. * IBCs.NeumannValue(nodes[0], t) + 2. * IBCs.NeumannValue(nodes[1], t) +
		DirichletConditionLeft * (3. * RobinCoefficientLeft + RobinCoefficientRight) +
		DirichletConditionRight * (RobinCoefficientLeft + RobinCoefficientRight),
		2. * IBCs.NeumannValue(nodes[0], t) + 4. * IBCs.NeumannValue(nodes[1], t) +
		DirichletConditionLeft * (RobinCoefficientLeft + RobinCoefficientRight) +
		DirichletConditionRight * (RobinCoefficientLeft + 3. * RobinCoefficientRight)
	}) *= length / 12.;
}