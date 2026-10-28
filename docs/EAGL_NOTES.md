# EAGL interface notes (Stage 7.4)

Our own notes, written for the 7.5 EAGL shim. They are from local disassembly of the iOS 16.7 shared
cache on our root (QuartzCore 1045.17.2, OpenGLES 20.5.2). Per docs/STAGE7_LICENCE.md, no Apple code,
listings or dumps are in git; the raw work files stay in WSL `~/ios-pixel/work/eagl`.
Observed = read in the disassembly. Guess = not checked.

## How CA picks GLES (observed)

- `CA::WindowServer::EAGLServer::renderer()` first tries Metal (`CAMetalContextCreate`). If that
  returns NULL, as on our port, it goes to GLES.
- The GLES branch is gated by a flag that the EAGLServer constructor sets from `CA_ENABLE_OGL`. It is
  **on when the variable is unset**; `CA_ENABLE_OGL=0` turns it off. No env var is needed.
- CA builds a properties dictionary with `kEAGLContextPropertyAccelerated` and
  `kEAGLContextPropertyClientRetainRelease` (the only OpenGLES symbols QuartzCore imports, plus the
  `EAGLContext` class).
- `[[EAGLContext alloc] initWithAPI:3 properties:]`, falling back to API 2. Then
  `setParameter:0x260 to:&0`, whose return value CA ignores (see "Closed questions" below).
- **It then requires `[ctx API] == 3`**, otherwise no GL renderer. So the shim must give ES 3.0+.
  Mesa freedreno on a618 offers ES 3.2.
- `new CA::OGL::GLESContext(ctx)` (0x870 bytes), then `CA::OGL::Renderer`.
- Today CA falls back to its CPU renderer because both Metal and EAGL fail (guess: EAGL fails because
  no `IOAcceleratorES` service or GL plugin exists).

## How CA calls GL (observed): the macro table, not the gl* exports

- QuartzCore imports **no** `gl*` functions. The GLESContext constructor calls
  `[ctx getMacroContextPrivate]` once, keeps the pointer at `this+0x7f8`, and makes every GL call as
  `table->fn[i](table->ctx, args...)`.
- `getMacroContextPrivate` returns `priv + 0x20`, where `priv` is the EAGLContext's `_private`
  pointer (ivar at object +8). The returned block is `{ void *gli_ctx; void *fn[1036]; }`: the context
  pointer is passed as the first argument, and slot `i` is at byte offset `8 + 8*i`.
- OpenGLES's own exported `glFoo` functions use the same table. Each one reads the current `priv` from
  thread-specific slot 30 (`TPIDRRO_EL0 + 0xf0`), loads `priv+0x28+8*i`, and jumps there with
  `priv+0x20` as the first argument. Other frameworks (UIKit, CoreImage, GLKit) call these exports.
- `_EAGLContextPrivate` fields seen used: +0x00 the EAGLContext object (what `+currentContext`
  returns), +0x08 sharegroup, +0x10 API (`-API` reads it), +0x18 flags (bit 0 checked in
  `+setCurrentContext:`), +0x20 GLI context, +0x28 the 1036-slot table, then a second table of the
  same size (its use is unknown).
- `+setCurrentContext:` stores `priv` in TSD slot 30 (through OpenGLES's internal
  `eagl_ctx_tls_set`), then calls into the plugin (not traced). `+currentContext` returns
  `*(id *)tsd[30]`.

### Slots CA::OGL calls (observed; a lower bound, from a static scan)

The slot numbers were matched to names through OpenGLES's exported trampolines (457 of them).

