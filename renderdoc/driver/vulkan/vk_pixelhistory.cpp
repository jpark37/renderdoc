/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2020 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include <float.h>
#include "driver/shaders/spirv/spirv_editor.h"
#include "driver/shaders/spirv/spirv_op_helpers.h"
#include "maths/formatpacking.h"
#include "vk_debug.h"
#include "vk_replay.h"
#include "vk_shader_cache.h"

bool isDirectWrite(ResourceUsage usage)
{
  return ((usage >= ResourceUsage::VS_RWResource && usage <= ResourceUsage::CS_RWResource) ||
          usage == ResourceUsage::CopyDst || usage == ResourceUsage::Copy ||
          usage == ResourceUsage::Resolve || usage == ResourceUsage::ResolveDst ||
          usage == ResourceUsage::GenMips);
}

enum
{
  TestEnabled_Culling = 1 << 0,
  TestEnabled_Scissor = 1 << 1,
  TestEnabled_SampleMask = 1 << 2,
  TestEnabled_DepthBounds = 1 << 3,
  TestEnabled_StencilTesting = 1 << 4,
  TestEnabled_DepthTesting = 1 << 5,
  TestEnabled_FragmentDiscard = 1 << 6,

  Blending_Enabled = 1 << 7,
  UnboundFragmentShader = 1 << 8,
  TestMustFail_Culling = 1 << 9,
  TestMustFail_Scissor = 1 << 10,
  TestMustPass_Scissor = 1 << 11,
  TestMustFail_DepthTesting = 1 << 12,
  TestMustFail_StencilTesting = 1 << 13,
  TestMustFail_SampleMask = 1 << 14,
};

struct CopyPixelParams
{
  bool depthCopy;
  bool stencilOnly;
  VkImage srcImage;
  VkFormat srcImageFormat;
  VkImageLayout srcImageLayout;
};

struct PixelHistoryResources
{
  VkBuffer dstBuffer;
  VkDeviceMemory bufferMemory;

  // Used for offscreen rendering for draw call events.
  VkImage colorImage;
  VkImageView colorImageView;
  VkImage stencilImage;
  VkImageView stencilImageView;
  VkDeviceMemory gpuMem;

  // Following are only used and created for multi sampled images.
  VkImage stagingImage;
  VkImageView stagingImageView;
  VkImage stencilStagingImage;
  VkImageView stencilStagingImageView;
  VkImageView targetImageView;
};

struct PixelHistoryCallbackInfo
{
  // Original image for which pixel history is requested.
  VkImage targetImage;
  // Information about the original target image.
  VkFormat targetImageFormat;
  uint32_t layers;
  uint32_t mipLevels;
  VkSampleCountFlagBits samples;
  VkExtent3D extent;
  // Information about the location of the pixel for which history was requested.
  Subresource targetSubresource;
  uint32_t x;
  uint32_t y;
  uint32_t sampleMask;

  // Image used to get per fragment data.
  VkImage subImage;
  VkImageView subImageView;

  // Image used to get stencil counts.
  VkImage stencilImage;
  VkImageView stencilImageView;

  // Only used for multi sampled images for copy into a staging resource.
  VkImage stagingImage;
  VkImage stencilStagingImage;

  // Buffer used to copy colour and depth information
  VkBuffer dstBuffer;
};

struct PixelHistoryValue
{
  // Max size is 4 component with 8 byte component width
  uint8_t color[32];
  union
  {
    uint32_t udepth;
    float fdepth;
  } depth;
  int8_t stencil;
  uint8_t padding[3];
};

struct EventInfo
{
  PixelHistoryValue premod;
  PixelHistoryValue postmod;
  uint8_t dsWithoutShaderDiscard[8];
  uint8_t dsWithShaderDiscard[8];
};

struct PerFragmentInfo
{
  // primitive ID is copied from a R32G32B32A32 texture.
  int32_t primitiveID;
  uint32_t padding[3];
  PixelHistoryValue shaderOut;
  PixelHistoryValue postMod;
};

struct PipelineReplacements
{
  VkPipeline fixedShaderStencil;
  VkPipeline originalShaderStencil;
};

// PixelHistoryShaderCache manages temporary shaders created for pixel history.
struct PixelHistoryShaderCache
{
  PixelHistoryShaderCache(WrappedVulkan *vk) : m_pDriver(vk) {}
  ~PixelHistoryShaderCache()
  {
    for(auto it = m_ShaderReplacements.begin(); it != m_ShaderReplacements.end(); ++it)
    {
      if(it->second != VK_NULL_HANDLE)
        m_pDriver->vkDestroyShaderModule(m_pDriver->GetDev(), it->second, NULL);
    }
    for(auto it = m_FixedColFS.begin(); it != m_FixedColFS.end(); it++)
      m_pDriver->vkDestroyShaderModule(m_pDriver->GetDev(), it->second, NULL);
    for(auto it = m_PrimIDFS.begin(); it != m_PrimIDFS.end(); it++)
      m_pDriver->vkDestroyShaderModule(m_pDriver->GetDev(), it->second, NULL);
  }

  // Returns a fragment shader module that outputs a fixed color to the given
  // color attachment.
  VkShaderModule GetFixedColShader(uint32_t framebufferIndex)
  {
    auto it = m_FixedColFS.find(framebufferIndex);
    if(it != m_FixedColFS.end())
      return it->second;
    VkShaderModule sh;
    m_pDriver->GetDebugManager()->PatchOutputLocation(sh, BuiltinShader::FixedColFS,
                                                      framebufferIndex);
    m_FixedColFS.insert(std::make_pair(framebufferIndex, sh));
    return sh;
  }

  // Returns a fragment shader module that outputs primitive ID to the given
  // color attachment.
  VkShaderModule GetPrimitiveIdShader(uint32_t framebufferIndex)
  {
    auto it = m_PrimIDFS.find(framebufferIndex);
    if(it != m_PrimIDFS.end())
      return it->second;
    VkShaderModule sh;
    m_pDriver->GetDebugManager()->PatchOutputLocation(sh, BuiltinShader::PixelHistoryPrimIDFS,
                                                      framebufferIndex);
    m_PrimIDFS.insert(std::make_pair(framebufferIndex, sh));
    return sh;
  }

  // Returns a shader that is equivalent to the given shader, but attempts to remove
  // side effects of shader execution for the given entry point (for ex., writes
  // to storage buffers/images).
  VkShaderModule GetShaderWithoutSideEffects(ResourceId shaderId, const rdcstr &entryPoint)
  {
    ShaderKey shaderKey = make_rdcpair(shaderId, entryPoint);
    auto it = m_ShaderReplacements.find(shaderKey);
    // Check if we processed this shader before.
    if(it != m_ShaderReplacements.end())
      return it->second;

    VkShaderModule shaderModule = CreateShaderReplacement(shaderId, entryPoint);
    m_ShaderReplacements.insert(std::make_pair(shaderKey, shaderModule));
    return shaderModule;
  }

private:
  VkShaderModule CreateShaderReplacement(ResourceId shaderId, const rdcstr &entryName)
  {
    const VulkanCreationInfo::ShaderModule &moduleInfo =
        m_pDriver->GetDebugManager()->GetShaderInfo(shaderId);
    rdcarray<uint32_t> modSpirv = moduleInfo.spirv.GetSPIRV();
    rdcspv::Editor editor(modSpirv);
    editor.Prepare();

    for(const rdcspv::EntryPoint &entry : editor.GetEntries())
    {
      if(entry.name == entryName)
      {
        // In some cases a shader might just be binding a RW resource but not writing to it.
        // If there are no writes (shader was not modified), no need to replace the shader,
        // just insert VK_NULL_HANDLE to indicate that this shader has been processed.
        VkShaderModule module = VK_NULL_HANDLE;
        bool modified = StripShaderSideEffects(editor, entry.id);
        if(modified)
        {
          VkShaderModuleCreateInfo moduleCreateInfo = {};
          moduleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
          moduleCreateInfo.pCode = modSpirv.data();
          moduleCreateInfo.codeSize = modSpirv.byteSize();
          VkResult vkr =
              m_pDriver->vkCreateShaderModule(m_pDriver->GetDev(), &moduleCreateInfo, NULL, &module);
          RDCASSERTEQUAL(vkr, VK_SUCCESS);
        }
        return module;
      }
    }
    RDCERR("Entry point %s not found", entryName.c_str());
    return VK_NULL_HANDLE;
  }

  // Removes instructions from the shader that would produce side effects (writing
  // to storage buffers, or images). Returns true if the shader was modified, and
  // false if there were no instructions to remove.
  bool StripShaderSideEffects(rdcspv::Editor &editor, const rdcspv::Id &entryId)
  {
    bool modified = false;

    std::set<rdcspv::Id> patchedFunctions;
    std::set<rdcspv::Id> functionPatchQueue;
    functionPatchQueue.insert(entryId);

    while(!functionPatchQueue.empty())
    {
      rdcspv::Id funcId;
      {
        auto it = functionPatchQueue.begin();
        funcId = *functionPatchQueue.begin();
        functionPatchQueue.erase(it);
        patchedFunctions.insert(funcId);
      }

      rdcspv::Iter it = editor.GetID(funcId);
      RDCASSERT(it.opcode() == rdcspv::Op::Function);

      it++;

      for(; it; ++it)
      {
        rdcspv::Op opcode = it.opcode();
        if(opcode == rdcspv::Op::FunctionEnd)
          break;

        switch(opcode)
        {
          case rdcspv::Op::FunctionCall:
          {
            rdcspv::OpFunctionCall call(it);
            if(functionPatchQueue.find(call.function) == functionPatchQueue.end() &&
               patchedFunctions.find(call.function) == patchedFunctions.end())
              functionPatchQueue.insert(call.function);
            break;
          }
          case rdcspv::Op::CopyMemory:
          case rdcspv::Op::AtomicStore:
          case rdcspv::Op::Store:
          {
            rdcspv::Id pointer = rdcspv::Id::fromWord(it.word(1));
            rdcspv::Id pointerType = editor.GetIDType(pointer);
            RDCASSERT(pointerType != rdcspv::Id());
            rdcspv::Iter pointerTypeIt = editor.GetID(pointerType);
            rdcspv::OpTypePointer ptr(pointerTypeIt);
            if(ptr.storageClass == rdcspv::StorageClass::Uniform ||
               ptr.storageClass == rdcspv::StorageClass::StorageBuffer)
            {
              editor.Remove(it);
              modified = true;
            }
            break;
          }
          case rdcspv::Op::ImageWrite:
          {
            editor.Remove(it);
            modified = true;
            break;
          }
          case rdcspv::Op::AtomicExchange:
          case rdcspv::Op::AtomicCompareExchange:
          case rdcspv::Op::AtomicCompareExchangeWeak:
          case rdcspv::Op::AtomicIIncrement:
          case rdcspv::Op::AtomicIDecrement:
          case rdcspv::Op::AtomicIAdd:
          case rdcspv::Op::AtomicISub:
          case rdcspv::Op::AtomicSMin:
          case rdcspv::Op::AtomicUMin:
          case rdcspv::Op::AtomicSMax:
          case rdcspv::Op::AtomicUMax:
          case rdcspv::Op::AtomicAnd:
          case rdcspv::Op::AtomicOr:
          case rdcspv::Op::AtomicXor:
          {
            rdcspv::IdResultType resultType = rdcspv::IdResultType::fromWord(it.word(1));
            rdcspv::IdResult result = rdcspv::IdResult::fromWord(it.word(2));
            rdcspv::Id pointer = rdcspv::Id::fromWord(it.word(3));
            rdcspv::IdScope memory = rdcspv::IdScope::fromWord(it.word(4));
            rdcspv::IdMemorySemantics semantics = rdcspv::IdMemorySemantics::fromWord(it.word(5));
            editor.Remove(it);
            // All of these instructions produce a result ID that is the original
            // value stored at the pointer. Since we removed the original instruction
            // we replace it with an OpAtomicLoad in case the result ID is used.
            // This is currently best effort and might be incorrect in some cases
            // (for ex. if shader invocations need to see the updated value).
            editor.AddOperation(
                it, rdcspv::OpAtomicLoad(resultType, result, pointer, memory, semantics));
            modified = true;
            break;
          }
          default: break;
        }
      }
    }
    return modified;
  }

  WrappedVulkan *m_pDriver;
  std::map<uint32_t, VkShaderModule> m_FixedColFS;
  std::map<uint32_t, VkShaderModule> m_PrimIDFS;

  // ShaderKey consists of original shader module ID and entry point name.
  typedef rdcpair<ResourceId, rdcstr> ShaderKey;
  std::map<ShaderKey, VkShaderModule> m_ShaderReplacements;
};

// VulkanPixelHistoryCallback is a generic VulkanDrawcallCallback that can be used for
// pixel history replays.
struct VulkanPixelHistoryCallback : public VulkanDrawcallCallback
{
  VulkanPixelHistoryCallback(WrappedVulkan *vk, PixelHistoryShaderCache *shaderCache,
                             const PixelHistoryCallbackInfo &callbackInfo, VkQueryPool occlusionPool)
      : m_pDriver(vk),
        m_ShaderCache(shaderCache),
        m_CallbackInfo(callbackInfo),
        m_OcclusionPool(occlusionPool)
  {
    m_pDriver->SetDrawcallCB(this);
  }

  ~VulkanPixelHistoryCallback()
  {
    m_pDriver->SetDrawcallCB(NULL);
    for(const VkRenderPass &rp : m_RpsToDestroy)
      m_pDriver->vkDestroyRenderPass(m_pDriver->GetDev(), rp, NULL);
    for(const VkFramebuffer &fb : m_FbsToDestroy)
      m_pDriver->vkDestroyFramebuffer(m_pDriver->GetDev(), fb, NULL);
  }
  // Update the given scissor to just the pixel for which pixel history was requested.
  void ScissorToPixel(const VkViewport &view, VkRect2D &scissor)
  {
    float fx = (float)m_CallbackInfo.x;
    float fy = (float)m_CallbackInfo.y;
    float y_start = view.y;
    float y_end = view.y + view.height;
    if(view.height < 0)
    {
      y_start = view.y + view.height;
      y_end = view.y;
    }

    if(fx < view.x || fy < y_start || fx >= view.x + view.width || fy >= y_end)
    {
      scissor.offset.x = scissor.offset.y = scissor.extent.width = scissor.extent.height = 0;
    }
    else
    {
      scissor.offset.x = m_CallbackInfo.x;
      scissor.offset.y = m_CallbackInfo.y;
      scissor.extent.width = scissor.extent.height = 1;
    }
  }

