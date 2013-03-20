/* Copyright (c) 2011-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/hrtimer.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>
#include <linux/fb.h>
#include <asm/system.h>
#include <asm/mach-types.h>
#include <mach/hardware.h>

#include "mdp.h"
#include "msm_fb.h"
#include "mdp4.h"
#include "mipi_dsi.h"



static struct mdp4_overlay_pipe *dsi_pipe;
static struct msm_fb_data_type *dsi_mfd;
static int busy_wait_cnt;
static int dsi_state;
static unsigned long  tout_expired;

#ifdef MDP_HANG_DEBUG
static int mipi_dsi_cmd_mode_on;
#endif

#ifdef MDP_HANG_DEBUG
#define TOUT_PERIOD	(2*HZ)	/* 4 second */
#else
#define TOUT_PERIOD	HZ	/* 1 second */
#endif

#define MS_100		(HZ/10)	/* 100 ms */

static int vsync_start_y_adjust = 4;

struct timer_list dsi_clock_timer;

void mdp4_overlay_dsi_state_set(int state)
{
	unsigned long flag;

	spin_lock_irqsave(&mdp_spin_lock, flag);
	dsi_state = state;
	spin_unlock_irqrestore(&mdp_spin_lock, flag);
}

int mdp4_overlay_dsi_state_get(void)
{
	return dsi_state;
}

static void dsi_clock_tout(unsigned long data)
{
#ifdef MDP_HANG_DEBUG
	if (mipi_dsi_cmd_mode_on) {
		if (dsi_clock_timer.function) {
			tout_expired = jiffies + TOUT_PERIOD;
			mod_timer(&dsi_clock_timer, tout_expired);
		}

		return;
	}
#endif
	spin_lock(&dsi_clk_lock);
	if (mipi_dsi_clk_on) {
		if (dsi_state == ST_DSI_PLAYING) {
			mipi_dsi_turn_off_clks();
			mdp4_overlay_dsi_state_set(ST_DSI_CLK_OFF);
		}
	}
	spin_unlock(&dsi_clk_lock);
}

static __u32 msm_fb_line_length(__u32 fb_index, __u32 xres, int bpp)
{
	/*
	 * The adreno GPU hardware requires that the pitch be aligned to
	 * 32 pixels for color buffers, so for the cases where the GPU
	 * is writing directly to fb0, the framebuffer pitch
	 * also needs to be 32 pixel aligned
	 */

	if (fb_index == 0)
		return ALIGN(xres, 32) * bpp;
	else
		return xres * bpp;
}

void mdp4_dsi_cmd_del_timer(void)
{
	del_timer_sync(&dsi_clock_timer);
}

void mdp4_mipi_vsync_enable(struct msm_fb_data_type *mfd,
		struct mdp4_overlay_pipe *pipe, int which)
{
	uint32 start_y, data, tear_en;

	tear_en = (1 << which);

	if ((mfd->use_mdp_vsync) && (mfd->ibuf.vsync_enable) &&
		(mfd->panel_info.lcd.vsync_enable)) {

		if (vsync_start_y_adjust <= pipe->dst_y)
			start_y = pipe->dst_y - vsync_start_y_adjust;
		else
			start_y = (mfd->total_lcd_lines - 1) -
				(vsync_start_y_adjust - pipe->dst_y);
		if (which == 0)
			MDP_OUTP(MDP_BASE + 0x210, start_y);	/* primary */
		else
			MDP_OUTP(MDP_BASE + 0x214, start_y);	/* secondary */

		data = inpdw(MDP_BASE + 0x20c);
		data |= tear_en;
		MDP_OUTP(MDP_BASE + 0x20c, data);
	} else {
		data = inpdw(MDP_BASE + 0x20c);
		data &= ~tear_en;
		MDP_OUTP(MDP_BASE + 0x20c, data);
	}
}

