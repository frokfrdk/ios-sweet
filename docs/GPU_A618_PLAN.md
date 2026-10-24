# Adreno 618 GPU acceleration plan (sunfish / Pixel 4a)

Status, 2026-10-25: **Stages 0 and 1 passed on the phone (kernel 0096, image G1b). Stage 2A is built (patch
0097, `sweetgpu=3`) and not yet booted.** Nothing is
flashed: every test is a `fastboot boot` image. The firmware is extracted. The hardware facts
come from the stock DTB and the downstream sunfish kernel. ios-sailfish's GPU work has been read. Every
stage below ends in a test you can see on the phone, and nothing advances until that test passes.

## 1. Goal and approach

Today CoreAnimation renders on the CPU (`CA::OGL::SWContext`). The H4 boot copies each 828x1792 frame
(5.9 MB) into a canvas buffer, and the MDP's QSEED3-Lite scaler upscales it to the panel. The goal is
the approach [ios-sailfish](https://github.com/joranv1/ios-sailfish) proved on the Pixel 1 (Adreno 530):

- **No Metal.** CoreAnimation's GLES renderer (`CA::OGL::GLESContext`) runs on **Mesa freedreno**.
- Mesa is built for iOS as `sweetgl.dylib` and injected into `backboardd` with `DYLD_INSERT_LIBRARIES`.
- An **EAGL shim** replaces the few `EAGLContext` methods CA uses, so its GL calls reach Mesa.
- A small **libdrm/msm emulation** turns Mesa's DRM ioctls into calls to an XNU user client
  (`SweetGPU`). That client offers buffer objects, IOSurface import, command submission and fences.
- The **kernel** powers the GPU, programs its SMMU, loads the CP microcode, has TrustZone load the zap
  shader, and feeds Mesa's command streams to the ring.

Mesa's freedreno already supports a618 (SC7180 Chromebooks, mainline Linux). The new work is therefore
almost all **kernel bring-up**. The a6xx generation is harder than ios-sailfish's a5xx for one reason:
the **GMU** (a Cortex-M3 power controller) and RPMh now sit between the CPU and the GPU's power rail.

## 2. What ios-sailfish did (studied 2026-10-25)

Kernel (`bsd/dev/sweet_gpu.c`, `iokit/Kernel/arm/SweetGPU.cpp`, `SweetSurface.cpp`). Patch numbers are theirs.

| Step | Their patches | What it does |
|---|---|---|
| Staged probe | 0119 | Boot-arg `sweetgpu=N`. Runs 40 s after boot, pausing 11 s between steps so the persistent log keeps the last good step. Order: GFX rail, CX GDSC + AHB, GX GDSC (IO clamp), clocks, then reads of VBIF_VERSION, RBBM_STATUS and ALWAYSON. |
| CP test | 0120 | CP microcode comes in as boot-payload blobs (`/chosen/memory-map` `GPU-PM4`/`GPU-PFP`). Then a 16 KB ring, `CP_ME_INIT`, and `CP_MEM_WRITE 0xc0ffee01` to a scratch page. |
| SMMU | 0121, 0123, 0155, 0156, 0161 | The firmware ignores sCR0.CLIENTPD, so the first CP fetch reset the phone. Fix: an identity map in CB0 (stage 1, 4K granule, 1 GB blocks over DRAM), SMR0 matching every stream, S2CR0 → CB0, then a readback check. Later patches make faults non-fatal, pad VMs and retry stage-2 reads. |
| Zap shader | 0130 | The GPU starts in secure mode: the ring writes nothing to normal memory, and `CP_SET_SECURE_MODE 0` hangs without a zap. The kernel loads the zap through SCM/SMCCC PAS (`INIT_IMAGE`, `MEM_SETUP`, segment copy, `AUTH_AND_RESET`) with PAS id 13. |
| Bring-up tests | 0126–0136, 0141–0149 | 2D blit, compute, blend, textured quad, benchmarks, clock and DDR votes. |
| GPU VA | 0137 | GPU VA 8–16 GB is mapped with 4 KB pages (L1[8..15] → L2 → L3) for scattered memory. The identity map below 8 GB stays for kernel buffers. |
| User client | 0138, 0150, 0151 | Selectors 0–3: map user memory, map IOSurface, unmap, quad. **10 bo new, 11 bo free, 12 bo import (IOSurface), 13 submit (IBs + bo list → seqno), 14 wait, 15 info.** A read-only fence page, hang detection (fence stuck 2 s → service off) and guard pages between mappings. One shared address space; their own notes call that acceptable only on a single-user bring-up device. |
| IOSurface | 0152–0162 | Row alignment and client-mapping fixes so Mesa can import CA's surfaces. |
| Present | 0164 | The GPU draws the swap surface into one of two 32 bpp panel buffers, and the MDP flips between them (`sweet_fbcon_flip`). Also 600 MHz. |
| Plumbing | 0104, 0163, 0139, 0166 | `sweetenv=proc:VAR=VAL` boot-arg (adds env vars at exec). An AMFI policy-syscall answer to dyld's op 0x5a (otherwise dyld ignores `DYLD_INSERT_LIBRARIES`). An `IOAcceleratorES` stub without `MetalPluginName`, which stops CA polling for it for 30 s while still choosing GLES. |

User space (`tools/sweetgl/`):

- **`mesa-setup.sh`**: meson for iOS arm64 (`ios-cross.txt`), gallium freedreno, GLES2/3, `-Dfreedreno-kmds=msm`,
  no LLVM, no window system, static libraries.
- **`mesa.patch`** (3 hunks):
  - `os_mmap` → `sweet_drm_bo_map`.
  - Build `src/freedreno/drm` on darwin.
  - An **a5xx-only** pitch-alignment fix for imported linear surfaces.
- **`sweet_drm.c`**: libdrm plus the msm ioctls (GET_PARAM, GEM_NEW/INFO/CPU_PREP/SUBMIT, WAIT_FENCE,
  SUBMITQUEUE_*) over the SweetGPU selectors. The fd is `/dev/null`.
- **`sweetgl.c`**: a minimal gallium frontend (modelled on Haiku's hgl), surfaceless, CA draws only into FBOs.
  - It retries for 2 minutes while the GPU service comes up.
  - It forces `nogmem`: rendering goes straight to memory, because GMEM tile resolves overran unpadded
    IOSurfaces.
  - It forces `nofp16`.
- **`eagl.c`** (77 KB), the EAGL shim:
  - It swizzles `initWithAPI:properties:`, `API`, `setParameter:to:`, `getMacroContextPrivate` (GLI dispatch
    `{rctx, disp[1036]}`, generated by `gen-dispatch.py`), `texImageIOSurface:…`,
    `attachImage:toCoreSurface:invertedRender:`, `+setCurrentContext:` and `+currentContext`.
  - IOSurfaces become textures through `glEGLImageTargetTexture2DOES`.
  - It emulates `GL_EXT_shader_framebuffer_fetch`. Without it, CA drops every blend-filter layer: CC toggles,
    App Library plates, Reduce Transparency backgrounds.
  - It also emulates `GL_TEXTURE_RECTANGLE` and copies surfaces whose rows the GPU cannot sample.
- **`build.sh`**: links everything into one ad-hoc-signed dylib, zero-padded to 64 MiB so the file on the phone
  keeps its size and can be rewritten in place.
- It is built with Xcode 15 on macOS (`xcrun`). Boot-args: `sweetgpu=8 sweetgpuclk=600 sweetddr=1804
  sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgl.dylib`.

## 3. What is different on the Pixel 4a

| | Pixel 1 (sailfish) | Pixel 4a (sunfish) |
|---|---|---|
| GPU | Adreno 530, chip 0x05030002 | **Adreno 618, chip 0x06010800**, GMEM 512 KiB |
| CP microcode | PM4 + PFP | **SQE** `a630_sqe.fw` (32136 B) |
| Power | Host votes GFX rail (PMI8994 SPMI), GDSCs, clocks | **GMU** (Cortex-M3, `a618_gmu.bin` 32 KiB legacy raw image) votes `gfx.lvl` through its own RSC/PDC to **RPMh**; the GX GDSC and GX clock are normally the GMU's job |
| Rails | SPMI regulator writes | RPMh ARC `gfx.lvl` (pm6150 s2), `cx.lvl` (pm6150l s2); apps RSC 0x18220000 (drv 2); cmd-db at 0xc3f000c |
| Zap | PAS 13 | PAS 13, `a615_zap.mdt/.b00-.b02`, carve-out **0x97715000** (0x2000) |
| SMMU | smmu-v2, no hypervisor | smmu-v2 at **0x5040000** behind Qualcomm's **hypervisor** (`qcom,hyp_secure_alloc`); SIDs: gfx3d_user 0, gfx3d_secure 2, GMU 4/5 |
| Quirks (DT) | – | `hfi-use-reg`, `secvid-set-once`, `no-nap`, highest-bank-bit 14, UBWC mode 2 |
| Present | CPU copy then GPU quad + MDP flip | H4 already scales on the MDP (VIG0 QSEED3-Lite); only the 5.9 MB copy per frame is left |
| Kernel already has | – | RPMh client (`sweet_rpmh.c`, cmd-db parser, VRM-only), persistent console log (0047), SweetSurface/IOMFB (0046), AMFI policy syscall (0092, answers op 0x60 only) |

## 4. Facts gathered

**Firmware** (from `vendor.img`, in `~/ios-pixel/gpu-fw/fw/`, **never committed**):

| File | Size / layout |
|---|---|
| `a630_sqe.fw` | 32136 B; dword 0 = 0, then the version |
| `a618_gmu.bin` | 32768 B, raw Cortex-M3 image with a vector table; fits the 32 KiB ITCM |
| `a615_zap.mdt` (+ `.b00/.b01/.b02`) | 3 program headers: NULL, NULL (hash, vaddr 0x6000), LOAD (paddr 0x5000, filesz 0x7c0) |

**DTB** (`~/ios-pixel/factory/stock/sunfish-base.dts`):

- **kgsl-3d0** at 0x5000000 (0x40000):
  - Other regions: cx_dbgc 0x5061000, cx_misc 0x509e000, qfprom 0x780000. IRQ SPI 300.
  - Speed bin: qfprom +0x41a0, mask 0x1fe00000, shift 21.
  - Bin-0 frequencies: 825/800/650/565/430/355/267/180 MHz. Initial level 7 = 180 MHz.
- **GMU** at 0x506a000 (0x31000):
  - PDC cfg 0xb290000, PDC seq 0xb490000. IRQs: HFI SPI 0x130, GMU SPI 0x131.
  - GMU clock 200 MHz.
  - Register offsets in the Linux XML (and `xnu/gpu/regs/`) count in dwords from the GPU base 0x5000000,
    as kgsl does: GMU_CM3_ITCM_START 0x1b400 = 0x506d000. `RSCC_*` count from the RSCC block
    (0x506a000 + 0x23000), the `A6XX_PDC*` domains from their PDC blocks.
- **gpucc** at 0x5090000 (0x9000):
  - GDSCs: CX 0x509106c, GX 0x509100c.
  - From downstream `gpucc-sdmmagpie.c`:
    - PLL0 (Fabia) at +0x0: L 0x12, alpha 0xC000.
    - PLL1 at +0x100.
    - GMU RCG at +0x1120: 200 MHz = GPLL0 div / 1.5.
    - GX gfx3d RCG at +0x101c: 180–825 MHz from the CRC divider.
    - Branches: ahb +0x1078, cx_gmu +0x1098, cxo +0x109c, cx_snoc_dvm +0x108c, cx_apb +0x1088,
      cx_gfx3d +0x10a4, gx_gfx3d +0x1054, gx_cxo +0x1060, gx_gmu +0x1064.
- **gcc** (`gcc-sdmmagpie.c`): gpu_cfg_ahb 0x71004, gpu_memnoc_gfx 0x7100c, gpu_snoc_dvm_gfx 0x71018,
  ddrss_gpu_axi 0x4452c, gpu_gpll0 / gpll0_div = enable bits 15 / 16 of 0x52004.

**Read on the phone** (Stage 0, boot G0, 2026-10-25):

- Speed bin: QFPROM 0x7841a0 reads 0x12468410 from EL1 without a fault, so **bin 146**. Its DTS table is
  700/650/565/430/355/267/180 MHz (levels 0–6, bus votes 11/10/9/8/7/6/4), initial level 6 = 180 MHz,
  ca-target level 4. Bin 0's 825 MHz table does not apply to this phone.
- cmd-db ARC level tables (hlvl index → RPMh vlvl):

  | Resource | addr | vlvl per hlvl |
  |---|---|---|
  | `gfx.lvl` | 0x30050 | 0 16 64 128 192 256 320 384 416 |
  | `cx.lvl` | 0x30000 | 0 16 64 128 192 256 384 |
  | `mx.lvl` | 0x30010 | 0 16 192 320 384 |
  | `lcx.lvl` / `lmx.lvl` | 0x30030 / 0x30040 | 0 16 64 128 256 384 / 0 16 128 256 384 |
  | `ddr.lvl` / `xo.lvl` | 0x30070 / 0x30090 | 0 32 128 160 / 0 32 80 128 |

  `ebi.lvl` and `mmcx.lvl` exist but have no level table. A second ARC header lists 10 `*.mol` entries
  (addr 0, one level each; `gfx.mol` 64, `mx.mol` 320).
- cmd-db BCMs (40): `MC0` 0x50000 (unit 1770000, width 16, vcd 0), `SH0` 0x5003c (unit 2090000, width 32,
  vcd 1), `ACV` 0x5000c (vcd 11). VRMs (64): `vrm.gfx` = smpa2 0x40600, `vrm.cx` = smpc2 0x42700,
  `vrm.mx` = smpa3 0x40000, which matches the rails in section 3.
- The four firmware blobs reach the kernel through `/chosen/memory-map` at the addresses and FNV-1a
  hashes mkpayload printed.

**Memory:**

- XNU's RAM is 0xb0000000+0x4d000000, or 0x100000000+0xfcc00000 in high-memory mode.
- An identity map of 0x80000000–0x1_ffff_ffff covers it, plus the splash carve-out (0x9c000000, 23 MB)
  and the zap carve-out.

## 5. Licence (decide before Stage 7)

ios-sailfish has **no LICENSE file**, so by default nothing in it may be copied. Upstream ios-sweet is
MIT, Mesa is MIT, and the freedreno register XML in Mesa is MIT. Mainline Linux is GPL-2.0: use it as a
reference for hardware sequences, and write our own code. The options:

1. **Ask the author (joranv1) to add a licence**, ideally MIT like upstream. This matters most for
   `eagl.c` / `sweet_drm.c` / `gen-dispatch.py`, the parts that are hardest to redo. The owner would make that
   contact; I will not post anything on the owner's behalf.
2. **Clean-room.** Use their write-up only as a map of *what* is needed, and write our own kernel and
   user-space code. The kernel side has to be rewritten for a6xx anyway. The EAGL shim holds a lot of
   reverse-engineered CoreAnimation behaviour that would have to be found again.

Stages 0–6 need nothing from their code, so the licence question blocks only Stage 7.

## 6. Stages

Each stage is a kernel patch in `xnu/patches/` built by the GitHub Actions workflow, booted with
`fastboot boot` only, so nothing persists. Boot-arg `sweetgpu=N` picks how far to go. N=0 or no boot-arg means
the GPU is untouched, so the everyday H4 behaviour stays the default.

### Stage 0: groundwork (no GPU register is touched)

1. **Firmware delivery.**
   - Add `--blob NAME=path` to `tools/mkpayload.py`. The blobs go after the ramdisk, and each gets a
     `/chosen/memory-map` property (`GPU-SQE`, `GPU-GMU`, `GPU-ZAP-MDT`, `GPU-ZAP-B02`…, 16 bytes = pa, len),
     as in their 0120.
   - `tools/sunfish/mkboot.sh` reads them from `~/ios-pixel/gpu-fw/fw/` with `GPUFW=1`. Boot images with blobs
     stay out of git.
2. **Probe framework.**
   - Add a `bsd/dev/sweet_gpu.c` skeleton: a thread starts about 40 s after boot when `sweetgpu=N`.
   - Each step logs, then sleeps 2 s before any access that could hang, so the 0047 log holds the last
     good step. It flushes every `ufslogsecs` seconds (default 10), so GPU test boots pass `ufslogsecs=1`.
   - Check that `tools/sweet-log-pull.sh` recovers the log after a deliberate hang.
3. **Register header.** Generate `a6xx` / `a6xx_gmu` / `adreno_pm4` offsets from Mesa's
   `src/freedreno/registers/adreno/*.xml` (MIT) with a small script. No hand-typed offsets.
4. **cmd-db dump** (read-only):
   - Extend `sweet_rpmh.c` to list ARC (slave 3) and BCM (slave 5) entries with their aux data:
     `gfx.lvl`, `cx.lvl`, `mx.lvl` level tables, and the `MC0` / `SH0` / `ACV` BCMs.
   - PDC programming (Stage 2B) and any direct votes (Stage 2A) take their addresses from this, not from
     constants.
5. **Speed bin:** read qfprom +0x41a0 and log the bin.

✅ Pass when a boot with `sweetgpu=1` logs the blob addresses, the cmd-db table and the speed bin, then
reaches the home screen as H4 does.

**Result (2026-10-25, commit dab0e56, kernel 0094, image G0):** passed. All four blobs checked out, the
cmd-db and speed-bin results are in section 4, and the home screen came up as with H4.

**Hang test (image G0H = G0 + `sweetgpuhang=1`):** passed for log recovery. The log was pulled after a
forced power-off, as `tools/sweet-log-pull.sh` does it, and held the `next: deliberate hang` line. One
core spinning with interrupts off did **not** freeze iOS: the phone kept responding for over 2 minutes, and
3686 more lines (12 of them CPUFREQ status) reached the log after the hang line. With `wdt=-1` nothing
notices a stuck core. For later stages this means:

- A stuck register access may leave the phone running. A `next:` line with no result line after it marks
  the hang, so every risky step must log its result.
- A stall of the whole SoC was not exercised. The 2 s pause plus `ufslogsecs=1` (0047 ends each flush with
  a UFS cache sync) puts the `next:` line on disk before the access, so it should still be there.

### Stage 1: CX side and the GMU block (reads only)

1. GCC: enable `gpu_cfg_ahb`, `ddrss_gpu_axi`, `gpu_memnoc_gfx`, `gpu_snoc_dvm_gfx`, and the
   `gpu_gpll0` / `gpll0_div` branches.
2. gpucc:
   - Turn the CX GDSC on (0x509106c, poll PWR_ON).
   - Set the GMU RCG to 200 MHz.
   - Enable cx_gmu, cxo, ahb, cx_snoc_dvm and cx_apb.
3. Read CX-domain registers only: GMU `CM3_FW_INIT_RESULT`, GMU always-on counter, `GPU_CC` IDs,
   `cx_misc`. **Reading a GX register now would hang the bus**, so the probe must not touch any.

✅ Pass when the log shows sane GMU/CX values and the phone keeps running.

**Built (patches 0095 + 0096, `sweetgpu=2`).** It runs after stage 0, in this order. Each step has its own
`next:` pause and logs before/after values:

1. If `gcc_gpu_cfg_ahb` (critical in Linux) is off, turn it on. gpucc is unreachable without it.
2. CX GDSC, following gdsc-regulator.c:
   - Clear HW_CONTROL and SW_OVERRIDE.
   - Set CLK_DIS_WAIT to 8.
   - Clear SW_COLLAPSE.
   - Poll PWR_ON at the hw-ctrl address 0x5091540 for up to 1 ms.

   It comes first, as in upstream `a6xx_gmu_resume()` (power domain before clocks). It is the GMU's
   `vddcx-supply` in the stock DT.
3. GCC: set the `gpu_gpll0` and `gpu_gpll0_div` votes (0x52004 bits 15 and 16). Then turn on
   `ddrss_gpu_axi` (0x4452c), `gpu_memnoc_gfx` (0x7100c) and `gpu_snoc_dvm_gfx` (0x71018) and wait for
   CLK_OFF to clear.
4. GMU RCG (0x5091120): CFG = source 6 (GPLL0 div, 300 MHz), divider 1.5 → 200 MHz. Then UPDATE.
5. Set wake/sleep 0xf on cx_gmu, as gpucc probe does. Then turn on ahb, cxo, cx_gmu, cx_snoc_dvm and
   cx_apb.
6. Reads:
   - GMU CM3/HFI/SPTPRAC/RPMh-state/AO/fence registers.
   - The always-on counter twice, 1 ms apart (expect ~19200 ticks).
   - The first 4 ITCM words.
   - cx_misc SYSTEM_CACHE_CNTL_0/1.

GMU offsets come from `sweet_gpu_regs.h`, a filtered copy made by
`gen-adreno-regs.py --only … --single sweet_gpu_regs.h`. They count from 0x5000000, as upstream
`gmu_read()` does; the probe subtracts 0x6a000 for the GMU mapping. GCC and gpucc offsets come from
gcc-/gpucc-sdmmagpie.c.

GX is never written. Its GDSCR, clamp register and PLLs are only read, through gpucc. Everything stays on
afterwards, because Stage 2 builds on it.

**G1 result (0095, GCC before the GDSC), 2026-10-25 05:10.** The phone booted and kept working. The stage
stopped cleanly at the GCC step.

Before:

| Register | Value | Meaning |
|---|---|---|
| `gpu_cfg_ahb` | 0x00000001 | on |
| `ddrss_gpu_axi` | 0x00000000 | off |
| `memnoc_gfx` | 0x80000220 | off |
| `snoc_dvm_gfx` | 0x80000001 | enabled but off |
| GCC vote | 0x01241d7b | |
| `gpu_misc` | 0x00010000 | |
| `cx_gdscr` | 0x00222001 | collapsed |
| `hw_ctrl` | 0x00005ff4 | PWR_ON clear |
| `gx_gdscr` | 0x00222001 | |
| `gx_domain` | 0x1 | clamped |
| GMU RCG | 0x80000000 / 0 | XO, root off |
| `cx_gmu` | 0x80004220 | |
| `ahb`, `cxo` | | on |
| `cx_snoc_dvm`, `cx_apb` | | off |
| PLL0/PLL1 mode and L | 0 | |

Steps:
- The votes took: 0x01241d7b → 0x01259d7b.
- `ddrss_gpu_axi` came on.
- `memnoc_gfx` (0x80000221) and `snoc_dvm_gfx` stayed at CLK_OFF, because they feed the collapsed CX domain.

Patch 0096 turns the CX GDSC on first.

**G1b result (0096): ✅ Stage 1 PASSED, 2026-10-25 05:29.** The owner saw the home screen, and the phone stayed
stable. The log has "stage 1 done", then 16,554 more lines (3,982 of them CPU-frequency and touch heartbeats),
with no panic or fault. Every step logged its result.

- **CX GDSC:** gdscr 0x00222001 → 0xf8228000, hw_ctrl 0x00005ff4 → 0x80001fe0 (PWR_ON).
  - Powering the domain alone took gpucc `ahb` and `cxo` from 0x80000001 to on.
  - It also turned `snoc_dvm_gfx` on, since its enable bit was already set at boot.
- **GCC:** votes 0x01241d7b → 0x01259d7b. `ddrss_gpu_axi`, `memnoc_gfx` (0x00000221) and `snoc_dvm_gfx` are on.
- **GMU RCG:** CFG 0 → 0x602 (200 MHz), UPDATE cleared. CMD reads 0 (root on) once `cx_gmu` runs.
- **gpucc:** `cx_gmu` 0x80004ff0 → 0x00004ff1 on. `ahb` and `cxo` on.
- **Unexpected:** the `cx_apb` and `cx_snoc_dvm` enable bits read back 0 after the write (0x80000000 and
  0x00000000). Neither is on the GPU path, so this blocks nothing:
  - The kgsl GPU node uses `core_clk rbbmtimer_clk mem_clk mem_iface_clk gmu_clk`.
  - The GMU node uses `gmu_clk cxo_clk axi_clk memnoc_clk`.
  - gpucc clock 0x08 (`gpu_apb_clk`) is used only by CoreSight funnel@6943000 and tpdm@6940000.
  - No node uses cx_snoc_dvm.
- **GMU reads:**

  | Register | Value | Meaning |
  |---|---|---|
  | `CM3_SYSRESET` | 1 | CM3 held in reset |
  | `BOOT_CONFIG` | 0 | |
  | `FW_BUSY` | 0 | |
  | `FW_INIT_RESULT` | 0 | |
  | `HFI_CTRL_STATUS` | 0 | |
  | `HFI_VERSION_INFO` | 0 | |
  | `SPTPRAC_PWR_CLK_STATUS` | 0xd4 | SPTP GDSC off, SP clock off, GX HM GDSC off, GX HM clock off |
  | `CX_GMU_RPMH_POWER_STATE` | 0xf | no firmware has run |
  | `AO_GPU_CX_BUSY_STATUS` | 0x30000000 | bits 28–29 are not named in the XML |
  | `AO_AHB_FENCE_CTRL` | 0 | Stage 2A step 2 is already the case |
  | `AHB_FENCE_STATUS` | 0 | |

- **Always-on counter:** 153803027 at the first read, then +21696 across `delay(1000)`.
  153803027 / 19.2 MHz = 8.01 s, which is the four 2 s pauses since the CX GDSC came on. So the counter
  starts at 0 when CX powers up and counts XO at 19.2 MHz. `delay(1000)` overshoots by about 13%.
- **ITCM:** `2060e473 06510925 e66e14c9 cd804228`. This is uninitialised SRAM, not firmware; the firmware
  would start with sp 0x43f80, reset 0x9. It reads without a hang.
- **cx_misc:** `SYSTEM_CACHE_CNTL_0/1` = 0/0.
- **GX:** `gx_gdscr` 0x00222001 (collapsed), `gx_domain` 0x1 (clamped), PLL0/PLL1 0. All untouched.

### Stage 2: GX power (decision gate)

**2A: GMU-less, host-driven (try first).** Every step reuses what we have and is observable on its own.
This mirrors how their 0119 powered the a530. Mainline's "GMU wrapper" a6xx parts (a610/a619_holi) show
that the GPU core runs with the host providing GX power. The steps:

1. Vote `gfx.lvl` to the 180 MHz corner (lowest active level) through the apps RSC. This generalises
   `sweet_rpmh.c` from VRM to ARC. `mx.lvl` must stay ≥ that level; read it first.
2. Clear the GMU AHB fence (`GMU_AO_AHB_FENCE_CTRL = 0`).
3. GX GDSC on (0x509100c): un-clamp IO, poll.
4. SPTP RAC on (`GMU_GX_SPTPRAC_POWER_CONTROL`, poll `SPTPRAC_PWR_CLK_STATUS`). This is what the GMU
   firmware would do.
5. Clocks:
   - PLL0 with the downstream config.
   - CRC divider, then the gfx3d RCG to 180 MHz.
   - Enable gx_gfx3d, gx_cxo, cx_gfx3d and gx_gmu.
6. Read `RBBM_STATUS`, the GBIF/VBIF version and the RBBM always-on counter twice. The counter must
   advance.

Risks:
- RPMh may ignore an apps-DRV vote on `gfx.lvl`. Check this with the ARC status read-back.
- The PLL may not lock. Check this with the lock bit.
- Levels are abstract corners, so a wrong vote cannot over-volt the rail. The worst case is a hang and
  reboot, which `fastboot boot` makes harmless.

**Built (patch 0097, `sweetgpu=3`).** It runs after stage 1 passes. The order follows the downstream drivers
(gdsc-regulator.c, clk-alpha-pll.c, clk-rcg2.c) instead of the list above. Each step logs a "next:" line
first, and the stage stops at the first failure:

| Step | What it writes | Check |
|---|---|---|
| 1. `mx.lvl` vote | one ACTIVE TCS command, data = hlvl | TCS completion |
| 2. `gfx.lvl` vote | the same | TCS completion |
| 3. PLL0 | L 0x12, FRAC 0xc000 (360 MHz), CAL_L 0x3f, the downstream config/user/test values, even post-divider 2; out of reset, RUN, then the outputs | lock bit within 200 µs |
| 4. gfx3d RCG (0x509101c) | ROOT_EN, then CFG = crc_div / 1 (180 MHz) and UPDATE | ROOT_OFF clears; UPDATE clears |
| 5. GX GDSC (0x509100c) | BCR 0x5091008 pulse, GMEM reset pulse and unclamp in 0x5091508, clear SW_COLLAPSE | PWR_ON within 500 µs; on timeout it collapses and clamps again |
| 6. GX branches | gx_cxo, gx_gmu, gx_gfx3d, cx_gfx3d enable bits | CLK_OFF clears (gx_cxo, gx_gmu) |
| 7. SPTP | `GMU_GX_SPTPRAC_POWER_CONTROL` = 0x778000 | status & 0x38 == 0x28 |
| 8. GX reads | none | only if the GMU's GX head-switch and clock bits read on |

How it differs from the plan:
- Both votes ask for vlvl ≥ 64 (LOW_SVS), mapped to an hlvl the way rpmhpd does it. 64 is what
  gdsc-regulator.c votes on the GX GDSC's parent. 180 MHz needs MIN_SVS (48), and gfx.lvl has no level
  below 64. So gfx gets hlvl 2 (vlvl 64), and mx gets hlvl 2 (vlvl 192, its lowest active level). mx is
  voted first so that it stays ≥ gfx. `sweet_rpmh_arc_vote` accepts only these two resources and vlvl ≤ 64.
- There is no ARC status read-back from the apps side. The GX GDSC reaching PWR_ON, and the GMU's
  `GX_HM_GDSC_POWER_OFF` bit clearing, are the evidence that the rail came up.
- The fence is not written: it already reads 0 (G1b).
- PLL0 runs at 360 MHz with the even divider at 2. This is the downstream config's own L and alpha, and it
  sits inside both Fabia VCO ranges. Linux would reach 180 MHz either this way or with the PLL at 180 MHz.
- No GBIF/VBIF version read: a618 has a GBIF, and the register XML has no version register for it
  (`VBIF_VERSION` belongs to a630's VBIF). The reads are `RBBM_STATUS`, `STATUS1-3`, `RBBM_CLOCK_CNTL` and
  the CP always-on counter twice, next to the GMU counter.
- The gx_* branch registers are read only after the GX GDSC is on.
- Everything stays on afterwards. The votes are ACTIVE only; iOS never enters the system sleep that would
  drop them.

**2B: the real GMU path (fallback; it is what mainline Linux does for a618).**

1. Load `a618_gmu.bin` into ITCM (legacy raw image; at most 0x8000 bytes).
2. Program the GMU RSCC and the PDC sequences. The TCS command addresses come from cmd-db (Stage 0.4).
3. Map the GMU's fixed-VA memory (HFI queues, logs) in the GPU SMMU for SIDs 4/5 (Stage 3).
4. Release CM3 reset and wait for `0xbabeface`.
5. HFI v1 with the `hfi-use-reg` handshake: INIT, FW_VERSION, PERF_TABLE (gfx/cx ARC votes from cmd-db),
   BW_TABLE (a618's BCM table), TEST.
6. Send `OOB_GPU_SET`. The GMU then powers GX.

This path costs more code and has more unknowns per boot, but it is proven on this exact GPU.

✅ Pass when the always-on counter advances and `RBBM_STATUS` reads idle.

### Stage 3: GPU SMMU

- Same idea as their 0121:
  - CB0 stage-1 AArch64, 4K granule, T0SZ 28.
  - 1 GB identity blocks for 0x80000000–0x1_ffff_ffff.
  - MAIR normal non-cacheable.
  - SMR0 matches all streams, S2CR0 → CB0.
  - Then TLBIALL, sync and read back.
- Read IDR0/IDR1 first, and log every global register before and after.
- Sunfish-specific risk: **the hypervisor may trap or refuse SMR/S2CR writes**, or stage 2 may block
  parts of DRAM. The readback is the test. If it fails, use the context bank and SMR layout the bootloader
  or hypervisor already set up for SID 0 (dump it first), and only write CB0's TTBR/TCR/SCTLR.
- Fault IRQs are not wired up. Poll the fault status register (FSR) and log it in every probe step.
- Add the 8–16 GB GPU VA window (L1[8..15] → L2 → L3, 4 KB pages, one guard page after each mapping), as
  in their 0137/0150.

✅ Pass when the readback matches and no fault is latched.

### Stage 4: CP first light (SQE)

1. Load the SQE image into a GPU-visible page. Write `CP_SQE_INSTR_BASE`.
2. Program the a618 hardware-init list:
   - GBIF/VBIF, UCHE ranges and trap base.
   - UBWC / bank-bit registers, set to the DT values: highest-bank-bit 14, UBWC mode 2.
   - CP_PROTECT: left open for now.
   - RBBM clock-gating settings.
   - Take the list from Mesa/mainline register names; it is a list of register writes, not code.
3. 16 KB ring, `CP_ME_INIT` (8 dwords), start the SQE.

Expected: the ring read pointer advances. A `CP_MEM_WRITE` marker to normal memory will **not** land
while the GPU is still secure (their 0130 finding). Use `CP_SCRATCH` registers or RPTR as the proof.

✅ Pass when RPTR equals WPTR after a NOP stream and the SQE version register reads back.

### Stage 5: zap shader (leave secure mode)

1. First a harmless SCM query: `PAS_IS_SUPPORTED(13)` over SMCCC SIP (svc 2, cmd 7).
2. `PAS_INIT_IMAGE(13, metadata)`. The metadata is the `.mdt` header plus program headers plus the hash
   segment, in one physically contiguous page.
3. `PAS_MEM_SETUP(13, 0x97715000, 0x2000)`.
4. Copy the LOAD segment (`.b02`, paddr 0x5000 relocated to the carve-out). Then `PAS_AUTH_AND_RESET(13)`.
   Retry on INTERRUPTED (1) and EBUSY (-12), as their 0130 does.
5. Ring: `CP_SET_SECURE_MODE 0`, then `CP_MEM_WRITE 0xc0ffee01`.
6. Because of the `secvid-set-once` quirk, **never** write `RBBM_SECVID_TRUST_CNTL` ourselves.

✅ Pass when the marker lands in DRAM. That is the GPU's first visible write.

### Stage 6: kernel GPU service and user client

- Port the *design* of their SweetGPU user client to our own code, with the same selector numbers so a
  sweetgl port maps 1:1:
  - 10 bo new
  - 11 bo free
  - 12 bo import IOSurface
  - 13 submit (IBs as `CP_INDIRECT_BUFFER_PFE`, then `CACHE_FLUSH_TS` to the fence page)
  - 14 wait
  - 15 info
- Info returns:
  - chip_id 0x06010800
  - gmem_size 0x80000
  - gmem_base 0x100000
  - max_freq for the current level
  - the fence page
- Hang detection: if the fence does not move for 2 s, the service turns off until reboot.
- Self-test: a kernel-built clear and copy (CP_BLIT / `CP_MEM_TO_MEM`), checked by the CPU.
- Clock steps 180 → 430 → 565 MHz, each needing the matching `gfx.lvl` corner, and a DDR BCM vote
  (their 0146) only if bandwidth turns out to be the limit.

✅ Pass when `sweetgpu=8` logs "service ready" and the self-test reports 0 bad pixels.

### Stage 7: sweetgl for a618 (needs the licence decision)

1. **Build.** Add a GitHub Actions macOS job next to the kernel build:
   - Mesa at a pinned release, freedreno only, built with the iphoneos SDK, cached between runs.
   - The artifact is `sweetgl.dylib`, zero-padded to 64 MiB.
2. **msm emulation for a6xx:**
   - GPU_ID 618 and CHIP_ID 0x06010800; check both against Mesa's `freedreno_devices.py`.
   - GMEM 512 KiB.
   - Answer the params a6xx asks for: highest bank bit 14, UBWC swizzle / macrotile, UCHE trap base,
     priorities, timestamp from the always-on counter.
   - Do not report `VA_START`, so Mesa uses kernel IOVAs.
   - Start with `FD_MESA_DEBUG=nogmem,noubwc` (linear only).
3. **mesa.patch.** The pitch-alignment hunk is `is_a5xx`-only. Check a6xx's rules for imported linear
   IOSurfaces (828 px × 4 = 3312 B, not 64-aligned unless our SweetSurface pads rows) and extend the hunk
   or pad surface rows.
4. **EAGL shim.**
   - It is mostly GPU-agnostic.
   - Mesa's a6xx driver should advertise framebuffer fetch (check `PIPE_CAP_FBFETCH` on the pinned
     release). Test whether CA's `gl_LastFragData` shaders compile natively before carrying over the
     texture-copy emulation.
5. **Kernel plumbing** (our own code):
   - `sweetenv=` boot-arg (exec-time env injection, several processes).
   - Answer dyld's AMFI op **0x5a** in our 0092 handler.
   - An `IOAcceleratorES` stub without `MetalPluginName`.
6. **Test order:**
   1. A standalone test binary (`SWEETGL_TEST=1`-style offscreen triangle, read back).
   2. `backboardd` with `DYLD_INSERT_LIBRARIES`.
   3. Check SpringBoard, CC toggles, App Library plates and blur.
7. **Deployment.** Putting `/usr/lib/sweetgl.dylib` on the root is a root edit: pull, journal replay,
   `fsck.hfsplus -fy`, edit, push. That is **destructive and needs the owner's OK** at that time. Later
   updates are same-size in-place block writes, like `dscpatch.ps1`.

✅ Pass when the home screen renders through Mesa, with `CA_*` debug showing the GLES context.

### Stage 8: present path

- Today (H4) the CPU copies 5.9 MB per frame into the canvas buffer in the splash carve-out, and the MDP
  scales it. Two options:
  - **(a) GPU copy.** A kernel-built blit copies the swap surface into the canvas, removing the CPU copy.
    Small and safe.
  - **(b) Zero-copy.** Allocate the swap surfaces physically contiguous (below 4 GB, 64-byte-aligned
    rows) and point VIG0 straight at the surface backboardd just swapped. The CTL flush latches it at
    the next frame. This would help even the current CPU path. It needs three 5.9 MB contiguous
    buffers, which means either reclaiming the 10 MB panel buffer once VIG0 is switched, or allocating
    them early in boot.
- Start with (a) and look into (b) afterwards.

### Stage 9: tuning

- Clock levels up to 650/825 MHz, if thermals allow.
- DDR bandwidth votes (BCM through the apps RSC).
- GMEM rendering, once surfaces are padded.
- UBWC for CA's private textures.
- fp16 (sailfish had to turn it off).

## 7. Safety and test protocol

- Kernels are tried with `fastboot boot` only: nothing is written to the phone.
- The only writes are the Stage 7 root edit and later dylib updates. Each needs the owner's OK at that time.
- A GPU hang or a bad SMMU setup at worst freezes or resets the phone. Hold power to reboot, then pull the
  log in TWRP. H4 without `sweetgpu` stays the safe everyday boot.
- No raw voltage writes: rails are RPMh corner votes, and clocks come from downstream tables.
- Vendor firmware, boot images that contain it, and device logs stay out of git.

## 8. Risks, most uncertain first

1. **GX power.** Whether the apps DRV may vote `gfx.lvl` (2A), versus the larger GMU/HFI/PDC job (2B).
2. **The hypervisor and the GPU SMMU.** Trapped global registers, or stage-2 holes.
3. **TrustZone PAS for the zap on Pixel firmware.** It may need GPU clocks on, or the metadata in specific memory.
4. **sweetgl's a5xx assumptions on a6xx.** Pitch rules, params, nogmem/nofp16 workarounds.
5. **Licence** of the sweetgl code (section 5).
6. **Build cost.** Mesa on GitHub macOS runners; mitigated by caching.

## 9. Next concrete step

Stages 0 and 1 passed on the phone. Stage 1 needed one fix: patch 0096 turns the CX GDSC on before the GCC
GPU branches. The owner approved Stage 2A. It is built as patch 0097 (`sweetgpu=3`). Next is the test boot
(image G2), once the owner has seen the patch and approves the boot. If the GX GDSC never reaches PWR_ON
after the votes, the apps DRV cannot raise `gfx.lvl`, and the fallback is 2B.
