#pragma once
#include "loader.h"
#include <stdexcept>
#include <string_view>
#include <vector>

namespace gfxvk {
// Owns the CPU-side state consumed by vkCreateGraphicsPipelines. Vulkan handles
// (layout/modules) remain borrowed: the eventual job owner must retain those
// objects until creation finishes, and join jobs before device destruction.
// Accepts guest and utility states for dynamic rendering or compatible render passes. Unknown extension
// chains and specialization data are rejected rather than borrowed.
class GraphicsPipelineState {
  VkGraphicsPipelineCreateInfo info;
  VkPipelineRenderingCreateInfo rendering{};
  VkPipelineVertexInputStateCreateInfo vertex;
  VkPipelineInputAssemblyStateCreateInfo assembly;
  VkPipelineViewportStateCreateInfo viewport;
  VkPipelineRasterizationStateCreateInfo raster;
  VkPipelineMultisampleStateCreateInfo multisample;
  VkPipelineDepthStencilStateCreateInfo depth;
  VkPipelineColorBlendStateCreateInfo blend;
  VkPipelineDynamicStateCreateInfo dynamic;
  std::vector<VkPipelineShaderStageCreateInfo> stages;
  std::vector<VkVertexInputBindingDescription> bindings;
  std::vector<VkVertexInputAttributeDescription> attributes;
  std::vector<VkPipelineColorBlendAttachmentState> attachments;
  std::vector<VkDynamicState> dynamics;
  std::vector<VkFormat> formats;
  std::vector<VkViewport> viewports;
  std::vector<VkRect2D> scissors;
  std::vector<VkSampleMask> sampleMasks;
  bool hasDepth = false, hasDynamic = false;

