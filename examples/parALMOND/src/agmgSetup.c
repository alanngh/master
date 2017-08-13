#include "agmg.h"

csr *strong_graph(csr *A, dfloat threshold);
bool customLess(iint smax, dfloat rmax, iint imax, iint s, dfloat r, iint i);
iint *form_aggregates(agmgLevel *level, csr *C);
void find_aggregate_owners(agmgLevel *level, iint* FineToCoarse);
csr *construct_interpolator(agmgLevel *level, iint *FineToCoarse, dfloat **nullCoarseA);
csr *transpose(agmgLevel* level, csr *A, iint *globalRowStarts, iint *globalColStarts);
csr *galerkinProd(agmgLevel *level, csr *R, csr *A, csr *P);
void coarsenAgmgLevel(agmgLevel *level, csr **coarseA, csr **P, csr **R, dfloat **nullCoarseA);


void agmgSetup(parAlmond_t *parAlmond, csr *A, dfloat *nullA, iint *globalRowStarts, const char* options){

  iint rank, size;
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  // approximate Nrows at coarsest level
  int gCoarseSize = 100;

  double seed = (double) rank;
  srand48(seed);

  agmgLevel **levels = parAlmond->levels;

  int lev = parAlmond->numLevels; //add this level to the end of the chain

  levels[lev] = (agmgLevel *) calloc(1,sizeof(agmgLevel));
  parAlmond->numLevels++;

  //copy A matrix and null vector
  levels[lev]->A = A;
  levels[lev]->nullA = nullA;
  levels[lev]->deviceA = newHYB(parAlmond, levels[lev]->A);

  levels[lev]->Nrows = A->Nrows;
  levels[lev]->Ncols = A->Ncols;

  SmoothType smoothType;
  if (strstr(options,"CHEBYSHEV")) {
    smoothType = CHEBYSHEV;
  } else { //default to DAMPED_JACOBI
    smoothType = DAMPED_JACOBI;
  }
  setupSmoother(parAlmond, levels[lev], smoothType);

  //set operator callback
  void **args = (void **) calloc(2,sizeof(void*));
  args[0] = (void *) parAlmond;
  args[1] = (void *) levels[lev];

  levels[lev]->AxArgs = args;
  levels[lev]->smoothArgs = args;
  levels[lev]->Ax = agmgAx;
  levels[lev]->smooth = agmgSmooth;
  levels[lev]->device_Ax = device_agmgAx;
  levels[lev]->device_smooth = device_agmgSmooth;

  //copy global partiton
  levels[lev]->globalRowStarts = (iint *) calloc(size+1,sizeof(iint));
  for (iint r=0;r<size+1;r++)
      levels[lev]->globalRowStarts[r] = globalRowStarts[r];

  iint localSize = levels[lev]->A->Nrows;
  iint globalSize = 0;
  MPI_Allreduce(&localSize, &globalSize, 1, MPI_IINT, MPI_SUM, MPI_COMM_WORLD);

  //if the system if already small, dont create MG levels
  bool done = false;
  if(globalSize <= gCoarseSize){
    setupExactSolve(parAlmond, levels[lev]);
    done = true;
  }


  while(!done){
    // create coarse MG level
    levels[lev+1] = (agmgLevel *) calloc(1,sizeof(agmgLevel));
    dfloat *nullCoarseA;

    printf("Setting up coarse level %d\n", lev+1);

    coarsenAgmgLevel(levels[lev], &(levels[lev+1]->A), &(levels[lev+1]->P),
                                  &(levels[lev+1]->R), &nullCoarseA);

    //set dimensions of the fine level (max among the A,R ops)
    levels[lev]->Ncols = mymax(levels[lev]->Ncols, levels[lev+1]->R->Ncols);

    parAlmond->numLevels++;

    levels[lev+1]->nullA = nullCoarseA;
    levels[lev+1]->Nrows = levels[lev+1]->A->Nrows;
    levels[lev+1]->Ncols = mymax(levels[lev+1]->A->Ncols, levels[lev+1]->P->Ncols);
    levels[lev+1]->globalRowStarts = levels[lev]->globalAggStarts;
    levels[lev+1]->deviceA = newHYB (parAlmond, levels[lev+1]->A);
    levels[lev+1]->deviceR = newHYB (parAlmond, levels[lev+1]->R);
    levels[lev+1]->dcsrP   = newDCOO(parAlmond, levels[lev+1]->P);

    setupSmoother(parAlmond, levels[lev+1], smoothType);

    //set operator callback
    void **args = (void **) calloc(2,sizeof(void*));
    args[0] = (void *) parAlmond;
    args[1] = (void *) levels[lev+1];

    levels[lev+1]->AxArgs = args;
    levels[lev+1]->coarsenArgs = args;
    levels[lev+1]->prolongateArgs = args;
    levels[lev+1]->smoothArgs = args;

    levels[lev+1]->Ax = agmgAx;
    levels[lev+1]->coarsen = agmgCoarsen;
    levels[lev+1]->prolongate = agmgProlongate;
    levels[lev+1]->smooth = agmgSmooth;

    levels[lev+1]->device_Ax = device_agmgAx;
    levels[lev+1]->device_coarsen = device_agmgCoarsen;
    levels[lev+1]->device_prolongate = device_agmgProlongate;
    levels[lev+1]->device_smooth = device_agmgSmooth;

    const iint localCoarseDim = levels[lev+1]->A->Nrows;
    iint globalCoarseSize;
    MPI_Allreduce(&localCoarseDim, &globalCoarseSize, 1, MPI_IINT, MPI_SUM, MPI_COMM_WORLD);

    if(globalCoarseSize <= gCoarseSize || globalSize < 2*globalCoarseSize){
      setupExactSolve(parAlmond, levels[lev+1]);
      break;
    }

    globalSize = globalCoarseSize;
    lev++;
  }

  //allocate vectors required
  occa::device device = parAlmond->device;
  for (int n=0;n<parAlmond->numLevels;n++) {
    iint N = levels[n]->Nrows;
    iint M = levels[n]->Ncols;

    if ((n>0)&&(n<parAlmond->numLevels-1)) { //kcycle vectors
      if (M) levels[n]->ckp1 = (dfloat *) calloc(M,sizeof(dfloat));
      if (N) levels[n]->vkp1 = (dfloat *) calloc(N,sizeof(dfloat));
      if (N) levels[n]->wkp1 = (dfloat *) calloc(N,sizeof(dfloat));

      if (M) levels[n]->o_ckp1 = device.malloc(M*sizeof(dfloat),levels[n]->ckp1);
      if (N) levels[n]->o_vkp1 = device.malloc(N*sizeof(dfloat),levels[n]->vkp1);
      if (N) levels[n]->o_wkp1 = device.malloc(N*sizeof(dfloat),levels[n]->wkp1);
    }
    if (M) levels[n]->x    = (dfloat *) calloc(M,sizeof(dfloat));
    if (M) levels[n]->res  = (dfloat *) calloc(M,sizeof(dfloat));
    if (N) levels[n]->rhs  = (dfloat *) calloc(N,sizeof(dfloat));

    if (M) levels[n]->o_x   = device.malloc(M*sizeof(dfloat),levels[n]->x);
    if (M) levels[n]->o_res = device.malloc(M*sizeof(dfloat),levels[n]->res);
    if (N) levels[n]->o_rhs = device.malloc(N*sizeof(dfloat),levels[n]->rhs);
  }
}

