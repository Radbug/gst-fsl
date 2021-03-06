/* VPU registered framebuffers structure
 * Copyright (C) 2013  Carlos Rafael Giani
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */


#include <string.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include "../common/phys_mem_allocator.h"
#include "framebuffers.h"
#include "utils.h"
#include "mem_blocks.h"


GST_DEBUG_CATEGORY_STATIC(vpu_framebuffers_debug);
#define GST_CAT_DEFAULT vpu_framebuffers_debug


#define ALIGN_VAL_TO(LENGTH, ALIGN_SIZE)  ( ((guintptr)((LENGTH) + (ALIGN_SIZE) - 1) / (ALIGN_SIZE)) * (ALIGN_SIZE) )
#define FRAME_ALIGN 16


G_DEFINE_TYPE(GstFslVpuFramebuffers, gst_fsl_vpu_framebuffers, GST_TYPE_OBJECT)


static gboolean gst_fsl_vpu_framebuffers_configure(GstFslVpuFramebuffers *framebuffers, GstFslVpuFramebufferParams *params, GstAllocator *allocator);
static void gst_fsl_vpu_framebuffers_finalize(GObject *object);




void gst_fsl_vpu_framebuffers_class_init(GstFslVpuFramebuffersClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = GST_DEBUG_FUNCPTR(gst_fsl_vpu_framebuffers_finalize);

	GST_DEBUG_CATEGORY_INIT(vpu_framebuffers_debug, "vpuframebuffers", 0, "Freescale VPU framebuffer memory blocks");
}


void gst_fsl_vpu_framebuffers_init(GstFslVpuFramebuffers *framebuffers)
{
	framebuffers->registration_state = GST_FSL_VPU_FRAMEBUFFERS_UNREGISTERED;
	memset(&(framebuffers->decenc_states), 0, sizeof(GstFslVpuFramebuffersDecEncStates));

	framebuffers->framebuffers = NULL;
	framebuffers->num_framebuffers = 0;
	framebuffers->num_available_framebuffers = 0;
	framebuffers->fb_mem_blocks = NULL;

	framebuffers->y_stride = framebuffers->uv_stride = 0;
	framebuffers->y_size = framebuffers->u_size = framebuffers->v_size = framebuffers->mv_size = 0;
	framebuffers->total_size = 0;

	g_mutex_init(&(framebuffers->available_fb_mutex));
}


GstFslVpuFramebuffers * gst_fsl_vpu_framebuffers_new(GstFslVpuFramebufferParams *params, GstAllocator *allocator)
{
	GstFslVpuFramebuffers *framebuffers;
	framebuffers = g_object_new(gst_fsl_vpu_framebuffers_get_type(), NULL);
	if (gst_fsl_vpu_framebuffers_configure(framebuffers, params, allocator))
		return framebuffers;
	else
		return NULL;
}


gboolean gst_fsl_vpu_framebuffers_register_with_decoder(GstFslVpuFramebuffers *framebuffers, VpuDecHandle handle)
{
	VpuDecRetCode vpu_ret;

	if (framebuffers->registration_state != GST_FSL_VPU_FRAMEBUFFERS_UNREGISTERED)
	{
		GST_ERROR_OBJECT(framebuffers, "framebuffers already registered");
		return FALSE;
	}

	framebuffers->decenc_states.dec.handle = handle;

	vpu_ret = VPU_DecRegisterFrameBuffer(handle, framebuffers->framebuffers, framebuffers->num_framebuffers);
	if (vpu_ret != VPU_DEC_RET_SUCCESS)
	{
		GST_ERROR_OBJECT(framebuffers, "registering framebuffers failed: %s", gst_fsl_vpu_strerror(vpu_ret));
		return FALSE;
	}

	framebuffers->registration_state = GST_FSL_VPU_FRAMEBUFFERS_DECODER_REGISTERED;
	framebuffers->decenc_states.dec.decoder_open = TRUE;

	return TRUE;
}


