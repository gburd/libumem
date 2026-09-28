#!/bin/bash
set +e
cd ~ && bash ~/disasm.sh
cd ~/ab-main && OUTDIR=~/r7h4/ab-main PRE=f40a2da POST=ae9af7e ./post/scripts/ec2/hotpath_ab.sh
echo ALL_DONE
