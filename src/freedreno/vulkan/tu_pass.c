/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include "tu_private.h"

#include "vk_util.h"
#include "vk_format.h"

static void
tu_render_pass_add_subpass_dep(struct tu_render_pass *pass,
                               const VkSubpassDependency2 *dep)
{
   uint32_t src = dep->srcSubpass;
   uint32_t dst = dep->dstSubpass;

   /* Ignore subpass self-dependencies as they allow the app to call
    * vkCmdPipelineBarrier() inside the render pass and the driver should only
    * do the barrier when called, not when starting the render pass.
    */
   if (src == dst)
      return;

   struct tu_subpass_barrier *src_barrier;
   if (src == VK_SUBPASS_EXTERNAL) {
      src_barrier = &pass->subpasses[0].start_barrier;
   } else if (src == pass->subpass_count - 1) {
      src_barrier = &pass->end_barrier;
   } else {
      src_barrier = &pass->subpasses[src + 1].start_barrier;
   }

   struct tu_subpass_barrier *dst_barrier;
   if (dst == VK_SUBPASS_EXTERNAL) {
      dst_barrier = &pass->end_barrier;
   } else {
      dst_barrier = &pass->subpasses[dst].start_barrier;
   }

   if (dep->dstStageMask != VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT)
      src_barrier->src_stage_mask |= dep->srcStageMask;
   src_barrier->src_access_mask |= dep->srcAccessMask;
   dst_barrier->dst_access_mask |= dep->dstAccessMask;
   if (src == VK_SUBPASS_EXTERNAL)
      pass->subpasses[dst].has_external_src = true;
   if (dst == VK_SUBPASS_EXTERNAL)
      pass->subpasses[src].has_external_dst = true;
}

/* We currently only care about undefined layouts, because we have to
 * flush/invalidate CCU for those. PREINITIALIZED is the same thing as
 * UNDEFINED for anything not linear tiled, but we don't know yet whether the
 * images used are tiled, so just assume they are.
 */

static bool
layout_undefined(VkImageLayout layout)
{
   return layout == VK_IMAGE_LAYOUT_UNDEFINED ||
          layout == VK_IMAGE_LAYOUT_PREINITIALIZED;
}

/* This implements the following bit of spec text:
 *
 *    If there is no subpass dependency from VK_SUBPASS_EXTERNAL to the
 *    first subpass that uses an attachment, then an implicit subpass
 *    dependency exists from VK_SUBPASS_EXTERNAL to the first subpass it is
 *    used in. The implicit subpass dependency only exists if there
 *    exists an automatic layout transition away from initialLayout.
 *    The subpass dependency operates as if defined with the
 *    following parameters:
 *
 *    VkSubpassDependency implicitDependency = {
 *        .srcSubpass = VK_SUBPASS_EXTERNAL;
 *        .dstSubpass = firstSubpass; // First subpass attachment is used in
 *        .srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
 *        .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
 *        .srcAccessMask = 0;
 *        .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
 *                         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
 *                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
 *                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
 *                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
 *        .dependencyFlags = 0;
 *    };
 *
 *    Similarly, if there is no subpass dependency from the last subpass
 *    that uses an attachment to VK_SUBPASS_EXTERNAL, then an implicit
 *    subpass dependency exists from the last subpass it is used in to
 *    VK_SUBPASS_EXTERNAL. The implicit subpass dependency only exists
 *    if there exists an automatic layout transition into finalLayout.
 *    The subpass dependency operates as if defined with the following
 *    parameters:
 *
 *    VkSubpassDependency implicitDependency = {
 *        .srcSubpass = lastSubpass; // Last subpass attachment is used in
 *        .dstSubpass = VK_SUBPASS_EXTERNAL;
 *        .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
 *        .dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
 *        .srcAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
 *                         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
 *                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
 *                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
 *                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
 *        .dstAccessMask = 0;
 *        .dependencyFlags = 0;
 *    };
 *
 * Note: currently this is the only use we have for layout transitions,
 * besides needing to invalidate CCU at the beginning, so we also flag
 * transitions from UNDEFINED here.
 */