gboolean gst_fsl_vpu_framebuffers_register_with_encoder(GstFslVpuFramebuffers *framebuffers, VpuEncHandle handle, guint src_stride)
{
	VpuEncRetCode vpu_ret;

	if (framebuffers->registration_state != GST_FSL_VPU_FRAMEBUFFERS_UNREGISTERED)
	{
		GST_ERROR_OBJECT(framebuffers, "framebuffers already registered");
		return FALSE;
	}

	framebuffers->decenc_states.enc.handle = handle;

	vpu_ret = VPU_EncRegisterFrameBuffer(handle, framebuffers->framebuffers, framebuffers->num_framebuffers, src_stride);
	if (vpu_ret != VPU_ENC_RET_SUCCESS)
	{
		GST_ERROR_OBJECT(framebuffers, "registering framebuffers failed: %s", gst_fsl_vpu_strerror(vpu_ret));
		return FALSE;
	}

	framebuffers->registration_state = GST_FSL_VPU_FRAMEBUFFERS_ENCODER_REGISTERED;
	framebuffers->decenc_states.enc.encoder_open = TRUE;

	return TRUE;
}


void gst_fsl_vpu_framebuffers_dec_init_info_to_params(VpuDecInitInfo *init_info, GstFslVpuFramebufferParams *params)
{
	params->pic_width = init_info->nPicWidth;
	params->pic_height = init_info->nPicHeight;
	params->min_framebuffer_count = init_info->nMinFrameBufferCount;
	params->mjpeg_source_format = init_info->nMjpgSourceFormat;
	params->interlace = init_info->nInterlace;
	params->address_alignment = init_info->nAddressAlignment;
}


void gst_fsl_vpu_framebuffers_enc_init_info_to_params(VpuEncInitInfo *init_info, GstFslVpuFramebufferParams *params)
{
	params->pic_width = 0;
	params->pic_height = 0;
	params->min_framebuffer_count = init_info->nMinFrameBufferCount;
	params->mjpeg_source_format = 0;
	params->interlace = 0;
	params->address_alignment = init_info->nAddressAlignment;
}