void mdp4_overlay_update_dsi_cmd(struct msm_fb_data_type *mfd)
{
	MDPIBUF *iBuf = &mfd->ibuf;
	uint8 *src;
	int ptype;
	struct mdp4_overlay_pipe *pipe;
	int bpp;
	int ret;

	if (mfd->key != MFD_KEY)
		return;

	dsi_mfd = mfd;		/* keep it */

	/* MDP cmd block enable */
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	if (dsi_pipe == NULL) {
		ptype = mdp4_overlay_format2type(mfd->fb_imgType);
		if (ptype < 0)
			printk(KERN_INFO "%s: format2type failed\n", __func__);
		pipe = mdp4_overlay_pipe_alloc(ptype, MDP4_MIXER0);
		if (pipe == NULL)
			printk(KERN_INFO "%s: pipe_alloc failed\n", __func__);
		pipe->pipe_used++;
		pipe->mixer_stage  = MDP4_MIXER_STAGE_BASE;
		pipe->mixer_num  = MDP4_MIXER0;
		pipe->src_format = mfd->fb_imgType;
		mdp4_overlay_panel_mode(pipe->mixer_num, MDP4_PANEL_DSI_CMD);
		ret = mdp4_overlay_format2pipe(pipe);
		if (ret < 0)
			printk(KERN_INFO "%s: format2type failed\n", __func__);

		init_timer(&dsi_clock_timer);
		dsi_clock_timer.function = dsi_clock_tout;
		dsi_clock_timer.data = (unsigned long) mfd;
		dsi_clock_timer.expires = 0xffffffff;
		add_timer(&dsi_clock_timer);
		tout_expired = jiffies;

		dsi_pipe = pipe; /* keep it */

		mdp4_init_writeback_buf(mfd, MDP4_MIXER0);
		pipe->blt_addr = 0;

	} else {
		pipe = dsi_pipe;
	}
	/*
	 * configure dsi stream id
	 * dma_p = 0, dma_s = 1
	 */
	MDP_OUTP(MDP_BASE + 0x000a0, 0x10);
	/* disable dsi trigger */
	MDP_OUTP(MDP_BASE + 0x000a4, 0x00);
	/* whole screen for base layer */
	src = (uint8 *) iBuf->buf;


	{
		struct fb_info *fbi;

		fbi = mfd->fbi;
		if (pipe->is_3d) {
			bpp = fbi->var.bits_per_pixel / 8;
			pipe->src_height = pipe->src_height_3d;
			pipe->src_width = pipe->src_width_3d;
			pipe->src_h = pipe->src_height_3d;
			pipe->src_w = pipe->src_width_3d;
			pipe->dst_h = pipe->src_height_3d;
			pipe->dst_w = pipe->src_width_3d;
			pipe->srcp0_ystride = msm_fb_line_length(0,
						pipe->src_width, bpp);
		} else {
			 /* 2D */
			pipe->src_height = fbi->var.yres;
			pipe->src_width = fbi->var.xres;
			pipe->src_h = fbi->var.yres;
			pipe->src_w = fbi->var.xres;
			pipe->dst_h = fbi->var.yres;
			pipe->dst_w = fbi->var.xres;
			pipe->srcp0_ystride = fbi->fix.line_length;
		}
		pipe->src_y = 0;
		pipe->src_x = 0;
		pipe->dst_y = 0;
		pipe->dst_x = 0;
		pipe->srcp0_addr = (uint32)src;
	}


	mdp4_overlay_rgb_setup(pipe);

	mdp4_mixer_stage_up(pipe);

	mdp4_overlayproc_cfg(pipe);

	mdp4_overlay_dmap_xy(pipe);

	mdp4_overlay_dmap_cfg(mfd, 0);

	mdp4_mipi_vsync_enable(mfd, pipe, 0);

	/* MDP cmd block disable */
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	wmb();
}