  // Intersects the originalScissor and newScissor and writes intersection to the newScissor.
  // newScissor always covers a single pixel, so if originalScissor does not touch that pixel
  // returns an empty scissor.
  void IntersectScissors(const VkRect2D &originalScissor, VkRect2D &newScissor)
  {
    RDCASSERT(newScissor.extent.height == 1);
    RDCASSERT(newScissor.extent.width == 1);
    if(originalScissor.offset.x > newScissor.offset.x ||
       originalScissor.offset.x + originalScissor.extent.width <
           newScissor.offset.x + newScissor.extent.width ||
       originalScissor.offset.y > newScissor.offset.y ||
       originalScissor.offset.y + originalScissor.extent.height <
           newScissor.offset.y + newScissor.extent.height)
    {
      // scissor does not touch our target pixel, make it empty
      newScissor.offset.x = newScissor.offset.y = newScissor.extent.width =
          newScissor.extent.height = 0;
    }
  }

protected:
  // MakeAllPassIncrementStencilPipelineCI fills in the provided pipeCreateInfo
  // to create a graphics pipeline that is based on the original. The modifications
  // to the original pipeline: disables all tests except stencil, stencil is set
  // to always pass and increment, scissor is set to scissor around target pixel,
  // all shaders are replaced with their "clean" versions (attempts to remove side
  // effects).
  void MakeAllPassIncrementStencilPipelineCI(uint32_t eid, ResourceId pipe,
                                             VkGraphicsPipelineCreateInfo &pipeCreateInfo,
                                             rdcarray<VkPipelineShaderStageCreateInfo> &stages)
  {
    const VulkanCreationInfo::Pipeline &p = m_pDriver->GetDebugManager()->GetPipelineInfo(pipe);
    m_pDriver->GetShaderCache()->MakeGraphicsPipelineInfo(pipeCreateInfo, pipe);

    VkPipelineRasterizationStateCreateInfo *rs =
        (VkPipelineRasterizationStateCreateInfo *)pipeCreateInfo.pRasterizationState;
    VkPipelineDepthStencilStateCreateInfo *ds =
        (VkPipelineDepthStencilStateCreateInfo *)pipeCreateInfo.pDepthStencilState;
    VkPipelineMultisampleStateCreateInfo *ms =
        (VkPipelineMultisampleStateCreateInfo *)pipeCreateInfo.pMultisampleState;
    VkPipelineViewportStateCreateInfo *vs =
        (VkPipelineViewportStateCreateInfo *)pipeCreateInfo.pViewportState;

    VkRect2D newScissors[16];
    memset(newScissors, 0, sizeof(newScissors));
    // Turn off all tests, except stencil which is set to always pass
    // and increment.
    {
      rs->cullMode = VK_CULL_MODE_NONE;
      rs->rasterizerDiscardEnable = VK_FALSE;
      ds->depthTestEnable = VK_FALSE;
      ds->depthWriteEnable = VK_FALSE;
      ds->depthBoundsTestEnable = VK_FALSE;
      if(m_pDriver->GetDeviceFeatures().depthClamp)
        rs->depthClampEnable = true;

      ds->stencilTestEnable = VK_TRUE;
      ds->front.compareOp = VK_COMPARE_OP_ALWAYS;
      ds->front.failOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
      ds->front.passOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
      ds->front.depthFailOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
      ds->front.compareMask = 0xff;
      ds->front.writeMask = 0xff;
      ds->front.reference = 0;
      ds->back = ds->front;

      ms->pSampleMask = &m_CallbackInfo.sampleMask;

      // Change scissors unless they are set dynamically.
      if(p.dynamicStates[VkDynamicScissor])
      {
        VulkanRenderState &pipestate = m_pDriver->GetCmdRenderState();
        for(uint32_t i = 0; i < pipestate.views.size(); i++)
          ScissorToPixel(pipestate.views[i], pipestate.scissors[i]);
      }
      else
      {
        for(uint32_t i = 0; i < vs->viewportCount; i++)
        {
          ScissorToPixel(vs->pViewports[i], newScissors[i]);
        }
        vs->pScissors = newScissors;
      }
    }

    // TODO: this is wrong, should take into account subpass.
    pipeCreateInfo.subpass = 0;

    stages.resize(pipeCreateInfo.stageCount);
    memcpy(stages.data(), pipeCreateInfo.pStages, stages.byteSize());

    EventFlags eventFlags = m_pDriver->GetEventFlags(eid);
    VkShaderModule replacementShaders[5] = {};

    // Clean shaders
    uint32_t numberOfStages = 5;
    for(size_t i = 0; i < numberOfStages; i++)
    {
      if((eventFlags & PipeStageRWEventFlags(StageFromIndex(i))) != EventFlags::NoFlags)
        replacementShaders[i] =
            m_ShaderCache->GetShaderWithoutSideEffects(p.shaders[i].module, p.shaders[i].entryPoint);
    }
    for(uint32_t i = 0; i < pipeCreateInfo.stageCount; i++)
    {
      VkShaderModule replacement = replacementShaders[StageIndex(stages[i].stage)];
      if(replacement != VK_NULL_HANDLE)
        stages[i].module = replacement;
    }
    pipeCreateInfo.pStages = stages.data();
  }

  // CreateRenderPass creates a new VkRenderPass based on the original that has a separate
  // depth-stencil attachment, and covers a single subpass. This will be used to replay
  // a single draw. The new renderpass also replaces the depth stencil attachment, so
  // it can be used to count the number of fragments. Optionally, the new renderpass
  // changes the format for the color image that corresponds to subImage.
  VkRenderPass CreateRenderPass(ResourceId rp, ResourceId fb, uint32_t subpassIdx,
                                VkImage subImage = VK_NULL_HANDLE,
                                VkFormat newFormat = VK_FORMAT_UNDEFINED)
  {
    const VulkanCreationInfo::RenderPass &rpInfo =
        m_pDriver->GetDebugManager()->GetRenderPassInfo(rp);
    const VulkanCreationInfo::RenderPass::Subpass &sub = rpInfo.subpasses[subpassIdx];

    // Copy color and input attachments, and ignore resolve attachments.
    // Since we are only using this renderpass to replay a single draw, we don't
    // need to do resolve operations.
    rdcarray<VkAttachmentReference> colorAttachments(sub.colorAttachments.size());
    rdcarray<VkAttachmentReference> inputAttachments(sub.inputAttachments.size());

    for(size_t i = 0; i < sub.colorAttachments.size(); i++)
    {
      colorAttachments[i].attachment = sub.colorAttachments[i];
      colorAttachments[i].layout = sub.colorLayouts[i];
    }
    for(size_t i = 0; i < sub.inputAttachments.size(); i++)
    {
      inputAttachments[i].attachment = sub.inputAttachments[i];
      inputAttachments[i].layout = sub.inputLayouts[i];
    }

    VkSubpassDescription subpassDesc = {};
    subpassDesc.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpassDesc.inputAttachmentCount = (uint32_t)sub.inputAttachments.size();
    subpassDesc.pInputAttachments = inputAttachments.data();
    subpassDesc.colorAttachmentCount = (uint32_t)sub.colorAttachments.size();
    subpassDesc.pColorAttachments = colorAttachments.data();

    rdcarray<VkAttachmentDescription> descs(rpInfo.attachments.size());
    for(uint32_t i = 0; i < rpInfo.attachments.size(); i++)
    {
      descs[i] = {};
      descs[i].flags = rpInfo.attachments[i].flags;
      descs[i].format = rpInfo.attachments[i].format;
      descs[i].samples = rpInfo.attachments[i].samples;
      descs[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      descs[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      descs[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      descs[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      descs[i].initialLayout = rpInfo.attachments[i].initialLayout;
      descs[i].finalLayout = rpInfo.attachments[i].finalLayout;
    }
    for(uint32_t a = 0; a < subpassDesc.colorAttachmentCount; a++)
    {
      if(subpassDesc.pColorAttachments[a].attachment != VK_ATTACHMENT_UNUSED)
      {
        descs[subpassDesc.pColorAttachments[a].attachment].initialLayout =
            descs[subpassDesc.pColorAttachments[a].attachment].finalLayout =
                subpassDesc.pColorAttachments[a].layout;
      }
    }

    for(uint32_t a = 0; a < subpassDesc.inputAttachmentCount; a++)
    {
      if(subpassDesc.pInputAttachments[a].attachment != VK_ATTACHMENT_UNUSED)
      {
        descs[subpassDesc.pInputAttachments[a].attachment].initialLayout =
            descs[subpassDesc.pInputAttachments[a].attachment].finalLayout =
                subpassDesc.pInputAttachments[a].layout;
      }
    }

    VkAttachmentDescription dsAtt = {};
    dsAtt.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    dsAtt.samples = m_CallbackInfo.samples;
    dsAtt.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    dsAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    dsAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    dsAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    dsAtt.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    dsAtt.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // If there is already a depth stencil attachment, substitute it.
    // Otherwise, add it at the end of all attachments.
    VkAttachmentReference dsAttachment = {};
    dsAttachment.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if(sub.depthstencilAttachment != -1)
    {
      descs[sub.depthstencilAttachment] = dsAtt;
      dsAttachment.attachment = sub.depthstencilAttachment;
    }
    else
    {
      descs.push_back(dsAtt);
      dsAttachment.attachment = (uint32_t)rpInfo.attachments.size();
    }
    subpassDesc.pDepthStencilAttachment = &dsAttachment;

    // If needed substitute the color attachment with the new format.
    const VulkanCreationInfo::Framebuffer &fbInfo =
        m_pDriver->GetDebugManager()->GetFramebufferInfo(fb);
    for(uint32_t i = 0; i < fbInfo.attachments.size(); i++)
    {
      if(m_pDriver->GetDebugManager()->GetImageViewInfo(fbInfo.attachments[i].createdView).image ==
         GetResID(subImage))
        descs[i].format = newFormat;
    }

    VkRenderPassCreateInfo rpCreateInfo = {};
    rpCreateInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpCreateInfo.attachmentCount = (uint32_t)descs.size();
    rpCreateInfo.subpassCount = 1;
    rpCreateInfo.pSubpasses = &subpassDesc;
    rpCreateInfo.pAttachments = descs.data();
    rpCreateInfo.dependencyCount = 0;
    rpCreateInfo.pDependencies = NULL;

    VkRenderPass renderpass;
    VkResult vkr =
        m_pDriver->vkCreateRenderPass(m_pDriver->GetDev(), &rpCreateInfo, NULL, &renderpass);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);
    m_RpsToDestroy.push_back(renderpass);
    return renderpass;
  }

  // CreateFrambuffer creates a new VkFramebuffer that is based on the original, but
  // substitutes the depth stencil image view. If there is no depth stencil attachment,
  // it will be added. Optionally, also substitutes the original target image view.
  VkFramebuffer CreateFramebuffer(ResourceId rp, VkRenderPass newRp, uint32_t subpassIndex,
                                  ResourceId origFb, VkImageView newDsImageView,
                                  VkImageView newImageView = VK_NULL_HANDLE)
  {
    const VulkanCreationInfo::RenderPass &rpInfo =
        m_pDriver->GetDebugManager()->GetRenderPassInfo(rp);
    const VulkanCreationInfo::RenderPass::Subpass &sub = rpInfo.subpasses[subpassIndex];
    const VulkanCreationInfo::Framebuffer &fbInfo =
        m_pDriver->GetDebugManager()->GetFramebufferInfo(origFb);
    rdcarray<VkImageView> atts(fbInfo.attachments.size());

    for(uint32_t i = 0; i < fbInfo.attachments.size(); i++)
    {
      atts[i] = m_pDriver->GetResourceManager()->GetCurrentHandle<VkImageView>(
          fbInfo.attachments[i].createdView);
      if(newImageView != VK_NULL_HANDLE)
      {
        ResourceId img =
            m_pDriver->GetDebugManager()->GetImageViewInfo(fbInfo.attachments[i].createdView).image;
        if(img == GetResID(m_CallbackInfo.targetImage))
          atts[i] = newImageView;
      }
    }

    if(sub.depthstencilAttachment != -1)
      atts[sub.depthstencilAttachment] = newDsImageView;
    else
      atts.push_back(newDsImageView);
    VkFramebufferCreateInfo fbCI = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbCI.renderPass = newRp;
    fbCI.attachmentCount = (uint32_t)atts.size();
    fbCI.pAttachments = atts.data();
    fbCI.width = fbInfo.width;
    fbCI.height = fbInfo.height;
    fbCI.layers = fbInfo.layers;

    VkFramebuffer framebuffer;
    VkResult vkr = m_pDriver->vkCreateFramebuffer(m_pDriver->GetDev(), &fbCI, NULL, &framebuffer);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);
    m_FbsToDestroy.push_back(framebuffer);
    return framebuffer;
  }

  void CopyImagePixel(VkCommandBuffer cmd, CopyPixelParams &p, size_t offset)
  {
    rdcarray<VkBufferImageCopy> regions;
    VkImageAspectFlags aspectFlags = 0;
    VkBufferImageCopy region = {};
    region.bufferOffset = (uint64_t)offset;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageOffset.x = m_CallbackInfo.x;
    region.imageOffset.y = m_CallbackInfo.y;
    region.imageOffset.z = 0;
    region.imageExtent.width = 1U;
    region.imageExtent.height = 1U;
    region.imageExtent.depth = 1U;
    region.imageSubresource.baseArrayLayer = m_CallbackInfo.targetSubresource.slice;
    region.imageSubresource.mipLevel = m_CallbackInfo.targetSubresource.mip;
    region.imageSubresource.layerCount = 1;

    // TODO: support depth/stencil copy for multi-sampled images.
    if(p.depthCopy && (m_CallbackInfo.samples != VK_SAMPLE_COUNT_1_BIT))
      return;

    if(!p.depthCopy)
    {
      region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      regions.push_back(region);
      aspectFlags = VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);
    }
    else if(p.stencilOnly)
    {
      region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
      regions.push_back(region);
      aspectFlags = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    else
    {
      region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
      if(IsDepthOnlyFormat(p.srcImageFormat) || IsDepthAndStencilFormat(p.srcImageFormat))
      {
        regions.push_back(region);
        aspectFlags |= VK_IMAGE_ASPECT_DEPTH_BIT;
      }
      if(IsStencilFormat(p.srcImageFormat))
      {
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        region.bufferOffset = offset + 4;
        regions.push_back(region);
        aspectFlags |= VK_IMAGE_ASPECT_STENCIL_BIT;
      }
    }

    VkImage cmdCopySource = p.srcImage;
    VkImageLayout cmdCopySourceLayout = p.srcImageLayout;
    uint32_t baseArrayLayer = m_CallbackInfo.targetSubresource.slice;

    // For multi-sampled images can't call vkCmdCopyImageToBuffer directly,
    // copy using a compute shader into a staging image first.
    if(m_CallbackInfo.samples != VK_SAMPLE_COUNT_1_BIT)
    {
      regions[0].imageSubresource.baseArrayLayer = 0;
      VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                      NULL,
                                      VK_ACCESS_SHADER_WRITE_BIT |
                                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                          VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                      VK_ACCESS_SHADER_READ_BIT,
                                      p.srcImageLayout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      Unwrap(p.srcImage),
                                      {aspectFlags, 0, 1, baseArrayLayer, 1}};

      DoPipelineBarrier(cmd, 1, &barrier);
      m_pDriver->GetReplay()->CopyPixelForPixelHistory(cmd, m_CallbackInfo.extent,
                                                       m_CallbackInfo.targetSubresource.sample,
                                                       m_CallbackInfo.targetImageFormat);

      barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      barrier.newLayout = p.srcImageLayout;
      DoPipelineBarrier(cmd, 1, &barrier);

      cmdCopySource = m_CallbackInfo.stagingImage;
      cmdCopySourceLayout = VK_IMAGE_LAYOUT_GENERAL;
      baseArrayLayer = 0;
    }

    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    NULL,
                                    VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                    VK_ACCESS_TRANSFER_READ_BIT,
                                    cmdCopySourceLayout,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    VK_QUEUE_FAMILY_IGNORED,
                                    VK_QUEUE_FAMILY_IGNORED,
                                    Unwrap(cmdCopySource),
                                    {aspectFlags, 0, 1, baseArrayLayer, 1}};

    DoPipelineBarrier(cmd, 1, &barrier);

    ObjDisp(cmd)->CmdCopyImageToBuffer(
        Unwrap(cmd), Unwrap(cmdCopySource), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        Unwrap(m_CallbackInfo.dstBuffer), (uint32_t)regions.size(), regions.data());

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = cmdCopySourceLayout;
    DoPipelineBarrier(cmd, 1, &barrier);
  }

  WrappedVulkan *m_pDriver;
  PixelHistoryShaderCache *m_ShaderCache;
  PixelHistoryCallbackInfo m_CallbackInfo;
  VkQueryPool m_OcclusionPool;
  rdcarray<VkRenderPass> m_RpsToDestroy;
  rdcarray<VkFramebuffer> m_FbsToDestroy;
};

// VulkanOcclusionCallback callback is used to determine which draw events might have
// modified the pixel by doing an occlusion query.
struct VulkanOcclusionCallback : public VulkanPixelHistoryCallback
{
  VulkanOcclusionCallback(WrappedVulkan *vk, PixelHistoryShaderCache *shaderCache,
                          const PixelHistoryCallbackInfo &callbackInfo, VkQueryPool occlusionPool,
                          const rdcarray<EventUsage> &allEvents)
      : VulkanPixelHistoryCallback(vk, shaderCache, callbackInfo, occlusionPool)
  {
    for(size_t i = 0; i < allEvents.size(); i++)
      m_Events.push_back(allEvents[i].eventId);
  }

  ~VulkanOcclusionCallback()
  {
    for(auto it = m_PipeCache.begin(); it != m_PipeCache.end(); ++it)
      m_pDriver->vkDestroyPipeline(m_pDriver->GetDev(), it->second, NULL);
  }

  void PreDraw(uint32_t eid, VkCommandBuffer cmd)
  {
    if(!m_Events.contains(eid))
      return;
    VulkanRenderState prevState = m_pDriver->GetCmdRenderState();
    VulkanRenderState &pipestate = m_pDriver->GetCmdRenderState();
    const VulkanCreationInfo::Pipeline &p =
        m_pDriver->GetDebugManager()->GetPipelineInfo(pipestate.graphics.pipeline);

    uint32_t framebufferIndex = 0;
    const rdcarray<ResourceId> &atts = pipestate.GetFramebufferAttachments();
    for(uint32_t i = 0; i < atts.size(); i++)
    {
      ResourceId img = m_pDriver->GetDebugManager()->GetImageViewInfo(atts[i]).image;
      if(img == GetResID(m_CallbackInfo.targetImage))
      {
        framebufferIndex = i;
        break;
      }
    }
    VkPipeline pipe = GetPixelOcclusionPipeline(eid, prevState.graphics.pipeline, framebufferIndex);
    if(p.dynamicStates[VkDynamicScissor])
      for(uint32_t i = 0; i < pipestate.views.size(); i++)
        ScissorToPixel(pipestate.views[i], pipestate.scissors[i]);
    pipestate.graphics.pipeline = GetResID(pipe);
    ReplayDrawWithQuery(cmd, eid);

    m_pDriver->GetCmdRenderState() = prevState;
    m_pDriver->GetCmdRenderState().BindPipeline(m_pDriver, cmd, VulkanRenderState::BindGraphics,
                                                true);
  }