void parAlmondReport(parAlmond_t *parAlmond) {

  iint rank, size;
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  if(rank==0) {
    printf("------------------ParAlmond Report-----------------------------------\n");
    printf("---------------------------------------------------------------------\n");
    printf("level| active ranks |   dimension   |  nnzs         |  nnz/row      |\n");
    printf("     |              | (min,max,avg) | (min,max,avg) | (min,max,avg) |\n");
    printf("---------------------------------------------------------------------\n");
  }

  for(int lev=0; lev<parAlmond->numLevels; lev++){

    iint Nrows = parAlmond->levels[lev]->Nrows;

    int active = (Nrows>0) ? 1:0;
    iint totalActive=0;
    MPI_Allreduce(&active, &totalActive, 1, MPI_IINT, MPI_SUM, MPI_COMM_WORLD);

    iint minNrows=0, maxNrows=0, totalNrows=0;
    dfloat avgNrows;
    MPI_Allreduce(&Nrows, &maxNrows, 1, MPI_IINT, MPI_MAX, MPI_COMM_WORLD);
    MPI_Allreduce(&Nrows, &totalNrows, 1, MPI_IINT, MPI_SUM, MPI_COMM_WORLD);
    avgNrows = (dfloat) totalNrows/totalActive;

    if (Nrows==0) Nrows=maxNrows; //set this so it's ignored for the global min
    MPI_Allreduce(&Nrows, &minNrows, 1, MPI_IINT, MPI_MIN, MPI_COMM_WORLD);


    iint nnz;
    if (parAlmond->levels[lev]->A)
      nnz = parAlmond->levels[lev]->A->diagNNZ+parAlmond->levels[lev]->A->offdNNZ;
    else
      nnz =0;
    iint minNnz=0, maxNnz=0, totalNnz=0;
    dfloat avgNnz;
    MPI_Allreduce(&nnz, &maxNnz, 1, MPI_IINT, MPI_MAX, MPI_COMM_WORLD);
    MPI_Allreduce(&nnz, &totalNnz, 1, MPI_IINT, MPI_SUM, MPI_COMM_WORLD);
    avgNnz = (dfloat) totalNnz/totalActive;

    if (nnz==0) nnz = maxNnz; //set this so it's ignored for the global min
    MPI_Allreduce(&nnz, &minNnz, 1, MPI_IINT, MPI_MIN, MPI_COMM_WORLD);

    Nrows = parAlmond->levels[lev]->Nrows;
    dfloat nnzPerRow = (Nrows==0) ? 0 : (dfloat) nnz/Nrows;
    dfloat minNnzPerRow=0, maxNnzPerRow=0, avgNnzPerRow=0;
    MPI_Allreduce(&nnzPerRow, &maxNnzPerRow, 1, MPI_DFLOAT, MPI_MAX, MPI_COMM_WORLD);
    MPI_Allreduce(&nnzPerRow, &avgNnzPerRow, 1, MPI_DFLOAT, MPI_SUM, MPI_COMM_WORLD);
    avgNnzPerRow /= totalActive;

    if (Nrows==0) nnzPerRow = maxNnzPerRow;
    MPI_Allreduce(&nnzPerRow, &minNnzPerRow, 1, MPI_DFLOAT, MPI_MIN, MPI_COMM_WORLD);

    if (rank==0){
      printf(" %3d |        %4d  |   %10.2f  |   %10.2f  |   %10.2f  |\n",
        lev, totalActive, (dfloat)minNrows, (dfloat)minNnz, minNnzPerRow);
      printf("     |              |   %10.2f  |   %10.2f  |   %10.2f  |\n",
        (dfloat)maxNrows, (dfloat)maxNnz, maxNnzPerRow);
      printf("     |              |   %10.2f  |   %10.2f  |   %10.2f  |\n",
        avgNrows, avgNnz, avgNnzPerRow);
    }
  }
  if(rank==0)
    printf("---------------------------------------------------------------------\n");
}


//create coarsened problem
void coarsenAgmgLevel(agmgLevel *level, csr **coarseA, csr **P, csr **R, dfloat **nullCoarseA){

  // establish the graph of strong connections
  level->threshold = 0.5;

  csr *C = strong_graph(level->A, level->threshold);

  iint *FineToCoarse = form_aggregates(level, C);

  //find_aggregate_owners(level,FineToCoarse);

  *P = construct_interpolator(level, FineToCoarse, nullCoarseA);
  *R = transpose(level, *P, level->globalRowStarts, level->globalAggStarts);
  *coarseA = galerkinProd(level, *R, level->A, *P);
}

csr * strong_graph(csr *A, dfloat threshold){

  const iint N = A->Nrows;
  const iint M = A->Ncols;

  csr *C = (csr *) calloc(1, sizeof(csr));

  C->Nrows = N;
  C->Ncols = M;

  C->diagRowStarts = (iint *) calloc(N+1,sizeof(iint));
  C->offdRowStarts = (iint *) calloc(N+1,sizeof(iint));

  dfloat *maxOD;
  if (N) maxOD = (dfloat *) calloc(N,sizeof(dfloat));

  //store the diagonal of A for all needed columns
  dfloat *diagA = (dfloat *) calloc(M,sizeof(dfloat));
  for (iint i=0;i<N;i++)
    diagA[i] = A->diagCoefs[A->diagRowStarts[i]];
  csrHaloExchange(A, sizeof(dfloat), diagA, A->sendBuffer, diagA+A->NlocalCols);

  for(iint i=0; i<N; i++){
    dfloat sign = (diagA[i] >= 0) ? 1:-1;
    dfloat Aii = fabs(diagA[i]);

    //find maxOD
    //local entries
    iint Jstart = A->diagRowStarts[i], Jend = A->diagRowStarts[i+1];
    for(iint jj= Jstart+1; jj<Jend; jj++){
      iint col = A->diagCols[jj];
      dfloat Ajj = fabs(diagA[col]);
      dfloat OD = -sign*A->diagCoefs[jj]/(sqrt(Aii)*sqrt(Ajj));
      if(OD > maxOD[i]) maxOD[i] = OD;
    }
    //non-local entries
    Jstart = A->offdRowStarts[i], Jend = A->offdRowStarts[i+1];
    for(iint jj= Jstart; jj<Jend; jj++){
      iint col = A->offdCols[jj];
      dfloat Ajj = fabs(diagA[col]);
      dfloat OD = -sign*A->offdCoefs[jj]/(sqrt(Aii)*sqrt(Ajj));
      if(OD > maxOD[i]) maxOD[i] = OD;
    }

    iint diag_strong_per_row = 1; // diagonal entry
    //local entries
    Jstart = A->diagRowStarts[i], Jend = A->diagRowStarts[i+1];
    for(iint jj = Jstart+1; jj<Jend; jj++){
      iint col = A->diagCols[jj];
      dfloat Ajj = fabs(diagA[col]);
      dfloat OD = -sign*A->diagCoefs[jj]/(sqrt(Aii)*sqrt(Ajj));
      if(OD > threshold*maxOD[i]) diag_strong_per_row++;
    }
    iint offd_strong_per_row = 0;
    //non-local entries
    Jstart = A->offdRowStarts[i], Jend = A->offdRowStarts[i+1];
    for(iint jj= Jstart; jj<Jend; jj++){
      iint col = A->offdCols[jj];
      dfloat Ajj = fabs(diagA[col]);
      dfloat OD = -sign*A->offdCoefs[jj]/(sqrt(Aii)*sqrt(Ajj));
      if(OD > threshold*maxOD[i]) offd_strong_per_row++;
    }

    C->diagRowStarts[i+1] = diag_strong_per_row;
    C->offdRowStarts[i+1] = offd_strong_per_row;
  }

  // cumulative sum
  for(iint i=1; i<N+1 ; i++) {
    C->diagRowStarts[i] += C->diagRowStarts[i-1];
    C->offdRowStarts[i] += C->offdRowStarts[i-1];
  }

  C->diagNNZ = C->diagRowStarts[N];
  C->offdNNZ = C->offdRowStarts[N];

  if (C->diagNNZ) C->diagCols = (iint *) calloc(C->diagNNZ, sizeof(iint));
  if (C->offdNNZ) C->offdCols = (iint *) calloc(C->offdNNZ, sizeof(iint));

  // fill in the columns for strong connections
  for(iint i=0; i<N; i++){
    dfloat sign = (diagA[i] >= 0) ? 1:-1;
    dfloat Aii = fabs(diagA[i]);

    iint diagCounter = C->diagRowStarts[i];
    iint offdCounter = C->offdRowStarts[i];

    //local entries
    C->diagCols[diagCounter++] = i;// diag entry
    iint Jstart = A->diagRowStarts[i], Jend = A->diagRowStarts[i+1];
    for(iint jj = Jstart+1; jj<Jend; jj++){
      iint col = A->diagCols[jj];
      dfloat Ajj = fabs(diagA[col]);
      dfloat OD = -sign*A->diagCoefs[jj]/(sqrt(Aii)*sqrt(Ajj));
      if(OD > threshold*maxOD[i])
        C->diagCols[diagCounter++] = A->diagCols[jj];
    }
    Jstart = A->offdRowStarts[i], Jend = A->offdRowStarts[i+1];
    for(iint jj = Jstart; jj<Jend; jj++){
      iint col = A->offdCols[jj];
      dfloat Ajj = fabs(diagA[col]);
      dfloat OD = -sign*A->offdCoefs[jj]/(sqrt(Aii)*sqrt(Ajj));
      if(OD > threshold*maxOD[i])
        C->offdCols[offdCounter++] = A->offdCols[jj];
    }
  }
  if(N) free(maxOD);

  return C;
}

