# My5 Prefetcher
ChampSim code for the my5 regular prefetcher  
Targeting publication in ISCA 2024, a tier 1 microarchitecture conference
# How it works
Trains for all possible delta prefetches, independetly of actions actually taken, whether they would be useful or not.
Based on these observations, it learns accuracies for all PC-Delta pairs, which is whenever a load is made with a given PC, the chance that for each delta d, the address of the load + d will be acessed within a set number of loads made on the cache(~1024).
On every load, all deltas corresponding to the load PC with an accuracy above a threshold will be prefetched.
This allows it to prefetch at a highly variable degree -- up to the whole page if necessary -- and look far further into the future than other prefetchers. Its accuracy threshold gauruntees that all issued prefetches will be on average useful.
# How to use
Can be used directly in the current champsim version at https://github.com/ChampSim/ChampSim#readme by selecting it as the L2 prefetcher  
```
{  
"L2C": {  
        "prefetcher": "my5"  
    }  
}
```
# Basic results
29% IPC improvement over no prefetcher.  
10% IPC improvement over previous state-of-the-art Pythia https://github.com/CMU-SAFARI/Pythia/tree/master