  bool PostDraw(uint32_t eid, VkCommandBuffer cmd) { return false; }
  void PostRedraw(uint32_t eid, VkCommandBuffer cmd) {}
  void PreDispatch(uint32_t eid, VkCommandBuffer cmd) { return; }
  bool PostDispatch(uint32_t eid, VkCommandBuffer cmd) { return false; }
  void PostRedispatch(uint32_t eid, VkCommandBuffer cmd) {}
  void PreMisc(uint32_t eid, DrawFlags flags, VkCommandBuffer cmd) { return; }
  bool PostMisc(uint32_t eid, DrawFlags flags, VkCommandBuffer cmd) { return false; }
  void PostRemisc(uint32_t eid, DrawFlags flags, VkCommandBuffer cmd) {}
  void PreEndCommandBuffer(VkCommandBuffer cmd) {}
  void AliasEvent(uint32_t primary, uint32_t alias) {}
  bool SplitSecondary() { return false; }
  void PreCmdExecute(uint32_t baseEid, uint32_t secondaryFirst, uint32_t secondaryLast,
                     VkCommandBuffer cmd)
  {
  }
  void PostCmdExecute(uint32_t baseEid, uint32_t secondaryFirst, uint32_t secondaryLast,
                      VkCommandBuffer cmd)
  {
  }

  void FetchOcclusionResults()
  {
    if(m_OcclusionQueries.size() == 0)
      return;

    m_OcclusionResults.resize(m_OcclusionQueries.size());
    VkResult vkr = ObjDisp(m_pDriver->GetDev())
                       ->GetQueryPoolResults(Unwrap(m_pDriver->GetDev()), m_OcclusionPool, 0,
                                             (uint32_t)m_OcclusionResults.size(),
                                             m_OcclusionResults.byteSize(),
                                             m_OcclusionResults.data(), sizeof(uint64_t),
                                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);
  }

  uint64_t GetOcclusionResult(uint32_t eventId)
  {
    auto it = m_OcclusionQueries.find(eventId);
    if(it == m_OcclusionQueries.end())
      return 0;
    RDCASSERT(it->second < m_OcclusionResults.size());
    return m_OcclusionResults[it->second];
  }

private:
  // ReplayDrawWithQuery binds the pipeline in the current state, and replays a single
  // draw with an occlusion query.
  void ReplayDrawWithQuery(VkCommandBuffer cmd, uint32_t eventId)
  {
    const DrawcallDescription *drawcall = m_pDriver->GetDrawcall(eventId);
    m_pDriver->GetCmdRenderState().BindPipeline(m_pDriver, cmd, VulkanRenderState::BindGraphics,
                                                true);

    uint32_t occlIndex = (uint32_t)m_OcclusionQueries.size();
    ObjDisp(cmd)->CmdBeginQuery(Unwrap(cmd), m_OcclusionPool, occlIndex, 0);

    if(drawcall->flags & DrawFlags::Indexed)
      ObjDisp(cmd)->CmdDrawIndexed(Unwrap(cmd), drawcall->numIndices, drawcall->numInstances,
                                   drawcall->indexOffset, drawcall->baseVertex,
                                   drawcall->instanceOffset);
    else
      ObjDisp(cmd)->CmdDraw(Unwrap(cmd), drawcall->numIndices, drawcall->numInstances,
                            drawcall->vertexOffset, drawcall->instanceOffset);

    ObjDisp(cmd)->CmdEndQuery(Unwrap(cmd), m_OcclusionPool, occlIndex);
    m_OcclusionQueries.insert(std::make_pair(eventId, occlIndex));
  }

  VkPipeline GetPixelOcclusionPipeline(uint32_t eid, ResourceId pipeline, uint32_t framebufferIndex)
  {
    auto it = m_PipeCache.find(pipeline);
    if(it != m_PipeCache.end())
      return it->second;

    VkGraphicsPipelineCreateInfo pipeCreateInfo = {};
    rdcarray<VkPipelineShaderStageCreateInfo> stages;
    MakeAllPassIncrementStencilPipelineCI(eid, pipeline, pipeCreateInfo, stages);
    {
      // We just need to determine if something attempted to write to pixel.
      // Disable actual color modifications.
      VkPipelineColorBlendStateCreateInfo *cbs =
          (VkPipelineColorBlendStateCreateInfo *)pipeCreateInfo.pColorBlendState;
      VkPipelineColorBlendAttachmentState *atts =
          (VkPipelineColorBlendAttachmentState *)cbs->pAttachments;
      for(uint32_t i = 0; i < cbs->attachmentCount; i++)
        atts[i].colorWriteMask = 0;
    }
    for(uint32_t i = 0; i < pipeCreateInfo.stageCount; i++)
    {
      if(stages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
      {
        stages[i].module = m_ShaderCache->GetFixedColShader(framebufferIndex);
        stages[i].pName = "main";
        break;
      }
    }
    VkPipeline pipe;
    VkResult vkr = m_pDriver->vkCreateGraphicsPipelines(m_pDriver->GetDev(), VK_NULL_HANDLE, 1,
                                                        &pipeCreateInfo, NULL, &pipe);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);
    m_PipeCache.insert(std::make_pair(pipeline, pipe));
    return pipe;
  }

private:
  std::map<ResourceId, VkPipeline> m_PipeCache;
  rdcarray<uint32_t> m_Events;
  // Key is event ID, and value is an index of where the occlusion result.
  std::map<uint32_t, uint32_t> m_OcclusionQueries;
  rdcarray<uint64_t> m_OcclusionResults;
};

struct VulkanColorAndStencilCallback : public VulkanPixelHistoryCallback
{
  VulkanColorAndStencilCallback(WrappedVulkan *vk, PixelHistoryShaderCache *shaderCache,
                                const PixelHistoryCallbackInfo &callbackInfo,
                                const rdcarray<uint32_t> &events)
      : VulkanPixelHistoryCallback(vk, shaderCache, callbackInfo, VK_NULL_HANDLE), m_Events(events)
  {
  }

  ~VulkanColorAndStencilCallback()
  {
    for(auto it = m_PipeCache.begin(); it != m_PipeCache.end(); ++it)
    {
      m_pDriver->vkDestroyPipeline(m_pDriver->GetDev(), it->second.fixedShaderStencil, NULL);
      m_pDriver->vkDestroyPipeline(m_pDriver->GetDev(), it->second.originalShaderStencil, NULL);
    }
  }

  void PreDraw(uint32_t eid, VkCommandBuffer cmd)
  {
    if(!m_Events.contains(eid) || !m_pDriver->IsCmdPrimary())
      return;

    // TODO: can't end renderpass if we are not on the last subpass.
    VulkanRenderState prevState = m_pDriver->GetCmdRenderState();
    VulkanRenderState &pipestate = m_pDriver->GetCmdRenderState();

    pipestate.EndRenderPass(cmd);

    // Get pre-modification values
    size_t storeOffset = m_EventIndices.size() * sizeof(EventInfo);
    VkImage depthImage = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    const DrawcallDescription *draw = m_pDriver->GetDrawcall(eid);
    if(draw && draw->depthOut != ResourceId())
    {
      ResourceId resId = m_pDriver->GetResourceManager()->GetLiveID(draw->depthOut);
      depthImage = m_pDriver->GetResourceManager()->GetCurrentHandle<VkImage>(resId);
      const VulkanCreationInfo::Image &imginfo = m_pDriver->GetDebugManager()->GetImageInfo(resId);
      depthFormat = imginfo.format;
    }

    CopyPixel(m_CallbackInfo.targetImage, m_CallbackInfo.targetImageFormat, depthImage, depthFormat,
              cmd, storeOffset);

    ResourceId prevRenderpass = pipestate.renderPass;
    ResourceId prevFramebuffer = pipestate.GetFramebuffer();
    rdcarray<ResourceId> prevFBattachments = pipestate.GetFramebufferAttachments();

    uint32_t prevSubpass = pipestate.subpass;

    {
      VkRenderPass newRp =
          CreateRenderPass(pipestate.renderPass, pipestate.GetFramebuffer(), pipestate.subpass);
      VkFramebuffer newFb =
          CreateFramebuffer(pipestate.renderPass, newRp, pipestate.subpass,
                            pipestate.GetFramebuffer(), m_CallbackInfo.stencilImageView);
      uint32_t framebufferIndex = 0;
      const rdcarray<ResourceId> &atts = pipestate.GetFramebufferAttachments();
      for(uint32_t i = 0; i < atts.size(); i++)
      {
        ResourceId img = m_pDriver->GetDebugManager()->GetImageViewInfo(atts[i]).image;
        if(img == GetResID(m_CallbackInfo.targetImage))
        {
          framebufferIndex = i;
          break;
        }
      }
      PipelineReplacements replacements =
          GetPipelineReplacements(eid, pipestate.graphics.pipeline, newRp, framebufferIndex);

      const VulkanCreationInfo::Pipeline &p =
          m_pDriver->GetDebugManager()->GetPipelineInfo(pipestate.graphics.pipeline);
      if(p.dynamicStates[VkDynamicScissor])
        for(uint32_t i = 0; i < pipestate.views.size(); i++)
          ScissorToPixel(pipestate.views[i], pipestate.scissors[i]);

      // Replay the draw with a fixed color shader that never discards, and stencil
      // increment to count number of fragments. We will get the number of fragments
      // not accounting for shader discard.
      pipestate.SetFramebuffer(m_pDriver, GetResID(newFb));
      pipestate.renderPass = GetResID(newRp);
      pipestate.subpass = 0;
      pipestate.graphics.pipeline = GetResID(replacements.fixedShaderStencil);
      ReplayDraw(cmd, eid, true);

      CopyPixelParams params = {};
      params.srcImage = m_CallbackInfo.stencilImage;
      params.srcImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      params.srcImageFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
      params.depthCopy = true;
      params.stencilOnly = true;
      // Copy stencil value that indicates the number of fragments ignoring
      // shader discard.
      CopyImagePixel(cmd, params, storeOffset + offsetof(struct EventInfo, dsWithoutShaderDiscard));

      // Replay the draw with the original fragment shader to get the actual number
      // of fragments, accounting for potential shader discard.
      pipestate.graphics.pipeline = GetResID(replacements.originalShaderStencil);
      ReplayDraw(cmd, eid, true);

      CopyImagePixel(cmd, params, storeOffset + offsetof(struct EventInfo, dsWithShaderDiscard));
    }

    // Restore the state.
    m_pDriver->GetCmdRenderState() = prevState;
    pipestate.SetFramebuffer(prevFramebuffer, prevFBattachments);
    pipestate.renderPass = prevRenderpass;
    pipestate.subpass = prevSubpass;

    // TODO: Need to re-start on the correct subpass.
    if(pipestate.graphics.pipeline != ResourceId())
      pipestate.BeginRenderPassAndApplyState(m_pDriver, cmd, VulkanRenderState::BindGraphics);
  }

  bool PostDraw(uint32_t eid, VkCommandBuffer cmd)
  {
    if(!m_Events.contains(eid) || !m_pDriver->IsCmdPrimary())
      return false;

    m_pDriver->GetCmdRenderState().EndRenderPass(cmd);

    size_t storeOffset = m_EventIndices.size() * sizeof(EventInfo);
    VkImage depthImage = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    const DrawcallDescription *draw = m_pDriver->GetDrawcall(eid);
    if(draw && draw->depthOut != ResourceId())
    {
      ResourceId resId = m_pDriver->GetResourceManager()->GetLiveID(draw->depthOut);
      depthImage = m_pDriver->GetResourceManager()->GetCurrentHandle<VkImage>(resId);
      const VulkanCreationInfo::Image &imginfo = m_pDriver->GetDebugManager()->GetImageInfo(resId);
      depthFormat = imginfo.format;
    }

    CopyPixel(m_CallbackInfo.targetImage, m_CallbackInfo.targetImageFormat, depthImage, depthFormat,
              cmd, storeOffset + offsetof(struct EventInfo, postmod));

    m_pDriver->GetCmdRenderState().BeginRenderPassAndApplyState(m_pDriver, cmd,
                                                                VulkanRenderState::BindGraphics);

    // Get post-modification values
    m_EventIndices.insert(std::make_pair(eid, m_EventIndices.size()));
    return false;
  }

  void PostRedraw(uint32_t eid, VkCommandBuffer cmd)
  {
    // nothing to do
  }

  void PreCmdExecute(uint32_t baseEid, uint32_t secondaryFirst, uint32_t secondaryLast,
                     VkCommandBuffer cmd)
  {
    uint32_t eventId = 0;
    if(m_Events.size() == 0)
      return;
    for(size_t i = 0; i < m_Events.size(); i++)
    {
      // Find the first event in range
      if(m_Events[i] >= secondaryFirst && m_Events[i] <= secondaryLast)
      {
        eventId = m_Events[i];
        break;
      }
    }
    if(eventId == 0)
      return;

    m_pDriver->GetCmdRenderState().EndRenderPass(cmd);

    // Copy
    size_t storeOffset = m_EventIndices.size() * sizeof(EventInfo);
    CopyPixel(m_CallbackInfo.targetImage, m_CallbackInfo.targetImageFormat, VK_NULL_HANDLE,
              VK_FORMAT_UNDEFINED, cmd, storeOffset);
    m_EventIndices.insert(std::make_pair(eventId, m_EventIndices.size()));

    m_pDriver->GetCmdRenderState().BeginRenderPassAndApplyState(m_pDriver, cmd,
                                                                VulkanRenderState::BindNone);
  }

  void PostCmdExecute(uint32_t baseEid, uint32_t secondaryFirst, uint32_t secondaryLast,
                      VkCommandBuffer cmd)
  {
    uint32_t eventId = 0;
    if(m_Events.size() == 0)
      return;
    for(int32_t i = (int32_t)m_Events.size() - 1; i >= 0; i--)
    {
      // Find the last event in range.
      if(m_Events[i] >= secondaryFirst && m_Events[i] <= secondaryLast)
      {
        eventId = m_Events[i];
        break;
      }
    }
    if(eventId == 0)
      return;

    m_pDriver->GetCmdRenderState().EndRenderPass(cmd);
    size_t storeOffset = 0;
    auto it = m_EventIndices.find(eventId);
    if(it != m_EventIndices.end())
    {
      storeOffset = it->second * sizeof(EventInfo);
    }
    else
    {
      storeOffset = m_EventIndices.size() * sizeof(EventInfo);
      m_EventIndices.insert(std::make_pair(eventId, m_EventIndices.size()));
    }
    CopyPixel(m_CallbackInfo.targetImage, m_CallbackInfo.targetImageFormat, VK_NULL_HANDLE,
              VK_FORMAT_UNDEFINED, cmd, storeOffset + offsetof(struct EventInfo, postmod));
    m_pDriver->GetCmdRenderState().BeginRenderPassAndApplyState(m_pDriver, cmd,
                                                                VulkanRenderState::BindNone);
  }

  void PreDispatch(uint32_t eid, VkCommandBuffer cmd)
  {
    if(!m_Events.contains(eid))
      return;

    size_t storeOffset = m_EventIndices.size() * sizeof(EventInfo);
    CopyPixel(m_CallbackInfo.targetImage, m_CallbackInfo.targetImageFormat, VK_NULL_HANDLE,
              VK_FORMAT_UNDEFINED, cmd, storeOffset);
  }
  bool PostDispatch(uint32_t eid, VkCommandBuffer cmd)
  {
    if(!m_Events.contains(eid))
      return false;

    size_t storeOffset = m_EventIndices.size() * sizeof(EventInfo);
    CopyPixel(m_CallbackInfo.targetImage, m_CallbackInfo.targetImageFormat, VK_NULL_HANDLE,
              VK_FORMAT_UNDEFINED, cmd, storeOffset + offsetof(struct EventInfo, postmod));
    m_EventIndices.insert(std::make_pair(eid, m_EventIndices.size()));
    return false;
  }
  void PostRedispatch(uint32_t eid, VkCommandBuffer cmd) {}
  void PreMisc(uint32_t eid, DrawFlags flags, VkCommandBuffer cmd) { PreDispatch(eid, cmd); }
  bool PostMisc(uint32_t eid, DrawFlags flags, VkCommandBuffer cmd)
  {
    if(!m_Events.contains(eid))
      return false;
    if(flags & DrawFlags::BeginPass)
      m_pDriver->GetCmdRenderState().EndRenderPass(cmd);

    bool ret = PostDispatch(eid, cmd);

    if(flags & DrawFlags::BeginPass)
      m_pDriver->GetCmdRenderState().BeginRenderPassAndApplyState(m_pDriver, cmd,
                                                                  VulkanRenderState::BindNone);

    return ret;
  }

