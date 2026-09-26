<!-- awesome-plan project=zedbsd record=ws069p005 -->

# ws069-p005: 固定機能の GL 1.x（glBegin/glEnd、行列、光源、display list）と gears

Phase ID: `ws069-p005`
Parent: [WS069](../ws.md)
Status: in-progress（q478-i01）
Phase disposition: normal
Queue: q478-i01
承認: 2026-09-26 ユーザーの GLX の指示と自律実行の指示
設計: [design.md](../design.md) §4、WS068 design.md §6（desktop GL は GLES の変換層の上の部分集合）

## 方式

- libGL の中だけに固定機能の層（`userland/X11/libGL/fixed.c`・`immediate.c`）。GLES の変換層（libGLESv2 と共有の sources）へは
  hook（`gles_fixed`、libGLESv2 では NULL）で入る: program の無い draw は固定機能の program（SPIR-V、uber shader）で描き、
  GL_LIGHTING 等の capability、GL_QUADS・GL_QUAD_STRIP・GL_POLYGON、glGet の固定機能の状態、desktop の GL_VERSION を足す。
- 固定機能の attribute は generic の 0〜3（位置・色・法線・texture 座標）に alias する（GL の compatibility の約束と同じ）。
  glColor 等の現在の値は attribute の現在の値、glVertexPointer 等は attribute の配列。
- 頂点ごとの光源（Gouraud）: 光源 8 つ（ambient・diffuse・specular・位置（glLight の時点の modelview で eye 空間へ）・減衰）、
  material（ambient・diffuse・specular・emission・shininess）、GL_COLOR_MATERIAL、GL_NORMALIZE、light model の ambient。
  texture は GL_MODULATE（と GL_REPLACE）、alpha test。
- glShadeModel(GL_FLAT): flat の program。GL の provoking vertex（primitive の最後の頂点、polygon は最初）が Vulkan の最初の頂点に
  なるよう、flat のときは index の list にして各 primitive を回す（巻きは保つ）。
- glBegin/glEnd は頂点を CPU に溜め、glEnd で client 配列として 1 回の draw。display list は GL 1.x の固定機能の命令を記録して
  glCallList で再生する（GL_COMPILE・GL_COMPILE_AND_EXECUTE、入れ子の glCallList）。

## 範囲

1. 行列: glMatrixMode・glLoadIdentity・glLoadMatrix{f,d}・glMultMatrix{f,d}・glTranslate{f,d}・glRotate{f,d}・glScale{f,d}・
   glFrustum・glOrtho・glPushMatrix・glPopMatrix（modelview 32 段、projection・texture 4 段）。
2. 即時: glBegin・glEnd・glVertex{2,3,4}{f,d,i,s}{,v}・glColor{3,4}{f,d,ub}{,v}・glNormal3{f,d}{,v}・glTexCoord{1,2,3,4}f{,v}・glRectf。
3. 配列: glEnableClientState・glDisableClientState・glVertexPointer・glColorPointer・glNormalPointer・glTexCoordPointer。
4. 光源・material: glLight{f,i}{,v}・glLightModel{f,i}{,v}・glMaterial{f,i}{,v}・glColorMaterial・glShadeModel。
5. その他: glAlphaFunc・glTexEnv{f,i}{,v}・glPointSize・glClearDepth・glDepthRange・glPolygonMode（FILL だけ）・
   glDrawBuffer・glReadBuffer・glPushAttrib・glPopAttrib（記録しない）・glGetDoublev。display list: glGenLists・glNewList・
   glEndList・glCallList・glCallLists・glDeleteLists・glIsList。
6. header: GL/gl.h に GL 1.x の定数と関数。
7. zgears（`userland/X11/zgears`、自前の code）: 光源と display list と flat の歯車 3 つが回る。readback で赤・緑・青の歯車の画素が
   あり背景が黒いことを確かめ、fps を出す。

## 受け入れ

1. Venus の zwl＋Xzed（rootless）で zgears の歯車が光源つきで回り（画面の読み取り）、readback の確かめが通る。
2. glxtest（p004）・egltest の試験が変わらず通る。build warning 0、新しい C の style-check 0。
3. i915 実機は範囲外（ws069-p006）。
