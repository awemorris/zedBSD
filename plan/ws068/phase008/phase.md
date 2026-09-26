<!-- awesome-plan project=zedbsd record=ws068p008 -->

# ws068-p008: GLES 2.0 の描画の核（SPIR-V の shader binary、GLSL の compiler の方式に依らない部分）

Phase ID: `ws068-p008`
Parent: [WS068](../ws.md)
Status: cleared（q475-i01、2026-09-26）
Phase disposition: normal
Queue: q475-i01
承認: 2026-09-26 ユーザーの自律実行の指示（EGL/GLES を含むデスクトップ関連を優先）
設計: [design.md](../design.md) §3・§4（GLES の方式 A・B のどちらでも要る「GL の状態 → Vulkan」の変換層）

## 背景

GLES の方式（design.md §4）はユーザーの判断待ちで、GLSL ES の compiler（自前か glslang か）が決まっていない。変換層（GL の object と状態を
Vulkan の pipeline・descriptor・buffer・image へ）は A・B のどちらでも同じなので、shader を SPIR-V で受ける道（`glShaderBinary`、
`GL_SHADER_BINARY_FORMAT_SPIR_V`）で先に作る。compiler が来たら、GLSL の source を同じ約束の SPIR-V にして同じ道へ入れる。

## SPIR-V の約束

- GL の uniform（sampler 以外）は set 0 binding 0 の uniform block 1 つ（std140、`gl_DefaultUniformBlock`）。`glGetUniformLocation` は
  block の member の名前（OpName・OpMemberName）と offset（OpMemberDecorate Offset）から。
- sampler は set 0 の binding 1 から（combined image sampler）、名前で `glGetUniformLocation`、`glUniform1i` で texture unit。
- attribute は location（OpDecorate Location）と名前（OpName）で `glGetAttribLocation`・`glBindAttribLocation`。
- y と z: vertex shader は GL の clip 座標のまま書く。link が entry point の各 OpReturn の前に `gl_Position.y = -y`、
  `gl_Position.z = (z + w) / 2` を足す（Vulkan 1.0 の core だけで、負の viewport も depth clip control も要らない）。
  y を裏返しても Vulkan の面積の式の符号が逆なので、GL の CCW は Vulkan の COUNTER_CLOCKWISE のまま。
- varying: fragment の input は同じ名前の vertex の output の location に書き換える。attribute は glBindAttribLocation の値に書き換える。
- 名前が要るので、SPIR-V は `-O` 無しで作る（glslc の `-O` は OpName を消す）。

## 範囲

1. libEGL の frame: 最初の GL の命令で acquire・command buffer・render pass（color と、config に depth があれば depth）を始め、
   `eglSwapBuffers` で終えて present。`glClear` は pass の中で `vkCmdClearAttachments`。
2. buffer（`glGenBuffers`・`glBindBuffer`・`glBufferData`・`glBufferSubData`・`glDeleteBuffers`）と client の配列。
3. shader と program（`glCreateShader`・`glShaderBinary`（SPIR-V）・`glShaderSource`＋`glCompileShader`（compiler 無しの log）・
   `glCreateProgram`・`glAttachShader`・`glLinkProgram`（SPIR-V の反射）・`glUseProgram`・`glGetAttribLocation`・`glBindAttribLocation`・
   `glGetUniformLocation`・`glUniform{1,2,3,4}{f,i}{,v}`・`glUniformMatrix4fv`・`glGetProgramiv`・`glGetShaderiv`・log）。
4. `glVertexAttribPointer`・`glEnableVertexAttribArray`・`glDrawArrays`・`glDrawElements`（TRIANGLES・STRIP・FAN・LINES・POINTS、
   STRIP と FAN は list に展開）。
5. texture（`glGenTextures`・`glBindTexture`・`glActiveTexture`・`glTexImage2D`（RGBA・RGB、UNSIGNED_BYTE）・`glTexSubImage2D`・
   `glTexParameteri`（filter・wrap）・`glDeleteTextures`）。
6. 状態: `glEnable`・`glDisable`（BLEND・DEPTH_TEST・CULL_FACE・SCISSOR_TEST）、`glBlendFunc`、`glDepthFunc`、`glDepthMask`、
   `glCullFace`・`glFrontFace`、`glScissor`、`glColorMask`、`glClearDepthf`。pipeline は状態の組で cache。
7. 試験: egltest に三角形・texture・blend・depth の場面（shader は host の glslc で SPIR-V にして埋め込む）。