  bool SplitSecondary() { return true; }
  void PostRemisc(uint32_t eid, DrawFlags flags, VkCommandBuffer cmd) {}
  void PreEndCommandBuffer(VkCommandBuffer cmd) {}
  void AliasEvent(uint32_t primary, uint32_t alias)
  {
    RDCWARN(
        "Alised events are not supported, results might be inaccurate. Primary event id: %u, "
        "alias: %u.",
        primary, alias);
  }

  int32_t GetEventIndex(uint32_t eventId)
  {
    auto it = m_EventIndices.find(eventId);
    if(it == m_EventIndices.end())
      // Most likely a secondary command buffer event for which there is no
      // information.
      return -1;
    RDCASSERT(it != m_EventIndices.end());
    return (int32_t)it->second;
  }

private:
  void CopyPixel(VkImage srcImage, VkFormat srcFormat, VkImage depthImage, VkFormat depthFormat,
                 VkCommandBuffer cmd, size_t offset)
  {
    CopyPixelParams colourCopyParams = {};
    colourCopyParams.srcImage = srcImage;
    colourCopyParams.srcImageFormat = srcFormat;
    colourCopyParams.srcImageLayout = m_pDriver->GetDebugManager()->GetImageLayout(
        GetResID(srcImage), VK_IMAGE_ASPECT_COLOR_BIT, m_CallbackInfo.targetSubresource.mip,
        m_CallbackInfo.targetSubresource.slice);
    CopyImagePixel(cmd, colourCopyParams, offset);

    if(depthImage != VK_NULL_HANDLE)
    {
      CopyPixelParams depthCopyParams = colourCopyParams;
      depthCopyParams.depthCopy = true;
      depthCopyParams.srcImage = depthImage;
      depthCopyParams.srcImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      depthCopyParams.srcImageFormat = depthFormat;
      CopyImagePixel(cmd, depthCopyParams, offset + offsetof(struct PixelHistoryValue, depth));
    }
  }

  // ReplayDraw begins renderpass, executes a single draw defined by the eventId and
  // ends the renderpass.
  void ReplayDraw(VkCommandBuffer cmd, uint32_t eventId, bool clear = false)
  {
    m_pDriver->GetCmdRenderState().BeginRenderPassAndApplyState(m_pDriver, cmd,
                                                                VulkanRenderState::BindGraphics);

    if(clear)
    {
      VkClearAttachment att = {};
      att.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
      VkClearRect rect = {};
      rect.rect.offset.x = m_CallbackInfo.x;
      rect.rect.offset.y = m_CallbackInfo.y;
      rect.rect.extent.width = 1;
      rect.rect.extent.height = 1;
      rect.baseArrayLayer = 0;
      rect.layerCount = 1;
      ObjDisp(cmd)->CmdClearAttachments(Unwrap(cmd), 1, &att, 1, &rect);
    }

    const DrawcallDescription *drawcall = m_pDriver->GetDrawcall(eventId);
    if(drawcall->flags & DrawFlags::Indexed)
      ObjDisp(cmd)->CmdDrawIndexed(Unwrap(cmd), drawcall->numIndices, drawcall->numInstances,
                                   drawcall->indexOffset, drawcall->baseVertex,
                                   drawcall->instanceOffset);
    else
      ObjDisp(cmd)->CmdDraw(Unwrap(cmd), drawcall->numIndices, drawcall->numInstances,
                            drawcall->vertexOffset, drawcall->instanceOffset);

    m_pDriver->GetCmdRenderState().EndRenderPass(cmd);
  }

  // GetPipelineReplacements creates pipeline replacements that disable all tests,
  // and use either fixed or original fragment shader, and shaders that don't
  // have side effects.
  PipelineReplacements GetPipelineReplacements(uint32_t eid, ResourceId pipeline, VkRenderPass rp,
                                               uint32_t framebufferIndex)
  {
    // The map does not keep track of the event ID, event ID is only used to figure out
    // which shaders need to be modified. Those flags are based on the shaders bound,
    // so in theory all events should share those flags if they are using the same
    // pipeline.
    auto pipeIt = m_PipeCache.find(pipeline);
    if(pipeIt != m_PipeCache.end())
      return pipeIt->second;

    VkGraphicsPipelineCreateInfo pipeCreateInfo = {};
    rdcarray<VkPipelineShaderStageCreateInfo> stages;
    MakeAllPassIncrementStencilPipelineCI(eid, pipeline, pipeCreateInfo, stages);
    // No need to change depth stencil state, it is already
    // set to always pass, and increment.
    pipeCreateInfo.renderPass = rp;

    {
      // We just need to determine if something attempted to write to pixel.
      // Disable actual color modifications.
      VkPipelineColorBlendStateCreateInfo *cbs =
          (VkPipelineColorBlendStateCreateInfo *)pipeCreateInfo.pColorBlendState;
      VkPipelineColorBlendAttachmentState *atts =
          (VkPipelineColorBlendAttachmentState *)cbs->pAttachments;
      for(uint32_t i = 0; i < cbs->attachmentCount; i++)
        atts[i].colorWriteMask = 0;
    }

    PipelineReplacements replacements = {};
    VkResult vkr = m_pDriver->vkCreateGraphicsPipelines(m_pDriver->GetDev(), VK_NULL_HANDLE, 1,
                                                        &pipeCreateInfo, NULL,
                                                        &replacements.originalShaderStencil);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);

    for(uint32_t i = 0; i < pipeCreateInfo.stageCount; i++)
    {
      if(stages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
      {
        stages[i].module = m_ShaderCache->GetFixedColShader(framebufferIndex);
        stages[i].pName = "main";
        break;
      }
    }

    vkr = m_pDriver->vkCreateGraphicsPipelines(m_pDriver->GetDev(), VK_NULL_HANDLE, 1,
                                               &pipeCreateInfo, NULL,
                                               &replacements.fixedShaderStencil);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);

    m_PipeCache.insert(std::make_pair(pipeline, replacements));

    return replacements;
  }

  std::map<ResourceId, PipelineReplacements> m_PipeCache;
  rdcarray<uint32_t> m_Events;
  // Key is event ID, and value is an index of where the event data is stored.
  std::map<uint32_t, size_t> m_EventIndices;
};

// TestsFailedCallback replays draws to figure out which tests failed (for ex., depth,
// stencil test etc).
struct TestsFailedCallback : public VulkanPixelHistoryCallback
{
  TestsFailedCallback(WrappedVulkan *vk, PixelHistoryShaderCache *shaderCache,
                      const PixelHistoryCallbackInfo &callbackInfo, VkQueryPool occlusionPool,
                      rdcarray<uint32_t> events)
      : VulkanPixelHistoryCallback(vk, shaderCache, callbackInfo, occlusionPool), m_Events(events)
  {
  }

  ~TestsFailedCallback() {}
  void PreDraw(uint32_t eid, VkCommandBuffer cmd)
  {
    if(!m_Events.contains(eid))
      return;

    VulkanRenderState &pipestate = m_pDriver->GetCmdRenderState();
    const VulkanCreationInfo::Pipeline &p =
        m_pDriver->GetDebugManager()->GetPipelineInfo(pipestate.graphics.pipeline);
    uint32_t eventFlags = CalculateEventFlags(p, pipestate);
    m_EventFlags[eid] = eventFlags;

    // TODO: figure out if the shader has early fragments tests turned on,
    // based on the currently bound fragment shader.
    bool earlyFragmentTests = false;
    m_HasEarlyFragments[eid] = earlyFragmentTests;

    ResourceId curPipeline = pipestate.graphics.pipeline;
    VulkanRenderState m_PrevState = m_pDriver->GetCmdRenderState();

    uint32_t framebufferIndex = 0;
    const rdcarray<ResourceId> &atts = pipestate.GetFramebufferAttachments();
    for(uint32_t i = 0; i < atts.size(); i++)
    {
      ResourceId img = m_pDriver->GetDebugManager()->GetImageViewInfo(atts[i]).image;
      if(img == GetResID(m_CallbackInfo.targetImage))
      {
        framebufferIndex = i;
        break;
      }
    }

    ReplayDrawWithTests(cmd, eid, eventFlags, curPipeline, framebufferIndex);

    m_pDriver->GetCmdRenderState() = m_PrevState;
    m_pDriver->GetCmdRenderState().BindPipeline(m_pDriver, cmd, VulkanRenderState::BindGraphics,
                                                false);
  }

  bool PostDraw(uint32_t eid, VkCommandBuffer cmd) { return false; }
  void AliasEvent(uint32_t primary, uint32_t alias)
  {
    // TODO: handle aliased events.
  }

  void PostRedraw(uint32_t eid, VkCommandBuffer cmd)
  {
    // nothing to do
  }

  void PreDispatch(uint32_t eid, VkCommandBuffer cmd) {}
  bool PostDispatch(uint32_t eid, VkCommandBuffer cmd) { return false; }
  void PostRedispatch(uint32_t eid, VkCommandBuffer cmd) {}
  void PreMisc(uint32_t eid, DrawFlags flags, VkCommandBuffer cmd) {}
  bool PostMisc(uint32_t eid, DrawFlags flags, VkCommandBuffer cmd) { return false; }
  void PostRemisc(uint32_t eid, DrawFlags flags, VkCommandBuffer cmd) {}
  bool SplitSecondary() { return false; }
  void PreCmdExecute(uint32_t baseEid, uint32_t secondaryFirst, uint32_t secondaryLast,
                     VkCommandBuffer cmd)
  {
  }
  void PostCmdExecute(uint32_t baseEid, uint32_t secondaryFirst, uint32_t secondaryLast,
                      VkCommandBuffer cmd)
  {
  }
  void PreEndCommandBuffer(VkCommandBuffer cmd) {}
  uint32_t GetEventFlags(uint32_t eventId)
  {
    auto it = m_EventFlags.find(eventId);
    if(it == m_EventFlags.end())
      RDCERR("Can't find event flags for event %u", eventId);
    return it->second;
  }

  void FetchOcclusionResults()
  {
    if(m_OcclusionQueries.empty())
      return;
    m_OcclusionResults.resize(m_OcclusionQueries.size());
    VkResult vkr =
        ObjDisp(m_pDriver->GetDev())
            ->GetQueryPoolResults(Unwrap(m_pDriver->GetDev()), m_OcclusionPool, 0,
                                  (uint32_t)m_OcclusionResults.size(), m_OcclusionResults.byteSize(),
                                  m_OcclusionResults.data(), sizeof(m_OcclusionResults[0]),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);
  }

  uint64_t GetOcclusionResult(uint32_t eventId, uint32_t test) const
  {
    auto it = m_OcclusionQueries.find(rdcpair<uint32_t, uint32_t>(eventId, test));
    if(it == m_OcclusionQueries.end())
      RDCERR("Can't locate occlusion query for event id %u and test flags %u", eventId, test);
    if(it->second >= m_OcclusionResults.size())
      RDCERR("Event %u, occlusion index is %u, and the total # of occlusion query data %zu",
             eventId, it->second, m_OcclusionResults.size());
    return m_OcclusionResults[it->second];
  }

  bool HasEarlyFragments(uint32_t eventId) const
  {
    auto it = m_HasEarlyFragments.find(eventId);
    RDCASSERT(it != m_HasEarlyFragments.end());
    return it->second;
  }

private:
  uint32_t CalculateEventFlags(const VulkanCreationInfo::Pipeline &p,
                               const VulkanRenderState &pipestate)
  {
    uint32_t flags = 0;

    // Culling
    {
      if(p.cullMode != VK_CULL_MODE_NONE)
        flags |= TestEnabled_Culling;

      if(p.cullMode == VK_CULL_MODE_FRONT_AND_BACK)
        flags |= TestMustFail_Culling;
    }

    // Depth and Stencil tests.
    {
      if(p.depthBoundsEnable)
        flags |= TestEnabled_DepthBounds;

      if(p.depthTestEnable)
      {
        if(p.depthCompareOp != VK_COMPARE_OP_ALWAYS)
          flags |= TestEnabled_DepthTesting;
        if(p.depthCompareOp == VK_COMPARE_OP_NEVER)
          flags |= TestMustFail_DepthTesting;
      }

      if(p.stencilTestEnable)
      {
        if(p.front.compareOp != VK_COMPARE_OP_ALWAYS || p.back.compareOp != VK_COMPARE_OP_ALWAYS)
          flags |= TestEnabled_StencilTesting;

        if(p.front.compareOp == VK_COMPARE_OP_NEVER && p.back.compareOp == VK_COMPARE_OP_NEVER)
          flags |= TestMustFail_StencilTesting;
        else if(p.front.compareOp == VK_COMPARE_OP_NEVER && p.cullMode == VK_CULL_MODE_BACK_BIT)
          flags |= TestMustFail_StencilTesting;
        else if(p.cullMode == VK_CULL_MODE_FRONT_BIT && p.back.compareOp == VK_COMPARE_OP_NEVER)
          flags |= TestMustFail_StencilTesting;
      }
    }

    // Scissor
    {
      bool inRegion = false;
      bool inAllRegions = true;
      // Do we even need to know viewerport here?
      const VkRect2D *pScissors;
      uint32_t scissorCount;
      if(p.dynamicStates[VkDynamicScissor])
      {
        pScissors = pipestate.scissors.data();
        scissorCount = (uint32_t)pipestate.scissors.size();
      }
      else
      {
        pScissors = p.scissors.data();
        scissorCount = (uint32_t)p.scissors.size();
      }
      for(uint32_t i = 0; i < scissorCount; i++)
      {
        const VkOffset2D &offset = pScissors[i].offset;
        const VkExtent2D &extent = pScissors[i].extent;
        if((m_CallbackInfo.x >= (uint32_t)offset.x) && (m_CallbackInfo.y >= (uint32_t)offset.y) &&
           (m_CallbackInfo.x < (offset.x + extent.width)) &&
           (m_CallbackInfo.y < (offset.y + extent.height)))
          inRegion = true;
        else
          inAllRegions = false;
      }
      if(!inRegion)
        flags |= TestMustFail_Scissor;
      if(inAllRegions)
        flags |= TestMustPass_Scissor;
    }

    // Blending
    {
      if(m_pDriver->GetDeviceFeatures().independentBlend)
      {
        for(size_t i = 0; i < p.attachments.size(); i++)
        {
          if(p.attachments[i].blendEnable)
          {
            flags |= Blending_Enabled;
            break;
          }
        }
      }
      else
      {
        // Might not have attachments if rasterization is disabled
        if(p.attachments.size() > 0 && p.attachments[0].blendEnable)
          flags |= Blending_Enabled;
      }
    }

    if(p.shaders[StageIndex(VK_SHADER_STAGE_FRAGMENT_BIT)].module == ResourceId())
      flags |= UnboundFragmentShader;

    // Samples
    {
      // TODO: figure out if we always need to check this.
      flags |= TestEnabled_SampleMask;

      // compare to ms->pSampleMask
      if((p.sampleMask & m_CallbackInfo.sampleMask) == 0)
        flags |= TestMustFail_SampleMask;
    }

    // TODO: is shader discard always possible?
    flags |= TestEnabled_FragmentDiscard;
    return flags;
  }

  // Flags to create a pipeline for tests, can be combined to control how
  // a pipeline is created.
  enum
  {
    PipelineCreationFlags_DisableCulling = 1 << 0,
    PipelineCreationFlags_DisableDepthTest = 1 << 1,
    PipelineCreationFlags_DisableStencilTest = 1 << 2,
    PipelineCreationFlags_DisableDepthBoundsTest = 1 << 3,
    PipelineCreationFlags_FixedColorShader = 1 << 4,
    PipelineCreationFlags_IntersectOriginalScissor = 1 << 5,
  };