bool customLess(iint smax, dfloat rmax, iint imax, iint s, dfloat r, iint i){

  if(s > smax) return true;
  if(smax > s) return false;

  if(r > rmax) return true;
  if(rmax > r) return false;

  if(i > imax) return true;
  if(i < imax) return false;

  return false;
}

iint * form_aggregates(agmgLevel *level, csr *C){

  iint rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  const iint N   = C->Nrows;
  const iint M   = C->Ncols;
  const iint diagNNZ = C->diagNNZ;
  const iint offdNNZ = C->offdNNZ;

  iint *FineToCoarse = (iint *) calloc(M, sizeof(iint));
  for (iint i =0;i<M;i++) FineToCoarse[i] = -1;

  dfloat *rands  = (dfloat *) calloc(M, sizeof(dfloat));
  iint   *states = (iint *)   calloc(M, sizeof(iint));

  dfloat *Tr = (dfloat *) calloc(M, sizeof(dfloat));
  iint   *Ts = (iint *)   calloc(M, sizeof(iint));
  iint   *Ti = (iint *)   calloc(M, sizeof(iint));
  iint   *Tc = (iint *)   calloc(M, sizeof(iint));

  csr *A = level->A;
  iint *globalRowStarts = level->globalRowStarts;

  iint *iintSendBuffer;
  dfloat *dfloatSendBuffer;
  if (level->A->NsendTotal) {
    iintSendBuffer = (iint *) calloc(A->NsendTotal,sizeof(iint));
    dfloatSendBuffer = (dfloat *) calloc(A->NsendTotal,sizeof(dfloat));
  }

  for(iint i=0; i<N; i++)
    rands[i] = (dfloat) drand48();

  for(iint i=0; i<N; i++)
    states[i] = 0;

  // add the number of non-zeros in each column
  //local non-zeros
  for(iint i=0; i<diagNNZ; i++)
    rands[C->diagCols[i]] += 1.;

  iint *nnzCnt, *recvNnzCnt;
  if (A->NHalo) nnzCnt = (iint *) calloc(A->NHalo,sizeof(iint));
  if (A->NsendTotal) recvNnzCnt = (iint *) calloc(A->NsendTotal,sizeof(iint));

  //count the non-local non-zeros
  for (iint i=0;i<offdNNZ;i++)
    nnzCnt[C->offdCols[i]-A->NlocalCols]++;

  //do a reverse halo exchange
  iint tag = 999;

  // initiate immediate send  and receives to each other process as needed
  iint recvOffset = 0;
  iint sendOffset = 0;
  iint sendMessage = 0, recvMessage = 0;
  for(iint r=0;r<size;++r){
    if (A->NsendTotal) {
      if(A->NsendPairs[r]) {
        MPI_Irecv(recvNnzCnt+sendOffset, A->NsendPairs[r], MPI_IINT, r, tag,
            MPI_COMM_WORLD, (MPI_Request*)A->haloSendRequests+sendMessage);
        sendOffset += A->NsendPairs[r];
        ++sendMessage;
      }
    }
    if (A->NrecvTotal) {
      if(A->NrecvPairs[r]){
        MPI_Isend(nnzCnt+recvOffset, A->NrecvPairs[r], MPI_IINT, r, tag,
            MPI_COMM_WORLD, (MPI_Request*)A->haloRecvRequests+recvMessage);
        recvOffset += A->NrecvPairs[r];
        ++recvMessage;
      }
    }
  }

  // Wait for all sent messages to have left and received messages to have arrived
  if (A->NrecvTotal) {
    MPI_Status *sendStatus = (MPI_Status*) calloc(A->NsendMessages, sizeof(MPI_Status));
    MPI_Waitall(A->NsendMessages, (MPI_Request*)A->haloSendRequests, sendStatus);
    free(sendStatus);
  }
  if (A->NsendTotal) {
    MPI_Status *recvStatus = (MPI_Status*) calloc(A->NrecvMessages, sizeof(MPI_Status));
    MPI_Waitall(A->NrecvMessages, (MPI_Request*)A->haloRecvRequests, recvStatus);
    free(recvStatus);
  }

  for(iint i=0;i<A->NsendTotal;++i){
    // local index of outgoing element in halo exchange
    iint id = A->haloElementList[i];

    rands[id] += recvNnzCnt[i];
  }

  if (A->NHalo) free(nnzCnt);
  if (A->NsendTotal) free(recvNnzCnt);

  //share randomizer values
  csrHaloExchange(A, sizeof(dfloat), rands, dfloatSendBuffer, rands+A->NlocalCols);



  int done = 0;
  while(!done){
    // first neighbours
    for(iint i=0; i<N; i++){

      iint smax = states[i];
      dfloat rmax = rands[i];
      iint imax = i + globalRowStarts[rank];

      if(smax != 1){
        //local entries
        for(iint jj=C->diagRowStarts[i]+1;jj<C->diagRowStarts[i+1];jj++){
          const iint col = C->diagCols[jj];
          if(customLess(smax, rmax, imax, states[col], rands[col], col + globalRowStarts[rank])){
            smax = states[col];
            rmax = rands[col];
            imax = col + globalRowStarts[rank];
          }
        }
        //nonlocal entries
        for(iint jj=C->offdRowStarts[i];jj<C->offdRowStarts[i+1];jj++){
          const iint col = C->offdCols[jj];
          if(customLess(smax, rmax, imax, states[col], rands[col], A->colMap[col])) {
            smax = states[col];
            rmax = rands[col];
            imax = A->colMap[col];
          }
        }
      }
      Ts[i] = smax;
      Tr[i] = rmax;
      Ti[i] = imax;
    }

    //share results
    csrHaloExchange(A, sizeof(dfloat), Tr, dfloatSendBuffer, Tr+A->NlocalCols);
    csrHaloExchange(A, sizeof(iint), Ts, iintSendBuffer, Ts+A->NlocalCols);
    csrHaloExchange(A, sizeof(iint), Ti, iintSendBuffer, Ti+A->NlocalCols);

    // second neighbours
    for(iint i=0; i<N; i++){
      iint   smax = Ts[i];
      dfloat rmax = Tr[i];
      iint   imax = Ti[i];

      //local entries
      for(iint jj=C->diagRowStarts[i]+1;jj<C->diagRowStarts[i+1];jj++){
        const iint col = C->diagCols[jj];
        if(customLess(smax, rmax, imax, Ts[col], Tr[col], Ti[col])){
          smax = Ts[col];
          rmax = Tr[col];
          imax = Ti[col];
        }
      }
      //nonlocal entries
      for(iint jj=C->offdRowStarts[i];jj<C->offdRowStarts[i+1];jj++){
        const iint col = C->offdCols[jj];
        if(customLess(smax, rmax, imax, Ts[col], Tr[col], Ti[col])){
          smax = Ts[col];
          rmax = Tr[col];
          imax = Ti[col];
        }
      }

      // if I am the strongest among all the 1 and 2 ring neighbours
      // I am an MIS node
      if((states[i] == 0) && (imax == (i + globalRowStarts[rank])))
        states[i] = 1;

      // if there is an MIS node within distance 2, I am removed
      if((states[i] == 0) && (smax == 1))
        states[i] = -1;
    }

    csrHaloExchange(A, sizeof(iint), states, iintSendBuffer, states+A->NlocalCols);

    // if number of undecided nodes = 0, algorithm terminates
    iint cnt = std::count(states, states+N, 0);
    MPI_Allreduce(&cnt,&done,1,MPI_IINT, MPI_SUM,MPI_COMM_WORLD);
    done = (done == 0) ? 1 : 0;
  }

  iint numAggs = 0;
  level->globalAggStarts = (iint *) calloc(size+1,sizeof(iint));
  // count the coarse nodes/aggregates
  for(iint i=0; i<N; i++)
    if(states[i] == 1) numAggs++;

  MPI_Allgather(&numAggs,1,MPI_IINT,level->globalAggStarts+1,1,MPI_IINT,MPI_COMM_WORLD);

  for (iint r=0;r<size;r++)
    level->globalAggStarts[r+1] += level->globalAggStarts[r];

  numAggs = 0;
  // enumerate the coarse nodes/aggregates
  for(iint i=0; i<N; i++)
    if(states[i] == 1)
      FineToCoarse[i] = level->globalAggStarts[rank] + numAggs++;

  //share the initial aggregate flags
  csrHaloExchange(A, sizeof(iint), FineToCoarse, iintSendBuffer, FineToCoarse+A->NlocalCols);

  // form the aggregates
  for(iint i=0; i<N; i++){
    iint   smax = states[i];
    dfloat rmax = rands[i];
    iint   imax = i + globalRowStarts[rank];
    iint   cmax = FineToCoarse[i];

    if(smax != 1){
      //local entries
      for(iint jj=C->diagRowStarts[i]+1;jj<C->diagRowStarts[i+1];jj++){
        const iint col = C->diagCols[jj];
        if(customLess(smax, rmax, imax, states[col], rands[col], col + globalRowStarts[rank])){
          smax = states[col];
          rmax = rands[col];
          imax = col + globalRowStarts[rank];
          cmax = FineToCoarse[col];
        }
      }
      //nonlocal entries
      for(iint jj=C->offdRowStarts[i];jj<C->offdRowStarts[i+1];jj++){
        const iint col = C->offdCols[jj];
        if(customLess(smax, rmax, imax, states[col], rands[col], A->colMap[col])){
          smax = states[col];
          rmax = rands[col];
          imax = A->colMap[col];
          cmax = FineToCoarse[col];
        }
      }
    }
    Ts[i] = smax;
    Tr[i] = rmax;
    Ti[i] = imax;
    Tc[i] = cmax;

    if((states[i] == -1) && (smax == 1) && (cmax > -1))
      FineToCoarse[i] = cmax;
  }

  csrHaloExchange(A, sizeof(iint), FineToCoarse, iintSendBuffer, FineToCoarse+A->NlocalCols);
  csrHaloExchange(A, sizeof(dfloat), Tr, dfloatSendBuffer, Tr+A->NlocalCols);
  csrHaloExchange(A, sizeof(iint), Ts, iintSendBuffer, Ts+A->NlocalCols);
  csrHaloExchange(A, sizeof(iint), Ti, iintSendBuffer, Ti+A->NlocalCols);
  csrHaloExchange(A, sizeof(iint), Tc, iintSendBuffer, Tc+A->NlocalCols);

  // second neighbours
  for(iint i=0; i<N; i++){
    iint   smax = Ts[i];
    dfloat rmax = Tr[i];
    iint   imax = Ti[i];
    iint   cmax = Tc[i];

    //local entries
    for(iint jj=C->diagRowStarts[i]+1;jj<C->diagRowStarts[i+1];jj++){
      const iint col = C->diagCols[jj];
      if(customLess(smax, rmax, imax, Ts[col], Tr[col], Ti[col])){
        smax = Ts[col];
        rmax = Tr[col];
        imax = Ti[col];
        cmax = Tc[col];
      }
    }
    //nonlocal entries
    for(iint jj=C->offdRowStarts[i];jj<C->offdRowStarts[i+1];jj++){
      const iint col = C->offdCols[jj];
      if(customLess(smax, rmax, imax, Ts[col], Tr[col], Ti[col])){
        smax = Ts[col];
        rmax = Tr[col];
        imax = Ti[col];
        cmax = Tc[col];
      }
    }

    if((states[i] == -1) && (smax == 1) && (cmax > -1))
      FineToCoarse[i] = cmax;
  }

  csrHaloExchange(A, sizeof(iint), FineToCoarse, iintSendBuffer, FineToCoarse+A->NlocalCols);

  free(rands);
  free(states);
  free(Tr);
  free(Ts);
  free(Ti);
  free(Tc);
  if (level->A->NsendTotal) {
    free(iintSendBuffer);
    free(dfloatSendBuffer);
  }

  //TODO maybe free C here?

  return FineToCoarse;
}

