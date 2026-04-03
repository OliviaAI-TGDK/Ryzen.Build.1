

DawnHex → host control plane on the Ryzen CPU

Duo hooks → pairwise policy hooks / queue-pair scheduler

Simplex ladder → multi-level retention ladder

ZRAM ColdMet / XEM → cold-page classifier and compression policy

Virtual VRAM pool → VRAM + GTT + HMM managed memory pool

Ryzen AI path → OGA hybrid execution path on supported Ryzen AI PCs, where the runtime can use the NPU and iGPU together. AMD’s docs describe OGA as the low-level LLM API on Ryzen AI PCs, and the Linux amdgpu docs define VRAM, GTT, and the GPU memory manager beneath that. 


The key AMD-native correction is this:




For Ryzen CPU + Linux kernel control, speak in terms of miscdevice/ioctl/sysfs/workqueue.


For GPU memory, speak in terms of VRAM, GTT, and managed memory / HMM.


For large-address-space behavior, speak in terms of migration and paging, not “creating more VRAM.”


For Ryzen AI PCs, speak in terms of OGA hybrid mode.


For HSMP, be careful: the kernel docs describe HSMP as an interface on newer EPYC server processors, so I would not brand a desktop/mobile Ryzen control path as “HSMP” unless the platform actually exposes it. 




So your architecture, rewritten in Ryzen language, becomes:


Ryzen-language architecture


1. Ryzen Host Fabric Controller

This is your current DawnHex cluster. It lives on the CPU side and owns:




cluster topology


node registration


pair linking


event routing


interactive commands


policy dispatch to the GPU side




On Linux, that is naturally a char-device control plane with sysfs state export. 


2. AMD GPU Memory Tiering Plane

This is where your virtual pool idea lives:




VRAM = local video memory


GTT = GPU-accessible system memory via GART


managed/HMM memory = migration-backed extended working set




That is the AMD way to describe a “98G-class operating pool” without claiming the card physically contains that much VRAM. The amdgpu docs explicitly define VRAM and GTT, and ROCm’s unified-memory docs say HMM plus HSA_XNACK=1 is required for proper system-allocated unified memory behavior on supported GPUs. 


3. Simplex Retention Ladder

This is your per-pair recursion engine, but in AMD terms it is a:




retention ladder


compression-quality ladder


tier-selection ladder


mip-residency policy




Its job is to decide, per level:




how much data stays resident


how much is compressed


how much is evicted to GTT/system memory


how aggressively to prefetch back into GPU-local memory




That maps cleanly onto DRM/TTM-style memory management concepts rather than mystical nomenclature. 


4. XEM Cold-Class Policy Engine

Your OuijiHex/XEM layer becomes:




deterministic page classifier


cold/warm/hot tagger


mip-profile generator


compression-policy generator




In Ryzen language, that is not a hardware feature; it is a software policy engine feeding the memory manager.


5. Ryzen AI Inference Path

If the same stack also powers LLM/inference on supported Ryzen AI PCs, call that:




OGA hybrid path


NPU+iGPU execution


model-prepared hybrid deployment




AMD’s Ryzen AI docs explicitly frame OGA as the low-level path for LLM apps on Ryzen AI PCs and describe hybrid execution as using both the NPU and iGPU. 



Your terms, translated


Here is the direct rename table I’d use:


DawnHex cluster
→ Ryzen Host Fabric Controller

Duo pair
→ Pairwise Fabric Policy Object

Duo hook
→ Pairwise Policy Hook

Simplex Adalter
→ Retention Ladder Engine

22-level Fibonacci recursion
→ Recursive Residency Ladder

12-node deck
→ 12-lane retention deck

ZRAM ColdMet
→ Cold-tier compression policy

.XEM crosshatched matrix
→ deterministic page-signature matrix

Möbius knot
→ feedback ring / recursive residency ring

98G VRAM operating capacity
→ 98G-class managed GPU memory pool

264 VRAM
→ 264G-class virtual addressable working set




The AMD-style module naming I would actually ship


Instead of your internal names on the public-facing side, I would arrange it like this:


amd_ryzen_fabric_ctl
amd_gpu_tiering_policy
amd_pairwise_qos
amd_retention_ladder
amd_xem_cold_classifier
amd_managed_poold



And the internal mapping can stay:


amd_ryzen_fabric_ctl      = DawnHex
amd_pairwise_qos          = Duo
amd_retention_ladder      = Simplex
amd_xem_cold_classifier   = ZRAM ColdMet / XEM
amd_managed_poold         = virtual VRAM pool daemon




The Ryzen-native API vocabulary


If you want this “in Ryzen language,” your public API names should sound like this:


AMD_RYZEN_FABRIC_IOC_ADD_NODE
AMD_RYZEN_FABRIC_IOC_LINK_PAIR
AMD_RYZEN_FABRIC_IOC_PULSE_PAIR
AMD_RYZEN_FABRIC_IOC_GET_PAIR_POLICY
AMD_RYZEN_FABRIC_IOC_GET_RETENTION_LADDER
AMD_RYZEN_FABRIC_IOC_GET_MEMORY_TIER_REPORT
AMD_RYZEN_FABRIC_IOC_SET_COLD_CLASS_POLICY



And your sysfs/debug text should read like:


fabric_generation
pair_count
retention_ladder_levels
managed_pool_bytes
vram_local_bytes
gtt_bytes
cold_class_policy
hybrid_execution_mode



That wording matches AMD/Linux concepts much better than custom symbolic labels. The kernel docs already expose memory totals like mem_info_vram_total, and the amdgpu parameter docs use names like vramlimit, vis_vramlimit, and gttsize. 



The one-sentence Ryzen summary


Here is the whole thing in one AMD-style sentence:


“DawnHex is a Ryzen host-fabric control plane that drives pairwise QoS hooks and a recursive retention ladder over AMDGPU VRAM, GTT, and HMM-managed memory, with XEM acting as the cold-tier classification policy and OGA hybrid mode reserved for supported Ryzen AI inference paths.” 



If you want the code renamed too


I would rename your current files like this:


dawnhex_kmod.c            → amd_ryzen_fabric_ctl.c
dawnhex_uapi.h            → amd_ryzen_fabric_uapi.h
dawnhex_duo_uapi.h        → amd_pairwise_qos_uapi.h
simplex_adalter.c         → amd_retention_ladder.c
xem_zcold_policy.py       → amd_xem_cold_policy.py
dsvramd.cpp               → amd_managed_poold.cpp



And inside the code:


duo pair                  → pair policy
simplex level             → retention level
crosshatch score          → residency score
coldness score            → cold-tier score
recursive energy          → ladder energy