## 受け入れ

1. Venus で、egltest の三角形（頂点色）、texture の四角、blend の重なり、depth の前後が画面の読み取りで期待の色になる（Wayland の窓）。
2. p002 の clear の試験が変わらず通る。build は warning 0、新しい C の style-check 0。
3. i915 実機は範囲外（ws068-p006）。GLSL の source は範囲外（compiler の方式の判断の後、p003）。

## 結果（2026-09-26、q475-i01）

cleared。受け入れ 1〜3 を満たした。

### 実装

- libEGL（vulkan.c・zegl.h・egl.c）: frame を GLES の最初の命令で開く（`zegl_frame_begin`）。pass は 2 つ（最初の pass は clear、
  readback の後の pass は load。どちらも終わると image は present の layout）。config に depth・stencil があれば depth buffer
  （D24S8、D32S8、D16S8、D32、X8D24、D16 の順で device が持つもの）。swapchain の image は TRANSFER_SRC（glReadPixels）。
  `zegl_frame_flush`（途中で submit して待つ）。present の後に GLES の `frame_done`、`eglDestroyContext` で `release`。
- libGLESv2: gles.h（状態）、gles.c（状態・固定機能・glGet）、buffer.c（device memory、stream、garbage、buffer object、upload）、
  texture.c（2D texture、RGBA8 への変換、mipmap の生成、sampler の cache、glCopyTex*）、spirv.c（反射と gl_Position の書き換え）、
  program.c（shader・program・link・uniform・attribute）、draw.c（vertex 配列、pipeline の cache、draw、glClear、glReadPixels）、
  framebuffer.c（FBO と renderbuffer は名前だけ。0 以外は `GL_FRAMEBUFFER_UNSUPPORTED`）。OpenGL ES 2.0 の 142 の関数をすべて export。
- 変換の要点: strip・fan・line loop は list に展開（i915 の F-023 を避ける）、byte の index は 32 bit に、device が fetch できない
  vertex 形式（GL_FIXED、3 byte 等）は float に変換。uniform block は draw ごとに stream へ写し、dynamic offset で渡す
  （同じ program・stream・texture の draw は descriptor set を使い回す）。buffer・texture の device 側は CPU の写しから作り、
  その frame が既に使っている device 側は書き換えずに新しく作る（古いものは frame の後に解放）。
- egltest: `--scene=draw`（scene.c、shaders/ と shaders.h、`shaders/regenerate.py`）。4 分割で strip（buffer object、byte の色）、
  texture（2x2、nearest）、blend（半透明）、depth と cull（glDrawElements、element buffer）。最初の frame を glReadPixels で読み、
  `EGLTEST PIXEL`・`EGLTEST CHECK` を出す。
- zwl: `ZWL GLASS dock` の行に docked の窓の位置と大きさ（試験用）。

### 検証（QEMU・Venus。i915 実機は未実施）

- host: `plan/ws068/tests/spirv-host/run.sh` PASS（glslc の出力の反射、書き換えた vertex shader を spirv-val で検査）。
- Venus: `plan/ws068/tests/egl-p008.sh` PASS（build/ws068-p008.log）。display 直接・zwl の窓・docked のそれぞれで画面の 10 点が
  期待の色、egltest の glReadPixels の 11 点も一致（failures=0、glerror=0）。画面: build/ws068-p008/{display,wayland,resized}.png。
- 回帰: `plan/ws068/tests/egl-p002.sh` PASS。build warning 0。新しい C の style-check 0、`git diff --check` 0。
- boot test: `plan/tools/boot-test.sh build/ws035-sq/hdd-image.img` PASS（build/ws068-p008-boot/login.png）。

### 分かったこと・制限

- 速さ: Venus で clear だけの frame は約 125 ms（Wayland）・150 ms（display 直接）。scene（11 draw）は約 133 ms。wltest（Vulkan
  の WSI を直接）は約 50 ms。差は EGL が 1 frame ずつ submit・present・fence を待つ作り（frame in flight が 1）による。
  → ws068-p009（frame を 2〜3 枚重ねる）。
- 範囲外で残したもの: GLSL の source（p003、方式の判断待ち）、FBO・renderbuffer・cube map（p010）、sampler の配列、
  行列の attribute、色 mask の一部だけの glClear（mask を無視して全 channel を消す）、線幅は 1 だけ、pbuffer。
- i915 実機: 未実施（ws068-p006）。
