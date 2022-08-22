#!/usr/bin/env bash

TESTFILE1=$(mktemp -p .)

if [ -x "$(command -v python3)" ]; then
  PYTHON=$(command -v python3)
else
  PYTHON=$(command -v python)
fi

${PYTHON} << END
import random as rnd
import time as time
rnd.seed(time.time())
randnum = rnd.sample(range(2,101,1), 18)
f1 = open("${TESTFILE1}", "w+")
for m in randnum:
    for n in randnum:
        padldo = int((m + 3)/4)*4
        line = str(m) + '_' + str(n) + '_' \
             + str(m) + '_' + str(padldo) + '\n'
        f1.write(line)
f1.close()
END

for i in `cat ${TESTFILE1}`
do
  M=`echo ${i} | awk -F"_" '{print $1}'`
  N=`echo ${i} | awk -F"_" '{print $2}'`
  LDI=`echo ${i} | awk -F"_" '{print $3}'`
  LDO=`echo ${i} | awk -F"_" '{print $4}'`
  echo ${M} ${N} ${LDI} ${LDO}
  ./eltwise_unary_transform Z BF8 ${M} ${N} ${LDI} ${LDO}
  ./eltwise_unary_transform Z I8 ${M} ${N} ${LDI} ${LDO}
done

rm ${TESTFILE1}