  void ReplayDrawWithTests(VkCommandBuffer cmd, uint32_t eid, uint32_t eventFlags,
                           ResourceId basePipeline, uint32_t framebufferIndex)
  {
    // Backface culling
    if(eventFlags & TestMustFail_Culling)
      return;

    const VulkanCreationInfo::Pipeline &p =
        m_pDriver->GetDebugManager()->GetPipelineInfo(basePipeline);
    EventFlags eventShaderFlags = m_pDriver->GetEventFlags(eid);
    uint32_t numberOfStages = 5;
    rdcarray<VkShaderModule> replacementShaders;
    replacementShaders.resize(numberOfStages);
    // Replace fragment shader because it might have early fragments
    for(size_t i = 0; i < numberOfStages; i++)
    {
      if(p.shaders[i].module == ResourceId())
        continue;
      ShaderStage stage = StageFromIndex(i);
      bool rwInStage = (eventShaderFlags & PipeStageRWEventFlags(stage)) != EventFlags::NoFlags;
      if(rwInStage || (stage == ShaderStage::Fragment))
        replacementShaders[i] =
            m_ShaderCache->GetShaderWithoutSideEffects(p.shaders[i].module, p.shaders[i].entryPoint);
    }

    bool dynamicScissor = p.dynamicStates[VkDynamicScissor];
    VulkanRenderState &pipestate = m_pDriver->GetCmdRenderState();
    rdcarray<VkRect2D> prevScissors = pipestate.scissors;
    if(dynamicScissor)
      for(uint32_t i = 0; i < pipestate.views.size(); i++)
        ScissorToPixel(pipestate.views[i], pipestate.scissors[i]);

    if(eventFlags & TestEnabled_Culling)
    {
      uint32_t pipeFlags =
          PipelineCreationFlags_DisableDepthTest | PipelineCreationFlags_DisableDepthBoundsTest |
          PipelineCreationFlags_DisableStencilTest | PipelineCreationFlags_FixedColorShader;
      VkPipeline pipe = CreatePipeline(basePipeline, pipeFlags, dynamicScissor, replacementShaders,
                                       framebufferIndex);
      ReplayDraw(cmd, pipe, eid, TestEnabled_Culling);
    }

    // Scissor
    if(eventFlags & TestMustFail_Scissor)
      return;

    if((eventFlags & (TestEnabled_Scissor | TestMustPass_Scissor)) == TestEnabled_Scissor)
    {
      uint32_t pipeFlags =
          PipelineCreationFlags_IntersectOriginalScissor | PipelineCreationFlags_DisableDepthTest |
          PipelineCreationFlags_DisableDepthBoundsTest | PipelineCreationFlags_DisableStencilTest |
          PipelineCreationFlags_FixedColorShader;
      VkPipeline pipe = CreatePipeline(basePipeline, pipeFlags, dynamicScissor, replacementShaders,
                                       framebufferIndex);
      // This will change the dynamic scissor state for the later tests, but since those
      // tests happen later in the pipeline, it does not matter.
      if(dynamicScissor)
        for(uint32_t i = 0; i < pipestate.views.size(); i++)
          IntersectScissors(prevScissors[i], pipestate.scissors[i]);
      ReplayDraw(cmd, pipe, eid, TestEnabled_Scissor);
    }

    // Sample mask
    if(eventFlags & TestMustFail_SampleMask)
      return;

    if(eventFlags & TestEnabled_SampleMask)
    {
      uint32_t pipeFlags =
          PipelineCreationFlags_DisableDepthBoundsTest | PipelineCreationFlags_DisableStencilTest |
          PipelineCreationFlags_DisableDepthTest | PipelineCreationFlags_FixedColorShader;
      VkPipeline pipe = CreatePipeline(basePipeline, pipeFlags, dynamicScissor, replacementShaders,
                                       framebufferIndex);
      ReplayDraw(cmd, pipe, eid, TestEnabled_SampleMask);
    }

    // Depth bounds
    if(eventFlags & TestEnabled_DepthBounds)
    {
      uint32_t pipeFlags = PipelineCreationFlags_DisableStencilTest |
                           PipelineCreationFlags_DisableDepthTest |
                           PipelineCreationFlags_FixedColorShader;
      VkPipeline pipe = CreatePipeline(basePipeline, pipeFlags, dynamicScissor, replacementShaders,
                                       framebufferIndex);
      ReplayDraw(cmd, pipe, eid, TestEnabled_DepthBounds);
    }

    // Stencil test
    if(eventFlags & TestMustFail_StencilTesting)
      return;

    if(eventFlags & TestEnabled_StencilTesting)
    {
      uint32_t pipeFlags =
          PipelineCreationFlags_DisableDepthTest | PipelineCreationFlags_FixedColorShader;
      VkPipeline pipe = CreatePipeline(basePipeline, pipeFlags, dynamicScissor, replacementShaders,
                                       framebufferIndex);
      ReplayDraw(cmd, pipe, eid, TestEnabled_StencilTesting);
    }

    // Depth test
    if(eventFlags & TestMustFail_DepthTesting)
      return;

    if(eventFlags & TestEnabled_DepthTesting)
    {
      // Previous test might have modified the stencil state, which could
      // cause this event to fail.
      uint32_t pipeFlags =
          PipelineCreationFlags_DisableStencilTest | PipelineCreationFlags_FixedColorShader;

      VkPipeline pipe = CreatePipeline(basePipeline, pipeFlags, dynamicScissor, replacementShaders,
                                       framebufferIndex);
      ReplayDraw(cmd, pipe, eid, TestEnabled_DepthTesting);
    }

    // Shader discard
    if(eventFlags & TestEnabled_FragmentDiscard)
    {
      // With early fragment tests, sample counting (occlusion query) will be done before the shader
      // executes.
      // TODO: remove early fragment tests if it is ON.
      uint32_t pipeFlags = PipelineCreationFlags_DisableDepthBoundsTest |
                           PipelineCreationFlags_DisableStencilTest |
                           PipelineCreationFlags_DisableDepthTest;
      VkPipeline pipe = CreatePipeline(basePipeline, pipeFlags, dynamicScissor, replacementShaders,
                                       framebufferIndex);
      ReplayDraw(cmd, pipe, eid, TestEnabled_FragmentDiscard);
    }
  }

  // Creates a pipeline that is based on the given pipeline and the given
  // pipeline flags. Modifies the base pipeline according to the flags, and
  // leaves the original pipeline behavior if a flag is not set.
  VkPipeline CreatePipeline(ResourceId basePipeline, uint32_t pipeCreateFlags, bool dynamicScissor,
                            const rdcarray<VkShaderModule> &replacementShaders,
                            uint32_t framebufferIndex)
  {
    rdcpair<ResourceId, uint32_t> pipeKey(basePipeline, pipeCreateFlags);
    auto it = m_PipeCache.find(pipeKey);
    // Check if we processed this pipeline before.
    if(it != m_PipeCache.end())
      return it->second;

    VkGraphicsPipelineCreateInfo ci = {};
    m_pDriver->GetShaderCache()->MakeGraphicsPipelineInfo(ci, basePipeline);
    VkPipelineRasterizationStateCreateInfo *rs =
        (VkPipelineRasterizationStateCreateInfo *)ci.pRasterizationState;
    VkPipelineDepthStencilStateCreateInfo *ds =
        (VkPipelineDepthStencilStateCreateInfo *)ci.pDepthStencilState;
    VkPipelineViewportStateCreateInfo *vs = (VkPipelineViewportStateCreateInfo *)ci.pViewportState;
    VkPipelineMultisampleStateCreateInfo *ms =
        (VkPipelineMultisampleStateCreateInfo *)ci.pMultisampleState;

    // Only interested in a single sample.
    ms->pSampleMask = &m_CallbackInfo.sampleMask;
    // We are going to replay a draw multiple times, don't want to modify the
    // depth value, not to influence later tests.
    ds->depthWriteEnable = VK_FALSE;

    if(pipeCreateFlags & PipelineCreationFlags_DisableCulling)
      rs->cullMode = VK_CULL_MODE_NONE;
    if(pipeCreateFlags & PipelineCreationFlags_DisableDepthTest)
      ds->depthTestEnable = VK_FALSE;
    if(pipeCreateFlags & PipelineCreationFlags_DisableStencilTest)
      ds->stencilTestEnable = VK_FALSE;
    if(pipeCreateFlags & PipelineCreationFlags_DisableDepthBoundsTest)
      ds->depthBoundsTestEnable = VK_FALSE;

    rdcarray<VkPipelineShaderStageCreateInfo> stages;
    stages.resize(ci.stageCount);
    memcpy(stages.data(), ci.pStages, stages.byteSize());

    for(size_t i = 0; i < ci.stageCount; i++)
    {
      if((ci.pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) &&
         (pipeCreateFlags & PipelineCreationFlags_FixedColorShader))
      {
        stages[i].module = m_ShaderCache->GetFixedColShader(framebufferIndex);
        stages[i].pName = "main";
      }
      else if(replacementShaders[StageIndex(stages[i].stage)] != VK_NULL_HANDLE)
      {
        stages[i].module = replacementShaders[StageIndex(stages[i].stage)];
      }
    }
    ci.pStages = stages.data();

    if(!dynamicScissor)
    {
      VkRect2D *pScissors = (VkRect2D *)vs->pScissors;
      for(uint32_t i = 0; i < vs->viewportCount; i++)
      {
        ScissorToPixel(vs->pViewports[i], pScissors[i]);
        if(pipeCreateFlags & PipelineCreationFlags_IntersectOriginalScissor)
          IntersectScissors(vs->pScissors[i], pScissors[i]);
      }
    }

    VkPipeline pipe;
    VkResult vkr = m_pDriver->vkCreateGraphicsPipelines(m_pDriver->GetDev(), VK_NULL_HANDLE, 1, &ci,
                                                        NULL, &pipe);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);
    m_PipeCache.insert(std::make_pair(pipeKey, pipe));
    return pipe;
  }

  void ReplayDraw(VkCommandBuffer cmd, VkPipeline pipe, int eventId, uint32_t test)
  {
    m_pDriver->GetCmdRenderState().graphics.pipeline = GetResID(pipe);
    m_pDriver->GetCmdRenderState().BindPipeline(m_pDriver, cmd, VulkanRenderState::BindGraphics,
                                                false);

    uint32_t index = (uint32_t)m_OcclusionQueries.size();
    if(m_OcclusionQueries.find(rdcpair<uint32_t, uint32_t>(eventId, test)) != m_OcclusionQueries.end())
      RDCERR("A query already exist for event id %u and test %u", eventId, test);
    m_OcclusionQueries.insert(std::make_pair(rdcpair<uint32_t, uint32_t>(eventId, test), index));

    ObjDisp(cmd)->CmdBeginQuery(Unwrap(cmd), m_OcclusionPool, index, 0);

    const DrawcallDescription *drawcall = m_pDriver->GetDrawcall(eventId);
    if(drawcall->flags & DrawFlags::Indexed)
      ObjDisp(cmd)->CmdDrawIndexed(Unwrap(cmd), drawcall->numIndices, drawcall->numInstances,
                                   drawcall->indexOffset, drawcall->baseVertex,
                                   drawcall->instanceOffset);
    else
      ObjDisp(cmd)->CmdDraw(Unwrap(cmd), drawcall->numIndices, drawcall->numInstances,
                            drawcall->vertexOffset, drawcall->instanceOffset);

    ObjDisp(cmd)->CmdEndQuery(Unwrap(cmd), m_OcclusionPool, index);
  }

  rdcarray<uint32_t> m_Events;
  // Key is event ID, value is the flags for that event.
  std::map<uint32_t, uint32_t> m_EventFlags;
  // Key is a pair <Base pipeline, pipeline flags>
  std::map<rdcpair<ResourceId, uint32_t>, VkPipeline> m_PipeCache;
  // Key: pair <event ID, test>
  // value: the index where occlusion query is in m_OcclusionResults
  std::map<rdcpair<uint32_t, uint32_t>, uint32_t> m_OcclusionQueries;
  std::map<uint32_t, bool> m_HasEarlyFragments;
  rdcarray<uint64_t> m_OcclusionResults;
};

// Callback used to get values for each fragment.
struct VulkanPixelHistoryPerFragmentCallback : VulkanPixelHistoryCallback
{
  VulkanPixelHistoryPerFragmentCallback(WrappedVulkan *vk, PixelHistoryShaderCache *shaderCache,
                                        const PixelHistoryCallbackInfo &callbackInfo,
                                        std::map<uint32_t, uint32_t> eventFragments)
      : VulkanPixelHistoryCallback(vk, shaderCache, callbackInfo, VK_NULL_HANDLE),
        m_EventFragments(eventFragments)
  {
  }

  ~VulkanPixelHistoryPerFragmentCallback()
  {
    for(const VkPipeline &pipe : m_PipesToDestroy)
      m_pDriver->vkDestroyPipeline(m_pDriver->GetDev(), pipe, NULL);
  }

  struct Pipelines
  {
    // Disable all tests, use the new render pass to render into a separate
    // attachment, and use fragment shader that outputs primitive ID.
    VkPipeline primitiveIdPipe;
    // Turn off blending.
    VkPipeline shaderOutPipe;
    // Enable blending to get post event values.
    VkPipeline postModPipe;
  };