#ifdef MDP_UNDERFLOW_RESET_CTRL_CMD
void mdp4_dsi_cmd_dmap_reconfig(void)
{
	/*
	 * called from mixer_reset -- IRQ context
	 * within spin_lock(&mdp_reset_irq) protection
	 */

	if (dsi_pipe)
		mdp4_overlay_dmap_xy(dsi_pipe);
	if (dsi_mfd) {
		mdp4_overlay_dmap_cfg(dsi_mfd, 0);
		spin_lock(&mdp_spin_lock);
		if (dsi_mfd->dma->busy == TRUE) {
			/* force to false to allow next kickoff */
			busy_wait_cnt = 0;
			dsi_mfd->dma->busy = FALSE;
			complete(&dsi_mfd->dma->comp);
			mdp_disable_irq_nosync(MDP_OVERLAY0_TERM);
		}
		if (dsi_mfd->dma->dmap_busy == TRUE) {
			dsi_mfd->dma->dmap_busy = FALSE;
			complete(&dsi_mfd->dma->dmap_comp);
			mdp_disable_irq_nosync(MDP_DMA2_TERM);
		}
		spin_unlock(&mdp_spin_lock);
	}
}
#endif
/* 3D side by side */
void mdp4_dsi_cmd_3d_sbys(struct msm_fb_data_type *mfd,
				struct msmfb_overlay_3d *r3d)
{
	struct fb_info *fbi;
	struct mdp4_overlay_pipe *pipe;
	int bpp;
	uint8 *src = NULL;

	if (dsi_pipe == NULL)
		return;

	dsi_pipe->is_3d = r3d->is_3d;
	dsi_pipe->src_height_3d = r3d->height;
	dsi_pipe->src_width_3d = r3d->width;

	pipe = dsi_pipe;

	if (pipe->is_3d)
		mdp4_overlay_panel_3d(pipe->mixer_num, MDP4_3D_SIDE_BY_SIDE);
	else
		mdp4_overlay_panel_3d(pipe->mixer_num, MDP4_3D_NONE);

	if (mfd->panel_power_on) {
		mdp4_dsi_cmd_dma_busy_wait(mfd);
		mdp4_dsi_blt_dmap_busy_wait(mfd);
	}

	fbi = mfd->fbi;
	if (pipe->is_3d) {
		bpp = fbi->var.bits_per_pixel / 8;
		pipe->src_height = pipe->src_height_3d;
		pipe->src_width = pipe->src_width_3d;
		pipe->src_h = pipe->src_height_3d;
		pipe->src_w = pipe->src_width_3d;
		pipe->dst_h = pipe->src_height_3d;
		pipe->dst_w = pipe->src_width_3d;
		pipe->srcp0_ystride = msm_fb_line_length(0,
					pipe->src_width, bpp);
	} else {
		 /* 2D */
		pipe->src_height = fbi->var.yres;
		pipe->src_width = fbi->var.xres;
		pipe->src_h = fbi->var.yres;
		pipe->src_w = fbi->var.xres;
		pipe->dst_h = fbi->var.yres;
		pipe->dst_w = fbi->var.xres;
		pipe->srcp0_ystride = fbi->fix.line_length;
	}
	pipe->src_y = 0;
	pipe->src_x = 0;
	pipe->dst_y = 0;
	pipe->dst_x = 0;
	pipe->srcp0_addr = (uint32)src;

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	mdp4_overlay_rgb_setup(pipe);

	mdp4_mixer_stage_up(pipe);

	mdp4_overlayproc_cfg(pipe);

	mdp4_overlay_dmap_xy(pipe);

	mdp4_overlay_dmap_cfg(mfd, 0);

