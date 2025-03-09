
                                   ___           ___           ___                               
                                   /\__\         /\__\         /|  |                           IceKit
                      ___         /:/  /        /:/ _/_       |:|  |        ___           ___     
                     /\__\       /:/  /        /:/ /\__\      |:|  |       /\__\         /\__\    
                    /:/__/      /:/  /  ___   /:/ /:/ _/_   __|:|  |      /:/__/        /:/  /    
                   /::\  \     /:/__/  /\__\ /:/_/:/ /\__\ /\ |:|__|____ /::\  \       /:/__/     
                   \/\:\  \__  \:\  \ /:/  / \:\/:/ /:/  / \:\/:::::/__/ \/\:\  \__   /::\  \     
                      \:\/\__\  \:\  /:/  /   \::/_/:/  /   \::/~~/~        \:\/\__\ /:/\:\  \    
                       \::/  /   \:\/:/  /     \:\/:/  /     \:\~~\          \::/  / \/__\:\  \   
                       /:/  /     \::/  /       \::/  /       \:\__\         /:/  /       \:\__\  
                       \/__/       \/__/         \/__/         \/__/         \/__/         \/__/            
                                    
                                Cache-As-Ram + CAT L3 Cache Line Locking on x86_64
                                            Ported from CacheKit ARMv7

               my x86_64 port repo                                       wintermute/0xWillows Corext A8 PoC ( WIP )
        (https://github.com/3itch/icekit)                                (https://github.com/3intermute/tlbkit)  


### CACHEKIT ( scroll all the way down for ICEKIT ) ### -----------------------------------------------------==========


Introduction:
    To start this repository is not an original idea. Not at all, this idea stems from a previous whitepaper
labelled "CacheKit: Evading Memory Introspection Using Cache Incoherence" [1].
    When I stumbled across this paper in the wild, I thought it was really neat! The goal was quite simple:

    - Evade memory introspection from TrustZones Secure World
    - Securely lock down malicious data in L1/L2 cache
    - Prevent malicious data/cache lines from being evicted by 
          the cache replacement policy


How this works?:
    CacheKit evades memory introspection ( memory scanning ) from Secure World by exploiting the pre-existing
Cache incoherency problem ARM TrustZone has. You see, TrustZone does not allow bidirectional cache reads by either
Secure World or Normal World.
    To get a better picture of this refer to the below diagram:


                ┌────────────────────────┐     ┌─────────────────────────┐
                │   secure world ( SW )  │     │   normal world ( NSW )  │
                │                        │     │                         │
                │ cache:                 ┼──X──► cache:                  │
                │                        │     │            [ LOCKED ]   │
                │   addr X: D ( NS = 0 ) │     │   addr X: K ( NS = 1  ) │
                │    ( original data )   │     │    ( cachekits code )   │
                └───────────▲───────┬────┘     └────┬───────▲────────────┘
                            │       │               │       │             
                            │       │               │       │             
                            │       │               │       │             
                            │       │               │       │             
                            │       │               │       │             
                            └────┬──▼───────────────▼──┬────┘             
                                 │    physical mem     │                  
                                 │                     │                  
                                 │ addr X: D           │                  
                                 │   ( original data ) │                  
                                 └─────────────────────┘                  
                    
                       [Figure 1]: TrustZone cache incoherence model 


    Here we have our cache incoherence model. CacheKit's code resides in NSW and the original data resides in SW.
    The SW has to read from the physical memory to perform introspection which SHOULD reflect its own data right?
... Well no! CacheKit exploits benign hardware-supported functionality in the ARMv7 architecture.


How:
    CacheKit knows that if they get data into cache, it'll likely be backed by RAM. But the thing is, RAM is vulnerable
to introspection, so CacheKit decides to take another approach. Instead they focus on I/O ( Input/Output ) memory.
    CacheKit instead carves out a slice of UNRESERVED/UNBACKED physical I/O memory. They do this knowing that little to
no memory introspection tools will scan this physical I/O memory in fear of any potential system crashes.
    This is an amazing idea, but it comes with some pros and cons.


pros:
    - Not commonly scanned
    - Scanning could crash system
    - Allows for full erasure of data
cons:
    - Suspicious
    - ^ the memory is mapped out but unbacked which is weird
    - the only way we evade memory introspection, but risking full erasure of data
            ( which can be a problem for persistent malware )
    - Very easy to prevent 
            ( CacheLight [2] | more on this later ) 

    
Cache-As-Ram:
    So we've *mapped out* ( get it XD..HELP ME ) why this will work and why it might not work.
    So let's now talk about how Cache-As-Ram actually works on ARMv7:

    1. Map out Physical I/O space 
        ( not set as WriteBack by default, unlike RAM )
    2. Allocate a page of memory
    3. Set page attribute as Writeback
        ( WriteBack indicates that this pages contents are cachable )
    4. Write data to page
    5. Cache fill is performed and data gets put in cache instead.


    In the next part we'll talk about how CacheKit indirectly locks cache lines into cache through locking
the cache ways / isolating them.

    I just want to mention that this doesn't protect against manual cache eviction operations, only automatic
cache replacement policy. But if the cache lines get flushed manually, then it means that the malicious data WILL NOT
be flushed to main memory. However the cache lines will get flushed back to the unbacked physical I/O memory.
    This is bad right? well no. This is because this unbacked I/O memory is well...unbacked. So our malicious data just gets
deleted, sent to the ether, voided, yeeted, gone forever! Which means even if post-introspection was a thought, it would be impossible.
    Unless the WriteBack flush operation was to be intercepted somehow (????).


Split TLB locking & Cache line locking:
    CacheKit has to keep their virtual-to-physical translations fixed somehow right?
    Because when we do world switching NSW->SW or SW->NSW then the TLB entries get flushed,
which isn't very ideal.
    So CacheKit locks the split icache and dcache TLBs ( ITLB & DTLB ), which prevents the virtual-to-physical address mappings
for their malicious data from being flushed by the automatic cache replacement policy. This doesn't inherently prevent cache line
eviction on its own, just keeps the translation mappings from disappearing which causes CacheKit to lose access to its data
                                                                                    ( which will cause future cache misses ).

    ```
        MRC p15, #0, r0, c10, c0, #0        /*  read c15's c10 ( tlb lockdown reg ) into r0  */
        ORR r0, r0, #1                     /*  inclusive OR to enable locking next entry    */
        MCR p15, #0, r0, c10, c0, #0      /*  write->tlb lockdown register                 */
    ```

    To actually avoid CacheKit's data/cache lines from being evicted they employ cache line locking.
    This is through a method called cache lockdown, which is a hardware-supported lock allowing
for data to reside in cache.
    They do this by setting the L bit as 1 for a specified cache way they want locked.
    
    ```
        MRC p15, #0, r0, c9, c0, #0         /*  read c15's c9 ( cache lockdown reg ) into r0  */
        ORR r0, r0, #1                     /*  inclusive OR to flip L1 bit                   */
        MCR p15, #0, r0, c9, c0, #0       /*  write->cache lockdown reg                     */
    ```

    This isolates the data in the specified cache way 
        ( cache way 0 in this case ) and therefore *locks* it from
being tampered with. Which is ideal for this technique because world switches
like NSW->SW will flush the TLB translation entries.
    This prevents that by locking the data into the isolates cache way.

    The only reason this is called cache way locking instead of cache line locking
is because it is cache way locking. CacheKit confuses a lot of readers that it's actually cache
line locking when it's really not in a technical and not theoretical sense.

    However, despite this small confusion, I'm still going to refer to this as cache line locking and
not cache way locking, to keep consistency of course ( as the CacheKit paper also does this ).


The good and the bad:
    If we go back and look at *Figure 1* we can see that SW can't directly access and cause introspection
for the NSW. It has to read the physical memory to see it. SW can however, read the NSW memory and registers, 
but it cannot read the cache as previously stated.
    So SW has no access to the cache, meaning that CacheKit hides their rootkit in the NSW/Normal World cache.
    This might be pretty obvious by now, but this is exactly why they leverage this benign and supported functionality.
    It's because 1. it works and 2. it's efficient in this context.

    Since our cache lines aren't backed by RAM but instead the physical I/O space, our data doesn't reflect main memory.
    Meaning that introspection will fail, but this itself is suspicious. *Unbacked* cache lines aren't really a normal thing...

    We could just read into L2 cache from SW and snuff out CacheKits malicious cache lines, since ARMv7 marks L2 cache as shared
while obviously L1 cache is private and each world has it's own L1 cache.
    LOL so this isn't good is it? so what does Cachekit do you may ask...? well nothing.
    CacheKit doesn't protect against this at all ( maybe they do int their PoC, but that's not in the paper anywhere ).
    It is not possible to protect against these reads, however maybe we could adapt to intercept these shared L2 read requests?
    To do this we would probably need our own secure application/firmware in SW to actually have the power to do such.
    Take that "statement" with a pinch of salt as I haven't looked too deep into it.


