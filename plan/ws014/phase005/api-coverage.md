# p005で確認するVulkan APIとU/K境界

この表は有限の `vkdemo` が使う追加の3D描画経路を記録する。実行の成否は [結果](results.md) を参照する。Venus wire format 1、virglrenderer 1.1.0の実dispatchを対象とし、汎用libvulkan.soの公開関数実装を完了したという表ではない。

| Vulkan操作 | wire command | Uの実装・確認対象 | Kで使う既存機能 |
| --- | --- | --- | --- |
| vkGetPhysicalDeviceFormatProperties | 4 | RGBA8のsample/color/transfer、D32のdepth対応確認 | command/replyの転送 |
| vkCreateImageView | 57 | color、depth、textureの2D view | command転送 |
| vkCreateShaderModule | 59 | 自作GLSLから生成・検証したvertex/fragment SPIR-V | command転送 |
| vkCreateGraphicsPipelines | 65 | VS/FS、頂点入力、viewport、rasterization、depth、color出力 | command転送 |
| vkCreatePipelineLayout | 68 | textureのdescriptor setと時刻push constant | command転送 |
| vkCreateSampler | 70 | nearest filteringとclamp | command転送 |
| vkCreateDescriptorSetLayout | 72 | fragment shaderのcombined image sampler binding | command転送 |
| vkCreateDescriptorPool | 74 | 一つのtexture descriptorの所有 | command転送 |
| vkAllocateDescriptorSets | 77 | descriptor setの割当て | command/replyの転送 |
| vkUpdateDescriptorSets | 79 | sampler、view、texture layoutの関連付け | command転送 |
| vkCreateFramebuffer | 80 | 320x240 color/depth attachment | command転送 |
| vkCreateRenderPass | 82 | color/depthのclear・描画・保存とlayout | command転送 |
| vkResetFences | 37 | 前frame完了後のfence再利用 | command転送 |
| vkResetCommandPool | 87 | 前frame完了後のcommand buffer再記録 | command転送 |
| vkCmdBindPipeline | 93 | graphics pipeline選択 | command転送 |
| vkCmdBindDescriptorSets | 103 | textureをshaderへ接続 | command転送 |
| vkCmdBindVertexBuffers | 105 | 36頂点のposition/UV buffer | command転送 |
| vkCmdDraw | 106 | 12三角形、6面の非等辺直方体 | command転送 |
| vkCmdCopyBufferToImage | 115 | 元textureのupload | 共有blob writeとcommand転送 |
| vkCmdPushConstants | 132 | 単調時計由来の秒数をvertex shaderへ渡す | command転送 |
| vkCmdBeginRenderPass / vkCmdEndRenderPass | 133 / 135 | depth付き3D描画の記録範囲 | command転送 |

p003からinstance/device/queue作成、memory/buffer/image、VkFence、command pool/buffer、barrier、submit、image-to-buffer copyを再利用する。Vk objectの破棄もUが符号化し、Kはその関数名や構造体を解釈しない。queueの取得は対象rendererが実装する `vkGetDeviceQueue2` を使う。

Uはshader、scene、Vulkan objectと状態遷移、wire encoding、VkResult、GPU fenceの完了、画像hashを扱う。共有clientはcallerが所有するsession構造体にfd・reply・wire stateを保持する。Kは既存のGPU session、opaque resource handle、capset、blob、所有権検証、有限copy、command転送、scanoutを扱う。

GPU coreのhandleは返信blob、upload blob、readback blob、presentation storageの4個を使用する。Vk object IDとは別の名前空間であり、Vk object数がそのままcoreの32slotを消費するわけではない。upload/readbackのVkDeviceMemory exportは各一度だけ行い、frameごとに追加しない。

frameはfence完了後にreadbackしてRGB hashを取り、同じ画素をpresentation storageへcopyする。これはCPUによる3D描画ではなく、GPU描画後のreadbackとcopyによる表示経路。zero-copy、汎用swapchain、device mmap、vblank/present完了通知、全Vulkan APIの適合性は別の未実装範囲としてp004へ引き渡す。