static void
tu_render_pass_add_implicit_deps(struct tu_render_pass *pass)
{
   bool att_used[pass->attachment_count];

   memset(att_used, 0, sizeof(att_used));

   for (unsigned i = 0; i < pass->subpass_count; i++) {
      struct tu_subpass *subpass = &pass->subpasses[i];
      if (!subpass->has_external_src)
         continue;

      bool src_implicit_dep = false;

      for (unsigned j = 0; j < subpass->input_count; j++) {
         unsigned att_idx = subpass->input_attachments[j].attachment;
         if (att_idx == VK_ATTACHMENT_UNUSED)
            continue;
         struct tu_render_pass_attachment *att = &pass->attachments[att_idx];
         if (att->initial_layout != subpass->input_attachments[j].layout &&
             !att_used[att_idx]) {
            src_implicit_dep = true;
         }
         att_used[att_idx] = true;
      }

      for (unsigned j = 0; j < subpass->color_count; j++) {
         unsigned att_idx = subpass->color_attachments[j].attachment;
         if (att_idx == VK_ATTACHMENT_UNUSED)
            continue;
         struct tu_render_pass_attachment *att = &pass->attachments[att_idx];
         if (att->initial_layout != subpass->color_attachments[j].layout &&
             !att_used[att_idx]) {
            src_implicit_dep = true;
         }
         att_used[att_idx] = true;
      }

      if (subpass->resolve_attachments) {
         for (unsigned j = 0; j < subpass->color_count; j++) {
            unsigned att_idx = subpass->resolve_attachments[j].attachment;
            if (att_idx == VK_ATTACHMENT_UNUSED)
               continue;
            struct tu_render_pass_attachment *att = &pass->attachments[att_idx];
            if (att->initial_layout != subpass->resolve_attachments[j].layout &&
                !att_used[att_idx]) {
               src_implicit_dep = true;
            }
            att_used[att_idx] = true;
         }
      }

      if (src_implicit_dep) {
         tu_render_pass_add_subpass_dep(pass, &(VkSubpassDependency2KHR) {
            .srcSubpass = VK_SUBPASS_EXTERNAL,
            .dstSubpass = i,
            .srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dependencyFlags = 0,
         });
      }
   }

   memset(att_used, 0, sizeof(att_used));

   for (int i = pass->subpass_count - 1; i >= 0; i--) {
      struct tu_subpass *subpass = &pass->subpasses[i];
      if (!subpass->has_external_dst)
         continue;

      bool dst_implicit_dep = false;

      for (unsigned j = 0; j < subpass->input_count; j++) {
         unsigned att_idx = subpass->input_attachments[j].attachment;
         if (att_idx == VK_ATTACHMENT_UNUSED)
            continue;
         struct tu_render_pass_attachment *att = &pass->attachments[att_idx];
         if (att->final_layout != subpass->input_attachments[j].layout &&
             !att_used[att_idx]) {
            dst_implicit_dep = true;
         }
         att_used[att_idx] = true;
      }

      for (unsigned j = 0; j < subpass->color_count; j++) {
         unsigned att_idx = subpass->color_attachments[j].attachment;
         if (att_idx == VK_ATTACHMENT_UNUSED)
            continue;
         struct tu_render_pass_attachment *att = &pass->attachments[att_idx];
         if (att->final_layout != subpass->color_attachments[j].layout &&
             !att_used[att_idx]) {
            dst_implicit_dep = true;
         }
         att_used[att_idx] = true;
      }

      if (subpass->resolve_attachments) {
         for (unsigned j = 0; j < subpass->color_count; j++) {
            unsigned att_idx = subpass->resolve_attachments[j].attachment;
            if (att_idx == VK_ATTACHMENT_UNUSED)
               continue;
            struct tu_render_pass_attachment *att = &pass->attachments[att_idx];
            if (att->final_layout != subpass->resolve_attachments[j].layout &&
                !att_used[att_idx]) {
               dst_implicit_dep = true;
            }
            att_used[att_idx] = true;
         }
      }

      if (dst_implicit_dep) {
         tu_render_pass_add_subpass_dep(pass, &(VkSubpassDependency2KHR) {
            .srcSubpass = i,
            .dstSubpass = VK_SUBPASS_EXTERNAL,
            .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            .srcAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = 0,
            .dependencyFlags = 0,
         });
      }
   }

   /* Handle UNDEFINED transitions, similar to the handling in tu_barrier().
    * Assume that if an attachment has an initial layout of UNDEFINED, it gets
    * transitioned eventually.
    */
   for (unsigned i = 0; i < pass->attachment_count; i++) {
      if (layout_undefined(pass->attachments[i].initial_layout)) {
         if (vk_format_is_depth_or_stencil(pass->attachments[i].format)) {
            pass->subpasses[0].start_barrier.incoherent_ccu_depth = true;
         } else {
            pass->subpasses[0].start_barrier.incoherent_ccu_color = true;
         }
      }
   }
}