  void PreDraw(uint32_t eid, VkCommandBuffer cmd)
  {
    if(m_EventFragments.find(eid) == m_EventFragments.end())
      return;

    VulkanRenderState prevState = m_pDriver->GetCmdRenderState();
    VulkanRenderState &state = m_pDriver->GetCmdRenderState();
    ResourceId curPipeline = state.graphics.pipeline;
    state.EndRenderPass(cmd);

    uint32_t numFragmentsInEvent = m_EventFragments[eid];

    VkRenderPass newRp = CreateRenderPass(state.renderPass, state.GetFramebuffer(), state.subpass,
                                          m_CallbackInfo.targetImage, VK_FORMAT_R32G32B32A32_SFLOAT);

    VkFramebuffer newFb =
        CreateFramebuffer(state.renderPass, newRp, state.subpass, state.GetFramebuffer(),
                          m_CallbackInfo.stencilImageView, m_CallbackInfo.subImageView);

    uint32_t framebufferIndex = 0;
    const rdcarray<ResourceId> &atts = prevState.GetFramebufferAttachments();
    for(uint32_t i = 0; i < atts.size(); i++)
    {
      ResourceId img = m_pDriver->GetDebugManager()->GetImageViewInfo(atts[i]).image;
      if(img == GetResID(m_CallbackInfo.targetImage))
      {
        framebufferIndex = i;
        break;
      }
    }

    Pipelines pipes = CreatePipelines(curPipeline, newRp, eid, false, 0, framebufferIndex);

    state.renderPass = GetResID(newRp);
    state.SetFramebuffer(m_pDriver, GetResID(newFb));

    VkPipeline pipesIter[2];
    pipesIter[0] = pipes.primitiveIdPipe;
    pipesIter[1] = pipes.shaderOutPipe;

    CopyPixelParams colourCopyParams = {};
    colourCopyParams.srcImage = m_CallbackInfo.subImage;
    colourCopyParams.srcImageLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;    // TODO: image layout
    colourCopyParams.srcImageFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

    const VulkanCreationInfo::Pipeline &p =
        m_pDriver->GetDebugManager()->GetPipelineInfo(prevState.graphics.pipeline);
    bool depthEnabled = p.depthTestEnable;

    // Get primitive ID and shader output value for each fragment.
    for(uint32_t f = 0; f < numFragmentsInEvent; f++)
    {
      for(uint32_t i = 0; i < 2; i++)
      {
        VkImageMemoryBarrier barrier = {
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            NULL,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED,
            Unwrap(m_CallbackInfo.stencilImage),
            {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1}};

        DoPipelineBarrier(cmd, 1, &barrier);

        // If depth is enabled, clear out the depth value so that the test always
        // passes. Depth writes only work if depth test is enabled.
        // Regardless also need to reset the stencil back to 0.
        VkClearDepthStencilValue dsValue = {};
        dsValue.depth = 1.0f;
        dsValue.stencil = 0;
        VkImageSubresourceRange range = {};
        range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        range.baseArrayLayer = 0;
        range.baseMipLevel = 0;
        range.layerCount = 1;
        range.levelCount = 1;
        ObjDisp(cmd)->CmdClearDepthStencilImage(Unwrap(cmd), Unwrap(m_CallbackInfo.stencilImage),
                                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dsValue, 1,
                                                &range);

        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        DoPipelineBarrier(cmd, 1, &barrier);

        m_pDriver->GetCmdRenderState().graphics.pipeline = GetResID(pipesIter[i]);

        m_pDriver->GetCmdRenderState().BeginRenderPassAndApplyState(
            m_pDriver, cmd, VulkanRenderState::BindGraphics);

        // Update stencil reference to the current fragment index, so that we get values
        // for a single fragment only.
        ObjDisp(cmd)->CmdSetStencilReference(Unwrap(cmd), VK_STENCIL_FACE_FRONT_AND_BACK, f);
        const DrawcallDescription *drawcall = m_pDriver->GetDrawcall(eid);
        if(drawcall->flags & DrawFlags::Indexed)
          ObjDisp(cmd)->CmdDrawIndexed(Unwrap(cmd), drawcall->numIndices, drawcall->numInstances,
                                       drawcall->indexOffset, drawcall->baseVertex,
                                       drawcall->instanceOffset);
        else
          ObjDisp(cmd)->CmdDraw(Unwrap(cmd), drawcall->numIndices, drawcall->numInstances,
                                drawcall->vertexOffset, drawcall->instanceOffset);
        state.EndRenderPass(cmd);

        uint32_t storeOffset = (fragsProcessed + f) * sizeof(PerFragmentInfo);
        if(i == 1)
        {
          storeOffset += offsetof(struct PerFragmentInfo, shaderOut);
          if(depthEnabled)
          {
            CopyPixelParams depthCopyParams = colourCopyParams;
            depthCopyParams.depthCopy = true;
            depthCopyParams.srcImage = m_CallbackInfo.stencilImage;
            depthCopyParams.srcImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthCopyParams.srcImageFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
            CopyImagePixel(cmd, depthCopyParams,
                           storeOffset + offsetof(struct PixelHistoryValue, depth));
          }
        }
        CopyImagePixel(cmd, colourCopyParams, storeOffset);
      }
    }

    VkImage depthImage = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    const DrawcallDescription *draw = m_pDriver->GetDrawcall(eid);
    if(draw && draw->depthOut != ResourceId())
    {
      ResourceId resId = m_pDriver->GetResourceManager()->GetLiveID(draw->depthOut);
      depthImage = m_pDriver->GetResourceManager()->GetCurrentHandle<VkImage>(resId);
      const VulkanCreationInfo::Image &imginfo = m_pDriver->GetDebugManager()->GetImageInfo(resId);
      depthFormat = imginfo.format;
    }

    // Restore the original framebuffer and render pass, so that we can get
    // post modification values.
    state.SetFramebuffer(prevState.GetFramebuffer(), prevState.GetFramebufferAttachments());
    state.renderPass = prevState.renderPass;
    colourCopyParams.srcImage = m_CallbackInfo.targetImage;
    colourCopyParams.srcImageFormat = m_CallbackInfo.targetImageFormat;

    // For every fragment except the last one, retrieve post-modification
    // value.
    for(uint32_t f = 0; f < numFragmentsInEvent - 1; f++)
    {
      // Get post-modification value, use the original framebuffer attachment.
      state.graphics.pipeline = GetResID(pipes.postModPipe);
      state.BeginRenderPassAndApplyState(m_pDriver, cmd, VulkanRenderState::BindGraphics);
      // Have to reset stencil.
      VkClearAttachment att = {};
      att.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
      VkClearRect rect = {};
      rect.rect.offset.x = m_CallbackInfo.x;
      rect.rect.offset.y = m_CallbackInfo.y;
      rect.rect.extent.width = 1;
      rect.rect.extent.height = 1;
      rect.baseArrayLayer = 0;
      rect.layerCount = 1;
      ObjDisp(cmd)->CmdClearAttachments(Unwrap(cmd), 1, &att, 1, &rect);
      ObjDisp(cmd)->CmdSetStencilReference(Unwrap(cmd), VK_STENCIL_FACE_FRONT_AND_BACK, f);
      const DrawcallDescription *drawcall = m_pDriver->GetDrawcall(eid);
      if(drawcall->flags & DrawFlags::Indexed)
        ObjDisp(cmd)->CmdDrawIndexed(Unwrap(cmd), drawcall->numIndices, drawcall->numInstances,
                                     drawcall->indexOffset, drawcall->baseVertex,
                                     drawcall->instanceOffset);
      else
        ObjDisp(cmd)->CmdDraw(Unwrap(cmd), drawcall->numIndices, drawcall->numInstances,
                              drawcall->vertexOffset, drawcall->instanceOffset);
      state.EndRenderPass(cmd);

      CopyImagePixel(cmd, colourCopyParams, (fragsProcessed + f) * sizeof(PerFragmentInfo) +
                                                offsetof(struct PerFragmentInfo, postMod));

      if(depthImage != VK_NULL_HANDLE)
      {
        CopyPixelParams depthCopyParams = colourCopyParams;
        depthCopyParams.depthCopy = true;
        depthCopyParams.srcImage = depthImage;
        depthCopyParams.srcImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthCopyParams.srcImageFormat = depthFormat;
        CopyImagePixel(cmd, depthCopyParams, (fragsProcessed + f) * sizeof(PerFragmentInfo) +
                                                 offsetof(struct PerFragmentInfo, postMod) +
                                                 offsetof(struct PixelHistoryValue, depth));
      }
    }

    m_EventIndices[eid] = fragsProcessed;
    fragsProcessed += numFragmentsInEvent;

    m_pDriver->GetCmdRenderState() = prevState;
    m_pDriver->GetCmdRenderState().BeginRenderPassAndApplyState(m_pDriver, cmd,
                                                                VulkanRenderState::BindGraphics);
  }
  bool PostDraw(uint32_t eid, VkCommandBuffer cmd) { return false; }
  void PostRedraw(uint32_t eid, VkCommandBuffer cmd) {}
  // CreatePipelines for getting per fragment information.
  Pipelines CreatePipelines(ResourceId pipe, VkRenderPass rp, uint32_t eid, bool dynamicScissor,
                            uint32_t fragmentIndex, uint32_t framebufferIndex)
  {
    const VulkanCreationInfo::Pipeline &p = m_pDriver->GetDebugManager()->GetPipelineInfo(pipe);
    VkGraphicsPipelineCreateInfo pipeCreateInfo = {};
    rdcarray<VkPipelineShaderStageCreateInfo> stages;
    m_pDriver->GetShaderCache()->MakeGraphicsPipelineInfo(pipeCreateInfo, pipe);

    VkPipelineDepthStencilStateCreateInfo *ds =
        (VkPipelineDepthStencilStateCreateInfo *)pipeCreateInfo.pDepthStencilState;
    VkPipelineMultisampleStateCreateInfo *ms =
        (VkPipelineMultisampleStateCreateInfo *)pipeCreateInfo.pMultisampleState;
    VkPipelineViewportStateCreateInfo *vs =
        (VkPipelineViewportStateCreateInfo *)pipeCreateInfo.pViewportState;

    VkRect2D newScissors[16];
    memset(newScissors, 0, sizeof(newScissors));
    // Modify the stencil state, so that only one fragment passes.
    {
      ds->stencilTestEnable = VK_TRUE;
      ds->front.compareOp = VK_COMPARE_OP_EQUAL;
      ds->front.failOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
      ds->front.passOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
      ds->front.depthFailOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
      ds->front.compareMask = 0xff;
      ds->front.writeMask = 0xff;
      ds->front.reference = 0;
      ds->back = ds->front;

      ms->pSampleMask = &m_CallbackInfo.sampleMask;

      // Change scissors unless they are set dynamically.
      if(p.dynamicStates[VkDynamicScissor])
      {
        VulkanRenderState &pipestate = m_pDriver->GetCmdRenderState();
        for(uint32_t i = 0; i < pipestate.views.size(); i++)
          ScissorToPixel(pipestate.views[i], pipestate.scissors[i]);
      }
      else
      {
        for(uint32_t i = 0; i < vs->viewportCount; i++)
        {
          ScissorToPixel(vs->pViewports[i], newScissors[i]);
        }
        vs->pScissors = newScissors;
      }
    }

    // TODO: this is wrong, should take into account subpass.
    pipeCreateInfo.subpass = 0;

    stages.resize(pipeCreateInfo.stageCount);
    memcpy(stages.data(), pipeCreateInfo.pStages, stages.byteSize());

    EventFlags eventFlags = m_pDriver->GetEventFlags(eid);
    VkShaderModule replacementShaders[5] = {};

    // Clean shaders
    uint32_t numberOfStages = 5;
    for(size_t i = 0; i < numberOfStages; i++)
    {
      if((eventFlags & PipeStageRWEventFlags(StageFromIndex(i))) != EventFlags::NoFlags)
        replacementShaders[i] =
            m_ShaderCache->GetShaderWithoutSideEffects(p.shaders[i].module, p.shaders[i].entryPoint);
    }
    for(uint32_t i = 0; i < pipeCreateInfo.stageCount; i++)
    {
      VkShaderModule replacement = replacementShaders[StageIndex(stages[i].stage)];
      if(replacement != VK_NULL_HANDLE)
        stages[i].module = replacement;
    }
    pipeCreateInfo.pStages = stages.data();

    VkPipelineDynamicStateCreateInfo *dynState =
        (VkPipelineDynamicStateCreateInfo *)pipeCreateInfo.pDynamicState;

    rdcarray<VkDynamicState> dynamicStates;
    RDCASSERT(dynState != NULL);
    if(!p.dynamicStates[VkDynamicStencilReference])
    {
      dynamicStates.resize(dynState->dynamicStateCount);
      memcpy(dynamicStates.data(), dynState->pDynamicStates, dynamicStates.byteSize());
      dynamicStates.push_back(VK_DYNAMIC_STATE_STENCIL_REFERENCE);
      dynState->dynamicStateCount = (uint32_t)dynamicStates.size();
      dynState->pDynamicStates = dynamicStates.data();
    }

    Pipelines pipes = {};
    VkResult vkr = m_pDriver->vkCreateGraphicsPipelines(m_pDriver->GetDev(), VK_NULL_HANDLE, 1,
                                                        &pipeCreateInfo, NULL, &pipes.postModPipe);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);
    m_PipesToDestroy.push_back(pipes.postModPipe);

    VkPipelineRasterizationStateCreateInfo *rs =
        (VkPipelineRasterizationStateCreateInfo *)pipeCreateInfo.pRasterizationState;
    // Disable some tests, leave depthTest and depthWriteEnable as is.
    // If we disable depth test, depth information would not be written.
    {
      rs->cullMode = VK_CULL_MODE_NONE;
      rs->rasterizerDiscardEnable = VK_FALSE;
      ds->depthBoundsTestEnable = VK_FALSE;
      if(ds->depthTestEnable)
        ds->depthCompareOp = VK_COMPARE_OP_ALWAYS;
    }

    pipeCreateInfo.renderPass = rp;
    VkPipelineColorBlendStateCreateInfo *cbs =
        (VkPipelineColorBlendStateCreateInfo *)pipeCreateInfo.pColorBlendState;
    // Turn off blending so that we can get shader output values.
    VkPipelineColorBlendAttachmentState *atts =
        (VkPipelineColorBlendAttachmentState *)cbs->pAttachments;
    for(uint32_t i = 0; i < cbs->attachmentCount; i++)
    {
      if(i == framebufferIndex)
      {
        atts[i].blendEnable = 0;
        atts[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
      }
      else
      {
        atts[i].colorWriteMask = 0;
      }
    }

    vkr = m_pDriver->vkCreateGraphicsPipelines(m_pDriver->GetDev(), VK_NULL_HANDLE, 1,
                                               &pipeCreateInfo, NULL, &pipes.shaderOutPipe);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);

    m_PipesToDestroy.push_back(pipes.shaderOutPipe);

    {
      ds->depthTestEnable = VK_FALSE;
      ds->depthWriteEnable = VK_FALSE;
    }

    // Output the primitive ID.
    VkPipelineShaderStageCreateInfo stageCI = {};
    stageCI.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageCI.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stageCI.module = m_ShaderCache->GetPrimitiveIdShader(framebufferIndex);
    stageCI.pName = "main";
    bool fsFound = false;
    for(uint32_t i = 0; i < pipeCreateInfo.stageCount; i++)
    {
      if(stages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
      {
        stages[i] = stageCI;
        fsFound = true;
        break;
      }
    }
    if(!fsFound)
    {
      stages.push_back(stageCI);
      pipeCreateInfo.stageCount = (uint32_t)stages.size();
      pipeCreateInfo.pStages = stages.data();
    }

    vkr = m_pDriver->vkCreateGraphicsPipelines(m_pDriver->GetDev(), VK_NULL_HANDLE, 1,
                                               &pipeCreateInfo, NULL, &pipes.primitiveIdPipe);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);
    m_PipesToDestroy.push_back(pipes.primitiveIdPipe);

    return pipes;
  }

  void PreDispatch(uint32_t eid, VkCommandBuffer cmd) {}
  bool PostDispatch(uint32_t eid, VkCommandBuffer cmd) { return false; }
  void PostRedispatch(uint32_t eid, VkCommandBuffer cmd) {}
  void PreMisc(uint32_t eid, DrawFlags flags, VkCommandBuffer cmd) {}
  bool PostMisc(uint32_t eid, DrawFlags flags, VkCommandBuffer cmd) { return false; }
  void PostRemisc(uint32_t eid, DrawFlags flags, VkCommandBuffer cmd) {}
  void PreEndCommandBuffer(VkCommandBuffer cmd) {}
  void AliasEvent(uint32_t primary, uint32_t alias) {}
  bool SplitSecondary() { return false; }
  void PreCmdExecute(uint32_t baseEid, uint32_t secondaryFirst, uint32_t secondaryLast,
                     VkCommandBuffer cmd)
  {
  }
  void PostCmdExecute(uint32_t baseEid, uint32_t secondaryFirst, uint32_t secondaryLast,
                      VkCommandBuffer cmd)
  {
  }

  uint32_t GetEventOffset(uint32_t eid)
  {
    auto it = m_EventIndices.find(eid);
    RDCASSERT(it != m_EventIndices.end());
    return it->second;
  }

private:
  // For each event, specifies where the occlusion query results start.
  std::map<uint32_t, uint32_t> m_EventIndices;
  // Number of fragments for each event.
  std::map<uint32_t, uint32_t> m_EventFragments;
  // Number of fragments processed so far.
  uint32_t fragsProcessed = 0;

  rdcarray<VkPipeline> m_PipesToDestroy;
};

// Callback used to determine the shader discard status for each fragment, where
// an event has multiple fragments with some being discarded in a fragment shader.
struct VulkanPixelHistoryDiscardedFragmentsCallback : VulkanPixelHistoryCallback
{
  // Key is event ID and value is a list of primitive IDs
  std::map<uint32_t, rdcarray<int32_t> > m_Events;
  VulkanPixelHistoryDiscardedFragmentsCallback(WrappedVulkan *vk,
                                               PixelHistoryShaderCache *shaderCache,
                                               const PixelHistoryCallbackInfo &callbackInfo,
                                               std::map<uint32_t, rdcarray<int32_t> > events,
                                               VkQueryPool occlusionPool)
      : VulkanPixelHistoryCallback(vk, shaderCache, callbackInfo, occlusionPool), m_Events(events)
  {
  }

  ~VulkanPixelHistoryDiscardedFragmentsCallback()
  {
    for(const VkPipeline &pipe : m_PipesToDestroy)
      m_pDriver->vkDestroyPipeline(m_pDriver->GetDev(), pipe, NULL);
  }

  void PreDraw(uint32_t eid, VkCommandBuffer cmd)
  {
    if(m_Events.find(eid) == m_Events.end())
      return;

    const rdcarray<int32_t> primIds = m_Events[eid];

    VulkanRenderState prevState = m_pDriver->GetCmdRenderState();
    VulkanRenderState &state = m_pDriver->GetCmdRenderState();
    // Create a pipeline with a scissor and colorWriteMask = 0, and disable all tests.
    VkPipeline newPipe = CreatePipeline(state.graphics.pipeline, eid);
    state.graphics.pipeline = GetResID(newPipe);
    state.BindPipeline(m_pDriver, cmd, VulkanRenderState::BindGraphics, false);
    for(uint32_t i = 0; i < primIds.size(); i++)
    {
      uint32_t queryId = (uint32_t)m_OcclusionIndices.size();
      ObjDisp(cmd)->CmdBeginQuery(Unwrap(cmd), m_OcclusionPool, queryId, 0);
      const DrawcallDescription *drawcall = m_pDriver->GetDrawcall(eid);
      uint32_t primId = primIds[i];
      // TODO once pixel history distinguishes between instances, draw only the instance for
      // this fragment
      if(drawcall->flags & DrawFlags::Indexed)
        ObjDisp(cmd)->CmdDrawIndexed(
            Unwrap(cmd), RENDERDOC_NumVerticesPerPrimitive(drawcall->topology),
            RDCMAX(1U, drawcall->numInstances),
            drawcall->indexOffset + RENDERDOC_VertexOffset(drawcall->topology, primId),
            drawcall->baseVertex, drawcall->instanceOffset);
      else
        ObjDisp(cmd)->CmdDraw(
            Unwrap(cmd), RENDERDOC_NumVerticesPerPrimitive(drawcall->topology),
            RDCMAX(1U, drawcall->numInstances),
            drawcall->vertexOffset + RENDERDOC_VertexOffset(drawcall->topology, primId),
            drawcall->instanceOffset);
      ObjDisp(cmd)->CmdEndQuery(Unwrap(cmd), m_OcclusionPool, queryId);

      m_OcclusionIndices[make_rdcpair<uint32_t, uint32_t>(eid, primId)] = queryId;
    }
    m_pDriver->GetCmdRenderState() = prevState;
    m_pDriver->GetCmdRenderState().BindPipeline(m_pDriver, cmd, VulkanRenderState::BindGraphics,
                                                false);
  }