CacheLight:
    What is CacheLight? What does it do? Why are we talking about it?
    Well in laymans terms CacheLight is the polar opposite of CacheKit.
    In common parlance it is basically a middle finger to CacheKit.

    Anyways, CacheLights locks down which world can lock cache lines.
    This means that the NSW will no longer be able to directly lock its own cache.
    Meaning that if the NSW requires a cache lock, then it must requiest this through
an SMC call ( Secure Monitor ).
    The SW can then evaluate and approve this request thoroughly, ensuring malicious code is
not locked inside of the cache 
            ( however, this itself does not mean malicious code still cannot get into cache ).

    CacheLight also implements cache timing measurements which sus out suspicious behaviour.
    If CacheKit is attempting to lock cache lines before restrictions prevent it, this alters
access times or hit rates in very detectable ways.
    The SW will probe memory regions and measure access times. if access times are suspiciously fast then
this indicates presense of malicious data.


    The below custom smc handler would be required to process any cache locking requests from NSW.
    In ARM Trustzone, smcs will trap to SW, and handler will check the call number ( CALL_NUMBER ) and
parameters ( f.eg r1 ( cache_type ) , r2 (way_num) ).

    ```
        void smc_handler(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
            if (r0 == CALL_NUMBER) {          /*  f.eg unique 0x100 cache locking call number  */
                uint32_t cache_type = r1;
                uint32_t way_num = r2;

                if (check_request(cache_type, way_num)) {
                    lock_cache_way(cache_type, way_num);
                    record_locks(cache_type, way_num);
                }
            }
            ... etc smc call handlers
        }
    ```


