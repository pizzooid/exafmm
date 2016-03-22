#!/bin/bash
make
make -f Makefile.testmd

rm 0*
mpirun -np 1 ./sample.exe test.inp
diff 01_energy.log 05_mpi_ene_00.dat > log.diff1
mpirun -np 2 ./sample.exe test.inp
diff 01_energy.log 05_mpi_ene_00.dat > log.diff2

cat log.diff1 log.diff2