  VkPipeline CreatePipeline(ResourceId pipe, uint32_t eid)
  {
    rdcarray<VkPipelineShaderStageCreateInfo> stages;
    VkGraphicsPipelineCreateInfo pipeCreateInfo = {};
    MakeAllPassIncrementStencilPipelineCI(eid, pipe, pipeCreateInfo, stages);

    VkPipelineDepthStencilStateCreateInfo *ds =
        (VkPipelineDepthStencilStateCreateInfo *)pipeCreateInfo.pDepthStencilState;
    VkPipelineColorBlendStateCreateInfo *cbs =
        (VkPipelineColorBlendStateCreateInfo *)pipeCreateInfo.pColorBlendState;

    {
      // Disable all tests, but stencil.
      ds->stencilTestEnable = VK_FALSE;

      VkPipelineColorBlendAttachmentState *atts =
          (VkPipelineColorBlendAttachmentState *)cbs->pAttachments;
      for(uint32_t i = 0; i < cbs->attachmentCount; i++)
        atts[i].colorWriteMask = 0;
    }

    VkPipeline newPipe;
    VkResult vkr = m_pDriver->vkCreateGraphicsPipelines(m_pDriver->GetDev(), VK_NULL_HANDLE, 1,
                                                        &pipeCreateInfo, NULL, &newPipe);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);
    m_PipesToDestroy.push_back(newPipe);
    return newPipe;
  }

  void FetchOcclusionResults()
  {
    m_OcclusionResults.resize(m_OcclusionIndices.size());
    VkResult vkr = ObjDisp(m_pDriver->GetDev())
                       ->GetQueryPoolResults(Unwrap(m_pDriver->GetDev()), m_OcclusionPool, 0,
                                             (uint32_t)m_OcclusionIndices.size(),
                                             m_OcclusionResults.byteSize(),
                                             m_OcclusionResults.data(), sizeof(uint64_t),
                                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);
  }

  bool PrimitiveDiscarded(uint32_t eid, uint32_t primId)
  {
    auto it = m_OcclusionIndices.find(make_rdcpair<uint32_t, uint32_t>(eid, primId));
    if(it == m_OcclusionIndices.end())
      return false;
    return m_OcclusionResults[it->second] == 0;
  }

  bool PostDraw(uint32_t eid, VkCommandBuffer cmd) { return false; }
  void PostRedraw(uint32_t eid, VkCommandBuffer cmd) {}
  void PreDispatch(uint32_t eid, VkCommandBuffer cmd) {}
  bool PostDispatch(uint32_t eid, VkCommandBuffer cmd) { return false; }
  void PostRedispatch(uint32_t eid, VkCommandBuffer cmd) {}
  void PreMisc(uint32_t eid, DrawFlags flags, VkCommandBuffer cmd) {}
  bool PostMisc(uint32_t eid, DrawFlags flags, VkCommandBuffer cmd) { return false; }
  void PostRemisc(uint32_t eid, DrawFlags flags, VkCommandBuffer cmd) {}
  void PreEndCommandBuffer(VkCommandBuffer cmd) {}
  void AliasEvent(uint32_t primary, uint32_t alias) {}
  bool SplitSecondary() { return false; }
  void PreCmdExecute(uint32_t baseEid, uint32_t secondaryFirst, uint32_t secondaryLast,
                     VkCommandBuffer cmd)
  {
  }
  void PostCmdExecute(uint32_t baseEid, uint32_t secondaryFirst, uint32_t secondaryLast,
                      VkCommandBuffer cmd)
  {
  }

private:
  std::map<rdcpair<uint32_t, uint32_t>, uint32_t> m_OcclusionIndices;
  rdcarray<uint64_t> m_OcclusionResults;

  rdcarray<VkPipeline> m_PipesToDestroy;
};

bool VulkanDebugManager::PixelHistorySetupResources(PixelHistoryResources &resources,
                                                    VkImage targetImage, VkExtent3D extent,
                                                    VkFormat format, VkSampleCountFlagBits samples,
                                                    const Subresource &sub, uint32_t numEvents)
{
  VkImage colorImage;
  VkImageView colorImageView;
  VkImage stencilImage;
  VkImageView stencilImageView;

  VkImage stagingImage = VK_NULL_HANDLE;
  VkImageView stagingImageView = VK_NULL_HANDLE;
  VkDeviceSize stagingImageOffset = 0;
  VkImage stencilStagingImage = VK_NULL_HANDLE;
  VkImageView stencilStagingImageView = VK_NULL_HANDLE;
  VkDeviceSize stencilStagingImageOffset = 0;
  VkImageView targetImageView = VK_NULL_HANDLE;

  VkDeviceMemory gpuMem;

  VkBuffer dstBuffer;
  VkDeviceMemory bufferMemory;

  VkResult vkr;
  VkDevice dev = m_pDriver->GetDev();

  VkDeviceSize totalMemorySize = 0;

  // Create Images
  VkImageCreateInfo imgInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  imgInfo.imageType = VK_IMAGE_TYPE_2D;
  imgInfo.mipLevels = 1;
  imgInfo.arrayLayers = 1;
  imgInfo.samples = samples;
  imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  // Device local resources:
  imgInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  imgInfo.extent.width = extent.width;
  imgInfo.extent.height = extent.height;
  imgInfo.extent.depth = 1;
  imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

  vkr = m_pDriver->vkCreateImage(dev, &imgInfo, NULL, &colorImage);
  RDCASSERTEQUAL(vkr, VK_SUCCESS);

  ImageState colorImageState = ImageState(colorImage, ImageInfo(imgInfo), eFrameRef_None);

  VkMemoryRequirements colorImageMrq = {0};
  m_pDriver->vkGetImageMemoryRequirements(dev, colorImage, &colorImageMrq);
  totalMemorySize = colorImageMrq.size;

  imgInfo.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
  imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

  vkr = m_pDriver->vkCreateImage(dev, &imgInfo, NULL, &stencilImage);
  RDCASSERTEQUAL(vkr, VK_SUCCESS);

  ImageState stencilImageState = ImageState(stencilImage, ImageInfo(imgInfo), eFrameRef_None);

  VkMemoryRequirements stencilImageMrq = {0};
  m_pDriver->vkGetImageMemoryRequirements(dev, stencilImage, &stencilImageMrq);
  VkDeviceSize offset = AlignUp(totalMemorySize, stencilImageMrq.alignment);
  totalMemorySize = offset + stencilImageMrq.size;

  if(samples != VK_SAMPLE_COUNT_1_BIT)
  {
    imgInfo.format = format;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    vkr = m_pDriver->vkCreateImage(dev, &imgInfo, NULL, &stagingImage);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);

    VkMemoryRequirements msImageMrq = {0};
    m_pDriver->vkGetImageMemoryRequirements(dev, stagingImage, &msImageMrq);
    stagingImageOffset = AlignUp(totalMemorySize, msImageMrq.alignment);
    totalMemorySize = stagingImageOffset + msImageMrq.size;

    imgInfo.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    vkr = m_pDriver->vkCreateImage(dev, &imgInfo, NULL, &stencilStagingImage);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);

    VkMemoryRequirements stencilStagingImageMrq = {0};
    m_pDriver->vkGetImageMemoryRequirements(dev, stencilStagingImage, &stencilStagingImageMrq);
    stencilStagingImageOffset = AlignUp(totalMemorySize, stencilStagingImageMrq.alignment);
    totalMemorySize = stencilStagingImageOffset + stencilStagingImageMrq.size;
  }

  VkMemoryAllocateInfo allocInfo = {
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, totalMemorySize,
      m_pDriver->GetGPULocalMemoryIndex(colorImageMrq.memoryTypeBits),
  };
  vkr = m_pDriver->vkAllocateMemory(m_Device, &allocInfo, NULL, &gpuMem);
  RDCASSERTEQUAL(vkr, VK_SUCCESS);

  vkr = m_pDriver->vkBindImageMemory(m_Device, colorImage, gpuMem, 0);
  RDCASSERTEQUAL(vkr, VK_SUCCESS);

  vkr = m_pDriver->vkBindImageMemory(m_Device, stencilImage, gpuMem, offset);
  RDCASSERTEQUAL(vkr, VK_SUCCESS);

  VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  viewInfo.image = colorImage;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  vkr = m_pDriver->vkCreateImageView(m_Device, &viewInfo, NULL, &colorImageView);
  RDCASSERTEQUAL(vkr, VK_SUCCESS);

  viewInfo.image = stencilImage;
  viewInfo.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};

  vkr = m_pDriver->vkCreateImageView(m_Device, &viewInfo, NULL, &stencilImageView);
  RDCASSERTEQUAL(vkr, VK_SUCCESS);

  if(samples != VK_SAMPLE_COUNT_1_BIT)
  {
    vkr = m_pDriver->vkBindImageMemory(m_Device, stagingImage, gpuMem, stagingImageOffset);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);

    viewInfo.image = stagingImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
                                 VK_REMAINING_ARRAY_LAYERS};

    uint32_t bs = GetByteSize(1, 1, 1, format, 0);

    if(bs == 1)
      viewInfo.format = VK_FORMAT_R8_UINT;
    else if(bs == 2)
      viewInfo.format = VK_FORMAT_R16_UINT;
    else if(bs == 4)
      viewInfo.format = VK_FORMAT_R32_UINT;
    else if(bs == 8)
      viewInfo.format = VK_FORMAT_R32G32_UINT;
    else if(bs == 16)
      viewInfo.format = VK_FORMAT_R32G32B32A32_UINT;

    if(viewInfo.format == VK_FORMAT_UNDEFINED)
    {
      RDCERR("Can't copy 2D to Array with format %s", ToStr(format).c_str());
    }

    vkr = m_pDriver->vkCreateImageView(m_Device, &viewInfo, NULL, &stagingImageView);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);

    viewInfo.image = targetImage;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, sub.mip, 1, sub.slice, 1};
    vkr = m_pDriver->vkCreateImageView(m_Device, &viewInfo, NULL, &targetImageView);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);

    viewInfo.image = stencilStagingImage;
    viewInfo.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    viewInfo.subresourceRange.baseArrayLayer = 0;

    vkr = m_pDriver->vkBindImageMemory(m_Device, stencilStagingImage, gpuMem,
                                       stencilStagingImageOffset);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);

    vkr = m_pDriver->vkCreateImageView(m_Device, &viewInfo, NULL, &stencilStagingImageView);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);
  }

  VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bufferInfo.size = AlignUp((uint32_t)(numEvents * sizeof(EventInfo)), 4096U);
  bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

  vkr = m_pDriver->vkCreateBuffer(m_Device, &bufferInfo, NULL, &dstBuffer);
  RDCASSERTEQUAL(vkr, VK_SUCCESS);

  // Allocate memory
  VkMemoryRequirements mrq = {};
  m_pDriver->vkGetBufferMemoryRequirements(m_Device, dstBuffer, &mrq);
  allocInfo.allocationSize = mrq.size;
  allocInfo.memoryTypeIndex = m_pDriver->GetReadbackMemoryIndex(mrq.memoryTypeBits);
  vkr = m_pDriver->vkAllocateMemory(m_Device, &allocInfo, NULL, &bufferMemory);
  RDCASSERTEQUAL(vkr, VK_SUCCESS);

  vkr = m_pDriver->vkBindBufferMemory(m_Device, dstBuffer, bufferMemory, 0);
  RDCASSERTEQUAL(vkr, VK_SUCCESS);

  VkCommandBuffer cmd = m_pDriver->GetNextCmd();
  VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};

  vkr = ObjDisp(dev)->BeginCommandBuffer(Unwrap(cmd), &beginInfo);
  RDCASSERTEQUAL(vkr, VK_SUCCESS);
  ObjDisp(cmd)->CmdFillBuffer(Unwrap(cmd), Unwrap(dstBuffer), 0, VK_WHOLE_SIZE, 0);
  colorImageState.InlineTransition(
      cmd, m_pDriver->m_QueueFamilyIdx, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, m_pDriver->GetImageTransitionInfo());
  stencilImageState.InlineTransition(
      cmd, m_pDriver->m_QueueFamilyIdx, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, m_pDriver->GetImageTransitionInfo());

  vkr = ObjDisp(dev)->EndCommandBuffer(Unwrap(cmd));
  RDCASSERTEQUAL(vkr, VK_SUCCESS);
  m_pDriver->SubmitCmds();
  m_pDriver->FlushQ();

  resources.colorImage = colorImage;
  resources.colorImageView = colorImageView;
  resources.stencilImage = stencilImage;
  resources.stencilImageView = stencilImageView;
  resources.stagingImage = stagingImage;
  resources.stagingImageView = stagingImageView;
  resources.stencilStagingImage = stencilStagingImage;
  resources.stencilStagingImageView = stencilStagingImageView;
  resources.targetImageView = targetImageView;
  resources.gpuMem = gpuMem;

  resources.bufferMemory = bufferMemory;
  resources.dstBuffer = dstBuffer;

  return true;
}

void VulkanReplay::UpdatePixelHistoryDescriptor(VkImageView sourceView, VkImageView destView)
{
  VkDescriptorImageInfo srcdesc = {0};
  srcdesc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  srcdesc.imageView = Unwrap(sourceView);
  srcdesc.sampler = Unwrap(m_General.PointSampler);    // not used - we use texelFetch

  VkDescriptorImageInfo destdesc = {0};
  destdesc.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  destdesc.imageView = Unwrap(destView);

  VkWriteDescriptorSet writeSet[] = {
      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, Unwrap(m_PixelHistory.MSCopyDescSet), 0, 0, 1,
       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &srcdesc, NULL, NULL},
      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, Unwrap(m_PixelHistory.MSCopyDescSet), 2, 0, 1,
       VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &destdesc, NULL, NULL},
  };

  ObjDisp(m_pDriver->GetDev())
      ->UpdateDescriptorSets(Unwrap(m_pDriver->GetDev()), ARRAY_COUNT(writeSet), writeSet, 0, NULL);
}

bool VulkanDebugManager::PixelHistoryDestroyResources(const PixelHistoryResources &r)
{
  VkDevice dev = m_pDriver->GetDev();
  if(r.gpuMem != VK_NULL_HANDLE)
    m_pDriver->vkFreeMemory(dev, r.gpuMem, NULL);
  if(r.colorImage != VK_NULL_HANDLE)
    m_pDriver->vkDestroyImage(dev, r.colorImage, NULL);
  if(r.colorImageView != VK_NULL_HANDLE)
    m_pDriver->vkDestroyImageView(dev, r.colorImageView, NULL);
  if(r.stencilImage != VK_NULL_HANDLE)
    m_pDriver->vkDestroyImage(dev, r.stencilImage, NULL);
  if(r.stencilImageView != VK_NULL_HANDLE)
    m_pDriver->vkDestroyImageView(dev, r.stencilImageView, NULL);
  if(r.stagingImage != VK_NULL_HANDLE)
    m_pDriver->vkDestroyImage(dev, r.stagingImage, NULL);
  if(r.stagingImageView != VK_NULL_HANDLE)
    m_pDriver->vkDestroyImageView(dev, r.stagingImageView, NULL);
  if(r.stencilStagingImage != VK_NULL_HANDLE)
    m_pDriver->vkDestroyImage(dev, r.stencilStagingImage, NULL);
  if(r.stencilStagingImageView != VK_NULL_HANDLE)
    m_pDriver->vkDestroyImageView(dev, r.stencilStagingImageView, NULL);
  if(r.dstBuffer != VK_NULL_HANDLE)
    m_pDriver->vkDestroyBuffer(dev, r.dstBuffer, NULL);
  if(r.bufferMemory != VK_NULL_HANDLE)
    m_pDriver->vkFreeMemory(dev, r.bufferMemory, NULL);
  return true;
}

void CreateOcclusionPool(WrappedVulkan *vk, uint32_t poolSize, VkQueryPool *pQueryPool)
{
  VkDevice dev = vk->GetDev();
  VkQueryPoolCreateInfo occlusionPoolCreateInfo = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  occlusionPoolCreateInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
  occlusionPoolCreateInfo.queryCount = poolSize;
  // TODO: check that occlusion feature is available
  VkResult vkr =
      ObjDisp(dev)->CreateQueryPool(Unwrap(dev), &occlusionPoolCreateInfo, NULL, pQueryPool);
  RDCASSERTEQUAL(vkr, VK_SUCCESS);
  VkCommandBuffer cmd = vk->GetNextCmd();
  VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};

  vkr = ObjDisp(dev)->BeginCommandBuffer(Unwrap(cmd), &beginInfo);
  RDCASSERTEQUAL(vkr, VK_SUCCESS);
  ObjDisp(dev)->CmdResetQueryPool(Unwrap(cmd), *pQueryPool, 0, poolSize);
  vkr = ObjDisp(dev)->EndCommandBuffer(Unwrap(cmd));
  RDCASSERTEQUAL(vkr, VK_SUCCESS);
  vk->SubmitCmds();
  vk->FlushQ();
}

