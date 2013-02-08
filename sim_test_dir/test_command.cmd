#!/bin/bash
# @ class = class_a
# @ job_name = testF1-1
# @ account = par11
# @ output = /tmp/testF1-1.out
# @ error = /tmp/testF1-1.err
# @ total_tasks = 16
# @ wall_clock_limit = 00:30:00

export OUTDIR=`pwd`/OUTPUT

srun merlot

