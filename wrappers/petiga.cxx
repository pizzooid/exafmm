#include "base_mpi.h"
#include "args.h"
#include "bound_box.h"
#include "build_tree.h"
#include "logger.h"
#include "partition.h"
#include "traversal.h"
#include "tree_mpi.h"
#include "up_down_pass.h"
#include "kernel_select.h"
using namespace exafmm;
vec3 TemplateKernel::Xperiodic = 0;
double TemplateKernel::eps2 = 0.0;
#if EXAFMM_HELMHOLTZ
complex_t TemplateKernel::wavek = complex_t(10.,1.) / real_t(2 * M_PI);
#endif

vec3 cycles;
Bodies buffer;
Bounds globalBounds;
Bodies bbodies;
Bodies vbodies;
Cells bcells;
Cells vcells;

Args * args;
BaseMPI * baseMPI;
BoundBox * boundBox;
BuildTree * localTree, * globalTree;
Partition * partition;
Traversal<kernel> * traversal;
TreeMPI<kernel> * treeMPI;
UpDownPass<kernel> * upDownPass;

void log_initialize() {
  args->verbose &= baseMPI->mpirank == 0;
  logger::verbose = args->verbose;
  logger::printTitle("FMM Parameters");
  args->print(logger::stringLength, P);
  logger::printTitle("FMM Profiling");
  logger::startTimer("Total FMM");
  logger::startPAPI();
}

void log_finalize() {
  logger::stopPAPI();
  logger::stopTimer("Total FMM");
  logger::printTitle("Total runtime");
  logger::printTime("Total FMM");
}

extern "C" void FMM_Init(double eps2, double kreal, double kimag, int ncrit, int threads,
			 int nb, double * xb, double * yb, double * zb, double * vb,
			 int nv, double * xv, double * yv, double * zv, double * vv) {
  const int nspawn = 1000;
  const int images = 0;
  const real_t theta = 0.4;
  const bool useRmax = false;
  const bool useRopt = false;
  const bool verbose = false;
  kernel::setup();

  args = new Args;
  baseMPI = new BaseMPI;
  boundBox = new BoundBox(nspawn);
  localTree = new BuildTree(ncrit, nspawn);
  globalTree = new BuildTree(1, nspawn);
  partition = new Partition(baseMPI->mpirank, baseMPI->mpisize);
  traversal = new Traversal<kernel>(nspawn, images);
  treeMPI = new TreeMPI<kernel>(baseMPI->mpirank, baseMPI->mpisize, images);
  upDownPass = new UpDownPass<kernel>(theta, useRmax, useRopt);
  num_threads(threads);

  args->ncrit = ncrit;
  args->distribution = "external";
  args->dual = 1;
  args->graft = 1;
  args->images = images;
  args->mutual = 0;
  args->numBodies = 0;
  args->useRopt = useRopt;
  args->nspawn = nspawn;
  args->theta = theta;
  args->verbose = verbose & (baseMPI->mpirank == 0);
  args->useRmax = useRmax;
  logger::verbose = args->verbose;
  bbodies.resize(nb);
  for (B_iter B=bbodies.begin(); B!=bbodies.end(); B++) {
    int i = B-bbodies.begin();
    B->X[0] = xb[i];
    B->X[1] = yb[i];
    B->X[2] = zb[i];
    B->SRC  = vb[i];
    B->WEIGHT = 1;
  }
  vbodies.resize(nv);
  for (B_iter B=vbodies.begin(); B!=vbodies.end(); B++) {
    int i = B-vbodies.begin();
    B->X[0] = xv[i];
    B->X[1] = yv[i];
    B->X[2] = zv[i];
    B->SRC  = vv[i];
    B->WEIGHT = 1;
  }
}

extern "C" void FMM_Finalize() {
  delete args;
  delete baseMPI;
  delete boundBox;
  delete localTree;
  delete globalTree;
  delete partition;
  delete traversal;
  delete treeMPI;
  delete upDownPass;
}