VkImageLayout VulkanDebugManager::GetImageLayout(ResourceId image, VkImageAspectFlagBits aspect,
                                                 uint32_t mip, uint32_t slice)
{
  auto state = m_pDriver->FindConstImageState(image);
  if(!state)
  {
    RDCERR("Could not find image state for %s", ToStr(image).c_str());
    return VK_IMAGE_LAYOUT_UNDEFINED;
  }
  if(state->GetImageInfo().extent.depth > 1)
    return state->GetImageLayout(aspect, mip, 0);
  else
    return state->GetImageLayout(aspect, mip, slice);
}

void UpdateTestsFailed(const TestsFailedCallback *tfCb, uint32_t eventId, uint32_t eventFlags,
                       PixelModification &mod)
{
  bool earlyFragmentTests = tfCb->HasEarlyFragments(eventId);

  if((eventFlags & (TestEnabled_Culling | TestMustFail_Culling)) == TestEnabled_Culling)
  {
    uint64_t occlData = tfCb->GetOcclusionResult(eventId, TestEnabled_Culling);
    mod.backfaceCulled = (occlData == 0);
  }

  if(mod.backfaceCulled)
    return;

  if((eventFlags & (TestEnabled_Scissor | TestMustPass_Scissor | TestMustFail_Scissor)) ==
     TestEnabled_Scissor)
  {
    uint64_t occlData = tfCb->GetOcclusionResult(eventId, TestEnabled_Scissor);
    mod.scissorClipped = (occlData == 0);
  }
  if(mod.scissorClipped)
    return;

  // TODO: Exclusive Scissor Test if NV extension is turned on.

  if((eventFlags & (TestEnabled_SampleMask | TestMustFail_SampleMask)) == TestEnabled_SampleMask)
  {
    uint64_t occlData = tfCb->GetOcclusionResult(eventId, TestEnabled_SampleMask);
    mod.sampleMasked = (occlData == 0);
  }
  if(mod.sampleMasked)
    return;

  // Shader discard with default fragment tests order.
  if(!earlyFragmentTests)
  {
    uint64_t occlData = tfCb->GetOcclusionResult(eventId, TestEnabled_FragmentDiscard);
    mod.shaderDiscarded = (occlData == 0);
    if(mod.shaderDiscarded)
      return;
  }

  if(eventFlags & TestEnabled_DepthBounds)
  {
    uint64_t occlData = tfCb->GetOcclusionResult(eventId, TestEnabled_DepthBounds);
    mod.depthClipped = (occlData == 0);
  }
  if(mod.depthClipped)
    return;

  if((eventFlags & (TestEnabled_StencilTesting | TestMustFail_StencilTesting)) ==
     TestEnabled_StencilTesting)
  {
    uint64_t occlData = tfCb->GetOcclusionResult(eventId, TestEnabled_StencilTesting);
    mod.stencilTestFailed = (occlData == 0);
  }
  if(mod.stencilTestFailed)
    return;

  if((eventFlags & (TestEnabled_DepthTesting | TestMustFail_DepthTesting)) == TestEnabled_DepthTesting)
  {
    uint64_t occlData = tfCb->GetOcclusionResult(eventId, TestEnabled_DepthTesting);
    mod.depthTestFailed = (occlData == 0);
  }
  if(mod.depthTestFailed)
    return;

  // Shader discard with early fragment tests order.
  if(earlyFragmentTests)
  {
    uint64_t occlData = tfCb->GetOcclusionResult(eventId, TestEnabled_FragmentDiscard);
    mod.shaderDiscarded = (occlData == 0);
  }
}

void FillInColor(ResourceFormat fmt, const PixelHistoryValue &value, ModificationValue &mod)
{
  FloatVector v4 = ConvertComponents(fmt, value.color);
  memcpy(mod.col.floatValue, &v4.x, sizeof(v4));
}

rdcarray<PixelModification> VulkanReplay::PixelHistory(rdcarray<EventUsage> events,
                                                       ResourceId target, uint32_t x, uint32_t y,
                                                       const Subresource &sub, CompType typeCast)
{
  if(!GetAPIProperties().pixelHistory)
  {
    VULKANNOTIMP("PixelHistory");
    return rdcarray<PixelModification>();
  }

  RDCDEBUG("PixelHistory: pixel: (%u, %u) with %u events", x, y, events.size());
  rdcarray<PixelModification> history;

  if(events.empty())
    return history;

  const VulkanCreationInfo::Image &imginfo = GetDebugManager()->GetImageInfo(target);
  if(imginfo.format == VK_FORMAT_UNDEFINED)
    return history;

  uint32_t sampleIdx = sub.sample;

  // TODO: use the given type hint for typeless textures
  SCOPED_TIMER("VkDebugManager::PixelHistory");

  if(sampleIdx > (uint32_t)imginfo.samples)
    sampleIdx = 0;

  uint32_t sampleMask = ~0U;
  if(sampleIdx < 32)
    sampleMask = 1U << sampleIdx;

  bool multisampled = (imginfo.samples > 1);

  if(sampleIdx == ~0U || !multisampled)
    sampleIdx = 0;

  VkDevice dev = m_pDriver->GetDev();
  VkQueryPool occlusionPool;
  CreateOcclusionPool(m_pDriver, (uint32_t)events.size(), &occlusionPool);

  PixelHistoryResources resources = {};
  // TODO: perhaps should do this after making an occlusion query, since we will
  // get a smaller subset of events that passed the occlusion query.
  VkImage targetImage = GetResourceManager()->GetCurrentHandle<VkImage>(target);
  GetDebugManager()->PixelHistorySetupResources(resources, targetImage, imginfo.extent,
                                                imginfo.format, imginfo.samples, sub,
                                                (uint32_t)events.size());
  if(multisampled)
    UpdatePixelHistoryDescriptor(resources.targetImageView, resources.stagingImageView);

  PixelHistoryShaderCache *shaderCache = new PixelHistoryShaderCache(m_pDriver);

  PixelHistoryCallbackInfo callbackInfo = {};
  callbackInfo.targetImage = targetImage;
  callbackInfo.targetImageFormat = imginfo.format;
  callbackInfo.layers = imginfo.arrayLayers;
  callbackInfo.mipLevels = imginfo.mipLevels;
  callbackInfo.samples = imginfo.samples;
  callbackInfo.extent = imginfo.extent;
  callbackInfo.targetSubresource = sub;
  callbackInfo.x = x;
  callbackInfo.y = y;
  callbackInfo.sampleMask = sampleMask;
  callbackInfo.subImage = resources.colorImage;
  callbackInfo.subImageView = resources.colorImageView;
  callbackInfo.stencilImage = resources.stencilImage;
  callbackInfo.stencilImageView = resources.stencilImageView;
  callbackInfo.dstBuffer = resources.dstBuffer;
  callbackInfo.stagingImage = resources.stagingImage;
  callbackInfo.stencilStagingImage = resources.stencilStagingImage;

  VulkanOcclusionCallback occlCb(m_pDriver, shaderCache, callbackInfo, occlusionPool, events);
  m_pDriver->ReplayLog(0, events.back().eventId, eReplay_Full);
  m_pDriver->SubmitCmds();
  m_pDriver->FlushQ();
  occlCb.FetchOcclusionResults();

  // Gather all draw events that could have written to pixel for another replay pass,
  // to determine if these draws failed for some reason (for ex., depth test).
  rdcarray<uint32_t> modEvents;
  rdcarray<uint32_t> drawEvents;
  for(size_t ev = 0; ev < events.size(); ev++)
  {
    bool clear = (events[ev].usage == ResourceUsage::Clear);
    bool directWrite = isDirectWrite(events[ev].usage);

    if(events[ev].view != ResourceId())
    {
      // TODO
    }

    if(directWrite || clear)
    {
      modEvents.push_back(events[ev].eventId);
    }
    else
    {
      uint64_t occlData = occlCb.GetOcclusionResult((uint32_t)events[ev].eventId);
      if(occlData > 0)
      {
        drawEvents.push_back(events[ev].eventId);
        modEvents.push_back(events[ev].eventId);
      }
    }
  }

  VulkanColorAndStencilCallback cb(m_pDriver, shaderCache, callbackInfo, modEvents);
  m_pDriver->ReplayLog(0, events.back().eventId, eReplay_Full);
  m_pDriver->SubmitCmds();
  m_pDriver->FlushQ();

  // If there are any draw events, do another replay pass, in order to figure out
  // which tests failed for each draw event.
  TestsFailedCallback *tfCb = NULL;
  if(drawEvents.size() > 0)
  {
    VkQueryPool tfOcclusionPool;
    CreateOcclusionPool(m_pDriver, (uint32_t)drawEvents.size() * 6, &tfOcclusionPool);

    tfCb = new TestsFailedCallback(m_pDriver, shaderCache, callbackInfo, tfOcclusionPool, drawEvents);
    m_pDriver->ReplayLog(0, events.back().eventId, eReplay_Full);
    m_pDriver->SubmitCmds();
    m_pDriver->FlushQ();
    tfCb->FetchOcclusionResults();
    ObjDisp(dev)->DestroyQueryPool(Unwrap(dev), tfOcclusionPool, NULL);
  }

  for(size_t ev = 0; ev < events.size(); ev++)
  {
    uint32_t eventId = events[ev].eventId;
    bool clear = (events[ev].usage == ResourceUsage::Clear);
    bool directWrite = isDirectWrite(events[ev].usage);
    if(drawEvents.contains(events[ev].eventId) || clear || directWrite)
    {
      PixelModification mod;
      RDCEraseEl(mod);

      mod.eventId = eventId;
      mod.directShaderWrite = directWrite;
      mod.unboundPS = false;

      if(!clear && !directWrite)
      {
        RDCASSERT(tfCb != NULL);
        uint32_t flags = tfCb->GetEventFlags(eventId);
        if(flags & TestMustFail_Culling)
          mod.backfaceCulled = true;
        if(flags & TestMustFail_DepthTesting)
          mod.depthTestFailed = true;
        if(flags & TestMustFail_Scissor)
          mod.scissorClipped = true;
        if(flags & TestMustFail_SampleMask)
          mod.sampleMasked = true;
        if(flags & UnboundFragmentShader)
          mod.unboundPS = true;

        UpdateTestsFailed(tfCb, eventId, flags, mod);
      }
      history.push_back(mod);
    }
  }

  // Try to read memory back

  EventInfo *eventsInfo;
  VkResult vkr =
      m_pDriver->vkMapMemory(dev, resources.bufferMemory, 0, VK_WHOLE_SIZE, 0, (void **)&eventsInfo);
  RDCASSERTEQUAL(vkr, VK_SUCCESS);

  std::map<uint32_t, uint32_t> eventsWithFrags;
  ResourceFormat fmt = MakeResourceFormat(imginfo.format);

  for(size_t h = 0; h < history.size();)
  {
    PixelModification &mod = history[h];

    int32_t eventIndex = cb.GetEventIndex(mod.eventId);
    if(eventIndex == -1)
    {
      // There is no information, skip the event.
      h++;
      continue;
    }
    const EventInfo &ei = eventsInfo[eventIndex];
    FillInColor(fmt, ei.premod, mod.preMod);
    FillInColor(fmt, ei.postmod, mod.postMod);
    mod.preMod.depth = ei.premod.depth.fdepth;
    mod.preMod.stencil = ei.premod.stencil;
    mod.postMod.depth = ei.postmod.depth.fdepth;
    mod.postMod.stencil = ei.postmod.stencil;

    int32_t frags = int32_t(ei.dsWithoutShaderDiscard[0]);
    int32_t fragsClipped = int32_t(ei.dsWithShaderDiscard[0]);
    mod.shaderOut.col.intValue[0] = frags;
    mod.shaderOut.col.intValue[1] = fragsClipped;
    bool someFragsClipped = (fragsClipped < frags);
    mod.primitiveID = someFragsClipped;
    // Draws in secondary command buffers will fail this check,
    // so nothing else needs to be checked in the callback itself.
    if(frags > 0)
      eventsWithFrags[mod.eventId] = frags;

    for(int32_t f = 1; f < frags; f++)
    {
      history.insert(h + 1, mod);
    }
    for(int32_t f = 0; f < frags; f++)
      history[h + f].fragIndex = f;
    h += RDCMAX(1, frags);
    RDCDEBUG(
        "PixelHistory event id: %u, fixed shader stencilValue = %u, original shader stencilValue = "
        "%u",
        mod.eventId, ei.dsWithoutShaderDiscard[0], ei.dsWithShaderDiscard[0]);
  }
  m_pDriver->vkUnmapMemory(dev, resources.bufferMemory);

  if(eventsWithFrags.size() > 0)
  {
    // Replay to get shader output value, post modification value and primitive ID for every
    // fragment.
    VulkanPixelHistoryPerFragmentCallback perFragmentCB(m_pDriver, shaderCache, callbackInfo,
                                                        eventsWithFrags);
    m_pDriver->ReplayLog(0, eventsWithFrags.rbegin()->first, eReplay_Full);
    m_pDriver->SubmitCmds();
    m_pDriver->FlushQ();

    PerFragmentInfo *bp = NULL;
    vkr = m_pDriver->vkMapMemory(dev, resources.bufferMemory, 0, VK_WHOLE_SIZE, 0, (void **)&bp);
    RDCASSERTEQUAL(vkr, VK_SUCCESS);

    // Retrieve primitive ID values where fragment shader discarded some
    // fragments. For these primitives we are going to perform an occlusion
    // query to see if a primitive was discarded.
    std::map<uint32_t, rdcarray<int32_t> > discardedPrimsEvents;
    uint32_t primitivesToCheck = 0;
    for(size_t h = 0; h < history.size(); h++)
    {
      uint32_t eid = history[h].eventId;
      if(eventsWithFrags.find(eid) == eventsWithFrags.end())
        continue;
      uint32_t f = history[h].fragIndex;
      bool someFragsClipped = (history[h].primitiveID == 1);
      int32_t primId = bp[perFragmentCB.GetEventOffset(eid) + f].primitiveID;
      history[h].primitiveID = primId;
      if(someFragsClipped)
      {
        discardedPrimsEvents[eid].push_back(primId);
        primitivesToCheck++;
      }
    }

    if(primitivesToCheck > 0)
    {
      VkQueryPool occlPool;
      CreateOcclusionPool(m_pDriver, primitivesToCheck, &occlPool);

      // Replay to see which primitives were discarded.
      VulkanPixelHistoryDiscardedFragmentsCallback discardedCb(m_pDriver, shaderCache, callbackInfo,
                                                               discardedPrimsEvents, occlPool);
      m_pDriver->ReplayLog(0, eventsWithFrags.rbegin()->first, eReplay_Full);
      m_pDriver->SubmitCmds();
      m_pDriver->FlushQ();
      discardedCb.FetchOcclusionResults();
      ObjDisp(dev)->DestroyQueryPool(Unwrap(dev), occlPool, NULL);

      for(size_t h = 0; h < history.size(); h++)
        history[h].shaderDiscarded =
            discardedCb.PrimitiveDiscarded(history[h].eventId, history[h].primitiveID);
    }

    uint32_t discardOffset = 0;
    ResourceFormat resourceFmt = MakeResourceFormat(imginfo.format);
    ResourceFormat shaderOutFormat = MakeResourceFormat(VK_FORMAT_R32G32B32A32_SFLOAT);
    for(size_t h = 0; h < history.size(); h++)
    {
      uint32_t eid = history[h].eventId;
      uint32_t f = history[h].fragIndex;
      // Reset discard offset if this is a new event.
      if(h > 0 && (eid != history[h - 1].eventId))
        discardOffset = 0;
      if(eventsWithFrags.find(eid) != eventsWithFrags.end())
      {
        if(history[h].shaderDiscarded)
        {
          discardOffset++;
          // Copy previous post-mod value if its not the first event
          if(h > 0)
            history[h].postMod = history[h - 1].postMod;
          continue;
        }
        uint32_t offset = perFragmentCB.GetEventOffset(eid) + f - discardOffset;
        FillInColor(shaderOutFormat, bp[offset].shaderOut, history[h].shaderOut);
        history[h].shaderOut.depth = bp[offset].shaderOut.depth.fdepth;

        if((h < history.size() - 1) && (history[h].eventId == history[h + 1].eventId))
        {
          // Get post-modification value if this is not the last fragment for the event.
          FillInColor(resourceFmt, bp[offset].postMod, history[h].postMod);
          history[h].postMod.depth = bp[offset].postMod.depth.fdepth;
        }
      }
    }
  }

  GetDebugManager()->PixelHistoryDestroyResources(resources);
  ObjDisp(dev)->DestroyQueryPool(Unwrap(dev), occlusionPool, NULL);
  delete shaderCache;

  return history;
}