static void update_samples(struct tu_subpass *subpass,
                           VkSampleCountFlagBits samples)
{
   assert(subpass->samples == 0 || subpass->samples == samples);
   subpass->samples = samples;
}

static void
create_render_pass_common(struct tu_render_pass *pass,
                          const struct tu_physical_device *phys_dev)
{
   uint32_t block_align_shift = 4; /* log2(gmem_align/(tile_align_w*tile_align_h)) */
   uint32_t tile_align_w = phys_dev->tile_align_w;
   uint32_t gmem_align = (1 << block_align_shift) * tile_align_w * TILE_ALIGN_H;

   /* calculate total bytes per pixel */
   uint32_t cpp_total = 0;
   for (uint32_t i = 0; i < pass->attachment_count; i++) {
      struct tu_render_pass_attachment *att = &pass->attachments[i];
      if (att->gmem_offset >= 0) {
         cpp_total += att->cpp;
         /* texture pitch must be aligned to 64, use a tile_align_w that is
          * a multiple of 64 for cpp==1 attachment to work as input attachment
          */
         if (att->cpp == 1 && tile_align_w % 64 != 0) {
            tile_align_w *= 2;
            block_align_shift -= 1;
         }
      }
   }

   pass->tile_align_w = tile_align_w;

   /* no gmem attachments */
   if (cpp_total == 0) {
      /* any value non-zero value so tiling config works with no attachments */
      pass->gmem_pixels = 1024*1024;
      return;
   }

   /* TODO: using ccu_offset_gmem so that BLIT_OP_SCALE resolve path
    * doesn't break things. maybe there is a better solution?
    * TODO: this algorithm isn't optimal
    * for example, two attachments with cpp = {1, 4}
    * result:  nblocks = {12, 52}, pixels = 196608
    * optimal: nblocks = {13, 51}, pixels = 208896
    */
   uint32_t gmem_blocks = phys_dev->ccu_offset_gmem / gmem_align;
   uint32_t offset = 0, pixels = ~0u;
   for (uint32_t i = 0; i < pass->attachment_count; i++) {
      struct tu_render_pass_attachment *att = &pass->attachments[i];
      if (att->gmem_offset < 0)
         continue;

      att->gmem_offset = offset;

      uint32_t align = MAX2(1, att->cpp >> block_align_shift);
      uint32_t nblocks = MAX2((gmem_blocks * att->cpp / cpp_total) & ~(align - 1), align);

      gmem_blocks -= nblocks;
      cpp_total -= att->cpp;
      offset += nblocks * gmem_align;
      pixels = MIN2(pixels, nblocks * gmem_align / att->cpp);
   }

   pass->gmem_pixels = pixels;

   for (uint32_t i = 0; i < pass->subpass_count; i++) {
      struct tu_subpass *subpass = &pass->subpasses[i];

      subpass->srgb_cntl = 0;

      for (uint32_t i = 0; i < subpass->color_count; ++i) {
         uint32_t a = subpass->color_attachments[i].attachment;
         if (a == VK_ATTACHMENT_UNUSED)
            continue;

         if (vk_format_is_srgb(pass->attachments[a].format))
            subpass->srgb_cntl |= 1 << i;
      }
   }

   /* disable unused attachments */
   for (uint32_t i = 0; i < pass->attachment_count; i++) {
      struct tu_render_pass_attachment *att = &pass->attachments[i];
      if (att->gmem_offset < 0) {
         att->clear_mask = 0;
         att->load = false;
      }
   }

   tu_render_pass_add_implicit_deps(pass);
}

