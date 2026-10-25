# Adreno 618 GPU acceleration plan (sunfish / Pixel 4a)

Status, 2026-10-25: **Stages 0 and 1 passed on the phone (kernel 0096, image G1b). Stage 2A ran on G2, G2b
and G2m (patches 0097–0099): the rail votes, PLL0 and the GX GDSC work, and the GMU sees GX powered. The
GCC counter shows the GPU core clock (gx_gfx3d) does not run at all, so no GX register has been read.
Per the 2A decision rule, the next path is 2B (the GMU powers GX). 2B is planned in detail in "Stage 2B"
below and waits for the owner's go-ahead; nothing of it is built.** Nothing is flashed: every test is a
`fastboot boot` image. The firmware is extracted. The hardware facts
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
| `a618_gmu.bin` | 32768 B, raw Cortex-M3 image: 16 KiB code for ITCM (vector table first), then 16 KiB of DTCM data (corrected in Stage 2B) |
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

**Result on G2 (kernel 0097, booted 10:17:04).** The owner saw the home screen and a stable phone. The log
(flush #206, 6537 lines) has no panic or fault, and 4394 lines follow the stage's last line. Stage 1 matched
G1b. Observed in 2A:
- `mx.lvl` vlvl 64 → hlvl 2 (vlvl 192) and `gfx.lvl` vlvl 64 → hlvl 2 (vlvl 64). Both went through TCS0
  with cmd-status 0x10101 (issued and completed). This shows RPMh accepted the votes, not that the rail
  came up.
- PLL0 locked: mode 0 → 0xd0800005 (LOCK_DET, ACTIVE, RESET_N, OUTCTRL), opmode RUN, USER_CTL 0x107. L
  0x12, CAL_L 0x3f and FRAC 0xc000 read back.
- The gfx3d RCG did not turn on: CMD 0x80000000 → 0x80000002. ROOT_EN stuck, but ROOT_OFF stayed set for
  500 µs, with CFG still 0 (XO). The stage stopped there, so the GX GDSC (gdscr 0x00222001, domain 0x1 =
  clamped) and SPTP (0xd4) were not touched.

Downstream does not treat this as fatal. The sunfish GX GDSC node (sdmmagpie.dtsi) has
`qcom,force-enable-root-clk` on `GPU_CC_GX_GFX3D_CLK_SRC`, `parent-supply = <&VDD_GFX_LEVEL>` and
`qcom,reset-aon-logic`. gdsc_enable then:
1. sets the parent to LOW_SVS;
2. calls clk_prepare_enable on the root, which goes through clk_rcg2_set_force_enable. If the root stays
   off, that function only prints a "rcg didn't turn on" warning and returns 0;
3. runs the BCR/GMEM/unclamp/un-collapse sequence.

So downstream never waits for this root before the GDSC. The RCG is a `gx_` clock with `vdd_gx`, and GX was
still clamped. Our guess is that the root, or its status, needs the GX side unclamped. That is not verified.

**Patch 0098 (image G2b).** It follows downstream: the root is forced on, and a root that stays off is
logged and the stage goes on. Then the GX GDSC (unchanged, reverted on timeout). With GX unclamped, the
root is checked again, and the switch to 180 MHz (crc_div) happens only once the root runs. The branches
and SPTP run either way. The GX reads also need the gfx3d root at 180 MHz, on top of the GMU's GX
power/clock bits.

**Result on G2b (kernel 0098, booted 10:38:34).** The owner saw the home screen and a stable phone. The log
(5759 lines) has no panic or fault, and 3417 lines follow the stage's last line. Stage 1, both votes and
PLL0 matched G2. Observed in 2A:
- Before the GDSC the gfx3d root stayed off again (CMD 0x80000000 → 0x80000002, CFG 0 = XO). The stage went
  on, as planned.
- **The GX GDSC came on:** gdscr 0x00222001 → 0xf8222000 (PWR_ON), domain 0x1 → 0 (unclamped). So the apps
  DRV `gfx.lvl` vote does bring up the GX rail, which was the first open question of 2A.
- The GMU's SPTPRAC_PWR_CLK_STATUS went 0xd4 → 0x94. Bit 6 (GX_HM_GDSC_POWER_OFF) cleared, so the GMU sees
  the GX head switch on. Bit 7 (GX_HM_CLK_OFF) stayed set.
- With GX unclamped the root still read off (CMD 0x80000002, CFG 0). 0098 only updates CFG once the root
  runs, so it never tried 180 MHz.
- Branches: gx_cxo on, and gx_gmu 0x80000000 → 0x00000001 (on). gx_gfx3d (0x80004220 → 0x80004221) and
  cx_gfx3d (0x80000000) still show CLK_OFF. Neither has a halt check here, and downstream marks gx_gfx3d
  BRANCH_HALT_SKIP.
- SPTPRAC enable: 0x94 → 0x94, not on. The check wants 0x28 (SPTP on, SP clock on, GMU_UP). Our guess, not
  verified: GMU_UP (bit 5) cannot come up while the CM3 is held in reset, so this check may never pass on 2A.
- The stage stopped before any GX read, as designed.

The open question is whether the gfx3d RCG really does not run, or only its status says so. Possible
causes, none verified:
1. The RCG needs a CFG update after GX power-up. Downstream's set_rate always does one inside the force
   enable, whether or not the root came on.
2. XO does not reach the GX side of the RCG.
3. The GX status bits are not reliable (downstream skips the gx_gfx3d halt check).
4. The GMU firmware normally owns this clock, which would point to 2B.

**Patch 0099 (image G2m).** sweet_gpu.c only. After the GX branches it uses the GCC frequency counter
(debugcc-sdmmagpie.c, clk-debug.c): gpucc debug mux → GCC debug mux 0x144 → a counter gated by XO/4.
- **First measurement.** It measures cx_gmu, gx_gfx3d, cx_gfx3d, gx_cxo and gx_gmu. Stage 1 set cx_gmu to
  200 MHz, so cx_gmu checks the method.
- **Update.** It then sets gfx3d to crc_div divider 1 (180 MHz) with an UPDATE even if the root reads off,
  as clk_rcg2_set_rate does.
- **Second measurement.** It measures cx_gmu, gx_gfx3d and cx_gfx3d again.
- **Gate for the GX reads.** All of these must hold:
  - the update must take;
  - gx_gfx3d must run at 100–250 MHz on the counter. The counter is trusted only when cx_gmu reads
    150–250 MHz; otherwise the root status bit decides;
  - the GMU GX bits must be on, as before.
- **Safety.** The counter uses bounded polls (1 ms, then 50 ms). It only touches the GCC and gpucc debug
  registers, and it turns their enables off again.

How to read G2m:
- cx_gmu far from 200 MHz: the counter setup is wrong; fix that first.
- gx_gfx3d near 19.2 MHz before the update, or near 180 MHz after it: the RCG runs and its status bit
  lies (cause 3), or the update started it (cause 1).
- gx_gfx3d 0 before and after: the RCG really does not run (cause 2 or 4).
- If gx_gfx3d runs but GX_HM_CLK_OFF stays set, the stage still stops before the reads. That bit is the
  next thing to study.

**Result on G2m (kernel 0099, booted 11:05:50).** The owner reported a working phone. The log (flush #133,
5179 lines) has no panic or fault, and 2710 lines follow the stage's last line. Everything up to the GX
branches matched G2b. The counter measured:

| clock | after the GX branches (CFG = XO) | after the 180 MHz update |
|---|---|---|
| cx_gmu | 200001 kHz | 199999 kHz |
| gx_gfx3d | 0 (not running) | 0 (not running) |
| cx_gfx3d | 0 (not running) | 0 (not running) |
| gx_cxo | 19201 kHz | not measured |
| gx_gmu | 199999 kHz | not measured |

- The counter method works. cx_gmu reads the 200 MHz that stage 1 set, and gx_cxo reads XO.
- Clocks on the powered GX side can run: gx_gmu runs at 200 MHz and gx_cxo at 19.2 MHz.
- The gfx3d RCG took the update (CFG 0 → 0x101, UPDATE cleared), but its root still read off. gx_gfx3d
  and cx_gfx3d count nothing, both from XO before the update and from crc_div after it. So the status bit
  was right: **the GPU core clock does not run when the host drives it.**
- SPTPRAC stayed 0x94, and the stage stopped before any GX read, as designed.

This rules out causes 1 and 3 above. Cause 2 is unlikely, because gx_cxo, which comes from the same XO,
runs. That leaves cause 4: something the host cannot see holds the gfx3d root off, most likely the GMU.
Supporting facts from downstream:
- The GMU node in sdmmagpie-gpu.dtsi lists only cx_gmu, cxo, the AXI and the memnoc clocks; gfx3d is not
  among them. Only the kgsl GPU node names gfx3d (as core_clk).
- No DT node enables gpu_cc_crc_ahb_clk or the ACD clocks. They are left to the GMU firmware or to the
  hardware defaults.
- Mainline Linux runs the a618 in sc7180 with the GMU firmware setting the GPU frequency. As far as we
  know it has no host gfx3d clock, and its GX GDSC enable is a no-op left to the GMU. This is from memory
  and was not re-checked in this session.

2A ends here, per its decision rule: gx_gfx3d never ran. The next path is 2B. A last cheap 2A try would be
turning on crc_ahb and the ACD clocks, but nothing in downstream does that, so it is a guess.

### Stage 2B: the GMU powers GX (detailed plan, nothing built yet)

Planned 2026-10-25, after 2A hit its decision rule. The owner asked to see this plan before any building.

**Idea.** On this GPU the GMU firmware owns GX power and the GPU core clock. 2B runs Qualcomm's own
`a618_gmu.bin` the way the sunfish kernel's kgsl does: give it memory, load it, start it, then ask it for
GX through its mailbox registers and HFI messages. An OOB ("out of band") request is one of those mailbox
bits: the host sets a request bit and the GMU answers with an ack bit.

**Sources, read in this session:**
- The sunfish downstream kgsl: android.googlesource.com `kernel/msm`, branch
  `android-msm-sunfish-4.14-android13-qpr3`, `drivers/gpu/msm/`: `adreno_a6xx_gmu.c`, `kgsl_gmu.c/h`,
  `kgsl_hfi.c/h`, `kgsl_gmu_core.h`, `a6xx_reg.h`, `adreno.h`, `adreno-gpulist.h`.
- Mainline Linux as a cross-check: `kernel/common` android-mainline, `drm/msm/adreno/a6xx_gmu.c` and
  `a6xx_hfi.c`.
- The ARM SMMUv2 architecture spec for the SMMU registers.
- Our cmd-db dump (Stage 0) and the stock DTB.

Both kernels are GPL-2.0. We take register offsets, values and order from them, which are hardware facts,
and write our own code (§5).

**What downstream does for a618:**
- kgsl puts a618 in its "a615 family" (`adreno_is_a615_family`: A615, A616, A618). That family uses the
  legacy GMU interface:
  - The firmware is a raw image written straight into the GMU's TCMs, with no header.
  - An OOB request sets bit `req+16` and waits for bit `req+24`.
  - HFI is version 1, with 4 queues at the legacy indices.
- The a618 entry in gpulist: features 64BIT, RPMH, PREEMPTION, GPMU, CONTENT_PROTECTION, IFPC,
  IOCOHERENT. Firmware `a618_gmu.bin`, expected GMU firmware version 1.8.
- The DT quirk `qcom,gpu-quirk-hfi-use-reg` changes two things. The GX rail comes up through mailbox
  registers and an OOB before HFI starts. And the last HFI message is TEST.
- The cold-boot order (`a6xx_gmu_fw_start`):
  1. TCM retention, then the RSCC/PDC microcode (first boot only).
  2. The firmware into the TCMs, then the boot registers and the power config.
  3. CM3 out of reset; wait for 0xBABEFACE.
  4. GX rail on (OOB), SPTPRAC on, HFI init, the HFI messages.
  5. Later, OOB_GPU.
- Mainline does the same for a618, with two differences:
  - It does an RSCC "start" handshake before every firmware start, cold boots included. Downstream does
    it only on warm boots.
  - It sends a fixed one-level "off" bandwidth table for a618; its comment says the 618 GMU does no bus
    scaling.

**Firmware layout (checked in this session).** The 32 KiB image has two halves:
- The first 16 KiB is Cortex-M3 code. Its vector table gives SP 0x00043F80 and reset 0x9.
- The second 16 KiB is initial DTCM data: 204 non-zero bytes, all below +0x664.

In the host window, ITCM (dword 0x1B400, 16 KiB) is directly followed by DTCM (0x1C400, 16 KiB). One
32 KiB write from ITCM_START therefore fills both, and both kernels load it that way. §4's "fits the
32 KiB ITCM" was wrong and is corrected there. The first DTCM word is 0x10110000. With kgsl's version
macros that reads as 1.17, but whether it is a version word is a guess until FW_VER answers.

**Boot-args:**
- `sweetgpu=4` selects 2B.
  - Stage 1 runs unchanged: CX clocks, CX GDSC, GMU clock at 200 MHz.
  - None of the 2A writes happen: no apps-DRV `mx.lvl`/`gfx.lvl` vote, no host GX GDSC, no host gfx3d RCG.
- The new `sweetgmu=N` (0–6) stops after step 2B.N. One kernel can then go exactly as far as the owner
  approves, and each further step needs only new boot-args.

**New code (patch 0100):**
- `sweet_gmu.c`: RSCC/PDC, firmware load, CM3 start, OOB, HFI. About 700 lines.
- `sweet_smmu.c`: the GPU SMMU survey, one context bank and its page tables. About 300 lines. Stage 3
  reuses it for the GPU's own context.
- `sweet_gpu.c`: the `sweetgpu=4` flow and the GX checks, reusing 0099's frequency counter.
- `mkboot.sh`: with GPUFW=1, it also adds a 1 MiB zero blob `gmu-mem` through `mkpayload.py --blob`. The
  blob holds no vendor data.

#### 2B.0 Read-only survey (`sweetgmu=0`)

Nothing is written beyond stage 1. Every value goes to the log.
- **GPU SMMU** at 0x5040000 (needs the CX GDSC and `gcc_gpu_memnoc_gfx`, both on after stage 1):
  - global: IDR0/1/2/7, sCR0, sACR, sGFSR, every SMR and S2CR;
  - per bank (8 banks): CBAR/CBA2R, then SCTLR, TCR, TTBR0, MAIR0, FSR.
- **RSCC** (GMU block +0x23000): RSC_STATUS0, PDC_SLAVE_ID, HIDDEN_TCS_CMD0..2, the PDC start and match
  registers, SEQ_BUSY, SEQ_MEM_0..4; also GMU_RSCC_CONTROL_REQ/ACK.
- **PDC**: in the cfg block (0xb290000), TCS0–TCS3, ENABLE_PDC and SEQ_START_ADDR; in the seq block
  (0xb490000), words 0–4.
- **GMU**: CM3_SYSRESET, FW_INIT_RESULT, BOOT_CONFIG, HFI_CTRL_STATUS, HFI_VERSION_INFO,
  GMU2HOST_INTR_INFO/MASK, AO_HOST_INTERRUPT_STATUS, RPMH_POWER_STATE, SPTPRAC_PWR_CLK_STATUS, GBIF_HALT,
  and the first ITCM and DTCM words.
- **The `gmu-mem` blob**: its PA, and a check that it reads zero.

It answers these questions:
- Is the SMMU on (sCR0.CLIENTPD)?
- Which stream-match groups and banks are in use? kgsl's `qcom,gpu-offset = <0x48000>` says the GPU
  itself uses bank 0, and TrustZone may hold a bank for the secure stream 2.
- Did the bootloader leave RSCC/PDC programmed?
- Can EL1 read the PDC at all?

✅ Pass when every read returns without a fault and the tables are in the log.

**Result on G3a (2026-10-25, kernel 0100, `sweetgpu=4 sweetgmu=0`): PASSED.** All reads returned, no panic,
the phone stayed usable, and the log ran ~2900 lines past "stage 2B done at 2B.0". Observed:
- **SMMU:** IDR0 0x48016605, IDR1 0x20008005, IDR2 0x00007511, sCR0 0x00200626, sGFSR 0. So: **5** stream-match
  groups (not 8 banks as planned), 3 SID bits, **5** context banks of 4 KiB from +0x8000, OAS 36 bits, stage 1
  and 4K tables supported. The SMMU is **on** (CLIENTPD 0) and unmatched streams fault (USFCFG 1).
- **All 5 SMRs are free and every S2CR is "fault"; all 5 banks are zero.** Nothing (bootloader, hypervisor,
  TrustZone) left a GPU context, so 2B.1's attach will take bank 4 and SMR 0. CB0 stays free for Stage 3.
- **GMU:** CM3 in reset (SYSRESET 1), FW_INIT_RESULT 0, HFI all zero, CM3_CFG 0x52, RPMH_POWER_STATE 0xf,
  SPTPRAC 0xd4. The TCMs hold non-zero words (ITCM 2060a47b…, DTCM 98aef028…), so they are not cleared at reset.
- **RSCC:** PDC_SLAVE_ID 1, PDC_SEQ_START 0x4520, MATCH_LO/HI 0x4510/0x4514: already the values 2B.2 writes.
  HIDDEN_TCS_CMD0/1/2_ADDR read 0x82204514/0x82204510/0x82204520 with DATA 0 (2B.2 writes them to 0, as both
  kernels do; whether these are reset values is a guess). SEQ_MEM_0..4 and SEQ_BUSY are 0.
- **PDC: EL1 reads work** (risk 2 is gone). ENABLE_PDC 0, SEQ_START 0, seq words 0, TCS0–3 all zero: not
  programmed.
- **gmu-mem:** pa 0x1024f0000, 1 MiB, all zero; the layout needs 0x89000.

**Result on G3b (2026-10-25 15:06, `sweetgmu=3`): stopped at 2B.1, phone fine.** The 0100 code did:
- uncached mapping of `gmu-mem` and the page tables for all six regions: OK (6 tables, exactly as sized);
- bank 4 (CBA2R, CBAR, TCR2, TCR, MAIR0, TTBR0, SCTLR): every register read back as written;
- S2CR0 = 0x4 (translate, bank 4): read back as written;
- **SMR0 = 0x80010004 (valid, id 4, mask 1): read back 0x00000000.** Attach undone; nothing after 2B.1 ran
  (no RSCC, PDC, TCM or CM3 writes).
- In the undo, S2CR0 was written back to 0x000200ff (fault) but **still read 0x4**.

What this suggests (guesses until tested): a writable S2CR whose "fault" write is ignored is the Qualcomm
hypervisor behaviour mainline Linux works around in `arm-smmu-qcom.c` (on some firmware, S2CR writes of type
fault are ignored). So the stream-mapping registers are probably hypervisor-emulated here, and the dropped SMR
is the emulation refusing our value. The stock DT gives the GMU two single stream IDs (`gmu_user` SID 4,
`gmu_kernel` SID 5, `#iommu-cells = <1>`, so mask 0); kgsl never writes a masked SMR. Next try (patch 0101):
one SMR per SID with mask 0, plus a probe that writes an SMR without the valid bit and reads it back, to tell
"every SMR write is dropped" from "this value is refused". If SMRs cannot be written at all, the fallback is to
reuse whatever the hypervisor allows (to be investigated from the downstream arm-smmu driver).

**Result on G3d (2026-10-25 16:23, patch 0101, `sweetgmu=3`): the attach worked; a false fault check stopped 2B.**
- SMR0 = 0x80000004 and SMR1 = 0x80000005 (valid, mask 0) both **read back as written**; S2CR0/1 = 0x4 (bank 4).
  So masked SMRs are refused but single-ID ones are accepted (observed); the probe did not run.
- Bank 4 read back SCTLR 0x1027, TCR 0x802020, TCR2 0x38011, TTBR0 0x00040001024f0000; sCR0 unchanged.
- The fault check after attach saw sGFSR 0 and **bank 4 FSR 0x00000400**, and 2B stopped. That value is FSR.FORMAT
  (bits 10:9 = 0b10, AArch64 descriptor format), a read-only field, not a fault: `sweet_smmu_faults` returns the
  raw FSR instead of masking the fault bits (TF..UUT bits 1–8, SS bit 30, MULTI bit 31, mask 0xc00001fe). Bug in 0100.
- The stop path left the attach in place (SMRs valid, bank 4 enabled); GMU still in reset, so nothing uses it.
  No RSCC, PDC, TCM or CM3 writes happened. Phone booted to the UI as before.
- Next: mask the FSR fault bits in `sweet_smmu_faults` (patch 0102), then repeat with `sweetgmu=3`.

**Result on G3e (2026-10-25 16:41, patch 0102, `sweetgmu=3`): 2B.1 passed; 2B.2 stopped at the RSCC, phone fine.**
- 2B.1: same SMR/S2CR/bank values as G3d, FSR 0x400, no fault; attach accepted. GMU stays in reset.
- 2B.2: GENERAL_7 = 1 (TCM retention), RSC_STATUS0 = 0x01000000, PDC_SLAVE_ID = 1, all three HIDDEN_TCS_CMDn_DATA
  (CMD2_DATA now 0x80000000), OVERRIDE_START_ADDR, PDC_SEQ_START/MATCH_LO/MATCH_HI and SEQ_MEM 0..4 all read back
  as written.
- **HIDDEN_TCS_CMD0/1/2_ADDR: wrote 0, kept 0x82204514 / 0x82204510 / 0x82204520** (the values seen in the 2B.0
  survey). The stop rule ("every RSCC value must read back") then stopped 2B before any PDC write.
- Observation: the low halves of the preset addresses (0x4514, 0x4510, 0x4520) are the same values as
  PDC_MATCH_VALUE_HI/LO and PDC_SEQ_START_ADDR. Guess: these three ADDR registers are set by boot firmware and
  write-protected from EL1; mainline and downstream kgsl both write 0 to them and never read them back, so stock
  Android probably runs with the preset values too.
- Next (proposal): write them as kgsl does, but treat a kept firmware value as expected (log it, do not stop);
  every other RSCC value must still read back.

**Result on G3f (2026-10-25 17:09, patch 0103, `sweetgmu=3`): RSCC passed; 2B.2 stopped at the PDC, phone fine.**
- 2B.1 as on G3e. RSCC: the three HIDDEN_TCS_CMDn_ADDR logged "kept" (same preset values); everything else read back.
- PDC: sequencer words, TCS1/TCS3 enable (0x7), every CMD ADDR (0x30010 mx, 0x30000 cx, 0x30090 xo) and DATA
  (TCS1 1/0/0, TCS3 2/2/3) read back; ENABLE_PDC reads 0x80000001.
- **The six CMD MSGID writes (0x10108) read back 0x10100**: bits 16 (write) and 8 (response required) stuck, the low
  byte (8, the length field in the RPMh TCS msgid layout) reads 0. Before 2B.2 these registers read 0.
- The stop rule stopped 2B after the PDC block, so **the PDC was left enabled** with TCS1/TCS3 loaded; the GMU stays in
  reset, so nothing triggers it (guess). Phone booted to the UI as before.
- Guess: the PDC does not implement (or does not return) the msgid length bits; kgsl writes 0x10108 and never reads
  back. Next (proposal): for the six MSGID writes only, accept a readback that differs from the written value only in
  bits 7:0 (log it); every other PDC value must still read back.

**Result on G3g (2026-10-25 17:30, patch 0104, `sweetgmu=3`): 2B.1–2B.3 all done; the GMU firmware runs.**
- 2B.1 and the RSCC as on G3f. PDC: the six MSGID writes logged "length byte differs, accepted"; every other PDC value
  read back; "2B.2 done".
- 2B.3: 8192 firmware words into ITCM/DTCM, 0 differ on read-back. Boot registers read back except (non-fatal by design,
  "going on"): CM3_CFG wrote 0x4052 reads 0x52; SYS_BUS_CONFIG, ICACHE_CONFIG, DCACHE_CONFIG wrote 1 read 0. HFI_QTBL_ADDR
  0x60100000, AHB_FENCE_RANGE_0 0x80a008a0, PWR_COL_CP_MSG 0x60300000, RPMH_CTRL 0xf11 read back.
- **CM3 out of reset: FW_INIT_RESULT = 0xBABEFACE after 42 us.** FW_BUSY 0, GMU2HOST 0, AO_HOST_INT 0,
  RPMH_POWER_STATE 0xf, SPTPRAC 0xd4, HFI_STATUS 0. No SMMU fault before or after (sGFSR 0, bank 4 FSR only the
  0x400 format field). Dummy and log pages still zero (firmware had not written them when read).
- The GMU was left running (2B stops at `sweetgmu=3`). Phone booted to the UI as before; no visible change (expected:
  nothing on screen uses the GPU yet).
- Open questions (guesses): whether the four ignored boot registers matter (cache configs reading 0 may mean they
  are write-only or locked like the RSCC addresses). Next: 2B.4 (`sweetgmu=4`), the GX rail vote through the GMU.

**Result on G3h (2026-10-25 18:16, same 0104 kernel, `sweetgmu=4`): 2B.4 PASSED; the GX side is powered.**
- 2B.1–2B.3 as on G3g (0xBABEFACE after 43 us, same four non-fatal boot-register mismatches).
- Perf table from cmd-db (gfx.lvl 0x30050, mx.lvl 0x30010, cx.lvl 0x30000): all eight GPU votes and the GMU's
  two match the expected table exactly (perf 1 = 180 MHz, vote 0x00400202).
- Mailbox BOOT_SLUMBER_OPTION 0, GX_VOTE_IDX 2, MX_VOTE_IDX 2 read back.
- **OOB boot_slumber acked** (GMU2HOST_INTR_INFO 0x40000000). SPTPRAC status 0x28 (bit 3 on); it read 0x28 right
  before our SPTPRAC write too, so the GMU had already switched it on during boot_slumber (observation).
- **GX GDSC on:** gx_gdscr 0x00222001 → 0xf8222000, gx_domain 0x003c0000. No SMMU fault.
- Clocks (0099 counter, trusted: cx_gmu 199999 kHz): **gx_gfx3d runs at 9600 kHz**, cx_gfx3d 9600 kHz, gx_cxo 19200 kHz,
  gx_gmu 0. gfx3d RCG cmd/cfg 0x2/0x0 and GPU PLL0 mode 0x01000005 with L = 0: the gfx3d clock is still on its
  boot source (XO/2), not the GPU PLL. That answers 2A's open question: the GX branch needs the GX rail, which only
  the GMU vote provides.
- Pass rule met (OOB acked, SPTPRAC bit 3, GX GDSC on, gx_gfx3d counts). 180 MHz is expected after 2B.6's DCVS
  request, not here (guess). Phone booted to the UI as before; no visible change.
- Next: 2B.5 (`sweetgmu=5`), HFI init with the GMU (same kernel).

**Result on G3i (2026-10-25 18:29, same 0104 kernel, `sweetgmu=5`): HFI up, but the INIT ack was skipped.**
- 2B.1–2B.4 as on G3h (cx_gmu 200001 kHz, gx_gfx3d 9600 kHz).
- BW table addresses from cmd-db: MC0 0x50000, SH0 0x5003c, ACV 0x5000c, CN0 0x500a8 (as planned).
- **HFI start: HFI_CTRL_STATUS 0x1 (up), HFI_VERSION_INFO 0** (major 0, so HFI v1 as expected).
- INIT sent with header 0x00100500 (id 0, 5 dwords, type 0, seq 1). The GMU consumed it (command queue read 5 =
  write 5) and posted one message: **header 0x0002047e = id 126 (F2H ACK), 4 dwords, type 2**. Our matcher requires
  word 1 to equal our whole header, so it logged "skipped message 0x0002047e" and timed out ("NO ACK in 100 ms").
  Type and size passed, so word 1 differed from 0x00100500 (observation); the log does not show word 1 or the error
  word, so how it differs is unknown.
- Mainline `a6xx_hfi_wait_for_ack` matches an ack by id 126 (HFI_F2H_MSG_ACK) and the **sequence number** (bits 31:20)
  of word 1 only. Guess: this firmware rewrites another field of the returned header.
- Nothing after INIT was sent. GMU and GX left on. Phone booted to the UI as before.
- Next (proposal, patch 0105): match acks the mainline way (id 126 + seqnum of word 1), and log every word of each
  ack and each skipped message, so the returned header and error code are visible.
- Patch 0105 (owner approved): acks matched by id 126 + seqnum (bits 31:20 of word 1); every dword of an ack or a
  skipped message is logged, plus "ack returns header X (sent Y)" when word 1 differs. Commit 2703ca8,
  CI 40175446607 passed. Next image: G3j (0105 kernel,
  `sweetgmu=5`), booted only with the owner's OK.

**Result on G3j (2026-10-25 18:48, 0105 kernel, `sweetgmu=5`): 2B.5 PASSED, all five HFI messages acked with error 0.**
- Log flush #133, 5032 lines, no panic. 2B.1–2B.4 as on G3i.
- Every ack is 4 dwords: header id 126, then word 1 = **our header with the size byte (bits 15:8) cleared**
  (INIT 0x00100500 → 0x00100000, FW_VER 0x00200201 → 0x00200001, and so on), error 0, payload. The same length-byte
  clearing was seen in the PDC MSGID readback (0104). The ack's own sequence number counts up by 1 per ack.

| Message | Sent header | Ack word 1 | Error | Payload |
|---|---|---|---|---|
| INIT | 0x00100500 | 0x00100000 | 0 | 0x0e550e55 |
| FW_VER | 0x00200201 | 0x00200001 | 0 | **0x10110000 = firmware 1.17** |
| PERF_TBL (8 GPU, 2 GMU levels) | 0x00302b04 | 0x00300004 | 0 | 0x0e550e55 |
| BW_TBL | 0x0040a003 | 0x00400003 | 0 | 0x0e550e55 |
| TEST | 0x00500205 | 0x00500005 | 0 | 0x0e550e55 |

- 0x0e550e55 looks like a filler for "no payload" (guess).
- After HFI: FW_INIT_RESULT 0xbabeface, FW_BUSY 1, RPMH_POWER_STATE 0xf, SPTPRAC 0x28, HFI_STATUS 1.
  SMMU: no global fault, bank 4 FSR 0x400 (FORMAT only, no fault).
- Next: 2B.6 (`sweetgmu=6`, same 0105 kernel): OOB_GPU, the DCVS vote through registers, and the first GX register
  reads (RBBM_STATUS, the CP always-on counter). This is the first GX register access. Only with the owner's OK.

**Result on G3k (2026-10-25 19:07, 0105 kernel, `sweetgmu=6`): 2B.6 PASSED. Stage 2 (GX power) is complete.**
- Log flush #389, 6158 lines, no panic. 2B.1–2B.5 as on G3j. Owner: phone "same as before".
- OOB_GPU acked (GMU2HOST bit 24). DCVS perf index 1: the three mailbox writes read back, OOB_DCVS acked (bit 31),
  DCVS_RETURN 0.
- After DCVS the GMU has set up the GPU PLL (mode 0xd1000005, L 0x12) and moved gfx3d onto it. Measured:
  **gx_gfx3d 180001 kHz** and cx_gfx3d 180001 kHz (the 180 MHz perf level), cx_gmu 200002 kHz, gx_cxo 19201 kHz.
  gx_gmu reads 0 now (it read 200 MHz on G2m; not understood, noted only).
- GX GDSC 0xf8222000 (on), SPTPRAC 0x28. RPMH_POWER_STATE reads 0 after DCVS (0xf after HFI); meaning unknown.
- **First GX reads work: RBBM_STATUS 0x00000001** (only bit 0, CP_AHB_BUSY_CX_MASTER, which is probably our own
  AHB read; the GPU is otherwise idle), RBBM_INT_0_STATUS 0, GBIF_HALT 0.
- **CP always-on counter runs:** +21938 over the "1 ms" wait, against +21931 on the GMU counter in the same wait,
  so the two counters agree (the wait itself was about 1.14 ms at 19.2 MHz).
- One minute of monitoring at 10 s steps: FW_INIT_RESULT stays 0xbabeface, GX GDSC stays on, no SMMU fault
  (bank 4 FSR 0x400 = FORMAT only).
- Stage 2 pass rule met: the always-on counter advances, RBBM_STATUS reads, no SMMU fault, phone stable.
- Next: Stage 3 (the GPU's own SMMU context bank, stream 0). It needs a new patch, written only with the owner's OK.

**Stage 3 built (patch 0106, owner approved, commit 721eab3, CI 40178628271 passed).** `sweetgpu=5` with `sweetgmu=6`
runs all of 2B, then:
- One level-1 table (4 KiB) in GPU-GMU-MEM at +0x89000, just past the GMU's part, so mkboot is unchanged.
- 1 GiB blocks map 0x80000000..0x1_ffff_ffff 1:1 (attr 0 normal non-cacheable, read/write, AF, nG).
- SMMU bank 0 (kgsl's GPU bank) for stream 0, mask 0, through `sweet_smmu_attach` with a 36-bit input range
  (T0SZ 28; IDR2 says IAS 36). The GMU keeps its 32-bit bank 4. Every write is read back as in 2B.1.
- Then the bank's fault registers, RBBM_STATUS3 bit 24 (stalled on an SMMU fault), and both banks every 10 s for 30 s.
- Differences from the plan above: no "all streams" SMR (stream 0 only, as planned), no separate global register dump
  (2B.0's survey already logs them), and the 8–16 GB GPU VA window is left for stage 4.
- Pass: every register reads back, no fault on bank 0 or bank 4, RBBM_STATUS3 not stalled, phone stable. ATOS is off
  and the GPU sends no traffic yet, so the first real walk is stage 4's first CP fetch.
- Test image: G3l = 0106 kernel, `sweetgpu=5 sweetgmu=6`, only with the owner's OK.

**Result on G3l (2026-10-25 19:38): Stage 3 PASSED.**
- Log flush #135, 5158 lines, no panic. Survey: IAS 36, OAS 36 bits. 2B done at 2B.6 as on G3k.
- Table at 0x102579000 (GPU-GMU-MEM +0x89000); 0x80000000..0x1ffffffff mapped 1:1 in 1 GiB blocks.
- Stream 0 → SMR2 0x80000000 (stuck), S2CR 0x00000000 (translate, bank 0). Bank 0 read back:
  SCTLR 0x00001027, TCR 0x0080201c (T0SZ 28), TCR2 0x00038011, TTBR0 0x0000000102579000. sCR0 unchanged 0x00200626.
  So the hypervisor lets EL1 program bank 0 and a stream-0 SMR, as it did for bank 4 (observation).
- Bank 0 FSR 0x400 (FORMAT only), sGFSR 0; RBBM_STATUS 0x1, RBBM_STATUS3 0 (not stalled).
- 30 s monitor: bank 0 and bank 4 clean, GMU alive (0xbabeface).
- Not tested, as planned: an actual walk (ATOS off, no GPU traffic). Stage 4's first CP fetch is that test.
- Next: Stage 4 (CP first light: SQE load, hardware init list, ring, CP_ME_INIT). New patch only with the owner's OK.

#### 2B.1 GMU memory and an SMMU context for streams 4–5 (`sweetgmu=1`)

The GMU reaches memory only through the GPU SMMU, as stream 5 (`gmu_kernel`) and stream 4 (`gmu_user`).
kgsl maps all GMU memory through stream 5. The DT's kgsl SMMU node has no `qcom,skip-init`, so the stock
Android kernel programs this SMMU itself, from EL1 (an inference from the DT).

**Memory.** The 1 MiB `gmu-mem` blob, page-aligned in kernel RAM like the firmware blobs.
- It is cleaned and invalidated once through the kernel's cacheable mapping.
- From then on it is used only through an uncached `ml_io_map_wcomb` mapping, with 32-bit stores.
- Cache maintenance alone would not be safe: the GMU writes HFI indices into the same cache lines as ours.

GMU addresses, from downstream's `gmu_vma` table and allocation order:

| GMU VA | Size | Use |
|---|---|---|
| 0x00000000 | 16 KiB | ITCM, inside the GMU, not mapped |
| 0x00004000 | 240 KiB | ICACHE backing |
| 0x00040000 | 16 KiB | DTCM, inside the GMU, not mapped |
| 0x00044000 | 240 KiB | DCACHE backing |
| 0x60000000 | 4 KiB | dummy page; downstream allocates it first, on purpose |
| 0x60100000 | 20 KiB | HFI queue table and 4 queues |
| 0x60200000 | 16 KiB | dump buffer (INIT's `dbg_buffer`) |
| 0x60300000 | 4 KiB | log (`PWR_COL_CP_MSG`) |

Each uncached region starts on its own 1 MiB, as downstream allocates them. With 24 KiB of page tables this
uses about 550 KiB of the blob.

**SMMU programming**, in this order. Register names are from the SMMUv2 spec. Context banks start at
+0x8000, 4 KiB each; kgsl's `gpu-offset` (bank 0 at 0x5048000) confirms that layout.
1. Pick a free bank (not bank 0) and a free stream-match group, from 2B.0.
2. Set up the bank:
   - SCTLR = 0 first.
   - CBA2R: AArch64 format. CBAR: stage 1 with stage 2 bypass.
   - TCR: 4 KiB granule, 32-bit input range (T0SZ 32, walks start at level 1, the 3-level tables the DT
     asks for), non-cacheable walks, TTBR1 walks off. Output size from IDR2.
   - MAIR0 attribute 0 = Normal non-cacheable. TTBR0 = the level-1 table.
   - TLBIALL and TLBSYNC.
   - Then SCTLR: translation on, fault reporting on, faults terminate.
3. Stream match: SMR = valid, ID 4, mask 1, so it matches 4 and 5. S2CR = translate to that bank.
4. Only if 2B.0 found the SMMU off: clear CLIENTPD. Unmatched streams keep bypassing, as now.
5. Global TLB invalidate and sync.
6. Read back every register written. Any mismatch stops 2B and undoes step 3.

**Page tables.**
- 1 level-1 table and 2 level-2 tables (0–1 GiB, 1–2 GiB).
- 3 level-3 tables: 0x0–0x1FFFFF, 0x60000000–0x601FFFFF, 0x60200000–0x603FFFFF.
- Leaf entries: access flag set, read/write at any privilege, attribute 0.

**Not written:** `attach-impl-defs`, the 12 Qualcomm tuning registers in the DT. Mainline does not write
them either; they are the fallback if the GMU later shows SMMU errors.

ATOS is disabled on this SMMU, so the CPU cannot test a translation. The real test is in 2B.3: the bank's
FSR must stay 0 while the firmware starts.

✅ Pass when every write reads back, and sGFSR and the bank's FSR are 0.

#### 2B.2 RSCC and PDC microcode (`sweetgmu=2`)

This is downstream's `_load_gmu_rpmh_ucode`, run once per boot, after GMU_GENERAL_7 (0x1F9CC) = 1 (TCM
retention), which downstream writes first. The RSCC sequences and the PDC TCS commands run only when the
GMU collapses or wakes the GPU power domain. We keep the GPU active, so they should not run during the
test. They are written anyway, because the firmware expects them.

RSCC (dword offsets from 0x5000000; the block starts at 0x23400):

| Register | Offset | Value |
|---|---|---|
| RSC_STATUS0_DRV0 | 0x23404 | 0x01000000 (bit 24: SDE clock gating off) |
| PDC_SLAVE_ID_DRV0 | 0x2340B | 1 |
| HIDDEN_TCS_CMD0 ADDR / DATA | 0x2340D / 0x2340E | 0 / 0 |
| HIDDEN_TCS_CMD1 ADDR / DATA | 0x2340F / 0x23410 | 0 / 0 |
| HIDDEN_TCS_CMD2 ADDR / DATA | 0x23411 / 0x23412 | 0 / 0x80000000 |
| OVERRIDE_START_ADDR | 0x23500 | 0 |
| PDC_SEQ_START_ADDR | 0x23408 | 0x4520 |
| PDC_MATCH_VALUE_LO / HI | 0x23409 / 0x2340A | 0x4510 / 0x4514 |
| SEQ_MEM_0..4_DRV0 | 0x23580.. | 0xA7A506A0 0xA1E6A6E7 0xA2E081E1 0xE9A982E2 0x0020E8A8 |

PDC seq block (0xb490000), words 0–4: 0xFEBEA1E1 0xA5A4A3A2 0x8382A6E0 0xBCE3E284 0x002081FC.

PDC cfg block (0xb290000, dword offsets): two TCS banks, each with 3 ARC commands (msgid 0x10108).
- The command addresses come from cmd-db at run time. On this phone they are mx.lvl 0x30010, cx.lvl
  0x30000 and xo.lvl 0x30090; mainline also uses 0x30090 for a618.
- The data are hlvl indices into our cmd-db tables (§4).

| Bank | CONTROL / ENABLE / WAIT | mx.lvl | cx.lvl | xo.lvl |
|---|---|---|---|---|
| TCS1 (0x1572) | 0 / 7 / 0 | 1 (vlvl 16) | 0 (off) | 0 (off) |
| TCS3 (0x15D6) | 0 / 7 / 0 | 2 (vlvl 192) | 2 (vlvl 64; the a618 value) | 3 (vlvl 128) |

Commands start at bank + 3 and are 4 dwords apart. Then PDC_GPU_SEQ_START_ADDR (0x1148) = 0 and
PDC_GPU_ENABLE_PDC (0x1140) = 0x80000001. Every write is read back.

These are votes from the GPU's own RSC. The AOP combines them with the other masters' votes, so a wrong
GPU vote cannot pull MX or CX below what the rest of the SoC asks for.

✅ Pass when every value reads back and the phone keeps running.

#### 2B.3 Firmware in, CM3 start (`sweetgmu=3`)

1. Fill the HFI queue table (layout in 2B.5). The boot registers point at it, so it must be ready before
   the CM3 starts.
2. Write the 8192 words of `a618_gmu.bin` from CM3_ITCM_START (0x1B400). Read them all back and compare
   (FNV-1a of the read-back against the blob's).
3. Boot registers:

   | Register | Offset | Value |
   |---|---|---|
   | CM3_FW_INIT_RESULT | 0x1F81C | 0 |
   | CM3_BOOT_CONFIG | 0x1F801 | 2 (TCM_BOOT) |
   | HFI_QTBL_ADDR | 0x1F985 | 0x60100000 |
   | HFI_QTBL_INFO | 0x1F984 | 1 |
   | AHB_FENCE_RANGE_0 | 0x23B11 | 0x80A008A0 (downstream's mask) |
   | CM3_CFG | 0x1F82D | 0x4052 |
   | HFI_SFR_ADDR | 0x1F982 | 0x06018000 (chip id 0x06010800, repacked) |
   | PWR_COL_CP_MSG | 0x1F900 | 0x60300000 (the log page; size field 0 = 1 page) |
   | SYS_BUS_CONFIG, ICACHE_CONFIG, DCACHE_CONFIG | 0x1F40F, 0x1F400, 0x1F401 | 1 each |
   | PWR_COL_INTER_FRAME_CTRL | 0x1F8C0 | 0x09C40400 |
   | RPMH_CTRL | 0x1F8E8 | OR 0xF11 (interface, LLC, DDR, MX, CX and GFX votes on) |

   Idle level "active": no IFPC, SPTP or NAP bits. The DT has `qcom,no-nap`, and the a618 has no SPTP_PC
   feature. IFPC stays off for bring-up, so the GPU never collapses behind our back.
4. CX_GMU_WFI_CONFIG (0x1F802) = 0. GBIF_HALT only if 2B.0 found it non-zero.
5. CM3_SYSRESET (0x1F800) = 0. Poll CM3_FW_INIT_RESULT for 0xBABEFACE, for up to 100 ms.
6. Log FW_INIT_RESULT, CM3_FW_BUSY, GMU2HOST_INTR_INFO, AO_HOST_INTERRUPT_STATUS, RPMH_POWER_STATE, the
   bank's FSR/FAR/FSYNR0, sGFSR, and the first words of the dummy and log pages.

On a timeout: CM3_SYSRESET = 1, log everything, stop.
- If the bank shows a fault, its FAR names the GMU address that was missing.
- If there is no fault and no 0xBABEFACE, the next try adds mainline's RSCC start handshake before step 5:
  RSCC_CONTROL_REQ bit 1, wait for the ACK bit, wait for SEQ_BUSY 0, then REQ = 0.

✅ Pass when 0xBABEFACE arrives within 100 ms and the bank's FSR is 0.

#### 2B.4 GX rail through the GMU (`sweetgmu=4`)

The perf table is computed at run time from the DT power levels, downstream's gfx3d voltage table and
cmd-db, as `rpmh_arc_votes_init` does. The vote word is vlvl << 16 | mx index << 8 | gfx index.
Expected values:

| Index | GPU | Corner (vlvl) | gfx.lvl index (vlvl) | mx.lvl index (vlvl) | Vote |
|---|---|---|---|---|---|
| 0 | off | — | 0 | 0 | 0x00000000 |
| 1 | 180 MHz | MIN_SVS 48 | 2 (64) | 2 (192) | 0x00400202 |
| 2 | 267 MHz | LOW_SVS 64 | 2 (64) | 2 (192) | 0x00400202 |
| 3 | 355 MHz | SVS 128 | 3 (128) | 2 (192) | 0x00800203 |
| 4 | 430 MHz | SVS_L1 192 | 4 (192) | 2 (192) | 0x00C00204 |
| 5 | 565 MHz | NOM 256 | 5 (256) | 3 (320) | 0x01000305 |
| 6 | 650 MHz | NOM_L1 320 | 6 (320) | 3 (320) | 0x01400306 |
| 7 | 700 MHz | TURBO 384 | 7 (384) | 4 (384) | 0x01800407 |

- This is the bin-146 list reversed, with the DT's 0 Hz level 7 as index 0, the way kgsl builds it.
- The GMU's own table: index 0 off, index 1 200 MHz. 200 MHz needs MIN_SVS, which gives cx.lvl index 2
  (64) and mx index 2: vote 0x00400202.

Then downstream's `a6xx_gmu_gfx_rail_on`, for the initial level 6 (180 MHz, perf index 1):
1. Mailbox words in DTCM: BOOT_SLUMBER_OPTION (0x1CBF8) = 0, GX_VOTE_IDX (0x1CBF9) = 2,
   MX_VOTE_IDX (0x1CBFA) = 2.
2. OOB boot_slumber: HOST2GMU_INTR_SET (0x1F994) = bit 22. Poll GMU2HOST_INTR_INFO (0x1F992) for bit 30,
   up to 100 ms. Then GMU2HOST_INTR_CLR (0x1F991) = bit 30.
3. SPTPRAC: GX_SPTPRAC_POWER_CONTROL (0x1A881) = 0x00778000. Poll SPTPRAC_PWR_CLK_STATUS (0x1F8D0) for
   bit 3, up to 10 ms.
4. Reads only: the GX GDSC, GX_DOMAIN_MISC, the gfx3d RCG CMD/CFG, SPTPRAC status. Then measure
   gx_gfx3d with 0099's counter. This answers the question 2A could not.

✅ Pass when the OOB is acked, SPTPRAC bit 3 is set, the GX GDSC is on and gx_gfx3d counts. Any non-zero
rate passes; 180 MHz is the expected one.

#### 2B.5 HFI (`sweetgmu=5`)

Queue table at GMU 0x60100000 (downstream's `hfi_init`):
- Header: version 0, size 54 dwords, first queue header at dword 6, 12 dwords per header, 4 queues, all 4
  active.
- Queues of 4 KiB (1024 dwords) from +0xE0, the table size rounded up to 16 bytes:

  | Header | Queue | GMU address | Index / priority (type word) | Status |
  |---|---|---|---|---|
  | 0 | command | 0x601000E0 | 0 / 10 (0x0A00) | on |
  | 1 | message | 0x601010E0 | 4 / 10 (0x0A04) | on |
  | 2 | debug | 0x601020E0 | 5 / 40 (0x2805) | on |
  | 3 | DSP | 0x601030E0 | 1 / 20 (0x1401) | off |

- The legacy indices 4, 5 and 1 apply to the a615 family. In every header: rx_wm 1, tx_wm 1, rx_req 1,
  the rest 0.

**Start.** HFI_CTRL_INIT (0x1F986) = 1. Poll HFI_CTRL_STATUS (0x1F980) bit 0, up to 100 ms. Log
HFI_VERSION_INFO (0x1F981); its major (bits 31:28) should be below 2, which kgsl treats as HFI v1.

**Sending a message:**
1. Write it at the command queue's write index, wrapping at 1024. Update write_index, then a barrier.
2. HOST2GMU_INTR_SET = 1.
3. Poll GMU2HOST_INTR_INFO bit 0 (message queue), up to 100 ms. Both kernels poll this raw status
   register, so the interrupt mask stays as found.
4. Clear that bit, then read the message queue.

The header word is id | dwords << 8 | type << 16 | sequence << 20. An ack has type 2. Its word 1 is our
header (the sequence must match), and word 2 is the error code (0 is OK).

**The messages:**
1. INIT (id 0, 5 dwords): seg_id 0, dbg_buffer 0x60200000, size 0x4000, boot_state 1 (cold).
2. FW_VER (id 1, 2 dwords): we support 1.8 (0x10080000). Word 3 of the reply is the firmware version.
   Log it; the major must be 1 and the minor at least 8.
3. PERF_TBL v1 (id 4, 43 dwords): 8 GPU levels and 2 GMU levels, the votes above, frequencies in kHz.
4. BW_TBL (id 3, 160 dwords with the fixed array sizes): one "off" level, the shape mainline sends for
   a618.
   - DDR: 3 commands, to MC0 0x50000, SH0 0x5003c and ACV 0x5000c. Data 0x40000000 each, wait mask 1.
   - CNOC: 1 command, to CN0. Data 0x40000000 (level 0) and 0x60000001 (level 1), wait mask 1.
   - All addresses come from cmd-db by name. **Mainline's CNOC address 0x5007c is SN15 on this SoC; CN0
     here is 0x500a8.** Values copied from sc7180 would be wrong.
5. TEST (id 5, 2 dwords). It also tells the GMU that no more HFI messages follow.

Because the a618 GMU does no bus scaling, the GPU's DDR bandwidth still rides on the CPU's and the
display's DDR votes. If bandwidth becomes the limit, an apps-side BCM vote is Stage 9 work.

✅ Pass when HFI_CTRL_STATUS bit 0 sets and all five messages are acked with error 0.

#### 2B.6 OOB_GPU and the first GX reads (`sweetgmu=6`, the whole of 2B)

1. OOB_GPU: HOST2GMU_INTR_SET = bit 16. Poll for bit 24, then clear it.
2. Frequency through the registers (downstream's `a6xx_gmu_dcvs_nohfi`):
   - DCVS_ACK_OPTION (0x1CBFC) = 0.
   - DCVS_PERF_SETTING (0x1CBFD) = 0x30000001: perf index 1, option "at least".
   - DCVS_BW_SETTING (0x1CBFE) = 0x000FF0FF: no change.
   - OOB_DCVS: set bit 23, wait for bit 31. Read DCVS_RETURN (0x1CBFF), then clear bit 31.
3. Measure gx_gfx3d again.
4. The first GX reads, only if the GX GDSC is on and gx_gfx3d runs: RBBM_STATUS (0x210),
   CP_ALWAYS_ON_COUNTER_LO/HI (0x980/0x981) twice, 1 ms apart, and RBBM_INT_0_STATUS (0x201).
5. Leave the GMU running with OOB_GPU still set, so GX stays up. For one minute, log every 10 s:
   FW_INIT_RESULT, AO_HOST_INTERRUPT_STATUS, RPMH_POWER_STATE, the GX GDSC and the bank's FSR.

✅ Pass (Stage 2 as a whole) when the always-on counter advances, RBBM_STATUS reads, no SMMU fault is
latched, and the phone stays stable.

#### Test boots

One kernel (0100) and three images that differ only in `sweetgmu`. Each boot needs the owner's OK, as
before. Image names are checked against `shim/out` first (G2c was already taken).

| Image | `sweetgmu` | Writes | Purpose |
|---|---|---|---|
| G3a | 0 | none beyond stage 1 | survey: SMMU, RSCC, PDC, GMU state |
| G3b | 3 | SMMU, RSCC/PDC, the TCMs, CM3 start | the firmware boots |
| G3c | 6 | also the OOBs, HFI and the GX reads | GX powered by the GMU |

G3b and G3c may need fixes in between. Those are new patches and CI runs, shown before anything boots.

#### Risks for 2B, most uncertain first

1. **The hypervisor and the SMMU global registers.** Writes might be trapped or dropped. The stock kernel
   programs this SMMU from EL1 on the same firmware, so it is probably allowed. The read-back in 2B.1
   shows it.
2. **PDC access from EL1.** The PDC is an always-on block. Downstream maps it from EL1, so reads probably
   work. If not, the 2B.0 read faults and the test boot panics; the log survives.
3. **The CM3 does not boot.** The likely causes, in order: a missing mapping (the bank's FSR/FAR names
   it), the RSCC start handshake (mainline's order), a firmware version mismatch.
4. **The boot_slumber ack never comes.** Either the GMU cannot vote gfx through its RSC (RSCC/PDC wrong),
   or the vote index is wrong. RPMH_POWER_STATE and the AO interrupt status tell more.
5. **HFI messages are rejected.** Most likely the bandwidth table. We copy mainline's a618 shape with this
   SoC's cmd-db addresses.
6. **A GMU left running.** It reaches only the memory we mapped, and its votes go through RPMh. The worst
   case is extra power use or a reboot. The everyday H4 boot never sets `sweetgpu`.

2B leaves these for later: the GPU's own SMMU context (stream 0, Stage 3), the SQE (Stage 4), interrupts
(everything polls), and power collapse (IFPC and slumber, Stage 9).

### Stage 3: GPU SMMU

- The SMMU code, the register survey and the first context bank (the GMU's, streams 4–5) come earlier,
  in 2B.0–2B.1 (`sweet_smmu.c`). Stage 3 adds the GPU's own bank.
- Same idea as their 0121:
  - CB0 stage-1 AArch64, 4K granule, T0SZ 28.
  - 1 GB identity blocks for 0x80000000–0x1_ffff_ffff.
  - MAIR normal non-cacheable.
  - An SMR for stream 0 only, S2CR → CB0. **Not "all streams"** as in their 0121: streams 4–5 already
    match the GMU's bank, and two matching SMRs would be a stream-match conflict.
  - Then TLBIALL, sync and read back.
- Read IDR0/IDR1 first, and log every global register before and after.
- Sunfish-specific risk: **the hypervisor may trap or refuse SMR/S2CR writes**, or stage 2 may block
  parts of DRAM. The readback is the test. If it fails, use the context bank and SMR layout the bootloader
  or hypervisor already set up for SID 0 (dump it first), and only write CB0's TTBR/TCR/SCTLR.
- Fault IRQs are not wired up. Poll the fault status register (FSR) and log it in every probe step.
- Add the 8–16 GB GPU VA window (L1[8..15] → L2 → L3, 4 KB pages, one guard page after each mapping), as
  in their 0137/0150.

✅ Pass when the readback matches and no fault is latched.

### Stage 4: CP first light (passed on G4c, patches 0107–0108)

Planned 2026-10-25, after G3l passed Stage 3. The owner asked to see this plan before any code.

**Idea.** Start the command processor (CP) the way mainline's a6xx `hw_init` does, minus everything that
is not needed to prove the CP runs. The SQE microcode goes into memory the GPU reaches through Stage 3's
bank, then come the hardware init list, a ring buffer, `CP_ME_INIT`, and packets whose only effect is a
CP register the CPU can read back. Stage 4 makes **no GPU writes to DRAM** by design: the ring has
`NO_UPDATE` set and no packet writes memory. Memory writes are Stage 5's test, because the GPU is still
in secure mode (their 0130 finding).

**Sources, read in this session:**
- Mainline Linux 6.18.54 (the WSL kernel tree): `drm/msm/adreno/a6xx_gpu.c` (`hw_init`,
  `a6xx_cp_init`, `a6xx_set_ubwc_config`, `a6xx_ucode_check_version`, `a6xx_idle`), `a6xx_catalog.c`
  (the `qcom,sm7150` a618 entry), `adreno_gpu.h` (PKT4/PKT7), `soc/qcom/ubwc_config.c` (`sm7150_data`).
- Our generated headers `xnu/gpu/regs/a6xx.xml.h`, `adreno_common.xml.h`, `adreno_pm4.xml.h` for the
  offsets.
- The downstream sunfish kgsl is **not** in `src/sunfish-kernel` (no `drivers/gpu/msm`), so mainline is
  the only reference for the init list. Values are hardware facts from GPL code. We write our own code (§5).
- The firmware file itself: `a630_sqe.fw` is 32136 B; dwords 0–3 are `0x00000000 0x016ee19a 0x01001ee1
  0x01190001`.

**What mainline does for this GPU** (the `qcom,sm7150` entry, chip id 0x06010800):
- SQE `a630_sqe.fw`, GMEM 512 KiB, `hwcg = a615_hwcg`, `protect = a630_protect`,
  `prim_fifo_threshold = 0x00180000`. Its zap is `a615_zap`, which matches our blobs.
- `a6xx_has_gbif()` is true (everything except a630), so the GBIF halts are cleared. The QoS write still
  takes the VBIF branch, because a618 is not in the a610/a640/a650 family list.
- UBWC from `sm7150_data`: UBWC 2.0, level 2 and 3 swizzle on, highest bank bit 14. So hbb = 1
  (hbb_lo 1, hbb_hi 0), `ubwc_mode` 0 (it is set only for UBWC 1.0), amsbc 0, and macrotile 0.
- The SQE check: the low 12 bits of the first word after dword 0 must be ≥ 0x190. Ours is 0x19a, so
  mainline would accept it and would use `WHERE_AM_I`. We use neither the RPTR shadow nor `WHERE_AM_I`.
- Order: OOB_GPU set → GBIF un-halt → SECVID TSB → 64-bit address modes → HWCG → QoS → UCHE → ROQ / mem
  pool / prim fifo → AHB, perf counters → UBWC → hang detect → UCHE_CLIENT_PF → GMU busy counters →
  CP_PROTECT → INT_0_MASK → SQE base → ring base/cntl → `CP_SQE_CNTL = 1` → `CP_ME_INIT` + idle → zap,
  then `CP_SET_SECURE_MODE 0` → clear OOB_GPU and OOB_BOOT_SLUMBER.

**What we already have from 2B/3:**
- OOB_GPU and OOB_BOOT_SLUMBER are set and never cleared, so the GMU keeps GX on. Stage 4 keeps it that way.
- Bank 0 (SID 0): 1 GiB identity blocks over 0x80000000–0x1_ffff_ffff, MAIR attribute 0 = Normal
  non-cacheable. The CPU maps GPU-GMU-MEM write-combined, so a `dsb sy` before each doorbell is enough and
  no cache maintenance is needed. The bank's SCTLR has CFRE but not CFCFG or HUPCF: a fault terminates
  the access instead of stalling it, and the FSR latches it.
- The `GPU-SQE` blob has been in the image since 0094.

**Boot-args:**
- `sweetgpu=6` selects Stage 4. It needs `sweetgmu=6`, and Stage 3 must pass in the same boot.
  `sweetgpu=5` stays exactly as on G3l.
- The new `sweetcp=N` (0–6, default 0) stops after step 4.N, like `sweetgmu`.

**New code (patch 0107):**
- `sweet_cp.c` (new, about 400 lines): steps 4.0–4.6, the PM4 header helpers (mainline's parity formula),
  the "CP dump" below.
- `sweet_gmu.c`: `sweet_gmu_spare()` becomes a page-aligned bump allocator. The first call still returns
  +0x89000, so Stage 3's table does not move.
- `sweet_gpu.c`: the `sweetgpu=6` hook after `gpu_stage3()`, which must return success. The "stages above
  N not built" message moves to `gpu_level > 6`.
- No `mkboot.sh` change: everything fits in the existing 1 MiB GPU-GMU-MEM blob.

**Memory layout** (GPU-GMU-MEM offsets; PAs as on G3l, where the blob started at 0x1024f0000; IOVA = PA
through the identity map):

| Offset | Size | PA on G3l | Use |
|---|---|---|---|
| +0x89000 | 4 KiB | 0x102579000 | Stage 3 level-1 table (unchanged) |
| +0x8a000 | 32 KiB | 0x10257a000 | SQE: the file without dword 0 (32132 B), rest zero |
| +0x92000 | 16 KiB | 0x102582000 | ring 0 (4096 dwords) |
| +0x96000 | 4 KiB | 0x102586000 | scratch page, filled with 0x5ee7dead; a tripwire in Stage 4, the marker target in Stage 5 |
| +0x97000 | | | end (the blob is 0x100000) |

**The "CP dump"** (logged after every step from 4.2 on, and on any stop):
RBBM_STATUS 0x210, STATUS1 0x211, STATUS2 0x212, STATUS3 0x213, RBBM_INT_0_STATUS 0x201,
CP_RB_RPTR 0x806, CP_RB_WPTR 0x807, CP_SQE_CNTL 0x808, CP_HW_FAULT 0x821, CP_INTERRUPT_STATUS 0x823,
CP_PROTECT_STATUS 0x824, CP_STATUS_1 0x825, CP_IB1_BASE 0x928/0x929, CP_IB1_REM_SIZE 0x92a,
CP_ROQ_AVAIL_RB 0x948, CP_SCRATCH_REG(0–7) 0x883–0x88a, CP_ALWAYS_ON_COUNTER 0x980/0x981, bank 0
FSR/FAR/FSYNR0, the GX GDSC, and the GMU's AO_HOST_INTERRUPT_STATUS. When a step from 4.3 on fails, the dump
is followed by `CP_SQE_CNTL = 0` (halt the SQE) and the stage stops. Nothing else is undone.

Every poll is bounded (1 s unless stated) and spins with 10 µs delays. Each register write is
announced with `gpu_pause()` as in 2B, then read back and logged with its expected value.

#### 4.0 Survey and memory (`sweetcp=0`)

No GPU register is written. CPU writes go only to our own blob.
1. Read and log the reset values of every register 4.1 and 4.2 will write. Also log
   RBBM_SECVID_TRUST_CNTL 0xf400 (read only, **never written**: the `secvid-set-once` quirk),
   RBBM_CLOCK_CNTL 0xae, CP_PROTECT_CNTL 0x84f, CP_PROTECT_REG(0–31) 0x850–0x86f, RBBM_INT_0_MASK 0x38,
   GBIF_HALT_ACK 0x3c46, RBBM_GBIF_HALT_ACK 0x17, and the CP dump.
2. Check the SQE blob: length 32136, dword 0 = 0, and (dword 1 & 0xfff) ≥ 0x190. A failed version check is
   logged but does not stop the stage, since nothing depends on `WHERE_AM_I` yet.
3. Allocate the three areas above. Copy the SQE without dword 0 and read every word back. Zero the ring,
   poison the scratch page, then `dsb sy`.

✅ Pass when the reads complete, the copy compares equal, and no SMMU fault is latched.

#### 4.1 Hardware init list (`sweetcp=1`)

Mainline's `hw_init` values for this GPU, in mainline's order. "Skip" rows are left at reset and logged
only.

| # | Register | Offset | Value | Note |
|---|---|---|---|---|
| 1 | GBIF_HALT, RBBM_GBIF_HALT | 0x3c45, 0x16 | 0, 0 | then read each back; log both ACKs |
| 2 | RBBM_SECVID_TSB_CNTL | 0xf803 | 0 | |
| 3 | RBBM_SECVID_TSB_TRUSTED_BASE, _SIZE | 0xf800/1, 0xf802 | 0, 0 | no trusted range |
| 4 | *_ADDR_MODE_CNTL × 12 | CP 0x842, VSC 0xc01, GRAS 0x8601, RB 0x8e05, PC 0x9e01, HLSQ 0xbe05, VFD 0xa601, VPC 0x9601, UCHE 0xe00, SP 0xae01, TPL1 0xb601, RBBM_SECVID_TSB 0xf810 | 1 each | 64-bit addressing |
| – | HWCG (`a615_hwcg`, ~60 writes plus GMU CGC) | | **skip** | clock gating is power, not function; Stage 9 |
| 5 | RBBM_VBIF_CLIENT_QOS_CNTL | 0x10 | 0x3 | |
| 6 | RBBM_PERFCTR_GPU_BUSY_MASKED | 0x50b | 0xffffffff | |
| 7 | UCHE_WRITE_RANGE_MAX | 0xe05/6 | 0x0001ffff_ffffffc0 | trap base + 0xfc0 |
| 8 | UCHE_TRAP_BASE, UCHE_WRITE_THRU_BASE | 0xe09/a, 0xe07/8 | 0x0001ffff_fffff000 | mainline's a6xx `uche_trap_base` |
| 9 | UCHE_GMEM_RANGE_MIN, _MAX | 0xe0b/c, 0xe0d/e | 0x00100000, 0x0017ffff | 512 KiB GMEM at 1 MiB |
| 10 | UCHE_FILTER_CNTL, UCHE_CACHE_WAYS | 0xe18, 0xe17 | 0x804, 0x4 | |
| 11 | CP_ROQ_THRESHOLDS_2, _1 | 0x8c2, 0x8c1 | 0x010000c0, 0x8040362c | |
| 12 | CP_MEM_POOL_SIZE | 0x8c3 | 128 | |
| 13 | PC_DBG_ECO_CNTL | 0x9e00 | 0x00180000 | prim fifo threshold |
| 14 | CP_AHB_CNTL | 0x98d | 0x1 | AHB default slave response = error |
| 15 | RBBM_PERFCTR_CNTL; CP_PERFCTR_CP_SEL(0) | 0x500; 0x8d0 | 1; 0 | 0 = `PERF_CP_ALWAYS_COUNT` (`a6xx_perfcntrs.xml`) |
| 16 | RB_NC_MODE_CNTL, TPL1_NC_MODE_CNTL, SP_NC_MODE_CNTL | 0x8e08, 0xb604, 0xae02 | 0x2 each | UBWC: hbb_lo 1, everything else 0 |
| 17 | UCHE_MODE_CNTL; RBBM_NC_MODE_CNTL | 0xe01; 0x534 | 0x00200000; 0 | hbb_lo << 21; macrotile 0 |
| 18 | RBBM_INTERFACE_HANG_INT_CNTL | 0x1f | 0x401fffff | hang detect on |
| 19 | UCHE_CLIENT_PF | 0xe19 | 0x81 | |
| – | GMU busy and power counters | | **skip** | Stage 9 |
| – | CP_PROTECT_CNTL and the 32 `a630_protect` entries | 0x84f, 0x850+ | **skip** | Stage 6 turns it on (see note) |
| – | RBBM_INT_0_MASK | 0x38 | **stays 0** | no handler yet; mainline's 0x03d0e242 waits for Stage 6 |

Note on CP_PROTECT: the scratch registers 0x883–0x88a sit just past mainline's `NORDWR(0x800, 0x82)` span,
so the 4.5 test keeps working once protection is on.

Never written in Stage 4: RBBM_SECVID_TRUST_CNTL, RBBM_SW_RESET_CMD, anything in the GMU's own range
except what 2B already does.

✅ Pass when every row reads back its value. A row that reads back differently is logged, as in 2B.2's RSCC
rule. It stops the stage only for rows 1–4 (halt, secure and address modes), which the CP needs.

#### 4.2 SQE and ring registers, CP still stopped (`sweetcp=2`)

| Register | Offset | Value |
|---|---|---|
| CP_SQE_INSTR_BASE | 0x830/0x831 | SQE IOVA (0x10257a000 on G3l's layout) |
| CP_RB_BASE | 0x800/0x801 | ring IOVA |
| CP_RB_CNTL | 0x802 | 0x0800020b: BUFSZ 11 (16 KiB / 8), BLKSZ 2 (32 B / 8) << 8, NO_UPDATE (bit 27) |
| CP_RB_WPTR | 0x807 | 0 |

CP_RB_RPTR_ADDR is not written, because NO_UPDATE means the CP never writes an RPTR shadow.

✅ Pass when all four read back, RPTR reads 0, and the CP dump shows nothing new.

#### 4.3 Start the SQE on an empty ring (`sweetcp=3`)

1. `CP_SQE_CNTL = 1`. **This is the GPU's first memory fetch through the SMMU**: the SQE reads its
   microcode from the SQE IOVA.
2. Dump at 1 ms, 100 ms and 1 s.

✅ Pass when bank 0's FSR stays 0, CP_HW_FAULT stays 0, RPTR stays 0 (the ring is empty), and RBBM_STATUS
settles to idle: `RBBM_STATUS & ~1 == 0`, mainline's check, where bit 0 is our own AHB access.

#### 4.4 `CP_ME_INIT` (`sweetcp=4`)

Ring dwords 0–8: `0x70c80008` (PKT7 opcode 0x48, count 8), then `0x2f, 0x3, 0x20000000, 0, 0, 0, 0, 0`,
the mainline a6xx payload. Then `dsb sy` and `CP_RB_WPTR = 9`.

✅ Pass when, within 1 s, RPTR = 9, RBBM_STATUS is idle and CP_HW_FAULT is 0. Log the time it took. This
is mainline's `a6xx_idle` test without the GMU-idle part.

#### 4.5 Register markers and ring wrap (`sweetcp=5`)

1. One marker: `0x48088a01` (PKT4 CP_SCRATCH_REG(7) 0x88a, count 1), then `0x5ee70001`. Update WPTR, then
   poll until CP_SCRATCH_REG(7) reads 0x5ee70001 and RPTR equals WPTR.
2. A wrap test: 80 batches. Each batch is 64 markers (128 dwords) with values 0x5ee71000 + n. After each
   batch, wait for idle and check the scratch register holds the batch's last value. 80 × 128 dwords is
   2.5 times round the 4096-dword ring. Because ME_INIT left an odd 9-dword offset, some packets straddle
   the end of the ring, as they will in real use.
3. Check that the scratch page still holds 0x5ee7dead everywhere and that the ring area outside what we
   wrote is still zero (no stray GPU writes).

✅ Pass when every batch lands, RPTR and WPTR agree after each batch (WPTR wraps modulo 4096), and both
tripwires are intact.

#### 4.6 Hold (`sweetcp=6`, the whole of Stage 4)

For 30 s, every 10 s: one marker (0x5ee72000 + n), the CP dump, the GX GDSC and the GMU state. Then leave
the SQE running and idle, with OOB_GPU still held. Rebooting clears it, as with Stage 3.

✅ Pass (Stage 4 as a whole) when all markers land, no SMMU fault, CP_HW_FAULT or CP_PROTECT_STATUS is
latched, RBBM_STATUS stays idle between markers, and the phone stays stable.

The draft's "SQE version register reads back" criterion is dropped: a6xx has no such register (the version
lives in the firmware file, checked in 4.0). The CP_SQE_INSTR_BASE readback in 4.2 replaces it.

#### Test boots

One kernel (0107) and three images. All use `sweetgpu=6 sweetgmu=6` and the 828xr ADT, and differ only in
`sweetcp`. `shim/out` has no G4* names yet. Each boot needs the owner's OK.

| Image | `sweetcp` | Writes beyond G3l | Purpose |
|---|---|---|---|
| G4a | 2 | the init list and the ring registers; CP not started | the writes stick |
| G4b | 4 | also `CP_SQE_CNTL = 1` and ME_INIT | the CP runs (first GPU fetch) |
| G4c | 6 | also the markers, the wrap test and the hold | Stage 4 complete |

Fixes found on G4a or G4b are new patches and CI runs, shown before anything boots.

#### Risks for Stage 4, most uncertain first

1. **The SQE fetch faults or reads garbage.** Bank 0 has not done a real walk yet (ATOS is off). The
   FSR/FAR name the address. Other suspects: dword 0 not skipped, or the write-combined copy not drained
   before `CP_SQE_CNTL` (the 4.0 readback plus `dsb sy` cover that).
2. **Secure mode blocks the ring fetch.** Mainline runs ME_INIT *before* leaving secure mode, so the CP
   should fetch non-secure memory while secure. If RPTR stays 0 with no fault, this is the first guess.
   The answer would be the zap (Stage 5) before ME_INIT, not writing SECVID_TRUST_CNTL.
3. **A SECVID TSB write is refused or raises an external abort.** The readback shows a refusal. An abort
   panics the test boot, and the persistent log survives (as on G0H).
4. **Hang detect that we cannot see.** INT_0_MASK stays 0, and we do not know yet whether INT_0_STATUS
   latches masked causes. The dumps tell. If it does not latch, hangs show only as RPTR ≠ WPTR plus
   RBBM_STATUS busy, which is enough for Stage 4.
5. **GX collapses under us.** OOB_GPU is held as in 2B.6, so the GMU should not collapse GX. A CPU read of a
   collapsed GX would stall one core. G0H showed that does not freeze iOS, and the log survives.
6. **The skipped HWCG.** Without it the clocks run ungated: more power, nothing functional. Mainline turns
   it on before the CP starts. If the SQE misbehaves only without it, Stage 4 gets an HWCG step.

Stage 4 leaves these for later: memory writes and leaving secure mode (Stage 5), CP_PROTECT, interrupts,
indirect buffers and fences (Stage 6), HWCG and power collapse (Stage 9).

### Stage 5: zap shader and leaving secure mode (patch 0109; G5a stopped at 5.0: PAS 13 not supported)

Planned 2026-10-25, after G4c passed Stage 4. The owner asked to see this plan before any code.

**Idea.** Do what mainline's a6xx `hw_init` does right after `CP_ME_INIT`: have TrustZone load and
authenticate the zap shader (PAS id 13), then send `CP_SET_SECURE_MODE 0` on the ring. Then test the GPU's
first writes to DRAM with `CP_MEM_WRITE` to the scratch page. Stage 4 left the CP running in secure mode,
where (by their 0130 finding) the ring writes nothing to normal memory. This is the first stage that calls
TrustZone for something other than PSCI.

**Sources, read in this session:**
- Mainline Linux 6.18.54 (the WSL kernel tree): `firmware/qcom/qcom_scm.c` (`qcom_scm_pas_init_image`,
  `_mem_setup`, `_auth_and_reset`, `_pas_supported`, `__qcom_scm_is_call_available`, `__get_convention`),
  `qcom_scm-smc.c` (`__scm_smc_call`, the INTERRUPTED, EBUSY and WAITQ loops), `qcom_scm.h` (ids, error
  codes, `QCOM_SCM_ARGS`), `soc/qcom/mdt_loader.c` (`qcom_mdt_read_metadata`, `qcom_mdt_pas_init`,
  `__qcom_mdt_load`), `drm/msm/adreno/adreno_gpu.c` (`zap_shader_load_mdt`) and `a6xx_gpu.c`
  (`a6xx_zap_shader_init` and the zap block of `hw_init`), and `adreno_pm4.xml` (opcodes).
- The firmware files, parsed in WSL (below).
- Our PSCI `smc #0` helper from patch 0065, which already works on this phone (`CPU_ON`, 8 cores).

**What mainline does:**
1. `qcom_mdt_pas_init`: build the metadata (ELF header segment plus hash segment), copy it into a
   physically contiguous, 4 KiB aligned, **non-cacheable** buffer ("to avoid XPU violations"), then call
   `PAS_INIT_IMAGE(13, pa)`. If any loadable segment has the relocatable flag (bit 27), also call
   `PAS_MEM_SETUP(13, carve-out pa, align_4k(max p_paddr + p_memsz) - min p_paddr)`.
2. `__qcom_mdt_load`: copy each loadable segment to `carve-out + (p_paddr - min p_paddr)` through a
   write-combined mapping, zero-fill `p_memsz - p_filesz`.
3. `PAS_AUTH_AND_RESET(13)`. `-EOPNOTSUPP` means "no zap on this target"; we do not take that path.
4. On the ring: `CP_SET_SECURE_MODE` with payload 0, flush, and `a6xx_idle`. With no zap (`-ENODEV`) mainline
   writes `RBBM_SECVID_TRUST_CNTL = 0` instead. Our `secvid-set-once` quirk forbids that, and this
   plan never does it.
5. The zap is loaded once per boot (`static bool loaded`). Later power-ups skip it. Power collapse is Stage 9.

**The SCM calls** (SMCCC, SIP owner, 64-bit, from `qcom_scm.h`). The function id is
`type << 31 | 1 << 30 | 2 << 24 | svc << 8 | cmd`. Mainline uses fast calls (type 1) for the atomic
convention probe and standard calls (type 0) for the rest. x1 = arginfo
(`nargs | type_i << (4 + 2i)`, VAL 0, RO 1, RW 2), x2..x5 = args. The results come back in x0 (status) and
x1 (`result[0]`).

| Call | x0 (function id) | x1 arginfo | x2.. | Wanted result |
|---|---|---|---|---|
| convention probe = `IS_CALL_AVAIL` (svc 6, cmd 1), fast | 0xc2000601 | 0x1 | 0x02000601 | x0 0, x1 1 |
| `IS_CALL_AVAIL` for PIL cmds 1, 2, 5, 7 | 0x42000601 | 0x1 | 0x02000201 / 202 / 205 / 207 | x0 0, x1 1 each |
| `PAS_IS_SUPPORTED` (svc 2, cmd 7) | 0x42000207 | 0x1 | 13 | x0 0, x1 1 |
| `PAS_INIT_IMAGE` (cmd 1) | 0x42000201 | 0x82 (VAL, RW) | 13, metadata pa | x0 0, x1 0 |
| `PAS_MEM_SETUP` (cmd 2) | 0x42000202 | 0x3 | 13, 0x97715000, 0x1000 | x0 0, x1 0 |
| `PAS_AUTH_AND_RESET` (cmd 5) | 0x42000205 | 0x1 | 13 | x0 0, x1 0 |

Never called: `PAS_SHUTDOWN` (cmd 6) and `SET_REMOTE_STATE`. A half-done PAS session is cleared by the
reboot that ends every test boot.

How x0 is handled, as in `qcom_scm-smc.c`:
- **1, INTERRUPTED:** call again with x0 = 1, the same x1–x5, and the x6 the call returned (Qualcomm's
  "A6 quirk": x6 carries the session). Up to 1000 times.
- **-12, EBUSY:** sleep 30 ms and repeat the original call, up to 20 times (mainline's numbers).
- **2, WAITQ_SLEEP:** needs the wait-queue service, which we do not implement. Log it and stop.
- **-1 / -2 / -3 / -4 / -5:** ERROR / EINVAL_ARG / EINVAL_ADDR / EOPNOTSUPP / ENOMEM. Log the name and stop.
- x0–x7 are logged for every call, together with the retry counts and the time taken.

The SMC is issued from the GPU thread with interrupts on (as for PSCI). The helper is a new
`sweet_scm_call()` modelled on our 0065 PSCI helper: x0–x7 in, x0–x3 and x6 out, x8–x17 clobbered.

**The firmware** (parsed in WSL from `~/ios-pixel/gpu-fw/fw/`):
- `a615_zap.mdt` is ELF32 (class 1, machine 164 = QDSP6 header convention), 6860 B, entry 0x5000,
  3 program headers:

| # | p_type | p_offset | p_paddr | p_filesz | p_memsz | p_flags | Meaning |
|---|---|---|---|---|---|---|---|
| 0 | NULL | 0 | 0 | 0x94 | 0 | 0x07000000 | the ELF header segment (MDT type 7) |
| 1 | NULL | 0x1000 | 0x6000 | 0x1a38 | 0x2000 | 0x02200000 | hash segment (MDT type 2) |
| 2 | LOAD | 0x3000 | 0x5000 | 0x7c0 | 0x7c0 | 0x08000007 | the code; **relocatable** (bit 27) |

- So the metadata is 0x94 + 0x1a38 = 6860 B. Because the hash follows the header in the file, the
  metadata is **the whole `.mdt` file**, which is exactly our `GPU-ZAP-MDT` blob. `.b00` and `.b01` are its
  two halves (checked byte for byte).
- The `GPU-ZAP-B02` blob (1984 B = 0x7c0) is segment 2.
- min p_paddr = 0x5000. max = align_4k(0x5000 + 0x7c0) = 0x6000. **`MEM_SETUP` size = 0x1000**: the
  old draft's 0x2000 was the carve-out size, not mainline's formula. Segment 2 goes to carve-out offset 0.
- The carve-out 0x97715000 (0x2000, from the Pixel DT) lies below XNU's RAM (0x1_0000_0000+ in
  high-memory mode), so XNU never allocates it. It is inside Stage 3's identity map.

**What we already have from Stages 2B–4:** OOB_GPU held, the SQE running and idle after ME_INIT and 4.5's
markers, `cp_emit` / `cp_kick` / `cp_wait_idle` / `cp_marker` / `cp_dump` / `cp_clean`, and the scratch page
filled with 0x5ee7dead. Both blobs have been in every image since 0094.

**Boot-args:**
- `sweetgpu=7` selects Stage 5. It needs `sweetgmu=6`, and Stage 4 must pass in the same boot with
  `sweetcp=5` or more, because the zap comes after ME_INIT as in mainline. The test images use `sweetcp=5`,
  which skips 4.6's 30 s hold. `sweetgpu=6` stays exactly as on G4c.
- The new `sweetzap=N` (0–6, default 0) stops after step 5.N.

**New code (patch 0109):**
- `sweet_zap.c` (new, about 350 lines): `sweet_scm_call()` with the retry rules, the MDT checks, steps
  5.0–5.6, and the "SCM log" line per call.
- `sweet_cp.c`: export the ring helpers (`emit`, `kick`, `wait_idle`, `marker`, `dump`, `clean`, `halt`) and
  the scratch page va/pa through `sweet_gpu_internal.h`. Nothing in stage 4's behaviour changes.
- `sweet_gpu.c`: parse `sweetzap`; after `sweet_cp_run()` succeeds with `gpu_level >= 7`, call
  `sweet_zap_run(gpu_zap_stop, MDT blob, B02 blob)`. The "stages above N not built" message moves to
  `gpu_level > 7`.
- No `mkboot.sh` change: both zap blobs are already in the image, and the metadata copy fits in GPU-GMU-MEM.

**Memory** (GPU-GMU-MEM offsets after Stage 4's areas; PAs as on G4c, where the tripwire/scratch page was
0x10258a000):

| Offset | Size | PA on G4c layout | Use |
|---|---|---|---|
| (stage 4) | 4 KiB | 0x10258a000 | scratch page: 0x5ee7dead, now the `CP_MEM_WRITE` target |
| next | 8 KiB | 0x10258b000 | metadata copy (6860 B, rest zero): contiguous, 4 KiB aligned, mapped non-cacheable like mainline's `dma_alloc_coherent` buffer |
| carve-out | 4 KiB used of 8 | 0x97715000 | segment 2, written through a device mapping (`ml_io_map`) |

After `PAS_INIT_IMAGE` the CPU never touches the metadata copy again. After `PAS_AUTH_AND_RESET` it never
touches the carve-out again, because TrustZone locks both with the XPU, and a CPU access would abort.

**Register writes in Stage 5:** none by the CPU except the doorbell `CP_RB_WPTR`, and `CP_SQE_CNTL = 0`
when a step fails after 5.1. Everything else goes through TrustZone or the ring. **Never** written:
`RBBM_SECVID_TRUST_CNTL`. It is also never *read* after 5.3 (see risk 5).

**The "zap dump"** = stage 4's CP dump, the bank 0 FSR, CP_HW_FAULT, CP_PROTECT_STATUS and words 0–3 and
1023 of the scratch page. It is logged after every step and on any stop.

#### 5.0 Survey: SCM queries and the files (`sweetzap=0`)

Read-only SMCs, the first SIP calls ours has made:
1. The convention probe, then `IS_CALL_AVAIL` for PIL cmds 1, 2, 5 and 7, then `PAS_IS_SUPPORTED(13)`.
2. Check the MDT blob: ELF magic, class 1, e_phnum 3, the three headers as in the table above, segment 0
   filesz + hash filesz == blob length. Compute min/max/relocate the way `qcom_mdt_pas_init` does, and
   log the `MEM_SETUP` size it gives (want 0x1000).
3. Check the B02 blob: length == segment 2's p_filesz (0x7c0), and p_memsz ≤ 0x2000 (fits the carve-out).
4. Log RBBM_SECVID_TRUST_CNTL (G4 read 0x1) for the last time.
5. Map the carve-out and read its first and last words (it should be ordinary memory: a reboot resets the
   XPU). Allocate the metadata area, copy the MDT, read it back.

✅ Pass when every SMC returns x0 0 with x1 1, the file checks hold, and the copies compare equal. A
`PAS_IS_SUPPORTED` of 0 stops the stage. We would then need a different plan, not a fallback.

*(G5a hit exactly that stop: `PAS_IS_SUPPORTED(13)` = 0. "5.0 revised" just below replaces this rule; patch
0110.)*

#### 5.0 revised after G5a (approved; patch 0110)

**What the stock sunfish kernel does** (android-msm-sunfish-4.14, read for hardware facts only, no code copied):
- The DT (`sdmmagpie-gpu.dtsi`) has `pil_gpu: qcom,kgsl-hyp` with compatible `qcom,pil-tz-generic`, `qcom,pas-id =
  <13>` and `qcom,firmware-name = "a615_zap"`. So PAS 13 and our files are the right ones. The node has no clocks,
  regulators or `qcom,mem-protect-id`.
- `sdmmagpie.dtsi` reserves `pil_gpu_mem: gpu_region@97715000` (`removed-dma-pool`, `no-map`, size 0x2000). I
  did not find which node links to it; the files were cut off.
- kgsl loads the zap shader with `subsystem_get("a615_zap")` in `a6xx_microcode_load`. This runs inside
  `a6xx_rb_start`, after the SQE base is written and **before** `CP_SQE_CNTL = 1`, so the SQE is still halted. The
  zap is skipped when `mmu.secured` is false.
- `subsys-pil-tz.c` **never calls `PAS_IS_SUPPORTED`**. Its command list has only INIT_IMAGE (1), MEM_SETUP (2),
  AUTH_AND_RESET (5) and SHUTDOWN. Mainline `adreno_gpu.c` and `mdt_loader.c` (v6.6) do not call
  `qcom_scm_pas_supported` either. So no known GPU zap loader ever asks this question.
- The SMC calls match ours:
  - INIT_IMAGE: arginfo `SCM_ARGS(2, VAL, RW)`, args (13, metadata pa);
  - MEM_SETUP: arginfo `SCM_ARGS(3)`, args (13, start, size);
  - AUTH_AND_RESET: arginfo `SCM_ARGS(1)`, arg 13.
- The metadata is the whole `.mdt`. Stock copies it into a `dma_alloc_attrs` buffer marked
  `DMA_ATTR_STRONGLY_ORDERED`, with the mask set to the full `dma_addr_t` width (no 4 GiB limit). It frees the
  buffer straight after INIT_IMAGE. There is no hyp_assign of the metadata.
- The region is assigned to a subsystem VMID only when the node has `qcom,mem-protect-id`; `kgsl-hyp` has none.
  The relocatable MEM_SETUP size is the 4 KiB-aligned span, 0x1000, as we have it.
- **Bandwidth vote:** before INIT_IMAGE and before AUTH_AND_RESET, stock calls `scm_pas_enable_bw()`. That is an
  msm-bus vote for `MSM_BUS_MASTER_CRYPTO_CORE_0`, presumably so TZ's hash checks have crypto-to-DDR bandwidth.
  Stock fails INIT_IMAGE if it cannot vote. **We have no such vote.**
- The secure-mode exit is `adreno_set_unsecured_mode`. With a zap handle it emits `cp_secure_mode(0)` on the ring,
  as our 5.4 does. Without a zap handle (only when `mmu.secured` is false) it writes `RBBM_SECVID_TRUST_CNTL = 0`
  directly. The DT sets `qcom,gpu-quirk-secvid-set-once`, which only stops the TSB base/size writes from
  repeating.

**Guess (not proven):** `PAS_IS_SUPPORTED` answers from a table that leaves out PAS 13, or the hypervisor answers
it. Since neither loader asks, a 0 says nothing about whether INIT_IMAGE will accept PAS 13. Only 5.2 can show
that.

**Revised 5.0 (`sweetzap=0`), read-only like before:**
1. The probe and `IS_CALL_AVAIL` for PIL cmds 1, 2, 5 and 7, as before.
2. `PAS_IS_SUPPORTED` becomes **log only**, for 13 and, as a baseline, PAS 1 (ADSP), 4 (modem), 9 (video) and
   18 (CDSP). These are mainline's ids; I have not checked them against this phone's DT. If those all read 0 as
   well, the call simply means nothing here. If they read 1 and only 13 reads 0, it is a warning to carry into
   5.2, not a stop.
3. The MDT/B02 checks and the `SECVID_TRUST_CNTL` read, unchanged.
4. Steps 4–5 from above, which G5a never reached: map the carve-out (0x97715000, 0x2000) and read words, then copy
   the metadata and read it back.

✅ Pass when: every SMC returns x0 0; `IS_CALL_AVAIL` gives x1 1 for cmds 1, 2 and 5 (cmd 7 is log only); the file
checks hold; and the copies compare equal. `PAS_IS_SUPPORTED` values are logged and never stop the stage.

**Changes for 5.2–5.3 (same patch, for the later boots):**
- No crypto bandwidth vote is added yet. Display, CPU and GPU traffic already keep DDR busy, so I guess the crypto
  engine still reaches memory, only maybe slower. If 5.2 or 5.3 returns -1, or takes over 1 s, the next plan adds an
  RPMh BCM vote for the crypto master. We already have the cmd-db lookups from stage 0.
- Order: we keep mainline's order (zap after CP init, with the SQE running after 4.5). Stock instead loads it with
  the SQE halted. If AUTH_AND_RESET fails only with a running SQE, a later variant can halt it first with
  `sweet_cp_halt`, call AUTH, then restart.
- Keep `SECVID_TRUST_CNTL` never written. The direct write is stock's no-zap path only, and on this phone the
  register is probably locked (guess).

**Images:** G5a2 = patch 0110, `sweetzap=0`, otherwise the same as G5a. After it passes: G5b (`sweetzap=3`, through
AUTH_AND_RESET), then G5c (6).

#### 5.1 Secure-mode baseline (`sweetzap=1`)

While the GPU is still in secure mode, put one `CP_MEM_WRITE` on the ring: header `0x703d8003` (PKT7 opcode
0x3d, count 3), scratch page pa low, high, value `0xc0ffee00` to word 0. Then a register marker
(0x5ee75001). Wait idle (1 s).

✅ Pass when the CP goes idle, RPTR = WPTR, the marker lands, and no fault is latched. Word 0 is **logged
either way**: 0x5ee7dead confirms their finding (writes are dropped in secure mode), and 0xc0ffee00 means
it does not apply here. Either answer is useful, and only a hang or a fault stops the stage. The test shows
that 5.5's marker can tell the two states apart.

#### 5.2 `PAS_INIT_IMAGE` and `PAS_MEM_SETUP` (`sweetzap=2`)

1. `dsb sy`, then `PAS_INIT_IMAGE(13, metadata pa)`. TrustZone checks the signature on the hash segment
   here.
2. `PAS_MEM_SETUP(13, 0x97715000, 0x1000)`, only if 5.0 found the relocatable flag (it did).

✅ Pass when both return x0 0 and x1 0. On EINVAL_ADDR from INIT_IMAGE, the likely cause is risk 1.

#### 5.3 Load and `PAS_AUTH_AND_RESET` (`sweetzap=3`)

1. Copy B02 into the carve-out at offset 0 as 496 32-bit words (device memory: no unaligned access, no
   cache). Read every word back. Leave the rest of the page as it was (p_memsz = p_filesz, so there is no
   zero-fill). `dsb sy`.
2. `PAS_AUTH_AND_RESET(13)`. Log the time taken.
3. Zap dump.

✅ Pass when the readback matches, AUTH returns x0 0 and x1 0, and nothing new is latched in the CP or
SMMU. The GPU is still in secure mode at this point.

#### 5.4 Leave secure mode (`sweetzap=4`)

Ring: `0x70e60001` (PKT7 `CP_SET_SECURE_MODE` 0x66, count 1), `0x00000000`, then a register marker
(0x5ee75004). Kick, then wait idle, 1 s (mainline's `a6xx_idle`).

✅ Pass when the CP is idle with RPTR = WPTR, the marker lands, and CP_HW_FAULT, CP_PROTECT_STATUS and
bank 0's FSR stay 0. Mainline's comment says this packet hangs the CP without a zap, so a hang here
points at 5.3 even if AUTH returned 0.

#### 5.5 First GPU writes to DRAM (`sweetzap=5`)

1. `CP_MEM_WRITE 0xc0ffee01` to word 0 of the scratch page. Wait idle, `dsb sy`, read the word from the CPU
   (the mapping is non-cacheable, so no invalidate is needed).
2. Then in one submission: `0xc0ffee02` to word 1023 (the end of the page) and 16 writes of
   `0xc0ff1000 + i` to words 16–31.
3. Compare the whole page: the written words hold their values, and every other word is still 0x5ee7dead
   (word 0 may hold 5.1's value only if 5.1 found writes allowed).

✅ Pass when every write lands, nothing else in the page changed, and no fault is latched. **This is the
GPU's first visible write to DRAM**, and it is what fences and Stage 6 build on.

#### 5.6 Hold (`sweetzap=6`, the whole of Stage 5)

For 30 s, every 10 s: `CP_MEM_WRITE 0xc0ff2000 + n` to word 2 and a register marker, then the zap dump, the
GX GDSC and the GMU state. Afterwards leave the SQE running and idle, outside secure mode, with OOB_GPU held.

✅ Pass (Stage 5 as a whole) when every write and marker lands, nothing is latched, RBBM_STATUS is idle
between them, and the phone stays stable.

#### Test boots

One kernel (0109) and three images. All use `sweetgpu=7 sweetgmu=6 sweetcp=5` and the 828xr ADT, and
differ only in `sweetzap`. `shim/out` has no G5* names yet. Each boot needs the owner's OK. Leave the phone
on about 5 minutes.

| Image | `sweetzap` | Beyond G4c | Purpose |
|---|---|---|---|
| G5a | 0 | read-only SMC queries, file checks, metadata copy | TrustZone answers, and supports PAS 13 |
| G5b | 3 | the secure baseline, INIT_IMAGE, MEM_SETUP, the load, AUTH_AND_RESET | TrustZone accepts our zap |
| G5c | 6 | `CP_SET_SECURE_MODE 0`, the DRAM writes, the hold | Stage 5 complete |

Fixes found on G5a or G5b are new patches and CI runs, shown before anything boots.

#### Risks for Stage 5, most uncertain first

1. **TrustZone refuses the metadata buffer.** Ours is above 4 GiB (0x1_0258_b000 on the G4c layout).
   Mainline's coherent buffer is usually below 4 GiB, and some TrustZone builds may want a 32-bit address.
   EINVAL_ADDR (-3) or an XPU error would say so. The fix would be a buffer below 4 GiB, outside XNU's RAM.
   Which range to use is the owner's decision, because Android-era carve-outs live there.
2. **The hypervisor filters SIP calls from our EL1.** PSCI passes through. The PIL calls should too, since
   Android's kgsl makes the same ones. An SMC that never returns stalls one core (G0H: iOS keeps running,
   and the log survives).
3. **Signature or anti-rollback failure in INIT_IMAGE / AUTH_AND_RESET.** The files come from the sunfish
   factory image's `vendor.img`, so the key is right. Nothing confirms that build matches the phone's
   TrustZone version, and an older zap could be refused for rollback. The x0/x1 codes are logged.
4. **CP_SET_SECURE_MODE hangs** even though AUTH passed. Dump, halt the SQE, and stop. The zap normally runs
   from the carve-out, so the first suspect is the load offset (MEM_SETUP base vs p_paddr).
5. **A read of a locked GPU register aborts.** After the zap, `RBBM_SECVID_TRUST_CNTL` may be protected, and
   `CP_AHB_CNTL = 1` (4.1) makes bad AHB accesses return an error, which can become a CPU abort. So 0xf400
   is not read after 5.3.
6. **The 5.1 baseline hangs or faults the CP.** That is unlikely, given their finding that the write is
   just dropped. If it happens, 5.1 becomes optional in a follow-up patch, and G5b is re-run without it.
7. **WAITQ_SLEEP (2) from TrustZone.** Newer firmware uses it, and this 2019 firmware probably does not. We
   stop rather than implement it.

Stage 5 leaves these for later: CP_PROTECT, interrupts, indirect buffers, `CACHE_FLUSH_TS` fences (Stage 6),
and reloading the zap after power collapse (Stage 9).

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

1. **GX power through the GMU (2B).** 2A showed the host cannot start the GPU core clock. 2B adds the
   SMMU, RSCC/PDC, the firmware start and HFI at once; its own risk list is at the end of Stage 2B.
2. **The hypervisor and the GPU SMMU.** Trapped global registers, or stage-2 holes. This now comes up in
   2B.1, before Stage 3.
3. **TrustZone PAS for the zap on Pixel firmware.** It may need the metadata in specific memory (below 4 GiB);
   see Stage 5's risk list.
4. **sweetgl's a5xx assumptions on a6xx.** Pitch rules, params, nogmem/nofp16 workarounds.
5. **Licence** of the sweetgl code (section 5).
6. **Build cost.** Mesa on GitHub macOS runners; mitigated by caching.

## 9. Next concrete step

Stages 0–3 passed on the phone. The last results: G3k (Stage 2 complete, gx_gfx3d at 180 MHz on the GPU
PLL) and G3l (Stage 3, the GPU's SMMU bank for SID 0 reads back with no fault). The history is in the
stage sections above.

Stage 4 (CP first light) is now planned in detail ("Stage 4" above): survey, the init list, SQE and ring
registers, SQE start, ME_INIT, register markers with a ring-wrap test, and a 30 s hold, under `sweetgpu=6
sweetcp=N`. Patch 0107 builds it (`sweet_cp.c`, the bump allocator in `sweet_gmu.c`, the `sweetgpu=6`
hook; commit 5f906bf, CI 40181166244 passed, artifact `xnu-kernel`).

Result on G4a (0107, `sweetcp=2`, second boot; the first was switched off before stage 3): 2B and stage 3
passed again, 4.0 passed (SQE at 0x10257e000, ring 0x102586000, tripwire 0x10258a000, all read back; CP
registers and CP_PROTECT 0, SECVID_TRUST_CNTL 1, GBIF_HALT 0, GBIF_HALT_ACK 0x3). 4.1 stopped at its second row:
RBBM_GBIF_HALT (0x16) reads 0xdeafbead before and after writing 0, as do 0x17 and RBBM_NC_MODE_CNTL (0x534).
No SMMU or CP fault; the SQE was never started. Guess, not proven: those registers do not exist on a618.
Patch 0108 (commit 2e6dacd, CI 40183251805 passed) makes that row logged only; GBIF_HALT stays required.
Leave the phone on about 4 minutes: stage 4 starts about 2.5 minutes after boot.

Result on G4d (0108, `sweetcp=2`): stage 4 done at 4.2. Every required 4.1 row read back. Logged rows that
differ: RBBM_GBIF_HALT and RBBM_NC_MODE_CNTL (0xdeafbead), RBBM_PERFCTR_GPU_BUSY_MASKED (0x002f7fff),
UCHE_GMEM_RANGE_MAX_LO (0x0017f000: the low 12 bits are not kept), CP_ROQ_THRESHOLDS_2 (0) and UCHE_CLIENT_PF
(0x1). 4.2's SQE and ring registers all read back, RPTR 0, RBBM_STATUS 0x1, no CP or SMMU fault. GBIF_HALT_ACK
still reads 0x3.

Result on G4b (0108, `sweetcp=4`): stage 4 done at 4.4, the CP runs. 4.3: with CP_SQE_CNTL=1 on the empty
ring, RPTR stays 0 and the GPU idle at 1 ms, 100 ms and 1 s, CP_HW_FAULT and CP_PROTECT_STATUS 0, and no fault on
SMMU bank 0 or 4, so the SQE's microcode fetch through bank 0 worked. 4.4: ME_INIT was consumed, RPTR 9 and idle
after 10 us, no fault. The SQE stays running.

Result on G4c (0108, `sweetcp=6`): **stage 4 passed.** 4.5: the first marker landed (SCRATCH_REG7 0x5ee70001,
RPTR 11), then all 80 batches of 64 (each idle within 10 us), two ring wraps, last value 0x5ee723ff, RPTR = WPTR
2059. The ring past ME_INIT was still zero beforehand and the tripwire page was unchanged after. 4.6: markers
0x5ee72001..3 at 10, 20 and 30 s, RBBM_STATUS 0x1, CP_HW_FAULT and CP_PROTECT_STATUS 0, no SMMU fault, GMU
and GX GDSC steady.

Stage 5 (zap shader, leaving secure mode, first DRAM writes) is now planned in detail ("Stage 5" above):
read-only SCM queries, a secure-mode baseline write, PAS INIT_IMAGE / MEM_SETUP (size 0x1000) / load /
AUTH_AND_RESET, `CP_SET_SECURE_MODE 0`, `CP_MEM_WRITE` to the scratch page, and a 30 s hold, under `sweetgpu=7
sweetcp=5 sweetzap=N`, images G5a/G5b/G5c. Patch 0109 builds it (new `sweet_zap.c`, ring helpers exported from
`sweet_cp.c`, the `sweetgpu=7` hook; commit 8e908dd, CI 40194604743 passed, artifact `xnu-kernel`). In 5.5 the 16 writes to words 16–31 carry
`0xc0ff1000 + word index`.

**G5a result (booted once, `sweetzap=0`): Stage 5 stopped at 5.0, as the pass rule says, because `PAS_IS_SUPPORTED(13)`
returned x0 0, x1 0.** Seen in the log:
- Stages 0–4 passed again. Stage 4 reached 4.5 ("stage 4 done at 4.5"). The scratch page held its poison: 0 of 1024
  words differed.
- The SMC64 probe (fast call, 15 µs) and `IS_CALL_AVAIL` for PIL cmds 1, 2, 5 and 7 all returned x0 0, x1 1. None was
  interrupted or busy.
- `PAS_IS_SUPPORTED(13)` returned x0 0 (the call itself succeeded) and x1 0 ("not supported"). It took 188 µs; the
  `IS_CALL_AVAIL` calls took 4–5 µs.
- `RBBM_SECVID_TRUST_CNTL` read 0x00000001.
- The MDT checks all held: ELF32, machine 164, 3 program headers, metadata = the whole 6860-byte file, segment 2 at
  carve-out + 0 (0x7c0 bytes, relocatable), MEM_SETUP size 0x1000, B02 1984 bytes.
- The carve-out read and the metadata copy did not run; the stage stopped first.
- No fault and no panic. The phone kept running to the end of the log.

Not known (guesses):
- Whether TZ would still accept `INIT_IMAGE` for PAS 13. The not-supported answer may only mean that this
  `IS_SUPPORTED` table leaves the GPU out. Mainline's zap loader does not call `PAS_IS_SUPPORTED` (from memory, not
  checked).
- Whether the hypervisor answered instead of TZ. The longer time suggests the call did more work than the others, but
  proves nothing.

Per this plan, 5.0 = 0 means a new plan, not a fallback. Research of the stock sunfish kernel, done with the
owner's OK, found:
- PAS 13 and `a615_zap` are right.
- The stock loader and mainline never call `PAS_IS_SUPPORTED`.
- There is no hyp_assign for the zap.
- Stock votes crypto-core bus bandwidth before INIT_IMAGE and AUTH_AND_RESET.

That gives "5.0 revised" (in Stage 5 above): `PAS_IS_SUPPORTED` becomes log only, with PAS 1/4/9/18 as a
baseline. The owner approved it.

Patch 0110 (commit 5b29b1f) changes only `zap_survey` in `sweet_zap.c`:
- `IS_CALL_AVAIL` must give 1 for cmds 1, 2 and 5; cmd 7 is logged only.
- `PAS_IS_SUPPORTED` is asked for 13, 1, 4, 9 and 18. Each answer is logged as "PAS n reads ...", and the SMC must
  still return x0 0.
- The `sup` term is gone from the pass rule.

The local clang check was clean, and the series 0094–0110 applies on 0093. CI run 40197408369 passed in 9m01s (artifact `xnu-kernel`). Nothing
is booted. G5a2 is packed from out-0110:
- The boot-args are identical to G5a's.
- The blob addresses are the same as G5a's.
- The 828xr ADT and all firmware are in the payload.

**G5a2 result (booted once, `sweetzap=0`): revised 5.0 PASSED ("stage 5 done at 5.0").** Seen in the log:
- Stages 0–4 passed again (4 done at 4.5). The scratch page was untouched: 0 of 1024 words differed.
- The probe and `IS_CALL_AVAIL` for PIL cmds 1, 2, 5 and 7 all gave x0 0, x1 1.
- `PAS_IS_SUPPORTED` gave x0 0 and x1 0 for **every** id asked: 13, 1, 4, 9 and 18. 13 took 188 µs; the others
  took 30–37 µs. Those other subsystems certainly run on this phone, so this call means nothing on this TZ.
  G5a's stop was a false alarm.
- `SECVID_TRUST_CNTL` read 0x1. The MDT and B02 checks were the same as G5a's.
- The carve-out at 0x97715000 mapped and read without a fault. The first and last words are both 0xafafafaf, a fill
  pattern (presumably left by the bootloader or TZ; not known).
- The metadata copy went to pa 0x10258f000, as guessed. All 6860 bytes were copied, the first word is 0x464c457f
  (ELF magic), and 0 words differed.
- SMMU bank 0 FSR stayed 0x400, the same as on every earlier boot. No panic.

Open risk for 5.2: the metadata pa is above 4 GiB. Stock sets no 32-bit mask, but nothing proves TZ accepts it. If it
does not, INIT_IMAGE should fail with an error (for example -3 EINVAL_ADDR) rather than hang (guess). Next, with the
owner's OK: pack G5b (`sweetzap=3`: 5.1 baseline, 5.2 INIT_IMAGE + MEM_SETUP, 5.3 load + AUTH_AND_RESET).
G5b is packed from out-0110:
- Its boot-args differ from G5a2's only in `sweetzap=3`.
- The blob addresses are the same as G5a2's.
- The 828xr ADT and all firmware are in the payload.

Not booted. Next, with the owner's OK: boot G5b.