typedef struct {

  iint fineId;
  iint coarseId;
  iint newCoarseId;

  iint orginRank;
  iint ownerRank;

} parallelAggregate_t;

int compareOwner(const void *a, const void *b){
  parallelAggregate_t *pa = (parallelAggregate_t *) a;
  parallelAggregate_t *pb = (parallelAggregate_t *) b;

  if (pa->ownerRank < pb->ownerRank) return -1;
  if (pa->ownerRank > pb->ownerRank) return +1;

  return 0;
};

int compareAgg(const void *a, const void *b){
  parallelAggregate_t *pa = (parallelAggregate_t *) a;
  parallelAggregate_t *pb = (parallelAggregate_t *) b;

  if (pa->coarseId < pb->coarseId) return -1;
  if (pa->coarseId > pb->coarseId) return +1;

  if (pa->orginRank < pb->orginRank) return -1;
  if (pa->orginRank > pb->orginRank) return +1;

  return 0;
};

void find_aggregate_owners(agmgLevel *level, iint* FineToCoarse) {
  // MPI info
  iint rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  iint N = level->A->Nrows;

  //Need to establish 'ownership' of aggregates
  //populate aggregate array
  iint gNumAggs = level->globalAggStarts[size]; //total number of aggregates
  parallelAggregate_t *sendAggs;
  if (N) sendAggs = (parallelAggregate_t *) calloc(N,sizeof(parallelAggregate_t));
  for (iint i=0;i<N;i++) {
    sendAggs[i].fineId = i;
    sendAggs[i].orginRank = rank;

    sendAggs[i].coarseId = FineToCoarse[i];
    //set a temporary owner. Evenly distibute aggregates amoungst ranks
    sendAggs[i].ownerRank = (FineToCoarse[i]*size)/gNumAggs;
  }

  //sort by owning rank for all_reduce
  qsort(sendAggs, N, sizeof(parallelAggregate_t), compareOwner);

  iint *sendCounts = (iint *) calloc(size,sizeof(iint));
  iint *recvCounts = (iint *) calloc(size,sizeof(iint));
  iint *sendOffsets = (iint *) calloc(size+1,sizeof(iint));
  iint *recvOffsets = (iint *) calloc(size+1,sizeof(iint));

  for(iint i=0;i<N;++i)
    sendCounts[sendAggs[i].ownerRank] += sizeof(parallelAggregate_t);

  // find how many nodes to expect (should use sparse version)
  MPI_Alltoall(sendCounts, 1, MPI_IINT, recvCounts, 1, MPI_IINT, MPI_COMM_WORLD);

  // find send and recv offsets for gather
  iint recvNtotal = 0;
  for(iint r=0;r<size;++r){
    sendOffsets[r+1] = sendOffsets[r] + sendCounts[r];
    recvOffsets[r+1] = recvOffsets[r] + recvCounts[r];
    recvNtotal += recvCounts[r]/sizeof(parallelAggregate_t);
  }
  parallelAggregate_t *recvAggs = (parallelAggregate_t *) calloc(recvNtotal,sizeof(parallelAggregate_t));

  MPI_Alltoallv(sendAggs, sendCounts, sendOffsets, MPI_CHAR,
                recvAggs, recvCounts, recvOffsets, MPI_CHAR,
                MPI_COMM_WORLD);

  //sort by coarse aggregate number, and then by original rank
  qsort(recvAggs, recvNtotal, sizeof(parallelAggregate_t), compareAgg);

  //count the number of unique aggregates here
  iint NumUniqueAggs =0;
  if (recvNtotal) NumUniqueAggs++;
  for (iint i=1;i<recvNtotal;i++)
    if(recvAggs[i].coarseId!=recvAggs[i-1].coarseId) NumUniqueAggs++;

  //get their locations in the array
  iint *aggStarts;
  if (NumUniqueAggs)
    aggStarts = (iint *) calloc(NumUniqueAggs+1,sizeof(iint));
  iint cnt = 1;
  for (iint i=1;i<recvNtotal;i++)
    if(recvAggs[i].coarseId!=recvAggs[i-1].coarseId) aggStarts[cnt++]=i;
  aggStarts[NumUniqueAggs] = recvNtotal;

  //use a random dfloat for each rank to break ties.
  dfloat rand = (dfloat) drand48();
  dfloat *gRands = (dfloat *) calloc(size,sizeof(dfloat));
  MPI_Allgather(&rand, 1, MPI_DFLOAT, gRands, 1, MPI_DFLOAT, MPI_COMM_WORLD);

  //determine the aggregates majority owner
  dfloat *rankCounts = (dfloat *) calloc(size,sizeof(dfloat));
  for (iint n=0;n<NumUniqueAggs;n++) {
    //populate randomizer
    for (iint r=0;r>size;r++)
      rankCounts[r] = gRands[r];

    //count the number of contributions to the aggregate from the separate ranks
    for (iint i=aggStarts[n];i<aggStarts[n+1];i++)
      rankCounts[recvAggs[i].orginRank]++;

    //find which rank is contributing the most to this aggregate
    iint ownerRank = 0;
    dfloat maxEntries = rankCounts[0];
    for (iint r=1;r<size;r++) {
      if (rankCounts[r]>maxEntries) {
        ownerRank = r;
        maxEntries = rankCounts[r];
      }
    }

    //set this aggregate's owner
    for (iint i=aggStarts[n];i<aggStarts[n+1];i++)
      recvAggs[i].ownerRank = ownerRank;
  }
  free(gRands); free(rankCounts);
  free(aggStarts);

  //sort by owning rank
  qsort(recvAggs, recvNtotal, sizeof(parallelAggregate_t), compareOwner);

  iint *newSendCounts = (iint *) calloc(size,sizeof(iint));
  iint *newRecvCounts = (iint *) calloc(size,sizeof(iint));
  iint *newSendOffsets = (iint *) calloc(size+1,sizeof(iint));
  iint *newRecvOffsets = (iint *) calloc(size+1,sizeof(iint));

  for(iint i=0;i<recvNtotal;++i)
    newSendCounts[recvAggs[i].ownerRank] += sizeof(parallelAggregate_t);

  // find how many nodes to expect (should use sparse version)
  MPI_Alltoall(newSendCounts, 1, MPI_IINT, newRecvCounts, 1, MPI_IINT, MPI_COMM_WORLD);

  // find send and recv offsets for gather
  iint newRecvNtotal = 0;
  for(iint r=0;r<size;++r){
    newSendOffsets[r+1] = newSendOffsets[r] + newSendCounts[r];
    newRecvOffsets[r+1] = newRecvOffsets[r] + newRecvCounts[r];
    newRecvNtotal += newRecvCounts[r]/sizeof(parallelAggregate_t);
  }
  parallelAggregate_t *newRecvAggs = (parallelAggregate_t *) calloc(newRecvNtotal,sizeof(parallelAggregate_t));

  MPI_Alltoallv(   recvAggs, newSendCounts, newSendOffsets, MPI_CHAR,
                newRecvAggs, newRecvCounts, newRecvOffsets, MPI_CHAR,
                MPI_COMM_WORLD);

  //sort by coarse aggregate number, and then by original rank
  qsort(newRecvAggs, newRecvNtotal, sizeof(parallelAggregate_t), compareAgg);

  //count the number of unique aggregates this rank owns
  iint numAggs = 0;
  if (newRecvNtotal) numAggs++;
  for (iint i=1;i<newRecvNtotal;i++)
    if(newRecvAggs[i].coarseId!=newRecvAggs[i-1].coarseId) numAggs++;

  //determine a global numbering of the aggregates
  MPI_Allgather(&numAggs, 1, MPI_IINT, level->globalAggStarts+1, 1, MPI_IINT, MPI_COMM_WORLD);

  level->globalAggStarts[size] =0;
  for (iint r=0;r<size;r++)
    level->globalAggStarts[r+1] += level->globalAggStarts[r];

  //set the new global coarse index
  cnt = level->globalAggStarts[rank];
  if (newRecvNtotal) newRecvAggs[0].newCoarseId = cnt;
  for (iint i=1;i<newRecvNtotal;i++) {
    if(newRecvAggs[i].coarseId!=newRecvAggs[i-1].coarseId) cnt++;

    newRecvAggs[i].newCoarseId = cnt;
  }

  //send the aggregate data back
  MPI_Alltoallv(newRecvAggs, newRecvCounts, newRecvOffsets, MPI_CHAR,
                   recvAggs, newSendCounts, newSendOffsets, MPI_CHAR,
                MPI_COMM_WORLD);
  MPI_Alltoallv(recvAggs, recvCounts, recvOffsets, MPI_CHAR,
                sendAggs, sendCounts, sendOffsets, MPI_CHAR,
                MPI_COMM_WORLD);

  free(recvAggs);
  free(sendCounts);  free(recvCounts);
  free(sendOffsets); free(recvOffsets);
  free(newRecvAggs);
  free(newSendCounts);  free(newRecvCounts);
  free(newSendOffsets); free(newRecvOffsets);

  //record the new FineToCoarse map
  for (iint i=0;i<N;i++)
    FineToCoarse[sendAggs[i].fineId] = sendAggs[i].newCoarseId;

  if (N) free(sendAggs);
}