check locking requests:

    ```
        bool check_request(uint32_t cache_type, uint32_t way_num) {
            uint32_t pid = get_current_pid();
            if (pid == TRUSTED_PROCESS_ID) {
                return true;
            }
            return false;
        }
    ```

    this will just simply checks the process ID and if the cache lock request comes
from a legit process, then we can accept it through other means.


tracking locks:

    ```
        bool locked_ways_d[MAX_WAYS];
        bool locked_ways_i[MAX_WAYS];

        void record_locks(uint32_t cache_type, uint32_t way_num) {
            if (cache_type == DATA_CACHE) {
                locked_ways_d[way_num] = true;
            } else if (cache_type == INSTRUCTION_CACHE) {
                locked_ways_i[way_num] = true;
            }
        }
    ```

    this creates arrays which hold the dataa and instruction cache ways.
    They can record which ways are locked, which can aid with future comparisons.


checking and resetting bad locks:

    ```
        void check_n_reset() {
            int i;
            uint32_t regd_val, regi_val;
            
            asm volatile("mrc p15, 0, %0, c9, c0, 0" : "=r"(regd_val));
            asm volatile("mrc p15, 0, %0, c9, c0, 1" : "=r"(regi_val));

            for (i = 0; i < MAX_WAYS; i++) {
                if ((regd_val & (1 << i)) && !locked_ways_d[i]) {
                    regd_val &= ~(1 << i);
                    asm volatile("mcr p15, 0, %0, c9, c0, 0" : : "r"(regd_val));
                }
                if ((regd_val & (1 << i)) && !locked_ways_i[i]) {
                    regi_val &= ~(1 << i);
                    asm volatile("mcr p15, 0, %0, c9, c0, 1" : : "r"(regi_val));
                }
            }  
        }
    ```

    here we implement a possible read of the current lockdown registers ( c15's c9 )
to check and compare with the recorded locks. We can then unlock any unauthorised ways.

    the bad thing about this though is that CacheKit can still write to the cp15's c9
register, because NSACR ( Non-secure access control register ) does not prevent such...
    So the above function could possibly fill in for that lack of prevention in the
secure world.

    The lack of the cache way locking function here is because it would literally just mimic the existing
cache locking function we kind of defined earlier with the move from coprocessor to register 
                                                                    ( MCR ) and vice versa.


That's all for this section for now ...

References:
    [1] https://csis.gmu.edu/ksun/publications/CacheKit-eurosp2016.pdf
    [2] https://zzm7000.github.io/publication/Cachelight.pdf



### ICEKIT ( scroll all the way up for CACHEKIT ) ### ---------------------------------------------------==========

I'll come back and do this later ... ZzZzZZzz