```
  5 BindTexture          7 BlendFunc           10 Clear              12 ClearColor
 56 CopyTexSubImage2D   59 DeleteTextures      63 Disable            65 DrawArrays
 67 DrawElements        72 Enable              89 Finish             90 Flush
 98 GenTextures        102 GetError           104 GetIntegerv       117 GetString
195 PixelStorei        251 Scissor            301 TexImage2D        302 TexParameterf
304 TexParameteri      307 TexSubImage2D      335 Viewport          336 BlendFuncSeparate
338 BlendEquation      342 ActiveTexture      380 CompressedTexImage2D
470 FinishObjectAPPLE  511 VertexAttribPointer 512 EnableVertexAttribArray
513 DisableVertexAttribArray                  591 DeleteProgram and DeleteShader (one slot)
594 CreateShader       595 ShaderSource       596 CompileShader     597 CreateProgram
598 AttachShader       599 LinkProgram        600 UseProgram        602 Uniform1f
605 Uniform4f          606 Uniform1i          612 Uniform3fv        613 Uniform4fv
619 UniformMatrix3fv   620 UniformMatrix4fv   625 GetUniformLocation 630 BindAttribLocation
672 BindFramebuffer    673 DeleteFramebuffers 674 GenFramebuffers   677 FramebufferTexture2D
681 GenerateMipmap     779 FenceSync          781 DeleteSync        782 ClientWaitSync
816 InsertEventMarkerEXT 817 PushGroupMarkerEXT 818 PopGroupMarkerEXT
1024 FramebufferParameteriAPPLE
```

- Apple-only entries: `FinishObjectAPPLE(GL_TEXTURE, name)` (wait for one texture),
  `FramebufferParameteriAPPLE` (only used with GL_APPLE_tile_order_rot; see below), and the EXT
  markers (no-ops are fine).
- Extensions CA probes in `glGetString(GL_EXTENSIONS)`: GL_EXT_shader_framebuffer_fetch (blend
  filters, `gl_LastFragData`), GL_APPLE_fence, GL_APPLE_texture_filter_cubic, GL_APPLE_texture_xr,
  GL_APPLE_texture_ycbcr_basic/extended_formats, GL_APPLE_tile_order_rot,
  GL_EXT_texture_filter_anisotropic, GL_EXT_texture_lod_bias. Missing ones just turn features off
  (guess for all but framebuffer fetch).

## IOSurfaces and the display (observed)

- **Display buffer** (`GLESContext::make_buffer_current`): GenTextures, BindTexture(GL_TEXTURE_2D),
  MIN/MAG LINEAR, WRAP_S/T CLAMP_TO_EDGE, then
  `[ctx attachImage:GL_TEXTURE_2D toCoreSurface:<the WindowServer surface's IOSurface>
  invertedRender:NO]`, then GenFramebuffers, BindFramebuffer, FramebufferTexture2D(GL_FRAMEBUFFER,
  COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0), BindFramebuffer(0). CA draws into that FBO.
- **IOSurface textures** (`create_surface_from_iosurface`, `attach_surface_texture`): the same texture
  setup, then `texImageIOSurface:surf target:GL_TEXTURE_2D internalFormat:GL_RGBA width:w height:h
  format:GL_BGRA_EXT type:GL_UNSIGNED_BYTE plane:0 invert:`. `attach_surface_texture` also uses
  `attachImage:toCoreSurface:`.
- So both selectors mean: "give the bound texture this IOSurface's memory as its storage, with no
  copy". For us that is an import of the IOSurface pages into GPU VA (the SweetGPU import selector,
  tested in G6h) wrapped as a linear Mesa resource with the surface's pitch.
- **Present**: the EAGLServer path never calls `presentRenderbuffer:` or `renderbufferStorage:`. CA
  renders into the display IOSurface and then swaps it through its own IOMFB code. So the shim's job
  at the end of a frame is to make the GPU writes complete and visible before CA swaps. CA already
  asks for that with Flush/Finish, FinishObjectAPPLE and FenceSync/ClientWaitSync.

## GPU plugin loading (observed strings only; not traced)

OpenGLES matches the `IOAcceleratorES` (or `IOAcceleratorESLegacy`) service. libGFXShared reads
`IOGLESBundleName` / `IOGLESMetalBundleName` / `IOGLESDefaultUseMetal` and loads
`/System/Library/Extensions/<name>.bundle`. The approach below does not use this path.

The stub is still worth having, for a different reason (observed 2026-10-28):
- QuartzCore's `CAMetalContextCreate` once-block loops up to 300 times. Each pass looks up
  `IOServiceMatching("IOAcceleratorES")`, walks the iterator, and sleeps 100000 µs if nothing was
  found. So with no such service, backboardd waits **30 s** before Metal fails and CA tries GLES.
- The callee identities (matching, iterate, release, usleep) come from the call shape. The stubs
  were not resolved.
