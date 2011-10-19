static char help[] = "Driver for a parametrized linear elastodynamic problem (Hyperbolic)";

#include "mpi.h"
#include <iostream>
#include <fstream>
#include <vector>

#include "petscksp.h"
#include "petscda.h"

#include "timeInfo.h"
#include "feMatrix.h"
#include "feVector.h"
#include "femUtils.h"
#include "timeStepper.h"
#include "newmark.h"
#include "elasStiffness.h"
#include "elasMass.h"
#include "raleighDamping.h"
#include "cardiacDynamic.h"
#include "parametricElasInverse.h"
#include "radialBasis.h"
#include "bSplineBasis.h"

#define min(X, Y)  ((X) < (Y) ? (X) : (Y))

void getForces(Vec params, std::vector<Vec> &forces, DA da, timeInfo *ti, std::vector<radialBasis> gBasis, bSplineBasis bBasis);

int main(int argc, char **argv)
{       
  PetscInitialize(&argc, &argv, "elas.opt", help);

  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);


  int Ns = 32;
  unsigned int dof = 3;

  char problemName[PETSC_MAX_PATH_LEN];
  char filename[PETSC_MAX_PATH_LEN];

  double t0 = 0.0;
  double dt = 0.1;
  double t1 = 1.0;
  double beta = 0.000001;

  // double dtratio = 1.0;
  DA  da;         // Underlying scalar DA - for scalar properties
  DA  da3d;       // Underlying vector DA - for vector properties

  Vec rho;        // density - elemental scalar
  Vec lambda;     // Lame parameter - lambda - elemental scalar
  Vec mu;         // Lame parameter - mu - elemental scalar

  // Initial conditions
  Vec initialDisplacement; 
  Vec initialVelocity;

  timeInfo ti;

  int parFac = 2;
  int numParams = 120;

  CHKERRQ ( PetscOptionsGetInt(0,"-pFac", &parFac,0) );

  numParams = parFac*parFac*parFac*5*3;
  if (!rank)
    std::cout << "Total number of unknowns is " << numParams << std::endl;

  // get Ns
  CHKERRQ ( PetscOptionsGetInt(0,"-Ns",&Ns,0) );

  CHKERRQ ( PetscOptionsGetScalar(0,"-t0",&t0,0) );
  CHKERRQ ( PetscOptionsGetScalar(0,"-t1",&t1,0) );
  CHKERRQ ( PetscOptionsGetScalar(0,"-dt",&dt,0) );
  CHKERRQ ( PetscOptionsGetScalar(0,"-beta",&beta,0) );
  CHKERRQ ( PetscOptionsGetString(PETSC_NULL,"-pn",problemName,PETSC_MAX_PATH_LEN-1,PETSC_NULL));

  if (!rank) {
    std::cout << "Problem size is " << Ns+1 << " spatially and NT = " << (int)ceil(1.0/dt) << std::endl;
  }

  // Time info for timestepping
  ti.start = t0;
  ti.stop  = t1;
  ti.step  = dt;

  // create DA
  CHKERRQ ( DACreate3d ( PETSC_COMM_WORLD, DA_NONPERIODIC, DA_STENCIL_BOX, 
                         Ns+1, Ns+1, Ns+1, PETSC_DECIDE, PETSC_DECIDE, PETSC_DECIDE,
                         1, 1, 0, 0, 0, &da) );
  CHKERRQ ( DACreate3d ( PETSC_COMM_WORLD, DA_NONPERIODIC, DA_STENCIL_BOX, 
                         Ns+1, Ns+1, Ns+1, PETSC_DECIDE, PETSC_DECIDE, PETSC_DECIDE,
                         dof, 1, 0, 0, 0, &da3d) );

  elasMass *Mass = new elasMass(feMat::PETSC); // Mass Matrix
  elasStiffness *Stiffness = new elasStiffness(feMat::PETSC); // Stiffness matrix
  raleighDamping *Damping = new raleighDamping(feMat::PETSC); // Damping Matrix

  cardiacDynamic *Force = new cardiacDynamic(feVec::PETSC); // Force Vector

  // create vectors 
  CHKERRQ( DACreateGlobalVector(da, &rho) );
  CHKERRQ( DACreateGlobalVector(da, &mu) );
  CHKERRQ( DACreateGlobalVector(da, &lambda) );

  CHKERRQ( DACreateGlobalVector(da3d, &initialDisplacement) );
  CHKERRQ( DACreateGlobalVector(da3d, &initialVelocity) );

  // Set initial conditions
  CHKERRQ( VecSet ( initialDisplacement, 0.0) ); 
  CHKERRQ( VecSet ( initialVelocity, 0.0) );

  VecZeroEntries( mu );
  VecZeroEntries( lambda );
  VecZeroEntries( rho );

  int x, y, z, m, n, p;
  int mx,my,mz, xne, yne, zne;

  CHKERRQ( DAGetCorners(da, &x, &y, &z, &m, &n, &p) ); 
  CHKERRQ( DAGetInfo(da,0, &mx, &my, &mz, 0,0,0,0,0,0,0) ); 

  if (x+m == mx) {
    xne=m-1;
  } else {
    xne=m;
  }
  if (y+n == my) {
    yne=n-1;
  } else {
    yne=n;
  }
  if (z+p == mz) {
    zne=p-1;
  } else {
    zne=p;
  }

  // double acx,acy,acz;
  // double hx = 1.0/((double)Ns);

  // Generate the basis ...
  std::vector < radialBasis > spatialBasis;
  bSplineBasis temporalBasis(3, 5); // this creates and sets up the basis ...
  // temporalBasis.knot();

  double fac = 1.0/parFac;
  // Now to set up the radial bases ...
  for (int k=0; k<parFac; k++) {
    for (int j=0; j<parFac; j++) {
      for (int i=0; i<parFac; i++) {
        // std::cout << "Adding radial basis at: " << fac/2+i*fac << ", " << fac/2+j*fac << ", " << fac/2+k*fac << std::endl;
        radialBasis tmp(Point( fac/2+i*fac,fac/2+j*fac,fac/2+k*fac), Point(fac/2,fac/2,fac/2));
        spatialBasis.push_back(tmp);
      }
    }
  }

  // SET MATERIAL PROPERTIES ...

  // @todo - Write routines to read/write in Parallel
  // allocate for temporary buffers ...
  unsigned int elemSize = Ns*Ns*Ns;
  // std::cout << "Elem size is " << elemSize << std::endl;
  // unsigned int nodeSize = (Ns+1)*(Ns+1)*(Ns+1);

  unsigned char *tmp_mat = new unsigned char[elemSize];  
  double *tmp_tau = new double[dof*elemSize];

  // generate filenames & read in the raw arrays first ...
  std::ifstream fin;

  sprintf(filename, "%s.%d.img", problemName, Ns); 
  fin.open(filename, std::ios::binary); fin.read((char *)tmp_mat, elemSize); fin.close();

  // Set Elemental material properties
  PetscScalar ***muArray, ***lambdaArray, ***rhoArray;

  CHKERRQ(DAVecGetArray(da, mu, &muArray));
  CHKERRQ(DAVecGetArray(da, lambda, &lambdaArray));
  CHKERRQ(DAVecGetArray(da, rho, &rhoArray));

  // assign material properties ...
  //       myo,   tissue
  // nu  = 0.49,  0.45
  // E   = 10000, 1000
  // rho = 1.0,   0.1

  // std::cout << "Setting Elemental properties." << std::endl;

  // loop through all elements ...
  for (int k=z; k<z+zne; k++) {
    for (int j=y; j<y+yne; j++) {
      for (int i=x; i<x+xne; i++) {
        int indx = k*Ns*Ns + j*Ns + i;

        if ( tmp_mat[indx] ) {
          muArray[k][j][i] = 0.1; //3355.7;
          lambdaArray[k][j][i] = 0.1; //164429.53;
          rhoArray[k][j][i] = 1.0;
        } else {
          muArray[k][j][i] = 0.1; //344.82;
          lambdaArray[k][j][i] = 0.1; //3103.448;
          rhoArray[k][j][i] = 1.0;
        }

      } // end i
    } // end j
  } // end k

  // std::cout << "Finished Elemental loop." << std::endl;

  CHKERRQ( DAVecRestoreArray ( da, mu, &muArray ) );
  CHKERRQ( DAVecRestoreArray ( da, lambda, &lambdaArray ) );
  CHKERRQ( DAVecRestoreArray ( da, rho, &rhoArray ) );

  // std::cout << "Finished restoring arrays" << std::endl; 
  // delete temporary buffers

  delete [] tmp_mat;

  // Now set the activation ...
  // unsigned int numSteps = (unsigned int)(ceil(( ti.stop - ti.start)/ti.step));

  
  std::vector<Vec> newF;

  Vec alpha;
  PetscScalar *avec;

  VecCreateSeq(PETSC_COMM_SELF, numParams, &alpha);

  VecGetArray(alpha, &avec);

  for (int j=0; j<numParams; j++)
    avec[j] = 0.0;

