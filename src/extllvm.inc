extern "C" const char* llvm_disassemble(const unsigned char* Code, int syntax)
{
    int x64 = 1;
    size_t code_size = 1024 * 100;
    std::string Error;
    llvm::TargetMachine *TM = extemp::EXTLLVM::EE->getTargetMachine();
    llvm::Triple Triple = TM->getTargetTriple();
    const llvm::Target TheTarget = TM->getTarget();
    std::string TripleName = Triple.getTriple();
    //const llvm::Target* TheTarget = llvm::TargetRegistry::lookupTarget(ArchName,Triple,Error);
    const llvm::MCRegisterInfo* MRI(TheTarget.createMCRegInfo(TripleName));
    const llvm::MCAsmInfo* AsmInfo(TheTarget.createMCAsmInfo(*MRI,TripleName));
    const llvm::MCSubtargetInfo* STI(TheTarget.createMCSubtargetInfo(TripleName,"",""));
    const llvm::MCInstrInfo* MII(TheTarget.createMCInstrInfo());
    //const llvm::MCInstrAnalysis* MIA(TheTarget->createMCInstrAnalysis(MII->get()));
    llvm::MCContext Ctx(AsmInfo, MRI, nullptr);
    llvm::MCDisassembler* DisAsm(TheTarget.createMCDisassembler(*STI, Ctx));
    llvm::MCInstPrinter* IP(TheTarget.createMCInstPrinter(Triple,syntax,*AsmInfo,*MII,*MRI)); //,*STI));
    IP->setPrintImmHex(true);
    IP->setUseMarkup(true);
    uint64_t MemoryAddr = 0;
    uint64_t Size = code_size;
    uint64_t Start = 0;
    uint64_t End = code_size;
    std::string out_str;
    llvm::raw_string_ostream OS(out_str);
    llvm::ArrayRef<uint8_t> mem(Code, code_size);
    uint64_t size;
    uint64_t index;
    OS << "\n";
    for (index = 0; index < code_size; index += size) {
        llvm::MCInst Inst;
        if (DisAsm->getInstruction(Inst, size, mem.slice(index), index, llvm::nulls(), llvm::nulls())) {
            auto instSize(*reinterpret_cast<const size_t*>(Code + index));
            if (instSize <= 0) {
                break;
            }
            OS.indent(4);
            OS.write("0x", 2);
            OS.write_hex(size_t(Code) + index);
            OS.write(": ", 2);
            OS.write_hex(instSize);
            IP->printInst(&Inst, OS, "", *STI);
            OS << "\n";
        } else if (!size) {
            size = 1;
        }
    }
    return strdup(OS.str().c_str());
}

static extemp::CMG DestroyMallocZoneWithDelayCM(
        [](extemp::TaskI* Task)->void {
            llvm_zone_destroy(static_cast<extemp::Task<llvm_zone_t*>*>(Task)->getArg());
        });

extern "C" void llvm_destroy_zone_after_delay(llvm_zone_t* Zone, uint64_t Delay)
{
    extemp::TaskScheduler::I()->add(new extemp::Task<llvm_zone_t*>(extemp::UNIV::TIME + Delay, extemp::UNIV::SECOND(),
            &DestroyMallocZoneWithDelayCM, Zone));
}

static extemp::CMG FreeWithDelayCM(
        [](extemp::TaskI* Task)->void {
            free(static_cast<extemp::Task<char*>*>(Task)->getArg());
        });

static void free_after_delay(char* Data, double Delay)
{
    extemp::TaskScheduler::I()->add(new extemp::Task<char*>(extemp::UNIV::TIME + Delay, extemp::UNIV::SECOND(),
            &FreeWithDelayCM, Data));
}

#if 0 // TODO: What is this needed for???
static long long llvm_get_next_prime(long long start)
{
    long long  how_many = start+100000;
    long long  *array = (long long*) calloc(how_many, sizeof(long long));
    long long  i, prime, multiple;
    /*  mark each int as potentially prime */
    for (i=0; i<how_many; i++)
        array[i] = 1;
    /* special cases: 0, 1 not considered prime */
    array[0] = array[1] = 0;
    /* foreach starting prime, mark every multiple as non-prime */
    prime = 0;
    while (1) {
        /* skip non-primes to find first prime */
        for (; (prime < how_many) && (!array[prime]); ++prime)
            continue;
        if (prime >= how_many)
            break;
        for (multiple=2*prime; multiple<how_many; multiple+=prime) {
            array[multiple] = 0;
        }
        ++prime;
    }
    /* Now that we have marked all multiple of primes as non-prime, */
    /* print the remaining numbers that fell through the sieve, and */
    /* are thus prime */
    for (i=start+1; i<how_many; i++) {
        if(array[i]) return i;
    }
    return -1;
}
#endif