static gboolean gst_fsl_vpu_framebuffers_configure(GstFslVpuFramebuffers *framebuffers, GstFslVpuFramebufferParams *params, GstAllocator *allocator)
{
	int alignment;
	unsigned char *phys_ptr, *virt_ptr;
	guint i;

	g_assert(GST_IS_FSL_PHYS_MEM_ALLOCATOR(allocator));

	framebuffers->num_reserve_framebuffers = params->min_framebuffer_count;
	framebuffers->num_framebuffers = MAX((guint)(params->min_framebuffer_count), (guint)10) + framebuffers->num_reserve_framebuffers;
	framebuffers->num_available_framebuffers = framebuffers->num_framebuffers - framebuffers->num_reserve_framebuffers;
	framebuffers->framebuffers = (VpuFrameBuffer *)g_slice_alloc(sizeof(VpuFrameBuffer) * framebuffers->num_framebuffers);

	framebuffers->allocator = allocator;

	framebuffers->y_stride = ALIGN_VAL_TO(params->pic_width, FRAME_ALIGN);
	if (params->interlace)
		framebuffers->y_size = framebuffers->y_stride * ALIGN_VAL_TO(params->pic_height, (2 * FRAME_ALIGN));
	else
		framebuffers->y_size = framebuffers->y_stride * ALIGN_VAL_TO(params->pic_height, FRAME_ALIGN);

	switch (params->mjpeg_source_format)
	{
		case 0: /* I420 (4:2:0) */
			framebuffers->uv_stride = framebuffers->y_stride / 2;
			framebuffers->u_size = framebuffers->v_size = framebuffers->mv_size = framebuffers->y_size / 4;
			break;
		case 1: /* Y42B (4:2:2 horizontal) */
			framebuffers->uv_stride = framebuffers->y_stride / 2;
			framebuffers->u_size = framebuffers->v_size = framebuffers->mv_size = framebuffers->y_size / 2;
			break;
		case 3: /* Y444 (4:4:4) */
			framebuffers->uv_stride = framebuffers->y_stride;
			framebuffers->u_size = framebuffers->v_size = framebuffers->mv_size = framebuffers->y_size;
			break;
		default:
			g_assert_not_reached();
	}

	alignment = params->address_alignment;
	if (alignment > 1)
	{
		framebuffers->y_size = ALIGN_VAL_TO(framebuffers->y_size, alignment);
		framebuffers->u_size = ALIGN_VAL_TO(framebuffers->u_size, alignment);
		framebuffers->v_size = ALIGN_VAL_TO(framebuffers->v_size, alignment);
		framebuffers->mv_size = ALIGN_VAL_TO(framebuffers->mv_size, alignment);
	}

	framebuffers->pic_width = params->pic_width;
	framebuffers->pic_height = params->pic_height;

	framebuffers->total_size = framebuffers->y_size + framebuffers->u_size + framebuffers->v_size + framebuffers->mv_size + alignment;
	GST_DEBUG_OBJECT(framebuffers, "num framebuffers:  total: %u  reserved: %u  available: %d", framebuffers->num_framebuffers, framebuffers->num_reserve_framebuffers, framebuffers->num_available_framebuffers);
	GST_DEBUG_OBJECT(framebuffers, "framebuffer memory block size:  total: %d  Y: %d  U: %d  V: %d  Mv:  %d  alignment: %d", framebuffers->total_size, framebuffers->y_size, framebuffers->u_size, framebuffers->v_size, framebuffers->mv_size, alignment);

	for (i = 0; i < framebuffers->num_framebuffers; ++i)
	{
		GstFslPhysMemory *memory;
		VpuFrameBuffer *framebuffer;

		framebuffer = &(framebuffers->framebuffers[i]);

		memory = (GstFslPhysMemory *)gst_allocator_alloc(allocator, framebuffers->total_size, NULL);
		if (memory == NULL)
			return FALSE;
		gst_fsl_vpu_append_phys_mem_block(memory, &(framebuffers->fb_mem_blocks));

		phys_ptr = (unsigned char*)(memory->phys_addr);
		virt_ptr = (unsigned char*)(memory->mapped_virt_addr); /* TODO */

		if (alignment > 1)
		{
			phys_ptr = (unsigned char*)ALIGN_VAL_TO(phys_ptr, alignment);
			virt_ptr = (unsigned char*)ALIGN_VAL_TO(virt_ptr, alignment);
		}

		framebuffer->nStrideY = framebuffers->y_stride;
		framebuffer->nStrideC = framebuffers->uv_stride;	

		/* fill phy addr*/
		framebuffer->pbufY     = phys_ptr;
		framebuffer->pbufCb    = phys_ptr + framebuffers->y_size;
		framebuffer->pbufCr    = phys_ptr + framebuffers->y_size + framebuffers->u_size;
		framebuffer->pbufMvCol = phys_ptr + framebuffers->y_size + framebuffers->u_size + framebuffers->v_size;

		/* fill virt addr */
		framebuffer->pbufVirtY     = virt_ptr;
		framebuffer->pbufVirtCb    = virt_ptr + framebuffers->y_size;
		framebuffer->pbufVirtCr    = virt_ptr + framebuffers->y_size + framebuffers->u_size;
		framebuffer->pbufVirtMvCol = virt_ptr + framebuffers->y_size + framebuffers->u_size + framebuffers->v_size;

		framebuffer->pbufY_tilebot = 0;
		framebuffer->pbufCb_tilebot = 0;
		framebuffer->pbufVirtY_tilebot = 0;
		framebuffer->pbufVirtCb_tilebot = 0;
	}

	return TRUE;
}


static void gst_fsl_vpu_framebuffers_finalize(GObject *object)
{
	GstFslVpuFramebuffers *framebuffers = GST_FSL_VPU_FRAMEBUFFERS(object);

	GST_DEBUG_OBJECT(framebuffers, "freeing framebuffer memory");

	if (framebuffers->framebuffers != NULL)
	{
		g_slice_free1(sizeof(VpuFrameBuffer) * framebuffers->num_framebuffers, framebuffers->framebuffers);
		framebuffers->framebuffers = NULL;
	}

	gst_fsl_vpu_free_phys_mem_blocks((GstFslPhysMemAllocator *)(framebuffers->allocator), &(framebuffers->fb_mem_blocks));

	G_OBJECT_CLASS(gst_fsl_vpu_framebuffers_parent_class)->finalize(object);
}