	/* MDP cmd block disable */
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

int mdp4_dsi_overlay_blt_start(struct msm_fb_data_type *mfd)
{
	unsigned long flag;

	pr_debug("%s: blt_end=%d blt_addr=%x pid=%d\n",
	__func__, dsi_pipe->blt_end, (int)dsi_pipe->blt_addr, current->pid);

	mdp4_allocate_writeback_buf(mfd, MDP4_MIXER0);

	if (mfd->ov0_wb_buf->phys_addr == 0) {
		pr_info("%s: no blt_base assigned\n", __func__);
		return -EBUSY;
	}

	if (dsi_pipe->blt_addr == 0) {
		mdp4_dsi_cmd_dma_busy_wait(mfd);
		spin_lock_irqsave(&mdp_spin_lock, flag);
		dsi_pipe->blt_end = 0;
		dsi_pipe->blt_cnt = 0;
		dsi_pipe->ov_cnt = 0;
		dsi_pipe->dmap_cnt = 0;
		dsi_pipe->blt_addr = mfd->ov0_wb_buf->phys_addr;
		mdp4_stat.blt_dsi_cmd++;
		spin_unlock_irqrestore(&mdp_spin_lock, flag);
		return 0;
	}

	return -EBUSY;
}

int mdp4_dsi_overlay_blt_stop(struct msm_fb_data_type *mfd)
{
	unsigned long flag;


	pr_debug("%s: blt_end=%d blt_addr=%x\n",
		 __func__, dsi_pipe->blt_end, (int)dsi_pipe->blt_addr);

	if ((dsi_pipe->blt_end == 0) && dsi_pipe->blt_addr) {
		spin_lock_irqsave(&mdp_spin_lock, flag);
		dsi_pipe->blt_end = 1;	/* mark as end */
		spin_unlock_irqrestore(&mdp_spin_lock, flag);
		return 0;
	}

	return -EBUSY;
}

int mdp4_dsi_overlay_blt_offset(struct msm_fb_data_type *mfd,
					struct msmfb_overlay_blt *req)
{
	req->offset = 0;
	req->width = dsi_pipe->src_width;
	req->height = dsi_pipe->src_height;
	req->bpp = dsi_pipe->bpp;

