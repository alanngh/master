/*

The MIT License (MIT)

Copyright (c) 2017 Tim Warburton, Noel Chalmers, Jesse Chan, Ali Karakus

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#include "gradient.h"

// interpolate data to plot nodes and save to file (one per process
void gradientPlotVTU(gradient_t *gradient, int isoNtris, dfloat *isoq, char *fileName){

  mesh_t *mesh = gradient->mesh;

  FILE *fp;
  
  fp = fopen(fileName, "w");

  fprintf(fp, "<VTKFile type=\"UnstructuredGrid\" version=\"0.1\" byte_order=\"BigEndian\">\n");
  fprintf(fp, "  <UnstructuredGrid>\n");
  fprintf(fp, "    <Piece NumberOfPoints=\"%d\" NumberOfCells=\"%d\">\n", 
          isoNtris*3,
          isoNtris);
  
  // write out nodes
  fprintf(fp, "      <Points>\n");
  fprintf(fp, "        <DataArray type=\"Float32\" NumberOfComponents=\"3\" Format=\"ascii\">\n");
  
  // compute plot node coordinates on the fly
  for(dlong n=0;n<isoNtris*3;++n){
    int id = n*(mesh->dim+gradient->Nfields);
    dfloat plotxn = isoq[id+0];
    dfloat plotyn = isoq[id+1];
    dfloat plotzn = isoq[id+2];
    
    fprintf(fp, "       ");
    fprintf(fp, "%g %g %g\n", plotxn,plotyn,plotzn);
  }
  fprintf(fp, "        </DataArray>\n");
  fprintf(fp, "      </Points>\n");
  

  fprintf(fp, "      <PointData Scalars=\"scalars\">\n");
  // write out velocity
  fprintf(fp, "        <DataArray type=\"Float32\" Name=\"Q\" NumberOfComponents=\"%d\" Format=\"ascii\">\n", gradient->Nfields);
  for(dlong n=0;n<3*isoNtris;++n){

    fprintf(fp, "       ");
    for(int fld=0;fld<gradient->Nfields;++fld){
      int id = n*(mesh->dim+gradient->Nfields) + mesh->dim + fld;
      dfloat plotqfld = isoq[id];

      fprintf(fp, "%g ", plotqfld);
    }
    fprintf(fp, "\n");
  }
  fprintf(fp, "       </DataArray>\n");

  fprintf(fp, "     </PointData>\n");
  
  fprintf(fp, "    <Cells>\n");
  fprintf(fp, "      <DataArray type=\"Int32\" Name=\"connectivity\" Format=\"ascii\">\n");
  
  for(dlong e=0;e<isoNtris;++e){
    fprintf(fp, "       ");
    for(int m=0;m<3;++m){
      fprintf(fp, "%d ", e*3+m);
    }
    fprintf(fp, "\n");
  }
  fprintf(fp, "        </DataArray>\n");
  
  fprintf(fp, "        <DataArray type=\"Int32\" Name=\"offsets\" Format=\"ascii\">\n");
  dlong cnt = 0;
  for(dlong e=0;e<isoNtris;++e){
    cnt += 3;
    fprintf(fp, "       ");
    fprintf(fp, "%d\n", cnt);
  }
  fprintf(fp, "       </DataArray>\n");
  
  fprintf(fp, "       <DataArray type=\"Int32\" Name=\"types\" Format=\"ascii\">\n");
  for(dlong e=0;e<isoNtris;++e){
    fprintf(fp, "5\n"); // tri
  }
  fprintf(fp, "        </DataArray>\n");
  fprintf(fp, "      </Cells>\n");
  fprintf(fp, "    </Piece>\n");
  fprintf(fp, "  </UnstructuredGrid>\n");
  fprintf(fp, "</VTKFile>\n");
  fclose(fp);

}
