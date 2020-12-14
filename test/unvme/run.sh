for batchsize in 1 2 4 8 16 32 64
do
    ./unvme_embed_test -s 1 -q 1 -r 64 -b $batchsize -e 100 01:00.0
done
