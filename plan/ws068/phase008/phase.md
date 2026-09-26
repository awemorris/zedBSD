<!-- awesome-plan project=zedbsd record=ws068p008 -->

# ws068-p008: GLES 2.0 の描画の核（SPIR-V の shader binary、GLSL の compiler の方式に依らない部分）

Phase ID: `ws068-p008`
Parent: [WS068](../ws.md)
Status: in-progress（q475-i01）
Phase disposition: normal
Queue: q475-i01
承認: 2026-09-26 ユーザーの自律実行の指示（EGL/GLES を含むデスクトップ関連を優先）
設計: [design.md](../design.md) §3・§4（GLES の方式 A・B のどちらでも要る「GL の状態 → Vulkan」の変換層）

## 背景

GLES の方式（design.md §4）はユーザーの判断待ちで、GLSL ES の compiler（自前か glslang か）が決まっていない。変換層（GL の object と状態を
Vulkan の pipeline・descriptor・buffer・image へ）は A・B のどちらでも同じなので、shader を SPIR-V で受ける道（`glShaderBinary`、
`GL_SHADER_BINARY_FORMAT_SPIR_V`）で先に作る。compiler が来たら、GLSL の source を同じ約束の SPIR-V にして同じ道へ入れる。

## SPIR-V の約束（glslang の Vulkan relaxed の出力と同じ形）

- GL の uniform（sampler 以外）は set 0 binding 0 の uniform block 1 つ（std140、`gl_DefaultUniformBlock`）。`glGetUniformLocation` は
  block の member の名前（OpName・OpMemberName）と offset（OpMemberDecorate Offset）から。
- sampler は set 0 の binding 1 から（combined image sampler）、名前で `glGetUniformLocation`、`glUniform1i` で texture unit。
- attribute は location（OpDecorate Location）と名前（OpName）で `glGetAttribLocation`・`glBindAttribLocation`。
- y の向き: vertex shader は GL の clip 空間のまま、変換層が viewport の高さを負にせず、描画の後の画像を上下反転しないよう、
  vertex shader の出力の y を反転する（`gl_Position.y = -gl_Position.y` を SPIR-V に足す、または push constant の反転の係数）。

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
