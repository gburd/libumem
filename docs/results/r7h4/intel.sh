#!/bin/bash
set +e
cd ~ && bash ~/disasm.sh
cd ~/ab-main && OUTDIR=~/r7h4/ab-main PRE=f40a2da POST=ae9af7e ./post/scripts/ec2/hotpath_ab.sh
bash ~/perfstat.sh
P="512 128 1"
cd ~/ab-lay  && POINTS="$P" OUTDIR=~/r7h4/ab-lay  PRE=f40a2da POST=r7h4-layout  ./post/scripts/ec2/hotpath_ab.sh
cd ~/ab-layb && POINTS="$P" OUTDIR=~/r7h4/ab-layb PRE=f40a2da POST=r7h4-layoutb ./post/scripts/ec2/hotpath_ab.sh
cd ~/ab-main && CFLAGS='-O2 -g -falign-functions=64 -falign-loops=32' POINTS="$P" OUTDIR=~/r7h4/ab-main-falign PRE=f40a2da POST=ae9af7e ./post/scripts/ec2/hotpath_ab.sh
echo ALL_DONE