extern "C" void llvm_zone_destroy(llvm_zone_t* Zone)
{
#if DEBUG_ZONE_ALLOC
    printf("DestroyZone: %p:%p:%lld:%lld\n", Zone, Zone->memory, Zone->offset, Zone->size);
#endif
    if (Zone->memories) {
        llvm_zone_destroy(Zone->memories);
    }
    free(Zone->memory);
    free(Zone);
}

extern "C" llvm_zone_t* llvm_pop_zone_stack()
{
    auto stack(llvm_threads_get_zone_stack());
    if (unlikely(!stack)) {
#if DEBUG_ZONE_STACK
        printf("TRYING TO POP A ZONE FROM AN EMPTY ZONE STACK\n");
#endif
        return nullptr;
    }
    llvm_zone_t* head = stack->head;
    llvm_zone_stack* tail = stack->tail;
#if DEBUG_ZONE_STACK
    llvm_threads_dec_zone_stacksize();
    if (!tail) {
        printf("%p: popping zone %p:%lld from stack with no tail\n",stack,head,head->size);
    } else {
        printf("%p: popping new zone %p:%lld back to old zone %p:%lld\n",stack,head,head->size,tail->head,tail->head->size);
    }
#endif
    free(stack);
    llvm_threads_set_zone_stack(tail);
    return head;
}

extern "C" void* llvm_zone_malloc(llvm_zone_t* zone, uint64_t size)
{
    extemp::EXTMutex::ScopedLock lock(alloc_mutex);
#if DEBUG_ZONE_ALLOC
    printf("MallocZone: %p:%p:%lld:%lld:%lld\n",zone,zone->memory,zone->offset,zone->size,size);
#endif
    size += LLVM_ZONE_ALIGN; // for storing size information
    if (unlikely(zone->offset + size >= zone->size)) {
#if EXTENSIBLE_ZONES // if extensible_zones is true then extend zone size by zone->size
        int old_zone_size = zone->size;
        bool iszero(!zone->size);
        if (size > zone->size) {
            zone->size = size;
        }
        zone->size *= 2; // keep doubling zone size for each new allocation // TODO: 1.5???
        if (zone->size < 1024) {
            zone->size = 1024; // allocate a min size of 1024 bytes
        }
        llvm_zone_t* newzone = llvm_zone_create(zone->size);
        void* tmp = newzone->memory;
        if (iszero) { // if initial zone is 0 - then replace don't extend
          zone->memory = tmp;
          free(newzone);
        } else {
            // printf("adding new memory %p:%lld to existing %p:%lld\n",newzone,newzone->size,zone,zone->size);
            newzone->memories = zone->memories;
            newzone->memory = zone->memory;
            newzone->size = old_zone_size;
            zone->memory = tmp;
            zone->memories = newzone;
        }
        llvm_zone_reset(zone);
#elif LEAKY_ZONES       // if LEAKY ZONE is TRUE then just print a warning and just leak the memory
        printf("\nZone:%p size:%lld is full ... leaking %lld bytes\n",zone,zone->size,size);
        printf("Leaving a leaky zone can be dangerous ... particularly for concurrency\n");
        fflush(NULL);
        return malloc((size_t)size);  // TODO: what about the stored size????
#else
        printf("\nZone:%p size:%lld is full ... exiting!\n",zone,zone->size,size);
        fflush(NULL);
        exit(1);
#endif
    }
    size = (size + LLVM_ZONE_ALIGNPAD) & ~LLVM_ZONE_ALIGNPAD;
    auto newptr = reinterpret_cast<void*>(reinterpret_cast<char*>(zone->memory) + zone->offset);
    memset(newptr, 0, size); // clear memory
    newptr = reinterpret_cast<char*>(newptr) + LLVM_ZONE_ALIGN; // skip past size
    *(reinterpret_cast<uint64_t*>(newptr) - 1) = size;
    zone->offset += size;
    return newptr;
}

static bool llvm_ptr_in_current_zone(void* ptr)
{
    return llvm_ptr_in_zone(llvm_peek_zone_stack(), ptr);
}

static __thread llvm_zone_t* tls_llvm_callback_zone = 0;

static inline llvm_zone_t* llvm_threads_get_callback_zone()
{
    if (unlikely(!tls_llvm_callback_zone)) {
        tls_llvm_callback_zone = llvm_zone_create(1024 * 1024); // default callback zone 1M
    }
    return tls_llvm_callback_zone;
}

extern "C" llvm_zone_t* llvm_zone_callback_setup()
{
    auto zone(llvm_threads_get_callback_zone());
    llvm_push_zone_stack(zone);
    return llvm_zone_reset(zone);
}