extern "C" void FMM_Partition(int & nb, double * xb, double * yb, double * zb, double * vb,
			      int & nv, double * xv, double * yv, double * zv, double * vv) {
  logger::printTitle("Partition Profiling");
  Bounds localBounds = boundBox->getBounds(bbodies);
  localBounds = boundBox->getBounds(vbodies, localBounds);
  globalBounds = baseMPI->allreduceBounds(localBounds);
  cycles = globalBounds.Xmax - globalBounds.Xmin;
  partition->bisection(bbodies, globalBounds);
  bbodies = treeMPI->commBodies(bbodies);
  partition->bisection(vbodies, globalBounds);
  vbodies = treeMPI->commBodies(vbodies);
  treeMPI->allgatherBounds(localBounds);

  nb = bbodies.size();
  for (B_iter B=bbodies.begin(); B!=bbodies.end(); B++) {
    int i = B-bbodies.begin();
    xb[i] = B->X[0];
    yb[i] = B->X[1];
    zb[i] = B->X[2];
#if EXAFMM_HELMHOLTZ
    vb[i] = std::real(B->SRC);
#else
    vb[i] = B->SRC;
#endif
    B->IBODY = i;
  }
  nv = vbodies.size();
  for (B_iter B=vbodies.begin(); B!=vbodies.end(); B++) {
    int i = B-vbodies.begin();
    xv[i] = B->X[0];
    yv[i] = B->X[1];
    zv[i] = B->X[2];
#if EXAFMM_HELMHOLTZ
    vv[i] = std::real(B->SRC);
#else
    vv[i] = B->SRC;
#endif
    B->IBODY = i;
  }
}

extern "C" void FMM_BuildTree() {
  Bounds localBoundsB = boundBox->getBounds(bbodies);
  bcells = localTree->buildTree(bbodies, buffer, localBoundsB);
  Bounds localBoundsV = boundBox->getBounds(vbodies);
  vcells = localTree->buildTree(vbodies, buffer, localBoundsV);
}

extern "C" void FMM_B2B(double * vi, double * vb, bool verbose) {
  args->verbose = verbose;
  log_initialize();
  for (B_iter B=bbodies.begin(); B!=bbodies.end(); B++) {
    B->SRC    = vb[B->IBODY];
    B->TRG    = 0;
  }
  upDownPass->upwardPass(bcells);
  treeMPI->setLET(bcells, cycles);
  Cells jcells = bcells;
  treeMPI->commBodies();
  treeMPI->commCells();
  traversal->initListCount(bcells);
  traversal->initWeight(bcells);
  traversal->traverse(bcells, jcells, cycles, args->dual, args->mutual);
  if (baseMPI->mpisize > 1) {
    if (args->graft) {
      treeMPI->linkLET();
      Bodies gbodies = treeMPI->root2body();
      jcells = globalTree->buildTree(gbodies, buffer, globalBounds);
      treeMPI->attachRoot(jcells);
      traversal->traverse(bcells, jcells, cycles, args->dual, false);
    } else {
      for (int irank=0; irank<baseMPI->mpisize; irank++) {
	treeMPI->getLET(jcells, (baseMPI->mpirank+irank)%baseMPI->mpisize);
	traversal->traverse(bcells, jcells, cycles, args->dual, false);
      }
    }
  }
  upDownPass->downwardPass(bcells);
  log_finalize();
  for (B_iter B=bbodies.begin(); B!=bbodies.end(); B++) {
#if EXAFMM_HELMHOLTZ
    vi[B->IBODY] += std::real(B->TRG[0]);
#else
    vi[B->IBODY] += B->TRG[0];
#endif
  }
}

extern "C" void FMM_V2B(double * vb, double * vv, bool verbose) {
  args->verbose = verbose;
  log_initialize();
  for (B_iter B=bbodies.begin(); B!=bbodies.end(); B++) {
    B->SRC    = 1;
    B->TRG    = 0;
  }
  for (B_iter B=vbodies.begin(); B!=vbodies.end(); B++) {
    B->SRC    = vv[B->IBODY];
    B->TRG    = 0;
  }
  upDownPass->upwardPass(bcells);
  upDownPass->upwardPass(vcells);
  treeMPI->setLET(vcells, cycles);
  treeMPI->commBodies();
  treeMPI->commCells();
  traversal->initListCount(bcells);
  traversal->initWeight(bcells);
  traversal->traverse(bcells, vcells, cycles, args->dual, args->mutual);
  if (baseMPI->mpisize > 1) {
    if (args->graft) {
      treeMPI->linkLET();
      Bodies gbodies = treeMPI->root2body();
      Cells jcells = globalTree->buildTree(gbodies, buffer, globalBounds);
      treeMPI->attachRoot(jcells);
      traversal->traverse(bcells, jcells, cycles, args->dual, false);
    } else {
      for (int irank=0; irank<baseMPI->mpisize; irank++) {
	Cells jcells;
	treeMPI->getLET(jcells, (baseMPI->mpirank+irank)%baseMPI->mpisize);
	traversal->traverse(bcells, jcells, cycles, args->dual, false);
      }
    }
  }
  upDownPass->downwardPass(bcells);
  log_finalize();
  for (B_iter B=bbodies.begin(); B!=bbodies.end(); B++) {
#if EXAFMM_HELMHOLTZ
    vb[B->IBODY] += std::real(B->TRG[0]);
#else
    vb[B->IBODY] += B->TRG[0];
#endif
  }
}