	return sizeof(*req);
}

void mdp4_dsi_overlay_blt(struct msm_fb_data_type *mfd,
					struct msmfb_overlay_blt *req)
{
	if (req->enable)
		mdp4_dsi_overlay_blt_start(mfd);
	else if (req->enable == 0)
		mdp4_dsi_overlay_blt_stop(mfd);

}

void mdp4_blt_xy_update(struct mdp4_overlay_pipe *pipe)
{
	uint32 off, addr, addr2;
	int bpp;
	char *overlay_base;


	if (pipe->blt_addr == 0)
		return;


#ifdef BLT_RGB565
	bpp = 2; /* overlay ouput is RGB565 */
#else
	bpp = 3; /* overlay ouput is RGB888 */
#endif
	off = 0;
	if (pipe->dmap_cnt & 0x01)
		off = pipe->src_height * pipe->src_width * bpp;
	addr = pipe->blt_addr + off;

	/* dmap */
	MDP_OUTP(MDP_BASE + 0x90008, addr);

	off = 0;
	if (pipe->ov_cnt & 0x01)
		off = pipe->src_height * pipe->src_width * bpp;
	addr2 = pipe->blt_addr + off;
	/* overlay 0 */
	overlay_base = MDP_BASE + MDP4_OVERLAYPROC0_BASE;/* 0x10000 */
	outpdw(overlay_base + 0x000c, addr2);
	outpdw(overlay_base + 0x001c, addr2);
}


/*
 * mdp4_dmap_done_dsi: called from isr
 * DAM_P_DONE only used when blt enabled
 */
void mdp4_dma_p_done_dsi(struct mdp_dma_data *dma)
{
	int diff;

	dsi_pipe->dmap_cnt++;
	diff = dsi_pipe->ov_cnt - dsi_pipe->dmap_cnt;
	pr_debug("%s: ov_cnt=%d dmap_cnt=%d\n",
			__func__, dsi_pipe->ov_cnt, dsi_pipe->dmap_cnt);

	if (diff <= 0) {
		spin_lock(&mdp_spin_lock);
		dma->dmap_busy = FALSE;
		complete(&dma->dmap_comp);
		spin_unlock(&mdp_spin_lock);
		if (dsi_pipe->blt_end) {
			dsi_pipe->blt_end = 0;
			dsi_pipe->blt_addr = 0;
			pr_debug("%s: END, ov_cnt=%d dmap_cnt=%d\n",
				__func__, dsi_pipe->ov_cnt, dsi_pipe->dmap_cnt);
			mdp_intr_mask &= ~INTR_DMA_P_DONE;
			outp32(MDP_INTR_ENABLE, mdp_intr_mask);
		}
		mdp_pipe_ctrl(MDP_OVERLAY0_BLOCK, MDP_BLOCK_POWER_OFF, TRUE);
		mdp_disable_irq_nosync(MDP_DMA2_TERM);  /* disable intr */
		return;
	}

	spin_lock(&mdp_spin_lock);
	dma->busy = FALSE;
	spin_unlock(&mdp_spin_lock);
	complete(&dma->comp);
	if (busy_wait_cnt)
		busy_wait_cnt--;

	pr_debug("%s: kickoff dmap\n", __func__);

	mdp4_blt_xy_update(dsi_pipe);
	/* kick off dmap */
	outpdw(MDP_BASE + 0x000c, 0x0);
	mdp4_stat.kickoff_dmap++;
	/* trigger dsi cmd engine */
	mipi_dsi_cmd_mdp_start();

	mdp_pipe_ctrl(MDP_OVERLAY0_BLOCK, MDP_BLOCK_POWER_OFF, TRUE);
}


/*
 * mdp4_overlay0_done_dsi_cmd: called from isr
 */
void mdp4_overlay0_done_dsi_cmd(struct mdp_dma_data *dma)
{
	int diff;

	if (dsi_pipe->blt_addr == 0) {
		mdp_pipe_ctrl(MDP_OVERLAY0_BLOCK, MDP_BLOCK_POWER_OFF, TRUE);
		spin_lock(&mdp_spin_lock);
		dma->busy = FALSE;
		spin_unlock(&mdp_spin_lock);
		complete(&dma->comp);
		if (busy_wait_cnt)
			busy_wait_cnt--;
		mdp_disable_irq_nosync(MDP_OVERLAY0_TERM);
		return;
	}

	/* blt enabled */
	if (dsi_pipe->blt_end == 0)
		dsi_pipe->ov_cnt++;

	pr_debug("%s: ov_cnt=%d dmap_cnt=%d\n",
			__func__, dsi_pipe->ov_cnt, dsi_pipe->dmap_cnt);

	if (dsi_pipe->blt_cnt == 0) {
		/* first kickoff since blt enabled */
		mdp_intr_mask |= INTR_DMA_P_DONE;
		outp32(MDP_INTR_ENABLE, mdp_intr_mask);
	}
	dsi_pipe->blt_cnt++;

	diff = dsi_pipe->ov_cnt - dsi_pipe->dmap_cnt;
	if (diff >= 2) {
		mdp_disable_irq_nosync(MDP_OVERLAY0_TERM);
		return;
	}

	spin_lock(&mdp_spin_lock);
	dma->busy = FALSE;
	dma->dmap_busy = TRUE;
	spin_unlock(&mdp_spin_lock);
	complete(&dma->comp);
	if (busy_wait_cnt)
		busy_wait_cnt--;

	pr_debug("%s: kickoff dmap\n", __func__);

	mdp4_blt_xy_update(dsi_pipe);
	mdp_enable_irq(MDP_DMA2_TERM);	/* enable intr */
	/* kick off dmap */
	outpdw(MDP_BASE + 0x000c, 0x0);
	mdp4_stat.kickoff_dmap++;
	/* trigger dsi cmd engine */
	mipi_dsi_cmd_mdp_start();
	mdp_disable_irq_nosync(MDP_OVERLAY0_TERM);
}

void mdp4_dsi_cmd_overlay_restore(void)
{
#ifdef FACTORY_TEST
	if (!is_lcd_connected)
		return;
#endif
	/* mutex holded by caller */
	if (dsi_mfd && dsi_pipe) {
		mdp4_dsi_cmd_dma_busy_wait(dsi_mfd);
		mipi_dsi_mdp_busy_wait(dsi_mfd);
		mdp4_overlay_update_dsi_cmd(dsi_mfd);

		if (dsi_pipe->blt_addr)
			mdp4_dsi_blt_dmap_busy_wait(dsi_mfd);
		mdp4_dsi_cmd_overlay_kickoff(dsi_mfd, dsi_pipe);
	}
}

void mdp4_dsi_blt_dmap_busy_wait(struct msm_fb_data_type *mfd)
{
	unsigned long flag;
	int need_wait = 0;

	spin_lock_irqsave(&mdp_spin_lock, flag);
	if (mfd->dma->dmap_busy == TRUE) {
		INIT_COMPLETION(mfd->dma->dmap_comp);
		need_wait++;
	}
	spin_unlock_irqrestore(&mdp_spin_lock, flag);

	if (need_wait) {
#ifdef MDP_HANG_DEBUG
		if (!wait_for_completion_timeout(&mfd->dma->dmap_comp,
		msecs_to_jiffies(VSYNC_PERIOD*30))) {
			/* Does not receive interrupt from MDP,
				Something wrong */

			dump_mdp_registers();
			panic("mdp interrupt missing");
		}

#else
		/* wait until DMA finishes the current job */
		wait_for_completion_timeout(&mfd->dma->dmap_comp,
			msecs_to_jiffies(VSYNC_PERIOD*2));
#endif
	}
}

/*
 * mdp4_dsi_cmd_dma_busy_wait: check dsi link activity
 * dsi link is a shared resource and it can only be used
 * while it is in idle state.
 * ov_mutex need to be acquired before call this function.
 */

#ifdef MDP_HANG_DEBUG

inline void printkout(int offset, int cnt)
{
	int i = 0;

	for (i = 0; i < cnt; i++) {
		pr_err("offset 0x%x = 0x%x\n",
		(offset + (i*4)), inpdw(MDP_BASE + (offset + (i*4))));
	}
}

inline void printkout_mipi(int offset, int cnt)
{
	int i = 0;

	for (i = 0; i < cnt; i++) {
		pr_err("offset 0x%x = 0x%x\n",
		(offset + (i*4)), inpdw(MIPI_DSI_BASE + (offset + (i*4))));
	}
}

void save_reg_dump()
{
	uint32 cnt = 0;

	for (cnt  = 0; cnt < MDP_GENENRL_DUMP_NUM; cnt++) {
		mdp4_stat.mdp_reg_dump_general[cnt]
			= inpdw(MDP_BASE + MDP_GENERAL_DUMP_START + (cnt*4));
	}

	for (cnt  = 0; cnt < MDP_SYNC_DUMP_NUM; cnt++) {
		mdp4_stat.mdp_reg_dump_sync[cnt]
			= inpdw(MDP_BASE + MDP_SYNC_DUMP_START + (cnt*4));
	}

	for (cnt  = 0; cnt < MDP_OV_PROC_DUMP_NUM; cnt++) {
		mdp4_stat.mdp_reg_dump_ov_pro[cnt]
			= inpdw(MDP_BASE + MDP_OV_PROC_DUMP_START + (cnt*4));
	}

	for (cnt  = 0; cnt < MDP_DMA_P_DUMP_NUM; cnt++) {
		mdp4_stat.mdp_reg_dump_prim_disp[cnt]
			= inpdw(MDP_BASE + MDP_DMA_P_DUMP_START + (cnt*4));
	}

	mdp4_stat.mdp_reg_dump_prim_disp2[0] = inpdw(MDP_BASE + 0x91000);
	mdp4_stat.mdp_reg_dump_prim_disp2[1] = inpdw(MDP_BASE + 0x91004);
	mdp4_stat.mdp_reg_dump_prim_disp2[2] = inpdw(MDP_BASE + 0x92000);

	for (cnt  = 0; cnt < MIPI_DSI1_DUMP_NUM; cnt++) {
		mdp4_stat.mipi_dsi1_reg_dump[cnt]
			= inpdw(MIPI_DSI_BASE + MIPI_DSI1_DUMP_START + (cnt*4));
	}
}

void dump_mdp_registers()
{


	pr_info("=====================================\n");
	pr_info(" [cmd_lockup]     MDP register dump\n");
	printkout(0x90000, 10);
	printkout(0x90070, 1);
	printkout(0x91000, 2);
	printkout(0x10100, 1);
	printkout(0x18000, 1);
	printkout(0x10000, 20);
	printkout(0x10104, 1);
	printkout(0x10124, 1);
	printkout(0x10144, 1);
	printkout(0x20000, 4);
	printkout(0x20040, 4);
	printkout(0x20050, 5);
	printkout(0x21000, 2);
	printkout(0x0018, 1);
	printkout(0x0034, 2);
	printkout(0x0050, 2);
	printkout(0x0100, 9);
	printkout(0x0124, 2);
	printkout(0x0134, 2);
	printkout(0x0140, 1);
	printkout(0x014c, 1);
	printkout(0x0154, 1);
	printkout(0x0200, 1);
	printkout(0x0210, 1);
	printkout(0x021c, 1);
	printkout(0x04B0, 3);
	printkout(0x04c4, 2);
	printkout(0x30000, 4);
	printkout(0x30040, 4);
	printkout(0x30050, 5);
	printkout(0x31000, 2);
	printkout(0x40000, 4);
	printkout(0x40040, 4);
	printkout(0x40050, 5);
	printkout(0x41000, 2);
	printkout(0x50000, 4);
	printkout(0x50040, 4);
	printkout(0x50050, 5);
	printkout(0x51000, 2);
	printkout(0xB0000, 5);
	printkout(0x18004, 2);
	printkout(0x18018, 1);
	printkout(0x18104, 1);
	printkout_mipi(0x0, 125);
	pr_info("=====================================\n");
	save_reg_dump();

}
#endif
void mdp4_dsi_cmd_dma_busy_wait(struct msm_fb_data_type *mfd)
{
	unsigned long flag;
	int need_wait = 0;
	int wait_cnt = 0;
#ifdef MDP_HANG_DEBUG
	mipi_dsi_cmd_mode_on = 1;
#endif

	if (dsi_clock_timer.function) {
#ifndef MDP_HANG_DEBUG
		if (time_after(jiffies, tout_expired)) {
#endif
			tout_expired = jiffies + TOUT_PERIOD;
			mod_timer(&dsi_clock_timer, tout_expired);
#ifndef MDP_HANG_DEBUG
			tout_expired -= MS_100;
		}
#endif
	}

	pr_debug("%s: start pid=%d dsi_clk_on=%d\n",
			__func__, current->pid, mipi_dsi_clk_on);

	/* satrt dsi clock if necessary */
	spin_lock_bh(&dsi_clk_lock);
	if (mipi_dsi_clk_on == 0) {
		mipi_dsi_turn_on_clks();
	}
	spin_unlock_bh(&dsi_clk_lock);
/*
*QCT_PATCH-sbyun to avoid confilt
*between on going video image and new overlay image,
*add some delay when mdp of dma is in busy status
*/
	spin_lock_irqsave(&mdp_spin_lock, flag);
	if (mfd->dma->busy == TRUE) {
		if (busy_wait_cnt == 0)
			INIT_COMPLETION(mfd->dma->comp);
		busy_wait_cnt++;
		need_wait++;
#if defined(DEBUG_MDP_LOCKUP)
		mdp4_stat.dma_wait_start_time = jiffies;
#endif
	}
	spin_unlock_irqrestore(&mdp_spin_lock, flag);

	if (need_wait) {
		/* wait until DMA finishes the current job */
		pr_debug("%s: pending pid=%d dsi_clk_on=%d\n",
				__func__, current->pid, mipi_dsi_clk_on);
#ifdef MDP_HANG_DEBUG
		if (!wait_for_completion_timeout(&mfd->dma->comp,
		msecs_to_jiffies(VSYNC_PERIOD*30))) {
			/* Does not receive interrupt from MDP,
				Something wrong */

			dump_mdp_registers();
			panic("mdp interrupt missing");
		}

#else
		wait_for_completion(&mfd->dma->comp);
#endif

	}

#ifdef MDP_HANG_DEBUG
	mipi_dsi_cmd_mode_on = 0;
#endif
	pr_debug("%s: done pid=%d dsi_clk_on=%d\n",
			 __func__, current->pid, mipi_dsi_clk_on);
}

void mdp4_dsi_cmd_kickoff_video(struct msm_fb_data_type *mfd,
				struct mdp4_overlay_pipe *pipe)
{
	/*
	 * a video kickoff may happen before UI kickoff after
	 * blt enabled. mdp4_overlay_update_dsi_cmd() need
	 * to be called before kickoff.
	 * vice versa for blt disabled.
	 */

#ifdef FACTORY_TEST
	if (!is_lcd_connected)
		return;
#endif
	if (dsi_pipe->blt_addr && dsi_pipe->blt_cnt == 0)
		mdp4_overlay_update_dsi_cmd(mfd); /* first time */
	else if (dsi_pipe->blt_addr == 0  && dsi_pipe->blt_cnt) {
		mdp4_overlay_update_dsi_cmd(mfd); /* last time */
		dsi_pipe->blt_cnt = 0;
	}