#ifdef __DEBUG__  
  if (!rank) {
    std::cout << x << ", " << y << ", " << z << " + " << xne << ", " << yne << ", " << zne << std::endl;
  }
#endif  

  sprintf(filename, "%s.%d.%.3d.fld", problemName, Ns, 3);
  fin.open(filename); fin.read((char *)tmp_tau, dof*elemSize*sizeof(double)); fin.close();

  double ax, ay, az;
  double xx,yy,zz;
  int ix, iy, iz;
  for (unsigned int i=0; i< spatialBasis.size(); i++) {
    spatialBasis[i].getCenter(xx,yy,zz);
    ix = (int)(xx*Ns); iy = (int)(yy*Ns); iz = (int)(zz*Ns);
    // std::cout << "Center is " << ix << ", " << iy << ", " << iz << std::endl;
    ax = tmp_tau[dof*(Ns*(iz*Ns + iy) + ix)]; 
    ay = tmp_tau[dof*(Ns*(iz*Ns + iy) + ix)+1];
    az = tmp_tau[dof*(Ns*(iz*Ns + iy) + ix)+2];
    for (unsigned int j=0; j<temporalBasis.getNumKnots(); j++) {
      unsigned int maxIndex = i*temporalBasis.getNumKnots()*dof + j*dof+2;
      if (maxIndex >= numParams) 
        std::cout << RED"maxIndex is out of bounds for "NRM << i << ", " << j << std::endl;

      avec[i*temporalBasis.getNumKnots()*dof + j*dof ]  += ax*( min(j, temporalBasis.getNumKnots()-1-i) +1 );
      avec[i*temporalBasis.getNumKnots()*dof + j*dof+1] += ay*( min(j, temporalBasis.getNumKnots()-1-i) +1 );
      avec[i*temporalBasis.getNumKnots()*dof + j*dof+2] += az*( min(j, temporalBasis.getNumKnots()-1-i) +1 );
    }
  }
  VecRestoreArray(alpha, &avec);

  // VecView(alpha, 0);

  delete [] tmp_tau;
  
  // MPI_Barrier(MPI_COMM_WORLD);
  // Generate the force ...
  // std::cout << "Generating forces" << std::endl;
  getForces(alpha, newF, da3d, &ti, spatialBasis, temporalBasis);
  // std::cout << "Done Generating forces" << std::endl;
  // MPI_Barrier(MPI_COMM_WORLD);

  // DONE - SET MATERIAL PROPERTIES ...

  // Setup Matrices and Force Vector ...
  Mass->setProblemDimensions(1.0, 1.0, 1.0);
  Mass->setDA(da3d);
  Mass->setDof(dof);
  Mass->setDensity(rho);

  Stiffness->setProblemDimensions(1.0, 1.0, 1.0);
  Stiffness->setDA(da3d);
  Stiffness->setDof(dof);
  Stiffness->setLame(lambda, mu);

  Damping->setAlpha(0.0);
  Damping->setBeta(0.00075);
  Damping->setMassMatrix(Mass);
  Damping->setStiffnessMatrix(Stiffness);
  Damping->setDA(da3d);
  Damping->setDof(dof);

  // Force Vector
  Force->setProblemDimensions(1.0,1.0,1.0);
  Force->setDA(da3d);
  // Force->setActivationVec(tau);
  // Force->setFiberOrientations(fibers);
  Force->setFDynamic(newF);
  Force->setTimeInfo(&ti);

  // Newmark time stepper ...
  newmark *ts = new newmark; 

  ts->setMassMatrix(Mass);
  ts->setDampingMatrix(Damping);
  ts->setStiffnessMatrix(Stiffness);
  ts->damp(false);
  ts->setTimeFrames(1);

  ts->setForceVector(Force);

  ts->setInitialDisplacement(initialDisplacement);
  ts->setInitialVelocity(initialVelocity);
  ts->storeVec(true);
  ts->setTimeInfo(&ti);
  ts->setAdjoint(false); // set if adjoint or forward
	ts->useMatrixFree(false);

  ts->init(); // initialize IMPORTANT 
  if (!rank)
    std::cout << RED"Starting Newmark Solve"NRM << std::endl;
  ts->solve();// solve 
  if (!rank)
    std::cout << GRN"Done Newmark"NRM << std::endl;

  std::vector<Vec> solvec = ts->getSolution();
  if (!rank)
    std::cout << "Got Solution" << std::endl;


  // CHecking ...

  // clear memory ...
  /*
  for (unsigned int i=0; i<solvec.size(); i++) {
    if (solvec[i] != NULL) {
      VecDestroy(solvec[i]);
    }
  }
  solvec.clear();
 
  std::cout << "Finished clearing solution" << std::endl;
  */

  // clear memory ...
  
  for (unsigned int i=0; i<newF.size(); i++) {
    if (newF[i] != NULL) {
      VecDestroy(newF[i]);
    }
  }
  newF.clear();

  /* Set very initial guess for the inverse problem*/
  /*
     PetscRandom rctx;
     PetscRandomCreate(PETSC_COMM_WORLD,&rctx);
     PetscRandomSetFromOptions(rctx);
     VecSetRandom(guess,rctx);
     VecNorm(guess,NORM_INFINITY,&norm);
     PetscPrintf(0,"guess norm = %g\n",norm);
     */

  
  double norm;
  if(!rank)  
    std::cout << "Solution size is " << solvec.size() << std::endl;
  for (unsigned int i=0; i<solvec.size(); i++) {
    VecNorm(solvec[i], NORM_2, &norm);
    if(!rank)
      std::cout << "norm at " << i << " is " << norm << std::endl;
  }
 

  double errnorm;
  double exsolnorm;

  Vec guess;
  // Vec truth;
  Vec Err;
  // concatenateVecs(solvec, guess);
  VecDuplicate(alpha, &guess);
  // VecDuplicate(alpha, truth);
  if (!rank)
    std::cout << "Done Concatenating" << std::endl;

  iC(VecNorm(alpha, NORM_2, &exsolnorm));
  /*

     std::cout << "Forward solver solution size is " << solvec.size() << std::endl;
     std::cout << "Forward solver solution norm is " << exsolnorm << std::endl;
     */
  VecZeroEntries(guess);

  // Inverse solver set up
  parametricElasInverse *hyperInv = new parametricElasInverse;
  PetscPrintf(0, "Constructed\n");

  hyperInv->setBasis(spatialBasis, temporalBasis);
  PetscPrintf(0, "Set Bases\n");
  hyperInv->setForwardInitialConditions(initialDisplacement, initialVelocity);
  PetscPrintf(0, "Set Initial\n");
  hyperInv->setTimeStepper(ts);    // set the timestepper
  hyperInv->setInitialGuess(guess);// set the initial guess 
  hyperInv->setRegularizationParameter(beta); // set the regularization paramter
  hyperInv->setObservations(solvec); // set the data for the problem 
  if (!rank)
    std::cout << "Initializing hyperInv" << std::endl;
  hyperInv->init(); // initialize the inverse solver
  PetscPrintf(0, "Starting Inverse solve\n");
  hyperInv->solve(); // solve
  PetscPrintf(0, "Done Inverse solve\n");
  hyperInv->getCurrentControl(guess); // get the solution 



  // see the error in the solution relative to the actual solution
  VecDuplicate(alpha, &Err);
  iC(VecZeroEntries(Err)); 
  iC(VecWAXPY(Err, -1.0, guess, alpha));
  iC(VecNorm(Err, NORM_2, &errnorm));
  PetscPrintf(0,"errr in inverse = %g\n", errnorm/exsolnorm);

  PetscFinalize();
}