csr *construct_interpolator(agmgLevel *level, iint *FineToCoarse, dfloat **nullCoarseA){
  // MPI info
  iint rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  const iint N = level->A->Nrows;
  const iint M = level->A->Ncols;

  iint *globalAggStarts = level->globalAggStarts;

  const iint globalAggOffset = level->globalAggStarts[rank];
  const iint NCoarse = globalAggStarts[rank+1]-globalAggStarts[rank]; //local num agg

  csr* P = (csr *) calloc(1, sizeof(csr));

  P->Nrows = N;
  P->Ncols = NCoarse;

  P->NlocalCols = NCoarse;
  P->NHalo = 0;

  P->diagRowStarts = (iint *) calloc(N+1, sizeof(iint));
  P->offdRowStarts = (iint *) calloc(N+1, sizeof(iint));

  // each row has exactly one nonzero per row
  P->diagNNZ =0;
  P->offdNNZ =0;
  for(iint i=0; i<N; i++) {
    iint col = FineToCoarse[i];
    if ((col>globalAggOffset-1)&&(col<globalAggOffset+NCoarse)) {
      P->diagNNZ++;
      P->diagRowStarts[i+1]++;
    } else {
      P->offdNNZ++;
      P->offdRowStarts[i+1]++;
    }
  }
  for(iint i=0; i<N; i++) {
    P->diagRowStarts[i+1] += P->diagRowStarts[i];
    P->offdRowStarts[i+1] += P->offdRowStarts[i];
  }

  if (P->diagNNZ) {
    P->diagCols  = (iint *)   calloc(P->diagNNZ, sizeof(iint));
    P->diagCoefs = (dfloat *) calloc(P->diagNNZ, sizeof(dfloat));
  }
  if (P->offdNNZ) {
    P->offdCols  = (iint *)   calloc(P->offdNNZ, sizeof(iint));
    P->offdCoefs = (dfloat *) calloc(P->offdNNZ, sizeof(dfloat));
  }

  iint diagCnt = 0;
  iint offdCnt = 0;
  for(iint i=0; i<N; i++) {
    iint col = FineToCoarse[i];
    if ((col>globalAggStarts[rank]-1)&&(col<globalAggStarts[rank+1])) {
      P->diagCols[diagCnt] = col - globalAggOffset; //local index
      P->diagCoefs[diagCnt++] = level->nullA[i];
    } else {
      P->offdCols[offdCnt] = col;
      P->offdCoefs[offdCnt++] = level->nullA[i];
    }
  }

  //record global indexing of columns
  P->colMap = (iint *)   calloc(P->Ncols, sizeof(iint));
  for (iint i=0;i<P->Ncols;i++)
    P->colMap[i] = i + globalAggOffset;

  if (P->offdNNZ) {
    //we now need to reorder the x vector for the halo, and shift the column indices
    iint *col = (iint *) calloc(P->offdNNZ,sizeof(iint));
    for (iint i=0;i<P->offdNNZ;i++)
      col[i] = P->offdCols[i]; //copy non-local column global ids

    //sort by global index
    std::sort(col,col+P->offdNNZ);

    //count unique non-local column ids
    P->NHalo = 0;
    for (iint i=1;i<P->offdNNZ;i++)
      if (col[i]!=col[i-1])  col[++P->NHalo] = col[i];
    P->NHalo++; //number of unique columns

    P->Ncols += P->NHalo;

    //save global column ids in colMap
    P->colMap = (iint *) realloc(P->colMap, P->Ncols*sizeof(iint));
    for (iint i=0; i<P->NHalo; i++)
      P->colMap[i+P->NlocalCols] = col[i];
    free(col);

    //shift the column indices to local indexing
    for (iint i=0;i<P->offdNNZ;i++) {
      iint gcol = P->offdCols[i];
      for (iint m=P->NlocalCols;m<P->Ncols;m++) {
        if (gcol == P->colMap[m])
          P->offdCols[i] = m;
      }
    }
  }

  csrHaloSetup(P,globalAggStarts);

  // normalize the columns of P
  *nullCoarseA = (dfloat *) calloc(P->Ncols,sizeof(dfloat));

  //add local nonzeros
  for(iint i=0; i<P->diagNNZ; i++)
    (*nullCoarseA)[P->diagCols[i]] += P->diagCoefs[i] * P->diagCoefs[i];

  dfloat *nnzSum, *recvNnzSum;
  if (P->NHalo) nnzSum = (dfloat *) calloc(P->NHalo,sizeof(dfloat));
  if (P->NsendTotal) recvNnzSum = (dfloat *) calloc(P->NsendTotal,sizeof(dfloat));

  //add the non-local non-zeros
  for (iint i=0;i<P->offdNNZ;i++)
    nnzSum[P->offdCols[i]-P->NlocalCols] += P->offdCoefs[i] * P->offdCoefs[i];

  //do a reverse halo exchange
  iint tag = 999;

  // initiate immediate send  and receives to each other process as needed
  iint recvOffset = 0;
  iint sendOffset = 0;
  iint sendMessage = 0, recvMessage = 0;
  for(iint r=0;r<size;++r){
    if (P->NsendTotal) {
      if(P->NsendPairs[r]) {
        MPI_Irecv(recvNnzSum+sendOffset, P->NsendPairs[r], MPI_DFLOAT, r, tag,
            MPI_COMM_WORLD, (MPI_Request*)P->haloSendRequests+sendMessage);
        sendOffset += P->NsendPairs[r];
        ++sendMessage;
      }
    }
    if (P->NrecvTotal) {
      if(P->NrecvPairs[r]){
        MPI_Isend(nnzSum+recvOffset, P->NrecvPairs[r], MPI_DFLOAT, r, tag,
            MPI_COMM_WORLD, (MPI_Request*)P->haloRecvRequests+recvMessage);
        recvOffset += P->NrecvPairs[r];
        ++recvMessage;
      }
    }
  }

  // Wait for all sent messages to have left and received messages to have arrived
  if (P->NrecvTotal) {
    MPI_Status *sendStatus = (MPI_Status*) calloc(P->NsendMessages, sizeof(MPI_Status));
    MPI_Waitall(P->NsendMessages, (MPI_Request*)P->haloSendRequests, sendStatus);
    free(sendStatus);
  }
  if (P->NsendTotal) {
    MPI_Status *recvStatus = (MPI_Status*) calloc(P->NrecvMessages, sizeof(MPI_Status));
    MPI_Waitall(P->NrecvMessages, (MPI_Request*)P->haloRecvRequests, recvStatus);
    free(recvStatus);
  }

  for(iint i=0;i<P->NsendTotal;++i){
    // local index of outgoing element in halo exchange
    iint id = P->haloElementList[i];

    (*nullCoarseA)[id] += recvNnzSum[i];
  }

  if (P->NHalo) free(nnzSum);

  for(iint i=0; i<NCoarse; i++)
    (*nullCoarseA)[i] = sqrt((*nullCoarseA)[i]);

  csrHaloExchange(P, sizeof(dfloat), *nullCoarseA, P->sendBuffer, *nullCoarseA+P->NlocalCols);

  for(iint i=0; i<P->diagNNZ; i++)
    P->diagCoefs[i] /= (*nullCoarseA)[P->diagCols[i]];
  for(iint i=0; i<P->offdNNZ; i++)
    P->offdCoefs[i] /= (*nullCoarseA)[P->offdCols[i]];

  if (P->NsendTotal) free(recvNnzSum);

  return P;
}