extern "C" void FMM_B2V(double * vv, double * vb, bool verbose) {
  args->verbose = verbose;
  log_initialize();
  for (B_iter B=vbodies.begin(); B!=vbodies.end(); B++) {
    B->SRC    = 1;
    B->TRG    = 0;
  }
  for (B_iter B=bbodies.begin(); B!=bbodies.end(); B++) {
    B->SRC    = vb[B->IBODY];
    B->TRG    = 0;
  }
  upDownPass->upwardPass(bcells);
  upDownPass->upwardPass(vcells);
  treeMPI->setLET(bcells, cycles);
  treeMPI->commBodies();
  treeMPI->commCells();
  traversal->initListCount(vcells);
  traversal->initWeight(vcells);
  traversal->traverse(vcells, bcells, cycles, args->dual, args->mutual);
  if (baseMPI->mpisize > 1) {
    if (args->graft) {
      treeMPI->linkLET();
      Bodies gbodies = treeMPI->root2body();
      Cells jcells = globalTree->buildTree(gbodies, buffer, globalBounds);
      treeMPI->attachRoot(jcells);
      traversal->traverse(vcells, jcells, cycles, args->dual, false);
    } else {
      for (int irank=0; irank<baseMPI->mpisize; irank++) {
	Cells jcells;
	treeMPI->getLET(jcells, (baseMPI->mpirank+irank)%baseMPI->mpisize);
	traversal->traverse(vcells, jcells, cycles, args->dual, false);
      }
    }
  }
  upDownPass->downwardPass(vcells);
  log_finalize();
  for (B_iter B=vbodies.begin(); B!=vbodies.end(); B++) {
#if EXAFMM_HELMHOLTZ
    vv[B->IBODY] += std::real(B->TRG[0]);
#else
    vv[B->IBODY] += B->TRG[0];
#endif
  }
}

extern "C" void FMM_V2V(double * vi, double * vv, bool verbose) {
  args->verbose = verbose;
  log_initialize();
  for (B_iter B=vbodies.begin(); B!=vbodies.end(); B++) {
    B->SRC    = vv[B->IBODY];
    B->TRG    = 0;
  }
  upDownPass->upwardPass(vcells);
  treeMPI->setLET(vcells, cycles);
  Cells jcells = vcells;
  treeMPI->commBodies();
  treeMPI->commCells();
  traversal->initListCount(vcells);
  traversal->initWeight(vcells);
  traversal->traverse(vcells, jcells, cycles, args->dual, args->mutual);
  if (baseMPI->mpisize > 1) {
    if (args->graft) {
      treeMPI->linkLET();
      Bodies gbodies = treeMPI->root2body();
      jcells = globalTree->buildTree(gbodies, buffer, globalBounds);
      treeMPI->attachRoot(jcells);
      traversal->traverse(vcells, jcells, cycles, args->dual, false);
    } else {
      for (int irank=0; irank<baseMPI->mpisize; irank++) {
	treeMPI->getLET(jcells, (baseMPI->mpirank+irank)%baseMPI->mpisize);
	traversal->traverse(vcells, jcells, cycles, args->dual, false);
      }
    }
  }
  upDownPass->downwardPass(vcells);
  log_finalize();
  for (B_iter B=vbodies.begin(); B!=vbodies.end(); B++) {
#if EXAFMM_HELMHOLTZ
    vi[B->IBODY] += std::real(B->TRG[0]);
#else
    vi[B->IBODY] += B->TRG[0];
#endif
  }
}

extern "C" void Direct(int ni, double * xi, double * yi, double * zi, double * vi,
		       int nj, double * xj, double * yj, double * zj, double * vj) {
  Bodies bodies(ni);
  for (B_iter B=bodies.begin(); B!=bodies.end(); B++) {
    int i = B-bodies.begin();
    B->X[0] = xi[i];
    B->X[1] = yi[i];
    B->X[2] = zi[i];
    B->TRG  = 0;
    B->SRC  = 1;
  }
  Bodies jbodies(nj);
  for (B_iter B=jbodies.begin(); B!=jbodies.end(); B++) {
    int i = B-jbodies.begin();
    B->X[0] = xj[i];
    B->X[1] = yj[i];
    B->X[2] = zj[i];
    B->SRC  = vj[i];
  }  
  for (int irank=0; irank<baseMPI->mpisize; irank++) {
    if (args->verbose) std::cout << "Direct loop          : " << irank+1 << "/" << baseMPI->mpisize << std::endl;
    treeMPI->shiftBodies(jbodies);
    traversal->direct(bodies, jbodies, cycles);
  }
  for (B_iter B=bodies.begin(); B!=bodies.end(); B++) {
    int i = B-bodies.begin();
#if EXAFMM_HELMHOLTZ
    vi[i] += std::real(B->TRG[0]);
#else
    vi[i] += B->TRG[0];
#endif
  }
}
