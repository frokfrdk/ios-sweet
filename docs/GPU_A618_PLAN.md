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

1. **GX power through the GMU (2B).** 2A showed the host cannot start the GPU core clock. 2B adds the
   SMMU, RSCC/PDC, the firmware start and HFI at once; its own risk list is at the end of Stage 2B.
2. **The hypervisor and the GPU SMMU.** Trapped global registers, or stage-2 holes. This now comes up in
   2B.1, before Stage 3.
3. **TrustZone PAS for the zap on Pixel firmware.** It may need GPU clocks on, or the metadata in specific memory.
4. **sweetgl's a5xx assumptions on a6xx.** Pitch rules, params, nogmem/nofp16 workarounds.
5. **Licence** of the sweetgl code (section 5).
6. **Build cost.** Mesa on GitHub macOS runners; mitigated by caching.

## 9. Next concrete step

Stages 0 and 1 passed on the phone. Stage 1 needed one fix: patch 0096 turns the CX GDSC on before the GCC
GPU branches. The owner approved Stage 2A. On G2 (patch 0097) the rail votes and PLL0 worked, and the stage
stopped because the gfx3d RCG root stayed off with GX clamped. On G2b (patch 0098) the GX GDSC came on and
the GMU saw GX powered, so the apps DRV can raise the GX rail. The gfx3d root still read off and SPTP did not
come on. On G2m (patch 0099) the GCC counter confirmed the clocks around it run (cx_gmu and gx_gmu at
200 MHz, gx_cxo at 19.2 MHz), but gx_gfx3d does not run at all, either from XO or after the 180 MHz update.
2A has hit its decision rule. 2B is now planned in detail ("Stage 2B" above): GMU memory and an SMMU bank
for streams 4–5, RSCC/PDC, the firmware into the TCMs, CM3 start, the GX rail through the GMU, HFI v1,
then OOB_GPU and the first GX reads. Nothing of it is built. With the owner's go-ahead:
1. Write patch 0100 (`sweet_gmu.c`, `sweet_smmu.c`, the `sweetgpu=4` flow, the `gmu-mem` blob in
   `mkboot.sh`). Show it, push it and run CI.
2. Boot G3a (`sweetgmu=0`, read-only survey) first. G3b and G3c follow, each with its own OK.