  template<class T> static std::vector<T> copy(const T* data, uint32_t count) {
    if (!count) return {};
    if (!data) throw std::runtime_error("missing pipeline state array");
    return {data, data + count};
  }
public:
  // (docs/VR_MULTIVIEW.md, stage M4) a multiview twin of the pipeline draws the views of this mask
  void set_view_mask(uint32_t mask) { rendering.viewMask = mask; }
  // (docs/VR_DLSS.md, stage D2b 2c) a motion twin's vertex input: every attribute again at location + offset, read
  // from its binding + offset (the previous frame's vertex data: motion_glsl.h's inputs at + 16), same format, stride
  // and rate. Throws if a location or binding would pass the device's limits
  void add_motion_inputs(uint32_t offset, uint32_t maxAttributes, uint32_t maxBindings) {
    const size_t nb = bindings.size(), na = attributes.size();
    for (size_t i = 0; i < nb; ++i) {
      VkVertexInputBindingDescription b = bindings[i];
      b.binding += offset;
      if (b.binding >= maxBindings) throw std::runtime_error("motion vertex binding past the device's limit");
      bindings.push_back(b);
    }
    for (size_t i = 0; i < na; ++i) {
      VkVertexInputAttributeDescription a = attributes[i];
      a.location += offset;
      a.binding += offset;
      if (a.location >= maxAttributes) throw std::runtime_error("motion vertex attribute past the device's limit");
      attributes.push_back(a);
    }
    vertex.vertexBindingDescriptionCount = uint32_t(bindings.size());
    vertex.vertexAttributeDescriptionCount = uint32_t(attributes.size());
  }
  // a motion twin's motion target: one more colour attachment of `format`, never blended, its red and green written,
  // at the first location after the colour attachments and at or after `minLocation` (the pixel shader's own outputs:
  // a depth-only pass's shader may still write location 0); a gap is filled with unused attachments
  // (VK_FORMAT_UNDEFINED, nothing written). Returns its location. Dynamic rendering only
  uint32_t add_motion_target(VkFormat format, uint32_t minLocation = 0) {
    if (info.renderPass) throw std::runtime_error("a motion target needs dynamic rendering");
    if (formats.size() != attachments.size()) throw std::runtime_error("colour formats and blend attachments differ");
    while (formats.size() < minLocation) {
      formats.push_back(VK_FORMAT_UNDEFINED);
      attachments.push_back(VkPipelineColorBlendAttachmentState{});
    }
    const uint32_t location = uint32_t(formats.size());
    formats.push_back(format);
    VkPipelineColorBlendAttachmentState a{};
    a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    attachments.push_back(a);
    rendering.colorAttachmentCount = uint32_t(formats.size());
    blend.attachmentCount = uint32_t(attachments.size());
    return location;
  }
  // the motion pass's pipeline (docs/VR_DLSS.md, stage D2b: the motion pass): one colour attachment, the motion
  // target (never blended, red and green written); the scene's depth tested EQUAL and never written; no stencil test.
  // The vertex input, raster and depth format stay the guest pipeline's (the same positions, the same culling).
  // Dynamic rendering only; throws if the guest pipeline had no depth
  void make_motion_only(VkFormat motionFormat) {
    if (info.renderPass) throw std::runtime_error("the motion pass needs dynamic rendering");
    if (!hasDepth || rendering.depthAttachmentFormat == VK_FORMAT_UNDEFINED)
      throw std::runtime_error("a pipeline without depth has no motion");
    formats.assign(1, motionFormat);
    VkPipelineColorBlendAttachmentState a{};
    a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    attachments.assign(1, a);
    rendering.colorAttachmentCount = 1;
    rendering.viewMask = 0;
    blend.attachmentCount = 1;
    blend.logicOpEnable = VK_FALSE;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_EQUAL;
    depth.stencilTestEnable = VK_FALSE;
    depth.depthBoundsTestEnable = VK_FALSE;
    multisample.alphaToCoverageEnable = VK_FALSE;
    // (the guest's dynamic blend constants and stencil reference stay declared: harmless with neither used)
  }
  // Mutate only the owned stage array; module lifetime belongs to the job owner.
  void set_stage_module(uint32_t index, VkShaderModule module) {
    if(index >= stages.size()) throw std::out_of_range("pipeline shader stage index");
    if(!module) throw std::invalid_argument("null pipeline shader module");
    stages[index].module = module;
  }
  explicit GraphicsPipelineState(const VkGraphicsPipelineCreateInfo& source,
      const VkPipelineRenderingCreateInfo* targetMetadata = nullptr)
      : info(source) {
    if (source.sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO ||
        (!source.pNext && !source.renderPass) || !source.pVertexInputState || !source.pInputAssemblyState ||
        !source.pViewportState || !source.pRasterizationState ||
        !source.pMultisampleState ||
        !source.pColorBlendState || source.pTessellationState)
      throw std::runtime_error("unsupported pipeline state snapshot");
    if (source.pNext && static_cast<const VkBaseInStructure*>(source.pNext)->sType !=
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)
      throw std::runtime_error("unsupported pipeline snapshot extension chain");
    rendering = source.pNext ? *static_cast<const VkPipelineRenderingCreateInfo*>(source.pNext) :
        targetMetadata ? *targetMetadata : VkPipelineRenderingCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    vertex = *source.pVertexInputState;
    assembly = *source.pInputAssemblyState;
    viewport = *source.pViewportState;
    raster = *source.pRasterizationState;
    multisample = *source.pMultisampleState;
    hasDepth = source.pDepthStencilState != nullptr;
    depth = hasDepth ? *source.pDepthStencilState : VkPipelineDepthStencilStateCreateInfo{};
    blend = *source.pColorBlendState;
    hasDynamic = source.pDynamicState != nullptr;
    dynamic = hasDynamic ? *source.pDynamicState : VkPipelineDynamicStateCreateInfo{};
    if (rendering.sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO ||
        rendering.pNext || vertex.pNext || assembly.pNext || viewport.pNext ||
        raster.pNext || multisample.pNext || depth.pNext || blend.pNext || dynamic.pNext)
      throw std::runtime_error("unsupported pipeline snapshot extension");
    if (viewport.pViewports) viewports = copy(viewport.pViewports, viewport.viewportCount);
    if (viewport.pScissors) scissors = copy(viewport.pScissors, viewport.scissorCount);
    if (multisample.pSampleMask) {
      const auto samples = uint32_t(multisample.rasterizationSamples);
      if (!samples || samples > 64 || (samples & (samples - 1)))
        throw std::runtime_error("invalid pipeline sample count");
      sampleMasks = copy(multisample.pSampleMask, (samples + 31) / 32);
    }
    stages = copy(source.pStages, source.stageCount);
    for (auto& stage : stages) {
      if (stage.pNext || stage.pSpecializationInfo || !stage.pName ||
          std::string_view(stage.pName) != "main")
        throw std::runtime_error("unsupported pipeline shader snapshot");
      stage.pName = "main";
    }
    bindings = copy(vertex.pVertexBindingDescriptions, vertex.vertexBindingDescriptionCount);
    attributes = copy(vertex.pVertexAttributeDescriptions, vertex.vertexAttributeDescriptionCount);
    attachments = copy(blend.pAttachments, blend.attachmentCount);
    dynamics = copy(dynamic.pDynamicStates, dynamic.dynamicStateCount);
    formats = copy(rendering.pColorAttachmentFormats, rendering.colorAttachmentCount);
  }

  // Portable recipes retain formats, never process-local render-pass handles.
  // Metadata remains available even when the Vulkan create info has no pNext.
  const VkPipelineRenderingCreateInfo& rendering_targets() {
    rendering.pColorAttachmentFormats = formats.data();
    return rendering;
  }

  // Returned pointers are valid until this snapshot is moved or destroyed.
  // Rebind on every use so default copy/move cannot retain pointers into a source.
  const VkGraphicsPipelineCreateInfo* get() {
    rendering.pColorAttachmentFormats = formats.data();
    vertex.pVertexBindingDescriptions = bindings.data();
    vertex.pVertexAttributeDescriptions = attributes.data();
    blend.pAttachments = attachments.data();
    dynamic.pDynamicStates = dynamics.data();
    viewport.pViewports = viewports.empty() ? nullptr : viewports.data();
    viewport.pScissors = scissors.empty() ? nullptr : scissors.data();
    multisample.pSampleMask = sampleMasks.empty() ? nullptr : sampleMasks.data();
    info.pNext = info.renderPass ? nullptr : &rendering;
    info.pStages = stages.data();
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = hasDepth ? &depth : nullptr;
    info.pColorBlendState = &blend;
    info.pDynamicState = hasDynamic ? &dynamic : nullptr;
    return &info;
  }
};
}