	pr_debug("%s: blt_addr=%d blt_cnt=%d\n",
		__func__, (int)dsi_pipe->blt_addr, dsi_pipe->blt_cnt);

	if (dsi_pipe->blt_addr)
		mdp4_dsi_blt_dmap_busy_wait(dsi_mfd);

	mdp4_dsi_cmd_overlay_kickoff(mfd, pipe);
}

void mdp4_dsi_cmd_kickoff_ui(struct msm_fb_data_type *mfd,
				struct mdp4_overlay_pipe *pipe)
{

#ifdef FACTORY_TEST
	if (!is_lcd_connected)
		return;
#endif
	pr_debug("%s: pid=%d\n", __func__, current->pid);
	mdp4_dsi_cmd_overlay_kickoff(mfd, pipe);
}


void mdp4_dsi_cmd_overlay_kickoff(struct msm_fb_data_type *mfd,
				struct mdp4_overlay_pipe *pipe)
{
	unsigned long flag, rflag;

#ifdef FACTORY_TEST
	if (!is_lcd_connected)
		return;
#endif
/*
*QCT_PATCH-sbyun to avoid confilt
*between on going video image and new overlay image,
*add some delay when mdp of dma is in busy status
*/
	/* change mdp clk */
	mdp4_set_perf_level();