- xnu patch 0114 adds the stub (`SweetAccel.cpp`, boot-arg `sweetaccel=1`). It has no
  MetalPluginName or GLES bundle, and it is published with SweetGPU.

## What this means for 7.5 (proposal)

- The shim does not replace OpenGLES. Loaded into backboardd, it overrides EAGLContext's methods
  (`initWithAPI:` / `initWithAPI:properties:` / `initWithAPI:sharegroup:`, `setCurrentContext:`,
  `currentContext`, `getMacroContextPrivate`, `API`, `setParameter:to:`, `attachImage:...`,
  `texImageIOSurface:...`, `dealloc`). The context object owns a `priv` block with the layout above,
  whose table points at our Mesa-backed functions.
- `setCurrentContext:` writes `priv` to TSD slot 30 and makes the Mesa context current. After that,
  Apple's own gl* exports also reach Mesa, so other callers in the process work too.
- Every one of the 1036 slots gets an entry. The ~62 above call Mesa. The rest go to a stub that logs
  its slot number once (through sel 16, as sweetgpu-test does) and returns 0, so a missed call shows
  up in the log rather than as a crash.
- API 3 must be reported. `getMacroContextPrivate` must return `priv+0x20`.
- The `priv` block is 0x40e8 bytes: the 0x28-byte header, the table, and a zeroed back table (below).
  Fill +0x20 too, because `GLIContextFromEAGLContext` returns it.
- Slot 591 serves both glDeleteShader and glDeleteProgram (both exports jump there). Mesa keeps
  shaders and programs in one name space, so the entry can check glIsProgram and call the right one.

## Closed questions (observed, 2026-10-28)

**setParameter:to: / getParameter:to:.** OpenGLES does not interpret the parameter itself. If the
sharegroup's renderer state is valid, it calls the GL plugin's set-integer / get-integer entry with
(GLI context, pname, pointer) and returns its result. Otherwise it returns 10000 (0x2710). These are
CGL-style context parameters: `setMultiThreaded:` is 0x139 (313, CGL's `kCGLCEMPEngine`), and
`setBlockFence:onQueue:` uses 0x263 and 0x264. Public headers do not name 0x260 (608). CA sets it to 0
straight after init and **does not check the return value**. So the shim can accept any pname and
return 0. It should keep 0x139 working, because `isMultiThreaded` reads it back.

**FramebufferParameteriAPPLE (slot 1024).**
- The one call site is `CA::OGL::GLESContext::set_surface`. It makes the call as
  `fn(ctx, GL_FRAMEBUFFER, 0x8a79, &value)`, so the third argument is a pointer.
- `value` is 0x8a7a or 0x8a7d, depending on a bit in the surface's flags.
- CA keeps the last value in the surface (+0x64) and skips the call when the value is unchanged.
- The call only happens when GLESContext flag bit 6 is set. The constructor sets that bit only if
  `check_extension("GL_APPLE_tile_order_rot")` succeeds. **If the shim does not advertise
  GL_APPLE_tile_order_rot, slot 1024 is never called.** The stub entry is enough.
- Guess: 0x8a79 is a tile order or rotation pname, and the two values are two orientations.

**The second table and other priv fields.**
- priv+0x2088 (right after the first table's 1036 × 8 bytes) is the **back dispatch**. It is only touched
  by OpenGLES's exported `GLCSelectDispatchBounded`, `GLCSelectDispatchFunction`,
  `GLCRestoreDispatch` and `GLCRestoreDispatchFunction`, with `GLCBackDispatch` returning a pointer
  to it.
- Those functions let a tool interpose GL calls: they save an original entry there before
  overwriting the front entry, and put it back on restore. A zero entry means "not saved".
- QuartzCore imports none of them. CA reads nothing past the first table (its highest slot is 1024;
  its own +0x2100 stores are into the GLESContext object, not priv).
- So the shim allocates the back table zeroed and need not do anything else. Because the layout
  matches Apple's, those GLC* exports then work on our contexts too.
- Other priv reads by OpenGLES code the shim does not override: `GLIContextFromEAGLContext` (+0x20),
  `-API` (+0x10), `-sharegroup` (+0x08) and `+currentContext` (+0x00).