static void
attachment_set_ops(struct tu_render_pass_attachment *att,
                   VkAttachmentLoadOp load_op,
                   VkAttachmentLoadOp stencil_load_op,
                   VkAttachmentStoreOp store_op,
                   VkAttachmentStoreOp stencil_store_op)
{
   /* load/store ops */
   att->clear_mask =
      (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) ? VK_IMAGE_ASPECT_COLOR_BIT : 0;
   att->load = (load_op == VK_ATTACHMENT_LOAD_OP_LOAD);
   att->store = (store_op == VK_ATTACHMENT_STORE_OP_STORE);

   bool stencil_clear = (stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR);
   bool stencil_load = (stencil_load_op == VK_ATTACHMENT_LOAD_OP_LOAD);
   bool stencil_store = (stencil_store_op == VK_ATTACHMENT_STORE_OP_STORE);

   switch (att->format) {
   case VK_FORMAT_D24_UNORM_S8_UINT: /* || stencil load/store */
      if (att->clear_mask)
         att->clear_mask = VK_IMAGE_ASPECT_DEPTH_BIT;
      if (stencil_clear)
         att->clear_mask |= VK_IMAGE_ASPECT_STENCIL_BIT;
      if (stencil_load)
         att->load = true;
      if (stencil_store)
         att->store = true;
      break;
   case VK_FORMAT_S8_UINT: /* replace load/store with stencil load/store */
      att->clear_mask = stencil_clear ? VK_IMAGE_ASPECT_COLOR_BIT : 0;
      att->load = stencil_load;
      att->store = stencil_store;
      break;
   default:
      break;
   }
}

VkResult
tu_CreateRenderPass(VkDevice _device,
                    const VkRenderPassCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkRenderPass *pRenderPass)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   struct tu_render_pass *pass;
   size_t size;
   size_t attachments_offset;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);

   size = sizeof(*pass);
   size += pCreateInfo->subpassCount * sizeof(pass->subpasses[0]);
   attachments_offset = size;
   size += pCreateInfo->attachmentCount * sizeof(pass->attachments[0]);

   pass = vk_alloc2(&device->alloc, pAllocator, size, 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pass == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   memset(pass, 0, size);
   pass->attachment_count = pCreateInfo->attachmentCount;
   pass->subpass_count = pCreateInfo->subpassCount;
   pass->attachments = (void *) pass + attachments_offset;

   for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
      struct tu_render_pass_attachment *att = &pass->attachments[i];

      att->format = pCreateInfo->pAttachments[i].format;
      att->samples = pCreateInfo->pAttachments[i].samples;
      att->cpp = vk_format_get_blocksize(att->format) * att->samples;
      att->initial_layout = pCreateInfo->pAttachments[i].initialLayout;
      att->final_layout = pCreateInfo->pAttachments[i].finalLayout;
      att->gmem_offset = -1;

      attachment_set_ops(att,
                         pCreateInfo->pAttachments[i].loadOp,
                         pCreateInfo->pAttachments[i].stencilLoadOp,
                         pCreateInfo->pAttachments[i].storeOp,
                         pCreateInfo->pAttachments[i].stencilStoreOp);
   }

   uint32_t subpass_attachment_count = 0;
   struct tu_subpass_attachment *p;
   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      const VkSubpassDescription *desc = &pCreateInfo->pSubpasses[i];

      subpass_attachment_count +=
         desc->inputAttachmentCount + desc->colorAttachmentCount +
         (desc->pResolveAttachments ? desc->colorAttachmentCount : 0);
   }

   if (subpass_attachment_count) {
      pass->subpass_attachments = vk_alloc2(
         &device->alloc, pAllocator,
         subpass_attachment_count * sizeof(struct tu_subpass_attachment), 8,
         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (pass->subpass_attachments == NULL) {
         vk_free2(&device->alloc, pAllocator, pass);
         return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   } else
      pass->subpass_attachments = NULL;

   p = pass->subpass_attachments;
   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      const VkSubpassDescription *desc = &pCreateInfo->pSubpasses[i];
      struct tu_subpass *subpass = &pass->subpasses[i];

      subpass->input_count = desc->inputAttachmentCount;
      subpass->color_count = desc->colorAttachmentCount;
      subpass->samples = 0;

      if (desc->inputAttachmentCount > 0) {
         subpass->input_attachments = p;
         p += desc->inputAttachmentCount;

         for (uint32_t j = 0; j < desc->inputAttachmentCount; j++) {
            uint32_t a = desc->pInputAttachments[j].attachment;
            subpass->input_attachments[j].attachment = a;
            subpass->input_attachments[j].layout =
               desc->pInputAttachments[j].layout;
            if (a != VK_ATTACHMENT_UNUSED)
               pass->attachments[a].gmem_offset = 0;
         }
      }

      if (desc->colorAttachmentCount > 0) {
         subpass->color_attachments = p;
         p += desc->colorAttachmentCount;

         for (uint32_t j = 0; j < desc->colorAttachmentCount; j++) {
            uint32_t a = desc->pColorAttachments[j].attachment;
            subpass->color_attachments[j].attachment = a;
            subpass->color_attachments[j].layout =
               desc->pColorAttachments[j].layout;

            if (a != VK_ATTACHMENT_UNUSED) {
               pass->attachments[a].gmem_offset = 0;
               update_samples(subpass, pCreateInfo->pAttachments[a].samples);
            }
         }
      }

      subpass->resolve_attachments = desc->pResolveAttachments ? p : NULL;
      if (desc->pResolveAttachments) {
         p += desc->colorAttachmentCount;
         for (uint32_t j = 0; j < desc->colorAttachmentCount; j++) {
            subpass->resolve_attachments[j].attachment =
                  desc->pResolveAttachments[j].attachment;
            subpass->resolve_attachments[j].layout =
               desc->pResolveAttachments[j].layout;
         }
      }

      uint32_t a = desc->pDepthStencilAttachment ?
         desc->pDepthStencilAttachment->attachment : VK_ATTACHMENT_UNUSED;
      subpass->depth_stencil_attachment.attachment = a;
      if (a != VK_ATTACHMENT_UNUSED) {
            pass->attachments[a].gmem_offset = 0;
            subpass->depth_stencil_attachment.layout =
               desc->pDepthStencilAttachment->layout;
            update_samples(subpass, pCreateInfo->pAttachments[a].samples);
      }

      subpass->samples = subpass->samples ?: 1;
   }

   for (unsigned i = 0; i < pCreateInfo->dependencyCount; ++i) {
      /* Convert to a Dependency2 */
      struct VkSubpassDependency2 dep2 = {
         .srcSubpass       = pCreateInfo->pDependencies[i].srcSubpass,
         .dstSubpass       = pCreateInfo->pDependencies[i].dstSubpass,
         .srcStageMask     = pCreateInfo->pDependencies[i].srcStageMask,
         .dstStageMask     = pCreateInfo->pDependencies[i].dstStageMask,
         .srcAccessMask    = pCreateInfo->pDependencies[i].srcAccessMask,
         .dstAccessMask    = pCreateInfo->pDependencies[i].dstAccessMask,
         .dependencyFlags  = pCreateInfo->pDependencies[i].dependencyFlags,
      };
      tu_render_pass_add_subpass_dep(pass, &dep2);
   }

   *pRenderPass = tu_render_pass_to_handle(pass);

   create_render_pass_common(pass, device->physical_device);

   return VK_SUCCESS;
}