typedef struct {

  iint row;
  iint col;
  dfloat val;
  iint owner;

} nonzero_t;

int compareNonZero(const void *a, const void *b){
  nonzero_t *pa = (nonzero_t *) a;
  nonzero_t *pb = (nonzero_t *) b;

  if (pa->owner < pb->owner) return -1;
  if (pa->owner > pb->owner) return +1;

  if (pa->row < pb->row) return -1;
  if (pa->row > pb->row) return +1;

  if (pa->col < pb->col) return -1;
  if (pa->col > pb->col) return +1;

  return 0;
};

csr * transpose(agmgLevel* level, csr *A,
                iint *globalRowStarts, iint *globalColStarts){

  // MPI info
  iint rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  csr *At = (csr *) calloc(1,sizeof(csr));

  At->Nrows = A->Ncols-A->NHalo;
  At->Ncols = A->Nrows;
  At->diagNNZ   = A->diagNNZ; //local entries remain local

  At->NlocalCols = At->Ncols;

  At->diagRowStarts = (iint *)   calloc(At->Nrows+1, sizeof(iint));
  At->offdRowStarts = (iint *)   calloc(At->Nrows+1, sizeof(iint));

  //start with local entries
  if (A->diagNNZ) {
    At->diagCols      = (iint *)   calloc(At->diagNNZ, sizeof(iint));
    At->diagCoefs     = (dfloat *) calloc(At->diagNNZ, sizeof(dfloat));
  }

  // count the num of nonzeros per row for transpose
  for(iint i=0; i<A->diagNNZ; i++){
    iint row = A->diagCols[i];
    At->diagRowStarts[row+1]++;
  }

  // cumulative sum for rows
  for(iint i=1; i<=At->Nrows; i++)
    At->diagRowStarts[i] += At->diagRowStarts[i-1];

  iint *counter = (iint *) calloc(At->Nrows+1,sizeof(iint));
  for (iint i=0; i<At->Nrows+1; i++)
    counter[i] = At->diagRowStarts[i];

  for(iint i=0; i<A->Nrows; i++){
    const iint Jstart = A->diagRowStarts[i], Jend = A->diagRowStarts[i+1];

    for(iint jj=Jstart; jj<Jend; jj++){
      iint row = A->diagCols[jj];
      At->diagCols[counter[row]]  = i;
      At->diagCoefs[counter[row]] = A->diagCoefs[jj];

      counter[row]++;
    }
  }
  free(counter);

  //record global indexing of columns
  At->colMap = (iint *)   calloc(At->Ncols, sizeof(iint));
  for (iint i=0;i<At->Ncols;i++)
    At->colMap[i] = i + globalRowStarts[rank];

  //now the nonlocal entries. Need to reverse the halo exchange to send the nonzeros
  iint tag = 999;

  nonzero_t *sendNonZeros;
  if (A->offdNNZ)
    sendNonZeros = (nonzero_t *) calloc(A->offdNNZ,sizeof(nonzero_t));

  iint *Nsend = (iint*) calloc(size, sizeof(iint));
  iint *Nrecv = (iint*) calloc(size, sizeof(iint));

  for(iint r=0;r<size;r++) {
    Nsend[r] =0;
    Nrecv[r] =0;
  }

  // copy data from nonlocal entries into send buffer
  for(iint i=0;i<A->Nrows;++i){
    for (iint j=A->offdRowStarts[i];j<A->offdRowStarts[i+1];j++) {
      iint col =  A->colMap[A->offdCols[j]]; //global ids
      for (iint r=0;r<size;r++) { //find owner's rank
        if ((globalColStarts[r]-1<col) && (col < globalColStarts[r+1])) {
          Nsend[r]++;
          sendNonZeros[j].owner = r;
        }
      }
      sendNonZeros[j].row = col;
      sendNonZeros[j].col = i + globalRowStarts[rank];     //global ids
      sendNonZeros[j].val = A->offdCoefs[j];
    }
  }

  //sort outgoing nonzeros by owner, then row and col
  if (A->offdNNZ)
    qsort(sendNonZeros, A->offdNNZ, sizeof(nonzero_t), compareNonZero);

  MPI_Alltoall(Nsend, 1, MPI_IINT, Nrecv, 1, MPI_IINT, MPI_COMM_WORLD);

  //count incoming nonzeros
  At->offdNNZ = 0;
  for (iint r=0;r<size;r++)
    At->offdNNZ += Nrecv[r];

  nonzero_t *recvNonZeros;
  if (At->offdNNZ)
    recvNonZeros = (nonzero_t *) calloc(At->offdNNZ,sizeof(nonzero_t));

  // initiate immediate send and receives to each other process as needed
  iint recvOffset = 0;
  iint sendOffset = 0;
  iint sendMessage = 0, recvMessage = 0;
  for(iint r=0;r<size;++r){
    if (At->offdNNZ) {
      if(Nrecv[r]) {
        MPI_Irecv(((char*)recvNonZeros)+recvOffset, Nrecv[r]*sizeof(nonzero_t),
                      MPI_CHAR, r, tag, MPI_COMM_WORLD,
                      (MPI_Request*)A->haloSendRequests+recvMessage);
        recvOffset += Nrecv[r]*sizeof(nonzero_t);
        ++recvMessage;
      }
    }
    if (A->offdNNZ) {
      if(Nsend[r]){
        MPI_Isend(((char*)sendNonZeros)+sendOffset, Nsend[r]*sizeof(nonzero_t),
                      MPI_CHAR, r, tag, MPI_COMM_WORLD,
                      (MPI_Request*)A->haloRecvRequests+sendMessage);
        sendOffset += Nsend[r]*sizeof(nonzero_t);
        ++sendMessage;
      }
    }
  }

  // Wait for all sent messages to have left and received messages to have arrived
  if (A->offdNNZ) {
    MPI_Status *sendStatus = (MPI_Status*) calloc(sendMessage, sizeof(MPI_Status));
    MPI_Waitall(sendMessage, (MPI_Request*)A->haloRecvRequests, sendStatus);
    free(sendStatus);
  }
  if (At->offdNNZ) {
    MPI_Status *recvStatus = (MPI_Status*) calloc(recvMessage, sizeof(MPI_Status));
    MPI_Waitall(recvMessage, (MPI_Request*)A->haloSendRequests, recvStatus);
    free(recvStatus);
  }
  if (A->offdNNZ) free(sendNonZeros);

  //free(Nsend); free(Nrecv);

  if (At->offdNNZ) {
    //sort recieved nonzeros by row and col
    qsort(recvNonZeros, At->offdNNZ, sizeof(nonzero_t), compareNonZero);

    At->offdCols  = (iint *)   calloc(At->offdNNZ,sizeof(iint));
    At->offdCoefs = (dfloat *) calloc(At->offdNNZ, sizeof(dfloat));

    //find row starts
    for(iint n=0;n<At->offdNNZ;++n) {
      iint row = recvNonZeros[n].row - globalColStarts[rank];
      At->offdRowStarts[row+1]++;
    }
    //cumulative sum
    for (iint i=0;i<At->Nrows;i++)
      At->offdRowStarts[i+1] += At->offdRowStarts[i];

    //fill cols and coefs
    for (iint i=0; i<At->Nrows; i++) {
      for (iint j=At->offdRowStarts[i]; j<At->offdRowStarts[i+1]; j++) {
        At->offdCols[j]  = recvNonZeros[j].col;
        At->offdCoefs[j] = recvNonZeros[j].val;
      }
    }
    free(recvNonZeros);

    //we now need to reorder the x vector for the halo, and shift the column indices
    iint *col = (iint *) calloc(At->offdNNZ,sizeof(iint));
    for (iint n=0;n<At->offdNNZ;n++)
      col[n] = At->offdCols[n]; //copy non-local column global ids

    //sort by global index
    std::sort(col,col+At->offdNNZ);

    //count unique non-local column ids
    At->NHalo = 0;
    for (iint n=1;n<At->offdNNZ;n++)
      if (col[n]!=col[n-1])  col[++At->NHalo] = col[n];
    At->NHalo++; //number of unique columns

    At->Ncols += At->NHalo;

    //save global column ids in colMap
    At->colMap = (iint *) realloc(At->colMap,At->Ncols*sizeof(iint));
    for (iint n=0; n<At->NHalo; n++)
      At->colMap[n+At->NlocalCols] = col[n];
    free(col);

    //shift the column indices to local indexing
    for (iint n=0;n<At->offdNNZ;n++) {
      iint gcol = At->offdCols[n];
      for (iint m=At->NlocalCols;m<At->Ncols;m++) {
        if (gcol == At->colMap[m])
          At->offdCols[n] = m;
      }
    }
  }

  csrHaloSetup(At,globalRowStarts);

  return At;
}

