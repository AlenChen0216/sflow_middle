#! /bin/bash

sudo nfp-rtsym -l 80000 -v _global_semaphores_dup:0 > dg.txt
sleep 0.1
sudo nfp-rtsym -l 640000 -v __flow_key_dup:0 > dk.txt
sleep 0.1
sudo nfp-rtsym -l 640000 -v __flow_data_dup:0 > dd.txt

sleep 1

sudo nfp-rtsym -l 80000 -v _global_semaphores:0 > g.txt
sleep 0.1
sudo nfp-rtsym -l 640000 -v __flow_key:0 > k.txt
sleep 0.1
sudo nfp-rtsym -l 640000 -v __flow_data:0 > d.txt