#include "pipeline_state_fixture.h"
#include <cassert>
#include <thread>

using gfxvk::GraphicsPipelineState;
using pipeline_test::state;
static void check(GraphicsPipelineState& snapshot) {
  const auto* ci = snapshot.get();
  const auto* rc = static_cast<const VkPipelineRenderingCreateInfo*>(ci->pNext);
  assert(rc->colorAttachmentCount == 1 && rc->pColorAttachmentFormats[0] == VK_FORMAT_R8G8B8A8_UNORM);
  assert(ci->pVertexInputState->pVertexBindingDescriptions[0].stride == 24);
  assert(ci->pVertexInputState->pVertexAttributeDescriptions[0].offset == 8);
  assert(ci->pInputAssemblyState->primitiveRestartEnable);
  assert(ci->pRasterizationState->depthBiasConstantFactor == 2.5f);
  assert(ci->pDepthStencilState->front.compareMask == 0x57);
  assert(ci->pColorBlendState->pAttachments[0].colorWriteMask == 5);
  assert(ci->pDynamicState->pDynamicStates[0] == VK_DYNAMIC_STATE_VIEWPORT);
  assert(std::string_view(ci->pStages[0].pName) == "main");
}
int main() {
  auto original = state();
  check(original);
  auto copy = original;
  auto moved = std::move(copy);
  // All source stack arrays are already dead. Destroy the original's vectors
  // before the moved snapshot is consumed on a different thread.
  original = state();
  std::thread worker([snapshot = std::move(moved)]() mutable { check(snapshot); });
  worker.join();
  // Utility pipelines can omit depth/dynamic state and use static arrays.
  // Mutate the originals before consuming a copied snapshot on another thread.
  auto fixture = state();
  auto ci = *fixture.get();
  auto vp = *ci.pViewportState;
  auto ms = *ci.pMultisampleState;
  VkViewport staticViewport{1, 2, 640, 480, 0, 1};
  VkRect2D staticScissor{{3, 4}, {320, 240}};
  VkSampleMask masks[2] = {0x12345678, 0x87654321};
  vp.pViewports = &staticViewport; vp.pScissors = &staticScissor;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_64_BIT; ms.pSampleMask = masks;
  ci.pViewportState = &vp; ci.pMultisampleState = &ms;
  ci.pDepthStencilState = nullptr; ci.pDynamicState = nullptr;
  GraphicsPipelineState utility(ci);
  staticViewport.width = 0; staticScissor.extent.width = 0; masks[1] = 0;
  std::thread utilityWorker([snapshot = utility]() mutable {
    const auto* owned = snapshot.get();
    assert(!owned->pDepthStencilState && !owned->pDynamicState);
    assert(owned->pViewportState->pViewports[0].width == 640);
    assert(owned->pViewportState->pScissors[0].extent.width == 320);
    assert(owned->pMultisampleState->pSampleMask[1] == 0x87654321);
  });
  utilityWorker.join();
  // Classic pass snapshots retain borrowed compatibility handles and owned
  // portable target formats without exposing dynamic-rendering pNext to Vulkan.
  auto targets = fixture.rendering_targets();
  ci = *fixture.get(); ci.pNext = nullptr;
  ci.renderPass = reinterpret_cast<VkRenderPass>(uintptr_t(123));
  GraphicsPipelineState classic(ci, &targets);
  auto classicCopy = classic;
  assert(classicCopy.get()->renderPass == ci.renderPass && !classicCopy.get()->pNext);
  // (docs/VR_DLSS.md, stage D2b 2c) a motion twin: inputs again at + 16 on bindings + 16, and a motion target after
  // the colour attachments, never blended
  {
    auto motion = state();
    motion.add_motion_inputs(16, 32, 32);
    const uint32_t location = motion.add_motion_target(VK_FORMAT_R16G16_SFLOAT);
    assert(location == 1);
    const auto* m = motion.get();
    const auto* vi = m->pVertexInputState;
    assert(vi->vertexBindingDescriptionCount == 2 && vi->vertexAttributeDescriptionCount == 2);
    assert(vi->pVertexBindingDescriptions[1].binding == 19 && vi->pVertexBindingDescriptions[1].stride == 24 &&
           vi->pVertexBindingDescriptions[1].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE);
    assert(vi->pVertexAttributeDescriptions[1].location == 18 && vi->pVertexAttributeDescriptions[1].binding == 19 &&
           vi->pVertexAttributeDescriptions[1].offset == 8);
    const auto* rendering = static_cast<const VkPipelineRenderingCreateInfo*>(m->pNext);
    assert(rendering->colorAttachmentCount == 2 && rendering->pColorAttachmentFormats[1] == VK_FORMAT_R16G16_SFLOAT);
    assert(m->pColorBlendState->attachmentCount == 2 && !m->pColorBlendState->pAttachments[1].blendEnable &&
           m->pColorBlendState->pAttachments[1].colorWriteMask == (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT));
    assert(m->pColorBlendState->pAttachments[0].colorWriteMask == 5);  // the game's attachment untouched
    bool threw = false;
    try { auto tight = state(); tight.add_motion_inputs(16, 18, 32); } catch (const std::runtime_error&) { threw = true; }
    assert(threw);  // location 18 past a limit of 18
    threw = false;
    try { auto pass = classic; pass.add_motion_target(VK_FORMAT_R16G16_SFLOAT); } catch (const std::runtime_error&) { threw = true; }
    assert(threw);  // render-pass pipelines are refused
    // the motion pass's pipeline: the motion target only, depth tested equal and not written, no stencil, the
    // guest's raster and vertex input kept
    {
      auto mo = state();
      mo.make_motion_only(VK_FORMAT_R16G16_SFLOAT);
      mo.add_motion_inputs(16, 32, 32);
      const auto* p = mo.get();
      const auto* pr = static_cast<const VkPipelineRenderingCreateInfo*>(p->pNext);
      assert(pr->colorAttachmentCount == 1 && pr->pColorAttachmentFormats[0] == VK_FORMAT_R16G16_SFLOAT);
      assert(p->pColorBlendState->attachmentCount == 1 && !p->pColorBlendState->pAttachments[0].blendEnable);
      assert(p->pDepthStencilState->depthTestEnable && !p->pDepthStencilState->depthWriteEnable &&
             p->pDepthStencilState->depthCompareOp == VK_COMPARE_OP_EQUAL && !p->pDepthStencilState->stencilTestEnable);
      assert(p->pRasterizationState->depthBiasConstantFactor == 2.5f);  // the guest's raster state
      assert(p->pVertexInputState->vertexAttributeDescriptionCount == 2);
    }
    // after the shader's own outputs: a gap of unused attachments
    auto gap = state();
    assert(gap.add_motion_target(VK_FORMAT_R16G16_SFLOAT, 3) == 3);
    const auto* g = gap.get();
    const auto* gr = static_cast<const VkPipelineRenderingCreateInfo*>(g->pNext);
    assert(gr->colorAttachmentCount == 4 && gr->pColorAttachmentFormats[1] == VK_FORMAT_UNDEFINED &&
           gr->pColorAttachmentFormats[2] == VK_FORMAT_UNDEFINED && gr->pColorAttachmentFormats[3] == VK_FORMAT_R16G16_SFLOAT);
    assert(g->pColorBlendState->attachmentCount == 4 && g->pColorBlendState->pAttachments[2].colorWriteMask == 0);
  }
  assert(classicCopy.rendering_targets().pColorAttachmentFormats[0] == VK_FORMAT_R8G8B8A8_UNORM);
  GraphicsPipelineState classicUtility(ci);
  assert(classicUtility.get()->renderPass == ci.renderPass && !classicUtility.get()->pNext);
  VkGraphicsPipelineCreateInfo invalid{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  bool rejected = false;
  try { GraphicsPipelineState unsupported(invalid); }
  catch (const std::runtime_error&) { rejected = true; }
  assert(rejected);
}
