# My5 Prefetcher
ChampSim code for the my5 regular prefetcher  
# How it works
Trains for all possible prefetches, independetly of actions actually taken, whether they would be useful or not. Based on these observations, it learns accuracies for all PC-Delta pairs, and prefetches all such pairs with accuracy above a threshold.  
This allows it to prefetch at a highly variable degree -- up to the whole page if necessary -- and look far further into the future than other prefetchers. Its accuracy threshold gauruntees that all issued prefetches will be on average useful.
# How to use
Can be used directly in the current champsim version at https://github.com/ChampSim/ChampSim#readme by selecting it as the L2 prefetcher  
{  
    "L2C": {  
        "prefetcher": "my5"  
    }  
}  
# Basic results
29% IPC improvement over no prefetcher.  
10% IPC improvement over previous state-of-the-art Pythia https://github.com/CMU-SAFARI/Pythia/tree/master