// Functions to hanlde the full / parametrized representations
void getForces(Vec params, std::vector<Vec> &forces, DA da, timeInfo *ti, std::vector<radialBasis> gBasis, bSplineBasis bBasis) {
#ifdef __DEBUG__
  std::cout << RED"Entering "NRM << __func__ << std::endl;
#endif
  // Generate the force vector based on the current parameters ...

  PetscScalar * pVec;
  VecGetArray(params, &pVec);
  // Clear the Forces
  for (unsigned int i=0; i<forces.size(); i++) {
    if (forces[i] != NULL) {
      VecDestroy(forces[i]);
    }
  }
  forces.clear();

  unsigned int numSteps = (unsigned int)(ceil(( ti->stop - ti->start)/ti->step));
  PetscScalar ***tauArray; // = (PetscScalar ****) new char*[numSteps+1];

  // create and initialize to 0
  for (unsigned int i=0; i<numSteps+1; i++) {
    Vec tmp;
    DACreateGlobalVector(da, &tmp);
    VecZeroEntries(tmp);
    forces.push_back(tmp);
    // DAVecGetArray(da, forces[i], &tauArray[i]);
  }

  double gx;
  int x, y, z, m, n, p;
  int mx,my,mz;

  DAGetCorners(da, &x, &y, &z, &m, &n, &p);
  DAGetInfo(da,0, &mx, &my, &mz, 0,0,0,0,0,0,0);

  double hx = 1.0/(mx-1.0);

  int knotsize = bBasis.getNumKnots();
  int bsize = 3*knotsize;
  double *currBasis = new double[knotsize];

  double currTime = ti->current;
  for (unsigned int t=0; t<numSteps+1; t++) {
    DAVecGetArray(da, forces[t], &tauArray);
    bBasis.basis(currTime, currBasis);
    
    /*
    std::cout << "Basis at " << currTime << " is ";
    for (int i=0; i<knotsize; i++) {
      std::cout << currBasis[i] << " ";
    }
    std::cout << std::endl;
    */

    for (int k = z; k < z + p ; k++) {
      for (int j = y; j < y + n; j++) {
        for (int i = x; i < x + m; i++) {
          gx = 0.0;
          Point px(i,j,k);
          px *= hx;
          // determine the factor from spatial basis
          for (unsigned int b=0; b<gBasis.size(); b++) {
            double val = gBasis[b].getValue(px);
            gx = val;

            // std::cout << " Val is " << val << std::endl;
            // time loop 
            for (unsigned int r=0; r<knotsize; r++) {
              tauArray[k][j][3*i]    += gx*currBasis[r]*pVec[b*bsize + 3*r ];
              tauArray[k][j][3*i+1]  += gx*currBasis[r]*pVec[b*bsize + 3*r + 1];
              tauArray[k][j][3*i+2]  += gx*currBasis[r]*pVec[b*bsize + 3*r + 2];
            }
          }
        }
      }
    }
    DAVecRestoreArray ( da, forces[t], &tauArray ) ;
    currTime += ti->step;
#ifdef __DEBUG__
    double norm;
    VecNorm(forces[t], NORM_2, &norm);
    std::cout << "Force norm at " << t << " is " << norm << std::endl;
#endif
  }

//  for (unsigned int t=0; t<numSteps+1; t++) {
//    DAVecRestoreArray ( da, forces[t], &tauArray ) ;
//  }

  VecRestoreArray(params, &pVec);
  delete [] currBasis;
#ifdef __DEBUG__
  std::cout << GRN"Leaving "NRM << __func__ << std::endl;
#endif
}