typedef struct {

  iint coarseId;
  dfloat coef;

} pEntry_t;

typedef struct {

  iint I;
  iint J;
  dfloat coef;

} rapEntry_t;

int compareRAPEntries(const void *a, const void *b){
  rapEntry_t *pa = (rapEntry_t *) a;
  rapEntry_t *pb = (rapEntry_t *) b;

  if (pa->I < pb->I) return -1;
  if (pa->I > pb->I) return +1;

  if (pa->J < pb->J) return -1;
  if (pa->J > pb->J) return +1;

  return 0;
};

csr *galerkinProd(agmgLevel *level, csr *R, csr *A, csr *P){

  // MPI info
  iint rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  iint *globalAggStarts = level->globalAggStarts;
  iint *globalRowStarts = level->globalRowStarts;

  iint globalAggOffset = globalAggStarts[rank];

  MPI_Datatype mpi_rapEntry_t, oldtypes[2];
  int blockcounts[2];

  MPI_Aint    rapEntryoffsets[2], extent;

  /* Setup description of the 4 MPI_FLOAT fields x, y, z, velocity */
  rapEntryoffsets[0] = 0;
  oldtypes[0] = MPI_IINT;
  blockcounts[0] = 2;

  MPI_Type_extent(MPI_IINT, &extent);
  rapEntryoffsets[1] = 2 * extent;
  oldtypes[1] = MPI_DFLOAT;
  blockcounts[1] = 1;

  /* Now define structured type and commit it */
  MPI_Type_struct(2, blockcounts, rapEntryoffsets, oldtypes, &mpi_rapEntry_t);
  MPI_Type_commit(&mpi_rapEntry_t);

  //The galerkin product can be computed as
  // (RAP)_IJ = sum_{i in Agg_I} sum_{j in Agg_j} P_iI A_ij P_jJ
  // Since each row of P has only one entry, we can share the ncessary
  // P entries, form the products, and send them to their destination rank

  iint N = A->Nrows;
  iint M = A->Ncols;

  printf("Level has %d rows, and is making %d aggregates\n", N, globalAggStarts[rank+1]-globalAggStarts[rank]);

  pEntry_t *PEntries;
  if (M) PEntries = (pEntry_t *) calloc(M,sizeof(pEntry_t));

  //record the entries of P that this rank has
  iint cnt =0;
  for (iint i=0;i<N;i++) {
    for (iint j=P->diagRowStarts[i];j<P->diagRowStarts[i+1];j++) {
      PEntries[cnt].coarseId = P->diagCols[j] + globalAggOffset; //global ID
      PEntries[cnt].coef = P->diagCoefs[j];
      cnt++;
    }
    for (iint j=P->offdRowStarts[i];j<P->offdRowStarts[i+1];j++) {
      PEntries[cnt].coarseId = P->colMap[P->offdCols[j]]; //global ID
      PEntries[cnt].coef = P->offdCoefs[j];
      cnt++;
    }
  }

  pEntry_t *entrySendBuffer;
  if (A->NsendTotal)
    entrySendBuffer = (pEntry_t *) calloc(A->NsendTotal,sizeof(pEntry_t));

  //fill in the entires of P needed in the halo
  csrHaloExchange(A, sizeof(pEntry_t), PEntries, entrySendBuffer, PEntries+A->NlocalCols);
  if (A->NsendTotal) free(entrySendBuffer);

  rapEntry_t *RAPEntries;
  iint totalNNZ = A->diagNNZ+A->offdNNZ;
  if (totalNNZ) {
    RAPEntries = (rapEntry_t *) calloc(totalNNZ,sizeof(rapEntry_t));
  } else {
    RAPEntries = (rapEntry_t *) calloc(1,sizeof(rapEntry_t)); //MPI_AlltoAll doesnt like null pointers
  }

  //for the RAP products
  cnt =0;
  for (iint i=0;i<N;i++) {
    for (iint j=A->diagRowStarts[i];j<A->diagRowStarts[i+1];j++) {
      iint col  = A->diagCols[j];
      dfloat coef = A->diagCoefs[j];

      RAPEntries[cnt].I = PEntries[i].coarseId;
      RAPEntries[cnt].J = PEntries[col].coarseId;
      RAPEntries[cnt].coef = coef*PEntries[i].coef*PEntries[col].coef;
      cnt++;
    }
  }
  for (iint i=0;i<N;i++) {
    for (iint j=A->offdRowStarts[i];j<A->offdRowStarts[i+1];j++) {
      iint col  = A->offdCols[j];
      dfloat coef = A->offdCoefs[j];

      RAPEntries[cnt].I = PEntries[i].coarseId;
      RAPEntries[cnt].J = PEntries[col].coarseId;
      RAPEntries[cnt].coef = PEntries[i].coef*coef*PEntries[col].coef;
      cnt++;
    }
  }

  //sort entries by the coarse row and col
  if (totalNNZ) qsort(RAPEntries, totalNNZ, sizeof(rapEntry_t), compareRAPEntries);

  iint *sendCounts = (iint *) calloc(size,sizeof(iint));
  iint *recvCounts = (iint *) calloc(size,sizeof(iint));
  iint *sendOffsets = (iint *) calloc(size+1,sizeof(iint));
  iint *recvOffsets = (iint *) calloc(size+1,sizeof(iint));

  for(iint i=0;i<totalNNZ;++i) {
    iint id = RAPEntries[i].I;
    for (iint r=0;r<size;r++) {
      if (globalAggStarts[r]-1<id && id < globalAggStarts[r+1])
        sendCounts[r]++;
    }
  }

  // find how many nodes to expect (should use sparse version)
  MPI_Alltoall(sendCounts, 1, MPI_IINT, recvCounts, 1, MPI_IINT, MPI_COMM_WORLD);

  // find send and recv offsets for gather
  iint recvNtotal = 0;
  for(iint r=0;r<size;++r){
    sendOffsets[r+1] = sendOffsets[r] + sendCounts[r];
    recvOffsets[r+1] = recvOffsets[r] + recvCounts[r];
    recvNtotal += recvCounts[r];
  }
  rapEntry_t *recvRAPEntries;
  if (recvNtotal) {
    recvRAPEntries = (rapEntry_t *) calloc(recvNtotal,sizeof(rapEntry_t));
  } else {
    recvRAPEntries = (rapEntry_t *) calloc(1,sizeof(rapEntry_t));//MPI_AlltoAll doesnt like null pointers
  }

  MPI_Alltoallv(RAPEntries, sendCounts, sendOffsets, mpi_rapEntry_t,
                recvRAPEntries, recvCounts, recvOffsets, mpi_rapEntry_t,
                MPI_COMM_WORLD);

  //sort entries by the coarse row and col
  if (recvNtotal) qsort(recvRAPEntries, recvNtotal, sizeof(rapEntry_t), compareRAPEntries);

  //count total number of nonzeros;
  iint nnz =0;
  if (recvNtotal) nnz++;
  for (iint i=1;i<recvNtotal;i++)
    if ((recvRAPEntries[i].I!=recvRAPEntries[i-1].I)||
          (recvRAPEntries[i].J!=recvRAPEntries[i-1].J)) nnz++;

  rapEntry_t *newRAPEntries;
  if (nnz) {
    newRAPEntries = (rapEntry_t *) calloc(nnz,sizeof(rapEntry_t));
  } else {
    newRAPEntries = (rapEntry_t *) calloc(1,sizeof(rapEntry_t));
  }

  //compress nonzeros
  nnz = 0;
  if (recvNtotal) newRAPEntries[nnz++] = recvRAPEntries[0];
  for (iint i=1;i<recvNtotal;i++) {
    if ((recvRAPEntries[i].I!=recvRAPEntries[i-1].I)||
          (recvRAPEntries[i].J!=recvRAPEntries[i-1].J)) {
      newRAPEntries[nnz++] = recvRAPEntries[i];
    } else {
      newRAPEntries[nnz-1].coef += recvRAPEntries[i].coef;
    }
  }

  iint numAggs = globalAggStarts[rank+1]-globalAggStarts[rank]; //local number of aggregates

  csr *RAP = (csr*) calloc(1,sizeof(csr));

  RAP->Nrows = numAggs;
  RAP->Ncols = numAggs;

  RAP->NlocalCols = numAggs;

  RAP->diagRowStarts = (iint *) calloc(numAggs+1, sizeof(iint));
  RAP->offdRowStarts = (iint *) calloc(numAggs+1, sizeof(iint));

  for (iint n=0;n<nnz;n++) {
    iint row = newRAPEntries[n].I - globalAggOffset;
    if ((newRAPEntries[n].J > globalAggStarts[rank]-1)&&
          (newRAPEntries[n].J < globalAggStarts[rank+1])) {
      RAP->diagRowStarts[row+1]++;
    } else {
      RAP->offdRowStarts[row+1]++;
    }
  }

  // cumulative sum
  for(iint i=0; i<numAggs; i++) {
    RAP->diagRowStarts[i+1] += RAP->diagRowStarts[i];
    RAP->offdRowStarts[i+1] += RAP->offdRowStarts[i];
  }
  RAP->diagNNZ = RAP->diagRowStarts[numAggs];
  RAP->offdNNZ = RAP->offdRowStarts[numAggs];

  iint *diagCols;
  dfloat *diagCoefs;
  if (RAP->diagNNZ) {
    RAP->diagCols  = (iint *)   calloc(RAP->diagNNZ, sizeof(iint));
    RAP->diagCoefs = (dfloat *) calloc(RAP->diagNNZ, sizeof(dfloat));
    diagCols  = (iint *)   calloc(RAP->diagNNZ, sizeof(iint));
    diagCoefs = (dfloat *) calloc(RAP->diagNNZ, sizeof(dfloat));
  }
  if (RAP->offdNNZ) {
    RAP->offdCols  = (iint *)   calloc(RAP->offdNNZ,sizeof(iint));
    RAP->offdCoefs = (dfloat *) calloc(RAP->offdNNZ, sizeof(dfloat));
  }

  iint diagCnt =0;
  iint offdCnt =0;
  for (iint n=0;n<nnz;n++) {
    if ((newRAPEntries[n].J > globalAggStarts[rank]-1)&&
          (newRAPEntries[n].J < globalAggStarts[rank+1])) {
      diagCols[diagCnt]  = newRAPEntries[n].J - globalAggOffset;
      diagCoefs[diagCnt] = newRAPEntries[n].coef;
      diagCnt++;
    } else {
      RAP->offdCols[offdCnt]  = newRAPEntries[n].J;
      RAP->offdCoefs[offdCnt] = newRAPEntries[n].coef;
      offdCnt++;
    }
  }

  //move diagonal entries first
  for (iint i=0;i<RAP->Nrows;i++) {
    iint start = RAP->diagRowStarts[i];
    iint cnt = 1;
    for (iint j=RAP->diagRowStarts[i]; j<RAP->diagRowStarts[i+1]; j++) {
      if (diagCols[j] == i) { //move diagonal to first entry
        RAP->diagCols[start] = diagCols[j];
        RAP->diagCoefs[start] = diagCoefs[j];
      } else {
        RAP->diagCols[start+cnt] = diagCols[j];
        RAP->diagCoefs[start+cnt] = diagCoefs[j];
        cnt++;
      }
    }
  }

  //record global indexing of columns
  RAP->colMap = (iint *)   calloc(RAP->Ncols, sizeof(iint));
  for (iint i=0;i<RAP->Ncols;i++)
    RAP->colMap[i] = i + globalAggOffset;

  if (RAP->offdNNZ) {
    //we now need to reorder the x vector for the halo, and shift the column indices
    iint *col = (iint *) calloc(RAP->offdNNZ,sizeof(iint));
    for (iint n=0;n<RAP->offdNNZ;n++)
      col[n] = RAP->offdCols[n]; //copy non-local column global ids

    //sort by global index
    std::sort(col,col+RAP->offdNNZ);

    //count unique non-local column ids
    RAP->NHalo = 0;
    for (iint n=1;n<RAP->offdNNZ;n++)
      if (col[n]!=col[n-1])  col[++RAP->NHalo] = col[n];
    RAP->NHalo++; //number of unique columns

    RAP->Ncols += RAP->NHalo;

    //save global column ids in colMap
    RAP->colMap = (iint *) realloc(RAP->colMap,RAP->Ncols*sizeof(iint));
    for (iint n=0; n<RAP->NHalo; n++)
      RAP->colMap[n+RAP->NlocalCols] = col[n];

    //shift the column indices to local indexing
    for (iint n=0;n<RAP->offdNNZ;n++) {
      iint gcol = RAP->offdCols[n];
      for (iint m=RAP->NlocalCols;m<RAP->Ncols;m++) {
        if (gcol == RAP->colMap[m])
          RAP->offdCols[n] = m;
      }
    }
    free(col);
  }
  csrHaloSetup(RAP,globalAggStarts);

  //clean up
  if (M) free(PEntries);
  free(sendCounts); free(recvCounts);
  free(sendOffsets); free(recvOffsets);
  if (RAP->diagNNZ) {
    free(diagCols);
    free(diagCoefs);
  }
  free(RAPEntries);
  free(newRAPEntries);
  free(recvRAPEntries);

  return RAP;
}