VkResult
tu_CreateRenderPass2(VkDevice _device,
                     const VkRenderPassCreateInfo2KHR *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkRenderPass *pRenderPass)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   struct tu_render_pass *pass;
   size_t size;
   size_t attachments_offset;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2_KHR);

   size = sizeof(*pass);
   size += pCreateInfo->subpassCount * sizeof(pass->subpasses[0]);
   attachments_offset = size;
   size += pCreateInfo->attachmentCount * sizeof(pass->attachments[0]);

   pass = vk_alloc2(&device->alloc, pAllocator, size, 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pass == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   memset(pass, 0, size);
   pass->attachment_count = pCreateInfo->attachmentCount;
   pass->subpass_count = pCreateInfo->subpassCount;
   pass->attachments = (void *) pass + attachments_offset;

   for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
      struct tu_render_pass_attachment *att = &pass->attachments[i];

      att->format = pCreateInfo->pAttachments[i].format;
      att->samples = pCreateInfo->pAttachments[i].samples;
      att->cpp = vk_format_get_blocksize(att->format) * att->samples;
      att->initial_layout = pCreateInfo->pAttachments[i].initialLayout;
      att->final_layout = pCreateInfo->pAttachments[i].finalLayout;
      att->gmem_offset = -1;

      attachment_set_ops(att,
                         pCreateInfo->pAttachments[i].loadOp,
                         pCreateInfo->pAttachments[i].stencilLoadOp,
                         pCreateInfo->pAttachments[i].storeOp,
                         pCreateInfo->pAttachments[i].stencilStoreOp);
   }
   uint32_t subpass_attachment_count = 0;
   struct tu_subpass_attachment *p;
   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      const VkSubpassDescription2KHR *desc = &pCreateInfo->pSubpasses[i];

      subpass_attachment_count +=
         desc->inputAttachmentCount + desc->colorAttachmentCount +
         (desc->pResolveAttachments ? desc->colorAttachmentCount : 0);
   }

   if (subpass_attachment_count) {
      pass->subpass_attachments = vk_alloc2(
         &device->alloc, pAllocator,
         subpass_attachment_count * sizeof(struct tu_subpass_attachment), 8,
         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (pass->subpass_attachments == NULL) {
         vk_free2(&device->alloc, pAllocator, pass);
         return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   } else
      pass->subpass_attachments = NULL;

   p = pass->subpass_attachments;
   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      const VkSubpassDescription2KHR *desc = &pCreateInfo->pSubpasses[i];
      struct tu_subpass *subpass = &pass->subpasses[i];

      subpass->input_count = desc->inputAttachmentCount;
      subpass->color_count = desc->colorAttachmentCount;
      subpass->samples = 0;

      if (desc->inputAttachmentCount > 0) {
         subpass->input_attachments = p;
         p += desc->inputAttachmentCount;

         for (uint32_t j = 0; j < desc->inputAttachmentCount; j++) {
            uint32_t a = desc->pInputAttachments[j].attachment;
            subpass->input_attachments[j].attachment = a;
            subpass->input_attachments[j].layout =
               desc->pInputAttachments[j].layout;
            if (a != VK_ATTACHMENT_UNUSED)
               pass->attachments[a].gmem_offset = 0;
         }
      }

      if (desc->colorAttachmentCount > 0) {
         subpass->color_attachments = p;
         p += desc->colorAttachmentCount;

         for (uint32_t j = 0; j < desc->colorAttachmentCount; j++) {
            uint32_t a = desc->pColorAttachments[j].attachment;
            subpass->color_attachments[j].attachment = a;
            subpass->color_attachments[j].layout =
               desc->pColorAttachments[j].layout;

            if (a != VK_ATTACHMENT_UNUSED) {
               pass->attachments[a].gmem_offset = 0;
               update_samples(subpass, pCreateInfo->pAttachments[a].samples);
            }
         }
      }

      subpass->resolve_attachments = desc->pResolveAttachments ? p : NULL;
      if (desc->pResolveAttachments) {
         p += desc->colorAttachmentCount;
         for (uint32_t j = 0; j < desc->colorAttachmentCount; j++) {
            subpass->resolve_attachments[j].attachment =
                  desc->pResolveAttachments[j].attachment;
            subpass->resolve_attachments[j].layout =
               desc->pResolveAttachments[j].layout;
         }
      }


      uint32_t a = desc->pDepthStencilAttachment ?
         desc->pDepthStencilAttachment->attachment : VK_ATTACHMENT_UNUSED;
      subpass->depth_stencil_attachment.attachment = a;
      if (a != VK_ATTACHMENT_UNUSED) {
            pass->attachments[a].gmem_offset = 0;
            subpass->depth_stencil_attachment.layout =
               desc->pDepthStencilAttachment->layout;
            update_samples(subpass, pCreateInfo->pAttachments[a].samples);
      }

      subpass->samples = subpass->samples ?: 1;
   }

   for (unsigned i = 0; i < pCreateInfo->dependencyCount; ++i) {
      tu_render_pass_add_subpass_dep(pass, &pCreateInfo->pDependencies[i]);
   }
 
   *pRenderPass = tu_render_pass_to_handle(pass);

   create_render_pass_common(pass, device->physical_device);

   return VK_SUCCESS;
}

void
tu_DestroyRenderPass(VkDevice _device,
                     VkRenderPass _pass,
                     const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(tu_render_pass, pass, _pass);

   if (!_pass)
      return;

   vk_free2(&device->alloc, pAllocator, pass->subpass_attachments);
   vk_free2(&device->alloc, pAllocator, pass);
}

void
tu_GetRenderAreaGranularity(VkDevice _device,
                            VkRenderPass renderPass,
                            VkExtent2D *pGranularity)
{
   pGranularity->width = GMEM_ALIGN_W;
   pGranularity->height = GMEM_ALIGN_H;
}