	mipi_dsi_mdp_busy_wait(mfd);
#ifdef MDP_UNDERFLOW_RESET_CTRL_CMD
	spin_lock_irqsave(&mixer_reset_lock, rflag);
#endif
	if (dsi_pipe->blt_addr == 0)
		mipi_dsi_cmd_mdp_start();

	mdp4_overlay_dsi_state_set(ST_DSI_PLAYING);

	spin_lock_irqsave(&mdp_spin_lock, flag);

	mdp_enable_irq(MDP_OVERLAY0_TERM);
	mfd->dma->busy = TRUE;

	if (dsi_pipe->blt_addr)
		mfd->dma->dmap_busy = TRUE;

	/* start OVERLAY pipe */
	spin_unlock_irqrestore(&mdp_spin_lock, flag);
#ifdef MDP_UNDERFLOW_RESET_CTRL_CMD
	spin_unlock_irqrestore(&mixer_reset_lock, rflag);
#endif
	mdp_pipe_kickoff(MDP_OVERLAY0_TERM, mfd);
	mdp4_stat.kickoff_ov0++;
}

void mdp_dsi_cmd_overlay_suspend(void)
{
	/* dis-engage rgb0 from mixer0 */
	if (dsi_pipe)
		mdp4_mixer_stage_down(dsi_pipe);
}

void mdp4_dsi_cmd_overlay(struct msm_fb_data_type *mfd)
{
	if (!mfd)
		return;
	mutex_lock(&mfd->dma->ov_mutex);

	if (mfd->panel_power_on) {
		mdp4_dsi_cmd_dma_busy_wait(mfd);

		if (dsi_pipe && dsi_pipe->blt_addr)
			mdp4_dsi_blt_dmap_busy_wait(mfd);

		mdp4_overlay_update_dsi_cmd(mfd);

		mdp4_dsi_cmd_kickoff_ui(mfd, dsi_pipe);

	/* signal if pan function is waiting for the update completion */
		if (mfd->pan_waiting) {
			mfd->pan_waiting = FALSE;
			complete(&mfd->pan_comp);
		}
	}
	mutex_unlock(&mfd->dma->ov_mutex);
}
